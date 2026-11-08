/* artful — from the vertices an app gives to what the backends draw. Done once per mesh, so that
 * nothing of it is paid in a frame:
 *
 *   - the triangles are put in the order the vertex cache likes (Tipsify, Sander et al. 2007);
 *   - the vertices in the order the triangles first use them, so that memory is read forwards;
 *   - each vertex is packed into 16 bytes, with its color apart, only where there are colors;
 *   - the triangles are cut into meshlets, each with a sphere and a cone to cull it by. */
#include "scene/sceneInternal.h"

#define CACHE_SIZE 16

/* ---- the order of the triangles ---- */

typedef struct Tipsify {
    const uint32_t* indices;
    uint32_t *live, *cacheTime, *deadEnds;
    uint32_t deadEndCount, cursor, vertexCount;
} Tipsify;

static uint32_t tipsifyNext(Tipsify* t, const uint32_t* candidates, uint32_t count, uint32_t time) {
    uint32_t best = UINT32_MAX;
    int bestPriority = -1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = candidates[i];
        if (!t->live[v]) continue;
        int priority = 0;
        if (time - t->cacheTime[v] + 2 * t->live[v] <= CACHE_SIZE) priority = (int)(time - t->cacheTime[v]);
        if (priority > bestPriority) { bestPriority = priority; best = v; }
    }
    if (best != UINT32_MAX) return best;
    while (t->deadEndCount) {
        uint32_t v = t->deadEnds[--t->deadEndCount];
        if (t->live[v]) return v;
    }
    while (t->cursor < t->vertexCount) {
        if (t->live[t->cursor]) return t->cursor;
        ++t->cursor;
    }
    return UINT32_MAX;
}

/* out: the same triangles, reordered. */
static void tipsify(const uint32_t* indices, uint32_t indexCount, uint32_t vertexCount, uint32_t* out) {
    const uint32_t triangleCount = indexCount / 3;
    uint32_t* offsets = artfCalloc((size_t)vertexCount + 1, 4);
    uint32_t* adjacency = artfMalloc((size_t)indexCount * 4);
    Tipsify t = {indices, artfCalloc(vertexCount, 4), artfCalloc(vertexCount, 4), artfMalloc((size_t)indexCount * 4), 0, 0, vertexCount};
    uint8_t* emitted = artfCalloc(triangleCount, 1);
    uint32_t candidates[3 * 256];

    for (uint32_t i = 0; i < indexCount; ++i) ++t.live[indices[i]];
    for (uint32_t v = 0; v < vertexCount; ++v) offsets[v + 1] = offsets[v] + t.live[v];
    uint32_t* fill = artfMalloc((size_t)vertexCount * 4 + 4);
    memcpy(fill, offsets, (size_t)vertexCount * 4);
    for (uint32_t i = 0; i < indexCount; ++i) adjacency[fill[indices[i]]++] = i / 3;
    artfFree(fill);

    uint32_t time = CACHE_SIZE + 1, written = 0, fan = indexCount ? indices[0] : UINT32_MAX;
    while (fan != UINT32_MAX) {
        uint32_t count = 0;
        for (uint32_t a = offsets[fan]; a < offsets[fan + 1]; ++a) {
            uint32_t tri = adjacency[a];
            if (emitted[tri]) continue;
            emitted[tri] = 1;
            for (int c = 0; c < 3; ++c) {
                uint32_t v = indices[tri * 3 + c];
                out[written++] = v;
                t.deadEnds[t.deadEndCount++] = v;
                if (count < ARTF_COUNT(candidates)) candidates[count++] = v;
                --t.live[v];
                if (time - t.cacheTime[v] > CACHE_SIZE) t.cacheTime[v] = time++;
            }
        }
        fan = tipsifyNext(&t, candidates, count, time);
    }

    artfFree(offsets);
    artfFree(adjacency);
    artfFree(t.live);
    artfFree(t.cacheTime);
    artfFree(t.deadEnds);
    artfFree(emitted);
}

/* ---- packing ---- */

