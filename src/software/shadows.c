/* artful — shadow maps on the CPU. The light's views are drawn for depth alone, in two steps: geometry, in
 * parallel by groups of instances, puts triangles into stripes of rows; then each stripe fills its rows.
 * A depth map takes the nearest of what falls on a texel, which does not depend on order, so threads do
 * not change the bytes. Nothing here depends on the kernels: the same code on every processor. */
#include "software/shadows.h"

#define STRIPE 64
#define GROUPS 64

typedef struct ShadowTri {
    int32_t x[3], y[3];     /* in 1/16 texels */
    float z[3], iw[3];      /* depth, and 1 / w */
    float uv[3][2];
    int32_t minX, minY, maxX, maxY;
    const ArtfMaterialData* mask; /* it has holes: NULL for most */
} ShadowTri;

typedef struct Group {
    ARTF_VEC(ShadowTri) tris;
    ARTF_VEC(uint32_t)* stripes;
} Group;

struct ArtfShadowMaps {
    ArtfPool* pool;
    ArtfShadowSource source;
    ArtfShadowFrame frame;
    size_t atlasFloats;
    float* atlas;
    Group groups[GROUPS];
    int stripeCount;
    const ArtfScene* scene;
    uint32_t view;
    uint64_t keys[ARTF_SHADOW_VIEWS]; /* what each map holds */
    uint64_t seen;                    /* the scene's changes these know of */
    uint32_t drawn;
};

/* ---- geometry ---- */

typedef struct Corner { float x, y, z, w, u, v; } Corner;

static void emit(ArtfShadowMaps* m, Group* g, const Corner* c, const ArtfMaterialData* mask) {
    const int size = (int)m->frame.sun[2];
    ShadowTri t;
    for (int i = 0; i < 3; ++i) {
        const float iw = 1.f / c[i].w;
        const float x = (c[i].x * iw * .5f + .5f) * (float)size * 16.f, y = (c[i].y * iw * .5f + .5f) * (float)size * 16.f;
        t.x[i] = (int32_t)floorf(artfClampf(x, -3e7f, 3e7f) + .5f);
        t.y[i] = (int32_t)floorf(artfClampf(y, -3e7f, 3e7f) + .5f);
        t.z[i] = c[i].z * iw;
        t.iw[i] = iw;
        t.uv[i][0] = c[i].u;
        t.uv[i][1] = c[i].v;
    }
    int64_t area = (int64_t)(t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - (int64_t)(t.x[2] - t.x[0]) * (t.y[1] - t.y[0]);
    if (!area) return;
    if (area < 0) {
        /* (shadows have no fronts: only the order of the corners is turned) */
        int32_t a = t.x[1]; t.x[1] = t.x[2]; t.x[2] = a;
        a = t.y[1]; t.y[1] = t.y[2]; t.y[2] = a;
        float b = t.z[1]; t.z[1] = t.z[2]; t.z[2] = b;
        b = t.iw[1]; t.iw[1] = t.iw[2]; t.iw[2] = b;
        for (int k = 0; k < 2; ++k) { b = t.uv[1][k]; t.uv[1][k] = t.uv[2][k]; t.uv[2][k] = b; }
    }
    const int32_t lox = artfMini(t.x[0], artfMini(t.x[1], t.x[2])), hix = artfMaxi(t.x[0], artfMaxi(t.x[1], t.x[2]));
    const int32_t loy = artfMini(t.y[0], artfMini(t.y[1], t.y[2])), hiy = artfMaxi(t.y[0], artfMaxi(t.y[1], t.y[2]));
    t.minX = artfMaxi((lox + 7) >> 4, 0);
    t.maxX = artfMini((hix - 8) >> 4, size - 1);
    t.minY = artfMaxi((loy + 7) >> 4, 0);
    t.maxY = artfMini((hiy - 8) >> 4, size - 1);
    if (t.minX > t.maxX || t.minY > t.maxY) return;
    t.mask = mask;
    artfVecPush(&g->tris, t);
    const uint32_t index = (uint32_t)g->tris.len - 1;
    for (int s = t.minY / STRIPE; s <= t.maxY / STRIPE; ++s) artfVecPush(&g->stripes[s], index);
}

/* What is in front of w = nearW, by the corners of a triangle: a triangle, or two. */
static void clipAndEmit(ArtfShadowMaps* m, Group* g, const Corner* in, const ArtfMaterialData* mask, float nearW) {
    Corner out[4];
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        const Corner *a = &in[i], *b = &in[(i + 1) % 3];
        if (a->w >= nearW) out[n++] = *a;
        if ((a->w >= nearW) != (b->w >= nearW)) {
            const float t = (nearW - a->w) / (b->w - a->w);
            const Corner c = {a->x + (b->x - a->x) * t, a->y + (b->y - a->y) * t, a->z + (b->z - a->z) * t, nearW, a->u + (b->u - a->u) * t, a->v + (b->v - a->v) * t};
            out[n++] = c;
        }
    }
    for (int i = 1; i + 1 < n; ++i) {
        const Corner fan[3] = {out[0], out[i], out[i + 1]};
        emit(m, g, fan, mask);
    }
}

