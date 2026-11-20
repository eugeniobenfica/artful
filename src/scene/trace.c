/* artful — what rays need: a tree of boxes for each mesh, one over the instances, and the paths
 * themselves, as plain C. The CPU draws with these; the GPU reads the same trees, and its shader
 * (shaders.c, artfShaderTrace) is this same arithmetic. */
#include "scene/sampling.h"
#include "scene/sceneInternal.h"
#include "core/threads.h"

#include <stdlib.h>

#define LEAF_SIZE 4
#define BINS 12
#define MAX_DEPTH 40    /* past this a node is cut in half: the stacks below stay in bounds */
#define MAX_STEPS 48    /* what a path may meet, counting what it goes through */
#define SHADOW_STEPS 8
#define LIGHT_LIMIT 64.f /* what one path can bring: a rare bright one would otherwise stay as a dot */

/* ---- the trees ---- */

typedef struct Box { float lo[3], hi[3]; } Box;

static void boxEmpty(Box* b) {
    for (int k = 0; k < 3; ++k) { b->lo[k] = 3e38f; b->hi[k] = -3e38f; }
}
static void boxAdd(Box* b, const Box* o) {
    for (int k = 0; k < 3; ++k) { b->lo[k] = artfMinf(b->lo[k], o->lo[k]); b->hi[k] = artfMaxf(b->hi[k], o->hi[k]); }
}
static float boxArea(const Box* b) {
    const float x = b->hi[0] - b->lo[0], y = b->hi[1] - b->lo[1], z = b->hi[2] - b->lo[2];
    return x < 0 ? 0 : x * y + y * z + z * x;
}

typedef ARTF_VEC(ArtfTraceNode) NodeList;

/* Cuts order[first .. first + count) where the surface area says, into bins along the widest axis of the
 * centers; `at` is where the node goes. */
static void buildNode(NodeList* nodes, uint32_t at, const Box* boxes, uint32_t* order, uint32_t first, uint32_t count, int depth) {
    Box all, centers;
    boxEmpty(&all);
    boxEmpty(&centers);
    for (uint32_t i = first; i < first + count; ++i) {
        const Box* b = &boxes[order[i]];
        boxAdd(&all, b);
        Box c;
        for (int k = 0; k < 3; ++k) c.lo[k] = c.hi[k] = (b->lo[k] + b->hi[k]) * .5f;
        boxAdd(&centers, &c);
    }
    ArtfTraceNode* n = &nodes->items[at];
    memcpy(n->lo, all.lo, sizeof n->lo);
    memcpy(n->hi, all.hi, sizeof n->hi);
    n->first = first;
    n->count = count;
    if (count <= LEAF_SIZE) return;

    int axis = 0;
    for (int k = 1; k < 3; ++k)
        if (centers.hi[k] - centers.lo[k] > centers.hi[axis] - centers.lo[axis]) axis = k;
    const float lo = centers.lo[axis], extent = centers.hi[axis] - lo;
    uint32_t mid = first + count / 2;
    if (extent > 0 && depth < MAX_DEPTH) {
        Box bins[BINS];
        uint32_t counts[BINS] = {0};
        for (int i = 0; i < BINS; ++i) boxEmpty(&bins[i]);
        const float scale = (float)BINS / extent;
#define BIN_OF(b) artfMini((int)((((b)->lo[axis] + (b)->hi[axis]) * .5f - lo) * scale), BINS - 1)
        for (uint32_t i = first; i < first + count; ++i) {
            const int bin = BIN_OF(&boxes[order[i]]);
            boxAdd(&bins[bin], &boxes[order[i]]);
            ++counts[bin];
        }
        float rightCost[BINS];
        Box side;
        boxEmpty(&side);
        uint32_t seen = 0;
        for (int i = BINS - 1; i > 0; --i) {
            boxAdd(&side, &bins[i]);
            seen += counts[i];
            rightCost[i] = boxArea(&side) * (float)seen;
        }
        boxEmpty(&side);
        seen = 0;
        float best = 3e38f;
        int cut = 0;
        for (int i = 0; i + 1 < BINS; ++i) {
            boxAdd(&side, &bins[i]);
            seen += counts[i];
            const float cost = boxArea(&side) * (float)seen + rightCost[i + 1];
            if (seen && seen < count && cost < best) { best = cost; cut = i + 1; }
        }
        if (cut) {
            uint32_t left = first, right = first + count;
            while (left < right) {
                if (BIN_OF(&boxes[order[left]]) < cut) ++left;
                else { const uint32_t t = order[left]; order[left] = order[--right]; order[right] = t; }
            }
            mid = left;
        }
#undef BIN_OF
    }
    const uint32_t children = (uint32_t)nodes->len;
    artfVecReserve(nodes, children + 2);
    nodes->len = children + 2;
    n = &nodes->items[at]; /* (the list may have moved) */
    n->first = children;
    n->count = 0;
    buildNode(nodes, children, boxes, order, first, mid - first, depth + 1);
    buildNode(nodes, children + 1, boxes, order, mid, first + count - mid, depth + 1);
}

/* A mesh's vertices, triangles and tree go at the end of the scene's. Its levels of detail have none: rays
 * do not care for distance (an instance drawing a level of what bends is met as its copy of the whole). */
static void addMesh(ArtfTrace* t, const ArtfMeshData* m) {
    const uint32_t firstVertex = (uint32_t)t->vertices.len, firstTriangle = (uint32_t)t->triangles.len, triangles = m->indexCount / 3;
    ArtfVertex* v = artfMalloc((size_t)m->vertexCount * sizeof *v);
    artfMeshUnpack(m, v);
    artfVecReserve(&t->vertices, firstVertex + m->vertexCount);
    for (uint32_t i = 0; i < m->vertexCount; ++i) {
        const ArtfVertex* a = &v[i];
        const ArtfTraceVertex o = {{a->px, a->py, a->pz}, a->u, {a->nx, a->ny, a->nz}, a->v, a->color ? a->color : 0xFFFFFFFFu, {0, 0, 0}};
        t->vertices.items[t->vertices.len++] = o;
    }
    Box* boxes = artfMalloc((size_t)triangles * sizeof *boxes);
    uint32_t* order = artfMalloc((size_t)triangles * 4);
    for (uint32_t i = 0; i < triangles; ++i) {
        boxEmpty(&boxes[i]);
        for (int c = 0; c < 3; ++c) {
            const ArtfVertex* a = &v[m->indices[i * 3 + (uint32_t)c]];
            Box p = {{a->px, a->py, a->pz}, {a->px, a->py, a->pz}};
            boxAdd(&boxes[i], &p);
        }
        order[i] = i;
    }
    const uint32_t root = (uint32_t)t->nodes.len, firstNode = root;
    ArtfTraceRange range = {root, 0, firstVertex, firstTriangle, triangles, m->deformVersion};
    artfVecReserve(&t->nodes, root + 1);
    t->nodes.len = root + 1;
    NodeList list = {t->nodes.items, t->nodes.len, t->nodes.cap};
    buildNode(&list, root, boxes, order, 0, triangles, 0);
    t->nodes.items = list.items;
    t->nodes.len = list.len;
    t->nodes.cap = list.cap;
    /* leaves point at the scene's triangles, and inner nodes at themselves already */
    for (size_t i = firstNode; i < t->nodes.len; ++i)
        if (t->nodes.items[i].count) t->nodes.items[i].first += firstTriangle;
    artfVecReserve(&t->triangles, firstTriangle + triangles);
    for (uint32_t i = 0; i < triangles; ++i) {
        const uint32_t* c = &m->indices[order[i] * 3];
        const ArtfTraceTriangle o = {{firstVertex + c[0], firstVertex + c[1], firstVertex + c[2]}, order[i]};
        t->triangles.items[t->triangles.len++] = o;
    }
    artfVecPush(&t->roots, root);
    range.nodeEnd = (uint32_t)t->nodes.len;
    artfVecPush(&t->ranges, range);
    artfFree(boxes);
    artfFree(order);
    artfFree(v);
}

/* New positions for a mesh a pose moved, and each box made to fit them again, from the leaves up: children
 * always come after their parent. The tree's shape stays. */
static void refit(ArtfTrace* t, const ArtfMeshData* m, ArtfTraceRange* range) {
    ArtfVertex* v = artfMalloc((size_t)m->vertexCount * sizeof *v);
    artfMeshUnpack(m, v);
    for (uint32_t i = 0; i < m->vertexCount; ++i) {
        ArtfTraceVertex* o = &t->vertices.items[range->firstVertex + i];
        o->back[0] = o->p[0] - v[i].px; o->back[1] = o->p[1] - v[i].py; o->back[2] = o->p[2] - v[i].pz;
        o->p[0] = v[i].px; o->p[1] = v[i].py; o->p[2] = v[i].pz;
        o->n[0] = v[i].nx; o->n[1] = v[i].ny; o->n[2] = v[i].nz;
    }
    artfFree(v);
    for (uint32_t i = range->nodeEnd; i-- > range->firstNode;) {
        ArtfTraceNode* n = &t->nodes.items[i];
        Box b;
        boxEmpty(&b);
        if (n->count) {
            for (uint32_t k = n->first; k < n->first + n->count; ++k)
                for (int c = 0; c < 3; ++c) {
                    const float* p = t->vertices.items[t->triangles.items[k].v[c]].p;
                    Box point = {{p[0], p[1], p[2]}, {p[0], p[1], p[2]}};
                    boxAdd(&b, &point);
                }
        } else {
            for (uint32_t k = 0; k < 2; ++k) {
                const ArtfTraceNode* child = &t->nodes.items[n->first + k];
                Box c;
                memcpy(c.lo, child->lo, sizeof c.lo);
                memcpy(c.hi, child->hi, sizeof c.hi);
                boxAdd(&b, &c);
            }
        }
        memcpy(n->lo, b.lo, sizeof n->lo);
        memcpy(n->hi, b.hi, sizeof n->hi);
    }
    range->deformVersion = m->deformVersion;
}

/* The rows of the inverse of a transform: world to object. false: it has no inverse. */
static bool invert(const float* m, float* out) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float k0 = e * i - f * h, k1 = c * h - b * i, k2 = b * f - c * e;
    const float det = a * k0 + d * k1 + g * k2;
    if (det == 0) return false;
    const float inv = 1.f / det;
    out[0] = k0 * inv; out[1] = k1 * inv; out[2] = k2 * inv;
    out[4] = (f * g - d * i) * inv; out[5] = (a * i - c * g) * inv; out[6] = (c * d - a * f) * inv;
    out[8] = (d * h - e * g) * inv; out[9] = (b * g - a * h) * inv; out[10] = (a * e - b * d) * inv;
    for (int r = 0; r < 3; ++r) out[r * 4 + 3] = -(out[r * 4] * m[3] + out[r * 4 + 1] * m[7] + out[r * 4 + 2] * m[11]);
    return true;
}