static uint16_t floatToHalf(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    uint32_t sign = (b >> 16) & 0x8000;
    int exp = (int)((b >> 23) & 255) - 127 + 15;
    uint32_t man = b & 0x7FFFFF;
    if (exp >= 31) return (uint16_t)(sign | 0x7BFF);    /* the largest there is */
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        man |= 0x800000;
        uint32_t shift = (uint32_t)(14 - exp);
        return (uint16_t)(sign | ((man + (1u << (shift - 1))) >> shift));
    }
    uint32_t h = sign | (uint32_t)exp << 10 | man >> 13;
    return (uint16_t)(h + ((man >> 12) & 1));          /* (rounding may carry into the exponent: that is right) */
}

static void octEncode(float x, float y, float z, int16_t* ox, int16_t* oy) {
    float l = fabsf(x) + fabsf(y) + fabsf(z);
    if (l == 0) { *ox = 0; *oy = 0; return; }
    x /= l;
    y /= l;
    if (z < 0) {
        float fx = (1.f - fabsf(y)) * (x >= 0 ? 1.f : -1.f), fy = (1.f - fabsf(x)) * (y >= 0 ? 1.f : -1.f);
        x = fx;
        y = fy;
    }
    *ox = (int16_t)floorf(artfClampf(x, -1.f, 1.f) * 32767.f + .5f);
    *oy = (int16_t)floorf(artfClampf(y, -1.f, 1.f) * 32767.f + .5f);
}

static void unpackPosition(const ArtfMeshData* m, const ArtfPackedVertex* v, float* out) {
    out[0] = (float)v->px / 65535.f * m->scale[0] + m->offset[0];
    out[1] = (float)v->py / 65535.f * m->scale[1] + m->offset[1];
    out[2] = (float)v->pz / 65535.f * m->scale[2] + m->offset[2];
}

/* ---- meshlets ---- */

static void finishMeshlet(ArtfMeshData* m, ArtfMeshlet* l) {
    const uint32_t* local = m->meshletVertices + l->vertexOffset;
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY}, p[3];
    for (uint32_t i = 0; i < l->vertexCount; ++i) {
        unpackPosition(m, &m->vertices[local[i]], p);
        for (int c = 0; c < 3; ++c) { lo[c] = artfMinf(lo[c], p[c]); hi[c] = artfMaxf(hi[c], p[c]); }
    }
    for (int c = 0; c < 3; ++c) l->center[c] = (lo[c] + hi[c]) * .5f;
    float r2 = 0;
    for (uint32_t i = 0; i < l->vertexCount; ++i) {
        unpackPosition(m, &m->vertices[local[i]], p);
        float dx = p[0] - l->center[0], dy = p[1] - l->center[1], dz = p[2] - l->center[2];
        r2 = artfMaxf(r2, dx * dx + dy * dy + dz * dz);
    }
    l->radius = sqrtf(r2) * 1.0001f;

    /* the cone: the faces' mean direction, and how far the widest strays from it */
    ArtfVec3 faces[ARTF_MESHLET_TRIANGLES], mean = artfVec3(0, 0, 0);
    uint32_t faceCount = 0;
    const uint8_t* tri = m->meshletTriangles + (size_t)l->triangleOffset * 3;
    for (uint32_t i = 0; i < l->triangleCount; ++i, tri += 3) {
        float a[3], b[3], c[3];
        unpackPosition(m, &m->vertices[local[tri[0]]], a);
        unpackPosition(m, &m->vertices[local[tri[1]]], b);
        unpackPosition(m, &m->vertices[local[tri[2]]], c);
        ArtfVec3 n = artfVec3Cross(artfVec3(b[0] - a[0], b[1] - a[1], b[2] - a[2]), artfVec3(c[0] - a[0], c[1] - a[1], c[2] - a[2]));
        if (artfVec3Dot(n, n) == 0) continue;
        n = artfVec3Normalize(n);
        faces[faceCount++] = n;
        mean = artfVec3Add(mean, n);
    }
    l->coneCutoff = 2.f;
    mean = artfVec3Normalize(mean);
    if (!faceCount || artfVec3Dot(mean, mean) == 0) return;
    float minDot = 1.f;
    for (uint32_t i = 0; i < faceCount; ++i) minDot = artfMinf(minDot, artfVec3Dot(faces[i], mean));
    if (minDot <= .05f) return; /* wider than half a sphere, or close to: no camera sees none of it */
    l->cone[0] = mean.x;
    l->cone[1] = mean.y;
    l->cone[2] = mean.z;
    l->coneCutoff = sqrtf(1.f - minDot * minDot);
}

