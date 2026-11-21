/* artful — the software backend's hot code, built once for each instruction set (kernelsAvx2.c,
 * kernelsPortable.c). KERNELS is the name of the table it fills.
 *
 * It works as a phone's GPU does:
 *
 *   geometry  meshlets are culled and transformed, and each triangle that can show goes into the
 *             bins of the tiles it touches. Nothing is drawn.
 *   tile      a 64 x 64 tile at a time, on one thread, its memory in the cache: first all its triangles
 *             are rasterized for depth alone, leaving for each pixel the id of the triangle that
 *             shows; then only the triangles that show are set up and shaded, each pixel once. The
 *             depth buffer never exists outside a tile, and the frame is written once.
 *   layers    what shows what is behind it comes after that, in the same tile, an object at a time.
 *
 * Rasterizing is exact: edge functions on integers, 1/16 pixel vertices, the top-left rule. With
 * antialiasing each of a pixel's four samples is a plane of its own, and only what differs from the first
 * plane is shaded again: so a surface costs its pixels, and an edge its samples. */
#include "software/simd.h"
#include "software/softwareInternal.h"

/* ---- geometry ------------------------------------------------------------------------------------ */

enum { OUT_LEFT = 1, OUT_RIGHT = 2, OUT_DOWN = 4, OUT_UP = 8, OUT_NEAR = 16, OUT_GUARD = 32, OUT_FRUSTUM = 31, OUT_CLIP = OUT_NEAR | OUT_GUARD };

typedef struct GeometryContext {
    ArtfSoftFrame* f;
    ArtfChunk* chunk;
    uint32_t instance;
    uint32_t key;
    bool twoSided;
} GeometryContext;

static inline void binPush(ArtfChunk* c, uint32_t tile, uint32_t tri) {
    uint32_t* head = c->heads[tile];
    if (head[1]) {
        ArtfBinBlock* b = &c->blocks.items[head[1] - 1];
        if (b->count < ARTF_BIN_CAP) { b->tris[b->count++] = tri; return; }
    }
    artfVecReserve(&c->blocks, c->blocks.len + 1);
    ArtfBinBlock* b = &c->blocks.items[c->blocks.len++];
    uint32_t index = (uint32_t)c->blocks.len;
    b->next = 0;
    b->count = 1;
    b->tris[0] = tri;
    if (head[1]) c->blocks.items[head[1] - 1].next = index;
    else head[0] = index;
    head[1] = index;
}

/* z: 1 / w at each vertex, or an orthographic view's depth. */
static inline void emitTriangle(GeometryContext* g, int32_t x0, int32_t y0, float z0, int32_t x1, int32_t y1, float z1, int32_t x2, int32_t y2, float z2) {
    /* In a frame, y goes down: a triangle counter-clockwise from the front has a negative area. The
     * others face away, and show only with two sides. Swapping two vertices leaves the rest with a
     * positive one. */
    int64_t area = (int64_t)(x1 - x0) * (y2 - y0) - (int64_t)(x2 - x0) * (y1 - y0);
    if (!area || (area > 0 && !g->twoSided)) return;
    const uint32_t back = area > 0 ? ARTF_TRI_BACK : 0;
    if (area < 0) {
        int32_t tx = x1, ty = y1;
        float tz = z1;
        x1 = x2; y1 = y2; z1 = z2;
        x2 = tx; y2 = ty; z2 = tz;
        area = -area;
    }

    /* The pixels whose centers (16 x + 8) may be inside, or their samples. A small triangle often has
     * none, which ends it here. */
    ArtfSoftFrame* f = g->f;
    const int reach = f->samples > 1 ? ARTF_SAMPLE_REACH : 0;
    int lox = x0 < x1 ? x0 : x1, hix = x0 < x1 ? x1 : x0, loy = y0 < y1 ? y0 : y1, hiy = y0 < y1 ? y1 : y0;
    lox = x2 < lox ? x2 : lox;
    hix = x2 > hix ? x2 : hix;
    loy = y2 < loy ? y2 : loy;
    hiy = y2 > hiy ? y2 : hiy;
    int minX = (lox + 7 - reach) >> 4, maxX = (hix - 8 + reach) >> 4, minY = (loy + 7 - reach) >> 4, maxY = (hiy - 8 + reach) >> 4;
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX >= f->width) maxX = f->width - 1;
    if (maxY >= f->height) maxY = f->height - 1;
    if (minX > maxX || minY > maxY) return;

    ArtfChunk* c = g->chunk;
    artfVecReserve(&c->tris, c->tris.len + 1);
    uint32_t index = (uint32_t)c->tris.len++;
    ArtfRasterTri* t = &c->tris.items[index];

    /* depth over the pixels, from the first vertex */
    double a = (double)area / 256.0;
    double d1x = (x1 - x0) / 16.0, d1y = (y1 - y0) / 16.0, d2x = (x2 - x0) / 16.0, d2y = (y2 - y0) / 16.0;
    double dz1 = (double)z1 - z0, dz2 = (double)z2 - z0;
    double zdx = (dz1 * d2y - dz2 * d1y) / a, zdy = (dz2 * d1x - dz1 * d2x) / a;
    t->zdx = (float)zdx;
    t->zdy = (float)zdy;
    t->z0 = (double)z0 - zdx * ((x0 - 8) / 16.0) - zdy * ((y0 - 8) / 16.0);
    t->x[0] = x0; t->x[1] = x1; t->x[2] = x2;
    t->y[0] = y0; t->y[1] = y1; t->y[2] = y2;
    t->instance = g->instance;
    t->triangle = g->key | back;
    t->minX = (int16_t)minX;
    t->minY = (int16_t)minY;
    t->maxX = (int16_t)maxX;
    t->maxY = (int16_t)maxY;

    const int tx0 = minX / ARTF_TILE, tx1 = maxX / ARTF_TILE, ty0 = minY / ARTF_TILE, ty1 = maxY / ARTF_TILE;
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int txx = tx0; txx <= tx1; ++txx) binPush(c, (uint32_t)(ty * f->tilesX + txx), index);
}

typedef struct ClipVertex { float x, y, w, z; } ClipVertex; /* (z: an orthographic view's depth) */

/* Keeps what is on the positive side of a * x + b * y + c * w + e * z + d >= 0. */
static int clipPlane(const ClipVertex* in, int n, ClipVertex* out, float a, float b, float c, float e, float d) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const ClipVertex *p = &in[i], *q = &in[i + 1 == n ? 0 : i + 1];
        float dp = a * p->x + b * p->y + c * p->w + e * p->z + d, dq = a * q->x + b * q->y + c * q->w + e * q->z + d;
        if (dp >= 0) out[m++] = *p;
        if ((dp >= 0) != (dq >= 0)) {
            /* Two triangles share this edge, and walk it in opposite ways: worked out in one order
             * for both, it ends at the same point in both, and no crack opens between them. */
            const bool swap = p->x > q->x || (p->x == q->x && (p->y > q->y || (p->y == q->y && p->w > q->w)));
            const ClipVertex *from = swap ? q : p, *to = swap ? p : q;
            const float df = swap ? dq : dp, dt = swap ? dp : dq;
            const float t = df / (df - dt);
            ClipVertex v = {from->x + (to->x - from->x) * t, from->y + (to->y - from->y) * t, from->w + (to->w - from->w) * t, from->z + (to->z - from->z) * t};
            out[m++] = v;
        }
    }
    return m;
}

/* The rare triangle: behind the eye, or too far outside to fit the integers. */
static void clipTriangle(GeometryContext* g, const ClipVertex* v) {
    ArtfSoftFrame* f = g->f;
    ClipVertex a[12], b[12];
    a[0] = v[0]; a[1] = v[1]; a[2] = v[2];
    /* (orthographic: w is 1, and what is nearer than near has a depth above 1) */
    const bool ortho = artfOrtho(&f->data);
    int n = ortho ? clipPlane(a, 3, b, 0, 0, 0, -1, 1) : clipPlane(a, 3, b, 0, 0, 1, 0, -f->nearW);
    n = clipPlane(b, n, a, -1, 0, f->guardX, 0, 0);
    n = clipPlane(a, n, b, 1, 0, f->guardX, 0, 0);
    n = clipPlane(b, n, a, 0, -1, f->guardY, 0, 0);
    n = clipPlane(a, n, b, 0, 1, f->guardY, 0, 0);
    if (n < 3) return;
    int32_t x[12], y[12];
    float z[12];
    const float hw = (float)f->width * .5f, hh = (float)f->height * .5f;
    for (int i = 0; i < n; ++i) {
        float iw = 1.f / b[i].w;
        x[i] = (int32_t)floorf((b[i].x * iw * hw + hw) * 16.f + .5f);
        y[i] = (int32_t)floorf((hh - b[i].y * iw * hh) * 16.f + .5f);
        z[i] = ortho ? b[i].z : iw;
    }
    for (int i = 1; i + 1 < n; ++i) emitTriangle(g, x[0], y[0], z[0], x[i], y[i], z[i], x[i + 1], y[i + 1], z[i + 1]);
}

static inline bool meshletCulled(const ArtfSoftFrame* f, const ArtfVisible* v, const ArtfMeshlet* l) {
    const ArtfMeshData* mesh = v->mesh;
    if (mesh->dagBounds) {
        /* a graph's cluster: drawn where it is as fine as asked and its parent is not (artfDagDrawn) */
        const uint32_t k = (uint32_t)(l - mesh->meshlets);
        if (!artfDagDrawn(mesh->dagBounds + (size_t)k * 10, k < mesh->dagWhole, v->instance->m, v->instance->maxScale, f->data.cameraPos, artfOrtho(&f->data) ? 0.f : f->data.lodScale)) return true;
    }
    if (v->useCone && l->coneCutoff < 2.f && artfOrtho(&f->data)) {
        /* (all rays go one way: all its triangles face away when that way is inside the cone's back) */
        if (v->camera[0] * l->cone[0] + v->camera[1] * l->cone[1] + v->camera[2] * l->cone[2] >= l->coneCutoff) return true;
    } else if (v->useCone && l->coneCutoff < 2.f) {
        float dx = l->center[0] - v->camera[0], dy = l->center[1] - v->camera[1], dz = l->center[2] - v->camera[2];
        float along = dx * l->cone[0] + dy * l->cone[1] + dz * l->cone[2];
        if (along >= l->coneCutoff * sqrtf(dx * dx + dy * dy + dz * dz) + l->radius) return true;
    }
    if (v->inside) return false;
    const float* m = v->instance->m;
    const float cx = m[0] * l->center[0] + m[1] * l->center[1] + m[2] * l->center[2] + m[3];
    const float cy = m[4] * l->center[0] + m[5] * l->center[1] + m[6] * l->center[2] + m[7];
    const float cz = m[8] * l->center[0] + m[9] * l->center[1] + m[10] * l->center[2] + m[11];
    const float r = l->radius * v->instance->maxScale;
    for (int i = 0; i < 5; ++i) {
        const float* p = f->data.planes[i];
        if (p[0] * cx + p[1] * cy + p[2] * cz + p[3] < -r) return true;
    }
    return false;
}

static void geometryMeshlet(GeometryContext* g, const ArtfVisible* v, uint32_t meshletIndex) {
    ArtfSoftFrame* f = g->f;
    const ArtfMeshData* mesh = v->mesh;
    const ArtfMeshlet* l = &mesh->meshlets[meshletIndex];
    if (meshletCulled(f, v, l)) return;
    g->twoSided = v->twoSided;

    _Alignas(32) float clipX[ARTF_MESHLET_VERTICES], clipY[ARTF_MESHLET_VERTICES], clipW[ARTF_MESHLET_VERTICES], invW[ARTF_MESHLET_VERTICES];
    _Alignas(32) int32_t fixedX[ARTF_MESHLET_VERTICES], fixedY[ARTF_MESHLET_VERTICES], codes[ARTF_MESHLET_VERTICES];
    const float* m = v->mvp;
    const V8F m00 = v8fSet(m[0]), m01 = v8fSet(m[1]), m02 = v8fSet(m[2]), m03 = v8fSet(m[3]);
    const V8F m10 = v8fSet(m[4]), m11 = v8fSet(m[5]), m12 = v8fSet(m[6]), m13 = v8fSet(m[7]);
    const V8F m20 = v8fSet(m[8]), m21 = v8fSet(m[9]), m22 = v8fSet(m[10]), m23 = v8fSet(m[11]);
    const V8F nearW = v8fSet(f->nearW), guardX = v8fSet(f->guardX), guardY = v8fSet(f->guardY);
    const V8F halfW = v8fSet((float)f->width * .5f), halfH = v8fSet((float)f->height * .5f);
    const V8F sixteen = v8fSet(16.f), half = v8fSet(.5f), one = v8fSet(1.f), zero = v8fSet(0.f);
    const V8F limit = v8fSet(262000.f); /* what the integers below are good for */
    /* orthographic: w is 1, and depth is the clip's z, which is cut at 1 (near) */
    const bool ortho = artfOrtho(&f->data);
    const V8F z0 = v8fSet(v->depthRow[0]), z1 = v8fSet(v->depthRow[1]), z2 = v8fSet(v->depthRow[2]), z3 = v8fSet(v->depthRow[3]);
    const float* px = mesh->positions + l->vertexOffset;
    const float* py = px + mesh->positionStride;
    const float* pz = py + mesh->positionStride;

    for (uint32_t i = 0; i < l->vertexCount; i += 8) {
        V8F x = v8fLoad(px + i), y = v8fLoad(py + i), z = v8fLoad(pz + i);
        V8F cx = v8fAdd(v8fAdd(v8fAdd(v8fMul(m00, x), v8fMul(m01, y)), v8fMul(m02, z)), m03);
        V8F cy = v8fAdd(v8fAdd(v8fAdd(v8fMul(m10, x), v8fMul(m11, y)), v8fMul(m12, z)), m13);
        V8F cw = v8fAdd(v8fAdd(v8fAdd(v8fMul(m20, x), v8fMul(m21, y)), v8fMul(m22, z)), m23);
        V8F negW = v8fSub(zero, cw);
        V8I code = v8iAnd(v8fLt(cx, negW), v8iSet(OUT_LEFT));
        code = v8iOr(code, v8iAnd(v8fGt(cx, cw), v8iSet(OUT_RIGHT)));
        code = v8iOr(code, v8iAnd(v8fLt(cy, negW), v8iSet(OUT_DOWN)));
        code = v8iOr(code, v8iAnd(v8fGt(cy, cw), v8iSet(OUT_UP)));
        code = v8iOr(code, v8iAnd(v8fLt(cw, nearW), v8iSet(OUT_NEAR)));
        V8I far = v8iOr(v8fGt(v8fAbs(cx), v8fMul(guardX, cw)), v8fGt(v8fAbs(cy), v8fMul(guardY, cw)));
        code = v8iOr(code, v8iAnd(far, v8iSet(OUT_GUARD)));
        V8F iw = v8fDiv(one, v8fMax(cw, nearW)); /* (what is nearer is clipped: its numbers are not used) */
        V8F sx = v8fAdd(v8fMul(v8fMul(cx, iw), halfW), halfW);
        V8F sy = v8fSub(halfH, v8fMul(v8fMul(cy, iw), halfH));
        if (ortho) {
            iw = v8fAdd(v8fAdd(v8fAdd(v8fMul(z0, x), v8fMul(z1, y)), v8fMul(z2, z)), z3);
            code = v8iOr(code, v8iAnd(v8fGt(iw, one), v8iSet(OUT_NEAR)));
        }
        sx = v8fMin(v8fMax(v8fFloor(v8fAdd(v8fMul(sx, sixteen), half)), v8fSub(zero, limit)), limit);
        sy = v8fMin(v8fMax(v8fFloor(v8fAdd(v8fMul(sy, sixteen), half)), v8fSub(zero, limit)), limit);
        v8fStore(clipX + i, cx);
        v8fStore(clipY + i, cy);
        v8fStore(clipW + i, cw);
        v8fStore(invW + i, iw);
        v8iStore(fixedX + i, v8fToI(sx));
        v8iStore(fixedY + i, v8fToI(sy));
        v8iStore(codes + i, code);
    }

    const uint8_t* tri = mesh->meshletTriangles + (size_t)l->triangleOffset * 3;
    for (uint32_t t = 0; t < l->triangleCount; ++t, tri += 3) {
        const uint32_t a = tri[0], b = tri[1], c = tri[2];
        const int ca = codes[a], cb = codes[b], cc = codes[c];
        if (ca & cb & cc & OUT_FRUSTUM) continue;
        g->key = meshletIndex << 7 | t;
        if ((ca | cb | cc) & OUT_CLIP) {
            const ClipVertex in[3] = {{clipX[a], clipY[a], clipW[a], invW[a]}, {clipX[b], clipY[b], clipW[b], invW[b]}, {clipX[c], clipY[c], clipW[c], invW[c]}};
            clipTriangle(g, in);
        } else {
            emitTriangle(g, fixedX[a], fixedY[a], invW[a], fixedX[b], fixedY[b], invW[b], fixedX[c], fixedY[c], invW[c]);
        }
    }
}

static void geometry(ArtfSoftFrame* f, uint32_t chunkIndex) {
    ArtfChunk* chunk = &f->chunks[chunkIndex];
    if (!f->geometryAppend) {
        chunk->tris.len = 0;
        chunk->blocks.len = 0;
        memset(chunk->heads, 0, (size_t)f->tilesX * f->tilesY * sizeof *chunk->heads);
    }
    if (f->firstVisible >= f->visibleCount) return;
    const uint32_t base = f->meshletStarts[f->firstVisible];
    const uint64_t total = f->meshletStarts[f->visibleCount] - base;
    const uint32_t first = base + (uint32_t)(total * chunkIndex / ARTF_CHUNKS), end = base + (uint32_t)(total * (chunkIndex + 1) / ARTF_CHUNKS);
    if (first >= end) return;

    /* the last instance that starts at or before first */
    uint32_t lo = f->firstVisible, hi = f->visibleCount;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (f->meshletStarts[mid] <= first) lo = mid;
        else hi = mid;
    }
    GeometryContext g = {f, chunk, lo, 0, false};
    for (uint32_t i = first; i < end; ++i) {
        while (i >= f->meshletStarts[g.instance + 1]) ++g.instance;
        geometryMeshlet(&g, &f->visible[g.instance], i - f->meshletStarts[g.instance]);
    }
}

/* ---- rasterizing --------------------------------------------------------------------------------- */

typedef struct ShadeContext ShadeContext;
/* Which of a block's eight pixels are holes in a material that has them. */
static V8I maskHoles(const ShadeContext* s, int x, int y);

/* Where a pixel's four samples are, from its center, in 1/16 pixels: Vulkan's standard places. */
static const int8_t sampleX[4] = {-2, 6, -6, 2}, sampleY[4] = {-6, -2, 2, 6};

/* Depth and id of a triangle over the part of a tile it covers, into one sample's planes. ox, oy: the
 * tile's corner; sx, sy: where the sample is. */
static inline void rasterizeBelow(float* depthPlane, uint32_t* idPlane, const ArtfRasterTri* t, uint32_t id, int ox, int oy, int sx_, int sy_, const ShadeContext* mask, const float* belowPlane, bool* met);
static inline void rasterize(float* depthPlane, uint32_t* idPlane, const ArtfRasterTri* t, uint32_t id, int ox, int oy, int sx_, int sy_, const ShadeContext* mask) {
    rasterizeBelow(depthPlane, idPlane, t, id, ox, oy, sx_, sy_, mask, NULL, NULL);
}

/* As rasterize, and only what is farther than belowPlane says, where it is given (depth is greater when nearer); met,
 * where given, made true if it covers a pixel that another triangle took. */
static inline void rasterizeBelow(float* depthPlane, uint32_t* idPlane, const ArtfRasterTri* t, uint32_t id, int ox, int oy, int sx_, int sy_, const ShadeContext* mask, const float* belowPlane, bool* met) {
    /* in the tile; columns from a whole vector */
    const int bx0 = (t->minX > ox ? t->minX - ox : 0) & ~7, by0 = t->minY > oy ? t->minY - oy : 0;
    const int bx1 = t->maxX - ox < ARTF_TILE - 1 ? t->maxX - ox : ARTF_TILE - 1, by1 = t->maxY - oy < ARTF_TILE - 1 ? t->maxY - oy : ARTF_TILE - 1;
    if (bx1 < bx0 || by1 < by0) return;

    /* E(x, y) >= 0 inside, at the center of the corner pixel: 64 bits, as the triangle may be huge.
     * Over one tile a value changes by less than 2^31, so an edge that crosses the tile fits 32
     * bits; one that does not either ends this here, or need not be looked at again. */
    int32_t e[3], stepX[3], stepY[3];
    const int64_t px = (int64_t)(ox + bx0) * 16 + 8 + sx_, py = (int64_t)(oy + by0) * 16 + 8 + sy_;
    const int spanX = (bx1 | 7) - bx0, spanY = by1 - by0; /* (to the last lane of the last block) */
    for (int i = 0; i < 3; ++i) {
        const int j = i == 2 ? 0 : i + 1;
        const int64_t dx = (int64_t)t->x[j] - t->x[i], dy = (int64_t)t->y[j] - t->y[i];
        const bool topLeft = dy < 0 || (dy == 0 && dx > 0);
        const int64_t value = dx * (py - t->y[i]) - dy * (px - t->x[i]) - (topLeft ? 0 : 1);
        const int64_t sx = -dy * 16, sy = dx * 16;
        const int64_t lowest = value + (sx < 0 ? sx * spanX : 0) + (sy < 0 ? sy * spanY : 0);
        const int64_t highest = value + (sx > 0 ? sx * spanX : 0) + (sy > 0 ? sy * spanY : 0);
        if (highest < 0) return;
        if (lowest >= 0) { e[i] = 0; stepX[i] = 0; stepY[i] = 0; continue; }
        e[i] = (int32_t)value;
        stepX[i] = (int32_t)sx;
        stepY[i] = (int32_t)sy;
    }

    const V8I lanes = v8iIota();
    const V8I sx0 = v8iSet(stepX[0]), sx1 = v8iSet(stepX[1]), sx2 = v8iSet(stepX[2]);
    const V8I block0 = v8iSet(stepX[0] * 8), block1 = v8iSet(stepX[1] * 8), block2 = v8iSet(stepX[2] * 8);
    const V8I lane0 = v8iMul(lanes, sx0), lane1 = v8iMul(lanes, sx1), lane2 = v8iMul(lanes, sx2);
    /* depth, from this tile's corner: the numbers stay small, and so does their error */
    const float zStart = (float)(t->z0 + (double)t->zdx * (ox + sx_ / 16.0) + (double)t->zdy * (oy + sy_ / 16.0));
    const V8F zdx = v8fSet(t->zdx), zBlock = v8fSet(t->zdx * 8.f);
    const V8F zLanes = v8fMul(v8iToF(lanes), zdx);
    const V8I idv = v8iSet((int32_t)id), minusOne = v8iSet(-1);

    int32_t r0 = e[0], r1 = e[1], r2 = e[2];
    for (int y = by0; y <= by1; ++y) {
        V8I e0 = v8iAdd(v8iSet(r0), lane0), e1 = v8iAdd(v8iSet(r1), lane1), e2 = v8iAdd(v8iSet(r2), lane2);
        float* depth = depthPlane + y * ARTF_TILE;
        uint32_t* ids = idPlane + y * ARTF_TILE;
        V8F z = v8fAdd(v8fSet(zStart + t->zdy * (float)y + t->zdx * (float)bx0), zLanes);
        bool entered = false;
        for (int x = bx0; x <= bx1; x += 8) {
            V8I outside = v8iOr(v8iOr(e0, e1), e2);
            int signs = v8iSigns(outside);
            if (signs != 0xFF) {
                entered = true;
                V8F old = v8fLoad(depth + x);
                V8I closer = v8iAnd(v8iGt(outside, minusOne), v8fGt(z, old));
                if (belowPlane) closer = v8iAnd(closer, v8fLt(z, v8fLoad(belowPlane + y * ARTF_TILE + x)));
                if (met && (v8iSigns(v8iGt(outside, minusOne)) & ~v8iSigns(v8iEq(v8iLoad(ids + x), v8iSet(0))))) *met = true;
                if (mask && v8iSigns(closer)) closer = v8iAndNot(maskHoles(mask, x, y), closer);
                if (v8iSigns(closer)) {
                    v8fStore(depth + x, v8fSelect(closer, old, z));
                    v8iStore(ids + x, v8iSelect(closer, v8iLoad(ids + x), idv));
                }
            } else if (entered) {
                break; /* (a triangle is convex: it does not come back on this row) */
            }
            e0 = v8iAdd(e0, block0);
            e1 = v8iAdd(e1, block1);
            e2 = v8iAdd(e2, block2);
            z = v8fAdd(z, zBlock);
        }
        r0 += stepY[0];
        r1 += stepY[1];
        r2 += stepY[2];
    }
}

/* ---- shading ------------------------------------------------------------------------------------- */

typedef struct Lanes3 { V8F x, y, z; } Lanes3;

V8_INLINE V8F dot3(Lanes3 a, Lanes3 b) { return v8fAdd(v8fAdd(v8fMul(a.x, b.x), v8fMul(a.y, b.y)), v8fMul(a.z, b.z)); }
V8_INLINE Lanes3 normalize3(Lanes3 a) {
    V8F inv = v8fDiv(v8fSet(1.f), v8fSqrt(dot3(a, a)));
    Lanes3 r = {v8fMul(a.x, inv), v8fMul(a.y, inv), v8fMul(a.z, inv)};
    return r;
}
/* Where the eye is for eight places (artfEyeOf): the camera, or in an orthographic view the place on its plane behind each. */
V8_INLINE Lanes3 eyeLanes(const ArtfFrameData* frame, Lanes3 world) {
    const Lanes3 camera = {v8fSet(frame->cameraPos[0]), v8fSet(frame->cameraPos[1]), v8fSet(frame->cameraPos[2])};
    if (!artfOrtho(frame)) return camera;
    const Lanes3 ahead = {v8fSet(frame->eye[2][0]), v8fSet(frame->eye[2][1]), v8fSet(frame->eye[2][2])};
    const Lanes3 from = {v8fSub(world.x, camera.x), v8fSub(world.y, camera.y), v8fSub(world.z, camera.z)};
    const V8F along = dot3(from, ahead);
    const Lanes3 r = {v8fSub(world.x, v8fMul(ahead.x, along)), v8fSub(world.y, v8fMul(ahead.y, along)), v8fSub(world.z, v8fMul(ahead.z, along))};
    return r;
}

/* artfLog2Fast and artfExp2Fast (core/color.h), eight lanes at a time: the same arithmetic in the same order. */
V8_INLINE V8F gradeClamp(V8F v, V8F lo, V8F hi) { return v8fSelect(v8fLt(v, lo), v8fSelect(v8fGt(v, hi), v, hi), lo); }
V8_INLINE V8F gradeLog2(V8F x) {
    const V8I bits = v8fAsI(x);
    const V8F e = v8fSub(v8iToF(v8iAnd(v8iShr(bits, 23), v8iSet(255))), v8fSet(127.f));
    const V8F m = v8iAsF(v8iOr(v8iAnd(bits, v8iSet(0x7FFFFF)), v8iSet(0x3F800000)));
    const V8F one = v8fSet(1.f), z = v8fDiv(v8fSub(m, one), v8fAdd(m, one)), z2 = v8fMul(z, z);
    const V8F p = v8fAdd(one, v8fMul(z2, v8fAdd(v8fSet(.33333333f), v8fMul(z2, v8fAdd(v8fSet(.2f), v8fMul(z2, v8fSet(.14285714f)))))));
    return v8fAdd(e, v8fMul(v8fMul(z, v8fSet(2.8853901f)), p));
}
V8_INLINE V8F gradeExp2(V8F x) {
    x = gradeClamp(x, v8fSet(-100.f), v8fSet(100.f));
    const V8I i = v8iSub(v8fToI(v8fAdd(x, v8fSet(128.f))), v8iSet(128));
    const V8F u = v8fMul(v8fSub(x, v8iToF(i)), v8fSet(.69314718f));
    V8F f = v8fAdd(v8fSet(.0083333333f), v8fMul(u, v8fSet(.0013888889f)));
    f = v8fAdd(v8fSet(.041666668f), v8fMul(u, f));
    f = v8fAdd(v8fSet(.16666667f), v8fMul(u, f));
    f = v8fAdd(v8fSet(.5f), v8fMul(u, f));
    f = v8fAdd(v8fSet(1.f), v8fMul(u, f));
    f = v8fAdd(v8fSet(1.f), v8fMul(u, f));
    return v8fMul(f, v8iAsF(v8iShl(v8iAdd(i, v8iSet(127)), 23)));
}
/* artfSrgbDecode and artfSrgbEncode (core/color.h), eight lanes at a time: the same polynomials in the same order. */
V8_INLINE V8F srgbDecodeLanes(V8F shown) {
    static const float c[7] = ARTF_SRGB_DECODE;
    V8F p = v8fSet(c[6]);
    for (int k = 5; k >= 0; --k) p = v8fAdd(v8fMul(p, shown), v8fSet(c[k]));
    return v8fSelect(v8fGt(shown, v8fSet(.04045f)), v8fMul(shown, v8fSet(1.f / 12.92f)), p);
}
V8_INLINE V8F srgbEncodeLanes(V8F light) {
    static const float c[6] = ARTF_SRGB_ENCODE;
    const V8F t = v8fSqrt(v8fSqrt(v8fMax(light, v8fSet(0.f))));
    V8F p = v8fSet(c[5]);
    for (int k = 4; k >= 0; --k) p = v8fAdd(v8fMul(p, t), v8fSet(c[k]));
    return v8fSelect(v8fGt(light, v8fSet(.0031308f)), v8fMul(v8fMax(light, v8fSet(0.f)), v8fSet(12.92f)), p);
}
V8_INLINE Lanes3 srgbDecode3(Lanes3 c) {
    const Lanes3 r = {srgbDecodeLanes(c.x), srgbDecodeLanes(c.y), srgbDecodeLanes(c.z)};
    return r;
}

/* The shading model (shading/shading.inc), in lanes: lanes... */
#define S_FN(name) lanes##name
#define S_DEF V8_INLINE
#define S_SELF
#define S_PASS
#define S_F V8F
#define S_V3 Lanes3
#define S_FLAG bool
#define S_WIDE 1
#define S_C(x) v8fSet(x)
#define S_ADD(a, b) v8fAdd(a, b)
#define S_SUB(a, b) v8fSub(a, b)
#define S_MUL(a, b) v8fMul(a, b)
#define S_DIV(a, b) v8fDiv(a, b)
#define S_MIN(a, b) v8fMin(a, b)
#define S_MAX(a, b) v8fMax(a, b)
#define S_SQRT(a) v8fSqrt(a)
#define S_GT(a, b) v8fGt(a, b)
#define S_SELECT(c, a, b) v8fSelect(c, b, a)
#define S_PICK(c, a, b) ((c) ? (a) : (b))
#define S_V3_OF(x, y, z) ((Lanes3){x, y, z})
#define S_X(v) (v).x
#define S_Y(v) (v).y
#define S_Z(v) (v).z
#define S_DOT3(a, b) dot3(a, b)
#define S_NORMALIZE(a) normalize3(a)
#include "shading/shading.inc"

