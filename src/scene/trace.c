/* artful — what rays need: a tree of boxes for each mesh, one over the instances, and the paths
 * themselves, as plain C. The CPU draws with these; the GPU reads the same trees, and its shader
 * (shaders.c, artfShaderTrace) is this same arithmetic. */
#include "scene/sceneInternal.h"

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
 * do not care for distance. */
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
    ArtfTraceRange range = {root, 0, firstVertex, m->deformVersion};
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
        const ArtfTraceTriangle o = {{firstVertex + c[0], firstVertex + c[1], firstVertex + c[2]}, 0};
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
    uint32_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        const ArtfInstanceData* d = &s->instances.items[i];
        ArtfTraceInstance o;
        memset(&o, 0, sizeof o);
        if (!d->opacity || !invert(d->m, o.toObject)) continue;
        o.root = t->roots.items[d->mesh];
        o.material = d->material;
        o.opacity = (float)d->opacity * (1.f / 65535.f);
        /* (what bends light must be met on the way out, too) */
        o.twoSided = s->materials.items[d->material].flags & ARTF_MATERIAL_DOUBLE || s->materials.items[d->material].ior > 0 ? 1 : 0;
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
    for (uint32_t i = 0; i < n; ++i) {
        t->instances.items[i] = made[order[i]];
        memcpy(t->motion.items + (size_t)i * 12, moved + (size_t)order[i] * 12, 48);
    }
    artfFree(moved);
    artfFree(boxes);
    artfFree(order);
    artfFree(made);
}

void artfSceneTraceSync(ArtfScene* s) {
    ArtfTrace* t = &s->trace;
    artfScenePosesApply(s);
    bool changed = false;
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
        ArtfTraceRange* range = &t->ranges.items[mesh];
        if (range->deformVersion == s->meshes.items[mesh].deformVersion) {
            /* (a pose that stays: nothing moved since the frame before) */
            if (range->moved) {
                for (uint32_t k = 0; k < s->meshes.items[mesh].vertexCount; ++k) memset(t->vertices.items[range->firstVertex + k].back, 0, 12);
                range->moved = false;
                changed = true;
            }
            continue;
        }
        refit(t, &s->meshes.items[mesh], range);
        range->moved = true;
        changed = true;
    }
    if (changed) ++t->meshVersion;
    if (changed || !t->top.len || t->instanceVersion != s->instanceVersion || t->structureVersion != s->structureVersion || t->materialVersion != s->materialVersion) {
        t->instanceVersion = s->instanceVersion;
        t->structureVersion = s->structureVersion;
        t->materialVersion = s->materialVersion;
        buildTop(s);
        ++t->topVersion;
        t->motionLive = true;
        ++t->motionVersion;
    } else if (t->motionLive) {
        static const float still[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        for (size_t i = 0; i < t->motion.len; i += 12) memcpy(t->motion.items + i, still, sizeof still);
        t->motionLive = false;
        ++t->motionVersion;
    }
}

void artfTraceFini(ArtfTrace* t) {
    artfVecFree(&t->nodes);
    artfVecFree(&t->triangles);
    artfVecFree(&t->vertices);
    artfVecFree(&t->roots);
    artfVecFree(&t->ranges);
    artfVecFree(&t->top);
    artfVecFree(&t->instances);
    artfVecFree(&t->motion);
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
    const uint64_t versions[9] = {s->trace.desc.denoise, s->id, s->textureVersion, s->instanceVersion, s->structureVersion, s->materialVersion, (uint64_t)s->trace.desc.mode, (uint64_t)s->trace.desc.bounces, s->environmentVersion};
    for (int i = 0; i < 9; ++i) h = (h ^ versions[i]) * 1099511628211ull;
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
                if (inst->twoSided || bothSides ? det == 0 : det <= 0) continue;
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
    float rough, a2, k, ndv, open;
    float f0[3], mirror[3]; /* what it reflects head on, and toward the eye */
    float emissive[3];
} Surface;

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