static void buildMeshlets(ArtfMeshData* m) {
    ARTF_VEC(ArtfMeshlet) meshlets = {0};
    ARTF_VEC(uint32_t) vertices = {0};
    ARTF_VEC(uint8_t) triangles = {0};
    uint8_t* seen = artfCalloc(m->vertexCount, 1); /* local index + 1 in the meshlet being made */
    ArtfMeshlet l = {0};

    for (uint32_t i = 0; i < m->indexCount; i += 3) {
        int fresh = 0;
        for (int c = 0; c < 3; ++c) fresh += !seen[m->indices[i + c]];
        if (l.vertexCount + (uint32_t)fresh > ARTF_MESHLET_VERTICES || l.triangleCount == ARTF_MESHLET_TRIANGLES) {
            for (uint32_t v = 0; v < l.vertexCount; ++v) seen[vertices.items[l.vertexOffset + v]] = 0;
            artfVecPush(&meshlets, l);
            memset(&l, 0, sizeof l);
            while (vertices.len & 7) artfVecPush(&vertices, 0); /* (each block starts on a whole vector) */
            l.vertexOffset = (uint32_t)vertices.len;
            l.triangleOffset = (uint32_t)(triangles.len / 3);
        }
        for (int c = 0; c < 3; ++c) {
            uint32_t v = m->indices[i + c];
            if (!seen[v]) {
                artfVecPush(&vertices, v);
                seen[v] = (uint8_t)++l.vertexCount;
            }
            artfVecPush(&triangles, (uint8_t)(seen[v] - 1));
        }
        ++l.triangleCount;
    }
    if (l.triangleCount) artfVecPush(&meshlets, l);
    while (vertices.len & 7) artfVecPush(&vertices, 0);
    artfFree(seen);

    m->meshlets = meshlets.items;
    m->meshletCount = (uint32_t)meshlets.len;
    m->meshletVertices = vertices.items;
    m->meshletTriangles = triangles.items;
    for (uint32_t i = 0; i < m->meshletCount; ++i) finishMeshlet(m, &m->meshlets[i]);

    /* the positions the CPU transforms: by blocks of a meshlet, x then y then z */
    m->positionStride = (uint32_t)vertices.len;
    m->positions = artfAlignedMalloc(vertices.len * 3 * sizeof(float) + 64, 64);
    for (size_t i = 0; i < vertices.len; ++i) {
        float p[3];
        unpackPosition(m, &m->vertices[vertices.items[i]], p);
        m->positions[i] = p[0];
        m->positions[m->positionStride + i] = p[1];
        m->positions[(size_t)m->positionStride * 2 + i] = p[2];
    }
}