/* Bilinear, repeating, in one mipmap; channels from 0 to 1, and alpha where it is asked for. */
V8_INLINE Lanes3 sampleTexture(const uint32_t* pixels, int width, int height, V8F u, V8F v, V8F* alpha) {
    const V8F one = v8fSet(1.f), half = v8fSet(.5f), wf = v8fSet((float)width), hf = v8fSet((float)height);
    const V8I wi = v8iSet(width), hi = v8iSet(height), zero = v8iSet(0), minusOne = v8iSet(-1);
    V8F x = v8fSub(v8fMul(v8fSub(u, v8fFloor(u)), wf), half);
    V8F y = v8fSub(v8fMul(v8fSub(v, v8fFloor(v)), hf), half);
    V8F xf = v8fFloor(x), yf = v8fFloor(y), tx = v8fSub(x, xf), ty = v8fSub(y, yf);
    V8I x0 = v8fToI(xf), y0 = v8fToI(yf);
    x0 = v8iSelect(v8iEq(x0, minusOne), x0, v8iSub(wi, v8iSet(1)));
    y0 = v8iSelect(v8iEq(y0, minusOne), y0, v8iSub(hi, v8iSet(1)));
    x0 = v8iSelect(v8iGt(x0, v8iSub(wi, v8iSet(1))), x0, zero); /* (rounding can reach the very end) */
    y0 = v8iSelect(v8iGt(y0, v8iSub(hi, v8iSet(1))), y0, zero);
    V8I x1 = v8iAdd(x0, v8iSet(1)), y1 = v8iAdd(y0, v8iSet(1));
    x1 = v8iSelect(v8iEq(x1, wi), x1, zero);
    y1 = v8iSelect(v8iEq(y1, hi), y1, zero);
    V8I row0 = v8iMul(y0, wi), row1 = v8iMul(y1, wi);
    V8I p00 = v8iGather(pixels, v8iAdd(row0, x0)), p10 = v8iGather(pixels, v8iAdd(row0, x1));
    V8I p01 = v8iGather(pixels, v8iAdd(row1, x0)), p11 = v8iGather(pixels, v8iAdd(row1, x1));
    const V8I byteMask = v8iSet(255);
    const V8F scale = v8fSet(1.f / 255.f);
    V8F w00 = v8fMul(v8fSub(one, tx), v8fSub(one, ty)), w10 = v8fMul(tx, v8fSub(one, ty)), w01 = v8fMul(v8fSub(one, tx), ty), w11 = v8fMul(tx, ty);
#define CHANNEL(shift) v8fMul(scale, v8fAdd(v8fAdd(v8fMul(w00, v8iToF(v8iAnd(v8iShr(p00, shift), byteMask))), v8fMul(w10, v8iToF(v8iAnd(v8iShr(p10, shift), byteMask)))), \
                                            v8fAdd(v8fMul(w01, v8iToF(v8iAnd(v8iShr(p01, shift), byteMask))), v8fMul(w11, v8iToF(v8iAnd(v8iShr(p11, shift), byteMask))))))
    Lanes3 r = {CHANNEL(16), CHANNEL(8), CHANNEL(0)};
    if (alpha) *alpha = CHANNEL(24);
#undef CHANNEL
    return r;
}

/* What a surface reflects (shading.inc): its lobe, drawn out or not; how much it reflects (specular); what it reflects
 * head on. */
typedef struct Reflecting {
    lanesLobe lobe;
    V8F strength;
    Lanes3 f0;
    Lanes3 gain; /* what the lobe is multiplied by, for the light it loses (shading.inc's Gain3) */
} Reflecting;

/* How much light a surface gives back toward the eye, for one light: adds to diffuse and specular. The highlight comes
 * from toward (toLight but for a light with a size), times energy. */
V8_INLINE void addLight(Lanes3 n, Lanes3 view, Lanes3 toLight, V8F strength, const float* color, bool shiny, const Reflecting* r, Lanes3* diffuse, Lanes3* highlight) {
    const V8F zero = v8fSet(0.f);
    V8F ndl = v8fMax(dot3(n, toLight), zero);
    V8F amount = v8fMul(ndl, strength);
    V8F cr = v8fSet(color[0]), cg = v8fSet(color[1]), cb = v8fSet(color[2]);
    diffuse->x = v8fAdd(diffuse->x, v8fMul(cr, amount));
    diffuse->y = v8fAdd(diffuse->y, v8fMul(cg, amount));
    diffuse->z = v8fAdd(diffuse->z, v8fMul(cb, amount));
    if (shiny) {
        V8F vdh;
        const V8F lit = lanesHighlight(&r->lobe, r->lobe.drawn, n, view, lanesHalfway(view, toLight), ndl, v8fMul(strength, r->strength), &vdh);
        const Lanes3 fresnel = lanesMul3(lanesSchlick3(r->f0, lanesFifth(vdh)), r->gain);
        highlight->x = v8fAdd(highlight->x, v8fMul(v8fMul(cr, lit), fresnel.x));
        highlight->y = v8fAdd(highlight->y, v8fMul(v8fMul(cg, lit), fresnel.y));
        highlight->z = v8fAdd(highlight->z, v8fMul(v8fMul(cb, lit), fresnel.z));
    }
}

/* A light with a size: its diffuse light from its middle, its highlight from its point nearest the mirror ray, kept as
 * bright in all as a point's. */
V8_INLINE void addLightSized(Lanes3 n, Lanes3 view, Lanes3 toLight, Lanes3 toward, V8F energy, V8F strength, const float* color, bool shiny, const Reflecting* r, Lanes3* diffuse, Lanes3* highlight) {
    const V8F zero = v8fSet(0.f);
    const V8F amount = v8fMul(v8fMax(dot3(n, toLight), zero), strength);
    diffuse->x = v8fAdd(diffuse->x, v8fMul(v8fSet(color[0]), amount));
    diffuse->y = v8fAdd(diffuse->y, v8fMul(v8fSet(color[1]), amount));
    diffuse->z = v8fAdd(diffuse->z, v8fMul(v8fSet(color[2]), amount));
    if (shiny) {
        Lanes3 none = {zero, zero, zero};
        addLight(n, view, toward, v8fMul(strength, energy), color, true, r, &none, highlight);
    }
}

/* The same, called: what has no light with a size keeps its loop as small as it was. */
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void addBall(Lanes3 n, Lanes3 view, Lanes3 d, Lanes3 toLight, float size, V8F rough, V8F strength, const float* color, bool shiny, const Reflecting* r, Lanes3* diffuse, Lanes3* highlight) {
    V8F energy;
    const Lanes3 toward = lanesBallNearest(n, view, d, v8fSet(size), rough, &energy); /* (Karis's nearest point) */
    addLightSized(n, view, toLight, toward, energy, strength, color, shiny, r, diffuse, highlight);
}

/* A clear coat, sheen and light under the surface, for one light (ArtfMaterialMore): what they need of the place, and
 * what they add up. */
typedef struct Extras {
    uint32_t flags;        /* ARTF_MORE_ */
    Lanes3 coatN;          /* the coat's normal: the surface's, but for a map of its own */
    lanesLobe coat;        /* its lobe */
    V8F ndv, sheenInverse; /* the surface's n . v; 1 / sheen's alpha */
    float reach;           /* log2 e / how far light goes under the surface */
    Lanes3 coatLight, sheenLight, under;
} Extras;

/* The coat is addLight's lobe with its own roughness and normal, F0 .04 and no color; sheen is Charlie's
 * (artfShadeCharlie). Under the surface, light wraps past where the surface turns from it, and comes through from
 * behind as much as what it went through leaves (through: 2^-(thickness x reach), 0 when it is not known); strength has
 * the shadow, open not. */
V8_INLINE void addExtras(Extras* x, Lanes3 n, Lanes3 view, Lanes3 toLight, V8F strength, V8F open, V8F through, const float* color) {
    const V8F zero = v8fSet(0.f);
    const V8F cr = v8fSet(color[0]), cg = v8fSet(color[1]), cb = v8fSet(color[2]);
    const Lanes3 h = lanesHalfway(view, toLight);
    const V8F towardLight = dot3(n, toLight), ndl = v8fMax(towardLight, zero);
    if (x->flags & ARTF_MORE_COAT) {
        V8F vdh;
        const V8F cndl = v8fMax(dot3(x->coatN, toLight), zero);
        const V8F lobe = lanesHighlight(&x->coat, false, x->coatN, view, h, cndl, strength, &vdh);
        const V8F lit = v8fMul(lobe, lanesCoatFresnel(lanesFifth(vdh)));
        x->coatLight.x = v8fAdd(x->coatLight.x, v8fMul(cr, lit));
        x->coatLight.y = v8fAdd(x->coatLight.y, v8fMul(cg, lit));
        x->coatLight.z = v8fAdd(x->coatLight.z, v8fMul(cb, lit));
    }
    if (x->flags & ARTF_MORE_SHEEN) {
        _Alignas(32) float lh[8], ll[8], lv[8], li[8], got[8];
        v8fStore(lh, v8fMax(dot3(n, h), zero));
        v8fStore(ll, ndl);
        v8fStore(lv, x->ndv);
        v8fStore(li, x->sheenInverse);
        for (int i = 0; i < 8; ++i) got[i] = artfShadeCharlie(lh[i], ll[i], lv[i], li[i]);
        const V8F grazing = v8fMul(v8fMul(v8fLoad(got), ndl), strength);
        x->sheenLight.x = v8fAdd(x->sheenLight.x, v8fMul(cr, grazing));
        x->sheenLight.y = v8fAdd(x->sheenLight.y, v8fMul(cg, grazing));
        x->sheenLight.z = v8fAdd(x->sheenLight.z, v8fMul(cb, grazing));
    }
    if (x->flags & ARTF_MORE_SUBSURFACE) {
        const V8F lit = v8fAdd(v8fMul(lanesWrap(towardLight, ndl), strength), v8fMul(v8fMul(v8fMax(v8fSub(zero, towardLight), zero), through), open));
        x->under.x = v8fAdd(x->under.x, v8fMul(cr, lit));
        x->under.y = v8fAdd(x->under.y, v8fMul(cg, lit));
        x->under.z = v8fAdd(x->under.z, v8fMul(cb, lit));
    }
}

/* How much of a light gets through what it meets first to eight places, from the depth in its shadow maps: 0 where
 * that is not known. light < 0: the sun. */
static V8F throughLanes(const ArtfSoftFrame* f, int light, Lanes3 world, float reach) {
    _Alignas(32) float wx[8], wy[8], wz[8], out[8];
    v8fStore(wx, world.x); v8fStore(wy, world.y); v8fStore(wz, world.z);
    for (int i = 0; i < 8; ++i) {
        const float p[3] = {wx[i], wy[i], wz[i]};
        const float t = light < 0 ? artfShadowThroughSun(f->shadows, f->data.cameraPos, p) : artfShadowThroughLight(f->shadows, (uint32_t)light, f->data.lights[light].position, p);
        out[i] = t >= 0 ? artfExp2Fast(-t * reach) : 0.f;
    }
    return v8fLoad(out);
}

/* A light with a shape or a profile (artfLightSetShape, artfLightSetProfile), place by place as artfLightPlace: a
 * shape's diffuse light and highlight from all of it (shading.inc's AreaLight), a ball's or a point's diffuse light
 * from its point nearest each and its highlight from its point nearest the mirror ray; both times its profile. A clear
 * coat, sheen and what is under the surface take a shape's point nearest. spot: its cone and shadow, open: its cone
 * alone. Called, as addBall is. */
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void addShaped(const ArtfSoftFrame* f, uint32_t i, Lanes3 n, Lanes3 view, Lanes3 world, V8F rough, V8F spot, V8F open, V8F through, bool shiny, const Reflecting* r,
                      Lanes3* diffuse, Lanes3* highlight, Extras* x) {
    const ArtfFrameData* frame = &f->data;
    const ArtfLightData* L = &frame->lights[i];
    const float(*shape)[4] = frame->lightShapes[i];
    const float* profile = shape[2][3] > 0 ? f->scene->lightProfile[(int)shape[2][3] - 1] : NULL;
    const float ball = frame->lightSizes[i], reach2 = 1.f / (L->radius * L->radius);
    _Alignas(32) float ln[3][8], lv[3][8], lw[3][8], lr[8], td[3][8], th[3][8], ds[8], hs[8], lam[8], gl[8];
    const bool area = shape[0][3] > 0;
    v8fStore(ln[0], n.x); v8fStore(ln[1], n.y); v8fStore(ln[2], n.z);
    v8fStore(lv[0], view.x); v8fStore(lv[1], view.y); v8fStore(lv[2], view.z);
    v8fStore(lw[0], world.x); v8fStore(lw[1], world.y); v8fStore(lw[2], world.z);
    v8fStore(lr, rough);
    for (int k = 0; k < 8; ++k) {
        const float p[3] = {lw[0][k], lw[1][k], lw[2][k]}, nn[3] = {ln[0][k], ln[1][k], ln[2][k]}, vv[3] = {lv[0][k], lv[1][k], lv[2][k]};
        ArtfShapeLit lit;
        artfLightPlace(shape[0], L->position, ball, reach2, p, nn, vv, lr[k], &lit);
        const float fade = artfMaxf(1.f - lit.distance2 * reach2, 0);
        float strength = fade * fade, profiled = 1.f;
        if (profile) {
            const float d[3] = {p[0] - L->position[0], p[1] - L->position[1], p[2] - L->position[2]}, inv = 1.f / sqrtf(artfMaxf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1e-12f));
            const float dir[3] = {d[0] * inv, d[1] * inv, d[2] * inv};
            profiled = artfProfileAt(profile, shape[2], shape[3], dir);
            strength *= profiled;
        }
        for (int c = 0; c < 3; ++c) {
            td[c][k] = lit.toDiffuse[c];
            th[c][k] = lit.toHighlight[c];
        }
        ds[k] = strength * lit.diffuse;
        hs[k] = strength * lit.highlight;
        /* (a shape's own: its fades are in them, the profile not) */
        lam[k] = lit.lambert * profiled;
        gl[k] = lit.glossy * profiled;
    }
    const Lanes3 toLight = {v8fLoad(td[0]), v8fLoad(td[1]), v8fLoad(td[2])}, toward = {v8fLoad(th[0]), v8fLoad(th[1]), v8fLoad(th[2])};
    const V8F lit = v8fLoad(ds), amount = v8fMul(lit, spot), ndl = v8fMax(dot3(n, toLight), v8fSet(0.f));
    if (area) {
        /* all of the shape: its light as it is, and the highlight's Fresnel from where it comes on the whole */
        const V8F shown = v8fMul(v8fLoad(lam), spot);
        diffuse->x = v8fAdd(diffuse->x, v8fMul(v8fSet(L->color[0]), shown));
        diffuse->y = v8fAdd(diffuse->y, v8fMul(v8fSet(L->color[1]), shown));
        diffuse->z = v8fAdd(diffuse->z, v8fMul(v8fSet(L->color[2]), shown));
        if (shiny) {
            const V8F vdh = v8fMax(dot3(view, lanesHalfway(view, toward)), v8fSet(0.f));
            const Lanes3 fresnel = lanesMul3(lanesSchlick3(r->f0, lanesFifth(vdh)), r->gain);
            const V8F shine = v8fMul(v8fMul(v8fLoad(gl), spot), r->strength);
            highlight->x = v8fAdd(highlight->x, v8fMul(v8fMul(v8fSet(L->color[0]), shine), fresnel.x));
            highlight->y = v8fAdd(highlight->y, v8fMul(v8fMul(v8fSet(L->color[1]), shine), fresnel.y));
            highlight->z = v8fAdd(highlight->z, v8fMul(v8fMul(v8fSet(L->color[2]), shine), fresnel.z));
        }
    } else {
        diffuse->x = v8fAdd(diffuse->x, v8fMul(v8fSet(L->color[0]), v8fMul(ndl, amount)));
        diffuse->y = v8fAdd(diffuse->y, v8fMul(v8fSet(L->color[1]), v8fMul(ndl, amount)));
        diffuse->z = v8fAdd(diffuse->z, v8fMul(v8fSet(L->color[2]), v8fMul(ndl, amount)));
        if (shiny) {
            Lanes3 none = {ndl, ndl, ndl};
            addLight(n, view, toward, v8fMul(v8fLoad(hs), spot), L->color, true, r, &none, highlight);
        }
    }
    if (x) addExtras(x, n, view, toLight, amount, v8fMul(lit, open), through, L->color);
}

/* ---- a frame in floats (f->post): light as RGB9E5, three nine-bit mantissas over one exponent ---- */

V8_INLINE V8I pack9e5(Lanes3 c) {
    const V8F zero = v8fSet(0.f), most = v8fSet(65408.f), half = v8fSet(.5f);
    const V8F r = v8fMin(v8fMax(c.x, zero), most), g = v8fMin(v8fMax(c.y, zero), most), b = v8fMin(v8fMax(c.z, zero), most);
    const V8F m = v8fMax(r, v8fMax(g, b));
    /* the shared exponent: floor(log2(m)) + 16, at least 0 */
    V8I e = v8iSub(v8iAnd(v8iShr(v8fAsI(m), 23), v8iSet(255)), v8iSet(111));
    e = v8iSelect(v8iGt(v8iSet(0), e), e, v8iSet(0));
    V8F inverse = v8iAsF(v8iShl(v8iSub(v8iSet(151), e), 23)); /* 2^(24 - e) */
    const V8I over = v8iGt(v8fToI(v8fFloor(v8fAdd(v8fMul(m, inverse), half))), v8iSet(511));
    e = v8iSelect(over, e, v8iAdd(e, v8iSet(1)));
    inverse = v8fSelect(over, inverse, v8fMul(inverse, half));
    const V8I top = v8iSet(511);
#define MANTISSA(x) v8iSelect(v8iGt(v8fToI(v8fFloor(v8fAdd(v8fMul(x, inverse), half))), top), v8fToI(v8fFloor(v8fAdd(v8fMul(x, inverse), half))), top)
    return v8iOr(v8iOr(MANTISSA(r), v8iShl(MANTISSA(g), 9)), v8iOr(v8iShl(MANTISSA(b), 18), v8iShl(e, 27)));
#undef MANTISSA
}

V8_INLINE Lanes3 unpack9e5(V8I v) {
    const V8F scale = v8iAsF(v8iShl(v8iAdd(v8iShr(v, 27), v8iSet(103)), 23)); /* 2^(e - 24) */
    const V8I nine = v8iSet(511);
    const Lanes3 c = {v8fMul(v8iToF(v8iAnd(v, nine)), scale), v8fMul(v8iToF(v8iAnd(v8iShr(v, 9), nine)), scale), v8fMul(v8iToF(v8iAnd(v8iShr(v, 18), nine)), scale)};
    return c;
}

/* A surface for what comes after: its normal, folded onto an octahedron (9 + 9 bits), roughness (5), metal (4), and
 * the share of its light that came from all around (5), which ambient occlusion takes from. */
V8_INLINE V8I packSurface(Lanes3 n, V8F rough, V8F metal, V8F ambient) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f), half = v8fSet(.5f);
    const V8F sum = v8fMax(v8fAdd(v8fAdd(v8fAbs(n.x), v8fAbs(n.y)), v8fAbs(n.z)), v8fSet(1e-20f));
    V8F a = v8fDiv(n.x, sum), b = v8fDiv(n.y, sum);
    const V8I below = v8fLt(n.z, zero);
    const V8F sa = v8fSelect(v8fLt(a, zero), one, v8fSet(-1.f)), sb = v8fSelect(v8fLt(b, zero), one, v8fSet(-1.f));
    const V8F fa = v8fMul(v8fSub(one, v8fAbs(b)), sa), fb = v8fMul(v8fSub(one, v8fAbs(a)), sb);
    a = v8fSelect(below, a, fa);
    b = v8fSelect(below, b, fb);
#define QUANT(x, top) v8fToI(v8fAdd(v8fMul(v8fMin(v8fMax(x, zero), one), v8fSet(top)), half))
    const V8I qa = QUANT(v8fAdd(v8fMul(a, half), half), 511.f), qb = QUANT(v8fAdd(v8fMul(b, half), half), 511.f);
    return v8iOr(v8iOr(qa, v8iShl(qb, 9)), v8iOr(v8iOr(v8iShl(QUANT(rough, 31.f), 18), v8iShl(QUANT(metal, 15.f), 23)), v8iShl(QUANT(ambient, 31.f), 27)));
#undef QUANT
}

/* What lighting gives eight pixels: their color, as the frame keeps it, and with a frame in floats their alpha and
 * surface too. */
typedef struct LitOut { V8I color; V8F alpha; V8I surface, under; } LitOut;
#define ARTF_POST_ALPHA (ARTF_TILE_PIXELS * 4)   /* from a color, in its buffer: its alpha, as a float */
#define ARTF_POST_SURFACE (ARTF_TILE_PIXELS * 8) /* and its surface */
#define ARTF_POST_UNDER (ARTF_TILE_PIXELS * 12)  /* and light under its surface (artfUnderWord): 0, none */

V8_INLINE void storeLit(const ArtfSoftFrame* f, uint32_t* at, LitOut o) {
    v8iStore(at, o.color);
    if (f->post) {
        v8fStore((float*)(at + ARTF_POST_ALPHA), o.alpha);
        v8iStore(at + ARTF_POST_SURFACE, o.surface);
        v8iStore(at + ARTF_POST_UNDER, o.under);
    }
}

/* Narkowicz's fit to ACES: what is very bright rolls off to white. As backgroundToneMap, and the GPU's. */
V8_INLINE V8F toneMap(V8F x) {
    const V8F top = v8fMul(x, v8fAdd(v8fMul(x, v8fSet(2.51f)), v8fSet(.03f)));
    const V8F bottom = v8fAdd(v8fMul(x, v8fAdd(v8fMul(x, v8fSet(2.43f)), v8fSet(.59f))), v8fSet(.14f));
    return v8fMin(v8fDiv(top, bottom), v8fSet(1.f));
}

/* Linear light to a pixel: the frame's colors are sRGB, as screens show them. The GPU does the same. */
V8_INLINE V8I packColor(Lanes3 c) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f), scale = v8fSet(255.f), half = v8fSet(.5f);
    V8I r = v8fToI(v8fAdd(v8fMul(srgbEncodeLanes(v8fMin(v8fMax(c.x, zero), one)), scale), half));
    V8I g = v8fToI(v8fAdd(v8fMul(srgbEncodeLanes(v8fMin(v8fMax(c.y, zero), one)), scale), half));
    V8I b = v8fToI(v8fAdd(v8fMul(srgbEncodeLanes(v8fMin(v8fMax(c.z, zero), one)), scale), half));
    return v8iOr(v8iOr(b, v8iShl(g, 8)), v8iOr(v8iShl(r, 16), v8iSet((int32_t)0xFF000000u)));
}

/* The same for a layer: premultiplied, in the frame's colors, as what it goes over is. */
V8_INLINE V8I packLayer(Lanes3 c, V8F alpha) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f), half = v8fSet(.5f);
    const V8F a = v8fMin(v8fMax(alpha, zero), one), scale = v8fMul(a, v8fSet(255.f));
    V8I r = v8fToI(v8fAdd(v8fMul(srgbEncodeLanes(v8fMin(v8fMax(c.x, zero), one)), scale), half));
    V8I g = v8fToI(v8fAdd(v8fMul(srgbEncodeLanes(v8fMin(v8fMax(c.y, zero), one)), scale), half));
    V8I b = v8fToI(v8fAdd(v8fMul(srgbEncodeLanes(v8fMin(v8fMax(c.z, zero), one)), scale), half));
    return v8iOr(v8iOr(b, v8iShl(g, 8)), v8iOr(v8iShl(r, 16), v8iShl(v8fToI(v8fAdd(scale, half)), 24)));
}

/* Which mipmap: the one whose texels are about a pixel's size here. The scalar work of a whole block. */
static inline int mipLevel(float rho2, int mipCount) {
    uint32_t bits;
    memcpy(&bits, &rho2, 4);
    int e = (int)(bits >> 23) - 127; /* floor(log2(rho2)) */
    int level = (e + 1) >> 1;        /* log2(rho), to the nearest */
    return level < 0 ? 0 : level >= mipCount ? mipCount - 1 : level;
}

/* What shading a triangle needs, worked out once for it, in the tile's coordinates. */
struct ShadeContext {
    uint32_t fade;    /* as ArtfVisible's */
    int originX, originY; /* the tile's corner */
    const ArtfMaterialData* material;
    const ArtfTextureData* texture;
    uint32_t lights;  /* a bit for each light that can reach it */
    bool needWorld;
    bool colored;     /* its mesh has colors */
    bool layered;     /* its instance is a layer */
    bool bumpy;       /* it has a normal map */
    bool turning;     /* it is shiny, and its normals are not all one */
    const ArtfTextureData* sizing; /* what says which mipmap: the first map it has */
    float opacity;
    float tangent[3], bitangent[3]; /* for a normal map: where u and v go, in the world */
    float A[3], B[3], C[3];
    float wp[3][3], nrm[3][3], uv[3][2];
    float col[3][4];  /* in linear light, and alpha */
    uint32_t who[8];  /* in a view: what it is colored by (viewWho), for each lane of a block */
    float drawn[8];   /* its instance's ArtfVisible.drawn, for each lane */
};

/* Pixels waiting for their light: they may come from many triangles of one material. Nothing is
 * cheaper per pixel than a full vector, and a dense mesh's triangles cover two or three pixels each:
 * so what a triangle does alone is only to interpolate, and light goes eight pixels at a time. */
typedef struct LitBatch {
    _Alignas(32) float nx[8], ny[8], nz[8], wx[8], wy[8], wz[8], u[8], v[8], r[8], g[8], b[8], a[8], tx[8], ty[8], tz[8], bx[8], by[8], bz[8];
    uint16_t at[8];
    uint32_t who[8]; /* in a view: as ShadeContext's */
    float drawn[8];  /* as ShadeContext's: each pixel may be of another instance */
    _Alignas(32) float turn[8]; /* the normal's variance over each pixel (turnAt) */
    int count;
    int level;
    uint32_t lights;
    const uint32_t* extra; /* the tile's list of lights past the frame's own; NULL: none */
    bool colored, layered;
    float opacity;
    const ArtfMaterialData* material;
    const ArtfTextureData* texture;
    const ArtfTextureData* textures; /* the scene's */
    bool bumpy;
} LitBatch;

/* The same for the k-th light past the frame's own. */
static V8F shadowLanesExtra(const ArtfSoftFrame* f, uint32_t k, const float* position, Lanes3 world, Lanes3 n) {
    _Alignas(32) float wx[8], wy[8], wz[8], nx[8], ny[8], nz[8], out[8];
    v8fStore(wx, world.x); v8fStore(wy, world.y); v8fStore(wz, world.z);
    v8fStore(nx, n.x); v8fStore(ny, n.y); v8fStore(nz, n.z);
    for (int i = 0; i < 8; ++i) {
        const float p[3] = {wx[i], wy[i], wz[i]}, normal[3] = {nx[i], ny[i], nz[i]};
        out[i] = artfShadowOfExtra(f->shadows, k, position, p, normal);
    }
    return v8fLoad(out);
}

/* How much light gets to eight places, one by one: shadow maps are read at random, so lanes gain little. */
static V8F shadowLanes(const ArtfSoftFrame* f, int light, Lanes3 world, Lanes3 n) {
    _Alignas(32) float wx[8], wy[8], wz[8], nx[8], ny[8], nz[8], out[8];
    v8fStore(wx, world.x); v8fStore(wy, world.y); v8fStore(wz, world.z);
    v8fStore(nx, n.x); v8fStore(ny, n.y); v8fStore(nz, n.z);
    const bool soft = f->scene->shadows.soft;
    /* (soft, with temporal antialiasing: fewer reads, turned by the pixel, which is where the place falls) */
    const bool turning = soft && f->data.post[1][3] > 0;
    const float* m = f->data.viewProj;
    for (int i = 0; i < 8; ++i) {
        const float p[3] = {wx[i], wy[i], wz[i]}, normal[3] = {nx[i], ny[i], nz[i]};
        float eye[3];
        artfEyeOf(&f->data, p, eye);
        int turn = -1;
        if (turning) {
            const float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13], cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
            const float sx = (cx / cw * .5f + .5f) * (float)f->width, sy = (.5f - cy / cw * .5f) * (float)f->height;
            turn = artfShadowTurn((int)sx, (int)sy, (uint32_t)f->data.output[3]);
        }
        out[i] = light < 0 ? artfShadowOfSun(f->shadows, eye, p, normal, soft ? f->data.look[2] : 0.f, turn)
                           : artfShadowOfLight(f->shadows, (uint32_t)light, f->data.lights[light].position, p, normal, soft ? f->data.lightSizes[light] : 0.f, turn);
    }
    return v8fLoad(out);
}

/* The same from rays (ArtfShadowDesc.traced), one by one; a place the light cannot reach anyway is not asked: its
 * light is nothing, whatever it says (strength, of each lane: 0 there). */
static V8F tracedLanes(const ArtfSoftFrame* f, int light, Lanes3 world, Lanes3 n, V8F strength, const float* drawn) {
    _Alignas(32) float wx[8], wy[8], wz[8], nx[8], ny[8], nz[8], reach[8], out[8];
    v8fStore(wx, world.x); v8fStore(wy, world.y); v8fStore(wz, world.z);
    v8fStore(nx, n.x); v8fStore(ny, n.y); v8fStore(nz, n.z);
    v8fStore(reach, strength);
    for (int i = 0; i < 8; ++i) {
        const float p[3] = {wx[i], wy[i], wz[i]}, normal[3] = {nx[i], ny[i], nz[i]};
        out[i] = reach[i] > 0 ? artfTracedShadowDrawn(f->scene, &f->data, p, normal, light, drawn[i]) : 1.f;
    }
    return v8fLoad(out);
}

/* Lights sampled: of each lane, the weight of the light it took, where it took this one (code: a frame's light, or
 * 0x80000000 | the place of one past them); 0 elsewhere. */
static V8F sampledWeight(const uint32_t* chosen, const float* weight, uint32_t code) {
    _Alignas(32) float w[8];
    for (int l = 0; l < 8; ++l) w[l] = chosen[l] == code ? weight[l] : 0.f;
    return v8fLoad(w);
}