static void surfaceAt(const ArtfScene* s, const Hit* h, const float* origin, const float* d, Surface* out) {
    const ArtfTrace* t = &s->trace;
    const ArtfTraceInstance* inst = &t->instances.items[h->instance];
    const ArtfTraceTriangle* tri = &t->triangles.items[h->triangle];
    const ArtfTraceVertex *a = &t->vertices.items[tri->v[0]], *b = &t->vertices.items[tri->v[1]], *c = &t->vertices.items[tri->v[2]];
    const float wa = 1.f - h->u - h->v, wb = h->u, wc = h->v;
    const ArtfMaterialData* m = &s->materials.items[inst->material];
    out->material = m;

    const float u = a->u * wa + b->u * wb + c->u * wc, v = a->v * wa + b->v * wb + c->v * wc;
    const ArtfTextureData* textures = s->textures.items;
    float surface[4] = {1, 1, 1, 1}, occlusion[4] = {1, 1, 1, 1};
    if (m->surfaceMap) artfTexelAt(&textures[m->surfaceMap], u, v, surface);
    if (m->occlusionMap) artfTexelAt(&textures[m->occlusionMap], u, v, occlusion);

    /* normals go by the inverse's transpose; the same with the color's channels and alpha, by weight */
    const float* k = inst->toObject;
    float on[3] = {a->n[0] * wa + b->n[0] * wb + c->n[0] * wc, a->n[1] * wa + b->n[1] * wb + c->n[1] * wc, a->n[2] * wa + b->n[2] * wb + c->n[2] * wc};
    if (m->normalMap) {
        /* where u and v go on the triangle, in the object: as in kernels.inc */
        float bumps[4], tangent[3], bitangent[3];
        artfTexelAt(&textures[m->normalMap], u, v, bumps);
        const float du1 = b->u - a->u, dv1 = b->v - a->v, du2 = c->u - a->u, dv2 = c->v - a->v;
        const float det = du1 * dv2 - du2 * dv1, inv = det != 0 ? 1.f / det : 0.f;
        for (int i = 0; i < 3; ++i) {
            const float e1 = b->p[i] - a->p[i], e2 = c->p[i] - a->p[i];
            tangent[i] = (e1 * dv2 - e2 * dv1) * inv;
            bitangent[i] = (e2 * du1 - e1 * du2) * inv;
        }
        const float tl = dot3(tangent, tangent), bl = dot3(bitangent, bitangent), ts = tl > 0 ? 1.f / sqrtf(tl) : 0.f, bs = bl > 0 ? 1.f / sqrtf(bl) : 0.f;
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
        if (ch < 3) { x *= x; y *= y; z *= z; }
        color[ch] = x * wa + y * wb + z * wc;
    }
    float texel[4] = {1, 1, 1, 1};
    if (m->texture) artfTexelAt(&textures[m->texture], u, v, texel);
    float albedo[3];
    for (int i = 0; i < 3; ++i) albedo[i] = m->baseColor[i] * texel[2 - i] * texel[2 - i] * color[i];
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
    memcpy(out->emissive, emissive, 12);
    const float notMetal = 1.f - metal;
    out->open = 1.f + (occlusion[2] - 1.f) * m->occlusion;
    out->shiny = m->specular > 0 || m->metallic > 0;
    const float alphaRough = out->rough * out->rough, plusOne = out->rough + 1.f;
    out->a2 = alphaRough * alphaRough;
    out->k = plusOne * plusOne * .125f;
    out->ndv = artfMaxf(-dot3(out->normal, d), 1e-4f);
    const float y = 1.f - out->ndv, y2 = y * y, far = y2 * y2 * y;
    for (int i = 0; i < 3; ++i) {
        out->f0[i] = .04f * m->specular * notMetal + albedo[i] * metal;
        out->mirror[i] = out->shiny ? out->f0[i] + (artfMaxf(1.f - out->rough, out->f0[i]) - out->f0[i]) * far : 0.f;
        out->albedo[i] = albedo[i] * notMetal;
    }
    const float coverage = texel[3] * color[3];
    out->alpha = inst->opacity * m->opacity * color[3] * graphAlpha;
    if (m->flags & ARTF_MATERIAL_BLEND) out->alpha *= texel[3];
    if (m->flags & ARTF_MATERIAL_MASK && coverage < m->cutoff) out->alpha = 0;
}