bool artfMeshBuild(ArtfMeshData* m, const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, const ArtfVertexSkin* skin, const ArtfMorphTarget* targets, uint32_t targetCount) {
    memset(m, 0, sizeof *m);
    /* what can be drawn: triangles whose vertices are there and differ */
    uint32_t* clean = artfMalloc(((size_t)indexCount + 3) * 4);
    uint32_t count = 0;
    for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
        uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount || a == b || b == c || a == c) continue;
        clean[count++] = a;
        clean[count++] = b;
        clean[count++] = c;
    }
    if (!count) { artfFree(clean); return false; }

    m->indices = artfMalloc((size_t)count * 4);
    m->indexCount = count;
    tipsify(clean, count, vertexCount, m->indices);
    artfFree(clean);

    /* the vertices in the order they are first used; those not used go */
    uint32_t* remap = artfMalloc((size_t)vertexCount * 4);
    memset(remap, 0xFF, (size_t)vertexCount * 4);
    uint32_t used = 0;
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = m->indices[i];
        if (remap[v] == UINT32_MAX) {
            remap[v] = used++;
            const float p[3] = {vertices[v].px, vertices[v].py, vertices[v].pz};
            for (int c = 0; c < 3; ++c) { lo[c] = artfMinf(lo[c], p[c]); hi[c] = artfMaxf(hi[c], p[c]); }
        }
    }
    /* What bends needs room to go: its 16 bits stand for a box many times its size, which keeps poses from
     * having to change what the GPU knows of the mesh. */
    const bool bends = skin || targetCount;
    if (bends) {
        float reach = 0;
        for (int c = 0; c < 3; ++c) reach = artfMaxf(reach, hi[c] - lo[c]);
        float restLow[3], restHigh[3];
        memcpy(restLow, lo, sizeof restLow);
        memcpy(restHigh, hi, sizeof restHigh);
        for (int c = 0; c < 3; ++c) {
            const float mid = (lo[c] + hi[c]) * .5f;
            lo[c] = mid - reach * 16.f;
            hi[c] = mid + reach * 16.f;
        }
        m->skin = ARTF_NEW(ArtfSkinData);
        m->ownsSkin = true;
        memcpy(m->skin->restLow, restLow, sizeof restLow);
        memcpy(m->skin->restHigh, restHigh, sizeof restHigh);
        m->skin->rest = artfMalloc((size_t)used * 24);
        if (skin) {
            m->skin->joints = artfMalloc((size_t)used * 16);
            m->skin->weights = artfMalloc((size_t)used * 32);
        }
        if (targetCount) {
            m->skin->morphCount = targetCount;
            m->skin->morphs = artfCalloc((size_t)used * targetCount, 24);
            m->skin->morphReach = artfCalloc(targetCount, 4);
        }
    }
    for (int c = 0; c < 3; ++c) { m->scale[c] = hi[c] - lo[c]; m->offset[c] = lo[c]; }

    m->vertexCount = used;
    m->vertices = artfMalloc((size_t)used * sizeof(ArtfPackedVertex));
    for (uint32_t v = 0; v < vertexCount && !m->colors; ++v)
        if (remap[v] != UINT32_MAX && vertices[v].color && vertices[v].color != 0xFFFFFFFFu) m->colors = artfMalloc((size_t)used * 4);
    for (uint32_t v = 0; v < vertexCount; ++v) {
        if (remap[v] == UINT32_MAX) continue;
        const ArtfVertex* in = &vertices[v];
        if (bends) {
            const float rest[6] = {in->px, in->py, in->pz, in->nx, in->ny, in->nz};
            memcpy(m->skin->rest + (size_t)remap[v] * 6, rest, sizeof rest);
            if (skin) {
                memcpy(m->skin->joints + (size_t)remap[v] * 8, skin[v].joints, 16);
                memcpy(m->skin->weights + (size_t)remap[v] * 8, skin[v].weights, 32);
            }
            for (uint32_t t = 0; t < targetCount; ++t) {
                float* to = m->skin->morphs + ((size_t)t * used + remap[v]) * 6;
                if (targets[t].positions) memcpy(to, targets[t].positions + (size_t)v * 3, 12);
                if (targets[t].normals) memcpy(to + 3, targets[t].normals + (size_t)v * 3, 12);
                m->skin->morphReach[t] = artfMaxf(m->skin->morphReach[t], artfMaxf(artfMaxf(fabsf(to[0]), fabsf(to[1])), fabsf(to[2])));
            }
        }
        if (m->colors) {
            const uint32_t color = in->color ? in->color : 0xFFFFFFFFu;
            m->colors[remap[v]] = color;
            m->hasAlpha = m->hasAlpha || color >> 24 != 255;
        }
        ArtfPackedVertex* o = &m->vertices[remap[v]];
        const float p[3] = {in->px, in->py, in->pz};
        uint16_t q[3];
        for (int c = 0; c < 3; ++c) q[c] = m->scale[c] > 0 ? (uint16_t)floorf((p[c] - lo[c]) / m->scale[c] * 65535.f + .5f) : 0;
        o->px = q[0];
        o->py = q[1];
        o->pz = q[2];
        o->pad = 65535; /* (the GPU reads this as w) */
        octEncode(in->nx, in->ny, in->nz, &o->nx, &o->ny);
        o->u = floatToHalf(in->u);
        o->v = floatToHalf(in->v);
    }
    for (uint32_t i = 0; i < count; ++i) m->indices[i] = remap[m->indices[i]];
    artfFree(remap);

    for (int c = 0; c < 3; ++c) m->center[c] = (lo[c] + hi[c]) * .5f;
    float r2 = 0;
    for (uint32_t v = 0; v < used; ++v) {
        float p[3];
        unpackPosition(m, &m->vertices[v], p);
        float dx = p[0] - m->center[0], dy = p[1] - m->center[1], dz = p[2] - m->center[2];
        r2 = artfMaxf(r2, dx * dx + dy * dy + dz * dz);
    }
    m->radius = sqrtf(r2) * 1.0001f;

    buildMeshlets(m);
    /* (poses move parts about: only the whole can be culled) */
    if (bends)
        for (uint32_t i = 0; i < m->meshletCount; ++i) {
            memcpy(m->meshlets[i].center, m->center, sizeof m->center);
            m->meshlets[i].radius = m->radius;
            m->meshlets[i].coneCutoff = 2.f;
        }
    return true;
}