/* The same toward one of the lights past the frame's own (no size), for those that cast. */
static V8F tracedLanesAt(const ArtfSoftFrame* f, const float* at, Lanes3 world, Lanes3 n, V8F strength, const float* drawn) {
    _Alignas(32) float wx[8], wy[8], wz[8], nx[8], ny[8], nz[8], reach[8], out[8];
    v8fStore(wx, world.x); v8fStore(wy, world.y); v8fStore(wz, world.z);
    v8fStore(nx, n.x); v8fStore(ny, n.y); v8fStore(nz, n.z);
    v8fStore(reach, strength);
    for (int i = 0; i < 8; ++i) {
        const float p[3] = {wx[i], wy[i], wz[i]}, normal[3] = {nx[i], ny[i], nz[i]};
        out[i] = reach[i] > 0 ? artfTracedShadowAt(f->scene, &f->data, p, normal, at, 0.f, drawn[i]) : 1.f;
    }
    return v8fLoad(out);
}
/* Light that bounced, at eight places: artfIndirectAt's arithmetic, in the same order, eight at a time. */
/* mirror: also, into reflected, what the same probes give toward that direction, for what a surface reflects. */
/* Of volume `cascade` (corner, spacing, wrap: artfIndirectCascade), as cascadeAt. */
static Lanes3 cascadeLanes(const ArtfIndirect* g, int cascade, const float* origin, const float* spacing, const int* wrap, Lanes3 p, Lanes3 n, const Lanes3* mirror, Lanes3* reflected) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f);
    V8F q[3], w[3];
    V8I i0[3];
    const V8F pk[3] = {p.x, p.y, p.z}, nk[3] = {n.x, n.y, n.z};
    for (int k = 0; k < 3; ++k) {
        q[k] = v8fAdd(pk[k], v8fMul(v8fMul(nk[k], v8fSet(spacing[k])), v8fSet(.25f)));
        const V8F at = v8fMin(v8fMax(v8fDiv(v8fSub(q[k], v8fSet(origin[k])), v8fSet(spacing[k])), zero), v8fSet((float)(g->dims[k] - 1)));
        V8I i = v8fToI(at);
        i = v8iSelect(v8iGt(i, v8iSet(g->dims[k] - 2)), i, v8iSet(g->dims[k] - 2));
        i0[k] = i;
        w[k] = v8fSub(at, v8iToF(i));
    }
    const uint32_t count = artfIndirectCount(g);
    const uint32_t* bits = (const uint32_t*)g->probes;
    V8F sum[3] = {zero, zero, zero}, mirrored[3] = {zero, zero, zero}, weights = zero;
    for (int c = 0; c < 8; ++c) {
        const V8I x = v8iAdd(i0[0], v8iSet(c & 1)), y = v8iAdd(i0[1], v8iSet(c >> 1 & 1)), z = v8iAdd(i0[2], v8iSet(c >> 2));
        V8F weight = v8fMul(v8fMul(c & 1 ? w[0] : v8fSub(one, w[0]), c & 2 ? w[1] : v8fSub(one, w[1])), c & 4 ? w[2] : v8fSub(one, w[2]));
        /* (where each is kept: turned by wrap, after the volumes before: artfIndirectSlotOf) */
        V8I sx = v8iAdd(x, v8iSet(wrap[0])), sy = v8iAdd(y, v8iSet(wrap[1])), sz = v8iAdd(z, v8iSet(wrap[2]));
        sx = v8iSelect(v8iGt(sx, v8iSet(g->dims[0] - 1)), sx, v8iSub(sx, v8iSet(g->dims[0])));
        sy = v8iSelect(v8iGt(sy, v8iSet(g->dims[1] - 1)), sy, v8iSub(sy, v8iSet(g->dims[1])));
        sz = v8iSelect(v8iGt(sz, v8iSet(g->dims[2] - 1)), sz, v8iSub(sz, v8iSet(g->dims[2])));
        const V8I index = v8iAdd(v8iAdd(v8iMul(v8iAdd(v8iMul(sz, v8iSet(g->dims[1])), sy), v8iSet(g->dims[0])), sx), v8iSet((int32_t)((uint32_t)cascade * artfIndirectPer(g))));
        const V8I base = v8iMul(index, v8iSet(16));
#define PROBE(j) v8iAsF(v8iGather(bits, v8iAdd(base, v8iSet(j))))
        weight = v8fMul(weight, PROBE(3)); /* (none from one inside something) */
        if (!g->desc.throughWalls) {
            const V8F there[3] = {v8fAdd(v8fSet(origin[0]), v8fMul(v8iToF(x), v8fSet(spacing[0]))), v8fAdd(v8fSet(origin[1]), v8fMul(v8iToF(y), v8fSet(spacing[1]))),
                                  v8fAdd(v8fSet(origin[2]), v8fMul(v8iToF(z), v8fSet(spacing[2])))};
            const V8F d0 = v8fSub(q[0], there[0]), d1 = v8fSub(q[1], there[1]), d2 = v8fSub(q[2], there[2]);
            const V8F distance = v8fSqrt(v8fAdd(v8fAdd(v8fMul(d0, d0), v8fMul(d1, d1)), v8fMul(d2, d2)));
            const V8F inv = v8fSelect(v8fGt(distance, v8fSet(1e-6f)), zero, v8fDiv(one, distance));
            const V8F facing = v8fMul(v8fSub(one, v8fMul(v8fAdd(v8fAdd(v8fMul(d0, nk[0]), v8fMul(d1, nk[1])), v8fMul(d2, nk[2])), inv)), v8fSet(.5f));
            weight = v8fMul(weight, v8fAdd(v8fMul(facing, facing), v8fSet(.2f)));
            /* as artfProbeFold, and the nearest texel */
            const V8F total = v8fAdd(v8fAdd(v8fAdd(v8fAbs(d0), v8fAbs(d1)), v8fAbs(d2)), v8fSet(1e-20f));
            V8F a = v8fDiv(d0, total), b = v8fDiv(d1, total);
            const V8I below = v8fLt(d2, zero);
            const V8F sa = v8fSelect(v8fLt(a, zero), one, v8fSet(-1.f)), sb = v8fSelect(v8fLt(b, zero), one, v8fSet(-1.f));
            const V8F fa = v8fMul(v8fSub(one, v8fAbs(b)), sa), fb = v8fMul(v8fSub(one, v8fAbs(a)), sb);
            a = v8fSelect(below, a, fa);
            b = v8fSelect(below, b, fb);
            const V8F u = v8fAdd(v8fMul(a, v8fSet(.5f)), v8fSet(.5f)), v = v8fAdd(v8fMul(b, v8fSet(.5f)), v8fSet(.5f));
            V8I tx = v8fToI(v8fMul(u, v8fSet((float)ARTF_PROBE_SIDE))), ty = v8fToI(v8fMul(v, v8fSet((float)ARTF_PROBE_SIDE)));
            tx = v8iSelect(v8iGt(tx, v8iSet(ARTF_PROBE_SIDE - 1)), tx, v8iSet(ARTF_PROBE_SIDE - 1));
            ty = v8iSelect(v8iGt(ty, v8iSet(ARTF_PROBE_SIDE - 1)), ty, v8iSet(ARTF_PROBE_SIDE - 1));
            const V8I texel = v8iAdd(v8iAdd(v8iSet((int32_t)(count * 16)), v8iMul(index, v8iSet(ARTF_PROBE_MAP))), v8iMul(v8iAdd(v8iMul(ty, v8iSet(ARTF_PROBE_SIDE)), tx), v8iSet(2)));
            const V8F mean = v8iAsF(v8iGather(bits, texel)), square = v8iAsF(v8iGather(bits, v8iAdd(texel, v8iSet(1))));
            const V8F variance = v8fMax(v8fSub(square, v8fMul(mean, mean)), v8fSet(1e-4f)), past = v8fSub(distance, mean);
            const V8F seen = v8fDiv(variance, v8fAdd(variance, v8fMul(past, past)));
            const V8F hidden = v8fMax(v8fMul(v8fMul(seen, seen), seen), v8fSet(.002f));
            weight = v8fSelect(v8fGt(distance, mean), weight, v8fMul(weight, hidden));
        }
        weights = v8fAdd(weights, weight);
        for (int k = 0; k < 3; ++k) {
            const V8F taken = v8fAdd(v8fAdd(v8fAdd(PROBE(k), v8fMul(PROBE(4 + 4 * k), nk[0])), v8fMul(PROBE(5 + 4 * k), nk[1])), v8fMul(PROBE(6 + 4 * k), nk[2]));
            sum[k] = v8fAdd(sum[k], v8fMul(weight, v8fMax(taken, zero)));
            if (mirror) {
                const V8F toward = v8fAdd(v8fAdd(v8fAdd(PROBE(k), v8fMul(PROBE(4 + 4 * k), mirror->x)), v8fMul(PROBE(5 + 4 * k), mirror->y)), v8fMul(PROBE(6 + 4 * k), mirror->z));
                mirrored[k] = v8fAdd(mirrored[k], v8fMul(weight, v8fMax(toward, zero)));
            }
        }
#undef PROBE
    }
    const V8F scale = v8fSelect(v8fGt(weights, v8fSet(1e-6f)), zero, v8fDiv(one, weights));
    const Lanes3 out = {v8fMul(sum[0], scale), v8fMul(sum[1], scale), v8fMul(sum[2], scale)};
    if (mirror) {
        reflected->x = v8fMul(mirrored[0], scale);
        reflected->y = v8fMul(mirrored[1], scale);
        reflected->z = v8fMul(mirrored[2], scale);
    }
    return out;
}

/* Light that bounced, at eight places: artfIndirectAt's arithmetic, in the same order, eight at a time, the volumes in
 * turn (cascades), each lane taking from those that hold it. */
/* mirror: also, into reflected, what the same probes give toward that direction, for what a surface reflects. */
static Lanes3 indirectLanes(const ArtfIndirect* g, Lanes3 p, Lanes3 n, const Lanes3* mirror, Lanes3* reflected) {
    float origin[3], spacing[3];
    int wrap[3];
    if (g->cascades <= 1) {
        artfIndirectCascade(g, 0, origin, spacing, wrap);
        return cascadeLanes(g, 0, origin, spacing, wrap, p, n, mirror, reflected);
    }
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f);
    Lanes3 out = {zero, zero, zero}, mirrored = {zero, zero, zero};
    V8F left = one;
    const V8F pk[3] = {p.x, p.y, p.z}, nk[3] = {n.x, n.y, n.z};
    for (int k = 0; k < g->cascades; ++k) {
        artfIndirectCascade(g, k, origin, spacing, wrap);
        V8F take = left;
        if (k < g->cascades - 1) {
            /* (artfIndirectInside) */
            V8F least = v8fSet(1e30f);
            for (int a = 0; a < 3; ++a) {
                const V8F q = v8fAdd(pk[a], v8fMul(v8fMul(nk[a], v8fSet(spacing[a])), v8fSet(.25f)));
                const V8F at = v8fDiv(v8fSub(q, v8fSet(origin[a])), v8fSet(spacing[a]));
                least = v8fMin(least, v8fMin(at, v8fSub(v8fSet((float)(g->dims[a] - 1)), at)));
            }
            take = v8fMul(left, v8fMin(v8fMax(v8fSub(least, v8fSet(.5f)), zero), one));
        }
        if (!v8iSigns(v8fGt(take, zero))) continue;
        Lanes3 r = {zero, zero, zero};
        const Lanes3 got = cascadeLanes(g, k, origin, spacing, wrap, p, n, mirror, &r);
        out.x = v8fAdd(out.x, v8fMul(take, got.x));
        out.y = v8fAdd(out.y, v8fMul(take, got.y));
        out.z = v8fAdd(out.z, v8fMul(take, got.z));
        if (mirror) {
            mirrored.x = v8fAdd(mirrored.x, v8fMul(take, r.x));
            mirrored.y = v8fAdd(mirrored.y, v8fMul(take, r.y));
            mirrored.z = v8fAdd(mirrored.z, v8fMul(take, r.z));
        }
        left = v8fSub(left, take);
    }
    if (mirror) *reflected = mirrored;
    return out;
}

/* What a view shows of eight pixels (ArtfView), as light: the frame shows it by sRGB. As the GPU's pixel shader,
 * in the same order. who: of each pixel, what it is colored by (viewWho); lights: how many reach it. */
static Lanes3 viewLanes(const ArtfSoftFrame* f, const ArtfFrameData* frame, const Lanes3* base, Lanes3 n, V8F rough, V8F metal, const Lanes3* around, const Lanes3* own, Lanes3 world,
                        const uint32_t* who, V8F lights) {
    const V8F half = v8fSet(.5f);
    Lanes3 d = {rough, rough, rough};
    _Alignas(32) float lw[3][8], out[3][8], counts[8];
    switch (f->view) {
    case ARTF_VIEW_BASE_COLOR: return *base;
    case ARTF_VIEW_AMBIENT: return *around;
    case ARTF_VIEW_EMISSIVE: return *own;
    case ARTF_VIEW_NORMALS:
        d.x = v8fAdd(v8fMul(n.x, half), half);
        d.y = v8fAdd(v8fMul(n.y, half), half);
        d.z = v8fAdd(v8fMul(n.z, half), half);
        break;
    case ARTF_VIEW_ROUGHNESS: break;
    case ARTF_VIEW_METALLIC: d.x = d.y = d.z = metal; break;
    default:
        v8fStore(lw[0], world.x); v8fStore(lw[1], world.y); v8fStore(lw[2], world.z);
        v8fStore(counts, lights);
        for (int i = 0; i < 8; ++i) {
            float c[3] = {0, 0, 0};
            const float x = lw[0][i], y = lw[1][i], z = lw[2][i];
            const float* m = frame->viewProj;
            if (f->view == ARTF_VIEW_LOD) artfViewLevel(who[i], c);
            else if (f->view == ARTF_VIEW_MESHLETS || f->view == ARTF_VIEW_INSTANCES) artfViewColor(who[i], c);
            else if (f->view == ARTF_VIEW_LIGHTS) artfViewHeat((uint32_t)counts[i], c);
            else if (f->view == ARTF_VIEW_DEPTH) {
                const float w = m[3] * x + m[7] * y + m[11] * z + m[15];
                c[0] = c[1] = c[2] = ARTF_VIEW_DEPTH_HALF / (ARTF_VIEW_DEPTH_HALF + artfMaxf(w, 0.f));
            } else if (f->view == ARTF_VIEW_MOTION) {
                /* where it is, and where it was: through its instance's motion and the frame before's view */
                float p[3] = {x, y, z};
                if (f->motion) {
                    const float* k = f->motion + (size_t)who[i] * 12;
                    for (int r = 0; r < 3; ++r) p[r] = k[r * 4] * x + k[r * 4 + 1] * y + k[r * 4 + 2] * z + k[r * 4 + 3];
                }
                const float* q = frame->previous;
                const float cw = artfMaxf(m[3] * x + m[7] * y + m[11] * z + m[15], 1e-5f), pw = artfMaxf(q[3] * p[0] + q[7] * p[1] + q[11] * p[2] + q[15], 1e-5f);
                const float nx = (m[0] * x + m[4] * y + m[8] * z + m[12]) / cw, ny = (m[1] * x + m[5] * y + m[9] * z + m[13]) / cw;
                const float px = (q[0] * p[0] + q[4] * p[1] + q[8] * p[2] + q[12]) / pw, py = (q[1] * p[0] + q[5] * p[1] + q[9] * p[2] + q[13]) / pw;
                /* (in pixels, y down) */
                const float vx = (nx - px) * .5f * frame->fog[3], vy = (py - ny) * .5f * frame->eye[1][3];
                c[0] = artfClampf(ARTF_VIEW_MOTION_STILL + vx * ARTF_VIEW_MOTION_SCALE, 0, 1);
                c[1] = artfClampf(ARTF_VIEW_MOTION_STILL + vy * ARTF_VIEW_MOTION_SCALE, 0, 1);
                c[2] = ARTF_VIEW_MOTION_STILL;
            }
            for (int k = 0; k < 3; ++k) out[k][i] = c[k];
        }
        d.x = v8fLoad(out[0]);
        d.y = v8fLoad(out[1]);
        d.z = v8fLoad(out[2]);
        break;
    }
    return srgbDecode3(d); /* (light, which the frame shows as d) */
}

/* The color of eight pixels. A light out of reach adds exactly nothing, so which lights are looked
 * at changes the time this takes, never the pixels. */
/* (reflecting is read only where shiny, coating only where coated: GCC cannot see that through the lanes, and zeroing
 * them for the eight pixels of every call costs a quarter of a plain frame) */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
/* coated: a clear coat or sheen. It is made twice, with and without (lightLanes): what has neither pays nothing for
 * them, not even the registers. */
V8_INLINE LitOut lightLanesAs(const ArtfSoftFrame* f, const ArtfFrameData* frame, const ArtfMaterialData* material, const ArtfTextureData* textures, const ArtfTextureData* texture, uint32_t lights, const uint32_t* extra, int level, V8F turn, Lanes3 rawN, Lanes3 world, V8F u, V8F v,
                              bool colored, bool layered, Lanes3 tint, V8F alpha, const Lanes3* tangent, const Lanes3* bitangent, const uint32_t* who, const float* drawn, const bool coated, const bool sized,
                              const bool viewed) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f);
    V8F lightsHere = zero; /* (in a view: how many lights reach each pixel) */
    const bool shiny = material->specular > 0 || material->metallic > 0 || material->graph;
    const ArtfMaterialMore* more = &f->scene->materialsMore.items[material - f->scene->materials.items];
    Lanes3 n = normalize3(rawN);

    /* what it is made of, here */
#define MAP_LEVEL(t) artfMini(artfMaxi(level, (t)->residentMip), (t)->mipCount - 1)
#define SAMPLE(t, out) sampleTexture((t)->pixels + (t)->offsets[MAP_LEVEL(t)], artfMaxi((t)->width >> MAP_LEVEL(t), 1), artfMaxi((t)->height >> MAP_LEVEL(t), 1), u, v, out)
    Lanes3 albedo = {v8fSet(material->baseColor[0]), v8fSet(material->baseColor[1]), v8fSet(material->baseColor[2])};
    if (texture) {
        V8F texel = v8fSet(1.f);
        Lanes3 t = SAMPLE(texture, material->flags & ARTF_MATERIAL_BLEND ? &texel : NULL);
        alpha = v8fMul(alpha, texel);
        albedo.x = v8fMul(albedo.x, srgbDecodeLanes(t.x));
        albedo.y = v8fMul(albedo.y, srgbDecodeLanes(t.y));
        albedo.z = v8fMul(albedo.z, srgbDecodeLanes(t.z));
    }
    if (colored) {
        albedo.x = v8fMul(albedo.x, tint.x);
        albedo.y = v8fMul(albedo.y, tint.y);
        albedo.z = v8fMul(albedo.z, tint.z);
    }
    V8F rough = v8fSet(material->roughness), metal = v8fSet(material->metallic), open = one;
    if (material->surfaceMap) {
        const ArtfTextureData* t = &textures[material->surfaceMap];
        Lanes3 m = SAMPLE(t, NULL);
        rough = v8fMax(v8fMul(rough, m.y), v8fSet(.045f));
        metal = v8fMul(metal, m.z);
    }
    if (material->occlusionMap) {
        const ArtfTextureData* t = &textures[material->occlusionMap];
        Lanes3 m = SAMPLE(t, NULL);
        open = v8fAdd(one, v8fMul(v8fSub(m.x, one), v8fSet(material->occlusion)));
    }
    if (material->normalMap) {
        const ArtfTextureData* t = &textures[material->normalMap];
        Lanes3 m = SAMPLE(t, NULL);
        const V8F scale = v8fSet(material->normalScale), two = v8fSet(2.f);
        /* (v goes down the picture, and the map's +y up it) */
        const V8F nx = v8fMul(v8fSub(v8fMul(m.x, two), one), scale), ny = v8fMul(v8fSub(one, v8fMul(m.y, two)), scale), nz = v8fSub(v8fMul(m.z, two), one);
        Lanes3 bent = {v8fAdd(v8fAdd(v8fMul(tangent->x, nx), v8fMul(bitangent->x, ny)), v8fMul(n.x, nz)),
                       v8fAdd(v8fAdd(v8fMul(tangent->y, nx), v8fMul(bitangent->y, ny)), v8fMul(n.y, nz)),
                       v8fAdd(v8fAdd(v8fMul(tangent->z, nx), v8fMul(bitangent->z, ny)), v8fMul(n.z, nz))};
        n = normalize3(bent);
        if (shiny && artfWidenHighlights) turn = v8fAdd(turn, lanesMapVariance(m.x, m.y, m.z));
    }
    /* past glTF's core (ArtfMaterialMore): the coat's and sheen's own, and their textures */
    const uint32_t extraFlags = coated ? more->flags : 0u;
    V8F coatAmount = zero, coatRough = zero, sheenRough = zero;
    Lanes3 sheenColor = {zero, zero, zero}, coatN = n;
    bool coatBent = false;
    if (coated) {
        coatAmount = v8fSet(more->coat);
        coatRough = v8fSet(more->coatRoughness);
        sheenRough = v8fSet(more->sheenRoughness);
        sheenColor.x = v8fSet(more->sheen[0]);
        sheenColor.y = v8fSet(more->sheen[1]);
        sheenColor.z = v8fSet(more->sheen[2]);
    }
    /* (KHR_materials_specular's textures: how much, and of what color; white when none) */
    Lanes3 specularTint = {one, one, one};
    if ((extraFlags & ARTF_MORE_SPECULAR) && ARTF_MORE_SPECULAR_MAPS(more)) {
        if (more->specularColorMap) {
            const Lanes3 t = SAMPLE(&textures[more->specularColorMap], NULL);
            specularTint.x = srgbDecodeLanes(t.x);
            specularTint.y = srgbDecodeLanes(t.y);
            specularTint.z = srgbDecodeLanes(t.z);
        }
        if (more->specularMap) {
            V8F texel = one;
            SAMPLE(&textures[more->specularMap], &texel);
            specularTint.x = v8fMul(specularTint.x, texel);
            specularTint.y = v8fMul(specularTint.y, texel);
            specularTint.z = v8fMul(specularTint.z, texel);
        }
    }
    /* (a thin film's amount and thickness, and the drawn highlights' direction and strength: their textures') */
    V8F filmAmount = v8fSet(more->iridescence), filmThickness = v8fSet(more->iridescenceThickness);
    V8F drawnC = v8fSet(more->anisotropyDirection[0]), drawnS = v8fSet(more->anisotropyDirection[1]), drawnStrength = v8fSet(more->anisotropy);
    if ((extraFlags & ARTF_MORE_IRIDESCENCE) && ARTF_MORE_FILM_MAPS(more)) {
        if (more->iridescenceMap) filmAmount = v8fMul(filmAmount, SAMPLE(&textures[more->iridescenceMap], NULL).x);
        if (more->iridescenceThicknessMap) {
            const V8F least = v8fSet(more->iridescenceThicknessMin);
            filmThickness = v8fAdd(least, v8fMul(v8fSub(filmThickness, least), SAMPLE(&textures[more->iridescenceThicknessMap], NULL).y));
        }
    }
    if ((extraFlags & ARTF_MORE_ANISOTROPY) && more->anisotropyMap) {
        const Lanes3 t = SAMPLE(&textures[more->anisotropyMap], NULL);
        const V8F two = v8fSet(2.f);
        V8F dx = v8fSub(v8fMul(t.x, two), one), dy = v8fSub(v8fMul(t.y, two), one);
        const V8F inverse = v8fDiv(one, v8fSqrt(v8fMax(v8fAdd(v8fMul(dx, dx), v8fMul(dy, dy)), v8fSet(1e-20f))));
        dx = v8fMul(dx, inverse);
        dy = v8fMul(dy, inverse);
        drawnC = v8fSub(v8fMul(v8fSet(more->anisotropyDirection[0]), dx), v8fMul(v8fSet(more->anisotropyDirection[1]), dy));
        drawnS = v8fAdd(v8fMul(v8fSet(more->anisotropyDirection[1]), dx), v8fMul(v8fSet(more->anisotropyDirection[0]), dy));
        drawnStrength = v8fMul(drawnStrength, t.z);
    }
    if (extraFlags & ARTF_MORE_MAPS) {
        if (more->coatMap) coatAmount = v8fMul(coatAmount, SAMPLE(&textures[more->coatMap], NULL).x);
        if (more->coatRoughnessMap) coatRough = v8fMax(v8fMul(coatRough, SAMPLE(&textures[more->coatRoughnessMap], NULL).y), v8fSet(.045f));
        if (more->sheenMap) {
            const Lanes3 t = SAMPLE(&textures[more->sheenMap], NULL);
            sheenColor.x = v8fMul(sheenColor.x, srgbDecodeLanes(t.x));
            sheenColor.y = v8fMul(sheenColor.y, srgbDecodeLanes(t.y));
            sheenColor.z = v8fMul(sheenColor.z, srgbDecodeLanes(t.z));
        }
        if (more->sheenRoughnessMap) {
            V8F texel = one;
            SAMPLE(&textures[more->sheenRoughnessMap], &texel);
            sheenRough = v8fMax(v8fMul(sheenRough, texel), v8fSet(.07f));
        }
        if (more->coatNormalMap) {
            /* (bent from the surface as it is, not as its own normal map bends it) */
            const Lanes3 m = SAMPLE(&textures[more->coatNormalMap], NULL), plain = normalize3(rawN);
            const V8F scale = v8fSet(more->coatNormalScale), two = v8fSet(2.f);
            const V8F nx = v8fMul(v8fSub(v8fMul(m.x, two), one), scale), ny = v8fMul(v8fSub(one, v8fMul(m.y, two)), scale), nz = v8fSub(v8fMul(m.z, two), one);
            const Lanes3 bent = {v8fAdd(v8fAdd(v8fMul(tangent->x, nx), v8fMul(bitangent->x, ny)), v8fMul(plain.x, nz)),
                                 v8fAdd(v8fAdd(v8fMul(tangent->y, nx), v8fMul(bitangent->y, ny)), v8fMul(plain.y, nz)),
                                 v8fAdd(v8fAdd(v8fMul(tangent->z, nx), v8fMul(bitangent->z, ny)), v8fMul(plain.z, nz))};
            coatN = normalize3(bent);
            coatBent = true;
        }
    }