static void buildTop(ArtfScene* s) {
    ArtfTrace* t = &s->trace;
    const size_t count = s->instances.len;
    Box* boxes = artfMalloc((count + 1) * sizeof *boxes);
    uint32_t* order = artfMalloc((count + 1) * 4);
    ArtfTraceInstance* made = artfMalloc((count + 1) * sizeof *made);
    float* moved = artfMalloc((count + 1) * 48);
    float* worlds = artfMalloc((count + 1) * 48);
    uint32_t* meshes = artfMalloc((count + 1) * 4);
    uint32_t* handles = artfMalloc((count + 1) * 4);
    uint32_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        const ArtfInstanceData* d = &s->instances.items[i];
        ArtfTraceInstance o;
        memset(&o, 0, sizeof o);
        /* (particles are left to what draws, but for a frame that is all traced) */
        if (!d->opacity || (d->flags & ARTF_INSTANCE_PARTICLE && !t->particles) || !invert(d->m, o.toObject)) continue;
        const uint32_t mesh = artfTracedMesh(s, d->mesh);
        o.root = t->roots.items[mesh];
        o.material = d->material;
        o.opacity = (float)d->opacity * (1.f / 65535.f);
        /* (what bends light must be met on the way out, too) */
        o.twoSided = (s->materials.items[d->material].flags & ARTF_MATERIAL_DOUBLE || s->materials.items[d->material].ior > 0 ? 1u : 0u) | t->ranges.items[mesh].firstTriangle << 1;
        const ArtfTraceNode* root = &t->nodes.items[o.root];
        Box b;
        boxEmpty(&b);
        for (int corner = 0; corner < 8; ++corner) {
            const float x = corner & 1 ? root->hi[0] : root->lo[0], y = corner & 2 ? root->hi[1] : root->lo[1], z = corner & 4 ? root->hi[2] : root->lo[2];
            Box p;
            for (int k = 0; k < 3; ++k) p.lo[k] = p.hi[k] = d->m[k * 4] * x + d->m[k * 4 + 1] * y + d->m[k * 4 + 2] * z + d->m[k * 4 + 3];
            boxAdd(&b, &p);
        }
        /* from the world now, through the object, to the world before */
        const uint32_t handle = s->instanceHandles.items[i];
        while (t->seen.len < handle) {
            const ArtfTraceSeen none = {{0}, false};
            artfVecPush(&t->seen, none);
        }
        ArtfTraceSeen* seen = &t->seen.items[handle - 1];
        const float* before = seen->there ? seen->at : d->m;
        float* to = moved + (size_t)n * 12;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) {
                to[r * 4 + c] = before[r * 4] * o.toObject[c] + before[r * 4 + 1] * o.toObject[4 + c] + before[r * 4 + 2] * o.toObject[8 + c];
                if (c == 3) to[r * 4 + c] += before[r * 4 + 3];
            }
        memcpy(seen->at, d->m, sizeof seen->at);
        seen->there = true;
        memcpy(worlds + (size_t)n * 12, d->m, 48);
        meshes[n] = mesh;
        handles[n] = d->flags & ARTF_INSTANCE_PARTICLE ? 0 : (uint32_t)s->handleGenerations.items[handle - 1] << 24 | handle;
        boxes[n] = b;
        order[n] = n;
        made[n++] = o;
    }
    t->top.len = 0;
    artfVecReserve(&t->top, 1);
    t->top.len = 1;
    memset(t->top.items, 0, sizeof *t->top.items);
    NodeList list = {t->top.items, t->top.len, t->top.cap};
    if (n) buildNode(&list, 0, boxes, order, 0, n, 0);
    else {
        /* (a box nothing reaches) */
        for (int k = 0; k < 3; ++k) { list.items[0].lo[k] = 1.f; list.items[0].hi[k] = -1.f; }
    }
    t->top.items = list.items;
    t->top.len = list.len;
    t->top.cap = list.cap;
    artfVecReserve(&t->instances, n + 1);
    t->instances.len = n;
    artfVecReserve(&t->motion, (size_t)n * 12 + 1);
    t->motion.len = (size_t)n * 12;
    artfVecReserve(&t->instanceMeshes, n + 1);
    t->instanceMeshes.len = n;
    artfVecReserve(&t->instanceWorlds, (size_t)n * 12 + 1);
    t->instanceWorlds.len = (size_t)n * 12;
    artfVecReserve(&t->instanceHandles, n + 1);
    t->instanceHandles.len = n;
    for (uint32_t i = 0; i < n; ++i) {
        t->instances.items[i] = made[order[i]];
        memcpy(t->motion.items + (size_t)i * 12, moved + (size_t)order[i] * 12, 48);
        t->instanceMeshes.items[i] = meshes[order[i]];
        memcpy(t->instanceWorlds.items + (size_t)i * 12, worlds + (size_t)order[i] * 12, 48);
        t->instanceHandles.items[i] = handles[order[i]];
    }
    artfFree(handles);
    artfFree(moved);
    artfFree(worlds);
    artfFree(meshes);
    artfFree(boxes);
    artfFree(order);
    artfFree(made);
}

/* forPick: a pick asks between frames. What moved since the frame before must then stay for the next frame to see, as if
 * it had made the trees itself. */
static void traceSync(ArtfScene* s, bool forPick) {
    ArtfTrace* t = &s->trace;
    artfSceneSkinWholesPosed(s);
    /* what the trees hold, posed: not the levels of what bends, which the GPU may bend itself */
    for (size_t i = 0; i < s->deforming.len; ++i) {
        ArtfMeshData* m = &s->meshes.items[s->deforming.items[i]];
        if (!m->isLod && m->pose.applied != m->deformVersion) artfMeshDeform(m);
    }
    bool changed = false, made = false;
    const bool keep = forPick || t->held;
    /* a mesh went: the arrays start over, with what is left */
    if (t->meshRemovals != s->meshRemovals) {
        t->meshRemovals = s->meshRemovals;
        t->nodes.len = t->triangles.len = t->vertices.len = t->roots.len = t->ranges.len = 0;
    }
    while (t->roots.len < s->meshes.len) {
        const ArtfMeshData* m = &s->meshes.items[t->roots.len];
        if (m->isLod || m->removed) {
            const ArtfTraceRange none = {0, 0, 0, 0};
            artfVecPush(&t->roots, 0);
            artfVecPush(&t->ranges, none);
        } else {
            addMesh(t, m);
        }
        changed = true;
    }
    for (size_t i = 0; i < s->deforming.len; ++i) {
        const uint32_t mesh = s->deforming.items[i];
        /* (a level has no place in the trees: its instance is met as its whole) */
        if (s->meshes.items[mesh].isLod || s->meshes.items[mesh].removed) continue;
        ArtfTraceRange* range = &t->ranges.items[mesh];
        if (range->deformVersion == s->meshes.items[mesh].deformVersion) {
            /* (a pose that stays: nothing moved since the frame before) */
            if (range->moved && !keep) {
                for (uint32_t k = 0; k < s->meshes.items[mesh].vertexCount; ++k) memset(t->vertices.items[range->firstVertex + k].back, 0, 12);
                range->moved = false;
                changed = true;
            }
            continue;
        }
        refit(t, &s->meshes.items[mesh], range);
        range->moved = true;
        changed = made = true;
    }
    if (changed) ++t->meshVersion;
    const bool particles = t->desc.mode != ARTF_TRACE_NONE;
    const uint64_t instances = particles ? s->instanceVersion : s->tracedVersion;
    if (particles != t->particles) {
        t->particles = particles;
        changed = true;
    }
    if (changed || !t->top.len || t->instanceVersion != instances || t->structureVersion != s->structureVersion || t->materialVersion != s->materialVersion) {
        t->instanceVersion = instances;
        t->structureVersion = s->structureVersion;
        t->materialVersion = s->materialVersion;
        buildTop(s);
        ++t->topVersion;
        t->motionLive = true;
        ++t->motionVersion;
        made = true;
    } else if (t->motionLive && !keep) {
        static const float still[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        for (size_t i = 0; i < t->motion.len; i += 12) memcpy(t->motion.items + i, still, sizeof still);
        t->motionLive = false;
        ++t->motionVersion;
    }
    t->held = forPick ? t->held || made : false;
}

void artfSceneTraceSync(ArtfScene* s) { traceSync(s, false); }

void artfTraceFini(ArtfTrace* t) {
    artfVecFree(&t->nodes);
    artfVecFree(&t->triangles);
    artfVecFree(&t->vertices);
    artfVecFree(&t->roots);
    artfVecFree(&t->ranges);
    artfVecFree(&t->top);
    artfVecFree(&t->instances);
    artfVecFree(&t->motion);
    artfVecFree(&t->instanceMeshes);
    artfVecFree(&t->instanceWorlds);
    artfVecFree(&t->instanceHandles);
    artfVecFree(&t->seen);
}

void artfSceneSetTrace(ArtfScene* s, const ArtfTraceDesc* desc) {
    memset(&s->trace.desc, 0, sizeof s->trace.desc);
    if (!desc) return;
    s->trace.desc = *desc;
    s->trace.desc.samples = artfMaxi(desc->samples, 1);
    if (desc->bounces <= 0) s->trace.desc.bounces = desc->mode == ARTF_TRACE_PATHS ? 6 : 3;
}

void artfSceneTraceFrame(const ArtfScene* s, const ArtfFrameData* frame, int width, int height, uint32_t done, ArtfTraceFrame* out) {
    memset(out, 0, sizeof *out);
    const ArtfCamera* c = &s->camera;
    const ArtfVec3 f = artfVec3Normalize(artfVec3Sub(c->target, c->position));
    const ArtfVec3 r = artfVec3Normalize(artfVec3Cross(f, c->up));
    const ArtfVec3 u = artfVec3Cross(r, f);
    const float rx = 1.f / frame->focalX, uy = 1.f / frame->focalY;
    const ArtfVec3 eye = c->position, right = artfVec3Scale(r, rx), up = artfVec3Scale(u, uy);
    memcpy(out->eye, &eye, 12);
    memcpy(out->right, &right, 12);
    memcpy(out->up, &up, 12);
    memcpy(out->forward, &f, 12);
    /* (orthographic: right and up are half the view's width and height, in the world, and all rays go forward) */
    out->eye[3] = artfOrtho(frame) ? 1.f : 0.f;
    out->width = (uint32_t)width;
    out->height = (uint32_t)height;
    out->done = done;
    out->samples = (uint32_t)s->trace.desc.samples;
    out->bounces = (uint32_t)s->trace.desc.bounces;
    out->paths = s->trace.desc.mode == ARTF_TRACE_PATHS;
}

/* What the picture depends on: when this changes, what was gathered for it goes. */
uint64_t artfSceneTraceKey(const ArtfScene* s, const ArtfFrameData* frame) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = (const uint8_t*)frame;
    for (size_t i = 0; i < offsetof(ArtfFrameData, hiz); ++i)
        if (i < offsetof(ArtfFrameData, hizLevels) || i >= offsetof(ArtfFrameData, lights)) h = (h ^ p[i]) * 1099511628211ull;
    const uint64_t versions[10] = {s->trace.desc.denoise, s->trace.desc.hardware, s->id, s->textureVersion, s->instanceVersion, s->structureVersion, s->materialVersion, (uint64_t)s->trace.desc.mode, (uint64_t)s->trace.desc.bounces, s->environmentVersion};
    for (int i = 0; i < 10; ++i) h = (h ^ versions[i]) * 1099511628211ull;
    for (size_t i = offsetof(ArtfFrameData, look); i < sizeof *frame; ++i) h = (h ^ p[i]) * 1099511628211ull;
    for (uint32_t i = 0; i < frame->lightCount; ++i) {
        const uint8_t* l = (const uint8_t*)&frame->lights[i];
        for (size_t k = 0; k < sizeof frame->lights[i]; ++k) h = (h ^ l[k]) * 1099511628211ull;
    }
    return h;
}

/* ---- a ray ---- */

typedef struct Hit {
    float t, u, v; /* the triangle's second and third vertices' shares */
    uint32_t instance, triangle;
} Hit;

static inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* Where a ray enters a box; a number past `limit`, if it does not. */
static inline float boxEnter(const ArtfTraceNode* n, const float* o, const float* inv, float limit) {
    float near_ = 0, far = limit;
    for (int k = 0; k < 3; ++k) {
        const float a = (n->lo[k] - o[k]) * inv[k], b = (n->hi[k] - o[k]) * inv[k];
        near_ = artfMaxf(near_, artfMinf(a, b));
        far = artfMinf(far, artfMaxf(a, b));
    }
    return near_ <= far ? near_ : 3e38f;
}

static inline void inverseOf(const float* d, float* inv) {
    for (int k = 0; k < 3; ++k) inv[k] = 1.f / (fabsf(d[k]) > 1e-20f ? d[k] : 1e-20f);
}

/* The nearest surface along a ray in one instance. d is not of length 1 here: t stays the world's. */
static void hitMesh(const ArtfTrace* t, const ArtfTraceInstance* inst, uint32_t index, const float* o, const float* d, bool bothSides, Hit* best) {
    float inv[3];
    inverseOf(d, inv);
    uint32_t stack[ARTF_TRACE_STACK], depth = 0, at = inst->root;
    for (;;) {
        const ArtfTraceNode* n = &t->nodes.items[at];
        if (n->count) {
            for (uint32_t i = n->first; i < n->first + n->count; ++i) {
                const ArtfTraceTriangle* tri = &t->triangles.items[i];
                const float* a = t->vertices.items[tri->v[0]].p;
                const float* b = t->vertices.items[tri->v[1]].p;
                const float* c = t->vertices.items[tri->v[2]].p;
                const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
                const float det = dot3(e1, p);
                /* (counter-clockwise from the front is above 0: a back that does not show is not there) */
                if (inst->twoSided & 1 || bothSides ? det == 0 : det <= 0) continue;
                const float id = 1.f / det;
                const float s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
                const float u = dot3(s, p) * id;
                if (u < 0 || u > 1) continue;
                const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
                const float v = dot3(d, q) * id;
                if (v < 0 || u + v > 1) continue;
                const float dist = dot3(e2, q) * id;
                if (dist <= 0 || dist >= best->t) continue;
                best->t = dist;
                best->u = u;
                best->v = v;
                best->instance = index;
                best->triangle = i;
            }
        } else {
            const float l = boxEnter(&t->nodes.items[n->first], o, inv, best->t), r = boxEnter(&t->nodes.items[n->first + 1], o, inv, best->t);
            const uint32_t near_ = l <= r ? n->first : n->first + 1, far = l <= r ? n->first + 1 : n->first;
            const float nearT = artfMinf(l, r), farT = artfMaxf(l, r);
            if (farT < 3e38f) stack[depth++] = far;
            if (nearT < 3e38f) { at = near_; continue; }
        }
        if (!depth) return;
        at = stack[--depth];
    }
}

