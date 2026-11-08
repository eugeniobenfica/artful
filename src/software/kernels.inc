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

/* z: 1 / w at each vertex. */
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

typedef struct ClipVertex { float x, y, w; } ClipVertex;

/* Keeps what is on the positive side of a * x + b * y + c * w + d >= 0. */
static int clipPlane(const ClipVertex* in, int n, ClipVertex* out, float a, float b, float c, float d) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const ClipVertex *p = &in[i], *q = &in[i + 1 == n ? 0 : i + 1];
        float dp = a * p->x + b * p->y + c * p->w + d, dq = a * q->x + b * q->y + c * q->w + d;
        if (dp >= 0) out[m++] = *p;
        if ((dp >= 0) != (dq >= 0)) {
            /* Two triangles share this edge, and walk it in opposite ways: worked out in one order
             * for both, it ends at the same point in both, and no crack opens between them. */
            const bool swap = p->x > q->x || (p->x == q->x && (p->y > q->y || (p->y == q->y && p->w > q->w)));
            const ClipVertex *from = swap ? q : p, *to = swap ? p : q;
            const float df = swap ? dq : dp, dt = swap ? dp : dq;
            const float t = df / (df - dt);
            ClipVertex v = {from->x + (to->x - from->x) * t, from->y + (to->y - from->y) * t, from->w + (to->w - from->w) * t};
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
    int n = clipPlane(a, 3, b, 0, 0, 1, -f->nearW);
    n = clipPlane(b, n, a, -1, 0, f->guardX, 0);
    n = clipPlane(a, n, b, 1, 0, f->guardX, 0);
    n = clipPlane(b, n, a, 0, -1, f->guardY, 0);
    n = clipPlane(a, n, b, 0, 1, f->guardY, 0);
    if (n < 3) return;
    int32_t x[12], y[12];
    float z[12];
    const float hw = (float)f->width * .5f, hh = (float)f->height * .5f;
    for (int i = 0; i < n; ++i) {
        float iw = 1.f / b[i].w;
        x[i] = (int32_t)floorf((b[i].x * iw * hw + hw) * 16.f + .5f);
        y[i] = (int32_t)floorf((hh - b[i].y * iw * hh) * 16.f + .5f);
        z[i] = iw;
    }
    for (int i = 1; i + 1 < n; ++i) emitTriangle(g, x[0], y[0], z[0], x[i], y[i], z[i], x[i + 1], y[i + 1], z[i + 1]);
}