static void geometryJob(void* user, uint32_t index, uint32_t worker) {
    (void)worker;
    ArtfShadowMaps* m = user;
    const ArtfScene* scene = m->scene;
    Group* g = &m->groups[index];
    g->tris.len = 0;
    for (int s = 0; s < m->stripeCount; ++s) g->stripes[s].len = 0;
    const ArtfShadowView* view = &m->frame.views[m->view];
    const float* vm = view->m;
    const size_t count = scene->instances.len, first = count * index / GROUPS, end = count * (index + 1) / GROUPS;
    const bool ortho = view->place[3] <= 0;
    /* (a lamp's near plane: w is distance there, and the map starts at this) */
    const float nearW = ortho ? .5f : view->place[3] * .01f;
    for (size_t i = first; i < end; ++i) {
        const ArtfInstanceData* inst = &scene->instances.items[i];
        if (inst->translucent) continue;
        const ArtfMeshData* mesh = &scene->meshes.items[inst->mesh];
        const float* w = inst->m;
        const float* c = mesh->center;
        const float wc[3] = {w[0] * c[0] + w[1] * c[1] + w[2] * c[2] + w[3], w[4] * c[0] + w[5] * c[1] + w[6] * c[2] + w[7], w[8] * c[0] + w[9] * c[1] + w[10] * c[2] + w[11]};
        const float r = mesh->radius * inst->maxScale;
        /* out of its reach: a box for the sun, a ball for a lamp */
        if (ortho) {
            const float half = view->info[2] * (float)m->frame.sun[2] * .5f;
            const float x = vm[0] * wc[0] + vm[1] * wc[1] + vm[2] * wc[2] + vm[3], y = vm[4] * wc[0] + vm[5] * wc[1] + vm[6] * wc[2] + vm[7];
            if (fabsf(x) > 1.f + r / half || fabsf(y) > 1.f + r / half) continue;
        } else {
            const float dx = wc[0] - view->place[0], dy = wc[1] - view->place[1], dz = wc[2] - view->place[2], reach = view->place[3] + r;
            if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
        }
        const ArtfMaterialData* material = &scene->materials.items[inst->material];
        const ArtfMaterialData* mask = material->flags & ARTF_MATERIAL_MASK ? material : NULL;
        /* object to the view's clip space */
        float mvp[16];
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                mvp[row * 4 + col] = vm[row * 4] * w[col] + vm[row * 4 + 1] * w[4 + col] + vm[row * 4 + 2] * w[8 + col] + (col == 3 ? vm[row * 4 + 3] : 0.f);
        for (uint32_t k = 0; k < mesh->meshletCount; ++k) {
            const ArtfMeshlet* l = &mesh->meshlets[k];
            Corner corners[ARTF_MESHLET_VERTICES];
            const float* px = mesh->positions + l->vertexOffset;
            const float* py = px + mesh->positionStride;
            const float* pz = py + mesh->positionStride;
            for (uint32_t v = 0; v < l->vertexCount; ++v) {
                const float x = px[v], y = py[v], z = pz[v];
                corners[v].x = mvp[0] * x + mvp[1] * y + mvp[2] * z + mvp[3];
                corners[v].y = mvp[4] * x + mvp[5] * y + mvp[6] * z + mvp[7];
                corners[v].z = mvp[8] * x + mvp[9] * y + mvp[10] * z + mvp[11];
                corners[v].w = mvp[12] * x + mvp[13] * y + mvp[14] * z + mvp[15];
                corners[v].u = corners[v].v = 0;
                if (mask) {
                    const ArtfPackedVertex* p = &mesh->vertices[mesh->meshletVertices[l->vertexOffset + v]];
                    corners[v].u = artfHalfToFloat(p->u);
                    corners[v].v = artfHalfToFloat(p->v);
                }
            }
            const uint8_t* tri = mesh->meshletTriangles + (size_t)l->triangleOffset * 3;
            for (uint32_t t = 0; t < l->triangleCount; ++t, tri += 3) {
                const Corner in[3] = {corners[tri[0]], corners[tri[1]], corners[tri[2]]};
                if (in[0].w >= nearW && in[1].w >= nearW && in[2].w >= nearW) emit(m, g, in, mask);
                else if (in[0].w >= nearW || in[1].w >= nearW || in[2].w >= nearW) clipAndEmit(m, g, in, mask, nearW);
            }
        }
    }
}