/* The nearest surface along a ray, up to limit. bothSides: a shadow's rays meet backs as well, as what
 * cannot be seen from behind still stops the light that comes from there. */
static bool closest(const ArtfTrace* t, const float* o, const float* d, float limit, bool bothSides, Hit* best) {
    float inv[3];
    inverseOf(d, inv);
    best->t = limit;
    best->u = best->v = 0;
    best->triangle = 0;
    best->instance = UINT32_MAX;
    if (boxEnter(&t->top.items[0], o, inv, limit) >= 3e38f) return false;
    uint32_t stack[ARTF_TRACE_STACK], depth = 0, at = 0;
    for (;;) {
        const ArtfTraceNode* n = &t->top.items[at];
        if (n->count) {
            for (uint32_t i = n->first; i < n->first + n->count; ++i) {
                const ArtfTraceInstance* inst = &t->instances.items[i];
                const float* m = inst->toObject;
                const float oo[3] = {m[0] * o[0] + m[1] * o[1] + m[2] * o[2] + m[3], m[4] * o[0] + m[5] * o[1] + m[6] * o[2] + m[7], m[8] * o[0] + m[9] * o[1] + m[10] * o[2] + m[11]};
                const float od[3] = {m[0] * d[0] + m[1] * d[1] + m[2] * d[2], m[4] * d[0] + m[5] * d[1] + m[6] * d[2], m[8] * d[0] + m[9] * d[1] + m[10] * d[2]};
                hitMesh(t, inst, i, oo, od, bothSides, best);
            }
        } else {
            const float l = boxEnter(&t->top.items[n->first], o, inv, best->t), r = boxEnter(&t->top.items[n->first + 1], o, inv, best->t);
            const uint32_t near_ = l <= r ? n->first : n->first + 1, far = l <= r ? n->first + 1 : n->first;
            const float nearT = artfMinf(l, r), farT = artfMaxf(l, r);
            if (farT < 3e38f) stack[depth++] = far;
            if (nearT < 3e38f) { at = near_; continue; }
        }
        if (!depth) return best->instance != UINT32_MAX;
        at = stack[--depth];
    }
}

/* ---- what a ray met ---- */

typedef struct Surface {
    const ArtfMaterialData* material;
    float normal[3];   /* toward where the ray came from */
    float albedo[3];
    float alpha;       /* how much of it is there */
    bool front;        /* the ray came to its front */
    bool shiny;
    float rough, open;
    artfShadeLobe lobe; /* its highlight's (shading.inc), drawn out or not */
    float f0[3], mirror[3]; /* what it reflects head on, and toward the eye (gain in it) */
    float gain[3];          /* what its lobe is multiplied by, for the light the lobe loses (shading.inc's Gain) */
    float emissive[3];
    /* past glTF's core (ArtfMaterialMore): what it has (ARTF_MORE_); how much coat, its lobe and normal; what the coat
     * leaves of what is under it; the sheen's color and 1 / its alpha, and the color of what comes from under the
     * surface, as much as the coat leaves of them; 1 / how far that goes, by log2; what its inside takes, traced */
    uint32_t flags;
    float coat, coatNormal[3], under, sheen[3], sheenInverse, subsurface[3], reach;
    artfShadeLobe coatLobe;
    const float* absorb;
} Surface;

static inline void normalize3(float* a) {
    const float l = 1.f / sqrtf(artfMaxf(dot3(a, a), 1e-30f));
    for (int k = 0; k < 3; ++k) a[k] *= l;
}

void artfTexelAt(const ArtfTextureData* t, float u, float v, float* out) { artfTexelAtLevel(t, t->residentMip, u, v, out); }