static void* copyOf(const void* from, size_t bytes) {
    void* to = artfMalloc(bytes + 1);
    if (bytes) memcpy(to, from, bytes);
    return to;
}

void artfMeshCopy(ArtfMeshData* to, const ArtfMeshData* from) {
    *to = *from;
    to->ownsSkin = false;
    memset(&to->pose, 0, sizeof to->pose);
    to->pose.applied = to->deformVersion;
    to->vertices = copyOf(from->vertices, (size_t)from->vertexCount * sizeof *from->vertices);
    to->indices = copyOf(from->indices, (size_t)from->indexCount * 4);
    to->meshlets = copyOf(from->meshlets, (size_t)from->meshletCount * sizeof *from->meshlets);
    to->meshletVertices = copyOf(from->meshletVertices, (size_t)from->positionStride * 4);
    const ArtfMeshlet* last = &from->meshlets[from->meshletCount - 1];
    to->meshletTriangles = copyOf(from->meshletTriangles, ((size_t)last->triangleOffset + last->triangleCount) * 3);
    to->colors = from->colors ? copyOf(from->colors, (size_t)from->vertexCount * 4) : NULL;
    to->positions = artfAlignedMalloc((size_t)from->positionStride * 12 + 64, 64);
    memcpy(to->positions, from->positions, (size_t)from->positionStride * 12);
}

bool artfMeshPoseFit(ArtfMeshData* m) {
    const ArtfSkinData* skin = m->skin;
    const ArtfPose* pose = &m->pose;
    float margin = 0;
    for (uint32_t t = 0; pose->weights && t < skin->morphCount; ++t) margin += fabsf(pose->weights[t]) * skin->morphReach[t];
    float lo[3] = {3e38f, 3e38f, 3e38f}, hi[3] = {-3e38f, -3e38f, -3e38f};
    /* a vertex between bones is between where each would put it: the corners of what it was, by each bone */
    const uint32_t bones = skin->joints && pose->jointCount ? pose->jointCount : 1;
    for (uint32_t b = 0; b < bones; ++b)
        for (int corner = 0; corner < 8; ++corner) {
            float p[3];
            for (int c = 0; c < 3; ++c) p[c] = corner >> c & 1 ? skin->restHigh[c] + margin : skin->restLow[c] - margin;
            for (int c = 0; c < 3; ++c) {
                float at = p[c];
                if (skin->joints && pose->jointCount) {
                    const float* J = pose->joints + (size_t)b * 12 + c * 4;
                    at = J[0] * p[0] + J[1] * p[1] + J[2] * p[2] + J[3];
                }
                lo[c] = artfMinf(lo[c], at);
                hi[c] = artfMaxf(hi[c], at);
            }
        }
    bool inside = true;
    for (int c = 0; c < 3; ++c) inside = inside && lo[c] >= m->offset[c] && hi[c] <= m->offset[c] + m->scale[c];
    if (inside) return false;
    /* with room to spare, so that a walk does not do this at each step */
    float r2 = 0;
    for (int c = 0; c < 3; ++c) {
        const float mid = (lo[c] + hi[c]) * .5f, half = artfMaxf(hi[c] - lo[c], 1e-6f);
        m->offset[c] = mid - half;
        m->scale[c] = half * 2.f;
        m->center[c] = mid;
        r2 += half * half;
    }
    m->radius = sqrtf(r2) * 1.0001f;
    for (uint32_t i = 0; i < m->meshletCount; ++i) {
        memcpy(m->meshlets[i].center, m->center, sizeof m->center);
        m->meshlets[i].radius = m->radius;
    }
    return true;
}