#undef SAMPLE
#undef MAP_LEVEL

    /* a material of the program's own, pixel by pixel */
    Lanes3 own = {v8fSet(material->emissive[0]), v8fSet(material->emissive[1]), v8fSet(material->emissive[2])};
    if (material->graph) {
        _Alignas(32) ArtfGraphLanes io;
        v8fStore(io.u, u); v8fStore(io.v, v);
        v8fStore(io.world[0], world.x); v8fStore(io.world[1], world.y); v8fStore(io.world[2], world.z);
        v8fStore(io.normalIn[0], n.x); v8fStore(io.normalIn[1], n.y); v8fStore(io.normalIn[2], n.z);
        v8fStore(io.normal[0], n.x); v8fStore(io.normal[1], n.y); v8fStore(io.normal[2], n.z);
        v8fStore(io.base[0], albedo.x); v8fStore(io.base[1], albedo.y); v8fStore(io.base[2], albedo.z);
        v8fStore(io.color[0], albedo.x); v8fStore(io.color[1], albedo.y); v8fStore(io.color[2], albedo.z);
        v8fStore(io.emissive[0], own.x); v8fStore(io.emissive[1], own.y); v8fStore(io.emissive[2], own.z);
        v8fStore(io.roughness, rough); v8fStore(io.metallic, metal); v8fStore(io.alpha, one);
        for (int i = 0; i < 8; ++i) {
            const float at[3] = {io.world[0][i], io.world[1][i], io.world[2][i]};
            float eye[3];
            artfEyeOf(frame, at, eye);
            const float dx = eye[0] - io.world[0][i], dy = eye[1] - io.world[1][i], dz = eye[2] - io.world[2][i], inverse = 1.f / sqrtf(artfMaxf(dx * dx + dy * dy + dz * dz, 1e-12f));
            io.toEye[0][i] = dx * inverse;
            io.toEye[1][i] = dy * inverse;
            io.toEye[2][i] = dz * inverse;
        }
        artfGraphRunLanes(f->scene, &f->scene->graphs.items[material->graph - 1], material, &io);
        n.x = v8fLoad(io.normal[0]); n.y = v8fLoad(io.normal[1]); n.z = v8fLoad(io.normal[2]);
        albedo.x = v8fLoad(io.color[0]); albedo.y = v8fLoad(io.color[1]); albedo.z = v8fLoad(io.color[2]);
        rough = v8fLoad(io.roughness); metal = v8fLoad(io.metallic); alpha = v8fMul(alpha, v8fLoad(io.alpha));
        own.x = v8fLoad(io.emissive[0]); own.y = v8fLoad(io.emissive[1]); own.z = v8fLoad(io.emissive[2]);
    }

    /* decals over it, in the order they were added: not on what blends */
    if (f->decalCount && !(material->flags & ARTF_MATERIAL_BLEND)) {
        const V8F half = v8fSet(.5f);
        for (uint32_t k = 0; k < f->decalCount; ++k) {
            const ArtfDecalData* d = &f->decals[k];
            const float* m = d->toBox;
            const V8F lx = v8fAdd(v8fAdd(v8fMul(world.x, v8fSet(m[0])), v8fMul(world.y, v8fSet(m[1]))), v8fAdd(v8fMul(world.z, v8fSet(m[2])), v8fSet(m[3])));
            const V8F ly = v8fAdd(v8fAdd(v8fMul(world.x, v8fSet(m[4])), v8fMul(world.y, v8fSet(m[5]))), v8fAdd(v8fMul(world.z, v8fSet(m[6])), v8fSet(m[7])));
            const V8F lz = v8fAdd(v8fAdd(v8fMul(world.x, v8fSet(m[8])), v8fMul(world.y, v8fSet(m[9]))), v8fAdd(v8fMul(world.z, v8fSet(m[10])), v8fSet(m[11])));
            const V8I in = v8iAnd(v8iAnd(v8fLt(v8fAbs(lx), half), v8fLt(v8fAbs(ly), half)), v8fLt(v8fAbs(lz), half));
            if (v8iSigns(in) == 0) continue;
            /* facing up the box, and away from its top and bottom */
            const float upLength = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]), inv = upLength > 0 ? 1.f / upLength : 0.f;
            const Lanes3 up = {v8fSet(m[4] * inv), v8fSet(m[5] * inv), v8fSet(m[6] * inv)};
            const V8F facing = v8fMin(v8fMax(v8fMul(v8fSub(dot3(n, up), v8fSet(.2f)), v8fSet(1.f / .3f)), zero), one);
            const V8F y2 = v8fMul(v8fMul(ly, ly), v8fSet(4.f)), fade = v8fMax(v8fSub(one, v8fMul(y2, y2)), zero);
            const ArtfTextureData* t = &textures[d->texture];
            const int level = (int)d->level;
            V8F texel = one;
            const Lanes3 c = sampleTexture(t->pixels + t->offsets[level], artfMaxi(t->width >> level, 1), artfMaxi(t->height >> level, 1), v8fAdd(lx, half), v8fAdd(lz, half), &texel);
            const V8F cover = v8fSelect(in, zero, v8fMul(v8fMul(texel, v8fSet(d->opacity)), v8fMul(facing, fade)));
            albedo.x = v8fAdd(albedo.x, v8fMul(v8fSub(v8fMul(v8fSet(d->color[0]), srgbDecodeLanes(c.x)), albedo.x), cover));
            albedo.y = v8fAdd(albedo.y, v8fMul(v8fSub(v8fMul(v8fSet(d->color[1]), srgbDecodeLanes(c.y)), albedo.y), cover));
            albedo.z = v8fAdd(albedo.z, v8fMul(v8fSub(v8fMul(v8fSet(d->color[2]), srgbDecodeLanes(c.z)), albedo.z), cover));
            if (d->roughness > 0) rough = v8fAdd(rough, v8fMul(v8fSub(v8fSet(d->roughness), rough), cover));
        }
    }

    /* a highlight as wide as the normal turns over the pixel (shading.inc's Widened): where it turns at all */
    if (shiny && v8iSigns(v8fGt(turn, zero))) rough = lanesWidened(rough, turn);
    const Lanes3 baseColor = albedo; /* (what an unlit material shows) */
    /* light from all around, then the sun */
    V8F up = v8fAdd(v8fMul(n.y, v8fSet(.5f)), v8fSet(.5f));
    Lanes3 diffuse = {lanesSkyGround(v8fSet(frame->ground[0]), v8fSet(frame->sky[0] - frame->ground[0]), up),
                      lanesSkyGround(v8fSet(frame->ground[1]), v8fSet(frame->sky[1] - frame->ground[1]), up),
                      lanesSkyGround(v8fSet(frame->ground[2]), v8fSet(frame->sky[2] - frame->ground[2]), up)};
    /* or from a picture of all around: lane by lane, as the GPU does it */
    const float* environment = frame->sky[3] > 0 ? f->scene->environment : NULL;
    _Alignas(32) float laneN[3][8];
    if (environment) {
        _Alignas(32) float got[3][8];
        v8fStore(laneN[0], n.x); v8fStore(laneN[1], n.y); v8fStore(laneN[2], n.z);
        for (int i = 0; i < 8; ++i) {
            float c[3];
            artfEnvSample(environment, ARTF_ENV_DIFFUSE, laneN[0][i], laneN[1][i], laneN[2][i], c);
            for (int k = 0; k < 3; ++k) got[k][i] = c[k] * frame->sky[3];
        }
        diffuse.x = v8fLoad(got[0]); diffuse.y = v8fLoad(got[1]); diffuse.z = v8fLoad(got[2]);
    }
    /* (what light that bounced gives a shiny surface in its mirror direction: what it reflects of it) */
    Lanes3 bounced = {v8fSet(0.f), v8fSet(0.f), v8fSet(0.f)};
    if (frame->indirect[0][3] > 0) {
        /* or from the probes near it: light that bounced */
        const Lanes3 eye = eyeLanes(frame, world);
        Lanes3 toEye = {v8fSub(eye.x, world.x), v8fSub(eye.y, world.y), v8fSub(eye.z, world.z)};
        const Lanes3 seen = normalize3(toEye);
        const V8F twice = v8fMul(dot3(n, seen), v8fSet(2.f));
        const Lanes3 mirror = {v8fSub(v8fMul(n.x, twice), seen.x), v8fSub(v8fMul(n.y, twice), seen.y), v8fSub(v8fMul(n.z, twice), seen.z)};
        const Lanes3 got = indirectLanes(&f->scene->indirect, world, n, shiny ? &mirror : NULL, &bounced);
        const V8F strength = v8fSet(frame->indirect[0][3]);
        diffuse.x = v8fMul(got.x, strength); diffuse.y = v8fMul(got.y, strength); diffuse.z = v8fMul(got.z, strength);
        bounced.x = v8fMul(bounced.x, strength); bounced.y = v8fMul(bounced.y, strength); bounced.z = v8fMul(bounced.z, strength);
    }
    diffuse.x = v8fMul(diffuse.x, open);
    diffuse.y = v8fMul(diffuse.y, open);
    diffuse.z = v8fMul(diffuse.z, open);
    Lanes3 highlight = {zero, zero, zero};
    Lanes3 view = {zero, zero, zero};
    Reflecting reflecting;
    reflecting.lobe.drawn = false;
    if (!coatBent) coatN = n;
    if (shiny || coated) {
        const Lanes3 eye = eyeLanes(frame, world);
        Lanes3 toEye = {v8fSub(eye.x, world.x), v8fSub(eye.y, world.y), v8fSub(eye.z, world.z)};
        view = normalize3(toEye);
    }
    if (shiny) {
        const V8F a = v8fMul(rough, rough), dielectric = v8fSet(.04f * material->specular), notMetal = v8fSub(one, metal);
        reflecting.lobe = lanesLobeOf(rough, v8fMax(dot3(n, view), v8fSet(1e-4f)));
        reflecting.strength = v8fSet(material->specular);
        reflecting.f0.x = lanesReflects(dielectric, notMetal, albedo.x, metal);
        reflecting.f0.y = lanesReflects(dielectric, notMetal, albedo.y, metal);
        reflecting.f0.z = lanesReflects(dielectric, notMetal, albedo.z, metal);
        if (extraFlags & ARTF_MORE_SPECULAR) {
            /* (a dielectric's of a color: KHR_materials_specular) */
            reflecting.f0.x = lanesReflects(v8fMul(v8fMul(dielectric, v8fSet(more->specularColor[0])), specularTint.x), notMetal, albedo.x, metal);
            reflecting.f0.y = lanesReflects(v8fMul(v8fMul(dielectric, v8fSet(more->specularColor[1])), specularTint.y), notMetal, albedo.y, metal);
            reflecting.f0.z = lanesReflects(v8fMul(v8fMul(dielectric, v8fSet(more->specularColor[2])), specularTint.z), notMetal, albedo.z, metal);
        }
        if (extraFlags & ARTF_MORE_IRIDESCENCE) {
            /* a thin film's colors, as it is seen: in place of what it reflects head on */
            _Alignas(32) float f0[3][8], cosine[8], amount[8], thickness[8];
            v8fStore(f0[0], reflecting.f0.x); v8fStore(f0[1], reflecting.f0.y); v8fStore(f0[2], reflecting.f0.z);
            v8fStore(cosine, reflecting.lobe.ndv);
            v8fStore(amount, filmAmount);
            v8fStore(thickness, filmThickness);
            for (int i = 0; i < 8; ++i) {
                float c[3] = {f0[0][i], f0[1][i], f0[2][i]};
                artfIridescence(c, cosine[i], amount[i], more->iridescenceIor, thickness[i]);
                for (int k = 0; k < 3; ++k) f0[k][i] = c[k];
            }
            reflecting.f0.x = v8fLoad(f0[0]); reflecting.f0.y = v8fLoad(f0[1]); reflecting.f0.z = v8fLoad(f0[2]);
        }
        reflecting.gain = lanesGain3(reflecting.f0, lanesUnkept(reflecting.lobe.ndv, rough));
        if (extraFlags & ARTF_MORE_ANISOTROPY) {
            /* the way u grows, square to the normal, turned as asked; long by glTF's alpha mixed toward 1 */
            const V8F along = dot3(n, *tangent);
            Lanes3 t = {v8fSub(tangent->x, v8fMul(n.x, along)), v8fSub(tangent->y, v8fMul(n.y, along)), v8fSub(tangent->z, v8fMul(n.z, along))};
            const V8F inverse = v8fDiv(one, v8fSqrt(v8fMax(dot3(t, t), v8fSet(1e-20f))));
            t.x = v8fMul(t.x, inverse); t.y = v8fMul(t.y, inverse); t.z = v8fMul(t.z, inverse);
            const Lanes3 b = {v8fSub(v8fMul(n.y, t.z), v8fMul(n.z, t.y)), v8fSub(v8fMul(n.z, t.x), v8fMul(n.x, t.z)), v8fSub(v8fMul(n.x, t.y), v8fMul(n.y, t.x))};
            const V8F strength = v8fMul(drawnStrength, drawnStrength);
            lanesDraw(&reflecting.lobe, true, t, b, drawnC, drawnS, a, lanesDrawnLong(a, strength));
        }
        /* what comes from all around, in the mirror direction: all a metal shows in a plain sky */
        const V8F twice = v8fMul(dot3(n, view), v8fSet(2.f));
        const V8F reflectedUp = v8fAdd(v8fMul(v8fSub(v8fMul(n.y, twice), view.y), v8fSet(.5f)), v8fSet(.5f));
        const V8F far = v8fMul(lanesFifth(reflecting.lobe.ndv), open), smooth = v8fSub(one, rough);
        const Lanes3 mirror = lanesMirror3(reflecting.f0, smooth, far, open);
        highlight.x = v8fMul(lanesSkyGround(v8fSet(frame->ground[0]), v8fSet(frame->sky[0] - frame->ground[0]), reflectedUp), mirror.x);
        highlight.y = v8fMul(lanesSkyGround(v8fSet(frame->ground[1]), v8fSet(frame->sky[1] - frame->ground[1]), reflectedUp), mirror.y);
        highlight.z = v8fMul(lanesSkyGround(v8fSet(frame->ground[2]), v8fSet(frame->sky[2] - frame->ground[2]), reflectedUp), mirror.z);
        if (environment) {
            /* the picture in the mirror direction, as blurred as the surface is rough */
            _Alignas(32) float laneV[3][8], laneRough[8], got[3][8];
            v8fStore(laneV[0], view.x); v8fStore(laneV[1], view.y); v8fStore(laneV[2], view.z);
            v8fStore(laneRough, rough);
            for (int i = 0; i < 8; ++i) {
                const float nx = laneN[0][i], ny = laneN[1][i], nz = laneN[2][i];
                const float twiceNv = 2.f * (nx * laneV[0][i] + ny * laneV[1][i] + nz * laneV[2][i]);
                float c[3];
                artfEnvReflected(environment, laneRough[i], nx * twiceNv - laneV[0][i], ny * twiceNv - laneV[1][i], nz * twiceNv - laneV[2][i], c);
                for (int k = 0; k < 3; ++k) got[k][i] = c[k] * frame->sky[3];
            }
            highlight.x = v8fMul(v8fLoad(got[0]), mirror.x);
            highlight.y = v8fMul(v8fLoad(got[1]), mirror.y);
            highlight.z = v8fMul(v8fLoad(got[2]), mirror.z);
        }
        if (frame->indirect[0][3] > 0) {
            /* light that bounced, toward the mirror direction, in place of the sky's: as rough as the probes are */
            highlight.x = v8fMul(bounced.x, mirror.x);
            highlight.y = v8fMul(bounced.y, mirror.y);
            highlight.z = v8fMul(bounced.z, mirror.z);
        }
        const V8F keep = v8fSub(one, metal);
        albedo.x = v8fMul(albedo.x, keep);
        albedo.y = v8fMul(albedo.y, keep);
        albedo.z = v8fMul(albedo.z, keep);
    }
    const Lanes3 aroundDiffuse = diffuse, aroundHighlight = highlight; /* (for ambient occlusion) */
    /* a clear coat and sheen: what comes from all around first, as for the surface under them */
    Extras x;
    Extras* extras = coated ? &x : NULL;
    V8F coatSeen = zero; /* (the coat's Fresnel toward the eye: what it takes from under it) */
    if (coated) {
        x.flags = extraFlags;
        x.coatN = coatN;
        x.coatLight.x = x.coatLight.y = x.coatLight.z = zero;
        x.sheenLight = x.under = x.coatLight;
        x.ndv = v8fMax(dot3(n, view), v8fSet(1e-4f));
        x.sheenInverse = v8fDiv(one, v8fMul(sheenRough, sheenRough));
        x.reach = 1.44269504f / more->subsurfaceRadius;
        x.coat = lanesLobeOf(coatRough, v8fMax(dot3(coatN, view), v8fSet(1e-4f)));
        const V8F far = lanesFifth(x.coat.ndv);
        coatSeen = lanesCoatFresnel(far);
        const V8F twice = v8fMul(dot3(coatN, view), v8fSet(2.f));
        const V8F reflectedUp = v8fAdd(v8fMul(v8fSub(v8fMul(coatN.y, twice), view.y), v8fSet(.5f)), v8fSet(.5f));
        const V8F kept = lanesMirror(v8fSet(.04f), v8fSub(one, coatRough), v8fMul(far, open), open);
        x.coatLight.x = v8fMul(lanesSkyGround(v8fSet(frame->ground[0]), v8fSet(frame->sky[0] - frame->ground[0]), reflectedUp), kept);
        x.coatLight.y = v8fMul(lanesSkyGround(v8fSet(frame->ground[1]), v8fSet(frame->sky[1] - frame->ground[1]), reflectedUp), kept);
        x.coatLight.z = v8fMul(lanesSkyGround(v8fSet(frame->ground[2]), v8fSet(frame->sky[2] - frame->ground[2]), reflectedUp), kept);
        if (environment) {
            _Alignas(32) float laneV[3][8], laneC[3][8], laneRough[8], got[3][8];
            v8fStore(laneV[0], view.x); v8fStore(laneV[1], view.y); v8fStore(laneV[2], view.z);
            v8fStore(laneC[0], coatN.x); v8fStore(laneC[1], coatN.y); v8fStore(laneC[2], coatN.z);
            v8fStore(laneRough, coatRough);
            for (int i = 0; i < 8; ++i) {
                const float nx = laneC[0][i], ny = laneC[1][i], nz = laneC[2][i];
                const float twiceNv = 2.f * (nx * laneV[0][i] + ny * laneV[1][i] + nz * laneV[2][i]);
                float c[3];
                artfEnvReflected(environment, laneRough[i], nx * twiceNv - laneV[0][i], ny * twiceNv - laneV[1][i], nz * twiceNv - laneV[2][i], c);
                for (int j = 0; j < 3; ++j) got[j][i] = c[j] * frame->sky[3];
            }
            x.coatLight.x = v8fMul(v8fLoad(got[0]), kept);
            x.coatLight.y = v8fMul(v8fLoad(got[1]), kept);
            x.coatLight.z = v8fMul(v8fLoad(got[2]), kept);
        }
        /* (sheen from all around: what lights the surface, as much as Charlie's sheen takes of it) */
        const V8F around = lanesSheenAround(lanesFifth(x.ndv), sheenRough);
        x.sheenLight.x = v8fMul(diffuse.x, around);
        x.sheenLight.y = v8fMul(diffuse.y, around);
        x.sheenLight.z = v8fMul(diffuse.z, around);
    }
    const Lanes3 aroundCoat = coated ? x.coatLight : view, aroundSheen = coated ? x.sheenLight : view;
    const bool under = (extraFlags & ARTF_MORE_SUBSURFACE) != 0;

    if (frame->sunColor[0] > 0 || frame->sunColor[1] > 0 || frame->sunColor[2] > 0) {
        Lanes3 toSun = {v8fSet(frame->toSun[0]), v8fSet(frame->toSun[1]), v8fSet(frame->toSun[2])};
        const V8F sunShadow = frame->tracedCasts & ARTF_TRACED_SUN ? tracedLanes(f, -1, world, n, dot3(n, toSun), drawn)
                              : f->shadows                       ? shadowLanes(f, -1, world, n)
                                                                 : v8fSet(1.f);
        addLight(n, view, toSun, sunShadow, frame->sunColor, shiny, &reflecting, &diffuse, &highlight);
        if (coated) addExtras(extras, n, view, toSun, sunShadow, one, under && f->shadows ? throughLanes(f, -1, world, x.reach) : zero, frame->sunColor);
    }
    /* lights sampled (ArtfShadowDesc.sampled): each pixel takes one light, artfSampledWeigh's; only those taken are looked
     * at, each weighed in the lanes that took it and nothing in the others */
    const bool sampled = frame->more[3] > 0;
    uint32_t chosen[8], extraTaken[8], extraTakenCount = 0;
    float chosenWeight[8];
    if (sampled) {
        _Alignas(32) float wx[8], wy[8], wz[8], nx[8], ny[8], nz[8];
        v8fStore(wx, world.x); v8fStore(wy, world.y); v8fStore(wz, world.z);
        v8fStore(nx, n.x); v8fStore(ny, n.y); v8fStore(nz, n.z);
        lights = 0;
        for (int l = 0; l < 8; ++l) {
            const float p[3] = {wx[l], wy[l], wz[l]}, nn[3] = {nx[l], ny[l], nz[l]};
            uint32_t px, py, base = 0, listed = 0;
            artfSampledPixel(frame, p, &px, &py);
            if (f->extraCount && f->clusterLights) {
                const uint32_t tilesX = ((uint32_t)frame->fog[3] + ARTF_LIGHT_TILE - 1) / ARTF_LIGHT_TILE;
                const float e[3] = {p[0] - frame->cameraPos[0], p[1] - frame->cameraPos[1], p[2] - frame->cameraPos[2]};
                const float along = (e[0] * frame->eye[2][0] + e[1] * frame->eye[2][1]) + e[2] * frame->eye[2][2];
                const float deep = log2f(artfMaxf(along, 1e-30f)) * frame->slices[0] + frame->slices[1];
                const uint32_t slice = (uint32_t)artfClampf(floorf(deep), 0.f, (float)(ARTF_LIGHT_SLICES - 1));
                const uint32_t head = (((py / ARTF_LIGHT_TILE) * tilesX + px / ARTF_LIGHT_TILE) * ARTF_LIGHT_SLICES + slice) * 2;
                base = f->clusterLights[head];
                listed = f->clusterLights[head + 1];
            }
            const uint32_t count = frame->lightCount + listed, seed = artfSampledSeed(px, py, (uint32_t)frame->output[3]);
            chosen[l] = UINT32_MAX;
            chosenWeight[l] = 0;
            float sum = 0, mine = 0;
            for (uint32_t c = 0; count && c < ARTF_SAMPLED_DRAWS; ++c) {
                const uint32_t j = artfSampledHash(seed + 2 * c + 1) % count;
                const uint32_t code = j < frame->lightCount ? j : 0x80000000u | f->clusterLights[base + j - frame->lightCount];
                const ArtfLightData* L = code & 0x80000000u ? &f->extra[code & 0x7FFFFFFFu] : &frame->lights[j];
                const float weigh = artfSampledWeigh(L->position, L->radius, L->color, L->direction, L->spotCut, L->spotScale, p, nn);
                sum += weigh;
                const float r = (float)(artfSampledHash(seed + 2 * c + 2) >> 8) * (1.f / 16777216.f);
                if (weigh > 0 && r * sum < weigh) {
                    chosen[l] = code;
                    mine = weigh;
                }
            }
            if (chosen[l] == UINT32_MAX) continue;
            chosenWeight[l] = (float)count * sum / ((float)ARTF_SAMPLED_DRAWS * mine);
            if (chosen[l] & 0x80000000u) {
                bool known = false;
                for (uint32_t k = 0; k < extraTakenCount; ++k) known = known || extraTaken[k] == (chosen[l] & 0x7FFFFFFFu);
                if (!known) extraTaken[extraTakenCount++] = chosen[l] & 0x7FFFFFFFu;
            } else lights |= 1u << chosen[l];
        }
    }
#define SAMPLED_WEIGHT(code) sampledWeight(chosen, chosenWeight, code)
    for (uint32_t i = 0; lights >> i; ++i) {
        if (!(lights >> i & 1)) continue;
        const ArtfLightData* L = &frame->lights[i];
        Lanes3 d = {v8fSub(v8fSet(L->position[0]), world.x), v8fSub(v8fSet(L->position[1]), world.y), v8fSub(v8fSet(L->position[2]), world.z)};
        V8F d2 = dot3(d, d);
        V8F invLength = v8fDiv(v8fSet(1.f), v8fSqrt(v8fMax(d2, v8fSet(1e-8f))));
        Lanes3 toLight = {v8fMul(d.x, invLength), v8fMul(d.y, invLength), v8fMul(d.z, invLength)};
        V8F fade = v8fMax(v8fSub(v8fSet(1.f), v8fMul(d2, v8fSet(1.f / (L->radius * L->radius)))), zero);
        if (viewed) lightsHere = v8fAdd(lightsHere, v8fSelect(v8fGt(fade, zero), zero, one));
        Lanes3 goes = {v8fSet(L->direction[0]), v8fSet(L->direction[1]), v8fSet(L->direction[2])};
        V8F spot = v8fMul(v8fSub(v8fSub(zero, dot3(toLight, goes)), v8fSet(L->spotCut)), v8fSet(L->spotScale));
        spot = v8fMin(v8fMax(spot, zero), v8fSet(1.f));
        if (sampled) spot = v8fMul(spot, SAMPLED_WEIGHT(i));
        const V8F cone = spot;
        if (frame->tracedCasts >> i & 1) spot = v8fMul(spot, tracedLanes(f, (int)i, world, n, v8fMul(fade, spot), drawn));
        else if (f->shadows && f->shadows->frame->lights[i][0]) spot = v8fMul(spot, shadowLanes(f, (int)i, world, n));
        const V8F through = under && f->shadows && f->shadows->frame->lights[i][0] ? throughLanes(f, (int)i, world, x.reach) : zero;
        if (sized && artfLightShaped(frame, i)) {
            addShaped(f, i, n, view, world, rough, spot, cone, through, shiny, &reflecting, &diffuse, &highlight, extras);
            continue;
        }
        if (sized && frame->lightSizes[i] > 0) addBall(n, view, d, toLight, frame->lightSizes[i], rough, v8fMul(v8fMul(fade, fade), spot), L->color, shiny, &reflecting, &diffuse, &highlight);
        else
        addLight(n, view, toLight, v8fMul(v8fMul(fade, fade), spot), L->color, shiny, &reflecting, &diffuse, &highlight);
        if (coated) addExtras(extras, n, view, toLight, v8fMul(v8fMul(fade, fade), spot), v8fMul(v8fMul(fade, fade), cone), through, L->color);
    }
    /* those past them that reach this tile: the same light; the first few with a shadow of their own */
    const uint32_t extraVisits = sampled ? extraTakenCount : extra ? extra[0] : 0;
    for (uint32_t k = 0; k < extraVisits; ++k) {
        const uint32_t index = sampled ? extraTaken[k] : extra[1 + k];
        const ArtfLightData* L = &f->extra[index];
        Lanes3 d = {v8fSub(v8fSet(L->position[0]), world.x), v8fSub(v8fSet(L->position[1]), world.y), v8fSub(v8fSet(L->position[2]), world.z)};
        V8F d2 = dot3(d, d);
        V8F invLength = v8fDiv(v8fSet(1.f), v8fSqrt(v8fMax(d2, v8fSet(1e-8f))));
        Lanes3 toLight = {v8fMul(d.x, invLength), v8fMul(d.y, invLength), v8fMul(d.z, invLength)};
        V8F fade = v8fMax(v8fSub(v8fSet(1.f), v8fMul(d2, v8fSet(1.f / (L->radius * L->radius)))), zero);
        if (v8iSigns(v8fGt(fade, zero)) == 0) continue; /* (none of the eight is in reach) */
        if (viewed) lightsHere = v8fAdd(lightsHere, v8fSelect(v8fGt(fade, zero), zero, one));
        Lanes3 goes = {v8fSet(L->direction[0]), v8fSet(L->direction[1]), v8fSet(L->direction[2])};
        V8F spot = v8fMul(v8fSub(v8fSub(zero, dot3(toLight, goes)), v8fSet(L->spotCut)), v8fSet(L->spotScale));
        spot = v8fMin(v8fMax(spot, zero), v8fSet(1.f));
        if (sampled) spot = v8fMul(spot, SAMPLED_WEIGHT(0x80000000u | index));
        if ((float)index < frame->more[2]) spot = v8fMul(spot, tracedLanesAt(f, L->position, world, n, v8fMul(fade, spot), drawn));
        else if (f->shadows && index < ARTF_SHADOW_EXTRA && f->shadows->frame->extra[index][0]) spot = v8fMul(spot, shadowLanesExtra(f, index, L->position, world, n));
        addLight(n, view, toLight, v8fMul(v8fMul(fade, fade), spot), L->color, shiny, &reflecting, &diffuse, &highlight);
        if (coated) addExtras(extras, n, view, toLight, v8fMul(v8fMul(fade, fade), spot), zero, zero, L->color);
    }

    Lanes3 lit = {v8fAdd(v8fAdd(v8fMul(albedo.x, diffuse.x), highlight.x), own.x),
                  v8fAdd(v8fAdd(v8fMul(albedo.y, diffuse.y), highlight.y), own.y),
                  v8fAdd(v8fAdd(v8fMul(albedo.z, diffuse.z), highlight.z), own.z)};
    /* (with light under the surface and a frame in floats: what of it spreads under the surface, diffuse) */
    Lanes3 spread = {v8fMul(albedo.x, diffuse.x), v8fMul(albedo.y, diffuse.y), v8fMul(albedo.z, diffuse.z)};
    /* (what came from all around, for ambient occlusion: only a frame in floats reads it) */
    Lanes3 aroundLit = {zero, zero, zero};
    if (f->post || coated || viewed) {
        aroundLit.x = v8fAdd(v8fMul(albedo.x, aroundDiffuse.x), aroundHighlight.x);
        aroundLit.y = v8fAdd(v8fMul(albedo.y, aroundDiffuse.y), aroundHighlight.y);
        aroundLit.z = v8fAdd(v8fMul(albedo.z, aroundDiffuse.z), aroundHighlight.z);
    }
    if (coated) {
        /* sheen on the surface, and what came from under it, under the coat; the coat takes from all of it what its
         * Fresnel reflects */
        const V8F notMetal = v8fSub(one, metal), coat = coatAmount, keep = v8fSub(one, v8fMul(coat, coatSeen));
        const V8F sr = v8fMul(sheenColor.x, notMetal), sg = v8fMul(sheenColor.y, notMetal), sb = v8fMul(sheenColor.z, notMetal);
        const Lanes3 coatLight = x.coatLight, sheenLight = x.sheenLight;
        if (under) {
            lit.x = v8fAdd(lit.x, v8fMul(v8fMul(v8fSet(more->subsurface[0]), notMetal), x.under.x));
            lit.y = v8fAdd(lit.y, v8fMul(v8fMul(v8fSet(more->subsurface[1]), notMetal), x.under.y));
            lit.z = v8fAdd(lit.z, v8fMul(v8fMul(v8fSet(more->subsurface[2]), notMetal), x.under.z));
            spread.x = v8fMul(v8fAdd(spread.x, v8fMul(v8fMul(v8fSet(more->subsurface[0]), notMetal), x.under.x)), keep);
            spread.y = v8fMul(v8fAdd(spread.y, v8fMul(v8fMul(v8fSet(more->subsurface[1]), notMetal), x.under.y)), keep);
            spread.z = v8fMul(v8fAdd(spread.z, v8fMul(v8fMul(v8fSet(more->subsurface[2]), notMetal), x.under.z)), keep);
        }
        lit.x = v8fAdd(v8fAdd(v8fMul(v8fAdd(v8fSub(lit.x, own.x), v8fMul(sr, sheenLight.x)), keep), v8fMul(coat, coatLight.x)), own.x);
        lit.y = v8fAdd(v8fAdd(v8fMul(v8fAdd(v8fSub(lit.y, own.y), v8fMul(sg, sheenLight.y)), keep), v8fMul(coat, coatLight.y)), own.y);
        lit.z = v8fAdd(v8fAdd(v8fMul(v8fAdd(v8fSub(lit.z, own.z), v8fMul(sb, sheenLight.z)), keep), v8fMul(coat, coatLight.z)), own.z);
        aroundLit.x = v8fAdd(v8fMul(v8fAdd(aroundLit.x, v8fMul(sr, aroundSheen.x)), keep), v8fMul(coat, aroundCoat.x));
        aroundLit.y = v8fAdd(v8fMul(v8fAdd(aroundLit.y, v8fMul(sg, aroundSheen.y)), keep), v8fMul(coat, aroundCoat.y));
        aroundLit.z = v8fAdd(v8fMul(v8fAdd(aroundLit.z, v8fMul(sb, aroundSheen.z)), keep), v8fMul(coat, aroundCoat.z));
    }
    if (material->flags & ARTF_MATERIAL_UNLIT) {
        /* its color as it is, and its own light */
        lit.x = v8fAdd(baseColor.x, own.x);
        lit.y = v8fAdd(baseColor.y, own.y);
        lit.z = v8fAdd(baseColor.z, own.z);
    }
    /* (in a view, what it shows: the frame for one has no fog or exposure) */
    if (viewed && f->view) lit = viewLanes(f, frame, &baseColor, n, rough, metal, &aroundLit, &own, world, who, lightsHere);
    if (frame->aerial[3][3] > 0) {
        /* aerial perspective (ArtfSkyDesc.aerial): lane by lane, as artfAerialOf has it */
        const Lanes3 eye = eyeLanes(frame, world);
        _Alignas(32) float from[3][8], l[3][8];
        v8fStore(from[0], v8fSub(world.x, eye.x)); v8fStore(from[1], v8fSub(world.y, eye.y)); v8fStore(from[2], v8fSub(world.z, eye.z));
        v8fStore(l[0], lit.x); v8fStore(l[1], lit.y); v8fStore(l[2], lit.z);
        for (int i = 0; i < 8; ++i) {
            const float x = from[0][i], y = from[1][i], z = from[2][i], d = sqrtf((x * x + y * y) + z * z);
            const float mu = d > 0 ? ((x * frame->toSun[0] + y * frame->toSun[1]) + z * frame->toSun[2]) / d : 0.f;
            float keep[3], add[3];
            artfAerialOf(frame, d, mu, keep, add);
            for (int c = 0; c < 3; ++c) l[c][i] = l[c][i] * keep[c] + add[c];
        }
        lit.x = v8fLoad(l[0]); lit.y = v8fLoad(l[1]); lit.z = v8fLoad(l[2]);
    }
    if (frame->look[1] > 0 && !f->postFog) {
        const Lanes3 eye = eyeLanes(frame, world);
        Lanes3 fromEye = {v8fSub(world.x, eye.x), v8fSub(world.y, eye.y), v8fSub(world.z, eye.z)};
        const V8F keep = lanesFogKeep(v8fMul(v8fSqrt(dot3(fromEye, fromEye)), v8fSet(frame->look[1])));
        const V8F fr = v8fSet(frame->fog[0]), fg = v8fSet(frame->fog[1]), fb = v8fSet(frame->fog[2]);
        lit.x = v8fAdd(fr, v8fMul(v8fSub(lit.x, fr), keep));
        lit.y = v8fAdd(fg, v8fMul(v8fSub(lit.y, fg), keep));
        lit.z = v8fAdd(fb, v8fMul(v8fSub(lit.z, fb), keep));
    }
    V8F airLeft = one; /* (what fog leaves of the light: ambient occlusion takes from that alone) */
    if (f->postFog) {
        /* fog that light goes through: from the grid, where this point falls in the frame */
        _Alignas(32) float lw[3][8], got[4][8];
        v8fStore(lw[0], world.x); v8fStore(lw[1], world.y); v8fStore(lw[2], world.z);
        const float* m = frame->viewProj;
        for (int i = 0; i < 8; ++i) {
            const float x = lw[0][i], y = lw[1][i], z = lw[2][i];
            const float cx = m[0] * x + m[4] * y + m[8] * z + m[12], cy = m[1] * x + m[5] * y + m[9] * z + m[13];
            const float cw = artfMaxf(m[3] * x + m[7] * y + m[11] * z + m[15], 1e-5f);
            /* (orthographic: w is 1, and how far along the view is the depth's) */
            const float along = artfOrtho(frame) ? frame->ortho[1] - (m[2] * x + m[6] * y + m[10] * z + m[14]) * frame->ortho[0] : cw;
            float c[4];
            artfFroxelAt(f, (cx / cw * .5f + .5f) * (float)f->width - .5f, (.5f - cy / cw * .5f) * (float)f->height - .5f, along, c);
            for (int j = 0; j < 4; ++j) got[j][i] = c[j];
        }
        airLeft = v8fLoad(got[3]);
        lit.x = v8fAdd(v8fMul(lit.x, airLeft), v8fLoad(got[0]));
        lit.y = v8fAdd(v8fMul(lit.y, airLeft), v8fLoad(got[1]));
        lit.z = v8fAdd(v8fMul(lit.z, airLeft), v8fLoad(got[2]));
    }
    LitOut out;
    if (f->post) {
        /* light as it is: exposure, tone mapping and fog come after */
        const V8F a = layered ? v8fMin(v8fMax(alpha, zero), one) : one;
        const Lanes3 kept = {v8fMul(lit.x, a), v8fMul(lit.y, a), v8fMul(lit.z, a)};
        out.color = pack9e5(kept);
        out.alpha = a;
        const V8F all = v8fAdd(v8fAdd(v8fMul(lit.x, v8fSet(.3f)), v8fMul(lit.y, v8fSet(.59f))), v8fMul(lit.z, v8fSet(.11f)));
        const V8F around = v8fAdd(v8fAdd(v8fMul(aroundLit.x, v8fSet(.3f)), v8fMul(aroundLit.y, v8fSet(.59f))), v8fMul(aroundLit.z, v8fSet(.11f)));
        out.surface = packSurface(n, rough, metal, v8fDiv(v8fMul(around, airLeft), v8fMax(all, v8fSet(1e-6f))));
        out.under = v8iSet(0);
        if (under && !(material->flags & ARTF_MATERIAL_UNLIT)) {
            /* light under the surface: its share of the light, and how far it goes (artfUnderWord) */
            const V8F spreadAll = v8fAdd(v8fAdd(v8fMul(spread.x, v8fSet(.3f)), v8fMul(spread.y, v8fSet(.59f))), v8fMul(spread.z, v8fSet(.11f)));
            const V8F share = v8fMin(v8fMax(v8fDiv(v8fMul(spreadAll, airLeft), v8fMax(all, v8fSet(1e-6f))), zero), one);
            const V8I bits = v8iSet((int32_t)artfUnderReach(more->subsurfaceRadius));
            out.under = v8iSelect(v8fGt(share, zero), v8iSet(0), v8iOr(bits, v8fToI(v8fAdd(v8fMul(share, v8fSet(255.f)), v8fSet(.5f)))));
        }
        return out;
    }
    if (frame->look[0] > 0) {
        const V8F exposure = v8fSet(frame->look[0]);
        lit.x = toneMap(v8fMul(lit.x, exposure));
        lit.y = toneMap(v8fMul(lit.y, exposure));
        lit.z = toneMap(v8fMul(lit.z, exposure));
    }
    out.color = layered ? packLayer(lit, alpha) : packColor(lit);
    return out;
}