/* ---- a path ---- */

static inline uint32_t hashOf(uint32_t x) {
    x = (x ^ x >> 16) * 0x7FEB352Du;
    x = (x ^ x >> 15) * 0x846CA68Bu;
    return x ^ x >> 16;
}
static inline float randomOf(uint32_t* state) {
    *state = *state * 747796405u + 2891336453u;
    return (float)(hashOf(*state) >> 8) * (1.f / 16777216.f);
}

/* A direction, even in a ball: with no sine or cosine, which differ from one C library to the next. */
static void randomInBall(uint32_t* state, float* out) {
    for (int i = 0; i < 16; ++i) {
        for (int k = 0; k < 3; ++k) out[k] = randomOf(state) * 2.f - 1.f;
        if (dot3(out, out) <= 1.f) return;
    }
    out[0] = out[1] = out[2] = 0;
}

static inline void normalize3(float* a) {
    const float l = 1.f / sqrtf(artfMaxf(dot3(a, a), 1e-30f));
    for (int k = 0; k < 3; ++k) a[k] *= l;
}

static inline float reachOf(const float* p) { return 1e-4f * (1.f + artfMaxf(fabsf(p[0]), artfMaxf(fabsf(p[1]), fabsf(p[2])))); }

/* How much light gets from a place to a light: what is in the way stops it as far as it is there. */
static float lightThrough(const ArtfScene* s, const float* from, const float* to, float limit) {
    float o[3] = {from[0], from[1], from[2]}, through = 1.f;
    for (int i = 0; i < SHADOW_STEPS; ++i) {
        Hit h;
        if (!closest(&s->trace, o, to, limit, true, &h)) return through;
        Surface m;
        surfaceAt(s, &h, o, to, &m);
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
        float half[3] = {view[0] + toLight[0], view[1] + toLight[1], view[2] + toLight[2]};
        normalize3(half);
        const float ndh = artfMaxf(dot3(m->normal, half), 0), vdh = artfMaxf(dot3(view, half), 0);
        const float d = ndh * ndh * (m->a2 - 1.f) + 1.f, rest = 1.f - m->k;
        const float seen = (m->ndv * rest + m->k) * (ndl * rest + m->k);
        const float lit = m->a2 / (d * d) * ndl * m->material->specular / (seen * 4.f);
        const float y = 1.f - vdh, far = y * y * y * y * y;
        for (int k = 0; k < 3; ++k) highlight[k] = (m->f0[k] + (1.f - m->f0[k]) * far) * lit;
    }
    for (int k = 0; k < 3; ++k) light[k] += path[k] * color[k] * strength * through * (m->albedo[k] * ndl + highlight[k]);
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
    uint32_t state = hashOf(hashOf(x + y * c->width) + sample);
    const float px = ((float)x + randomOf(&state)) / (float)c->width * 2.f - 1.f, py = 1.f - ((float)y + randomOf(&state)) / (float)c->height * 2.f;
    float o[3], d[3], path[3] = {1, 1, 1}, light[3] = {0, 0, 0}, travelled = 0, keep = 1.f;
    for (int k = 0; k < 3; ++k) {
        o[k] = c->eye[k];
        d[k] = c->forward[k] + c->right[k] * px + c->up[k] * py;
    }
    normalize3(d);
    bool covered = false;
    uint32_t bounces = 0;
    for (int step = 0; step < MAX_STEPS; ++step) {
        Hit h;
        if (!closest(&s->trace, o, d, 3e37f, false, &h)) {
            /* the sky, for what has met something: what has not leaves the frame's own background */
            if (covered && f->sky[3] > 0 && s->environment) {
                float around[3];
                artfEnvSample(s->environment, 0, d[0], d[1], d[2], around);
                for (int k = 0; k < 3; ++k) light[k] += path[k] * around[k] * f->sky[3];
            } else if (covered) {
                const float up = d[1] * .5f + .5f;
                for (int k = 0; k < 3; ++k) light[k] += path[k] * (f->ground[k] + (f->sky[k] - f->ground[k]) * up);
            }
            break;
        }
        Surface m;
        surfaceAt(s, &h, o, d, &m);
        float p[3];
        for (int k = 0; k < 3; ++k) p[k] = o[k] + d[k] * h.t;
        const float reach = reachOf(p);
        travelled += h.t;
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
        float ball[3];
        randomInBall(&state, ball);
        if (f->sunColor[0] > 0 || f->sunColor[1] > 0 || f->sunColor[2] > 0) {
            /* (a sun with a size: each path sees another point of it, and shadows go soft with distance) */
            float toSun[3];
            for (int k = 0; k < 3; ++k) toSun[k] = f->toSun[k] + ball[k] * f->look[2];
            normalize3(toSun);
            lightFrom(s, lifted, view, &m, toSun, 3e37f, 1.f, f->sunColor, path, light);
        }
        for (uint32_t i = 0; i < f->lightCount; ++i) {
            const ArtfLightData* L = &f->lights[i];
            float to[3] = {L->position[0] - lifted[0], L->position[1] - lifted[1], L->position[2] - lifted[2]};
            const float d2 = dot3(to, to), distance = sqrtf(artfMaxf(d2, 1e-8f));
            const float fade = artfMaxf(1.f - d2 / (L->radius * L->radius), 0);
            for (int k = 0; k < 3; ++k) to[k] /= distance;
            const float spot = artfClampf((-dot3(to, L->direction) - L->spotCut) * L->spotScale, 0, 1);
            lightFrom(s, lifted, view, &m, to, distance, fade * fade * spot, L->color, path, light);
        }
        const float dn = dot3(d, m.normal);
        float mirror[3];
        for (int k = 0; k < 3; ++k) mirror[k] = d[k] - 2.f * dn * m.normal[k];
        /* (how rough it is, as how far a mirror's ray strays) */
        const float stray = m.rough * m.rough;
        if (!c->paths) {
            /* rays: light from all around as the rasterizer has it, and a mirror for what reflects */
            const float* env = f->sky[3] > 0 ? s->environment : NULL;
            const float up = m.normal[1] * .5f + .5f;
            float around[3];
            if (env) artfEnvSample(env, ARTF_ENV_DIFFUSE, m.normal[0], m.normal[1], m.normal[2], around);
            for (int k = 0; k < 3; ++k) light[k] += path[k] * m.albedo[k] * m.open * (env ? around[k] * f->sky[3] : f->ground[k] + (f->sky[k] - f->ground[k]) * up);
            if (!m.shiny) break;
            if (bounces >= c->bounces) {
                const float reflectedUp = mirror[1] * .5f + .5f;
                if (env) artfEnvReflected(env, m.rough, mirror[0], mirror[1], mirror[2], around);
                for (int k = 0; k < 3; ++k) light[k] += path[k] * m.mirror[k] * m.open * (env ? around[k] * f->sky[3] : f->ground[k] + (f->sky[k] - f->ground[k]) * reflectedUp);
                break;
            }
            for (int k = 0; k < 3; ++k) {
                path[k] *= m.mirror[k];
                d[k] = mirror[k] + ball[k] * stray;
            }
        } else {
            if (bounces >= c->bounces) break;
            const float most = artfMaxf(m.mirror[0], artfMaxf(m.mirror[1], m.mirror[2]));
            const float chance = m.shiny ? artfClampf(most, .05f, .95f) : 0.f;
            if (randomOf(&state) < chance) {
                for (int k = 0; k < 3; ++k) {
                    d[k] = mirror[k] + ball[k] * stray;
                    path[k] *= m.mirror[k] / chance;
                }
            } else {
                /* a point on a ball over the surface: as many rays as the cosine says */
                const float keep = m.open * (1.f - most) / (1.f - chance);
                for (int k = 0; k < 3; ++k) {
                    d[k] = m.normal[k] * 1.0001f + ball[k];
                    path[k] *= m.albedo[k] * keep;
                }
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