void artfMeshDeform(ArtfMeshData* m) {
    const ArtfSkinData* skin = m->skin;
    const ArtfPose* pose = &m->pose;
    const uint32_t count = pose->jointCount;
    const float* joints = pose->joints;
    for (uint32_t v = 0; v < m->vertexCount; ++v) {
        float r[6];
        memcpy(r, skin->rest + (size_t)v * 6, sizeof r);
        for (uint32_t k = 0; pose->weights && k < skin->morphCount; ++k) {
            const float* d = skin->morphs + ((size_t)k * m->vertexCount + v) * 6;
            const float weight = pose->weights[k];
            if (weight != 0)
                for (int i = 0; i < 6; ++i) r[i] += d[i] * weight;
        }
        float t[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (skin->joints && count) {
            const uint16_t* j = skin->joints + (size_t)v * 8;
            const float* w = skin->weights + (size_t)v * 8;
            memset(t, 0, sizeof t);
            for (int k = 0; k < 8; ++k) {
                if (w[k] == 0 || j[k] >= count) continue;
                const float* J = joints + (size_t)j[k] * 12;
                for (int i = 0; i < 12; ++i) t[i] += J[i] * w[k];
            }
        }
        ArtfPackedVertex* o = &m->vertices[v];
        const float p[3] = {t[0] * r[0] + t[1] * r[1] + t[2] * r[2] + t[3], t[4] * r[0] + t[5] * r[1] + t[6] * r[2] + t[7], t[8] * r[0] + t[9] * r[1] + t[10] * r[2] + t[11]};
        uint16_t q[3];
        for (int c = 0; c < 3; ++c) q[c] = (uint16_t)floorf(artfClampf((p[c] - m->offset[c]) / m->scale[c], 0, 1) * 65535.f + .5f);
        o->px = q[0];
        o->py = q[1];
        o->pz = q[2];
        const float nx = t[0] * r[3] + t[1] * r[4] + t[2] * r[5], ny = t[4] * r[3] + t[5] * r[4] + t[6] * r[5], nz = t[8] * r[3] + t[9] * r[4] + t[10] * r[5];
        octEncode(nx, ny, nz, &o->nx, &o->ny);
    }
    for (uint32_t i = 0; i < m->positionStride; ++i) {
        float p[3];
        unpackPosition(m, &m->vertices[m->meshletVertices[i]], p);
        m->positions[i] = p[0];
        m->positions[m->positionStride + i] = p[1];
        m->positions[(size_t)m->positionStride * 2 + i] = p[2];
    }
    m->pose.applied = m->deformVersion;
}

void artfMeshUnpack(const ArtfMeshData* m, ArtfVertex* out) {
    for (uint32_t i = 0; i < m->vertexCount; ++i) {
        const ArtfPackedVertex* v = &m->vertices[i];
        float p[3];
        unpackPosition(m, v, p);
        const ArtfVec3 n = artfVec3Normalize(artfOctUnfold(v->nx, v->ny));
        const ArtfVertex o = {p[0], p[1], p[2], n.x, n.y, n.z, artfHalfToFloat(v->u), artfHalfToFloat(v->v), m->colors ? m->colors[i] : 0};
        out[i] = o;
    }
}

void artfMeshFini(ArtfMeshData* m) {
    artfFree(m->pose.joints);
    artfFree(m->pose.weights);
    artfFree(m->vertices);
    artfFree(m->indices);
    artfFree(m->meshlets);
    artfFree(m->meshletVertices);
    artfFree(m->meshletTriangles);
    artfFree(m->colors);
    artfAlignedFree(m->positions);
    if (m->ownsSkin) {
        artfFree(m->skin->rest);
        artfFree(m->skin->morphs);
        artfFree(m->skin->morphReach);
        artfFree(m->skin->joints);
        artfFree(m->skin->weights);
        artfFree(m->skin);
    }
}