/* (with a clear coat or sheen, or lights with a size: a copy of its own, called, so that the plain one stays as small as
 * it was) */
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static LitOut lightLanesCoated(const ArtfSoftFrame* f, const ArtfFrameData* frame, const ArtfMaterialData* material, const ArtfTextureData* textures, const ArtfTextureData* texture, uint32_t lights, const uint32_t* extra, int level, V8F turn, Lanes3 rawN, Lanes3 world, V8F u, V8F v,
                               bool colored, bool layered, Lanes3 tint, V8F alpha, const Lanes3* tangent, const Lanes3* bitangent, const uint32_t* who, const float* drawn) {
    return lightLanesAs(f, frame, material, textures, texture, lights, extra, level, turn, rawN, world, u, v, colored, layered, tint, alpha, tangent, bitangent, who, drawn,
                        artfMaterialLayered(&f->scene->materialsMore.items[material - f->scene->materials.items]), true, f->view != ARTF_VIEW_LIT);
}

/* (lightsApart: lights with a size, or a view: what the plain copy does not have to look at) */
static LitOut lightLanes(const ArtfSoftFrame* f, const ArtfFrameData* frame, const ArtfMaterialData* material, const ArtfTextureData* textures, const ArtfTextureData* texture, uint32_t lights, const uint32_t* extra, int level, V8F turn, Lanes3 rawN, Lanes3 world, V8F u, V8F v,
                      bool colored, bool layered, Lanes3 tint, V8F alpha, const Lanes3* tangent, const Lanes3* bitangent, const uint32_t* who, const float* drawn) {
    if (f->lightsApart || artfMaterialLayered(&f->scene->materialsMore.items[material - f->scene->materials.items]))
        return lightLanesCoated(f, frame, material, textures, texture, lights, extra, level, turn, rawN, world, u, v, colored, layered, tint, alpha, tangent, bitangent, who, drawn);
    return lightLanesAs(f, frame, material, textures, texture, lights, extra, level, turn, rawN, world, u, v, colored, layered, tint, alpha, tangent, bitangent, who, drawn, false, false, false);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static void batchFlush(LitBatch* b, const ArtfSoftFrame* f, uint32_t* color) {
    const ArtfFrameData* frame = &f->data;
    if (!b->count) return;
    for (int i = b->count; i < 8; ++i) { /* (what is left of the vector: any number that is not a NaN) */
        b->nx[i] = b->nx[0]; b->ny[i] = b->ny[0]; b->nz[i] = b->nz[0];
        b->wx[i] = b->wx[0]; b->wy[i] = b->wy[0]; b->wz[i] = b->wz[0];
        b->u[i] = b->u[0]; b->v[i] = b->v[0];
        b->turn[i] = b->turn[0];
        if (b->colored) { b->r[i] = b->r[0]; b->g[i] = b->g[0]; b->b[i] = b->b[0]; b->a[i] = b->a[0]; }
        if (b->bumpy) { b->tx[i] = b->tx[0]; b->ty[i] = b->ty[0]; b->tz[i] = b->tz[0]; b->bx[i] = b->bx[0]; b->by[i] = b->by[0]; b->bz[i] = b->bz[0]; }
        if (f->view) b->who[i] = b->who[0];
        b->drawn[i] = b->drawn[0];
    }
    Lanes3 n = {v8fLoad(b->nx), v8fLoad(b->ny), v8fLoad(b->nz)}, world = {v8fLoad(b->wx), v8fLoad(b->wy), v8fLoad(b->wz)};
    const V8F one = v8fSet(1.f);
    Lanes3 tint = {one, one, one};
    V8F alpha = v8fSet(b->opacity);
    if (b->colored) {
        tint.x = v8fLoad(b->r);
        tint.y = v8fLoad(b->g);
        tint.z = v8fLoad(b->b);
        alpha = v8fMul(alpha, v8fLoad(b->a));
    }
    _Alignas(32) uint32_t colors[8], surfaces[8];
    _Alignas(32) float alphas[8];
    Lanes3 tangent = {one, one, one}, bitangent = {one, one, one};
    if (b->bumpy) {
        tangent.x = v8fLoad(b->tx); tangent.y = v8fLoad(b->ty); tangent.z = v8fLoad(b->tz);
        bitangent.x = v8fLoad(b->bx); bitangent.y = v8fLoad(b->by); bitangent.z = v8fLoad(b->bz);
    }
    const LitOut lit = lightLanes(f, frame, b->material, b->textures, b->texture, b->lights, b->extra, b->level, v8fLoad(b->turn), n, world, v8fLoad(b->u), v8fLoad(b->v), b->colored, b->layered, tint, alpha, &tangent, &bitangent, b->who, b->drawn);
    v8iStore(colors, lit.color);
    for (int i = 0; i < b->count; ++i) color[b->at[i]] = colors[i];
    if (f->post) {
        _Alignas(32) uint32_t unders[8];
        v8fStore(alphas, lit.alpha);
        v8iStore(surfaces, lit.surface);
        v8iStore(unders, lit.under);
        for (int i = 0; i < b->count; ++i) {
            ((float*)(color + ARTF_POST_ALPHA))[b->at[i]] = alphas[i];
            color[ARTF_POST_SURFACE + b->at[i]] = surfaces[i];
            color[ARTF_POST_UNDER + b->at[i]] = unders[i];
        }
    }
    b->count = 0;
    b->lights = 0;
}

/* The mipmap at a pixel, by the quotient rule on u = (u0 e0 + u1 e1 + u2 e2) / (e0 + e1 + e2). */
static int mipAt(const ShadeContext* s, float x, float y) {
    const ArtfTextureData* texture = s->sizing;
    if (!texture || texture->mipCount == 1) return 0;
    float S = 0, Sx = 0, Sy = 0, U = 0, Ux = 0, Uy = 0, V = 0, Vx = 0, Vy = 0;
    for (int i = 0; i < 3; ++i) {
        const float e = s->A[i] * x + s->B[i] * y + s->C[i];
        S += e; Sx += s->A[i]; Sy += s->B[i];
        U += s->uv[i][0] * e; Ux += s->uv[i][0] * s->A[i]; Uy += s->uv[i][0] * s->B[i];
        V += s->uv[i][1] * e; Vx += s->uv[i][1] * s->A[i]; Vy += s->uv[i][1] * s->B[i];
    }
    const float s2 = 1.f / (S * S), tw = (float)texture->width, th = (float)texture->height;
    const float dudx = (Ux * S - U * Sx) * s2 * tw, dvdx = (Vx * S - V * Sx) * s2 * th;
    const float dudy = (Uy * S - U * Sy) * s2 * tw, dvdy = (Vy * S - V * Sy) * s2 * th;
    return mipLevel(artfMaxf(dudx * dudx + dvdx * dvdx, dudy * dudy + dvdy * dvdy), texture->mipCount);
}

/* How much the normal turns over a pixel (shading.inc's Widened): a pixel's variance, 1/4, times the square of how fast
 * the normal, made of length 1, turns from one pixel to the next, across and down; by the quotient rule on
 * n = (n0 e0 + n1 e1 + n2 e2) / (e0 + e1 + e2), as mipAt. */
static float turnAt(const ShadeContext* s, float x, float y) {
    float S = 0, Sx = 0, Sy = 0, P[3] = {0, 0, 0}, Px[3] = {0, 0, 0}, Py[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        const float e = s->A[i] * x + s->B[i] * y + s->C[i];
        S += e; Sx += s->A[i]; Sy += s->B[i];
        for (int c = 0; c < 3; ++c) {
            P[c] += s->nrm[i][c] * e;
            Px[c] += s->nrm[i][c] * s->A[i];
            Py[c] += s->nrm[i][c] * s->B[i];
        }
    }
    const float inverse = 1.f / S, s2 = inverse * inverse;
    float N[3], dx[3], dy[3];
    for (int c = 0; c < 3; ++c) {
        N[c] = P[c] * inverse;
        dx[c] = (Px[c] * S - P[c] * Sx) * s2;
        dy[c] = (Py[c] * S - P[c] * Sy) * s2;
    }
    const float length2 = N[0] * N[0] + N[1] * N[1] + N[2] * N[2];
    if (!(length2 > 1e-12f)) return 0.f;
    /* (of n / |n|: what is square to n, over |n|) */
    const float alongX = (N[0] * dx[0] + N[1] * dx[1] + N[2] * dx[2]) / length2, alongY = (N[0] * dy[0] + N[1] * dy[1] + N[2] * dy[2]) / length2;
    float turned = 0;
    for (int c = 0; c < 3; ++c) {
        const float tx = dx[c] - N[c] * alongX, ty = dy[c] - N[c] * alongY;
        turned += tx * tx + ty * ty;
    }
    return .25f * turned / length2;
}

/* In a view, what a triangle is colored by: its level of detail, its meshlet in its mesh, or its instance (for motion
 * too, which follows the instance). */
static uint32_t viewWho(const ArtfSoftFrame* f, const ArtfRasterTri* t) {
    const ArtfVisible* v = &f->visible[t->instance];
    if (f->view == ARTF_VIEW_MESHLETS) return (t->triangle & ~ARTF_TRI_BACK) >> 7;
    if (f->view != ARTF_VIEW_LOD) return v->index;
    const ArtfMeshData* base = &f->scene->meshes.items[v->instance->mesh];
    for (uint32_t k = 0; k < base->lodCount; ++k)
        if (v->mesh == &f->scene->meshes.items[base->lods[k]]) return k + 1;
    return 0;
}

static void shadeSetup(ShadeContext* s, const ArtfSoftFrame* f, const ArtfRasterTri* t, int ox, int oy) {
    const ArtfVisible* v = &f->visible[t->instance];
    for (int i = 0; i < 8; ++i) s->drawn[i] = v->drawn;
    if (f->view) {
        const uint32_t who = viewWho(f, t);
        for (int i = 0; i < 8; ++i) s->who[i] = who;
    }
    const ArtfMeshData* mesh = v->mesh;
    const ArtfMeshlet* l = &mesh->meshlets[(t->triangle & ~ARTF_TRI_BACK) >> 7];
    const uint8_t* local = mesh->meshletTriangles + ((size_t)l->triangleOffset + (t->triangle & 127)) * 3;
    const float* m = v->instance->m;
    const float* k = v->normal;
    const ArtfFrameData* frame = &f->data;
    const ArtfMaterialData* material = &f->scene->materials.items[v->instance->material];
    s->material = material;
    s->fade = v->fade;
    s->originX = ox;
    s->originY = oy;
    s->texture = material->texture ? &f->scene->textures.items[material->texture] : NULL;
    s->needWorld = material->specular > 0 || material->metallic > 0 || frame->lightCount || f->extraCount || frame->indirect[0][3] > 0 || frame->look[1] > 0 || frame->aerial[3][3] > 0 || f->shadows || frame->tracedCasts || material->graph || f->decalCount || f->postFog || f->view;
    s->bumpy = material->normalMap != 0 || ARTF_MORE_TANGENT(&f->scene->materialsMore.items[v->instance->material]);
    s->turning = false; /* (when its normals are known, below) */
    const ArtfTextureData* textures = f->scene->textures.items;
    s->sizing = s->texture ? s->texture : material->normalMap ? &textures[material->normalMap] : material->surfaceMap ? &textures[material->surfaceMap] : material->occlusionMap ? &textures[material->occlusionMap] : NULL;
    if (!s->sizing) {
        /* (or the first of a coat's or sheen's) */
        const ArtfMaterialMore* more = &f->scene->materialsMore.items[v->instance->material];
        const uint32_t map = more->sheenMap ? more->sheenMap : more->coatMap ? more->coatMap : more->coatRoughnessMap ? more->coatRoughnessMap : more->coatNormalMap ? more->coatNormalMap : more->sheenRoughnessMap ? more->sheenRoughnessMap
                           : more->specularColorMap ? more->specularColorMap : more->specularMap ? more->specularMap : more->iridescenceMap ? more->iridescenceMap
                           : more->iridescenceThicknessMap ? more->iridescenceThicknessMap : more->anisotropyMap;
        s->sizing = map ? &textures[map] : NULL;
    }
    s->colored = mesh->colors != NULL;
    s->layered = v->instance->translucent;
    s->opacity = (float)v->instance->opacity * (1.f / 65535.f) * material->opacity;

    /* the triangle itself: clipping changed what is covered, not this */
    double clip[3][3];
    for (int i = 0; i < 3; ++i) {
        const uint32_t li = local[i];
        const float* p = mesh->positions + l->vertexOffset + li;
        const float x = p[0], y = p[mesh->positionStride], z = p[(size_t)mesh->positionStride * 2];
        for (int r = 0; r < 3; ++r) {
            s->wp[i][r] = m[r * 4] * x + m[r * 4 + 1] * y + m[r * 4 + 2] * z + m[r * 4 + 3];
            clip[i][r] = (double)v->mvp[r * 4] * x + (double)v->mvp[r * 4 + 1] * y + (double)v->mvp[r * 4 + 2] * z + v->mvp[r * 4 + 3];
        }
        const ArtfPackedVertex* pv = &mesh->vertices[mesh->meshletVertices[l->vertexOffset + li]];
        /* (of no fixed length until here: one normalizing does for the octahedron and the matrix) */
        const ArtfVec3 on = artfOctUnfold(pv->nx, pv->ny);
        const float nx = k[0] * on.x + k[3] * on.y + k[6] * on.z, ny = k[1] * on.x + k[4] * on.y + k[7] * on.z, nz = k[2] * on.x + k[5] * on.y + k[8] * on.z;
        /* (a back is lit as a front: its normals come toward the eye) */
        const float scale = (t->triangle & ARTF_TRI_BACK ? -1.f : 1.f) / sqrtf(nx * nx + ny * ny + nz * nz);
        s->nrm[i][0] = nx * scale; s->nrm[i][1] = ny * scale; s->nrm[i][2] = nz * scale;
        if (i && (s->nrm[i][0] != s->nrm[0][0] || s->nrm[i][1] != s->nrm[0][1] || s->nrm[i][2] != s->nrm[0][2])) s->turning = true;
        s->uv[i][0] = artfHalfToFloat(pv->u);
        s->uv[i][1] = artfHalfToFloat(pv->v);
        if (s->colored) {
            /* (sRGB, as a texture's texels are) */
            const uint32_t word = mesh->colors[mesh->meshletVertices[l->vertexOffset + li]];
            s->col[i][0] = artfSrgbDecode((float)(word >> 16 & 255) * (1.f / 255.f));
            s->col[i][1] = artfSrgbDecode((float)(word >> 8 & 255) * (1.f / 255.f));
            s->col[i][2] = artfSrgbDecode((float)(word & 255) * (1.f / 255.f));
            s->col[i][3] = (float)(word >> 24) * (1.f / 255.f);
        }
    }

    /* where u and v go on it, for a normal map: worked out here, so that vertices need not hold it */
    if (s->bumpy) {
        const float du1 = s->uv[1][0] - s->uv[0][0], dv1 = s->uv[1][1] - s->uv[0][1], du2 = s->uv[2][0] - s->uv[0][0], dv2 = s->uv[2][1] - s->uv[0][1];
        const float det = du1 * dv2 - du2 * dv1, inv = det != 0 ? 1.f / det : 0.f;
        float tl = 0, bl = 0;
        for (int c = 0; c < 3; ++c) {
            const float e1 = s->wp[1][c] - s->wp[0][c], e2 = s->wp[2][c] - s->wp[0][c];
            s->tangent[c] = (e1 * dv2 - e2 * dv1) * inv;
            s->bitangent[c] = (e2 * du1 - e1 * du2) * inv;
            tl += s->tangent[c] * s->tangent[c];
            bl += s->bitangent[c] * s->bitangent[c];
        }
        tl = tl > 0 ? 1.f / sqrtf(tl) : 0.f;
        bl = bl > 0 ? 1.f / sqrtf(bl) : 0.f;
        for (int c = 0; c < 3; ++c) { s->tangent[c] *= tl; s->bitangent[c] *= bl; }
    }

    s->turning = s->turning && artfWidenHighlights && (material->specular > 0 || material->metallic > 0 || material->graph);

    /* Barycentrics straight from clip space (Olano & Greer): each is a linear function of the pixel
     * over their sum, perspective included, with no need for the vertices to be in front of the eye. */
    const double pw = f->pixelW, ph = f->pixelH;
    double sum = 0, a[3], b[3], c[3];
    for (int i = 0; i < 3; ++i) {
        const double *p = clip[i == 2 ? 0 : i + 1], *q = clip[i == 0 ? 2 : i - 1];
        const double kx = p[1] * q[2] - p[2] * q[1], ky = p[2] * q[0] - p[0] * q[2], kz = p[0] * q[1] - p[1] * q[0];
        /* at pixel X, Y: ndc x = 2 (X + .5) / W - 1, ndc y = 1 - 2 (Y + .5) / H */
        a[i] = kx * 2 * pw;
        b[i] = -ky * 2 * ph;
        c[i] = kx * (pw - 1) + ky * (1 - ph) + kz + a[i] * ox + b[i] * oy;
        sum += fabs(a[i]) + fabs(b[i]);
    }
    /* (only their ratios matter: scaled, so that floats hold them whatever the distance) */
    const double scale = sum > 0 ? 1.0 / sum : 1.0;
    for (int i = 0; i < 3; ++i) { s->A[i] = (float)(a[i] * scale); s->B[i] = (float)(b[i] * scale); s->C[i] = (float)(c[i] * scale); }

    /* which lights can reach it */
    s->lights = 0;
    if (frame->lightCount) {
        const float cx = (s->wp[0][0] + s->wp[1][0] + s->wp[2][0]) / 3.f, cy = (s->wp[0][1] + s->wp[1][1] + s->wp[2][1]) / 3.f, cz = (s->wp[0][2] + s->wp[1][2] + s->wp[2][2]) / 3.f;
        float r2 = 0;
        for (int i = 0; i < 3; ++i) {
            const float dx = s->wp[i][0] - cx, dy = s->wp[i][1] - cy, dz = s->wp[i][2] - cz;
            r2 = artfMaxf(r2, dx * dx + dy * dy + dz * dz);
        }
        const float r = sqrtf(r2);
        for (uint32_t i = 0; i < frame->lightCount; ++i) {
            const ArtfLightData* L = &frame->lights[i];
            const float dx = L->position[0] - cx, dy = L->position[1] - cy, dz = L->position[2] - cz, reach = L->radius + r;
            if (dx * dx + dy * dy + dz * dz < reach * reach) s->lights |= 1u << i;
        }
    }
}

static V8I maskHoles(const ShadeContext* s, int x, int y) {
    const V8F fx = v8fAdd(v8iToF(v8iIota()), v8fSet((float)x)), fy = v8fSet((float)y);
    V8F b0 = v8fAdd(v8fAdd(v8fMul(v8fSet(s->A[0]), fx), v8fMul(v8fSet(s->B[0]), fy)), v8fSet(s->C[0]));
    V8F b1 = v8fAdd(v8fAdd(v8fMul(v8fSet(s->A[1]), fx), v8fMul(v8fSet(s->B[1]), fy)), v8fSet(s->C[1]));
    V8F b2 = v8fAdd(v8fAdd(v8fMul(v8fSet(s->A[2]), fx), v8fMul(v8fSet(s->B[2]), fy)), v8fSet(s->C[2]));
    const V8F inv = v8fDiv(v8fSet(1.f), v8fAdd(v8fAdd(b0, b1), b2));
    b0 = v8fMul(b0, inv);
    b1 = v8fMul(b1, inv);
#define LERP(a, c) v8fAdd(v8fAdd(v8fMul(b0, v8fSet(s->a[0][c] - s->a[2][c])), v8fMul(b1, v8fSet(s->a[1][c] - s->a[2][c]))), v8fSet(s->a[2][c]))
    V8F alpha = v8fSet(1.f);
    if (s->texture) {
        const ArtfTextureData* t = s->texture;
        const int level = artfMaxi(mipAt(s, (float)x + 3.5f, (float)y), t->residentMip);
        sampleTexture(t->pixels + t->offsets[level], artfMaxi(t->width >> level, 1), artfMaxi(t->height >> level, 1), LERP(uv, 0), LERP(uv, 1), &alpha);
    }
    if (s->colored) alpha = v8fMul(alpha, LERP(col, 3));
#undef LERP
    V8I holes = v8fLt(alpha, v8fSet(s->material->flags & ARTF_MATERIAL_MASK ? s->material->cutoff : s->material->flags & ARTF_MATERIAL_BLEND ? 1.f / 255.f : 0.f));
    if (s->fade) {
        /* the same pattern as the GPU's: each level keeps the pixels the other leaves */
        _Alignas(32) int32_t out[8];
        const int level = (int)(s->fade >> 1), lighter = s->fade & 1;
        for (int i = 0; i < 8; ++i) {
            const int px = s->originX + x + i, py = s->originY + y, a = px ^ py;
            const int cell = ((a & 1) << 5 | (py & 1) << 4 | (a & 2) << 2 | (py & 2) << 1 | (a & 4) >> 1 | (py & 4) >> 2) * 4 + 2;
            out[i] = (cell < level) == lighter ? 0 : -1;
        }
        holes = v8iOr(holes, v8iLoad(out));
    }
    return holes;
}

static inline void rasterizeSamples(const ArtfSoftFrame* f, ArtfWorker* w, const ArtfRasterTri* t, uint32_t id, int ox, int oy) {
    ShadeContext setup;
    const bool masked = f->visible[t->instance].masked;
    if (masked) shadeSetup(&setup, f, t, ox, oy);
    if (f->samples == 1) {
        if (masked) rasterize(w->depth, w->ids, t, id, ox, oy, 0, 0, &setup);
        else rasterize(w->depth, w->ids, t, id, ox, oy, 0, 0, NULL);
        return;
    }
    for (int k = 0; k < f->samples; ++k) rasterize(w->depth + k * ARTF_TILE_PIXELS, w->ids + k * ARTF_TILE_PIXELS, t, id, ox, oy, sampleX[k], sampleY[k], masked ? &setup : NULL);
}

/* Colors all the pixels of a plane that show triangle id: a whole block of eight at once, the others by
 * way of the batch. first: the first sample's ids, where what shows the same there is left alone. */
static void shadeTriangle(ArtfSoftFrame* f, LitBatch* batch, const ArtfRasterTri* t, uint32_t id, int ox, int oy, const uint32_t* plane, const uint32_t* first, uint32_t* color) {
    ShadeContext s;
    shadeSetup(&s, f, t, ox, oy);
    const ArtfFrameData* frame = &f->data;

    if (batch->material != s.material || batch->colored != s.colored) {
        batchFlush(batch, f, color);
        batch->material = s.material;
        batch->texture = s.texture;
        batch->colored = s.colored;
        batch->bumpy = s.bumpy;
        batch->textures = f->scene->textures.items;
    }
    batch->layered = s.layered;
    batch->opacity = s.opacity;

    const V8F lanes = v8iToF(v8iIota());
    const V8I idv = v8iSet((int32_t)id);
    const float inA0 = s.A[0], inA1 = s.A[1], inA2 = s.A[2], inB0 = s.B[0], inB1 = s.B[1], inB2 = s.B[2], inC0 = s.C[0], inC1 = s.C[1], inC2 = s.C[2];
    const int bx0 = (t->minX > ox ? t->minX - ox : 0) & ~7, by0 = t->minY > oy ? t->minY - oy : 0;
    const int bx1 = t->maxX - ox < ARTF_TILE - 1 ? t->maxX - ox : ARTF_TILE - 1, by1 = t->maxY - oy < ARTF_TILE - 1 ? t->maxY - oy : ARTF_TILE - 1;

    for (int y = by0; y <= by1; ++y) {
        const uint32_t* ids = plane + y * ARTF_TILE;
        const float fy = (float)y;
        for (int x = bx0; x <= bx1; x += 8) {
            const V8I here = v8iLoad(ids + x);
            int mine = v8iSigns(v8iEq(here, idv));
            if (mine && first) mine &= ~v8iSigns(v8iEq(here, v8iLoad(first + y * ARTF_TILE + x)));
            if (!mine) continue;
            if (mine == 0xFF) {
                const V8F fx = v8fAdd(lanes, v8fSet((float)x));
                V8F b0 = v8fAdd(v8fAdd(v8fMul(v8fSet(inA0), fx), v8fSet(inB0 * fy)), v8fSet(inC0));
                V8F b1 = v8fAdd(v8fAdd(v8fMul(v8fSet(inA1), fx), v8fSet(inB1 * fy)), v8fSet(inC1));
                V8F b2 = v8fAdd(v8fAdd(v8fMul(v8fSet(inA2), fx), v8fSet(inB2 * fy)), v8fSet(inC2));
                V8F inv = v8fDiv(v8fSet(1.f), v8fAdd(v8fAdd(b0, b1), b2));
                b0 = v8fMul(b0, inv);
                b1 = v8fMul(b1, inv);
#define LERP(a, c) v8fAdd(v8fAdd(v8fMul(b0, v8fSet(s.a[0][c] - s.a[2][c])), v8fMul(b1, v8fSet(s.a[1][c] - s.a[2][c]))), v8fSet(s.a[2][c]))
                Lanes3 n = {LERP(nrm, 0), LERP(nrm, 1), LERP(nrm, 2)};
                Lanes3 world = {LERP(wp, 0), LERP(wp, 1), LERP(wp, 2)};
                const V8F one = v8fSet(1.f);
                Lanes3 tint = {one, one, one};
                V8F alpha = v8fSet(s.opacity);
                if (s.colored) {
                    tint.x = LERP(col, 0);
                    tint.y = LERP(col, 1);
                    tint.z = LERP(col, 2);
                    alpha = v8fMul(alpha, LERP(col, 3));
                }
                Lanes3 tangent = {v8fSet(s.tangent[0]), v8fSet(s.tangent[1]), v8fSet(s.tangent[2])}, bitangent = {v8fSet(s.bitangent[0]), v8fSet(s.bitangent[1]), v8fSet(s.bitangent[2])};
                storeLit(f, color + y * ARTF_TILE + x, lightLanes(f, frame, s.material, f->scene->textures.items, s.texture, s.lights, batch->extra, mipAt(&s, (float)x + 3.5f, fy), v8fSet(s.turning ? turnAt(&s, (float)x + 3.5f, fy) : 0.f), n, world, LERP(uv, 0), LERP(uv, 1), s.colored, s.layered, tint, alpha, &tangent, &bitangent, s.who, s.drawn));
#undef LERP
                continue;
            }
            for (int lane = 0; mine; ++lane, mine >>= 1) {
                if (!(mine & 1)) continue;
                const float fx = (float)(x + lane);
                float b0 = inA0 * fx + inB0 * fy + inC0, b1 = inA1 * fx + inB1 * fy + inC1, b2 = inA2 * fx + inB2 * fy + inC2;
                const float inv = 1.f / (b0 + b1 + b2);
                b0 *= inv;
                b1 *= inv;
                const int i = batch->count;
                if (!i) batch->level = mipAt(&s, fx, fy);
#define LERP(a, c) (b0 * (s.a[0][c] - s.a[2][c]) + b1 * (s.a[1][c] - s.a[2][c]) + s.a[2][c])
                batch->turn[i] = s.turning ? turnAt(&s, fx, fy) : 0.f;
                batch->nx[i] = LERP(nrm, 0);
                batch->ny[i] = LERP(nrm, 1);
                batch->nz[i] = LERP(nrm, 2);
                if (s.needWorld) {
                    batch->wx[i] = LERP(wp, 0);
                    batch->wy[i] = LERP(wp, 1);
                    batch->wz[i] = LERP(wp, 2);
                }
                if (s.sizing || s.material->graph) {
                    batch->u[i] = LERP(uv, 0);
                    batch->v[i] = LERP(uv, 1);
                }
                if (s.bumpy) {
                    batch->tx[i] = s.tangent[0]; batch->ty[i] = s.tangent[1]; batch->tz[i] = s.tangent[2];
                    batch->bx[i] = s.bitangent[0]; batch->by[i] = s.bitangent[1]; batch->bz[i] = s.bitangent[2];
                }
                if (s.colored) {
                    batch->r[i] = LERP(col, 0);
                    batch->g[i] = LERP(col, 1);
                    batch->b[i] = LERP(col, 2);
                    batch->a[i] = LERP(col, 3);
                }
#undef LERP
                batch->at[i] = (uint16_t)(y * ARTF_TILE + x + lane);
                if (f->view) batch->who[i] = s.who[0];
                batch->drawn[i] = s.drawn[0];
                batch->lights |= s.lights;
                if ((batch->count = i + 1) == 8) batchFlush(batch, f, color);
            }
        }
    }
}

/* The triangles that show in a plane, into its colors, each pixel once. */
/* rows: those from y0 to y1 (exclusive) that the triangles can be in. */
static void shadePlane(ArtfSoftFrame* f, ArtfWorker* w, const ArtfRasterTri* const* tris, size_t count, const uint32_t* plane, const uint32_t* first, uint32_t* color, int ox, int oy, int tw, int y0, int y1) {
    artfVecReserve(&w->seen, count + 1);
    memset(w->seen.items, 0, count + 1);
    uint8_t* seen = w->seen.items;
    for (int y = y0; y < y1; ++y) {
        const uint32_t* ids = plane + y * ARTF_TILE;
        const uint32_t* same = first ? first + y * ARTF_TILE : NULL;
        uint32_t last = 0;
        for (int x = 0; x < tw; ++x)
            if (ids[x] != last && (!same || ids[x] != same[x])) { last = ids[x]; seen[last] = 1; }
    }
    LitBatch batch;
    batch.count = 0;
    batch.lights = 0;
    batch.extra = f->extraCount ? f->tileLights + (size_t)((oy / ARTF_TILE) * f->tilesX + ox / ARTF_TILE) * (1 + f->extraCount) : NULL;
    batch.material = NULL;
    batch.texture = NULL;
    batch.level = 0;
    batch.colored = false;
    batch.bumpy = false;
    batch.textures = f->scene->textures.items;
    batch.layered = false;
    batch.opacity = 1.f;
    /* (read before written, in what a material without highlights does not look at) */
    memset(batch.wx, 0, sizeof batch.wx * 3 + sizeof batch.u * 2);
    for (size_t i = 1; i <= count; ++i)
        if (seen[i]) shadeTriangle(f, &batch, tris[i - 1], (uint32_t)i, ox, oy, plane, first, color);
    batchFlush(&batch, f, color);
}

/* ---- a background pass ---- */

V8_INLINE V8I backgroundHash(V8I x, V8I y, V8I z) {
    V8I h = v8iXor(v8iXor(v8iMul(x, v8iSet(73856093)), v8iMul(y, v8iSet(19349663))), v8iMul(z, v8iSet(83492791)));
    h = v8iMul(v8iXor(h, v8iShr(h, 15)), v8iSet(0x2C1B3C6D));
    h = v8iMul(v8iXor(h, v8iShr(h, 12)), v8iSet(0x297A2D39));
    return v8iXor(h, v8iShr(h, 15));
}

/* Light to what a screen can show, as the curve film stock has: a toe, and a shoulder that bends all
 * that is very bright to white, without clipping. (Narkowicz's fit to ACES.) */
V8_INLINE V8F backgroundToneMap(V8F x) {
    const V8F top = v8fMul(x, v8fAdd(v8fMul(x, v8fSet(2.51f)), v8fSet(.03f)));
    const V8F bottom = v8fAdd(v8fMul(x, v8fAdd(v8fMul(x, v8fSet(2.43f)), v8fSet(.59f))), v8fSet(.14f));
    return srgbEncodeLanes(v8fMin(v8fDiv(top, bottom), v8fSet(1.f)));
}

/* A tile's background: the light with its bloom, tone mapped. Glows fade over more pixels than a byte
 * has steps, so a little noise goes in to keep them from showing bands. */
static void backgroundFill(ArtfSoftFrame* f, ArtfWorker* w, int ox, int oy, int tw, int th) {
    const ArtfBackgroundBlock* h = &f->backgroundData;
    const float exposure = h->image[0];
    const uint32_t* level = h->bloom[0];
    const int bw = (int)level[1], bh = (int)level[2];
    const V8F lanes = v8iToF(v8iIota()), zero = v8fSet(0.f);
    const V8F spread = v8fSet(h->image[1] * (1.f - ARTF_BLOOM_FADE));
    const V8F lastX = v8fSet((float)(bw - 1));
    const V8F bytes = v8fSet(255.f), round = v8fSet(.5f);
    for (int y = 0; y < th; ++y) {
        const float sy = ((float)(oy + y) + .5f) * .5f - .5f, baseY = floorf(sy), partY = sy - baseY;
        const int y0 = artfClampi((int)baseY, 0, bh - 1), y1 = artfClampi((int)baseY + 1, 0, bh - 1);
        const V8F rowPart = v8fSet(partY);
        const size_t row = (size_t)(oy + y) * f->hdrStride + ox;
        for (int x = 0; x < tw; x += 8) {
            const V8F pixel = v8fAdd(lanes, v8fSet((float)(ox + x)));
            const V8F sx = v8fSub(v8fMul(v8fAdd(pixel, round), round), round);
            const V8F baseX = v8fFloor(sx), partX = v8fSub(sx, baseX);
            const V8I x0 = v8fToI(v8fMin(v8fMax(baseX, zero), lastX)), x1 = v8fToI(v8fMin(v8fMax(v8fAdd(baseX, v8fSet(1.f)), zero), lastX));
            const V8I top = v8iSet(y0 * bw), bottom = v8iSet(y1 * bw);
            V8F glows[3];
            for (int c = 0; c < 3; ++c) {
                const uint32_t* plane = (const uint32_t*)(f->bloom[c] + level[0]);
                const V8F a = v8iAsF(v8iGather(plane, v8iAdd(top, x0))), b = v8iAsF(v8iGather(plane, v8iAdd(top, x1)));
                const V8F d = v8iAsF(v8iGather(plane, v8iAdd(bottom, x0))), e = v8iAsF(v8iGather(plane, v8iAdd(bottom, x1)));
                const V8F upper = v8fAdd(a, v8fMul(v8fSub(b, a), partX)), lower = v8fAdd(d, v8fMul(v8fSub(e, d), partX));
                glows[c] = v8fAdd(upper, v8fMul(v8fSub(lower, upper), rowPart));
            }
            /* a lens's glow is nearly white: the colors of what makes it run together in it */
            const V8F grey = v8fAdd(v8fAdd(v8fMul(glows[0], v8fSet(.3f)), v8fMul(glows[1], v8fSet(.59f))), v8fMul(glows[2], v8fSet(.11f)));
            V8F out[3];
            for (int c = 0; c < 3; ++c) {
                const V8F glow = v8fAdd(glows[c], v8fMul(v8fSub(grey, glows[c]), v8fSet(ARTF_BLOOM_WHITE)));
                const V8F light = v8fAdd(v8fLoad(f->hdr[c] + row + x), v8fMul(glow, spread));
                out[c] = backgroundToneMap(v8fMul(light, v8fSet(exposure)));
            }
            const V8I id = v8iAdd(v8iAdd(v8iSet(ox + x), v8iIota()), v8iSet((oy + y) * 8192));
            const V8I noiseBits = backgroundHash(id, v8iSet(7), v8iSet(13));
            const V8F dither = v8fMul(v8iToF(v8iAnd(noiseBits, v8iSet(255))), v8fSet(1.f / 255.f));
#define BYTE(v) v8fToI(v8fMin(v8fAdd(v8fMul(v, bytes), dither), bytes))
            const V8I pixelOut = v8iOr(v8iOr(v8iShl(BYTE(out[0]), 16), v8iShl(BYTE(out[1]), 8)), v8iOr(BYTE(out[2]), v8iSet((int32_t)0xFF000000u)));
#undef BYTE
            v8iStore(w->color + y * ARTF_TILE + x, pixelOut);
        }
    }
}

/* ---- a tile -------------------------------------------------------------------------------------- */

/* byte x rest / 255, rounded. */
V8_INLINE V8I timesRest(V8I byte, V8I rest) {
    const V8I t = v8iAdd(v8iMul(byte, rest), v8iSet(128));
    return v8iShr(v8iAdd(t, v8iShr(t, 8)), 8);
}

/* dst = src + dst (1 - alpha), alpha too: what is on top is premultiplied. */
V8_INLINE V8I overlayBlend(V8I src, V8I dst) {
    const V8I byteMask = v8iSet(255);
    V8I rest = v8iSub(byteMask, v8iShr(src, 24));
#define CHANNEL(shift) timesRest(v8iAnd(v8iShr(dst, shift), byteMask), rest)
    V8I a = v8iShl(CHANNEL(24), 24), r = v8iShl(CHANNEL(16), 16), g = v8iShl(CHANNEL(8), 8), b = CHANNEL(0);
#undef CHANNEL
    /* (no channel of a premultiplied pixel goes past its alpha, so the sums stay in their bytes) */
    return v8iAdd(src, v8iOr(v8iOr(a, r), v8iOr(g, b)));
}

/* ---- a tile of a frame in floats ---- */

/* What the tile's colors were made of before: a background pass's, already tone mapped, back to light. */
static void postFromBytes(uint32_t* c, int count) {
    const V8F inverse = v8fSet(1.f / 255.f);
    const V8I byte = v8iSet(255);
    for (int i = 0; i < count; i += 8) {
        const V8I p = v8iLoad(c + i);
        const V8F a = v8fMul(v8iToF(v8iShr(p, 24)), inverse);
        const V8F r = v8fMul(v8iToF(v8iAnd(v8iShr(p, 16), byte)), inverse), g = v8fMul(v8iToF(v8iAnd(v8iShr(p, 8), byte)), inverse), b = v8fMul(v8iToF(v8iAnd(p, byte)), inverse);
        /* (premultiplied in the frame's colors: light is what they show over alpha, times alpha) */
        const V8F over = v8fDiv(v8fSet(1.f), v8fMax(a, v8fSet(1e-6f)));
        const Lanes3 light = {v8fMul(srgbDecodeLanes(v8fMul(r, over)), a), v8fMul(srgbDecodeLanes(v8fMul(g, over)), a), v8fMul(srgbDecodeLanes(v8fMul(b, over)), a)};
        v8iStore(c + i, pack9e5(light));
        v8fStore((float*)(c + ARTF_POST_ALPHA + i), a);
        v8iStore(c + ARTF_POST_SURFACE + i, v8iSet(0));
        v8iStore(c + ARTF_POST_UNDER + i, v8iSet(0));
    }
}

/* top over under, both premultiplied, in light. */
V8_INLINE void postBlend(uint32_t* under, const uint32_t* top, V8F topAlpha) {
    const Lanes3 a = unpack9e5(v8iLoad(top)), b = unpack9e5(v8iLoad(under));
    const V8F rest = v8fSub(v8fSet(1.f), topAlpha);
    const Lanes3 out = {v8fAdd(a.x, v8fMul(b.x, rest)), v8fAdd(a.y, v8fMul(b.y, rest)), v8fAdd(a.z, v8fMul(b.z, rest))};
    v8iStore(under, pack9e5(out));
    float* alpha = (float*)(under + ARTF_POST_ALPHA);
    v8fStore(alpha, v8fAdd(topAlpha, v8fMul(v8fLoad(alpha), rest)));
}

/* Each layer of a tile, the farthest first. It takes its depth among what is there, so that only its
 * nearest surface shows, and goes over the colors of each sample. */
static void drawLayers(ArtfSoftFrame* f, ArtfWorker* w, int ox, int oy, int tw, int th) {
    const V8I zeroI = v8iSet(0);
    const ArtfRasterTri* const* tris = w->layers.items;
    const size_t count = w->layers.len;
    for (size_t at = 0, end; at < count; at = end) {
        /* (only the rows its triangles reach: a particle is small) */
        int y0 = th, y1 = 0;
        for (end = at; end < count && tris[end]->instance == tris[at]->instance; ++end) {
            y0 = artfMini(y0, artfMaxi(tris[end]->minY - oy, 0));
            y1 = artfMaxi(y1, artfMini(tris[end]->maxY - oy + 1, th));
        }
        if (y0 >= y1) continue;
        for (int k = 0; k < f->samples; ++k)
            for (int i = y0 * ARTF_TILE; i < y1 * ARTF_TILE; i += 8) v8iStore(w->ids + k * ARTF_TILE_PIXELS + i, zeroI);
        for (size_t i = at; i < end; ++i) rasterizeSamples(f, w, tris[i], (uint32_t)(i - at + 1), ox, oy);
        for (int k = 0; k < f->samples; ++k) {
            const size_t plane = (size_t)k * ARTF_TILE_PIXELS;
            shadePlane(f, w, tris + at, end - at, w->ids + plane, k ? w->ids : NULL, w->layer + plane, ox, oy, tw, y0, y1);
        }
        for (int k = 0; k < f->samples; ++k) {
            const size_t plane = (size_t)k * ARTF_TILE_PIXELS;
            for (int i = y0 * ARTF_TILE; i < y1 * ARTF_TILE; i += 8) {
                const V8I id = v8iLoad(w->ids + plane + i), empty = v8iEq(id, zeroI);
                if (v8iSigns(empty) == 0xFF) continue;
                const V8I under = v8iLoad(w->color + plane + i);
                /* (what shows the same as in the first sample was shaded there) */
                const V8I fromFirst = k ? v8iEq(id, v8iLoad(w->ids + i)) : v8iSet(-1);
                if (f->post) {
                    _Alignas(32) uint32_t top[8];
                    const V8F topAlpha = v8fSelect(empty, v8fSelect(fromFirst, v8fLoad((const float*)(w->layer + ARTF_POST_ALPHA + plane + i)), v8fLoad((const float*)(w->layer + ARTF_POST_ALPHA + i))), v8fSet(0.f));
                    v8iStore(top, v8iSelect(empty, v8iSelect(fromFirst, v8iLoad(w->layer + plane + i), v8iLoad(w->layer + i)), v8iSet(0)));
                    postBlend(w->color + plane + i, top, topAlpha);
                    continue;
                }
                const V8I src = k ? v8iSelect(fromFirst, v8iLoad(w->layer + plane + i), v8iLoad(w->layer + i)) : v8iLoad(w->layer + i);
                v8iStore(w->color + plane + i, v8iSelect(empty, overlayBlend(src, under), under));
            }
        }
    }
}

/* Layers by each pixel's depth, by transmittance (artfLayerResolve): each layer's surfaces, peeled one after another
 * (the nearest against what is solid, the next behind it), and what each leaves goes in the tile's grid; then each
 * goes in its sample's sums, by its weight, and the sums over the frame. (The grid's columns are in the tile: a tile
 * is so many of them.) */
static void drawLayersSorted(ArtfSoftFrame* f, ArtfWorker* w, int ox, int oy, int tw, int th) {
    enum { COLUMNS = ARTF_TILE / ARTF_LAYER_TILE, PEELS = 16 /* surfaces of one layer in a sample, at most */ };
    _Static_assert(ARTF_TILE % ARTF_LAYER_TILE == 0, "a tile, so many columns of the grid");
    (void)oy;
    const size_t all = (size_t)ARTF_TILE_PIXELS * (size_t)f->samples;
    if (!w->solidDepth) {
        w->solidDepth = artfMalloc(all * 4);
        w->layerGrid = artfMalloc((size_t)COLUMNS * COLUMNS * ARTF_LAYER_SLICES * 4);
        w->layerSums = artfMalloc(all * ARTF_LAYER_SUMS * 4);
        w->layerBelow = artfMalloc(all * 4);
    }
    memcpy(w->solidDepth, w->depth, all * 4);
    memset(w->layerGrid, 0, (size_t)COLUMNS * COLUMNS * ARTF_LAYER_SLICES * 4);
    w->layerBits.len = 0;
    const float* range = f->data.layers;
    const V8I zeroI = v8iSet(0);
    const ArtfRasterTri* const* tris = w->layers.items;
    const size_t count = w->layers.len;
    (void)ox;
    for (size_t at = 0, end; at < count; at = end) {
        /* (only the rows its triangles reach) */
        int y0 = th, y1 = 0;
        for (end = at; end < count && tris[end]->instance == tris[at]->instance; ++end) {
            y0 = artfMini(y0, artfMaxi(tris[end]->minY - oy, 0));
            y1 = artfMaxi(y1, artfMini(tris[end]->maxY - oy + 1, th));
        }
        if (y0 >= y1) continue;
        const size_t rows = (size_t)(y1 - y0) * ARTF_TILE;
        /* each of its surfaces, the nearest first: each time against what is solid and behind the surfaces before */
        for (int peel = 0; peel < PEELS; ++peel) {
            /* (whether two of its triangles met in a sample: if not, there is nothing behind for another time) */
            bool met = false;
            for (int k = 0; k < f->samples; ++k) {
                const size_t from = (size_t)k * ARTF_TILE_PIXELS + (size_t)y0 * ARTF_TILE;
                memcpy(w->depth + from, w->solidDepth + from, rows * 4);
                for (size_t p = 0; p < rows; p += 8) v8iStore(w->ids + from + p, zeroI);
            }
            for (size_t i = at; i < end; ++i) {
                const ArtfRasterTri* t = tris[i];
                ShadeContext setup;
                const bool masked = f->visible[t->instance].masked;
                if (masked) shadeSetup(&setup, f, t, ox, oy);
                for (int k = 0; k < f->samples; ++k) {
                    const size_t plane = (size_t)k * ARTF_TILE_PIXELS;
                    rasterizeBelow(w->depth + plane, w->ids + plane, t, (uint32_t)(i - at + 1), ox, oy, f->samples > 1 ? sampleX[k] : 0, f->samples > 1 ? sampleY[k] : 0, masked ? &setup : NULL, peel ? w->layerBelow + plane : NULL, &met);
                }
            }
            bool found = false;
            for (int k = 0; k < f->samples && !found; ++k) {
                const size_t from = (size_t)k * ARTF_TILE_PIXELS + (size_t)y0 * ARTF_TILE;
                for (size_t p = 0; p < rows && !found; p += 8) found = v8iSigns(v8iEq(v8iLoad(w->ids + from + p), zeroI)) != 0xFF;
            }
            if (!found) break;
            for (int k = 0; k < f->samples; ++k) {
                const size_t plane = (size_t)k * ARTF_TILE_PIXELS;
                shadePlane(f, w, tris + at, end - at, w->ids + plane, k ? w->ids : NULL, w->layer + plane, ox, oy, tw, y0, y1);
            }
            for (int k = 0; k < f->samples; ++k) {
                const size_t plane = (size_t)k * ARTF_TILE_PIXELS;
                for (int p = y0 * ARTF_TILE; p < y1 * ARTF_TILE; ++p) {
                    const uint32_t id = w->ids[plane + p];
                    if (!id) continue;
                    /* (what shows the same as in the first sample was shaded there) */
                    const uint32_t color = k && w->ids[p] == id ? w->layer[p] : w->layer[plane + p];
                    if (!(color >> 24)) continue;
                    const uint32_t taken = f->layerTaken[color >> 24], slice = artfLayerSlice(range, w->depth[plane + p]);
                    const uint32_t column = (uint32_t)((p / ARTF_TILE / ARTF_LAYER_TILE) * COLUMNS + p % ARTF_TILE / ARTF_LAYER_TILE);
                    w->layerGrid[column * ARTF_LAYER_SLICES + slice] += taken;
                    artfVecReserve(&w->layerBits, w->layerBits.len + 1);
                    w->layerBits.items[w->layerBits.len].at = (uint32_t)(plane + (size_t)p);
                    w->layerBits.items[w->layerBits.len].color = color;
                    w->layerBits.items[w->layerBits.len].taken = taken;
                    w->layerBits.items[w->layerBits.len++].slice = slice;
                }
            }
            if (!met) break;
            /* (the next surface is behind this one; where there was none, behind what is solid: nothing) */
            for (int k = 0; k < f->samples; ++k) {
                const size_t from = (size_t)k * ARTF_TILE_PIXELS + (size_t)y0 * ARTF_TILE;
                memcpy(w->layerBelow + from, w->depth + from, rows * 4);
            }
        }
    }
    memcpy(w->depth, w->solidDepth, all * 4);
    if (!w->layerBits.len) return;
    /* each column's cells: what those in front of each took */
    for (int c = 0; c < COLUMNS * COLUMNS; ++c) {
        uint32_t* cells = w->layerGrid + (size_t)c * ARTF_LAYER_SLICES;
        uint32_t before = 0;
        for (int k = 0; k < ARTF_LAYER_SLICES; ++k) {
            const uint32_t here = cells[k];
            cells[k] = before;
            before += here;
        }
    }
    /* and each cell's weight, by that: artfLayerWeight, eight at a time, the same arithmetic in the same order */
    {
        const V8F perSample = v8fSet(1.f / (ARTF_LAYER_ONE * (float)((uint32_t)f->samples * ARTF_LAYER_TILE * ARTF_LAYER_TILE)));
        const V8F one = v8fSet(ARTF_LAYER_ONE), halfF = v8fSet(.5f), zeroF = v8fSet(0.f);
        for (int c = 0; c < COLUMNS * COLUMNS * ARTF_LAYER_SLICES; c += 8) {
            const V8F mean = v8fMul(v8iToF(v8iLoad(w->layerGrid + c)), perSample);
            const V8I weight = v8fToI(v8fAdd(v8fMul(gradeExp2(v8fSub(zeroF, mean)), one), halfF));
            v8iStore(w->layerGrid + c, v8iSelect(v8iEq(weight, zeroI), weight, v8iSet(1)));
        }
    }
    /* (the sums in planes: red, green, blue, alpha, optical depth) */
    uint32_t* const sums[ARTF_LAYER_SUMS] = {w->layerSums, w->layerSums + all, w->layerSums + all * 2, w->layerSums + all * 3, w->layerSums + all * 4};
    memset(w->layerSums, 0, all * ARTF_LAYER_SUMS * 4);
    for (size_t i = 0; i < w->layerBits.len; ++i) {
        const uint32_t at = w->layerBits.items[i].at, color = w->layerBits.items[i].color, p = at % ARTF_TILE_PIXELS;
        const uint32_t column = (p / ARTF_TILE / ARTF_LAYER_TILE) * COLUMNS + p % ARTF_TILE / ARTF_LAYER_TILE;
        const uint32_t weight = w->layerGrid[column * ARTF_LAYER_SLICES + w->layerBits.items[i].slice];
        sums[0][at] += (color >> 16 & 255) * weight;
        sums[1][at] += (color >> 8 & 255) * weight;
        sums[2][at] += (color & 255) * weight;
        sums[3][at] += (color >> 24) * weight;
        sums[4][at] += w->layerBits.items[i].taken;
    }
    /* each sample's sums over it: artfLayerResolve, eight at a time, the same arithmetic in the same order */
    const V8F scaleT = v8fSet(-1.f / ARTF_LAYER_ONE), c255 = v8fSet(255.f), half = v8fSet(.5f), oneF = v8fSet(1.f);
    const V8I byte = v8iSet(255), round = v8iSet(128);
    for (size_t i = 0; i < all; i += 8) {
        const V8I a = v8iLoad(sums[3] + i), none = v8iEq(a, zeroI);
        if (v8iSigns(none) == 0xFF) continue;
        const V8F cover = v8fSub(oneF, gradeExp2(v8fMul(v8iToF(v8iLoad(sums[4] + i)), scaleT)));
        const V8F coverBytes = v8fMul(cover, c255);
        const V8F scale = v8fDiv(coverBytes, v8iToF(v8iSelect(none, a, v8iSet(1))));
        const V8I alpha = v8fToI(v8fAdd(coverBytes, half));
        V8I top = v8iShl(alpha, 24);
        for (int k = 0; k < 3; ++k) top = v8iOr(top, v8iShl(v8fToI(v8fMin(v8fAdd(v8fMul(v8iToF(v8iLoad(sums[k] + i)), scale), half), c255)), 16 - 8 * k));
        const V8I under = v8iLoad(w->color + i), rest = v8iSub(byte, alpha);
        V8I blended = v8iSet(0);
        for (int k = 0; k < 4; ++k) {
            V8I t = v8iAdd(v8iMul(v8iAnd(v8iShr(under, 8 * k), byte), rest), round);
            t = v8iShr(v8iAdd(t, v8iShr(t, 8)), 8);
            blended = v8iOr(blended, v8iShl(t, 8 * k));
        }
        v8iStore(w->color + i, v8iSelect(none, v8iAdd(top, blended), under));
    }
}

/* The mean of a pixel's four samples, into the first plane: they are premultiplied, so this holds for
 * alpha too, and an edge over nothing comes out as part of a pixel. */
static void resolveSamples(const ArtfSoftFrame* f, ArtfWorker* w) {
    if (f->post) {
        uint32_t* c = w->color;
        const V8F quarter = v8fSet(.25f);
        for (int i = 0; i < ARTF_TILE_PIXELS; i += 8) {
            Lanes3 sum = unpack9e5(v8iLoad(c + i));
            V8F alpha = v8fLoad((const float*)(c + ARTF_POST_ALPHA + i));
            for (int k = 1; k < 4; ++k) {
                const Lanes3 s = unpack9e5(v8iLoad(c + k * ARTF_TILE_PIXELS + i));
                sum.x = v8fAdd(sum.x, s.x);
                sum.y = v8fAdd(sum.y, s.y);
                sum.z = v8fAdd(sum.z, s.z);
                alpha = v8fAdd(alpha, v8fLoad((const float*)(c + ARTF_POST_ALPHA + k * ARTF_TILE_PIXELS + i)));
            }
            const Lanes3 mean = {v8fMul(sum.x, quarter), v8fMul(sum.y, quarter), v8fMul(sum.z, quarter)};
            v8iStore(c + i, pack9e5(mean));
            v8fStore((float*)(c + ARTF_POST_ALPHA + i), v8fMul(alpha, quarter));
        }
        return;
    }
    const V8I mask = v8iSet(0x00FF00FF), rounding = v8iSet(0x00020002);
    uint32_t* c = w->color;
    for (int i = 0; i < ARTF_TILE_PIXELS; i += 8) {
        const V8I p0 = v8iLoad(c + i), p1 = v8iLoad(c + ARTF_TILE_PIXELS + i), p2 = v8iLoad(c + ARTF_TILE_PIXELS * 2 + i), p3 = v8iLoad(c + ARTF_TILE_PIXELS * 3 + i);
        /* (two bytes at a time, as they fit in the gaps) */
        const V8I even = v8iAdd(v8iAdd(v8iAnd(p0, mask), v8iAnd(p1, mask)), v8iAdd(v8iAnd(p2, mask), v8iAnd(p3, mask)));
        const V8I odd = v8iAdd(v8iAdd(v8iAnd(v8iShr(p0, 8), mask), v8iAnd(v8iShr(p1, 8), mask)), v8iAdd(v8iAnd(v8iShr(p2, 8), mask), v8iAnd(v8iShr(p3, 8), mask)));
        v8iStore(c + i, v8iOr(v8iAnd(v8iShr(v8iAdd(even, rounding), 2), mask), v8iShl(v8iAnd(v8iShr(v8iAdd(odd, rounding), 2), mask), 8)));
    }
}

/* The overdraw view (ArtfView): in place of shading, how many triangles cover each sample, solid and layers, whatever
 * is in front of them; as light where the frame is in floats. The same edges as rasterize, one sample at a time. */
static void overdrawTile(const ArtfSoftFrame* f, ArtfWorker* w, int ox, int oy, int tw, int th) {
    uint16_t counts[ARTF_TILE_PIXELS];
    for (int k = 0; k < f->samples; ++k) {
        memset(counts, 0, sizeof counts);
        const int sx = f->samples > 1 ? sampleX[k] : 0, sy = f->samples > 1 ? sampleY[k] : 0;
        for (int list = 0; list < 2; ++list) {
            const ArtfRasterTri* const* tris = list ? w->layers.items : w->tris.items;
            const size_t count = list ? w->layers.len : w->tris.len;
            for (size_t i = 0; i < count; ++i) {
                const ArtfRasterTri* t = tris[i];
                const int x0 = artfMaxi(t->minX - ox, 0), x1 = artfMini(t->maxX - ox, tw - 1), y0 = artfMaxi(t->minY - oy, 0), y1 = artfMini(t->maxY - oy, th - 1);
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const int64_t px = (int64_t)(ox + x) * 16 + 8 + sx, py = (int64_t)(oy + y) * 16 + 8 + sy;
                        int inside = 1;
                        for (int e = 0; e < 3 && inside; ++e) {
                            const int j = e == 2 ? 0 : e + 1;
                            const int64_t dx = (int64_t)t->x[j] - t->x[e], dy = (int64_t)t->y[j] - t->y[e];
                            const bool topLeft = dy < 0 || (dy == 0 && dx > 0);
                            inside = dx * (py - t->y[e]) - dy * (px - t->x[e]) - (topLeft ? 0 : 1) >= 0;
                        }
                        counts[y * ARTF_TILE + x] = (uint16_t)(counts[y * ARTF_TILE + x] + inside);
                    }
            }
        }
        uint32_t* color = w->color + (size_t)k * ARTF_TILE_PIXELS;
        for (int i = 0; i < ARTF_TILE_PIXELS; i += 8) {
            _Alignas(32) float heat[3][8];
            _Alignas(32) uint32_t bytes[8];
            for (int j = 0; j < 8; ++j) {
                float c[3];
                artfViewHeat(counts[i + j], c);
                for (int m = 0; m < 3; ++m) heat[m][j] = c[m];
                bytes[j] = 0xFF000000u | (uint32_t)(c[0] * 255.f + .5f) << 16 | (uint32_t)(c[1] * 255.f + .5f) << 8 | (uint32_t)(c[2] * 255.f + .5f);
            }
            if (!f->post) {
                v8iStore(color + i, v8iLoad((const uint32_t*)bytes));
                continue;
            }
            const Lanes3 light = {v8fLoad(heat[0]), v8fLoad(heat[1]), v8fLoad(heat[2])};
            v8iStore(color + i, pack9e5(light));
            v8fStore((float*)(color + ARTF_POST_ALPHA + i), v8fSet(1.f));
            v8iStore(color + ARTF_POST_SURFACE + i, v8iSet(0));
            v8iStore(color + ARTF_POST_UNDER + i, v8iSet(0));
        }
    }
}