/* ---- rows ---- */

static bool isHole(const ArtfScene* scene, const ShadowTri* t, float b0, float b1, float b2) {
    const ArtfMaterialData* material = t->mask;
    if (!material->texture) return false;
    const float w0 = b0 * t->iw[0], w1 = b1 * t->iw[1], w2 = b2 * t->iw[2], inv = 1.f / (w0 + w1 + w2);
    const float u = (w0 * t->uv[0][0] + w1 * t->uv[1][0] + w2 * t->uv[2][0]) * inv, v = (w0 * t->uv[0][1] + w1 * t->uv[1][1] + w2 * t->uv[2][1]) * inv;
    float texel[4];
    artfTexelAt(&scene->textures.items[material->texture], u, v, texel);
    return texel[3] < material->cutoff;
}

static void fillTriangle(ArtfShadowMaps* m, const ShadowTri* t, int firstRow, int lastRow, float* tile) {
    const int width = m->source.width;
    int64_t dx[3], dy[3], rowStart[3];
    const int y0 = artfMaxi(t->minY, firstRow), y1 = artfMini(t->maxY, lastRow);
    const int64_t px = (int64_t)t->minX * 16 + 8, py = (int64_t)y0 * 16 + 8;
    for (int i = 0; i < 3; ++i) {
        const int j = i == 2 ? 0 : i + 1;
        dx[i] = (int64_t)t->x[j] - t->x[i];
        dy[i] = (int64_t)t->y[j] - t->y[i];
        const bool topLeft = dy[i] < 0 || (dy[i] == 0 && dx[i] > 0);
        rowStart[i] = dx[i] * (py - t->y[i]) - dy[i] * (px - t->x[i]) - (topLeft ? 0 : 1);
    }
    const int64_t area = (int64_t)(t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) - (int64_t)(t->x[2] - t->x[0]) * (t->y[1] - t->y[0]);
    const float inverseArea = 1.f / (float)area;
    for (int y = y0; y <= y1; ++y) {
        int64_t e0 = rowStart[0], e1 = rowStart[1], e2 = rowStart[2];
        float* row = tile + (size_t)y * width;
        for (int x = t->minX; x <= t->maxX; ++x) {
            if ((e0 | e1 | e2) >= 0) {
                /* each edge's value is the share of the corner across from it */
                const float b0 = (float)e1 * inverseArea, b1 = (float)e2 * inverseArea, b2 = (float)e0 * inverseArea;
                const float z = t->z[0] * b0 + t->z[1] * b1 + t->z[2] * b2;
                if (z < row[x] && z >= 0 && !(t->mask && isHole(m->scene, t, b0, b1, b2))) row[x] = z;
            }
            e0 -= dy[0] * 16;
            e1 -= dy[1] * 16;
            e2 -= dy[2] * 16;
        }
        rowStart[0] += dx[0] * 16;
        rowStart[1] += dx[1] * 16;
        rowStart[2] += dx[2] * 16;
    }
}