static inline bool meshletCulled(const ArtfSoftFrame* f, const ArtfVisible* v, const ArtfMeshlet* l) {
    if (v->useCone && l->coneCutoff < 2.f) {
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
            const ClipVertex in[3] = {{clipX[a], clipY[a], clipW[a]}, {clipX[b], clipY[b], clipW[b]}, {clipX[c], clipY[c], clipW[c]}};
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
static inline void rasterize(float* depthPlane, uint32_t* idPlane, const ArtfRasterTri* t, uint32_t id, int ox, int oy, int sx_, int sy_, const ShadeContext* mask) {
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

/* What a surface reflects, for physically based light: Cook and Torrance's, with Trowbridge and Reitz's
 * distribution (GGX), Schlick's Fresnel and Smith's shadowing. Light here has pi taken out, as diffuse
 * light has: so the 1 / pi of the distribution is not there either. */
typedef struct Reflecting {
    V8F a2, k, ndv, strength;
    Lanes3 f0;
} Reflecting;

V8_INLINE V8F fifth(V8F x) {
    const V8F y = v8fSub(v8fSet(1.f), x), y2 = v8fMul(y, y);
    return v8fMul(v8fMul(y2, y2), y);
}

/* How much light a surface gives back toward the eye, for one light: adds to diffuse and specular. */
V8_INLINE void addLight(Lanes3 n, Lanes3 view, Lanes3 toLight, V8F strength, const float* color, bool shiny, const Reflecting* r, Lanes3* diffuse, Lanes3* highlight) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f);
    V8F ndl = v8fMax(dot3(n, toLight), zero);
    V8F amount = v8fMul(ndl, strength);
    V8F cr = v8fSet(color[0]), cg = v8fSet(color[1]), cb = v8fSet(color[2]);
    diffuse->x = v8fAdd(diffuse->x, v8fMul(cr, amount));
    diffuse->y = v8fAdd(diffuse->y, v8fMul(cg, amount));
    diffuse->z = v8fAdd(diffuse->z, v8fMul(cb, amount));
    if (shiny) {
        Lanes3 sum = {v8fAdd(view.x, toLight.x), v8fAdd(view.y, toLight.y), v8fAdd(view.z, toLight.z)};
        Lanes3 h = normalize3(sum);
        V8F ndh = v8fMax(dot3(n, h), zero), vdh = v8fMax(dot3(view, h), zero);
        V8F d = v8fAdd(v8fMul(v8fMul(ndh, ndh), v8fSub(r->a2, one)), one);
        V8F distribution = v8fDiv(r->a2, v8fMul(d, d));
        V8F rest = v8fSub(one, r->k);
        V8F seen = v8fMul(v8fAdd(v8fMul(r->ndv, rest), r->k), v8fAdd(v8fMul(ndl, rest), r->k));
        V8F lit = v8fDiv(v8fMul(v8fMul(distribution, ndl), v8fMul(strength, r->strength)), v8fMul(seen, v8fSet(4.f)));
        lit = v8fSelect(v8fGt(ndl, zero), zero, lit);
        V8F far = fifth(vdh);
        highlight->x = v8fAdd(highlight->x, v8fMul(v8fMul(cr, lit), v8fAdd(r->f0.x, v8fMul(v8fSub(one, r->f0.x), far))));
        highlight->y = v8fAdd(highlight->y, v8fMul(v8fMul(cg, lit), v8fAdd(r->f0.y, v8fMul(v8fSub(one, r->f0.y), far))));
        highlight->z = v8fAdd(highlight->z, v8fMul(v8fMul(cb, lit), v8fAdd(r->f0.z, v8fMul(v8fSub(one, r->f0.z), far))));
    }
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
typedef struct LitOut { V8I color; V8F alpha; V8I surface; } LitOut;
#define ARTF_POST_ALPHA (ARTF_TILE_PIXELS * 4)   /* from a color, in its buffer: its alpha, as a float */
#define ARTF_POST_SURFACE (ARTF_TILE_PIXELS * 8) /* and its surface */

V8_INLINE void storeLit(const ArtfSoftFrame* f, uint32_t* at, LitOut o) {
    v8iStore(at, o.color);
    if (f->post) {
        v8fStore((float*)(at + ARTF_POST_ALPHA), o.alpha);
        v8iStore(at + ARTF_POST_SURFACE, o.surface);
    }
}

/* Narkowicz's fit to ACES: what is very bright rolls off to white. As lensToneMap, and the GPU's. */
V8_INLINE V8F toneMap(V8F x) {
    const V8F top = v8fMul(x, v8fAdd(v8fMul(x, v8fSet(2.51f)), v8fSet(.03f)));
    const V8F bottom = v8fAdd(v8fMul(x, v8fAdd(v8fMul(x, v8fSet(2.43f)), v8fSet(.59f))), v8fSet(.14f));
    return v8fMin(v8fDiv(top, bottom), v8fSet(1.f));
}

/* Linear light to a pixel: the frame's colors are the square root of light, which is close to what
 * screens expect and costs one instruction. The GPU does the same. */
V8_INLINE V8I packColor(Lanes3 c) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f), scale = v8fSet(255.f), half = v8fSet(.5f);
    V8I r = v8fToI(v8fAdd(v8fMul(v8fSqrt(v8fMin(v8fMax(c.x, zero), one)), scale), half));
    V8I g = v8fToI(v8fAdd(v8fMul(v8fSqrt(v8fMin(v8fMax(c.y, zero), one)), scale), half));
    V8I b = v8fToI(v8fAdd(v8fMul(v8fSqrt(v8fMin(v8fMax(c.z, zero), one)), scale), half));
    return v8iOr(v8iOr(b, v8iShl(g, 8)), v8iOr(v8iShl(r, 16), v8iSet((int32_t)0xFF000000u)));
}

/* The same for a layer: premultiplied, in the frame's colors, as what it goes over is. */
V8_INLINE V8I packLayer(Lanes3 c, V8F alpha) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f), half = v8fSet(.5f);
    const V8F a = v8fMin(v8fMax(alpha, zero), one), scale = v8fMul(a, v8fSet(255.f));
    V8I r = v8fToI(v8fAdd(v8fMul(v8fSqrt(v8fMin(v8fMax(c.x, zero), one)), scale), half));
    V8I g = v8fToI(v8fAdd(v8fMul(v8fSqrt(v8fMin(v8fMax(c.y, zero), one)), scale), half));
    V8I b = v8fToI(v8fAdd(v8fMul(v8fSqrt(v8fMin(v8fMax(c.z, zero), one)), scale), half));
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
    const ArtfTextureData* sizing; /* what says which mipmap: the first map it has */
    float opacity;
    float tangent[3], bitangent[3]; /* for a normal map: where u and v go, in the world */
    float A[3], B[3], C[3];
    float wp[3][3], nrm[3][3], uv[3][2];
    float col[3][4];  /* in linear light, and alpha */
};