static ArtfTileClass classifyTile(const ArtfSoftFrame* f, int ox, int oy, int tw, int th) {
    if (!f->overlay || ox >= f->overlayWidth || oy >= f->overlayHeight) return ARTF_TILE_EMPTY;
    const int ow = artfMini(tw, f->overlayWidth - ox), oh = artfMini(th, f->overlayHeight - oy);
    uint32_t any = 0, all = 0xFFFFFFFFu;
    for (int y = 0; y < oh; ++y) {
        const uint32_t* row = f->overlay + (size_t)(oy + y) * f->overlayStride + ox;
        for (int x = 0; x < ow; ++x) { any |= row[x]; all &= row[x]; }
    }
    if (!any) return ARTF_TILE_EMPTY;
    return (all >> 24) == 255 && ow == tw && oh == th ? ARTF_TILE_OPAQUE : ARTF_TILE_MIXED;
}

/* All of a tile's solid triangles, in the order of the chunks, into its depth and ids, and its layers'
 * into their list; false when it has none. */
static bool rasterizeTile(ArtfSoftFrame* f, ArtfWorker* w, uint32_t index, int ox, int oy) {
    w->tris.len = 0;
    w->layers.len = 0;
    const V8F zeroF = v8fSet(0.f);
    const V8I zeroI = v8iSet(0);
    bool cleared = false;
    for (uint32_t c = 0; c < ARTF_CHUNKS; ++c) {
        const ArtfChunk* chunk = &f->chunks[c];
        uint32_t block = chunk->heads[index][0];
        if (block && !cleared) {
            cleared = true;
            for (int i = 0; i < ARTF_TILE_PIXELS * f->samples; i += 8) {
                v8fStore(w->depth + i, zeroF);
                v8iStore(w->ids + i, zeroI);
            }
        }
        for (; block; block = chunk->blocks.items[block - 1].next) {
            const ArtfBinBlock* b = &chunk->blocks.items[block - 1];
            artfVecReserve(&w->tris, w->tris.len + b->count);
            for (uint32_t i = 0; i < b->count; ++i) {
                const ArtfRasterTri* t = &chunk->tris.items[b->tris[i]];
                if (t->instance >= f->solidCount) {
                    artfVecPush(&w->layers, t);
                    continue;
                }
                w->tris.items[w->tris.len++] = t;
                rasterizeSamples(f, w, t, (uint32_t)w->tris.len, ox, oy);
            }
        }
    }
    return cleared;
}