static void stripeJob(void* user, uint32_t stripe, uint32_t worker) {
    (void)worker;
    ArtfShadowMaps* m = user;
    const ArtfShadowView* view = &m->frame.views[m->view];
    const int size = (int)m->frame.sun[2], width = m->source.width;
    float* tile = m->atlas + (size_t)view->info[1] * width + (size_t)view->info[0];
    const int firstRow = (int)stripe * STRIPE, lastRow = artfMini(firstRow + STRIPE, size) - 1;
    for (int y = firstRow; y <= lastRow; ++y)
        for (int x = 0; x < size; ++x) tile[(size_t)y * width + x] = 1.f;
    for (int g = 0; g < GROUPS; ++g) {
        const Group* group = &m->groups[g];
        const uint32_t* list = group->stripes[stripe].items;
        for (size_t i = 0; i < group->stripes[stripe].len; ++i) fillTriangle(m, &group->tris.items[list[i]], firstRow, lastRow, tile);
    }
}

/* ---- all of it ---- */

ArtfShadowMaps* artfShadowMapsNew(ArtfPool* pool) {
    ArtfShadowMaps* m = ARTF_NEW(ArtfShadowMaps);
    m->pool = pool;
    return m;
}

static void freeStripes(ArtfShadowMaps* m) {
    for (int g = 0; g < GROUPS; ++g) {
        if (!m->groups[g].stripes) continue;
        for (int s = 0; s < m->stripeCount; ++s) artfVecFree(&m->groups[g].stripes[s]);
        artfFree(m->groups[g].stripes);
        m->groups[g].stripes = NULL;
    }
}

void artfShadowMapsFree(ArtfShadowMaps* m) {
    if (!m) return;
    freeStripes(m);
    for (int g = 0; g < GROUPS; ++g) artfVecFree(&m->groups[g].tris);
    artfFree(m->atlas);
    artfFree(m);
}

uint32_t artfShadowMapsDrawn(const ArtfShadowMaps* m) { return m ? m->drawn : 0; }

const ArtfShadowSource* artfShadowMapsDraw(ArtfShadowMaps* m, const ArtfScene* scene, const ArtfFrameData* frame) {
    const uint32_t views = artfSceneShadowFrame(scene, frame, &m->frame);
    if (!views) return NULL;
    const int size = (int)m->frame.sun[2], stripes = (size + STRIPE - 1) / STRIPE;
    m->source.width = size * ARTF_SHADOW_ATLAS_COLUMNS;
    const size_t floats = (size_t)m->source.width * size * ((views + ARTF_SHADOW_ATLAS_COLUMNS - 1) / ARTF_SHADOW_ATLAS_COLUMNS);
    if (floats != m->atlasFloats) {
        artfFree(m->atlas);
        m->atlas = artfMalloc(floats * 4);
        m->atlasFloats = floats;
        memset(m->keys, 0, sizeof m->keys);
    }
    if (stripes != m->stripeCount) {
        freeStripes(m);
        m->stripeCount = stripes;
        for (int g = 0; g < GROUPS; ++g) m->groups[g].stripes = artfCalloc((size_t)stripes, sizeof *m->groups[g].stripes);
    }
    m->scene = scene;
    m->drawn = 0;
    for (uint32_t v = 0; v < views; ++v) {
        /* what did not change is there already */
        const uint64_t key = artfShadowViewKey(scene, &m->frame.views[v], m->frame.sun[2]);
        if (key == m->keys[v] && !artfShadowViewTouched(scene, &m->frame.views[v], m->frame.sun[2], m->seen)) continue;
        m->keys[v] = key;
        ++m->drawn;
        m->view = v;
        artfPoolRun(m->pool, geometryJob, m, GROUPS);
        artfPoolRun(m->pool, stripeJob, m, (uint32_t)stripes);
    }
    m->seen = scene->shadowChangeCount;
    m->source.frame = &m->frame;
    m->source.atlas = m->atlas;
    return &m->source;
}