/* Pixels waiting for their light: they may come from many triangles of one material. Nothing is
 * cheaper per pixel than a full vector, and a dense mesh's triangles cover two or three pixels each:
 * so what a triangle does alone is only to interpolate, and light goes eight pixels at a time. */
typedef struct LitBatch {
    _Alignas(32) float nx[8], ny[8], nz[8], wx[8], wy[8], wz[8], u[8], v[8], r[8], g[8], b[8], a[8], tx[8], ty[8], tz[8], bx[8], by[8], bz[8];
    uint16_t at[8];
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

/* How much light gets to eight places, one by one: shadow maps are read at random, so lanes gain little. */
static V8F shadowLanes(const ArtfSoftFrame* f, int light, Lanes3 world, Lanes3 n) {
    _Alignas(32) float wx[8], wy[8], wz[8], nx[8], ny[8], nz[8], out[8];
    v8fStore(wx, world.x); v8fStore(wy, world.y); v8fStore(wz, world.z);
    v8fStore(nx, n.x); v8fStore(ny, n.y); v8fStore(nz, n.z);
    for (int i = 0; i < 8; ++i) {
        const float p[3] = {wx[i], wy[i], wz[i]}, normal[3] = {nx[i], ny[i], nz[i]};
        out[i] = light < 0 ? artfShadowOfSun(f->shadows, f->data.cameraPos, p, normal) : artfShadowOfLight(f->shadows, (uint32_t)light, f->data.lights[light].position, p, normal);
    }
    return v8fLoad(out);
}

/* The color of eight pixels. A light out of reach adds exactly nothing, so which lights are looked
 * at changes the time this takes, never the pixels. */
static LitOut lightLanes(const ArtfSoftFrame* f, const ArtfFrameData* frame, const ArtfMaterialData* material, const ArtfTextureData* textures, const ArtfTextureData* texture, uint32_t lights, const uint32_t* extra, int level, Lanes3 rawN, Lanes3 world, V8F u, V8F v,
                      bool colored, bool layered, Lanes3 tint, V8F alpha, const Lanes3* tangent, const Lanes3* bitangent) {
    const V8F zero = v8fSet(0.f), one = v8fSet(1.f);
    const bool shiny = material->specular > 0 || material->metallic > 0 || material->graph;
    Lanes3 n = normalize3(rawN);

    /* what it is made of, here */
#define MAP_LEVEL(t) artfMini(artfMaxi(level, (t)->residentMip), (t)->mipCount - 1)
#define SAMPLE(t, out) sampleTexture((t)->pixels + (t)->offsets[MAP_LEVEL(t)], artfMaxi((t)->width >> MAP_LEVEL(t), 1), artfMaxi((t)->height >> MAP_LEVEL(t), 1), u, v, out)
    Lanes3 albedo = {v8fSet(material->baseColor[0]), v8fSet(material->baseColor[1]), v8fSet(material->baseColor[2])};
    if (texture) {
        V8F texel = v8fSet(1.f);
        Lanes3 t = SAMPLE(texture, material->flags & ARTF_MATERIAL_BLEND ? &texel : NULL);
        alpha = v8fMul(alpha, texel);
        albedo.x = v8fMul(albedo.x, v8fMul(t.x, t.x));
        albedo.y = v8fMul(albedo.y, v8fMul(t.y, t.y));
        albedo.z = v8fMul(albedo.z, v8fMul(t.z, t.z));
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
        const float* eye = frame->cameraPos;
        for (int i = 0; i < 8; ++i) {
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

    /* light from all around, then the sun */
    V8F up = v8fAdd(v8fMul(n.y, v8fSet(.5f)), v8fSet(.5f));
    Lanes3 diffuse = {v8fAdd(v8fSet(frame->ground[0]), v8fMul(up, v8fSet(frame->sky[0] - frame->ground[0]))),
                      v8fAdd(v8fSet(frame->ground[1]), v8fMul(up, v8fSet(frame->sky[1] - frame->ground[1]))),
                      v8fAdd(v8fSet(frame->ground[2]), v8fMul(up, v8fSet(frame->sky[2] - frame->ground[2])))};
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
    if (frame->indirect[0][3] > 0) {
        /* or from the probes near it: light that bounced */
        _Alignas(32) float laneW[3][8], laneD[3][8], got[3][8];
        v8fStore(laneW[0], world.x); v8fStore(laneW[1], world.y); v8fStore(laneW[2], world.z);
        v8fStore(laneD[0], n.x); v8fStore(laneD[1], n.y); v8fStore(laneD[2], n.z);
        for (int i = 0; i < 8; ++i) {
            const float p[3] = {laneW[0][i], laneW[1][i], laneW[2][i]}, nn[3] = {laneD[0][i], laneD[1][i], laneD[2][i]};
            float c[3];
            artfIndirectAt(&f->scene->indirect, p, nn, c);
            for (int k = 0; k < 3; ++k) got[k][i] = c[k] * frame->indirect[0][3];
        }
        diffuse.x = v8fLoad(got[0]); diffuse.y = v8fLoad(got[1]); diffuse.z = v8fLoad(got[2]);
    }
    diffuse.x = v8fMul(diffuse.x, open);
    diffuse.y = v8fMul(diffuse.y, open);
    diffuse.z = v8fMul(diffuse.z, open);
    Lanes3 highlight = {zero, zero, zero};
    Lanes3 view = {zero, zero, zero};
    Reflecting reflecting;
    if (shiny) {
        Lanes3 toEye = {v8fSub(v8fSet(frame->cameraPos[0]), world.x), v8fSub(v8fSet(frame->cameraPos[1]), world.y), v8fSub(v8fSet(frame->cameraPos[2]), world.z)};
        view = normalize3(toEye);
        const V8F a = v8fMul(rough, rough), plusOne = v8fAdd(rough, one), dielectric = v8fSet(.04f * material->specular), notMetal = v8fSub(one, metal);
        reflecting.a2 = v8fMul(a, a);
        reflecting.k = v8fMul(v8fMul(plusOne, plusOne), v8fSet(.125f));
        reflecting.ndv = v8fMax(dot3(n, view), v8fSet(1e-4f));
        reflecting.strength = v8fSet(material->specular);
        reflecting.f0.x = v8fAdd(v8fMul(dielectric, notMetal), v8fMul(albedo.x, metal));
        reflecting.f0.y = v8fAdd(v8fMul(dielectric, notMetal), v8fMul(albedo.y, metal));
        reflecting.f0.z = v8fAdd(v8fMul(dielectric, notMetal), v8fMul(albedo.z, metal));
        /* what comes from all around, in the mirror direction: all a metal shows in a plain sky */
        const V8F twice = v8fMul(dot3(n, view), v8fSet(2.f));
        const V8F reflectedUp = v8fAdd(v8fMul(v8fSub(v8fMul(n.y, twice), view.y), v8fSet(.5f)), v8fSet(.5f));
        const V8F far = v8fMul(fifth(reflecting.ndv), open), smooth = v8fSub(one, rough);
#define AMBIENT(c, f) { const V8F env = v8fAdd(v8fSet(frame->ground[c]), v8fMul(reflectedUp, v8fSet(frame->sky[c] - frame->ground[c]))); \
                        highlight.f = v8fMul(env, v8fAdd(v8fMul(reflecting.f0.f, open), v8fMul(v8fSub(v8fMax(smooth, reflecting.f0.f), reflecting.f0.f), far))); }
        AMBIENT(0, x)
        AMBIENT(1, y)
        AMBIENT(2, z)
#undef AMBIENT
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
            const Lanes3 env = {v8fLoad(got[0]), v8fLoad(got[1]), v8fLoad(got[2])};
            highlight.x = v8fMul(env.x, v8fAdd(v8fMul(reflecting.f0.x, open), v8fMul(v8fSub(v8fMax(smooth, reflecting.f0.x), reflecting.f0.x), far)));
            highlight.y = v8fMul(env.y, v8fAdd(v8fMul(reflecting.f0.y, open), v8fMul(v8fSub(v8fMax(smooth, reflecting.f0.y), reflecting.f0.y), far)));
            highlight.z = v8fMul(env.z, v8fAdd(v8fMul(reflecting.f0.z, open), v8fMul(v8fSub(v8fMax(smooth, reflecting.f0.z), reflecting.f0.z), far)));
        }
        const V8F keep = v8fSub(one, metal);
        albedo.x = v8fMul(albedo.x, keep);
        albedo.y = v8fMul(albedo.y, keep);
        albedo.z = v8fMul(albedo.z, keep);
    }
    const Lanes3 aroundDiffuse = diffuse, aroundHighlight = highlight; /* (for ambient occlusion) */
    if (frame->sunColor[0] > 0 || frame->sunColor[1] > 0 || frame->sunColor[2] > 0) {
        Lanes3 toSun = {v8fSet(frame->toSun[0]), v8fSet(frame->toSun[1]), v8fSet(frame->toSun[2])};
        addLight(n, view, toSun, f->shadows ? shadowLanes(f, -1, world, n) : v8fSet(1.f), frame->sunColor, shiny, &reflecting, &diffuse, &highlight);
    }
    for (uint32_t i = 0; lights >> i; ++i) {
        if (!(lights >> i & 1)) continue;
        const ArtfLightData* L = &frame->lights[i];
        Lanes3 d = {v8fSub(v8fSet(L->position[0]), world.x), v8fSub(v8fSet(L->position[1]), world.y), v8fSub(v8fSet(L->position[2]), world.z)};
        V8F d2 = dot3(d, d);
        V8F invLength = v8fDiv(v8fSet(1.f), v8fSqrt(v8fMax(d2, v8fSet(1e-8f))));
        Lanes3 toLight = {v8fMul(d.x, invLength), v8fMul(d.y, invLength), v8fMul(d.z, invLength)};
        V8F fade = v8fMax(v8fSub(v8fSet(1.f), v8fMul(d2, v8fSet(1.f / (L->radius * L->radius)))), zero);
        Lanes3 goes = {v8fSet(L->direction[0]), v8fSet(L->direction[1]), v8fSet(L->direction[2])};
        V8F spot = v8fMul(v8fSub(v8fSub(zero, dot3(toLight, goes)), v8fSet(L->spotCut)), v8fSet(L->spotScale));
        spot = v8fMin(v8fMax(spot, zero), v8fSet(1.f));
        if (f->shadows && f->shadows->frame->lights[i][0]) spot = v8fMul(spot, shadowLanes(f, (int)i, world, n));
        addLight(n, view, toLight, v8fMul(v8fMul(fade, fade), spot), L->color, shiny, &reflecting, &diffuse, &highlight);
    }
    /* those past them that reach this tile: the same light, with no shadow */
    for (uint32_t k = 0; extra && k < extra[0]; ++k) {
        const ArtfLightData* L = &f->extra[extra[1 + k]];
        Lanes3 d = {v8fSub(v8fSet(L->position[0]), world.x), v8fSub(v8fSet(L->position[1]), world.y), v8fSub(v8fSet(L->position[2]), world.z)};
        V8F d2 = dot3(d, d);
        V8F invLength = v8fDiv(v8fSet(1.f), v8fSqrt(v8fMax(d2, v8fSet(1e-8f))));
        Lanes3 toLight = {v8fMul(d.x, invLength), v8fMul(d.y, invLength), v8fMul(d.z, invLength)};
        V8F fade = v8fMax(v8fSub(v8fSet(1.f), v8fMul(d2, v8fSet(1.f / (L->radius * L->radius)))), zero);
        if (v8iSigns(v8fGt(fade, zero)) == 0) continue; /* (none of the eight is in reach) */
        Lanes3 goes = {v8fSet(L->direction[0]), v8fSet(L->direction[1]), v8fSet(L->direction[2])};
        V8F spot = v8fMul(v8fSub(v8fSub(zero, dot3(toLight, goes)), v8fSet(L->spotCut)), v8fSet(L->spotScale));
        spot = v8fMin(v8fMax(spot, zero), v8fSet(1.f));
        addLight(n, view, toLight, v8fMul(v8fMul(fade, fade), spot), L->color, shiny, &reflecting, &diffuse, &highlight);
    }

    Lanes3 lit = {v8fAdd(v8fAdd(v8fMul(albedo.x, diffuse.x), highlight.x), own.x),
                  v8fAdd(v8fAdd(v8fMul(albedo.y, diffuse.y), highlight.y), own.y),
                  v8fAdd(v8fAdd(v8fMul(albedo.z, diffuse.z), highlight.z), own.z)};
    if (frame->look[1] > 0 && !f->postFog) {
        Lanes3 fromEye = {v8fSub(world.x, v8fSet(frame->cameraPos[0])), v8fSub(world.y, v8fSet(frame->cameraPos[1])), v8fSub(world.z, v8fSet(frame->cameraPos[2]))};
        /* (with no exp, whose bits differ from one C library to the next) */
        const V8F air = v8fMul(v8fSqrt(dot3(fromEye, fromEye)), v8fSet(frame->look[1]));
        const V8F keep = v8fDiv(v8fSet(1.f), v8fAdd(v8fAdd(v8fSet(1.f), air), v8fMul(v8fMul(air, air), v8fSet(.5f))));
        const V8F fr = v8fSet(frame->fog[0]), fg = v8fSet(frame->fog[1]), fb = v8fSet(frame->fog[2]);
        lit.x = v8fAdd(fr, v8fMul(v8fSub(lit.x, fr), keep));
        lit.y = v8fAdd(fg, v8fMul(v8fSub(lit.y, fg), keep));
        lit.z = v8fAdd(fb, v8fMul(v8fSub(lit.z, fb), keep));
    }
    LitOut out;
    if (f->post) {
        /* light as it is: exposure, tone mapping and fog come after */
        const V8F a = layered ? v8fMin(v8fMax(alpha, zero), one) : one;
        const Lanes3 kept = {v8fMul(lit.x, a), v8fMul(lit.y, a), v8fMul(lit.z, a)};
        out.color = pack9e5(kept);
        out.alpha = a;
        const V8F all = v8fAdd(v8fAdd(v8fMul(lit.x, v8fSet(.3f)), v8fMul(lit.y, v8fSet(.59f))), v8fMul(lit.z, v8fSet(.11f)));
        const V8F around = v8fAdd(v8fAdd(v8fMul(v8fAdd(v8fMul(albedo.x, aroundDiffuse.x), aroundHighlight.x), v8fSet(.3f)), v8fMul(v8fAdd(v8fMul(albedo.y, aroundDiffuse.y), aroundHighlight.y), v8fSet(.59f))),
                                  v8fMul(v8fAdd(v8fMul(albedo.z, aroundDiffuse.z), aroundHighlight.z), v8fSet(.11f)));
        out.surface = packSurface(n, rough, metal, v8fDiv(around, v8fMax(all, v8fSet(1e-6f))));
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

static void batchFlush(LitBatch* b, const ArtfSoftFrame* f, uint32_t* color) {
    const ArtfFrameData* frame = &f->data;
    if (!b->count) return;
    for (int i = b->count; i < 8; ++i) { /* (what is left of the vector: any number that is not a NaN) */
        b->nx[i] = b->nx[0]; b->ny[i] = b->ny[0]; b->nz[i] = b->nz[0];
        b->wx[i] = b->wx[0]; b->wy[i] = b->wy[0]; b->wz[i] = b->wz[0];
        b->u[i] = b->u[0]; b->v[i] = b->v[0];
        if (b->colored) { b->r[i] = b->r[0]; b->g[i] = b->g[0]; b->b[i] = b->b[0]; b->a[i] = b->a[0]; }
        if (b->bumpy) { b->tx[i] = b->tx[0]; b->ty[i] = b->ty[0]; b->tz[i] = b->tz[0]; b->bx[i] = b->bx[0]; b->by[i] = b->by[0]; b->bz[i] = b->bz[0]; }
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
    const LitOut lit = lightLanes(f, frame, b->material, b->textures, b->texture, b->lights, b->extra, b->level, n, world, v8fLoad(b->u), v8fLoad(b->v), b->colored, b->layered, tint, alpha, &tangent, &bitangent);
    v8iStore(colors, lit.color);
    for (int i = 0; i < b->count; ++i) color[b->at[i]] = colors[i];
    if (f->post) {
        v8fStore(alphas, lit.alpha);
        v8iStore(surfaces, lit.surface);
        for (int i = 0; i < b->count; ++i) {
            ((float*)(color + ARTF_POST_ALPHA))[b->at[i]] = alphas[i];
            color[ARTF_POST_SURFACE + b->at[i]] = surfaces[i];
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

static void shadeSetup(ShadeContext* s, const ArtfSoftFrame* f, const ArtfRasterTri* t, int ox, int oy) {
    const ArtfVisible* v = &f->visible[t->instance];
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
    s->needWorld = material->specular > 0 || material->metallic > 0 || frame->lightCount || f->extraCount || frame->indirect[0][3] > 0 || frame->look[1] > 0 || f->shadows || material->graph;
    s->bumpy = material->normalMap != 0;
    const ArtfTextureData* textures = f->scene->textures.items;
    s->sizing = s->texture ? s->texture : material->normalMap ? &textures[material->normalMap] : material->surfaceMap ? &textures[material->surfaceMap] : material->occlusionMap ? &textures[material->occlusionMap] : NULL;
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
        s->uv[i][0] = artfHalfToFloat(pv->u);
        s->uv[i][1] = artfHalfToFloat(pv->v);
        if (s->colored) {
            /* (squared, as a texture's texels are) */
            const uint32_t word = mesh->colors[mesh->meshletVertices[l->vertexOffset + li]];
            const float r = (float)(word >> 16 & 255) * (1.f / 255.f), g = (float)(word >> 8 & 255) * (1.f / 255.f), b = (float)(word & 255) * (1.f / 255.f);
            s->col[i][0] = r * r;
            s->col[i][1] = g * g;
            s->col[i][2] = b * b;
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
                storeLit(f, color + y * ARTF_TILE + x, lightLanes(f, frame, s.material, f->scene->textures.items, s.texture, s.lights, batch->extra, mipAt(&s, (float)x + 3.5f, fy), n, world, LERP(uv, 0), LERP(uv, 1), s.colored, s.layered, tint, alpha, &tangent, &bitangent));
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
                batch->lights |= s.lights;
                if ((batch->count = i + 1) == 8) batchFlush(batch, f, color);
            }
        }
    }
}

/* The triangles that show in a plane, into its colors, each pixel once. */
static void shadePlane(ArtfSoftFrame* f, ArtfWorker* w, const ArtfRasterTri* const* tris, size_t count, const uint32_t* plane, const uint32_t* first, uint32_t* color, int ox, int oy, int tw, int th) {
    artfVecReserve(&w->seen, count + 1);
    memset(w->seen.items, 0, count + 1);
    uint8_t* seen = w->seen.items;
    for (int y = 0; y < th; ++y) {
        const uint32_t* ids = plane + y * ARTF_TILE;
        const uint32_t* same = first ? first + y * ARTF_TILE : NULL;
        uint32_t last = 0;
        for (int x = 0; x < tw; ++x)
            if (ids[x] != last && (!same || ids[x] != same[x])) { last = ids[x]; seen[last] = 1; }
    }
    LitBatch batch;
    batch.count = 0;
    batch.lights = 0;
    batch.extra = f->extraCount ? f->tileLights + (size_t)((oy / ARTF_TILE) * f->tilesX + ox / ARTF_TILE) * (1 + ARTF_TILE_LIGHTS) : NULL;
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

#include "software/lens.inc"

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

/* What the tile's colors were made of before: a black hole's, already tone mapped, back to light. */
static void postFromBytes(uint32_t* c, int count) {
    const V8F inverse = v8fSet(1.f / 255.f);
    const V8I byte = v8iSet(255);
    for (int i = 0; i < count; i += 8) {
        const V8I p = v8iLoad(c + i);
        const V8F a = v8fMul(v8iToF(v8iShr(p, 24)), inverse);
        const V8F r = v8fMul(v8iToF(v8iAnd(v8iShr(p, 16), byte)), inverse), g = v8fMul(v8iToF(v8iAnd(v8iShr(p, 8), byte)), inverse), b = v8fMul(v8iToF(v8iAnd(p, byte)), inverse);
        /* (premultiplied in the frame's colors: light is their square, over alpha, times alpha) */
        const V8F over = v8fDiv(v8fSet(1.f), v8fMax(a, v8fSet(1e-6f)));
        const Lanes3 light = {v8fMul(v8fMul(r, r), over), v8fMul(v8fMul(g, g), over), v8fMul(v8fMul(b, b), over)};
        v8iStore(c + i, pack9e5(light));
        v8fStore((float*)(c + ARTF_POST_ALPHA + i), a);
        v8iStore(c + ARTF_POST_SURFACE + i, v8iSet(0));
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
        for (end = at; end < count && tris[end]->instance == tris[at]->instance; ++end) {}
        for (int i = 0; i < ARTF_TILE_PIXELS * f->samples; i += 8) v8iStore(w->ids + i, zeroI);
        for (size_t i = at; i < end; ++i) rasterizeSamples(f, w, tris[i], (uint32_t)(i - at + 1), ox, oy);
        for (int k = 0; k < f->samples; ++k) {
            const size_t plane = (size_t)k * ARTF_TILE_PIXELS;
            shadePlane(f, w, tris + at, end - at, w->ids + plane, k ? w->ids : NULL, w->layer + plane, ox, oy, tw, th);
        }
        for (int k = 0; k < f->samples; ++k) {
            const size_t plane = (size_t)k * ARTF_TILE_PIXELS;
            for (int i = 0; i < ARTF_TILE_PIXELS; i += 8) {
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

/* Layers by each pixel's depth: each is drawn against what is solid alone, and what it leaves in a pixel goes in the
 * pixel's list. Then each list goes over the frame, the farthest first. */
static void drawLayersSorted(ArtfSoftFrame* f, ArtfWorker* w, int ox, int oy, int tw, int th) {
    enum { MOST = ARTF_LAYER_FRAGMENTS };
    const size_t all = (size_t)ARTF_TILE_PIXELS * (size_t)f->samples;
    if (!w->solidDepth) {
        w->solidDepth = artfMalloc(all * 4);
        w->fragmentDepth = artfMalloc(all * MOST * 4);
        w->fragmentColor = artfMalloc(all * MOST * 4);
        w->fragmentCount = artfMalloc(all);
    }
    memcpy(w->solidDepth, w->depth, all * 4);
    memset(w->fragmentCount, 0, all);
    const V8I zeroI = v8iSet(0);
    const ArtfRasterTri* const* tris = w->layers.items;
    const size_t count = w->layers.len;
    for (size_t at = 0, end; at < count; at = end) {
        for (end = at; end < count && tris[end]->instance == tris[at]->instance; ++end) {}
        memcpy(w->depth, w->solidDepth, all * 4);
        for (size_t i = 0; i < all; i += 8) v8iStore(w->ids + i, zeroI);
        for (size_t i = at; i < end; ++i) rasterizeSamples(f, w, tris[i], (uint32_t)(i - at + 1), ox, oy);
        for (int k = 0; k < f->samples; ++k) {
            const size_t plane = (size_t)k * ARTF_TILE_PIXELS;
            shadePlane(f, w, tris + at, end - at, w->ids + plane, k ? w->ids : NULL, w->layer + plane, ox, oy, tw, th);
        }
        for (size_t i = 0; i < all; ++i) {
            if (!w->ids[i]) continue;
            /* (what shows the same as in the first sample was shaded there) */
            const size_t inFirst = i % ARTF_TILE_PIXELS;
            const uint32_t color = w->ids[i] == w->ids[inFirst] ? w->layer[inFirst] : w->layer[i];
            float* depths = w->fragmentDepth + (size_t)i * MOST;
            uint32_t* colors = w->fragmentColor + (size_t)i * MOST;
            int n = w->fragmentCount[i];
            if (n == MOST) {
                /* the farthest goes: depth is greater when nearer */
                if (w->depth[i] <= depths[0]) continue;
                --n;
                memmove(depths, depths + 1, (size_t)n * 4);
                memmove(colors, colors + 1, (size_t)n * 4);
            }
            int place = n;
            while (place > 0 && depths[place - 1] > w->depth[i]) {
                depths[place] = depths[place - 1];
                colors[place] = colors[place - 1];
                --place;
            }
            depths[place] = w->depth[i];
            colors[place] = color;
            w->fragmentCount[i] = (uint8_t)(n + 1);
        }
    }
    for (size_t i = 0; i < all; ++i) {
        uint32_t under = w->color[i];
        for (int k = 0; k < w->fragmentCount[i]; ++k) {
            const uint32_t top = w->fragmentColor[(size_t)i * MOST + (size_t)k], rest = 255 - (top >> 24);
            uint32_t t0 = (under & 255) * rest + 128, t1 = ((under >> 8) & 255) * rest + 128, t2 = ((under >> 16) & 255) * rest + 128, t3 = (under >> 24) * rest + 128;
            t0 = (t0 + (t0 >> 8)) >> 8;
            t1 = (t1 + (t1 >> 8)) >> 8;
            t2 = (t2 + (t2 >> 8)) >> 8;
            t3 = (t3 + (t3 >> 8)) >> 8;
            under = top + (t0 | t1 << 8 | t2 << 16 | t3 << 24);
        }
        w->color[i] = under;
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
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            const float px = ((float)(ox + x) + .5f) / (float)f->width * 2.f - 1.f, py = 1.f - ((float)(oy + y) + .5f) / (float)f->height * 2.f;
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
                bits |= (uint32_t)(sqrtf(artfClampf(v, 0, 1)) * alpha * 255.f + .5f) << (16 - 8 * k);
            }
            w->color[y * ARTF_TILE + x] = bits;
        }
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
    if (f->hole) {
        lensFill(f, w, ox, oy, tw, th);
        if (f->post) postFromBytes(w->color, ARTF_TILE_PIXELS);
    } else if (f->post) {
        /* the background as light: what shows, squared */
        const float* bg = f->data.background;
        const float a = bg[3], over = a > 0 ? 1.f / a : 0.f;
        const Lanes3 light = {v8fSet(bg[0] * bg[0] * over), v8fSet(bg[1] * bg[1] * over), v8fSet(bg[2] * bg[2] * over)};
        const V8I packed = pack9e5(light);
        for (int i = 0; i < ARTF_TILE_PIXELS; i += 8) {
            v8iStore(w->color + i, packed);
            v8fStore((float*)(w->color + ARTF_POST_ALPHA + i), v8fSet(a));
            v8iStore(w->color + ARTF_POST_SURFACE + i, v8iSet(0));
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
        }
    }

    if (w->tris.len) {
        /* the triangles that still show after all the others */
        for (int k = 0; k < f->samples; ++k) {
            const size_t plane = (size_t)k * ARTF_TILE_PIXELS;
            shadePlane(f, w, w->tris.items, w->tris.len, w->ids + plane, k ? w->ids : NULL, w->color + plane, ox, oy, tw, th);
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
    /* (what comes after reads the depth of what is solid: layers write theirs over it) */
    if (f->post) memcpy(w->postDepth, w->depth, ARTF_TILE_PIXELS * 4);
    if (w->layers.len) {
        if (f->scene->features[ARTF_FEATURE_LAYERS_SORTED] && !f->post) drawLayersSorted(f, w, ox, oy, tw, th);
        else drawLayers(f, w, ox, oy, tw, th);
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
                    | (uint32_t)(sqrtf(artfClampf(light[0], 0, 1)) * alpha * 255.f + .5f) << 16
                    | (uint32_t)(sqrtf(artfClampf(light[1], 0, 1)) * alpha * 255.f + .5f) << 8
                    | (uint32_t)(sqrtf(artfClampf(light[2], 0, 1)) * alpha * 255.f + .5f);
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

const ArtfKernels KERNELS = {KERNELS_NAME, geometry, tile, depthTile, lensTile};