/* What occlusion tests against: how far away each cell of the tile reaches, its farthest pixel. A pixel
 * that nothing covers reaches to infinity, depth 0. Under an opaque overlay nothing can be seen at all,
 * so there the cells say that everything is hidden. */
static void depthTile(ArtfSoftFrame* f, uint32_t index, uint32_t workerIndex) {
    ArtfWorker* w = &f->workers[workerIndex];
    const int tx = (int)(index % (uint32_t)f->tilesX), ty = (int)(index / (uint32_t)f->tilesX);
    const int ox = tx * ARTF_TILE, oy = ty * ARTF_TILE;
    const int tw = artfMini(ARTF_TILE, f->width - ox), th = artfMini(ARTF_TILE, f->height - oy);
    if (f->tileDirty[index]) {
        f->tileClass[index] = (uint8_t)classifyTile(f, ox, oy, tw, th);
        f->tileDirty[index] = 0;
    }
    const bool opaque = f->tileClass[index] == ARTF_TILE_OPAQUE;
    const bool any = !opaque && rasterizeTile(f, w, index, ox, oy);
    float tileFar = 3e38f;
    for (int cy = 0; cy * ARTF_HIZ_CELL < th; ++cy)
        for (int cx = 0; cx * ARTF_HIZ_CELL < tw; ++cx) {
            float far = opaque ? 3e38f : 0.f;
            if (any) {
                const int x0 = cx * ARTF_HIZ_CELL, y0 = cy * ARTF_HIZ_CELL;
                const int x1 = artfMini(x0 + ARTF_HIZ_CELL, tw), y1 = artfMini(y0 + ARTF_HIZ_CELL, th);
                far = 3e38f;
                for (int k = 0; k < f->samples; ++k)
                    for (int y = y0; y < y1; ++y)
                        for (int x = x0; x < x1; ++x) far = artfMinf(far, w->depth[k * ARTF_TILE_PIXELS + y * ARTF_TILE + x]);
            }
            f->hizCells[(size_t)(ty * (ARTF_TILE / ARTF_HIZ_CELL) + cy) * f->cellsX + tx * (ARTF_TILE / ARTF_HIZ_CELL) + cx] = far;
            tileFar = artfMinf(tileFar, far);
        }
    f->hizTiles[index] = tileFar;
}

/* The picture of all around, behind the scene: as the GPU's backdrop draws it. */
static void environmentFill(const ArtfSoftFrame* f, ArtfWorker* w, int ox, int oy, int tw, int th) {
    const ArtfFrameData* d = &f->data;
    const float* env = f->scene->environment;
    const float light = d->sky[3], exposure = d->look[0], alpha = d->background[3];
    /* (orthographic: every ray goes the way the view looks, and sees the same) */
    const float spread = artfOrtho(d) ? 0.f : 1.f;
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            const float px = (((float)(ox + x) + .5f) / (float)f->width * 2.f - 1.f) * spread, py = (1.f - ((float)(oy + y) + .5f) / (float)f->height * 2.f) * spread;
            float c[3];
            artfEnvSample(env, 0, d->eye[2][0] + px * d->eye[0][0] + py * d->eye[1][0], d->eye[2][1] + px * d->eye[0][1] + py * d->eye[1][1],
                          d->eye[2][2] + px * d->eye[0][2] + py * d->eye[1][2], c);
            if (f->post) {
                /* light as it is, for what comes after */
                _Alignas(32) uint32_t packed[8];
                const Lanes3 light3 = {v8fSet(c[0] * light * alpha), v8fSet(c[1] * light * alpha), v8fSet(c[2] * light * alpha)};
                v8iStore(packed, pack9e5(light3));
                w->color[y * ARTF_TILE + x] = packed[0];
                ((float*)(w->color + ARTF_POST_ALPHA))[y * ARTF_TILE + x] = alpha;
                continue;
            }
            uint32_t bits = (uint32_t)(alpha * 255.f + .5f) << 24;
            for (int k = 0; k < 3; ++k) {
                float v = c[k] * light;
                if (exposure > 0) {
                    v *= exposure;
                    v = v * (v * 2.51f + .03f) / (v * (v * 2.43f + .59f) + .14f);
                }
                bits |= (uint32_t)(artfSrgbEncode(artfClampf(v, 0, 1)) * alpha * 255.f + .5f) << (16 - 8 * k);
            }
            w->color[y * ARTF_TILE + x] = bits;
        }
}

/* Fog that light goes through, over what is behind the scene, where nothing solid was drawn: the grid's farthest
 * slice, by how much shows. Each sample of each pixel, where its depth is still the cleared one. */
static void fogBackdrop(const ArtfSoftFrame* f, ArtfWorker* w, int ox, int oy, int tw, int th, bool drawn) {
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; x += 8) {
            bool any = !drawn;
            for (int k = 0; k < f->samples && !any; ++k) any = v8iSigns(v8fGt(v8fSet(1e-30f), v8fLoad(w->depth + k * ARTF_TILE_PIXELS + y * ARTF_TILE + x))) != 0;
            if (!any) continue;
            _Alignas(32) float got[4][8];
            for (int i = 0; i < 8; ++i) {
                float c[4];
                artfFroxelAt(f, (float)(ox + x + i), (float)(oy + y), 3e38f, c);
                for (int j = 0; j < 4; ++j) got[j][i] = c[j];
            }
            const V8F left = v8fLoad(got[3]);
            for (int k = 0; k < f->samples; ++k) {
                uint32_t* at = w->color + k * ARTF_TILE_PIXELS + y * ARTF_TILE + x;
                const V8F alpha = v8fLoad((const float*)(at + ARTF_POST_ALPHA));
                const V8I behind = drawn ? v8fGt(v8fSet(1e-30f), v8fLoad(w->depth + k * ARTF_TILE_PIXELS + y * ARTF_TILE + x)) : v8iSet(-1);
                const Lanes3 was = unpack9e5(v8iLoad(at));
                const Lanes3 now = {v8fAdd(v8fMul(was.x, left), v8fMul(v8fLoad(got[0]), alpha)), v8fAdd(v8fMul(was.y, left), v8fMul(v8fLoad(got[1]), alpha)),
                                    v8fAdd(v8fMul(was.z, left), v8fMul(v8fLoad(got[2]), alpha))};
                v8iStore(at, v8iSelect(behind, v8iLoad(at), pack9e5(now)));
            }
        }
}

/* Where what a pose bent was a frame ago, at a pixel of one of its triangles: its vertices where they were (their back),
 * through its instance as it was and the view before, at the point of the triangle the pixel's center sees; as
 * ARTF_MOTION_VECTOR's word. The GPU's vertex shader takes its vertices back as well, and its pixels interpolate. */
static uint32_t pixelMotion(const ArtfSoftFrame* f, const ArtfRasterTri* t, const float* back, int px, int py) {
    const ArtfVisible* v = &f->visible[t->instance];
    const ArtfMeshData* mesh = v->mesh;
    const ArtfMeshlet* l = &mesh->meshlets[(t->triangle & ~ARTF_TRI_BACK) >> 7];
    const uint8_t* local = mesh->meshletTriangles + ((size_t)l->triangleOffset + (t->triangle & 127)) * 3;
    const uint32_t stride = mesh->positionStride;
    const float* m = v->instance->m;
    const float* motion = f->motion + (size_t)v->index * 12;
    const float* previous = f->data.previous;
    float a[3][3], was[3][3];
    for (int k = 0; k < 3; ++k) {
        const uint32_t slot = l->vertexOffset + local[k];
        const float p[3] = {mesh->positions[slot], mesh->positions[stride + slot], mesh->positions[(size_t)stride * 2 + slot]};
        for (int r = 0; r < 3; ++r) a[k][r] = v->mvp[r * 4] * p[0] + v->mvp[r * 4 + 1] * p[1] + v->mvp[r * 4 + 2] * p[2] + v->mvp[r * 4 + 3];
        float q[3], w[3];
        for (int c = 0; c < 3; ++c) q[c] = p[c] + back[(size_t)slot * 3 + c];
        for (int r = 0; r < 3; ++r) w[r] = m[r * 4] * q[0] + m[r * 4 + 1] * q[1] + m[r * 4 + 2] * q[2] + m[r * 4 + 3];
        for (int r = 0; r < 3; ++r) q[r] = motion[r * 4] * w[0] + motion[r * 4 + 1] * w[1] + motion[r * 4 + 2] * w[2] + motion[r * 4 + 3];
        /* (x, y and w through the view before, by columns) */
        static const int rows[3] = {0, 1, 3};
        for (int r = 0; r < 3; ++r) was[k][r] = previous[rows[r]] * q[0] + previous[4 + rows[r]] * q[1] + previous[8 + rows[r]] * q[2] + previous[12 + rows[r]];
    }
    /* (where on the triangle: from its corners in clip space, which needs none in front of the eye) */
    const float nx = ((float)px + .5f) / (float)f->width * 2.f - 1.f, ny = 1.f - ((float)py + .5f) / (float)f->height * 2.f;
    float weight[3], sum = 0;
    for (int k = 0; k < 3; ++k) {
        const float* b = a[(k + 1) % 3];
        const float* c = a[(k + 2) % 3];
        weight[k] = nx * (b[1] * c[2] - b[2] * c[1]) + ny * (b[2] * c[0] - b[0] * c[2]) + (b[0] * c[1] - b[1] * c[0]);
        sum += weight[k];
    }
    if (sum == 0) return artfMotionPack(0, 0);
    float clip[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k)
        for (int r = 0; r < 3; ++r) clip[r] += was[k][r] * (weight[k] / sum);
    if (clip[2] <= 1e-5f) return artfMotionPack(1e5f, 1e5f);
    const float sx = (clip[0] / clip[2] * .5f + .5f) * (float)f->width - .5f, sy = (.5f - clip[1] / clip[2] * .5f) * (float)f->height - .5f;
    return artfMotionPack(sx - ((float)px - f->data.post[2][2]), sy - ((float)py + f->data.post[2][3]));
}

static uint32_t tile(ArtfSoftFrame* f, uint32_t index, uint32_t workerIndex) {
    ArtfWorker* w = &f->workers[workerIndex];
    const int ox = (int)(index % (uint32_t)f->tilesX) * ARTF_TILE, oy = (int)(index / (uint32_t)f->tilesX) * ARTF_TILE;
    const int tw = artfMini(ARTF_TILE, f->width - ox), th = artfMini(ARTF_TILE, f->height - oy);

    if (f->tileDirty[index]) {
        f->tileClass[index] = (uint8_t)classifyTile(f, ox, oy, tw, th);
        f->tileDirty[index] = 0;
    }
    const uint8_t klass = f->tileClass[index];

    /* under an opaque overlay nothing of the scene shows, so none of it is drawn */
    if (klass == ARTF_TILE_OPAQUE && f->post) {
        for (int y = 0; y < th; ++y) {
            const size_t row = (size_t)(oy + y) * f->width + ox;
            for (int c = 0; c < 4; ++c) memset(f->postLight[c] + row, 0, (size_t)tw * 4);
            memset(f->postDepth + row, 0, (size_t)tw * 4);
            memset(f->postSurface + row, 0, (size_t)tw * 4);
            memset(f->postUnder + row, 0, (size_t)tw * 4);
            memset(f->postInstance + row, 0, (size_t)tw * 4);
        }
        return 1;
    }
    if (klass == ARTF_TILE_OPAQUE) {
        for (int y = 0; y < th; ++y)
            memcpy(f->pixels + (size_t)(oy + y) * f->width + ox, f->overlay + (size_t)(oy + y) * f->overlayStride + ox, (size_t)tw * 4);
        return 1;
    }

    /* (a tile with nothing in it leaves its depth as the last tile had it: nothing reads it, but what comes after) */
    const bool drawn = rasterizeTile(f, w, index, ox, oy);

    const V8I zeroI = v8iSet(0);
    const V8I background = v8iSet((int32_t)f->background);
    if (f->backgroundPass) {
        backgroundFill(f, w, ox, oy, tw, th);
        if (f->post) postFromBytes(w->color, ARTF_TILE_PIXELS);
    } else if (f->post) {
        /* the background as light: what shows, as light */
        const float* bg = f->data.background;
        const float a = bg[3], over = a > 0 ? 1.f / a : 0.f;
        const Lanes3 light = {v8fSet(artfSrgbDecode(bg[0] * over) * a), v8fSet(artfSrgbDecode(bg[1] * over) * a), v8fSet(artfSrgbDecode(bg[2] * over) * a)};
        const V8I packed = pack9e5(light);
        for (int i = 0; i < ARTF_TILE_PIXELS; i += 8) {
            v8iStore(w->color + i, packed);
            v8fStore((float*)(w->color + ARTF_POST_ALPHA + i), v8fSet(a));
            v8iStore(w->color + ARTF_POST_SURFACE + i, v8iSet(0));
            v8iStore(w->color + ARTF_POST_UNDER + i, v8iSet(0));
        }
        if (f->data.sky[3] > 0 && f->data.ground[3] > 0 && f->scene->environment) environmentFill(f, w, ox, oy, tw, th);
    } else {
        for (int i = 0; i < ARTF_TILE_PIXELS; i += 8) v8iStore(w->color + i, background);
        if (f->data.sky[3] > 0 && f->data.ground[3] > 0 && f->scene->environment) environmentFill(f, w, ox, oy, tw, th);
    }
    for (int k = 1; k < f->samples; ++k) {
        memcpy(w->color + k * ARTF_TILE_PIXELS, w->color, ARTF_TILE_PIXELS * 4);
        if (f->post) {
            memcpy(w->color + ARTF_POST_ALPHA + k * ARTF_TILE_PIXELS, w->color + ARTF_POST_ALPHA, ARTF_TILE_PIXELS * 4);
            memcpy(w->color + ARTF_POST_SURFACE + k * ARTF_TILE_PIXELS, w->color + ARTF_POST_SURFACE, ARTF_TILE_PIXELS * 4);
            memcpy(w->color + ARTF_POST_UNDER + k * ARTF_TILE_PIXELS, w->color + ARTF_POST_UNDER, ARTF_TILE_PIXELS * 4);
        }
    }

    if (f->view == ARTF_VIEW_OVERDRAW) {
        overdrawTile(f, w, ox, oy, tw, th);
    } else if (w->tris.len) {
        /* the triangles that still show after all the others */
        for (int k = 0; k < f->samples; ++k) {
            const size_t plane = (size_t)k * ARTF_TILE_PIXELS;
            shadePlane(f, w, w->tris.items, w->tris.len, w->ids + plane, k ? w->ids : NULL, w->color + plane, ox, oy, tw, 0, th);
        }
        /* what shows the same as in the first sample takes its color from there */
        for (int k = 1; k < f->samples; ++k) {
            const size_t plane = (size_t)k * ARTF_TILE_PIXELS;
            for (int i = 0; i < ARTF_TILE_PIXELS; i += 8) {
                const V8I same = v8iEq(v8iLoad(w->ids + plane + i), v8iLoad(w->ids + i));
                v8iStore(w->color + plane + i, v8iSelect(same, v8iLoad(w->color + plane + i), v8iLoad(w->color + i)));
                if (f->post) {
                    float* alpha = (float*)(w->color + ARTF_POST_ALPHA);
                    v8fStore(alpha + plane + i, v8fSelect(same, v8fLoad(alpha + plane + i), v8fLoad(alpha + i)));
                    v8iStore(w->color + ARTF_POST_SURFACE + plane + i, v8iSelect(same, v8iLoad(w->color + ARTF_POST_SURFACE + plane + i), v8iLoad(w->color + ARTF_POST_SURFACE + i)));
                }
            }
        }
    }
    /* fog that light goes through, over what is behind the scene: where nothing solid is, before layers */
    if (f->postFog) fogBackdrop(f, w, ox, oy, tw, th, drawn);
    /* (what comes after reads the depth of what is solid: layers write theirs over it; and what instance shows) */
    if (f->post) {
        memcpy(w->postDepth, w->depth, ARTF_TILE_PIXELS * 4);
        /* (only when something moved, or a material is reactive: else no instance is needed) */
        if (f->motion || f->reactive) {
            /* (pixels side by side mostly show the same triangle: it is looked up once for them) */
            uint32_t last = ~0u, who = 0;
            const ArtfRasterTri* bent = NULL; /* (what a pose moved goes by its vertices) */
            const float* back = NULL;
            for (int i = 0; i < ARTF_TILE_PIXELS; ++i) {
                const uint32_t id = drawn ? w->ids[i] : 0;
                if (id != last) {
                    last = id;
                    who = id && id <= w->tris.len ? f->visible[w->tris.items[id - 1]->instance].index + 1 : 0;
                    bent = NULL;
                    if (who && f->motionBack && f->motionBack[who - 1]) {
                        const ArtfVisible* v = &f->visible[w->tris.items[id - 1]->instance];
                        if (v->mesh == &f->scene->meshes.items[v->instance->mesh]) {
                            bent = w->tris.items[id - 1];
                            back = f->motionBack[who - 1];
                        }
                    }
                }
                w->postInstance[i] = bent ? pixelMotion(f, bent, back, ox + (i & (ARTF_TILE - 1)), oy + i / ARTF_TILE) : who;
            }
        }
    }
    if (w->layers.len && f->view != ARTF_VIEW_OVERDRAW) {
        if (f->scene->features[ARTF_FEATURE_LAYERS_SORTED] && !f->post) {
            /* (particles of emitters of many come after the lists, each over what is there, as on the GPU) */
            const size_t all = w->layers.len;
            size_t split = 0;
            while (split < all && w->layers.items[split]->instance < f->swarmFirst) ++split;
            w->layers.len = split;
            if (split) drawLayersSorted(f, w, ox, oy, tw, th);
            w->layers.items += split;
            w->layers.len = all - split;
            drawLayers(f, w, ox, oy, tw, th);
            w->layers.items -= split;
            w->layers.len = all;
        } else {
            drawLayers(f, w, ox, oy, tw, th);
        }
    }
    if (f->tracing) {
        /* what rays gathered goes over the background, as a layer does */
        const float each = 1.f / (float)f->traceDone;
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                const float* sum = f->traceSum + ((size_t)(oy + y) * f->width + ox + x) * 4;
                if (sum[3] <= 0) continue;
                const float alpha = sum[3] * each, scale = 1.f / sum[3], exposure = f->data.look[0];
                if (f->post) {
                    /* light over what is there, as a layer: tone mapping comes after */
                    uint32_t* at = w->color + y * ARTF_TILE + x;
                    float* under = (float*)(at + ARTF_POST_ALPHA);
                    _Alignas(32) uint32_t one[8];
                    v8iStore(one, v8iLoad(at - (x & 7))); /* (the block this pixel is in) */
                    const Lanes3 was = unpack9e5(v8iLoad(one));
                    _Alignas(32) float r[8], g[8], b[8];
                    v8fStore(r, was.x); v8fStore(g, was.y); v8fStore(b, was.z);
                    const int lane = x & 7;
                    const Lanes3 now = {v8fSet(sum[0] * scale * alpha + r[lane] * (1.f - alpha)), v8fSet(sum[1] * scale * alpha + g[lane] * (1.f - alpha)), v8fSet(sum[2] * scale * alpha + b[lane] * (1.f - alpha))};
                    _Alignas(32) uint32_t packed[8];
                    v8iStore(packed, pack9e5(now));
                    *at = packed[0];
                    *under = alpha + *under * (1.f - alpha);
                    continue;
                }
                float light[3];
                for (int c = 0; c < 3; ++c) {
                    const float v = sum[c] * scale * (exposure > 0 ? exposure : 1.f);
                    light[c] = exposure > 0 ? v * (v * 2.51f + .03f) / (v * (v * 2.43f + .59f) + .14f) : v;
                }
                const uint32_t top = (uint32_t)(alpha * 255.f + .5f) << 24
                    | (uint32_t)(artfSrgbEncode(artfClampf(light[0], 0, 1)) * alpha * 255.f + .5f) << 16
                    | (uint32_t)(artfSrgbEncode(artfClampf(light[1], 0, 1)) * alpha * 255.f + .5f) << 8
                    | (uint32_t)(artfSrgbEncode(artfClampf(light[2], 0, 1)) * alpha * 255.f + .5f);
                const uint32_t d = w->color[y * ARTF_TILE + x], rest = 255 - (top >> 24);
                uint32_t t0 = (d & 255) * rest + 128, t1 = ((d >> 8) & 255) * rest + 128, t2 = ((d >> 16) & 255) * rest + 128, t3 = (d >> 24) * rest + 128;
                t0 = (t0 + (t0 >> 8)) >> 8;
                t1 = (t1 + (t1 >> 8)) >> 8;
                t2 = (t2 + (t2 >> 8)) >> 8;
                t3 = (t3 + (t3 >> 8)) >> 8;
                w->color[y * ARTF_TILE + x] = top + (t0 | t1 << 8 | t2 << 16 | t3 << 24);
            }
    }
    if (f->samples > 1 && (w->tris.len || w->layers.len || f->post)) resolveSamples(f, w);

    if (f->post) {
        /* out to the frame's planes, in light: the overlay goes on after what follows */
        for (int y = 0; y < th; ++y) {
            const size_t row = (size_t)(oy + y) * f->width + ox;
            const uint32_t* color = w->color + y * ARTF_TILE;
            for (int x = 0; x < tw; x += 8) {
                const Lanes3 c = unpack9e5(v8iLoad(color + x));
                _Alignas(32) float out[4][8], depth[8];
                v8fStore(out[0], c.x); v8fStore(out[1], c.y); v8fStore(out[2], c.z);
                v8fStore(out[3], v8fLoad((const float*)(color + ARTF_POST_ALPHA + x)));
                v8fStore(depth, drawn ? v8fLoad(w->postDepth + y * ARTF_TILE + x) : v8fSet(0.f));
                const int n = artfMini(8, tw - x);
                for (int k = 0; k < 4; ++k) memcpy(f->postLight[k] + row + x, out[k], (size_t)n * 4);
                memcpy(f->postDepth + row + x, depth, (size_t)n * 4);
                memcpy(f->postSurface + row + x, color + ARTF_POST_SURFACE + x, (size_t)n * 4);
                memcpy(f->postUnder + row + x, color + ARTF_POST_UNDER + x, (size_t)n * 4);
                if (f->motion || f->reactive) memcpy(f->postInstance + row + x, w->postInstance + y * ARTF_TILE + x, (size_t)n * 4);
            }
        }
        return 0;
    }
    /* the overlay, and out to the frame: the one time this tile's pixels touch main memory */
    const int overlayRows = klass == ARTF_TILE_EMPTY ? 0 : artfMini(th, f->overlayHeight - oy);
    const int overlayColumns = klass == ARTF_TILE_EMPTY ? 0 : artfMini(tw, f->overlayWidth - ox);
    for (int y = 0; y < th; ++y) {
        uint32_t* color = w->color + y * ARTF_TILE;
        uint32_t* out = f->pixels + (size_t)(oy + y) * f->width + ox;
        if (y < overlayRows) {
            const uint32_t* over = f->overlay + (size_t)(oy + y) * f->overlayStride + ox;
            int x = 0;
            for (; x + 8 <= overlayColumns; x += 8) {
                V8I src = v8iLoadU(over + x);
                if (v8iSigns(v8iEq(src, zeroI)) != 0xFF) v8iStore(color + x, overlayBlend(src, v8iLoad(color + x)));
            }
            for (; x < overlayColumns; ++x) {
                const uint32_t s = over[x], d = color[x], rest = 255 - (s >> 24);
                uint32_t t0 = (d & 255) * rest + 128, t1 = ((d >> 8) & 255) * rest + 128, t2 = ((d >> 16) & 255) * rest + 128, t3 = (d >> 24) * rest + 128;
                t0 = (t0 + (t0 >> 8)) >> 8;
                t1 = (t1 + (t1 >> 8)) >> 8;
                t2 = (t2 + (t2 >> 8)) >> 8;
                t3 = (t3 + (t3 >> 8)) >> 8;
                color[x] = s + (t0 | t1 << 8 | t2 << 16 | t3 << 24);
            }
        }
        memcpy(out, color, (size_t)tw * 4);
    }
    return 0;
}

/* ---- the look, after the frame ---------------------------------------------------------------- */

/* post.c's gradeShown, eight pixels at a time: the same arithmetic in the same order, each ternary a select, so that
 * what comes out is the same bits as one pixel at a time. */
V8_INLINE V8I gradeHash(V8I x) {
    x = v8iXor(x, v8iShr(x, 16));
    x = v8iMul(x, v8iSet((int32_t)0x7FEB352Du));
    x = v8iXor(x, v8iShr(x, 15));
    x = v8iMul(x, v8iSet((int32_t)0x846CA68Bu));
    return v8iXor(x, v8iShr(x, 16));
}

/* c: three planes of n colors as a screen shows them (n a multiple of 8, from 32-byte boundaries); at: the grain's
 * hash input of the first, those after it one more each. */
static void shownLanes(float* const* c, int n, float contrast) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f), half = v8fSet(.5f), steeper = v8fSet(1.f + contrast);
    for (int i = 0; i < n; i += 8)
        for (int k = 0; k < 3; ++k) {
            V8F s = srgbEncodeLanes(gradeClamp(v8fLoad(c[k] + i), zero, one));
            if (contrast != 0) s = gradeClamp(v8fAdd(half, v8fMul(v8fSub(s, half), steeper)), zero, one);
            v8fStore(c[k] + i, s);
        }
}

static void gradeLanes(const ArtfFrameData* f, const float* lut, float* const* c, int n, uint32_t at) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f);
    /* (in what is shown only with gradeShown: else it was done in ACEScct, before the curve: artfGradeCct) */
    const bool shownSpace = f->gradeShown > 0;
    const bool curves = shownSpace && f->grade2[0][3] > 0;
    const int size = (int)f->grade2[3][3];
    const bool looked = shownSpace && size >= 2 && lut;
    const float grain = f->grade2[1][3];
    for (int x = 0; x < n; x += 8) {
        V8F v[3] = {v8fLoad(c[0] + x), v8fLoad(c[1] + x), v8fLoad(c[2] + x)};
        if (curves) {
            for (int k = 0; k < 3; ++k) {
                const V8F o = v8fMul(v8fSet(f->grade2[2][k]), v8fAdd(v[k], v8fMul(v8fSet(f->grade2[0][k]), v8fSub(one, v[k]))));
                if (f->grade2[1][k] == 1) { v[k] = o; continue; }
                v[k] = v8fSelect(v8fGt(o, zero), zero, gradeExp2(v8fMul(gradeLog2(o), v8fSet(f->grade2[1][k]))));
            }
            const V8F l = gradeClamp(v8fAdd(v8fAdd(v8fMul(v8fSet(.2126f), v[0]), v8fMul(v8fSet(.7152f), v[1])), v8fMul(v8fSet(.0722f), v[2])), zero, one);
            const V8F rest = v8fSub(one, l);
            const V8F dark = v8fMul(rest, rest), middle = v8fMul(v8fMul(v8fSet(2.f), l), rest), bright = v8fMul(l, l);
            for (int k = 0; k < 3; ++k) {
                const V8F wheel = v8fAdd(v8fAdd(v8fMul(dark, v8fSet(f->grade2[3][k])), v8fMul(middle, v8fSet(f->grade2[4][k]))), v8fMul(bright, v8fSet(f->grade2[5][k])));
                v[k] = gradeClamp(v8fMul(v[k], wheel), zero, one);
            }
        }
        if (looked) {
            V8I index = v8iSet(0);
            V8F t[3];
            const V8F last = v8fSet((float)(size - 1));
            for (int k = 2; k >= 0; --k) {
                const V8F q = v8fMul(gradeClamp(v[k], zero, one), last);
                V8I a = v8fToI(q);
                a = v8iSelect(v8iGt(a, v8iSet(size - 2)), a, v8iSet(size - 2));
                t[k] = v8fSub(q, v8iToF(a));
                index = v8iAdd(v8iMul(index, v8iSet(k == 2 ? 0 : size)), a);
            }
            index = v8iMul(index, v8iSet(3));
            const int dy = 3 * size, dz = 3 * size * size;
            const uint32_t* table = (const uint32_t*)lut;
            const V8F amount = v8fSet(f->grade2[4][3]);
            for (int k = 0; k < 3; ++k) {
                const V8I q = v8iAdd(index, v8iSet(k));
#define LUT_AT(o) v8iAsF(v8iGather(table, v8iAdd(q, v8iSet(o))))
                const V8F q0 = LUT_AT(0), q3 = LUT_AT(3), qy = LUT_AT(dy), qy3 = LUT_AT(dy + 3);
                const V8F qz = LUT_AT(dz), qz3 = LUT_AT(dz + 3), qzy = LUT_AT(dz + dy), qzy3 = LUT_AT(dz + dy + 3);
#undef LUT_AT
                const V8F a = v8fAdd(q0, v8fMul(v8fSub(q3, q0), t[0])), b = v8fAdd(qy, v8fMul(v8fSub(qy3, qy), t[0]));
                const V8F d = v8fAdd(qz, v8fMul(v8fSub(qz3, qz), t[0])), e = v8fAdd(qzy, v8fMul(v8fSub(qzy3, qzy), t[0]));
                const V8F nearer = v8fAdd(a, v8fMul(v8fSub(b, a), t[1])), farther = v8fAdd(d, v8fMul(v8fSub(e, d), t[1]));
                const V8F found = v8fAdd(nearer, v8fMul(v8fSub(farther, nearer), t[2]));
                v[k] = gradeClamp(v8fAdd(v[k], v8fMul(v8fSub(found, v[k]), amount)), zero, one);
            }
        }
        if (grain > 0) {
            const V8I h = gradeHash(v8iAdd(v8iSet((int32_t)(at + (uint32_t)x)), v8iIota()));
            const V8F noise = v8fSub(v8fMul(v8iToF(v8iShr(h, 8)), v8fSet(1.f / 16777216.f)), v8fSet(.5f));
            const V8F scaled = v8fMul(noise, v8fSet(grain));
            for (int k = 0; k < 3; ++k) {
                const V8F mid = v8fAdd(v8fSet(.0625f), v8fMul(v8fMul(v8fSet(.75f), v[k]), v8fSub(one, v[k])));
                v[k] = gradeClamp(v8fAdd(v[k], v8fMul(scaled, mid)), zero, one);
            }
        }
        for (int k = 0; k < 3; ++k) v8fStore(c[k] + x, v[k]);
    }
}