void artfTexelAtLevel(const ArtfTextureData* t, int level, float u, float v, float* out) {
    const int width = artfMaxi(t->width >> level, 1), height = artfMaxi(t->height >> level, 1);
    const uint32_t* px = t->pixels + t->offsets[level];
    const float x = (u - floorf(u)) * (float)width - .5f, y = (v - floorf(v)) * (float)height - .5f;
    const float bx = floorf(x), by = floorf(y), tx = x - bx, ty = y - by;
    const int x0 = ((int)bx + width) % width, y0 = ((int)by + height) % height;
    const int x1 = (x0 + 1) % width, y1 = (y0 + 1) % height;
    const uint32_t p[4] = {px[(size_t)y0 * width + x0], px[(size_t)y0 * width + x1], px[(size_t)y1 * width + x0], px[(size_t)y1 * width + x1]};
    const float w[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
    for (int c = 0; c < 4; ++c) {
        float sum = 0;
        for (int k = 0; k < 4; ++k) sum += w[k] * (float)(p[k] >> (c * 8) & 255);
        out[c] = sum * (1.f / 255.f);
    }
}

/* A texel of t by the cone's width (ray cones: Akenine-Möller et al., "Improved Shader and Texture Level of Detail Using
 * Ray Cones", JCGT 2021): the level where a texel is about as wide as the cone, from its first held, between two.
 * lodBase: log2 of the cone's width over a texel of a texture of one texel here (the surface's own; each texture adds
 * half the log2 of its count). As the tracing shader's traceTexture, which asks the GPU for a texture's size. */
static void texelCone(const ArtfTextureData* t, float lodBase, float u, float v, float* out) {
    const int top = artfMini(t->residentMip, t->mipCount - 1);
    /* (half the log2 of its texels, by their sides' highest bits: as the shader's FindUMsb) */
    int bits = 0;
    for (uint32_t w = (uint32_t)artfMaxi(t->width >> top, 1); w > 1; w >>= 1) ++bits;
    for (uint32_t h = (uint32_t)artfMaxi(t->height >> top, 1); h > 1; h >>= 1) ++bits;
    const float most = (float)(t->mipCount - 1 - top);
    const float at = artfClampf(lodBase + .5f * (float)bits, 0.f, most);
    const int first = (int)at;
    const float share = at - (float)first;
    artfTexelAtLevel(t, top + first, u, v, out);
    if (share > 0) {
        float next[4];
        artfTexelAtLevel(t, top + first + 1, u, v, next);
        for (int c = 0; c < 4; ++c) out[c] += (next[c] - out[c]) * share;
    }
}

/* cone: how wide the ray's cone is where it met the surface; 0, a ray (the finest level). */
static void surfaceAt(const ArtfScene* s, const Hit* h, const float* origin, const float* d, float cone, Surface* out) {
    const ArtfTrace* t = &s->trace;
    const ArtfTraceInstance* inst = &t->instances.items[h->instance];
    const ArtfTraceTriangle* tri = &t->triangles.items[h->triangle];
    const ArtfTraceVertex *a = &t->vertices.items[tri->v[0]], *b = &t->vertices.items[tri->v[1]], *c = &t->vertices.items[tri->v[2]];
    const float wa = 1.f - h->u - h->v, wb = h->u, wc = h->v;
    const ArtfMaterialData* m = &s->materials.items[inst->material];
    out->material = m;

    const float u = a->u * wa + b->u * wb + c->u * wc, v = a->v * wa + b->v * wb + c->v * wc;
    const ArtfTextureData* textures = s->textures.items;
    const float* k = inst->toObject;
    /* how many of a texture's texels the cone covers here: the triangle's share of a texture over its share of the
     * world (twice each: the world's by the instance, through the inverse's transpose and its determinant), and the
     * cone over how squarely it meets the triangle */
    float lodBase = -100.f;
    if (cone > 0) {
        float e1[3], e2[3];
        for (int i = 0; i < 3; ++i) {
            e1[i] = b->p[i] - a->p[i];
            e2[i] = c->p[i] - a->p[i];
        }
        const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float w[3] = {k[0] * cr[0] + k[4] * cr[1] + k[8] * cr[2], k[1] * cr[0] + k[5] * cr[1] + k[9] * cr[2], k[2] * cr[0] + k[6] * cr[1] + k[10] * cr[2]};
        const float det = k[0] * (k[5] * k[10] - k[6] * k[9]) - k[1] * (k[4] * k[10] - k[6] * k[8]) + k[2] * (k[4] * k[9] - k[5] * k[8]);
        const float wl = sqrtf(dot3(w, w)), world = wl / artfMaxf(fabsf(det), 1e-30f);
        const float area = fabsf((b->u - a->u) * (c->v - a->v) - (c->u - a->u) * (b->v - a->v));
        const float facing = artfMaxf(fabsf(dot3(w, d)) / artfMaxf(wl, 1e-30f), 1e-3f);
        if (area > 0 && world > 0) lodBase = .5f * artfLog2Fast(area / world) + artfLog2Fast(cone / facing);
    }
    float surface[4] = {1, 1, 1, 1}, occlusion[4] = {1, 1, 1, 1};
    if (m->surfaceMap) texelCone(&textures[m->surfaceMap], lodBase, u, v, surface);
    if (m->occlusionMap) texelCone(&textures[m->occlusionMap], lodBase, u, v, occlusion);

    /* normals go by the inverse's transpose; the same with the color's channels and alpha, by weight */
    float on[3] = {a->n[0] * wa + b->n[0] * wb + c->n[0] * wc, a->n[1] * wa + b->n[1] * wb + c->n[1] * wc, a->n[2] * wa + b->n[2] * wb + c->n[2] * wc};
    const ArtfMaterialMore* more = &s->materialsMore.items[inst->material];
    const float plain[3] = {on[0], on[1], on[2]};
    float tangent[3] = {0, 0, 0}, bitangent[3] = {0, 0, 0}, ts = 0, bs = 0;
    if (m->normalMap || ARTF_MORE_TANGENT(more)) {
        /* where u and v go on the triangle, in the object: as in kernels.inc */
        const float du1 = b->u - a->u, dv1 = b->v - a->v, du2 = c->u - a->u, dv2 = c->v - a->v;
        const float det = du1 * dv2 - du2 * dv1, inv = det != 0 ? 1.f / det : 0.f;
        for (int i = 0; i < 3; ++i) {
            const float e1 = b->p[i] - a->p[i], e2 = c->p[i] - a->p[i];
            tangent[i] = (e1 * dv2 - e2 * dv1) * inv;
            bitangent[i] = (e2 * du1 - e1 * du2) * inv;
        }
        const float tl = dot3(tangent, tangent), bl = dot3(bitangent, bitangent);
        ts = tl > 0 ? 1.f / sqrtf(tl) : 0.f;
        bs = bl > 0 ? 1.f / sqrtf(bl) : 0.f;
    }
    if (m->normalMap) {
        float bumps[4];
        texelCone(&textures[m->normalMap], lodBase, u, v, bumps);
        const float x = (bumps[2] * 2.f - 1.f) * m->normalScale, y = (1.f - bumps[1] * 2.f) * m->normalScale, z = bumps[0] * 2.f - 1.f;
        for (int i = 0; i < 3; ++i) on[i] = tangent[i] * ts * x + bitangent[i] * bs * y + on[i] * z;
    }
    float n[3] = {k[0] * on[0] + k[4] * on[1] + k[8] * on[2], k[1] * on[0] + k[5] * on[1] + k[9] * on[2], k[2] * on[0] + k[6] * on[1] + k[10] * on[2]};
    const float length = 1.f / sqrtf(dot3(n, n));
    /* (toward the ray: the back of a two sided surface, and normals that lean past an edge) */
    out->front = dot3(n, d) <= 0;
    const float side = out->front ? length : -length;
    for (int i = 0; i < 3; ++i) out->normal[i] = n[i] * side;

    float color[4];
    for (int ch = 0; ch < 4; ++ch) {
        const int shift = ch == 3 ? 24 : 16 - ch * 8;
        float x = (float)(a->color >> shift & 255) * (1.f / 255.f), y = (float)(b->color >> shift & 255) * (1.f / 255.f), z = (float)(c->color >> shift & 255) * (1.f / 255.f);
        if (ch < 3) { x = artfSrgbDecode(x); y = artfSrgbDecode(y); z = artfSrgbDecode(z); }
        color[ch] = x * wa + y * wb + z * wc;
    }
    float texel[4] = {1, 1, 1, 1};
    if (m->texture) texelCone(&textures[m->texture], lodBase, u, v, texel);
    float albedo[3];
    for (int i = 0; i < 3; ++i) albedo[i] = m->baseColor[i] * artfSrgbDecode(texel[2 - i]) * color[i];
    float metal = m->metallic * surface[0];
    out->rough = artfMaxf(m->roughness * surface[1], .045f);
    float graphAlpha = 1.f;
    float emissive[3];
    memcpy(emissive, m->emissive, 12);
    if (m->graph) {
        /* (where it is in the world: from the ray that got here) */
        ArtfGraphInput in;
        ArtfGraphResult result;
        in.u = u;
        in.v = v;
        for (int i = 0; i < 3; ++i) {
            in.world[i] = origin[i] + d[i] * h->t;
            in.normal[i] = out->normal[i];
            in.toEye[i] = -d[i];
            in.base[i] = albedo[i];
            result.color[i] = albedo[i];
            result.emissive[i] = emissive[i];
            result.normal[i] = out->normal[i];
        }
        result.roughness = out->rough;
        result.metallic = metal;
        result.alpha = 1.f;
        artfGraphRun(s, &s->graphs.items[m->graph - 1], m, &in, &result);
        memcpy(albedo, result.color, 12);
        memcpy(emissive, result.emissive, 12);
        memcpy(out->normal, result.normal, 12);
        out->rough = result.roughness;
        metal = result.metallic;
        graphAlpha = result.alpha;
    }
    if (m->flags & ARTF_MATERIAL_UNLIT) {
        /* its color is light of its own, and it takes none */
        for (int i = 0; i < 3; ++i) {
            emissive[i] += albedo[i];
            albedo[i] = 0;
        }
        metal = 0;
    }
    memcpy(out->emissive, emissive, 12);
    const float notMetal = 1.f - metal;
    out->open = 1.f + (occlusion[2] - 1.f) * m->occlusion;
    out->shiny = (m->specular > 0 || m->metallic > 0) && !(m->flags & ARTF_MATERIAL_UNLIT);
    const float ndv = artfMaxf(-dot3(out->normal, d), 1e-4f), far = artfShadeFifth(ndv);
    out->lobe = artfShadeLobeOf(out->rough, ndv);
    const uint32_t flags = m->flags & ARTF_MATERIAL_UNLIT ? more->flags & ARTF_MORE_VOLUME : more->flags;
    out->flags = flags;
    out->absorb = more->absorb;
    for (int i = 0; i < 3; ++i) out->f0[i] = artfShadeReflects(.04f * m->specular * (flags & ARTF_MORE_SPECULAR ? more->specularColor[i] : 1.f), notMetal, albedo[i], metal);
    if (flags & ARTF_MORE_IRIDESCENCE) artfIridescence(out->f0, ndv, more->iridescence, more->iridescenceIor, more->iridescenceThickness);
    const float unkept = artfShadeUnkept(ndv, out->rough);
    for (int i = 0; i < 3; ++i) {
        out->gain[i] = artfShadeGain(out->f0[i], unkept);
        out->mirror[i] = out->shiny ? artfShadeMirror(out->f0[i], 1.f - out->rough, far, 1.f) * out->gain[i] : 0.f;
        out->albedo[i] = albedo[i] * notMetal;
    }
    if (flags & ARTF_MORE_ANISOTROPY) {
        /* highlights drawn out, as in kernels.inc: the way u grows, into the world (by the inverse of toObject), square
         * to the normal, turned as asked */
        const float* r0 = k;
        const float* r1 = k + 4;
        const float* r2 = k + 8;
        const float c12[3] = {r1[1] * r2[2] - r1[2] * r2[1], r1[2] * r2[0] - r1[0] * r2[2], r1[0] * r2[1] - r1[1] * r2[0]};
        const float c20[3] = {r2[1] * r0[2] - r2[2] * r0[1], r2[2] * r0[0] - r2[0] * r0[2], r2[0] * r0[1] - r2[1] * r0[0]};
        const float c01[3] = {r0[1] * r1[2] - r0[2] * r1[1], r0[2] * r1[0] - r0[0] * r1[2], r0[0] * r1[1] - r0[1] * r1[0]};
        const float sign = r0[0] * c12[0] + r0[1] * c12[1] + r0[2] * c12[2] < 0 ? -1.f : 1.f;
        float t[3];
        for (int i = 0; i < 3; ++i) t[i] = (c12[i] * tangent[0] + c20[i] * tangent[1] + c01[i] * tangent[2]) * sign;
        const float along = dot3(out->normal, t);
        for (int i = 0; i < 3; ++i) t[i] -= out->normal[i] * along;
        normalize3(t);
        const float* nn = out->normal;
        const float bt[3] = {nn[1] * t[2] - nn[2] * t[1], nn[2] * t[0] - nn[0] * t[2], nn[0] * t[1] - nn[1] * t[0]};
        const float alpha = out->rough * out->rough;
        /* (how long: artfShadeDrawnLong's, but for the order the strength is squared in) */
        const float il = 1.f / (alpha + (1.f - alpha) * more->anisotropy * more->anisotropy);
        artfShadeDraw(&out->lobe, true, (ArtfShade3){t[0], t[1], t[2]}, (ArtfShade3){bt[0], bt[1], bt[2]}, more->anisotropyDirection[0], more->anisotropyDirection[1], alpha, il);
    }
    /* a coat over all that, and sheen and what comes from under it beneath that: the coat takes what its Fresnel
     * reflects, and what is under keeps the rest, mirror included; the coat mirrors as smoothly as it is */
    out->coat = 0;
    out->under = 1.f;
    for (int i = 0; i < 3; ++i) {
        out->sheen[i] = out->subsurface[i] = 0;
        out->coatNormal[i] = out->normal[i];
    }
    if (flags & ARTF_MORE_DRAWN) {
        float coat = more->coat, coatRough = more->coatRoughness, sheenRough = more->sheenRoughness, sheen[3] = {more->sheen[0], more->sheen[1], more->sheen[2]};
        if (flags & ARTF_MORE_MAPS) {
            float texel[4];
            if (more->coatMap) { texelCone(&textures[more->coatMap], lodBase, u, v, texel); coat *= texel[2]; }
            if (more->coatRoughnessMap) { texelCone(&textures[more->coatRoughnessMap], lodBase, u, v, texel); coatRough = artfMaxf(coatRough * texel[1], .045f); }
            if (more->sheenMap) {
                texelCone(&textures[more->sheenMap], lodBase, u, v, texel);
                for (int i = 0; i < 3; ++i) sheen[i] *= artfSrgbDecode(texel[2 - i]);
            }
            if (more->sheenRoughnessMap) { texelCone(&textures[more->sheenRoughnessMap], lodBase, u, v, texel); sheenRough = artfMaxf(sheenRough * texel[3], .07f); }
            if (more->coatNormalMap) {
                /* (bent from the surface as it is, not as its own normal map bends it) */
                texelCone(&textures[more->coatNormalMap], lodBase, u, v, texel);
                const float x = (texel[2] * 2.f - 1.f) * more->coatNormalScale, yy = (1.f - texel[1] * 2.f) * more->coatNormalScale, z = texel[0] * 2.f - 1.f;
                float co[3];
                for (int i = 0; i < 3; ++i) co[i] = tangent[i] * ts * x + bitangent[i] * bs * yy + plain[i] * z;
                float cn[3] = {k[0] * co[0] + k[4] * co[1] + k[8] * co[2], k[1] * co[0] + k[5] * co[1] + k[9] * co[2], k[2] * co[0] + k[6] * co[1] + k[10] * co[2]};
                normalize3(cn);
                for (int i = 0; i < 3; ++i) out->coatNormal[i] = out->front ? cn[i] : -cn[i];
            }
        }
        const float coatNv = artfMaxf(-dot3(out->coatNormal, d), 1e-4f), coatFar = artfShadeFifthInRow(coatNv);
        out->coat = coat;
        out->coatLobe = artfShadeLobeOf(coatRough, coatNv);
        out->under = 1.f - coat * artfShadeCoatFresnel(coatFar);
        out->sheenInverse = 1.f / (sheenRough * sheenRough);
        out->reach = 1.44269504f / more->subsurfaceRadius;
        const float coatMirror = artfShadeMirror(.04f, 1.f - coatRough, coatFar, 1.f);
        for (int i = 0; i < 3; ++i) {
            out->albedo[i] *= out->under;
            out->mirror[i] = out->mirror[i] * out->under + coat * coatMirror;
            out->sheen[i] = flags & ARTF_MORE_SHEEN ? sheen[i] * notMetal * out->under : 0.f;
            out->subsurface[i] = flags & ARTF_MORE_SUBSURFACE ? more->subsurface[i] * notMetal * out->under : 0.f;
        }
        out->shiny = true;
    }
    const float coverage = texel[3] * color[3];
    out->alpha = inst->opacity * m->opacity * color[3] * graphAlpha;
    if (m->flags & ARTF_MATERIAL_BLEND) out->alpha *= texel[3];
    if (m->flags & ARTF_MATERIAL_MASK && coverage < m->cutoff) out->alpha = 0;
}

/* ---- a path ---- */

bool artfTraceAim = true;
bool artfTraceCones = true;
bool artfWidenHighlights = true; /* (kernels.inc's, declared with these) */

static inline uint32_t hashOf(uint32_t x) {
    x = (x ^ x >> 16) * 0x7FEB352Du;
    x = (x ^ x >> 15) * 0x846CA68Bu;
    return x ^ x >> 16;
}
static inline float randomOf(uint32_t* state) {
    *state = *state * 747796405u + 2891336453u;
    return (float)(hashOf(*state) >> 8) * (1.f / 16777216.f);
}

/* A point even in a ball, from three of a path's numbers (scene/sampling.h): a direction (its height even from -1 to 1,
 * and an angle round), and how far out (the cube root, so that the ball's volume is even). No sine or cosine of a C
 * library, which differ from one to the next: artfCosFast, as the tracing shader's ballOfSpv. */
static void directionOf(const float* u, float* out) {
    const float z = 1.f - 2.f * u[0], across = sqrtf(artfMaxf(1.f - z * z, 0.f)), turn = 6.28318531f * u[1];
    out[0] = across * artfCosFast(turn);
    out[1] = across * artfCosFast(turn - 1.57079633f);
    out[2] = z;
}
static void ballOf(const float* u, float* out) {
    directionOf(u, out);
    const float r = artfExp2Fast(artfLog2Fast(u[2]) * (1.f / 3.f));
    for (int k = 0; k < 3; ++k) out[k] *= r;
}

/* Trowbridge and Reitz's distribution (GGX) of alpha: how much of a surface's microfacets face h (its cosine), and
 * Smith's Lambda, of which masking is made, for a direction of this cosine. As the tracing shader's. */
static float ggxD(float alpha, float nh) {
    const float a2 = alpha * alpha, k = nh * nh * (a2 - 1.f) + 1.f;
    return a2 / (3.14159265f * k * k);
}
static float ggxLambda(float alpha, float c) {
    const float c2 = artfMaxf(c * c, 1e-8f);
    return (sqrtf(1.f + alpha * alpha * (1.f - c2) / c2) - 1.f) * .5f;
}

/* A normal of the microsurface, as the view sees them (GGX's visible normals, by a spherical cap: Dupuy and Benyoub,
 * 2023), from two numbers: what a highlight reflects is around it. n and v unit; v toward the eye. */
static void visibleNormalOf(const float* n, const float* v, float alpha, const float* u, float* out) {
    /* (a frame around n, with no branch: Duff et al., 2017) */
    const float sign = n[2] >= 0 ? 1.f : -1.f, a = -1.f / (sign + n[2]), b = n[0] * n[1] * a;
    const float t[3] = {1.f + sign * n[0] * n[0] * a, sign * b, -sign * n[0]}, bt[3] = {b, sign + n[1] * n[1] * a, -n[1]};
    float sx = dot3(v, t) * alpha, sy = dot3(v, bt) * alpha, sz = artfMaxf(dot3(v, n), 1e-4f);
    const float inverse = 1.f / sqrtf(sx * sx + sy * sy + sz * sz);
    sx *= inverse;
    sy *= inverse;
    sz *= inverse;
    const float turn = 6.28318531f * u[0], z = (1.f - u[1]) * (1.f + sz) - sz, across = sqrtf(artfClampf(1.f - z * z, 0.f, 1.f));
    float cx = (across * artfCosFast(turn) + sx) * alpha, cy = (across * artfCosFast(turn - 1.57079633f) + sy) * alpha, cz = z + sz;
    const float back = 1.f / sqrtf(cx * cx + cy * cy + cz * cz);
    cx *= back;
    cy *= back;
    cz *= back;
    for (int k = 0; k < 3; ++k) out[k] = t[k] * cx + bt[k] * cy + n[k] * cz;
}

/* What a highlight reflects (visibleNormalOf): the way it goes, how much of m.mirror it keeps (G2 / G1, of the way it
 * goes and the way it is seen; 0 below the surface), and how likely it was, if the highlight is taken with this chance.
 * Worked out only where a highlight is taken: it is most of what a bounce costs. */
static void glossyOf(const Surface* m, const float* d, const float* view, float alpha, float nv, const float* u, float chance, float* out, float* kept, float* likely) {
    float facet[3];
    visibleNormalOf(m->normal, view, alpha, u, facet);
    const float df = dot3(d, facet);
    for (int k = 0; k < 3; ++k) out[k] = d[k] - 2.f * df * facet[k];
    const float lambdaView = ggxLambda(alpha, nv), cosine = dot3(m->normal, out);
    *kept = cosine > 0 ? (1.f + lambdaView) / (1.f + lambdaView + ggxLambda(alpha, cosine)) : 0.f;
    *likely = chance * ggxD(alpha, artfMaxf(dot3(m->normal, facet), 0.f)) / ((1.f + lambdaView) * 4.f * nv);
}

/* A direction, even in a ball, by rejection: for the probes, whose rays the GPU draws in the same order. */
static void randomInBall(uint32_t* state, float* out) {
    for (int i = 0; i < 16; ++i) {
        for (int k = 0; k < 3; ++k) out[k] = randomOf(state) * 2.f - 1.f;
        if (dot3(out, out) <= 1.f) return;
    }
    out[0] = out[1] = out[2] = 0;
}

static inline float reachOf(const float* p) { return 1e-4f * (1.f + artfMaxf(fabsf(p[0]), artfMaxf(fabsf(p[1]), fabsf(p[2])))); }

/* How much light gets from a place to a light: what is in the way stops it as far as it is there. */
static float lightThrough(const ArtfScene* s, const float* from, const float* to, float limit) {
    float o[3] = {from[0], from[1], from[2]}, through = 1.f;
    for (int i = 0; i < SHADOW_STEPS; ++i) {
        Hit h;
        if (!closest(&s->trace, o, to, limit, true, &h)) return through;
        Surface m;
        surfaceAt(s, &h, o, to, 0.f, &m);
        through *= 1.f - artfClampf(m.alpha, 0, 1);
        if (through < .01f) return 0;
        const float step = h.t + reachOf(o);
        for (int k = 0; k < 3; ++k) o[k] += to[k] * step;
        limit -= step;
        if (limit <= 0) return through;
    }
    return through;
}

/* The same as addLight in kernels.inc, with what stands in the way. */
static void lightFrom(const ArtfScene* s, const float* p, const float* view, const Surface* m, const float* toLight, float limit, float strength, const float* color, const float* path, float* light) {
    const float ndl = dot3(m->normal, toLight);
    if (ndl <= 0 || strength <= 0) return;
    const float through = lightThrough(s, p, toLight, limit);
    if (through <= 0) return;
    float highlight[3] = {0, 0, 0};
    if (m->shiny) {
        const ArtfShade3 n = {m->normal[0], m->normal[1], m->normal[2]}, v = {view[0], view[1], view[2]};
        const ArtfShade3 half = artfShadeHalfway(v, (ArtfShade3){toLight[0], toLight[1], toLight[2]});
        float vdh;
        const float lit = artfShadeHighlight(&m->lobe, false, n, v, half, ndl, m->material->specular, &vdh), far = artfShadeFifthInRow(vdh);
        for (int k = 0; k < 3; ++k) highlight[k] = artfShadeSchlick(m->f0[k], far) * m->gain[k] * lit;
    }
    for (int k = 0; k < 3; ++k) light[k] += path[k] * color[k] * strength * through * (m->albedo[k] * ndl + highlight[k]);
}

/* The same with what lies past glTF's core (Surface.flags), and for a light whose diffuse light is this share of
 * its highlight's (a rectangle's, to the side): the coat's lobe, sheen (artfShadeCharlie), highlights drawn out, and
 * light under the surface, as addExtras in kernels.inc. What comes through from behind is found by a ray inside, toward
 * the light: how far it goes in there, and then what stands in the way. */
static void lightFromMore(const ArtfScene* s, const float* p, const float* view, const Surface* m, const float* toLight, float limit, float strength, float share, const float* color, const float* path, float* light) {
    const float ndl = dot3(m->normal, toLight);
    const bool under = (m->flags & ARTF_MORE_SUBSURFACE) != 0;
    if ((ndl <= 0 && !under) || strength <= 0) return;
    float sum[3] = {0, 0, 0};
    const float lit = artfMaxf(ndl, 0);
    const float through = ndl > 0 ? lightThrough(s, p, toLight, limit) : 0.f;
    if (through > 0) {
        float highlight[3] = {0, 0, 0};
        if (m->shiny && ndl > 0) {
            const ArtfShade3 n = {m->normal[0], m->normal[1], m->normal[2]}, v = {view[0], view[1], view[2]};
            const ArtfShade3 half = artfShadeHalfway(v, (ArtfShade3){toLight[0], toLight[1], toLight[2]});
            float vdh;
            const float base = artfShadeHighlight(&m->lobe, m->lobe.drawn, n, v, half, ndl, m->material->specular, &vdh), far = artfShadeFifthInRow(vdh);
            for (int k = 0; k < 3; ++k) highlight[k] = artfShadeSchlick(m->f0[k], far) * m->gain[k] * base * m->under;
            if (m->coat > 0) {
                /* the coat's own lobe: of no color, F0 .04, by its own normal; its amount comes first here, in that
                 * order (D and Seen are shading.inc's) */
                const ArtfShade3 coatN = {m->coatNormal[0], m->coatNormal[1], m->coatNormal[2]};
                const float cndl = dot3(m->coatNormal, toLight), cndh = artfMaxf(artfShadeDot(coatN, half), 0);
                if (cndl > 0) {
                    const artfShadeLobe* c = &m->coatLobe;
                    const float dc = artfShadeGgxBelow(cndh, c->a2), seenC = artfShadeSeen(c->ndv, cndl, c->k);
                    const float coat = m->coat * c->a2 / (dc * dc) * cndl / (seenC * 4.f) * artfShadeCoatFresnel(far);
                    for (int k = 0; k < 3; ++k) highlight[k] += coat;
                }
            }
            if (m->flags & ARTF_MORE_SHEEN) {
                const float sheen = artfShadeCharlie(artfMaxf(artfShadeDot(n, half), 0), ndl, m->lobe.ndv, m->sheenInverse) * ndl;
                for (int k = 0; k < 3; ++k) highlight[k] += m->sheen[k] * sheen;
            }
        }
        const float wrap = under ? artfShadeWrap(ndl, lit) : 0.f;
        for (int k = 0; k < 3; ++k) sum[k] = through * ((m->albedo[k] * lit + m->subsurface[k] * wrap) * share + highlight[k]);
    }
    if (under && ndl < 0) {
        /* from just under the surface, toward the light: what is inside, then what is past it */
        float o[3], inside = 0;
        const float reach = reachOf(p);
        for (int k = 0; k < 3; ++k) o[k] = p[k] - m->normal[k] * reach * 3.f;
        Hit h;
        if (closest(&s->trace, o, toLight, limit, true, &h)) inside = h.t;
        float exit[3];
        for (int k = 0; k < 3; ++k) exit[k] = o[k] + toLight[k] * (inside + reach);
        const float behind = lightThrough(s, exit, toLight, limit - inside) * artfExp2Fast(-inside * m->reach) * -ndl;
        for (int k = 0; k < 3; ++k) sum[k] += m->subsurface[k] * behind * share;
    }
    for (int k = 0; k < 3; ++k) light[k] += path[k] * color[k] * strength * sum[k];
}

/* A light with a shape or a profile (artfLightSetShape, artfLightSetProfile): each path sees another point of the tube
 * or the rectangle, so its shadows go soft; a rectangle lights only the side it faces, and its diffuse light less to the
 * side; the profile goes by the way from its middle. ball: this path's point in a ball. */
static void shapedFrom(const ArtfScene* s, const ArtfFrameData* f, uint32_t i, uint32_t* state, const float* ball, const float* p, const float* view, const Surface* m, const float* path, float* light) {
    const ArtfLightData* L = &f->lights[i];
    const float(*shape)[4] = f->lightShapes[i];
    const float* u = shape[0];
    const float* facing = shape[1];
    const float size = f->lightSizes[i];
    float to[3];
    for (int k = 0; k < 3; ++k) to[k] = L->position[k] + ball[k] * size - p[k];
    if (shape[0][3] > 0) {
        const float along = (randomOf(state) * 2.f - 1.f) * shape[0][3];
        if (shape[1][3] > 0) {
            const float across = (randomOf(state) * 2.f - 1.f) * shape[1][3];
            const float side[3] = {facing[1] * u[2] - facing[2] * u[1], facing[2] * u[0] - facing[0] * u[2], facing[0] * u[1] - facing[1] * u[0]};
            for (int k = 0; k < 3; ++k) to[k] = L->position[k] + u[k] * along + side[k] * across - p[k];
        } else {
            for (int k = 0; k < 3; ++k) to[k] += u[k] * along;
        }
    }
    const float d2 = dot3(to, to), distance = sqrtf(artfMaxf(d2, 1e-8f));
    const float fade = artfMaxf(1.f - d2 / (L->radius * L->radius), 0);
    for (int k = 0; k < 3; ++k) to[k] /= distance;
    const float share = shape[1][3] > 0 ? -dot3(to, facing) : 1.f;
    if (share <= 0) return;
    /* (its cone and profile from its middle) */
    float middle[3] = {p[0] - L->position[0], p[1] - L->position[1], p[2] - L->position[2]};
    normalize3(middle);
    float strength = fade * fade * artfClampf((dot3(middle, L->direction) - L->spotCut) * L->spotScale, 0, 1);
    if (shape[2][3] > 0) strength *= artfProfileAt(s->lightProfile[(int)shape[2][3] - 1], shape[2], shape[3], middle);
    lightFromMore(s, p, view, m, to, distance, strength, share, L->color, path, light);
}

/* What bones did to where a ray landed, from the object to the world: the inverse of what takes it back. */
static void shiftOf(const ArtfScene* s, const Hit* h, float* out) {
    const ArtfTrace* t = &s->trace;
    const ArtfTraceTriangle* tri = &t->triangles.items[h->triangle];
    const ArtfTraceVertex *a = &t->vertices.items[tri->v[0]], *b = &t->vertices.items[tri->v[1]], *c = &t->vertices.items[tri->v[2]];
    const float wa = 1.f - h->u - h->v, wb = h->u, wc = h->v;
    const float d[3] = {a->back[0] * wa + b->back[0] * wb + c->back[0] * wc, a->back[1] * wa + b->back[1] * wb + c->back[1] * wc, a->back[2] * wa + b->back[2] * wb + c->back[2] * wc};
    const float* k = t->instances.items[h->instance].toObject;
    const float* r0 = k;
    const float* r1 = k + 4;
    const float* r2 = k + 8;
    const float c12[3] = {r1[1] * r2[2] - r1[2] * r2[1], r1[2] * r2[0] - r1[0] * r2[2], r1[0] * r2[1] - r1[1] * r2[0]};
    const float c20[3] = {r2[1] * r0[2] - r2[2] * r0[1], r2[2] * r0[0] - r2[0] * r0[2], r2[0] * r0[1] - r2[1] * r0[0]};
    const float c01[3] = {r0[1] * r1[2] - r0[2] * r1[1], r0[2] * r1[0] - r0[0] * r1[2], r0[0] * r1[1] - r0[1] * r1[0]};
    const float det = r0[0] * c12[0] + r0[1] * c12[1] + r0[2] * c12[2], inverse = det != 0 ? 1.f / det : 0.f;
    for (int i = 0; i < 3; ++i) out[i] = (c12[i] * d[0] + c20[i] * d[1] + c01[i] * d[2]) * inverse;
    out[3] = 0;
}

void artfTraceSample(const ArtfScene* s, const ArtfFrameData* f, const ArtfTraceFrame* c, uint32_t x, uint32_t y, uint32_t sample, float* out, ArtfTraceGuide* guide) {
    /* where in the pixel, which way a bounce goes and which lobe: Sobol's points, this pixel's own scrambling of them
     * (artfSobol4); what goes through, what ends and points on shaped lights: a hash */
    const uint32_t pixel = x + y * c->width;
    uint32_t state = hashOf(hashOf(pixel) + sample);
    float jitter[4];
    artfSobol4(sample, artfPathSeed(pixel, 0), jitter);
    const float px = ((float)x + jitter[0]) / (float)c->width * 2.f - 1.f, py = 1.f - ((float)y + jitter[1]) / (float)c->height * 2.f;
    float o[3], d[3], path[3] = {1, 1, 1}, light[3] = {0, 0, 0}, travelled = 0, keep = 1.f;
    if (c->eye[3] > 0)
        for (int k = 0; k < 3; ++k) {
            o[k] = c->eye[k] + c->right[k] * px + c->up[k] * py;
            d[k] = c->forward[k];
        }
    else
        for (int k = 0; k < 3; ++k) {
            o[k] = c->eye[k];
            d[k] = c->forward[k] + c->right[k] * px + c->up[k] * py;
        }
    normalize3(d);
    bool covered = false;
    uint32_t bounces = 0;
    /* the ray's cone (texelCone): as wide as a pixel at the eye's distance, and widening by a pixel's angle; an
     * orthographic view's, a pixel wide all along. A bounce widens it: by its lobe's width, or wholly when diffuse. */
    const float pixelSize = 2.f * sqrtf(c->up[0] * c->up[0] + c->up[1] * c->up[1] + c->up[2] * c->up[2]) / (float)c->height;
    float coneWidth = !artfTraceCones ? 0.f : c->eye[3] > 0 ? pixelSize : 0.f, coneSpread = !artfTraceCones ? 0.f : c->eye[3] > 0 ? 0.f : pixelSize;
    /* Paths aim at the picture of all around as well as bouncing into it (artfEnvAim): each way's light is weighed by
     * how likely each way was to find it (MIS, the power heuristic), lobe by lobe: what a surface gives back diffusely
     * against its diffuse bounce, its highlight against its highlight's. bounceLikely: how likely the last bounce's
     * direction was, by the lobe it took; 0 for the eye's ray, whose light is not weighed. */
    const float* aimed = artfTraceAim && c->paths && f->sky[3] > 0 && s->environment ? s->environment : NULL;
    float bounceLikely = 0;
    for (int step = 0; step < MAX_STEPS; ++step) {
        Hit h;
        if (!closest(&s->trace, o, d, 3e37f, false, &h)) {
            /* the sky, for what has met something: what has not leaves the frame's own background */
            if (covered && f->sky[3] > 0 && s->environment) {
                float around[3];
                artfEnvSample(s->environment, 0, d[0], d[1], d[2], around);
                float weight = 1.f;
                if (aimed && bounceLikely > 0) {
                    const float other = artfEnvAimDensity(aimed, d);
                    weight = bounceLikely * bounceLikely / (bounceLikely * bounceLikely + other * other);
                }
                for (int k = 0; k < 3; ++k) light[k] += path[k] * around[k] * f->sky[3] * weight;
            } else if (covered) {
                const float up = d[1] * .5f + .5f;
                for (int k = 0; k < 3; ++k) light[k] += path[k] * artfShadeSkyGround(f->ground[k], f->sky[k] - f->ground[k], up);
            }
            break;
        }
        Surface m;
        coneWidth += coneSpread * h.t;
        surfaceAt(s, &h, o, d, coneWidth, &m);
        float p[3];
        for (int k = 0; k < 3; ++k) p[k] = o[k] + d[k] * h.t;
        const float reach = reachOf(p);
        travelled += h.t;
        /* (what came from inside a thing that takes some light as it goes: KHR_materials_volume) */
        if (!m.front && m.flags & ARTF_MORE_VOLUME)
            for (int k = 0; k < 3; ++k) path[k] *= artfExp2Fast(-m.absorb[k] * h.t);
        if (randomOf(&state) >= m.alpha) {
            /* it goes through: for a path as much of the time as the surface is not there, bent if the
             * surface bends it, or turned back when it comes out too flat */
            if (m.material->ior > 0) {
                const float eta = m.front ? 1.f / m.material->ior : m.material->ior, cosine = -dot3(d, m.normal);
                const float k2 = 1.f - eta * eta * (1.f - cosine * cosine);
                const float along = k2 < 0 ? 2.f * cosine : eta * cosine - sqrtf(artfMaxf(k2, 0)), onward = k2 < 0 ? 1.f : eta;
                for (int k = 0; k < 3; ++k) d[k] = d[k] * onward + m.normal[k] * along;
                normalize3(d);
            }
            for (int k = 0; k < 3; ++k) o[k] = p[k] + d[k] * reach;
            continue;
        }
        if (!covered) {
            if (guide) {
                const uint32_t bits = artfNormalPack(m.normal);
                memcpy(guide->place, p, 12);
                memcpy(&guide->place[3], &bits, 4);
                /* (what a metal shows is what it reflects: that is its color here) */
                for (int k = 0; k < 3; ++k) guide->albedo[k] = artfMaxf(m.albedo[k], m.f0[k]);
                guide->albedo[3] = (float)(h.instance + 1);
                shiftOf(s, &h, guide->shift);
            }
            /* (as the rasterizer's fog: what the first surface shows goes through this much air) */
            const float air = travelled * f->look[1];
            keep = 1.f / (1.f + air + .5f * air * air);
        }
        covered = true;
        const float view[3] = {-d[0], -d[1], -d[2]};
        float lifted[3];
        for (int k = 0; k < 3; ++k) {
            lifted[k] = p[k] + m.normal[k] * reach;
            light[k] += path[k] * m.emissive[k];
        }
        float ball[3], u[4];
        artfSobol4(sample, artfPathSeed(pixel, bounces + 1), u);
        ballOf(u, ball);
        if (f->sunColor[0] > 0 || f->sunColor[1] > 0 || f->sunColor[2] > 0) {
            /* (a sun with a size: each path sees another point of it, and shadows go soft with distance) */
            float toSun[3];
            for (int k = 0; k < 3; ++k) toSun[k] = f->toSun[k] + ball[k] * f->look[2];
            normalize3(toSun);
            if (m.flags & ARTF_MORE_DRAWN) lightFromMore(s, lifted, view, &m, toSun, 3e37f, 1.f, 1.f, f->sunColor, path, light);
            else lightFrom(s, lifted, view, &m, toSun, 3e37f, 1.f, f->sunColor, path, light);
        }
        for (uint32_t i = 0; i < f->lightCount; ++i) {
            const ArtfLightData* L = &f->lights[i];
            if (artfLightShaped(f, i)) {
                shapedFrom(s, f, i, &state, ball, lifted, view, &m, path, light);
                continue;
            }
            /* (a light with a size: each path sees another point of it, so its shadows go soft) */
            const float size = f->lightSizes[i];
            float to[3] = {L->position[0] + ball[0] * size - lifted[0], L->position[1] + ball[1] * size - lifted[1], L->position[2] + ball[2] * size - lifted[2]};
            const float d2 = dot3(to, to), distance = sqrtf(artfMaxf(d2, 1e-8f));
            const float fade = artfMaxf(1.f - d2 / (L->radius * L->radius), 0);
            for (int k = 0; k < 3; ++k) to[k] /= distance;
            const float spot = artfClampf((-dot3(to, L->direction) - L->spotCut) * L->spotScale, 0, 1);
            if (m.flags & ARTF_MORE_DRAWN) lightFromMore(s, lifted, view, &m, to, distance, fade * fade * spot, 1.f, L->color, path, light);
            else lightFrom(s, lifted, view, &m, to, distance, fade * fade * spot, L->color, path, light);
        }
        const float dn = dot3(d, m.normal);
        float mirror[3];
        for (int k = 0; k < 3; ++k) mirror[k] = d[k] - 2.f * dn * m.normal[k];
        /* what a highlight reflects: around a visible normal of GGX's of its alpha (glossyOf). It reflects m.mirror of
         * all around, less what its microfacets hide of each other */
        const float alpha = artfMaxf(m.rough * m.rough, 1e-3f), nv = artfMaxf(dot3(m.normal, view), 1e-4f);
        float glossy[3], kept, likelyShine;
        if (!c->paths) {
            /* rays: light from all around as the rasterizer has it, and a mirror for what reflects */
            const float* env = f->sky[3] > 0 ? s->environment : NULL;
            const float up = m.normal[1] * .5f + .5f;
            float around[3];
            if (env) artfEnvSample(env, ARTF_ENV_DIFFUSE, m.normal[0], m.normal[1], m.normal[2], around);
            for (int k = 0; k < 3; ++k) light[k] += path[k] * m.albedo[k] * m.open * (env ? around[k] * f->sky[3] : artfShadeSkyGround(f->ground[k], f->sky[k] - f->ground[k], up));
            if (!m.shiny) break;
            if (bounces >= c->bounces) {
                const float reflectedUp = mirror[1] * .5f + .5f;
                if (env) artfEnvReflected(env, m.rough, mirror[0], mirror[1], mirror[2], around);
                for (int k = 0; k < 3; ++k) light[k] += path[k] * m.mirror[k] * m.open * (env ? around[k] * f->sky[3] : artfShadeSkyGround(f->ground[k], f->sky[k] - f->ground[k], reflectedUp));
                break;
            }
            glossyOf(&m, d, view, alpha, nv, u, 1.f, glossy, &kept, &likelyShine);
            for (int k = 0; k < 3; ++k) {
                path[k] *= m.mirror[k] * kept;
                d[k] = glossy[k];
            }
            coneSpread += 2.f * alpha;
        } else {
            if (bounces >= c->bounces) break;
            const float most = artfMaxf(m.mirror[0], artfMaxf(m.mirror[1], m.mirror[2]));
            const float chance = m.shiny ? artfClampf(most, .05f, .95f) : 0.f;
            if (aimed) {
                /* aimed at the picture of all around, for what the surface gives back diffusely: albedo, what of it is
                 * open and what is not reflected, over pi, times the cosine */
                float e[4], toward[3], around[3];
                artfSobol4(sample, artfPathSeed(pixel, 64u + bounces + 1u), e);
                const float likely = artfEnvAim(aimed, e[0], e[1], toward), cosine = dot3(m.normal, toward);
                if (likely > 0 && cosine > 0) {
                    const float other = (1.f - chance) * cosine * (1.f / 3.14159265f);
                    const float through = lightThrough(s, lifted, toward, 3e37f);
                    if (through > 0) {
                        artfEnvSample(aimed, 0, toward[0], toward[1], toward[2], around);
                        const float weight = likely * likely / (likely * likely + other * other);
                        const float share = m.open * (1.f - most) * (1.f / 3.14159265f) * cosine * through * (weight / likely) * f->sky[3];
                        /* and the highlight: GGX's, D G2 / (4 n.v) of m.mirror, against its own bounce's likelihood */
                        float highlight = 0;
                        if (chance > 0) {
                            float h[3] = {view[0] + toward[0], view[1] + toward[1], view[2] + toward[2]};
                            normalize3(h);
                            const float facing = ggxD(alpha, artfMaxf(dot3(m.normal, h), 0.f));
                            const float lambdaView = ggxLambda(alpha, nv);
                            const float otherShine = chance * facing / ((1.f + lambdaView) * 4.f * nv);
                            const float shineWeight = likely * likely / (likely * likely + otherShine * otherShine);
                            highlight = facing / (4.f * nv * (1.f + lambdaView + ggxLambda(alpha, cosine))) * through * (shineWeight / likely) * f->sky[3];
                        }
                        for (int k = 0; k < 3; ++k) light[k] += path[k] * (m.albedo[k] * share + m.mirror[k] * highlight) * around[k];
                    }
                }
            }
            if (u[3] < chance) {
                glossyOf(&m, d, view, alpha, nv, u, chance, glossy, &kept, &likelyShine);
                for (int k = 0; k < 3; ++k) {
                    d[k] = glossy[k];
                    path[k] *= m.mirror[k] * kept / chance;
                }
                bounceLikely = likelyShine;
                coneSpread += 2.f * alpha;
            } else {
                /* a point on a sphere over the surface: as many rays as the cosine says, exactly */
                const float keep = m.open * (1.f - most) / (1.f - chance);
                float unit[3];
                directionOf(u, unit);
                for (int k = 0; k < 3; ++k) {
                    d[k] = m.normal[k] * 1.0001f + unit[k];
                    path[k] *= m.albedo[k] * keep;
                }
                normalize3(d);
                bounceLikely = (1.f - chance) * artfMaxf(dot3(d, m.normal), 0.f) * (1.f / 3.14159265f);
                coneSpread += 1.f;
            }
        }
        normalize3(d);
        if (dot3(d, m.normal) <= 0) break;
        ++bounces;
        for (int k = 0; k < 3; ++k) o[k] = lifted[k];
        /* past a few bounces, what brings little ends now and then, and what goes on makes up for it */
        if (bounces > 3) {
            const float keep = artfClampf(artfMaxf(path[0], artfMaxf(path[1], path[2])), .05f, 1.f);
            if (randomOf(&state) >= keep) break;
            const float scale = 1.f / keep;
            for (int k = 0; k < 3; ++k) path[k] *= scale;
        }
    }
    for (int k = 0; k < 3; ++k) {
        const float seen = f->fog[k] + (artfMinf(light[k], LIGHT_LIMIT) - f->fog[k]) * keep;
        out[k] += covered ? seen : 0.f;
    }
    out[3] += covered ? 1.f : 0.f;
}

/* ---- rays while drawing: shadows and reflections (ArtfShadowDesc.traced, ArtfPostDesc.screen.tracedReflections) ----
 *
 * The rasterizer's pixel asks the trees what is toward a light, or along its mirror: the same arithmetic as the GPU's
 * (tracedShadowSpv and artfShaderPostReflectionTraced in shaders.c), so that both draw the same. */

/* (four by four; the radii those of even shells of the ball, each four through all of it) */
const float artfTracedBall[16][3] = {
    {.1819f, .1819f, .1819f},   {.3782f, -.3782f, -.3782f}, {-.4677f, .4677f, -.4677f}, {-.5317f, -.5317f, .5317f},
    {-.2621f, -.2621f, -.2621f}, {-.4047f, .4047f, .4047f}, {.4850f, -.4850f, .4850f},   {.5456f, .5456f, -.5456f},
    {.4401f, 0, -.3112f},        {-.6050f, 0, -.4279f},     {0, .7095f, .5018f},          {0, -.7904f, .5589f},
    {-.4923f, 0, .3482f},        {.6344f, 0, .4486f},       {0, -.7316f, -.5174f},        {0, .8083f, -.5716f}};

uint32_t artfTracedPlace(const ArtfFrameData* f, const float* p) {
    const float* m = f->viewProj;
    const float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    const float cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    const float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    const float inv = 1.f / artfMaxf(cw, 1e-6f);
    const float x = artfClampf((cx * inv * .5f + .5f) * f->eye[0][3], -1e6f, 1e6f), y = artfClampf((.5f - cy * inv * .5f) * f->eye[1][3], -1e6f, 1e6f);
    const uint32_t ix = (uint32_t)(int32_t)floorf(x), iy = (uint32_t)(int32_t)floorf(y);
    return ((ix & 1u) + (iy & 1u) * 2u + f->tracedFrame) & 3u;
}

/* Off the surface, along its normal: by more with distance, as what is drawn far away may be a lighter level than the
 * trees hold. */
static void tracedLift(const ArtfFrameData* f, const float* p, const float* n, float* out) {
    const float e[3] = {p[0] - f->cameraPos[0], p[1] - f->cameraPos[1], p[2] - f->cameraPos[2]};
    const float lift = reachOf(p) + sqrtf(dot3(e, e)) * 1e-3f;
    for (int k = 0; k < 3; ++k) out[k] = p[k] + n[k] * lift;
}

float artfTracedShadow(const ArtfScene* s, const ArtfFrameData* f, const float* p, const float* n, int light) {
    float o[3];
    tracedLift(f, p, n, o);
    const float size = light < 0 ? f->look[2] : f->lightSizes[light];
    const uint32_t rays = size > 0 ? artfMaxi((int)f->tracedRays, 1) : 1u, first = artfTracedPlace(f, p) * 4u;
    float sum = 0;
    for (uint32_t k = 0; k < rays; ++k) {
        const float* b = artfTracedBall[(first + k) & 15u];
        float to[3], limit = 3e37f;
        if (light < 0) {
            for (int i = 0; i < 3; ++i) to[i] = f->toSun[i] + b[i] * size;
            normalize3(to);
        } else {
            const float* at = f->lights[light].position;
            for (int i = 0; i < 3; ++i) to[i] = at[i] + b[i] * size - o[i];
            limit = sqrtf(artfMaxf(dot3(to, to), 1e-8f));
            const float inv = 1.f / limit;
            for (int i = 0; i < 3; ++i) to[i] *= inv;
        }
        sum += lightThrough(s, o, to, limit);
    }
    return sum / (float)rays;
}

bool artfTracedReflection(const ArtfScene* s, const ArtfFrameData* f, const float* p, const float* n, const float* v, float rough, uint32_t point, float* out) {
    float o[3], d[3], mirror[3];
    tracedLift(f, p, n, o);
    const float vn = dot3(v, n), stray = rough * rough;
    const float* b = artfTracedBall[point & 15u];
    for (int k = 0; k < 3; ++k) {
        mirror[k] = v[k] - 2.f * vn * n[k];
        d[k] = mirror[k] + b[k] * stray;
    }
    normalize3(d);
    /* (not into the surface) */
    if (dot3(d, n) <= 0) memcpy(d, mirror, sizeof d);
    for (int step = 0; step < 4; ++step) {
        Hit h;
        if (!closest(&s->trace, o, d, 3e37f, false, &h)) return false;
        Surface m;
        surfaceAt(s, &h, o, d, 0.f, &m);
        float q[3];
        for (int k = 0; k < 3; ++k) q[k] = o[k] + d[k] * h.t;
        if (m.alpha < .5f) {
            /* (what is mostly not there is gone through) */
            const float reach = reachOf(q);
            for (int k = 0; k < 3; ++k) o[k] = q[k] + d[k] * reach;
            continue;
        }
        /* its color under light from all around, the sun and the frame's lights, with no shadows; and its own light */
        float lit[3];
        if (f->sky[3] > 0 && s->environment) {
            artfEnvSample(s->environment, ARTF_ENV_DIFFUSE, m.normal[0], m.normal[1], m.normal[2], lit);
            for (int k = 0; k < 3; ++k) lit[k] *= f->sky[3] * m.open;
        } else {
            const float up = m.normal[1] * .5f + .5f;
            for (int k = 0; k < 3; ++k) lit[k] = artfShadeSkyGround(f->ground[k], f->sky[k] - f->ground[k], up) * m.open;
        }
        const float sun = artfMaxf(dot3(m.normal, f->toSun), 0);
        for (int k = 0; k < 3; ++k) lit[k] += f->sunColor[k] * sun;
        for (uint32_t i = 0; i < f->lightCount; ++i) {
            const ArtfLightData* L = &f->lights[i];
            float to[3] = {L->position[0] - q[0], L->position[1] - q[1], L->position[2] - q[2]};
            const float d2 = dot3(to, to), inv = 1.f / sqrtf(artfMaxf(d2, 1e-8f));
            for (int k = 0; k < 3; ++k) to[k] *= inv;
            const float fade = artfMaxf(1.f - d2 / (L->radius * L->radius), 0);
            const float spot = artfClampf((-dot3(to, L->direction) - L->spotCut) * L->spotScale, 0, 1);
            const float amount = artfMaxf(dot3(m.normal, to), 0) * fade * fade * spot;
            for (int k = 0; k < 3; ++k) lit[k] += L->color[k] * amount;
        }
        for (int k = 0; k < 3; ++k) out[k] = m.emissive[k] + (m.albedo[k] + m.f0[k]) * lit[k];
        return true;
    }
    return false;
}

/* ---- light that bounced: probes ----
 *
 * Each step each probe sends rays the same random ways (from the step's number: the same on every processor), and
 * what comes back along each, from the sky, a light, or a surface lit directly and by the probes before, makes a
 * first-order spherical harmonic: the mean, and how it leans. What a surface facing n takes from all around is, over
 * pi, the mean plus 2/3 of its lean along n (Ramamoorthi and Hanrahan), and the probes keep that directly. */

/* How far a probe sees toward dir (of any length): the mean and the mean of the square, from its map. */
static inline void probeReach(const float* map, const float* dir, float* mean, float* square) {
    /* (the nearest texel: the probes' own weights blend what is between them) */
    float u, v;
    artfProbeFold(dir, &u, &v);
    const int x = artfMini((int)(u * ARTF_PROBE_SIDE), ARTF_PROBE_SIDE - 1), y = artfMini((int)(v * ARTF_PROBE_SIDE), ARTF_PROBE_SIDE - 1);
    const float* texel = map + (y * ARTF_PROBE_SIDE + x) * 2;
    *mean = texel[0];
    *square = texel[1];
}

void artfIndirectAt(const ArtfIndirect* g, const float* p, const float* n, float* out) {
    float at[3], w[3], q[3];
    int i0[3];
    for (int k = 0; k < 3; ++k) {
        /* (a little out along the normal: a probe on the surface's own side counts more) */
        q[k] = p[k] + n[k] * g->spacing[k] * .25f;
        at[k] = artfClampf((q[k] - g->origin[k]) / g->spacing[k], 0, (float)(g->dims[k] - 1));
        i0[k] = artfMini((int)at[k], g->dims[k] - 2);
        w[k] = at[k] - (float)i0[k];
    }
    float sum[3] = {0, 0, 0}, weights = 0;
    for (int c = 0; c < 8; ++c) {
        const int x = i0[0] + (c & 1), y = i0[1] + (c >> 1 & 1), z = i0[2] + (c >> 2);
        float weight = (c & 1 ? w[0] : 1.f - w[0]) * (c & 2 ? w[1] : 1.f - w[1]) * (c & 4 ? w[2] : 1.f - w[2]);
        const size_t index = (size_t)(z * g->dims[1] + y) * g->dims[0] + x;
        const float* probe = g->probes + index * 16;
        weight *= probe[3]; /* (none from one inside something) */
        if (g->desc.throughWalls) {
            weights += weight;
            for (int k = 0; k < 3; ++k) sum[k] += weight * artfMaxf(probe[k] + probe[4 + 4 * k] * n[0] + probe[5 + 4 * k] * n[1] + probe[6 + 4 * k] * n[2], 0);
            continue;
        }
        /* from the probe to the point: one behind the surface counts less, and one that cannot see it hardly at all */
        const float there[3] = {g->origin[0] + (float)x * g->spacing[0], g->origin[1] + (float)y * g->spacing[1], g->origin[2] + (float)z * g->spacing[2]};
        const float dir[3] = {q[0] - there[0], q[1] - there[1], q[2] - there[2]};
        const float distance = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        const float inv = distance > 1e-6f ? 1.f / distance : 0.f;
        const float facing = (1.f - (dir[0] * n[0] + dir[1] * n[1] + dir[2] * n[2]) * inv) * .5f;
        weight *= facing * facing + .2f;
        float mean, square;
        probeReach(g->probes + (size_t)artfIndirectCount(g) * 16 + index * ARTF_PROBE_MAP, dir, &mean, &square);
        if (distance > mean) {
            const float variance = artfMaxf(square - mean * mean, 1e-4f), past = distance - mean;
            const float seen = variance / (variance + past * past);
            weight *= artfMaxf(seen * seen * seen, .002f);
        }
        weights += weight;
        for (int k = 0; k < 3; ++k) sum[k] += weight * artfMaxf(probe[k] + probe[4 + 4 * k] * n[0] + probe[5 + 4 * k] * n[1] + probe[6 + 4 * k] * n[2], 0);
    }
    const float scale = weights > 1e-6f ? 1.f / weights : 0.f;
    for (int k = 0; k < 3; ++k) out[k] = sum[k] * scale;
}

typedef struct ProbeJob { ArtfScene* s; const ArtfFrameData* f; } ProbeJob;

static void probeJob(void* user, uint32_t index, uint32_t worker) {
    (void)worker;
    const ProbeJob* j = user;
    ArtfScene* s = j->s;
    const ArtfFrameData* f = j->f;
    ArtfIndirect* g = &s->indirect;
    const int x = (int)(index % (uint32_t)g->dims[0]), y = (int)(index / (uint32_t)g->dims[0] % (uint32_t)g->dims[1]), z = (int)(index / (uint32_t)(g->dims[0] * g->dims[1]));
    const float o[3] = {g->origin[0] + (float)x * g->spacing[0], g->origin[1] + (float)y * g->spacing[1], g->origin[2] + (float)z * g->spacing[2]};
    const float* wasProbe = g->probes + (size_t)index * 16;
    /* (one inside something, on the steps it is not followed: none, and what it had, as it was) */
    const bool resting = g->frames && wasProbe[3] == 0 && (g->frames + index) % ARTF_PROBE_INSIDE_EVERY != 0;
    const int rays = resting ? 0 : g->desc.rays > 0 ? g->desc.rays : 64;
    int backs = 0;
    const float* env = f->sky[3] > 0 ? s->environment : NULL;
    uint32_t state = hashOf(index * 9781u + g->frames * 6271u + 1u);
    float mean[3] = {0, 0, 0}, lean[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    /* how far it sees, toward each of its map's directions: the mean and the mean of the square, of the rays near each */
    enum { TEXELS = ARTF_PROBE_SIDE * ARTF_PROBE_SIDE };
    float reach[TEXELS], reach2[TEXELS], reachWeight[TEXELS], toward[TEXELS][3];
    for (int k = 0; k < TEXELS; ++k) {
        /* (the direction at the texel's middle, unfolded) */
        float a = ((float)(k % ARTF_PROBE_SIDE) + .5f) / ARTF_PROBE_SIDE * 2.f - 1.f, b = ((float)(k / ARTF_PROBE_SIDE) + .5f) / ARTF_PROBE_SIDE * 2.f - 1.f;
        const float z = 1.f - fabsf(a) - fabsf(b);
        if (z < 0) {
            const float fa = (1.f - fabsf(b)) * (a >= 0 ? 1.f : -1.f);
            b = (1.f - fabsf(a)) * (b >= 0 ? 1.f : -1.f);
            a = fa;
        }
        const float length = sqrtf(a * a + b * b + z * z);
        toward[k][0] = a / length;
        toward[k][1] = b / length;
        toward[k][2] = z / length;
        reach[k] = reach2[k] = reachWeight[k] = 0;
    }
    const float most = sqrtf(g->spacing[0] * g->spacing[0] + g->spacing[1] * g->spacing[1] + g->spacing[2] * g->spacing[2]) * 1.5f;
    const float one[3] = {1, 1, 1};
    for (int r = 0; r < rays; ++r) {
        float d[3];
        randomInBall(&state, d);
        normalize3(d);
        float light[3] = {0, 0, 0};
        Hit h;
        const bool hit = closest(&s->trace, o, d, 3e37f, true, &h);
        {
            const float far = hit ? artfMinf(h.t, most) : most;
            for (int k = 0; k < TEXELS; ++k) {
                /* (a narrow lobe each: the cosine to the 32nd) */
                float w = artfMaxf(dot3(d, toward[k]), 0);
                w *= w;
                w *= w;
                w *= w;
                w *= w;
                w *= w;
                reach[k] += w * far;
                reach2[k] += w * far * far;
                reachWeight[k] += w;
            }
        }
        if (!hit) {
            if (env) {
                artfEnvSample(env, ARTF_ENV_DIFFUSE - 1, d[0], d[1], d[2], light);
                for (int k = 0; k < 3; ++k) light[k] *= f->sky[3];
            } else {
                const float up = d[1] * .5f + .5f;
                for (int k = 0; k < 3; ++k) light[k] = artfShadeSkyGround(f->ground[k], f->sky[k] - f->ground[k], up);
            }
        } else {
            Surface m;
            surfaceAt(s, &h, o, d, 0.f, &m);
            /* (a probe inside something sees its backs: it takes nothing from them) */
            backs += !m.front;
            if (m.front) {
                float p[3], lifted[3];
                const float view[3] = {-d[0], -d[1], -d[2]};
                for (int k = 0; k < 3; ++k) p[k] = o[k] + d[k] * h.t;
                const float reach = reachOf(p);
                for (int k = 0; k < 3; ++k) lifted[k] = p[k] + m.normal[k] * reach;
                for (int k = 0; k < 3; ++k) light[k] = m.emissive[k];
                if (f->sunColor[0] > 0 || f->sunColor[1] > 0 || f->sunColor[2] > 0) lightFrom(s, lifted, view, &m, f->toSun, 3e37f, 1.f, f->sunColor, one, light);
                for (uint32_t i = 0; i < f->lightCount; ++i) {
                    const ArtfLightData* L = &f->lights[i];
                    float to[3] = {L->position[0] - lifted[0], L->position[1] - lifted[1], L->position[2] - lifted[2]};
                    const float d2 = dot3(to, to), distance = sqrtf(artfMaxf(d2, 1e-8f));
                    const float fade = artfMaxf(1.f - d2 / (L->radius * L->radius), 0);
                    for (int k = 0; k < 3; ++k) to[k] /= distance;
                    const float spot = artfClampf((-dot3(to, L->direction) - L->spotCut) * L->spotScale, 0, 1);
                    lightFrom(s, lifted, view, &m, to, distance, fade * fade * spot, L->color, one, light);
                }
                /* what bounced to it before: the probes, as the last step left them */
                float around[3];
                artfIndirectAt(g, p, m.normal, around);
                for (int k = 0; k < 3; ++k) light[k] += m.albedo[k] * around[k];
            }
        }
        for (int k = 0; k < 3; ++k) {
            light[k] = artfMinf(light[k], LIGHT_LIMIT);
            mean[k] += light[k];
            for (int a = 0; a < 3; ++a) lean[k][a] += light[k] * d[a];
        }
    }
    float* out = g->next + (size_t)index * 16;
    const float* was = g->probes + (size_t)index * 16;
    float* outMap = g->next + (size_t)artfIndirectCount(g) * 16 + (size_t)index * ARTF_PROBE_MAP;
    const float* wasMap = g->probes + (size_t)artfIndirectCount(g) * 16 + (size_t)index * ARTF_PROBE_MAP;
    const float mapKeep = resting ? 1.f : g->frames ? (g->desc.hysteresis > 0 ? artfClampf(g->desc.hysteresis, 0, .999f) : .9f) : 0.f;
    const float each = resting ? 0.f : 1.f / (float)rays;
    /* (the light it had keeps less when what came is far from it: artfProbeKeep) */
    const float keep = resting || !g->frames ? mapKeep : artfProbeKeep(mapKeep, (.2126f * mean[0] + .7152f * mean[1] + .0722f * mean[2]) * each, .2126f * was[0] + .7152f * was[1] + .0722f * was[2]);
    for (int k = 0; k < 3; ++k) {
        out[k] = was[k] * keep + mean[k] * each * (1.f - keep);
        for (int a = 0; a < 3; ++a) out[4 + 4 * k + a] = was[4 + 4 * k + a] * keep + 2.f * lean[k][a] * each * (1.f - keep);
        out[7 + 4 * k] = 0;
    }
    out[3] = resting || backs * 4 > rays ? 0.f : 1.f;
    for (int k = 0; k < TEXELS; ++k) {
        /* (a direction no ray went near: as far as a probe sees) */
        const float m = reachWeight[k] > 1e-6f ? reach[k] / reachWeight[k] : most, m2 = reachWeight[k] > 1e-6f ? reach2[k] / reachWeight[k] : most * most;
        outMap[k * 2] = g->frames ? wasMap[k * 2] * mapKeep + m * (1.f - mapKeep) : m;
        outMap[k * 2 + 1] = g->frames ? wasMap[k * 2 + 1] * mapKeep + m2 * (1.f - mapKeep) : m2;
    }
}

void artfSceneIndirectStep(ArtfScene* s, const ArtfFrameData* frame, ArtfPool* pool) {
    ArtfIndirect* g = &s->indirect;
    if (!g->on || !g->probes) return;
    artfSceneTraceSync(s);
    ProbeJob j = {s, frame};
    if (pool) artfPoolRun(pool, probeJob, &j, artfIndirectCount(g));
    else
        for (uint32_t i = 0; i < artfIndirectCount(g); ++i) probeJob(&j, i, 0);
    float* t = g->probes;
    g->probes = g->next;
    g->next = t;
    ++g->frames;
    ++g->version;
}

/* ---- picking ---- */

/* The nearest surface along a ray that is at least half there: what holes leave out, what has faded or shows through
 * more than it shows, and particles, are gone through. d of length 1. */
static bool pickAlong(ArtfScene* s, const float* origin, const float* d, float limit, ArtfPickHit* out) {
    traceSync(s, true);
    const ArtfTrace* t = &s->trace;
    float o[3] = {origin[0], origin[1], origin[2]}, travelled = 0;
    for (int step = 0; step < MAX_STEPS; ++step) {
        Hit h;
        if (!closest(t, o, d, limit - travelled, false, &h)) return false;
        float p[3];
        for (int k = 0; k < 3; ++k) p[k] = o[k] + d[k] * h.t;
        const uint32_t handle = t->instanceHandles.items[h.instance];
        bool there = handle != 0;
        if (there) {
            Surface m;
            surfaceAt(s, &h, o, d, 0.f, &m);
            there = m.alpha >= .5f;
        }
        if (there) {
            const ArtfTraceTriangle* tri = &t->triangles.items[h.triangle];
            const ArtfTraceVertex *a = &t->vertices.items[tri->v[0]], *b = &t->vertices.items[tri->v[1]], *c = &t->vertices.items[tri->v[2]];
            const float wa = 1.f - h.u - h.v;
            /* the triangle's own normal, into the world by the inverse's transpose, toward where the ray came from */
            const float e1[3] = {b->p[0] - a->p[0], b->p[1] - a->p[1], b->p[2] - a->p[2]}, e2[3] = {c->p[0] - a->p[0], c->p[1] - a->p[1], c->p[2] - a->p[2]};
            const float on[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float* k = t->instances.items[h.instance].toObject;
            const float n[3] = {k[0] * on[0] + k[4] * on[1] + k[8] * on[2], k[1] * on[0] + k[5] * on[1] + k[9] * on[2], k[2] * on[0] + k[6] * on[1] + k[10] * on[2]};
            const float length = sqrtf(artfMaxf(dot3(n, n), 1e-30f)), side = dot3(n, d) > 0 ? -1.f / length : 1.f / length;
            memset(out, 0, sizeof *out);
            out->instance = handle;
            out->triangle = tri->pad;
            out->position = artfVec3(p[0], p[1], p[2]);
            out->normal = artfVec3(n[0] * side, n[1] * side, n[2] * side);
            out->distance = travelled + h.t;
            out->u = a->u * wa + b->u * h.u + c->u * h.v;
            out->v = a->v * wa + b->v * h.u + c->v * h.v;
            return true;
        }
        /* (on, past what is not there) */
        const float past = h.t + reachOf(p);
        travelled += past;
        for (int k = 0; k < 3; ++k) o[k] += d[k] * past;
        if (travelled >= limit) return false;
    }
    return false;
}

bool artfSceneRaycast(ArtfScene* s, ArtfVec3 origin, ArtfVec3 direction, float maxDistance, ArtfPickHit* out) {
    const float length = artfVec3Length(direction);
    if (!s || !out || !(length > 0)) return false;
    const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {direction.x / length, direction.y / length, direction.z / length};
    return pickAlong(s, o, d, maxDistance > 0 ? artfMinf(maxDistance, 3e37f) : 3e37f, out);
}

bool artfScenePick(ArtfScene* s, const ArtfCamera* camera, float x, float y, int width, int height, ArtfPickHit* out) {
    if (!s || !out || width <= 0 || height <= 0) return false;
    const ArtfCamera* c = camera ? camera : &s->camera;
    const ArtfVec3 f = artfVec3Normalize(artfVec3Sub(c->target, c->position));
    const ArtfVec3 r = artfVec3Normalize(artfVec3Cross(f, c->up)), u = artfVec3Cross(r, f);
    const float aspect = (float)width / (float)height, px = x / (float)width * 2.f - 1.f, py = 1.f - y / (float)height * 2.f;
    float o[3], d[3], start, limit = 3e37f;
    if (c->orthoHeight > 0) {
        /* from the view's plane at near, straight ahead: what is drawn there, as far as far */
        const float half = c->orthoHeight * .5f;
        for (int k = 0; k < 3; ++k) {
            o[k] = (&c->position.x)[k] + (&r.x)[k] * px * half * aspect + (&u.x)[k] * py * half + (&f.x)[k] * c->near_;
            d[k] = (&f.x)[k];
        }
        start = c->near_;
        if (c->far_ > c->near_) limit = c->far_ - c->near_;
    } else {
        /* from the eye, through the pixel, starting at near as the frame does */
        const float tanY = artfTan(c->fovY * .5f);
        float l = 0;
        for (int k = 0; k < 3; ++k) {
            d[k] = (&f.x)[k] + (&r.x)[k] * px * tanY * aspect + (&u.x)[k] * py * tanY;
            l += d[k] * d[k];
        }
        l = sqrtf(l);
        for (int k = 0; k < 3; ++k) d[k] /= l;
        start = c->near_ * l;
        for (int k = 0; k < 3; ++k) o[k] = (&c->position.x)[k] + d[k] * start;
    }
    if (!pickAlong(s, o, d, limit, out)) return false;
    out->distance += start;
    return true;
}