/* ---- a pixel ---- */

/* How much of a view's light gets to a place: nine texels around it, each in or out. */
static float lookUp(const ArtfShadowSource* s, uint32_t index, const float* p) {
    const ArtfShadowView* view = &s->frame->views[index];
    const float* m = view->m;
    const float w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
    if (w <= 0) return 1.f;
    const int size = (int)s->frame->sun[2];
    const float iw = 1.f / w;
    const float x = (m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]) * iw, y = (m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7]) * iw;
    const float depth = (m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]) * iw - view->info[3];
    const float u = (x * .5f + .5f) * (float)size, v = (y * .5f + .5f) * (float)size;
    if (!(u >= 0 && v >= 0 && u < (float)size && v < (float)size)) return 1.f;
    const int cx = (int)u, cy = (int)v, ox = (int)view->info[0], oy = (int)view->info[1];
    int lit = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        const float* row = s->atlas + (size_t)(oy + artfClampi(cy + dy, 0, size - 1)) * s->width + ox;
        for (int dx = -1; dx <= 1; ++dx) lit += depth <= row[artfClampi(cx + dx, 0, size - 1)];
    }
    return (float)lit * (1.f / 9.f);
}

float artfShadowOfSun(const ArtfShadowSource* s, const float* eye, const float* world, const float* normal) {
    const ArtfShadowFrame* f = s->frame;
    if (!f->sun[0]) return 1.f;
    const float dx = world[0] - eye[0], dy = world[1] - eye[1], dz = world[2] - eye[2];
    const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    uint32_t cascade = 0;
    while (cascade < f->sun[1] && distance >= f->splits[cascade]) ++cascade;
    if (cascade >= f->sun[1]) return 1.f;
    const uint32_t view = f->sun[0] - 1 + cascade;
    const float* toSun = f->views[view].place;
    /* (what the light only grazes needs lifting more: a texel covers more depth there) */
    const float lift = f->views[view].info[2] * 2.f / artfMaxf(normal[0] * toSun[0] + normal[1] * toSun[1] + normal[2] * toSun[2], .15f);
    const float p[3] = {world[0] + normal[0] * lift, world[1] + normal[1] * lift, world[2] + normal[2] * lift};
    return lookUp(s, view, p);
}

float artfShadowOfLight(const ArtfShadowSource* s, uint32_t light, const float* position, const float* world, const float* normal) {
    const uint32_t word = s->frame->lights[light][0];
    if (!word) return 1.f;
    uint32_t view = (word & 255) - 1;
    const float dx = world[0] - position[0], dy = world[1] - position[1], dz = world[2] - position[2];
    if (word >> 8 == 6) {
        /* the face of the cube it looks through */
        const float ax = fabsf(dx), ay = fabsf(dy), az = fabsf(dz);
        view += ax >= ay && ax >= az ? (dx > 0 ? 0u : 1u) : ay >= az ? (dy > 0 ? 2u : 3u) : (dz > 0 ? 4u : 5u);
    }
    const float distance = sqrtf(dx * dx + dy * dy + dz * dz), facing = -(normal[0] * dx + normal[1] * dy + normal[2] * dz) / artfMaxf(distance, 1e-6f);
    const float lift = s->frame->views[view].info[2] * distance * 2.f / artfMaxf(facing, .15f);
    const float p[3] = {world[0] + normal[0] * lift, world[1] + normal[1] * lift, world[2] + normal[2] * lift};
    return lookUp(s, view, p);
}