/* ---- temporal antialiasing, after the frame ----------------------------------------------------- */

V8_INLINE V8F loadU(const float* p) { return v8iAsF(v8iLoadU(p)); }
V8_INLINE V8F gatherF(const float* plane, V8I index) { return v8iAsF(v8iGather((const uint32_t*)plane, index)); }

/* post.c's temporalRow for the pixels from from to to of row y, eight at a time: the same arithmetic in the same order as
 * wasAt (perspective, at the scene's size), tap and the blend, each ternary a select. A group of eight is done here
 * only when it is away from the frame's sides and none of its pixels went by its vertices (ARTF_MOTION_VECTOR); done[g]
 * says so of group g (from from + 8 g), and post.c does the others a pixel at a time. */
/* post.c's tapSharp, eight at a time: tx and ty already within the plane. */
static V8F sharpLanes(const float* plane, int w, int h, V8F tx, V8F ty) {
    const V8I x0 = v8fToI(tx), y0 = v8fToI(ty);
    V8F wx[4], wy[4];
    const V8F half = v8fSet(.5f), two = v8fSet(2.f), three = v8fSet(3.f), four = v8fSet(4.f), five = v8fSet(5.f);
    for (int axis = 0; axis < 2; ++axis) {
        const V8F t = axis ? v8fSub(ty, v8iToF(y0)) : v8fSub(tx, v8iToF(x0)), t2 = v8fMul(t, t), t3 = v8fMul(t2, t);
        V8F* out = axis ? wy : wx;
        out[0] = v8fMul(half, v8fSub(v8fSub(v8fMul(two, t2), t3), t));
        out[1] = v8fMul(half, v8fAdd(v8fSub(v8fMul(three, t3), v8fMul(five, t2)), two));
        out[2] = v8fMul(half, v8fAdd(v8fSub(v8fMul(four, t2), v8fMul(three, t3)), t));
        out[3] = v8fMul(half, v8fSub(t3, t2));
    }
    V8I cols[4];
    for (int i = 0; i < 4; ++i) {
        V8I c = v8iAdd(x0, v8iSet(i - 1));
        c = v8iSelect(v8iGt(v8iSet(0), c), c, v8iSet(0));
        cols[i] = v8iSelect(v8iGt(c, v8iSet(w - 1)), c, v8iSet(w - 1));
    }
    V8F total = v8fSet(0.f);
    for (int j = 0; j < 4; ++j) {
        V8I r = v8iAdd(y0, v8iSet(j - 1));
        r = v8iSelect(v8iGt(v8iSet(0), r), r, v8iSet(0));
        r = v8iSelect(v8iGt(r, v8iSet(h - 1)), r, v8iSet(h - 1));
        const V8I at = v8iMul(r, v8iSet(w));
        const V8F across = v8fAdd(v8fAdd(v8fAdd(v8fMul(wx[0], gatherF(plane, v8iAdd(at, cols[0]))), v8fMul(wx[1], gatherF(plane, v8iAdd(at, cols[1])))),
                                          v8fMul(wx[2], gatherF(plane, v8iAdd(at, cols[2])))),
                                  v8fMul(wx[3], gatherF(plane, v8iAdd(at, cols[3]))));
        total = j ? v8fAdd(total, v8fMul(wy[j], across)) : v8fMul(wy[j], across);
    }
    return v8fMax(total, v8fSet(0.f));
}

static void temporalLanes(ArtfSoftPost* p, int y, int from, int to, uint8_t* done) {
    const ArtfSoftFrame* sf = p->f;
    const ArtfFrameData* f = &sf->data;
    const int w = p->width, h = p->height;
    const float jx = f->post[2][2], jy = f->post[2][3];
    const float py = 1.f - (((float)y + jy) + .5f) / (float)h * 2.f;
    const size_t rows[3] = {(size_t)artfClampi(y - 1, 0, h - 1) * w, (size_t)y * w, (size_t)artfClampi(y + 1, 0, h - 1) * w};
    const V8F zero = v8fSet(0.f), wF = v8fSet((float)w), hF = v8fSet((float)h), lastX = v8fSet((float)(w - 1)), lastY = v8fSet((float)(h - 1));
    const V8I zeroI = v8iSet(0);
    const float* m = f->previous;
    for (int x = from, g = 0; x < to; x += 8, ++g) {
        if (x < 1 || x + 8 > w - 1 || x + 8 > to) continue;
        const size_t i = (size_t)y * w + x;
        const V8F depth = loadU(sf->postDepth + i);
        const V8I covered = v8fGt(depth, zero);
        const V8F d = v8fSelect(covered, zero, v8fDiv(v8fSet(1.f), depth));
        const V8I seen = v8fGt(d, zero);
        V8I who = zeroI;
        if (sf->motion) who = v8iSelect(seen, zeroI, v8iLoadU(sf->postInstance + i));
        if (v8iSigns(v8iEq(v8iAnd(who, v8iSet((int32_t)ARTF_MOTION_VECTOR)), zeroI)) != 0xFF) continue;
        /* worldAt */
        const V8F xs = v8fSub(v8iToF(v8iAdd(v8iSet(x), v8iIota())), v8fSet(jx));
        const V8F px = v8fSub(v8fMul(v8fDiv(v8fAdd(xs, v8fSet(.5f)), wF), v8fSet(2.f)), v8fSet(1.f));
        const V8F along = v8fSelect(seen, v8fSet(1e4f), d);
        V8F at[3];
        for (int k = 0; k < 3; ++k) {
            const V8F dir = v8fAdd(v8fAdd(v8fSet(f->eye[2][k]), v8fMul(px, v8fSet(f->eye[0][k]))), v8fMul(v8fSet(py), v8fSet(f->eye[1][k])));
            at[k] = v8fAdd(v8fSet(f->cameraPos[k]), v8fMul(dir, along));
        }
        if (sf->motion) {
            /* (how its instance moved: lanes of no instance read the first's, and keep their own) */
            const V8I none = v8iEq(who, zeroI);
            const V8I base = v8iSelect(none, v8iMul(v8iSub(who, v8iSet(1)), v8iSet(12)), zeroI);
            V8F was[3];
            for (int r = 0; r < 3; ++r) {
                V8F sum = v8fMul(gatherF(sf->motion, v8iAdd(base, v8iSet(r * 4))), at[0]);
                sum = v8fAdd(sum, v8fMul(gatherF(sf->motion, v8iAdd(base, v8iSet(r * 4 + 1))), at[1]));
                sum = v8fAdd(sum, v8fMul(gatherF(sf->motion, v8iAdd(base, v8iSet(r * 4 + 2))), at[2]));
                was[r] = v8fAdd(sum, gatherF(sf->motion, v8iAdd(base, v8iSet(r * 4 + 3))));
            }
            for (int r = 0; r < 3; ++r) at[r] = v8fSelect(none, was[r], at[r]);
        }
        /* project */
#define ROW_OF(a, b, c, e) v8fAdd(v8fAdd(v8fAdd(v8fMul(v8fSet(m[a]), at[0]), v8fMul(v8fSet(m[b]), at[1])), v8fMul(v8fSet(m[c]), at[2])), v8fSet(m[e]))
        const V8F cx = ROW_OF(0, 4, 8, 12), cy = ROW_OF(1, 5, 9, 13), cw = ROW_OF(3, 7, 11, 15);
#undef ROW_OF
        const V8F half = v8fSet(.5f);
        const V8F sx = v8fSub(v8fMul(v8fAdd(v8fMul(v8fDiv(cx, cw), half), half), wF), half);
        const V8F sy = v8fSub(v8fMul(v8fSub(half, v8fMul(v8fDiv(cy, cw), half)), hF), half);
        V8I fresh = v8iSet(-1);
        if (p->historyValid) {
            fresh = v8iXor(v8fGt(cw, v8fSet(1e-5f)), v8iSet(-1)); /* (!(cw > 1e-5): as cw <= 1e-5 for what is a number) */
            fresh = v8iOr(fresh, v8iOr(v8fLt(sx, zero), v8fLt(sy, zero)));
            fresh = v8iOr(fresh, v8iOr(v8fGt(sx, lastX), v8fGt(sy, lastY)));
        }
        const V8F share = v8fSelect(fresh, v8fSet(.1f), v8fSet(1.f));
        /* tap, where the history is read at all */
        const V8F tx = gradeClamp(v8fSelect(fresh, sx, zero), zero, lastX), ty = gradeClamp(v8fSelect(fresh, sy, zero), zero, lastY);
        const V8I x0 = v8fToI(tx), y0 = v8fToI(ty);
        const V8I x1 = v8iSelect(v8iGt(v8iSet(w - 1), v8iAdd(x0, v8iSet(1))), v8iSet(w - 1), v8iAdd(x0, v8iSet(1)));
        const V8I y1 = v8iSelect(v8iGt(v8iSet(h - 1), v8iAdd(y0, v8iSet(1))), v8iSet(h - 1), v8iAdd(y0, v8iSet(1)));
        const V8F fx = v8fSub(tx, v8iToF(x0)), fy = v8fSub(ty, v8iToF(y0));
        const V8I r0 = v8iMul(y0, v8iSet(w)), r1 = v8iMul(y1, v8iSet(w));
        const V8I i00 = v8iAdd(r0, x0), i01 = v8iAdd(r0, x1), i10 = v8iAdd(r1, x0), i11 = v8iAdd(r1, x1);
        for (int c = 0; c < 3; ++c) {
            V8F lo = v8fSet(3e38f), hi = v8fSet(-3e38f);
            for (int r = 0; r < 3; ++r)
                for (int k = -1; k <= 1; ++k) {
                    const V8F there = loadU(sf->postLight[c] + rows[r] + x + k);
                    lo = v8fSelect(v8fLt(there, lo), lo, there);
                    hi = v8fSelect(v8fLt(hi, there), hi, there);
                }
            const float* plane = p->history[c];
            const V8F a = gatherF(plane, i00), b = gatherF(plane, i01), cc = gatherF(plane, i10), e = gatherF(plane, i11);
            const V8F top = v8fAdd(a, v8fMul(v8fSub(b, a), fx)), bottom = v8fAdd(cc, v8fMul(v8fSub(e, cc), fx));
            const V8F tapped = p->f->data.post[1][3] > 1.5f ? sharpLanes(plane, w, h, tx, ty) : v8fAdd(top, v8fMul(v8fSub(bottom, top), fy));
            const V8F now = loadU(sf->postLight[c] + i);
            const V8F was = v8fSelect(fresh, gradeClamp(tapped, lo, hi), now);
            v8iStoreU(p->scratch[c] + i, v8fAsI(v8fAdd(was, v8fMul(v8fSub(now, was), share))));
        }
        done[g] = 1;
    }
}

/* ---- upscaling, after the frame ------------------------------------------------------------------ */

V8_INLINE V8I clampLanes(V8I v, int lo, int hi) {
    v = v8iSelect(v8iGt(v8iSet(lo), v), v, v8iSet(lo));
    return v8iSelect(v8iGt(v, v8iSet(hi)), v, v8iSet(hi));
}
V8_INLINE V8I floorLanes(V8F v) {
    const V8I i = v8fToI(v);
    return v8iAdd(i, v8fLt(v, v8iToF(i)));
}

/* Luma, and a ridge: brighter or darker than both sides, one way or the other, by a quarter of the brightest (boxLanes,
 * upscaleLanes). */
V8_INLINE V8F lumaLanes(V8F r, V8F g, V8F b) { return v8fAdd(v8fAdd(v8fMul(v8fSet(.2126f), r), v8fMul(v8fSet(.7152f), g)), v8fMul(v8fSet(.0722f), b)); }
static float lumaAt(float* const* light, size_t at) { return .2126f * light[0][at] + .7152f * light[1][at] + .0722f * light[2][at]; }
V8_INLINE V8I ridgeLanes(V8F l, V8F left, V8F right, V8F up, V8F down) {
    const V8F zero = v8fSet(0.f), quarter = v8fSet(.25f);
    const V8F a1 = v8fSub(l, left), a2 = v8fSub(l, right), b1 = v8fSub(l, up), b2 = v8fSub(l, down);
    const V8F across = v8fMul(v8fMax(v8fMax(l, left), right), quarter), along = v8fMul(v8fMax(v8fMax(l, up), down), quarter);
    const V8I h = v8iAnd(v8fGt(v8fMul(a1, a2), zero), v8iAnd(v8fGt(v8fAbs(a1), across), v8fGt(v8fAbs(a2), across)));
    const V8I v = v8iAnd(v8fGt(v8fMul(b1, b2), zero), v8iAnd(v8fGt(v8fAbs(b1), along), v8fGt(v8fAbs(b2), along)));
    return v8iOr(h, v);
}
static bool ridgeAt(float l, float left, float right, float up, float down) {
    const float a1 = l - left, a2 = l - right, b1 = l - up, b2 = l - down;
    const float across = artfMaxf(artfMaxf(l, left), right) * .25f, along = artfMaxf(artfMaxf(l, up), down) * .25f;
    return (a1 * a2 > 0.f && fabsf(a1) > across && fabsf(a2) > across) || (b1 * b2 > 0.f && fabsf(b1) > along && fabsf(b2) > along);
}
/* post.c's upscaleRow with temporal antialiasing, for the pixels from from to to of row y, eight at a time: wasAt at
 * the frame's size (by the view alone where no instance shows, by the instance's motion where one does), the four
 * samples, the least and most around the nearest, the history: the same arithmetic in the same order. done[g] as for
 * temporalLanes; a group where a pixel went by its vertices is left to post.c. */
static void upscaleLanes(ArtfSoftPost* p, int y, int from, int to, uint8_t* done) {
    const ArtfSoftFrame* sf = p->f;
    const ArtfFrameData* f = &sf->data;
    const int w = p->width, h = p->height, sw = p->sceneW, sh = p->sceneH;
    const float rx = p->ratioX, ry = p->ratioY, jx = f->post[2][2], jy = f->post[2][3];
    const float gain = .1f / (rx * ry);
    const float v = ((float)y + .5f) * ry - .5f - jy;
    const int iv = (int)v, y0 = iv - (v < (float)iv);
    const float fy = v - (float)y0;
    const int row0 = artfClampi(y0, 0, sh - 1) * sw, row1 = artfClampi(y0 + 1, 0, sh - 1) * sw;
    const float oy0 = fy / ry, oy1 = (1.f - fy) / ry, oy00 = oy0 * oy0, oy11 = oy1 * oy1;
    /* (planeAt's row, and worldAt's) */
    const float sv = ((float)y + .5f) * p->ratioY - f->post[2][3];
    const int isv = (int)sv, planeRow = artfClampi(isv - (sv < (float)isv), 0, sh - 1) * sw;
    const float py = 1.f - (((float)y + 0.f) + .5f) / (float)h * 2.f;
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f), half = v8fSet(.5f), wF = v8fSet((float)w), hF = v8fSet((float)h);
    const V8F lastX = v8fSet((float)(w - 1)), lastY = v8fSet((float)(h - 1));
    const V8I zeroI = v8iSet(0);
    const float* m = f->previous;
    const float* back = p->viewBack;
    const V8F wy0 = v8fSet(1.f - fy), wy1 = v8fSet(fy);
    for (int x = from, g = 0; x < to; x += 8, ++g) {
        if (x + 8 > to) continue;
        const V8F xs = v8iToF(v8iAdd(v8iSet(x), v8iIota()));
        /* the four samples around the center */
        const V8F u = v8fAdd(v8fSub(v8fMul(v8fAdd(xs, half), v8fSet(rx)), half), v8fSet(jx));
        const V8I x0 = floorLanes(u);
        const V8F fx = v8fSub(u, v8iToF(x0));
        const V8I c0 = clampLanes(x0, 0, sw - 1), c1 = clampLanes(v8iAdd(x0, v8iSet(1)), 0, sw - 1);
        const V8I q[4] = {v8iAdd(v8iSet(row0), c0), v8iAdd(v8iSet(row0), c1), v8iAdd(v8iSet(row1), c0), v8iAdd(v8iSet(row1), c1)};
        const V8F rest = v8fSub(one, fx);
        const V8F weights[4] = {v8fMul(rest, wy0), v8fMul(fx, wy0), v8fMul(rest, wy1), v8fMul(fx, wy1)};
        /* wasAt: the scene's sample nearest the center */
        const V8I psx = clampLanes(floorLanes(v8fAdd(v8fMul(v8fAdd(xs, half), v8fSet(p->ratioX)), v8fSet(f->post[2][2]))), 0, sw - 1);
        const V8I at = v8iAdd(v8iSet(planeRow), psx);
        const V8F depth = gatherF(sf->postDepth, at);
        const V8F d = v8fSelect(v8fGt(depth, zero), zero, v8fDiv(one, depth));
        const V8I seen = v8fGt(d, zero);
        V8I who = zeroI;
        if (sf->motion) who = v8iSelect(seen, zeroI, v8iGather(sf->postInstance, at));
        if (v8iSigns(v8iEq(v8iAnd(who, v8iSet((int32_t)ARTF_MOTION_VECTOR)), zeroI)) != 0xFF) continue;
        const V8I none = v8iEq(who, zeroI);
        /* by the view alone */
        const V8F z = v8fSelect(v8fGt(depth, zero), v8fSet(1e-4f), depth), yF = v8fSet((float)y);
#define BACK_OF(a, b, c, e) v8fAdd(v8fAdd(v8fAdd(v8fMul(v8fSet(back[a]), z), v8fSet(back[b])), v8fMul(v8fSet(back[c]), xs)), v8fMul(v8fSet(back[e]), yF))
        const V8F bx = BACK_OF(0, 4, 8, 12), by = BACK_OF(1, 5, 9, 13), bw = BACK_OF(3, 7, 11, 15);
#undef BACK_OF
        const V8I okBack = v8fGt(bw, v8fMul(v8fSet(1e-5f), z));
        const V8F sxBack = v8fSub(v8fMul(v8fAdd(v8fMul(v8fDiv(bx, bw), half), half), wF), half);
        const V8F syBack = v8fSub(v8fMul(v8fSub(half, v8fMul(v8fDiv(by, bw), half)), hF), half);
        /* by the instance's motion: worldAt, then project */
        V8F sx = sxBack, sy = syBack;
        V8I ok = okBack;
        if (v8iSigns(none) != 0xFF) {
            const V8F px = v8fSub(v8fMul(v8fDiv(v8fAdd(v8fSub(xs, zero), half), wF), v8fSet(2.f)), one);
            const V8F along = v8fSelect(seen, v8fSet(1e4f), d);
            V8F place[3];
            for (int k = 0; k < 3; ++k) {
                const V8F dir = v8fAdd(v8fAdd(v8fSet(f->eye[2][k]), v8fMul(px, v8fSet(f->eye[0][k]))), v8fMul(v8fSet(py), v8fSet(f->eye[1][k])));
                place[k] = v8fAdd(v8fSet(f->cameraPos[k]), v8fMul(dir, along));
            }
            const V8I base = v8iSelect(none, v8iMul(v8iSub(who, v8iSet(1)), v8iSet(12)), zeroI);
            V8F was[3];
            for (int r = 0; r < 3; ++r) {
                V8F sum = v8fMul(gatherF(sf->motion, v8iAdd(base, v8iSet(r * 4))), place[0]);
                sum = v8fAdd(sum, v8fMul(gatherF(sf->motion, v8iAdd(base, v8iSet(r * 4 + 1))), place[1]));
                sum = v8fAdd(sum, v8fMul(gatherF(sf->motion, v8iAdd(base, v8iSet(r * 4 + 2))), place[2]));
                was[r] = v8fAdd(sum, gatherF(sf->motion, v8iAdd(base, v8iSet(r * 4 + 3))));
            }
#define ROW_OF(a, b, c, e) v8fAdd(v8fAdd(v8fAdd(v8fMul(v8fSet(m[a]), was[0]), v8fMul(v8fSet(m[b]), was[1])), v8fMul(v8fSet(m[c]), was[2])), v8fSet(m[e]))
            const V8F cx = ROW_OF(0, 4, 8, 12), cy = ROW_OF(1, 5, 9, 13), cw = ROW_OF(3, 7, 11, 15);
#undef ROW_OF
            sx = v8fSelect(none, v8fSub(v8fMul(v8fAdd(v8fMul(v8fDiv(cx, cw), half), half), wF), half), sxBack);
            sy = v8fSelect(none, v8fSub(v8fMul(v8fSub(half, v8fMul(v8fDiv(cy, cw), half)), hF), half), syBack);
            ok = v8iSelect(none, v8fGt(cw, v8fSet(1e-5f)), okBack);
        }
        V8I fresh = v8iXor(ok, v8iSet(-1));
        fresh = v8iOr(fresh, v8iOr(v8fLt(sx, zero), v8fLt(sy, zero)));
        fresh = v8iOr(fresh, v8iOr(v8fGt(sx, lastX), v8fGt(sy, lastY)));
        /* how near each sample is */
        const V8F ox0 = v8fDiv(fx, v8fSet(rx)), ox1 = v8fDiv(rest, v8fSet(rx));
        const V8F ox00 = v8fMul(ox0, ox0), ox11 = v8fMul(ox1, ox1);
        const V8F d2[4] = {v8fAdd(ox00, v8fSet(oy00)), v8fAdd(ox11, v8fSet(oy00)), v8fAdd(ox00, v8fSet(oy11)), v8fAdd(ox11, v8fSet(oy11))};
        V8F near[4], sum = zero;
        for (int k = 0; k < 4; ++k) {
            const V8F left = v8fSub(one, d2[k]);
            near[k] = v8fSelect(v8fLt(d2[k], one), zero, v8fMul(left, left));
            sum = v8fAdd(sum, near[k]);
        }
        const V8F scaled = v8fMul(sum, v8fSet(gain));
        const V8F share = v8fSelect(v8fLt(one, scaled), scaled, one);
        const V8F each = v8fSelect(v8fGt(sum, zero), zero, v8fDiv(one, sum));
        /* (the least and most around the nearest sample: boxRow's) */
        const V8I nearest = v8iAdd(v8iSet(fy < .5f ? row0 : row1), clampLanes(v8iSelect(v8fLt(fx, half), v8iAdd(x0, v8iSet(1)), x0), 0, sw - 1));
        /* the history, where it is read */
        const V8F tx = gradeClamp(v8fSelect(fresh, sx, zero), zero, lastX), ty = gradeClamp(v8fSelect(fresh, sy, zero), zero, lastY);
        const V8I hx0 = v8fToI(tx), hy0 = v8fToI(ty);
        const V8I hx1 = v8iSelect(v8iGt(v8iSet(w - 1), v8iAdd(hx0, v8iSet(1))), v8iSet(w - 1), v8iAdd(hx0, v8iSet(1)));
        const V8I hy1 = v8iSelect(v8iGt(v8iSet(h - 1), v8iAdd(hy0, v8iSet(1))), v8iSet(h - 1), v8iAdd(hy0, v8iSet(1)));
        const V8F hfx = v8fSub(tx, v8iToF(hx0)), hfy = v8fSub(ty, v8iToF(hy0));
        const V8I r0 = v8iMul(hy0, v8iSet(w)), r1 = v8iMul(hy1, v8iSet(w));
        const V8I i00 = v8iAdd(r0, hx0), i01 = v8iAdd(r0, hx1), i10 = v8iAdd(r1, hx0), i11 = v8iAdd(r1, hx1);
        const size_t i = (size_t)y * w + x;
        /* the lock (post.c's: a ridge here, or what it was where the pixel was, worn) */
        const V8I lx = v8fToI(v8fAdd(tx, half)), ly = v8fToI(v8fAdd(ty, half));
        const V8F lockWas = gatherF(p->lock[p->lockAt ^ 1], v8iAdd(v8iMul(ly, v8iSet(w)), lx));
        const V8F worn = v8fMax(v8fSub(lockWas, v8fSet(1.f / 24.f)), zero);
        const V8F lock = v8fSelect(v8fGt(gatherF(p->ridge, nearest), zero), worn, one);
        v8iStoreU(p->lock[p->lockAt] + i, v8fAsI(v8fSelect(fresh, lock, zero)));
        V8F was[3], now[3], between[4];
        for (int c = 0; c < 4; ++c) {
            const float* light = sf->postLight[c];
            const V8F l0 = gatherF(light, q[0]), l1 = gatherF(light, q[1]), l2 = gatherF(light, q[2]), l3 = gatherF(light, q[3]);
            between[c] = v8fAdd(v8fAdd(v8fAdd(v8fMul(l0, weights[0]), v8fMul(l1, weights[1])), v8fMul(l2, weights[2])), v8fMul(l3, weights[3]));
            if (c == 3) break;
            const V8F lo = gatherF(p->box[c], nearest), hi = gatherF(p->box[3 + c], nearest);
            const float* plane = p->history[c];
            const V8F a = gatherF(plane, i00), b = gatherF(plane, i01), cc = gatherF(plane, i10), e = gatherF(plane, i11);
            const V8F top = v8fAdd(a, v8fMul(v8fSub(b, a), hfx)), bottom = v8fAdd(cc, v8fMul(v8fSub(e, cc), hfx));
            const V8F had = p->f->data.post[1][3] > 1.5f ? sharpLanes(plane, w, h, tx, ty) : v8fAdd(top, v8fMul(v8fSub(bottom, top), hfy)), kept = gradeClamp(had, lo, hi);
            was[c] = v8fAdd(kept, v8fMul(v8fSub(had, kept), lock));
            now[c] = v8fMul(v8fAdd(v8fAdd(v8fAdd(v8fMul(l0, near[0]), v8fMul(l1, near[1])), v8fMul(l2, near[2])), v8fMul(l3, near[3])), each);
        }
        v8iStoreU(p->scratch[3] + i, v8fAsI(between[3]));
        /* (each by 1 / (1 + its luma)) */
        const V8F byNow = v8fDiv(share, v8fAdd(one, lumaLanes(now[0], now[1], now[2]))), byWas = v8fDiv(v8fSub(one, share), v8fAdd(one, lumaLanes(was[0], was[1], was[2])));
        const V8F taken = v8fDiv(byNow, v8fAdd(byNow, byWas));
        for (int c = 0; c < 3; ++c) {
            const V8F gathered = v8fAdd(was[c], v8fMul(v8fSub(now[c], was[c]), taken));
            v8iStoreU(p->scratch[c] + i, v8fAsI(v8fSelect(fresh, gathered, between[c])));
        }
        done[g] = 1;
    }
}

/* post.c's boxRow: of each of the scene's pixels in row y, what the history may be around it, three by three (the least
 * and most of each color, held to their mean and three quarters of their deviation), and whether it is a ridge; in the
 * same order; eight at a time away from the sides. */
static void boxLanes(ArtfSoftPost* p, int y) {
    const int sw = p->sceneW, sh = p->sceneH;
    float* const* light = p->f->postLight;
    const size_t rows[3] = {(size_t)artfMaxi(y - 1, 0) * sw, (size_t)y * sw, (size_t)artfMini(y + 1, sh - 1) * sw};
    const size_t at = (size_t)y * sw;
    const float ninth = 1.f / 9.f;
    for (int x = 0; x < sw;) {
        if (x >= 1 && x + 8 <= sw - 1) {
            const V8F ninthL = v8fSet(ninth), most = v8fSet(.75f), zero = v8fSet(0.f);
            for (int c = 0; c < 3; ++c) {
                V8F lo = v8fSet(3e38f), hi = v8fSet(-3e38f), sum = zero, squares = zero;
                for (int r = 0; r < 3; ++r)
                    for (int k = -1; k <= 1; ++k) {
                        const V8F there = loadU(light[c] + rows[r] + x + k);
                        lo = v8fSelect(v8fLt(there, lo), lo, there);
                        hi = v8fSelect(v8fLt(hi, there), hi, there);
                        sum = v8fAdd(sum, there);
                        squares = v8fAdd(squares, v8fMul(there, there));
                    }
                const V8F mean = v8fMul(sum, ninthL), deviation = v8fSqrt(v8fMax(v8fSub(v8fMul(squares, ninthL), v8fMul(mean, mean)), zero));
                const V8F low = v8fSub(mean, v8fMul(most, deviation)), high = v8fAdd(mean, v8fMul(most, deviation));
                lo = v8fSelect(v8fLt(lo, low), lo, low);
                hi = v8fSelect(v8fLt(high, hi), hi, high);
                v8iStoreU(p->box[c] + at + x, v8fAsI(lo));
                v8iStoreU(p->box[3 + c] + at + x, v8fAsI(hi));
            }
#define LUMA_AT(row, dx) lumaLanes(loadU(light[0] + (row) + x + (dx)), loadU(light[1] + (row) + x + (dx)), loadU(light[2] + (row) + x + (dx)))
            const V8I ridge = ridgeLanes(LUMA_AT(rows[1], 0), LUMA_AT(rows[1], -1), LUMA_AT(rows[1], 1), LUMA_AT(rows[0], 0), LUMA_AT(rows[2], 0));
#undef LUMA_AT
            v8iStoreU(p->ridge + at + x, v8fAsI(v8fSelect(ridge, zero, v8fSet(1.f))));
            x += 8;
            continue;
        }
        const int xs[3] = {artfMaxi(x - 1, 0), x, artfMini(x + 1, sw - 1)};
        for (int c = 0; c < 3; ++c) {
            float lo = 3e38f, hi = -3e38f, sum = 0.f, squares = 0.f;
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) {
                    const float there = light[c][rows[r] + xs[k]];
                    lo = artfMinf(lo, there);
                    hi = artfMaxf(hi, there);
                    sum = sum + there;
                    squares = squares + there * there;
                }
            const float mean = sum * ninth, deviation = sqrtf(artfMaxf(squares * ninth - mean * mean, 0.f));
            p->box[c][at + x] = artfMaxf(lo, mean - .75f * deviation);
            p->box[3 + c][at + x] = artfMinf(hi, mean + .75f * deviation);
        }
        p->ridge[at + x] = ridgeAt(lumaAt(light, rows[1] + xs[1]), lumaAt(light, rows[1] + xs[0]), lumaAt(light, rows[1] + xs[2]), lumaAt(light, rows[0] + xs[1]), lumaAt(light, rows[2] + xs[1])) ? 1.f : 0.f;
        ++x;
    }
}

const ArtfKernels KERNELS = {KERNELS_NAME, KERNELS_SIMD, geometry, tile, depthTile, gradeLanes, shownLanes, temporalLanes, upscaleLanes, boxLanes};
