/* artful — the software backend: drives the kernels through a frame, on every core. */
#include "render/rendererInternal.h"
#include "software/softwareInternal.h"
#include "core/threads.h"
#include "software/denoise.h"

#include <stdio.h>

#if defined(_MSC_VER) && (defined(ARTFUL_HAVE_AVX2) || defined(ARTFUL_HAVE_SSE41))
#include <intrin.h>
#include <immintrin.h>
#endif

typedef struct Software {
    ArtfRenderer base;
    ArtfPool* pool;
    const ArtfKernels* kernels;
    ArtfSoftFrame f;
    uint32_t* pixels;
    ArtfVisible* groups[ARTF_CHUNKS]; /* what each group of instances kept */
    ArtfVisible* candidates;          /* in view, but not seen last frame */
    ARTF_VEC(ArtfLayer) layers;       /* what shows what is behind it, in view */
    uint32_t candidateCount, earlyCount;
    uint8_t* wasVisible;              /* of each instance of the scene, last frame */
    uint8_t* hidden;                  /* what the test found, for each early, then each candidate */
    uint32_t hiddenCount;
    uint32_t flagCapacity;
    uint32_t groupCounts[ARTF_CHUNKS];
    uint32_t visibleCapacity;
    char description[96];
    double cpuMs, cullMs, geometryMs, rasterMs, lensMs;
    uint32_t tilesSkipped[16];
    uint32_t bloomLevel;  /* the one being made */
    uint64_t traceKey;    /* of what traceSum holds */
    uint32_t traceAdding; /* samples this frame */
    uint32_t traceFrames, traceCounted; /* with a filter: all samples, for their numbers; and of what is shown */
    ArtfTraceFrame traceFrame;
    ArtfShadowMaps* shadowMaps;
    ArtfDenoiser denoiser;
    bool denoising;
    size_t hdrPixels, bloomPixels;
    ArtfSoftPost post;
    uint32_t postFrames;  /* for the jitter */
    float previous[16];   /* the frame before's viewProj */
    bool hasPrevious;
} Software;

#ifdef ARTFUL_HAVE_AVX2
static bool haveAvx2(void) {
#ifdef _MSC_VER
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    if (!(r[2] >> 27 & 1) || !(r[2] >> 28 & 1) || (_xgetbv(0) & 6) != 6) return false; /* (the system keeps its registers) */
    __cpuidex(r, 7, 0);
    return r[1] >> 5 & 1;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#endif
}
#endif

#ifdef ARTFUL_HAVE_SSE41
static bool haveSse41(void) {
#ifdef _MSC_VER
    int r[4];
    __cpuid(r, 1);
    return r[2] >> 19 & 1;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("sse4.1");
#endif
}
#endif

static const ArtfKernels* chooseKernels(ArtfSimd simd) {
    (void)simd;
#ifdef ARTFUL_HAVE_AVX2
    if ((simd == ARTF_SIMD_AUTO || simd == ARTF_SIMD_256) && haveAvx2()) return &artfKernelsAvx2;
#endif
#ifdef ARTFUL_HAVE_SSE41
    if (simd != ARTF_SIMD_PORTABLE && haveSse41()) return &artfKernelsSse41;
#endif
#ifdef ARTFUL_HAVE_NEON
    if (simd != ARTF_SIMD_PORTABLE) return &artfKernelsNeon;
#endif
    return &artfKernelsPortable;
}

/* ---- the size ---- */

static void allocateTargets(Software* s, int width, int height) {
    ArtfSoftFrame* f = &s->f;
    artfAlignedFree(s->pixels);
    artfFree(f->traceSum);
    f->traceSum = NULL;
    f->traceDone = 0;
    f->width = width;
    f->height = height;
    f->pixelW = 1.0 / width;
    f->pixelH = 1.0 / height;
    f->tilesX = (width + ARTF_TILE - 1) / ARTF_TILE;
    f->tilesY = (height + ARTF_TILE - 1) / ARTF_TILE;
    const size_t tiles = (size_t)f->tilesX * f->tilesY;
    s->pixels = artfAlignedMalloc((size_t)width * height * 4, 64);
    memset(s->pixels, 0, (size_t)width * height * 4);
    f->pixels = s->pixels;
    for (int i = 0; i < ARTF_CHUNKS; ++i) {
        artfFree(f->chunks[i].heads);
        f->chunks[i].heads = artfCalloc(tiles, sizeof *f->chunks[i].heads);
    }
    artfFree(f->tileClass);
    artfFree(f->tileDirty);
    f->tileClass = artfCalloc(tiles, 1);
    f->tileDirty = artfMalloc(tiles);
    memset(f->tileDirty, 1, tiles);
    f->cellsX = f->tilesX * (ARTF_TILE / ARTF_HIZ_CELL);
    f->cellsY = f->tilesY * (ARTF_TILE / ARTF_HIZ_CELL);
    artfFree(f->hizCells);
    artfFree(f->hizTiles);
    f->hizCells = artfCalloc((size_t)f->cellsX * f->cellsY, 4);
    f->hizTiles = artfCalloc(tiles, 4);
    if (f->post) {
        const size_t pixels = (size_t)width * height;
        for (int c = 0; c < 4; ++c) {
            artfFree(f->postLight[c]);
            f->postLight[c] = artfCalloc(pixels, 4);
        }
        artfFree(f->postDepth);
        artfFree(f->postSurface);
        f->postDepth = artfCalloc(pixels, 4);
        f->postSurface = artfCalloc(pixels, 4);
        artfSoftPostSize(&s->post, width, height);
        s->hasPrevious = false;
    }
    /* as far out as the integers of the rasterizer go */
    f->guardX = 2.f * 15000.f / (float)width;
    f->guardY = 2.f * 15000.f / (float)height;
}

static void softResize(ArtfRenderer* r, int width, int height) {
    Software* s = (Software*)r;
    if (width <= 0 || height <= 0 || (width == s->f.width && height == s->f.height)) return;
    allocateTargets(s, width, height);
}

/* ---- the overlay ---- */

static void softSetOverlay(ArtfRenderer* r, const ArtfOverlay* o) {
    Software* s = (Software*)r;
    ArtfSoftFrame* f = &s->f;
    const size_t tiles = (size_t)f->tilesX * f->tilesY;
    if (!o || !o->pixels) {
        f->overlay = NULL;
        memset(f->tileDirty, 1, tiles);
        return;
    }
    const bool same = f->overlayWidth == o->width && f->overlayHeight == o->height && f->overlay;
    f->overlay = o->pixels;
    f->overlayWidth = o->width;
    f->overlayHeight = o->height;
    f->overlayStride = o->stride ? o->stride : o->width;
    if (!o->partial || !same) {
        memset(f->tileDirty, 1, tiles);
        return;
    }
    for (size_t i = 0; i < o->damageCount; ++i) {
        const ArtfRect d = o->damage[i];
        const int tx0 = artfMaxi(d.x, 0) / ARTF_TILE, ty0 = artfMaxi(d.y, 0) / ARTF_TILE;
        const int tx1 = artfMini((d.x + d.w - 1) / ARTF_TILE, f->tilesX - 1), ty1 = artfMini((d.y + d.h - 1) / ARTF_TILE, f->tilesY - 1);
        for (int y = ty0; y <= ty1; ++y)
            for (int x = tx0; x <= tx1; ++x) f->tileDirty[y * f->tilesX + x] = 1;
    }
}

/* ---- a frame ---- */

/* What a frame needs of an instance; false when it is out of view. */
static bool placeInstance(const ArtfSoftFrame* f, size_t i, ArtfVisible* v) {
    const ArtfScene* scene = f->scene;
    const float* vp = f->data.viewProj;
    const float* eye = f->data.cameraPos;
    const ArtfInstanceData* inst = &scene->instances.items[i];
    const ArtfMeshData* mesh = &scene->meshes.items[inst->mesh];
    const float* m = inst->m;
    const float* c = mesh->center;
    const float wx = m[0] * c[0] + m[1] * c[1] + m[2] * c[2] + m[3];
    const float wy = m[4] * c[0] + m[5] * c[1] + m[6] * c[2] + m[7];
    const float wz = m[8] * c[0] + m[9] * c[1] + m[10] * c[2] + m[11];
    const float r = mesh->radius * inst->maxScale;
    const uint32_t base = inst->mesh;
    bool inside = true;
    for (int p = 0; p < 5; ++p) {
        const float* pl = f->data.planes[p];
        const float d = pl[0] * wx + pl[1] * wy + pl[2] * wz + pl[3];
        if (d < -r) return false;
        if (d < r) inside = false;
    }

    v->instance = inst;
    const float dx = wx - eye[0], dy = wy - eye[1], dz = wz - eye[2];
    const float distance = artfMaxf(sqrtf(dx * dx + dy * dy + dz * dz) - r, f->nearW);
    v->mesh = &scene->meshes.items[artfMeshLevel(scene, base, f->data.lodScale, inst->maxScale, distance)];
    uint32_t from;
    const uint32_t fade = artfMeshFade(scene, base, f->data.lodScale, inst->maxScale, distance, f->data.look[3], &from);
    v->fade = fade ? fade << 1 | 1 : 0;
    v->fadeFrom = fade ? &scene->meshes.items[from] : NULL;
    v->inside = inside;
    v->index = (uint32_t)i;
    v->center[0] = wx;
    v->center[1] = wy;
    v->center[2] = wz;
    v->radius = r;
    /* clip rows x, y, w of view-projection times the instance */
    static const int rows[3] = {0, 1, 3};
    for (int k = 0; k < 3; ++k) {
        const int row = rows[k];
        const float a = vp[row], b = vp[4 + row], d = vp[8 + row], e = vp[12 + row];
        for (int col = 0; col < 4; ++col) v->mvp[k * 4 + col] = a * m[col] + b * m[4 + col] + d * m[8 + col] + (col == 3 ? e : 0.f);
    }
    const ArtfVec3 c0 = artfVec3(m[0], m[4], m[8]), c1 = artfVec3(m[1], m[5], m[9]), c2 = artfVec3(m[2], m[6], m[10]);
    const ArtfVec3 k[3] = {artfVec3Cross(c1, c2), artfVec3Cross(c2, c0), artfVec3Cross(c0, c1)};
    memcpy(v->normal, k, sizeof v->normal);
    const uint32_t material = scene->materials.items[inst->material].flags;
    /* (what blends with a texture leaves out what is nearly not there, so that its depth does not hide what is behind) */
    v->masked = material & ARTF_MATERIAL_MASK || fade || (material & ARTF_MATERIAL_BLEND && scene->materials.items[inst->material].texture);
    v->twoSided = material & ARTF_MATERIAL_DOUBLE;
    v->useCone = inst->flags & ARTF_INSTANCE_UNIFORM && !v->twoSided && scene->features[ARTF_FEATURE_CONES];
    if (v->useCone) {
        const float dx = eye[0] - m[3], dy = eye[1] - m[7], dz = eye[2] - m[11], inv = 1.f / (inst->maxScale * inst->maxScale);
        for (int k = 0; k < 3; ++k) v->camera[k] = (m[k] * dx + m[4 + k] * dy + m[8 + k] * dz) * inv;
    }
    return true;
}

/* Which of a group of instances are in view. Layers come later, in their own order. */
static void cullGroup(void* user, uint32_t group, uint32_t worker) {
    (void)worker;
    Software* s = user;
    const ArtfSoftFrame* f = &s->f;
    const size_t count = f->scene->instances.len;
    const size_t first = count * group / ARTF_CHUNKS, end = count * (group + 1) / ARTF_CHUNKS;
    ArtfVisible* out = s->groups[group];
    uint32_t n = 0;
    for (size_t i = first; i < end; ++i)
        if (!f->scene->instances.items[i].translucent && placeInstance(f, i, &out[n])) ++n;
    s->groupCounts[group] = n;
}

/* Is all of this sphere behind what is in the cells it covers? The sphere is taken as a cube facing
 * the eye, which holds it, and whose corners give the rectangle it can reach on screen. */
static bool sphereHidden(const ArtfSoftFrame* f, const ArtfVisible* v) {
    const float* m = f->data.viewProj;
    const float x = v->center[0], y = v->center[1], z = v->center[2], r = v->radius;
    const float cx = m[0] * x + m[4] * y + m[8] * z + m[12], cy = m[1] * x + m[5] * y + m[9] * z + m[13], cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    const float nearest = cw - r, farthest = cw + r;
    if (nearest <= f->nearW) return false;
    const float px = r * f->data.focalX, py = r * f->data.focalY;
    const float xlo = artfMinf((cx - px) / nearest, (cx - px) / farthest), xhi = artfMaxf((cx + px) / nearest, (cx + px) / farthest);
    const float ylo = artfMinf((cy - py) / nearest, (cy - py) / farthest), yhi = artfMaxf((cy + py) / nearest, (cy + py) / farthest);
    const int x0 = artfMaxi((int)floorf(artfClampf(xlo * .5f + .5f, 0, 1) * (float)f->width), 0);
    const int x1 = artfMini((int)ceilf(artfClampf(xhi * .5f + .5f, 0, 1) * (float)f->width), f->width - 1);
    const int y0 = artfMaxi((int)floorf(artfClampf(.5f - yhi * .5f, 0, 1) * (float)f->height), 0);
    const int y1 = artfMini((int)ceilf(artfClampf(.5f - ylo * .5f, 0, 1) * (float)f->height), f->height - 1);
    if (x1 < x0 || y1 < y0) return false;
    const float depth = 1.f / nearest;
    const int cx0 = x0 / ARTF_HIZ_CELL, cx1 = x1 / ARTF_HIZ_CELL, cy0 = y0 / ARTF_HIZ_CELL, cy1 = y1 / ARTF_HIZ_CELL;
    if ((cx1 - cx0 + 1) * (cy1 - cy0 + 1) <= 64) {
        for (int cy2 = cy0; cy2 <= cy1; ++cy2)
            for (int cx2 = cx0; cx2 <= cx1; ++cx2)
                if (f->hizCells[(size_t)cy2 * f->cellsX + cx2] <= depth) return false;
        return true;
    }
    for (int ty = y0 / ARTF_TILE; ty <= y1 / ARTF_TILE; ++ty)
        for (int tx = x0 / ARTF_TILE; tx <= x1 / ARTF_TILE; ++tx)
            if (f->hizTiles[ty * f->tilesX + tx] <= depth) return false;
    return true;
}

static void occlusionGroup(void* user, uint32_t group, uint32_t worker) {
    (void)worker;
    Software* s = user;
    const uint32_t count = s->earlyCount + s->candidateCount;
    const uint32_t first = (uint32_t)((uint64_t)count * group / ARTF_CHUNKS), end = (uint32_t)((uint64_t)count * (group + 1) / ARTF_CHUNKS);
    for (uint32_t i = first; i < end; ++i) {
        const ArtfVisible* v = i < s->earlyCount ? &s->f.visible[i] : &s->candidates[i - s->earlyCount];
        const bool hidden = sphereHidden(&s->f, v);
        s->hidden[i] = hidden;
        s->wasVisible[v->index] = !hidden;
    }
}

static void depthJob(void* user, uint32_t tile, uint32_t worker) {
    Software* s = user;
    s->kernels->depth(&s->f, tile, worker);
}

static void geometryJob(void* user, uint32_t chunk, uint32_t worker) {
    (void)worker;
    Software* s = user;
    s->kernels->geometry(&s->f, chunk);
}

static void tileJob(void* user, uint32_t tile, uint32_t worker) {
    Software* s = user;
    s->tilesSkipped[worker] += s->kernels->tile(&s->f, tile, worker);
}

/* ---- a black hole's light ---- */

static void lensJob(void* user, uint32_t tile, uint32_t worker) {
    (void)worker;
    Software* s = user;
    s->kernels->lens(&s->f, tile);
}

/* A level at a place given in its texels, read between them. */
static inline float bloomTap(const float* level, int width, int height, float sx, float sy) {
    const float baseX = floorf(sx), baseY = floorf(sy), partX = sx - baseX, partY = sy - baseY;
    const int x0 = artfClampi((int)baseX, 0, width - 1), x1 = artfClampi((int)baseX + 1, 0, width - 1);
    const float* r0 = level + (size_t)artfClampi((int)baseY, 0, height - 1) * width;
    const float* r1 = level + (size_t)artfClampi((int)baseY + 1, 0, height - 1) * width;
    const float top = r0[x0] + (r0[x1] - r0[x0]) * partX, bottom = r1[x0] + (r1[x1] - r1[x0]) * partX;
    return top + (bottom - top) * partY;
}

/* Each level of the bloom is a level of a pyramid, and the sum of them is a blur that reaches far at
 * the cost of a few passes. The first takes the mean of four pixels, with what is very bright held
 * down, so that a pixel on the photons' orbit does not paint a square. The others read sixteen texels
 * by way of four taps: means of four alone leave steps, which a tone curve shows up in the dark. */
static void bloomDownRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    Software* s = user;
    ArtfSoftFrame* f = &s->f;
    const uint32_t* to = f->holeData.bloom[s->bloomLevel];
    if (!s->bloomLevel) {
        const int lastX = f->width - 1, lastY = f->height - 1;
        const int y0 = artfMini((int)y * 2, lastY), y1 = artfMini((int)y * 2 + 1, lastY);
        for (int c = 0; c < 3; ++c) {
            float* out = f->bloom[c] + to[0] + (size_t)y * to[1];
            const float* a = f->hdr[c] + (size_t)y0 * f->hdrStride;
            const float* b = f->hdr[c] + (size_t)y1 * f->hdrStride;
            for (uint32_t x = 0; x < to[1]; ++x) {
                const int x0 = artfMini((int)x * 2, lastX), x1 = artfMini((int)x * 2 + 1, lastX);
                #define BRIGHT(v) artfMinf(artfMaxf((v) - ARTF_BLOOM_THRESHOLD, 0.f), ARTF_BLOOM_CEILING)
                out[x] = (BRIGHT(a[x0]) + BRIGHT(a[x1]) + BRIGHT(b[x0]) + BRIGHT(b[x1])) * .25f;
#undef BRIGHT
            }
        }
        return;
    }
    const uint32_t* from = f->holeData.bloom[s->bloomLevel - 1];
    const float cy = (float)y * 2.f + .5f;
    for (int c = 0; c < 3; ++c) {
        float* out = f->bloom[c] + to[0] + (size_t)y * to[1];
        const float* level = f->bloom[c] + from[0];
        for (uint32_t x = 0; x < to[1]; ++x) {
            const float cx = (float)x * 2.f + .5f;
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += bloomTap(level, (int)from[1], (int)from[2], cx + (k & 1 ? 1.f : -1.f), cy + (k & 2 ? 1.f : -1.f));
            out[x] = sum * .25f;
        }
    }
}

/* Adds the level above, spread by four taps. */
static void bloomUpRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    Software* s = user;
    ArtfSoftFrame* f = &s->f;
    const uint32_t* to = f->holeData.bloom[s->bloomLevel];
    const uint32_t* from = f->holeData.bloom[s->bloomLevel + 1];
    const float sy = ((float)y + .5f) * .5f - .5f;
    for (int c = 0; c < 3; ++c) {
        float* out = f->bloom[c] + to[0] + (size_t)y * to[1];
        const float* level = f->bloom[c] + from[0];
        for (uint32_t x = 0; x < to[1]; ++x) {
            const float sx = ((float)x + .5f) * .5f - .5f;
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += bloomTap(level, (int)from[1], (int)from[2], sx + (k & 1 ? .5f : -.5f), sy + (k & 2 ? .5f : -.5f));
            out[x] += sum * (.25f * ARTF_BLOOM_FADE);
        }
    }
}

/* Room for the hole's light at this size. */
static void holeTargets(Software* s, size_t bloomPixels) {
    ArtfSoftFrame* f = &s->f;
    f->hdrStride = (f->width + 7) & ~7;
    const size_t pixels = (size_t)f->hdrStride * f->height;
    if (pixels != s->hdrPixels) {
        for (int c = 0; c < 3; ++c) {
            artfAlignedFree(f->hdr[c]);
            f->hdr[c] = artfAlignedMalloc(pixels * 4, 64);
        }
        s->hdrPixels = pixels;
    }
    if (bloomPixels != s->bloomPixels) {
        for (int c = 0; c < 3; ++c) {
            artfAlignedFree(f->bloom[c]);
            f->bloom[c] = artfAlignedMalloc(bloomPixels * 4 + 64, 64);
        }
        s->bloomPixels = bloomPixels;
    }
}

/* All of the hole's light, up to what tiles need to tone map it. */
static void drawHole(Software* s, const ArtfScene* scene) {
    ArtfSoftFrame* f = &s->f;
    uint32_t bloomPixels = 0;
    artfHoleFrame(scene, f->width, f->height, &f->holeData, &bloomPixels);
    f->holeMap = scene->hole.map;
    f->holeFluid = artfFluidField(scene->hole.fluid);
    holeTargets(s, bloomPixels);
    artfPoolRun(s->pool, lensJob, s, (uint32_t)(f->tilesX * f->tilesY));
    const uint32_t levels = f->holeData.size[3];
    for (uint32_t i = 0; i < levels; ++i) {
        s->bloomLevel = i;
        artfPoolRun(s->pool, bloomDownRow, s, f->holeData.bloom[i][2]);
    }
    for (uint32_t i = levels - 1; i-- > 0;) {
        s->bloomLevel = i;
        artfPoolRun(s->pool, bloomUpRow, s, f->holeData.bloom[i][2]);
    }
}

/* Where each instance's meshlets start, in f->visible from first on. */
static void countMeshlets(ArtfSoftFrame* f, uint32_t first) {
    uint32_t meshlets = f->meshletStarts[first];
    for (uint32_t i = first; i < f->visibleCount; ++i) {
        f->meshletStarts[i] = meshlets;
        meshlets += f->visible[i].mesh->meshletCount;
    }
    f->meshletStarts[f->visibleCount] = meshlets;
}

/* Occlusion in two passes, which is exact: what showed last frame is drawn first, its depth tells
 * what it hides, and only the rest of what is in view is tested against that. So nothing that should
 * show is ever missing, even for a frame; what last frame's picture got wrong costs time, not pixels.
 * The tiles' depth is thrown away here, and worked out again with the rest. */
static void drawWithOcclusion(Software* s, uint32_t count) {
    ArtfSoftFrame* f = &s->f;
    if (count > s->flagCapacity) {
        const uint32_t capacity = count * 2;
        s->wasVisible = artfRealloc(s->wasVisible, capacity);
        memset(s->wasVisible + s->flagCapacity, 0, capacity - s->flagCapacity);
        s->flagCapacity = capacity;
    }
    /* what showed last frame stays; the others wait */
    uint32_t early = 0, late = 0;
    for (uint32_t i = 0; i < f->visibleCount; ++i) {
        if (s->wasVisible[f->visible[i].index]) f->visible[early++] = f->visible[i];
        else s->candidates[late++] = f->visible[i];
    }
    f->visibleCount = early;
    s->earlyCount = early;
    s->candidateCount = late;
    f->meshletStarts[0] = 0;
    countMeshlets(f, 0);
    artfPoolRun(s->pool, geometryJob, s, ARTF_CHUNKS);

    /* (also with nothing drawn yet: an opaque overlay hides what is under it all the same) */
    s->hiddenCount = 0;
    artfPoolRun(s->pool, depthJob, s, (uint32_t)(f->tilesX * f->tilesY));
    artfPoolRun(s->pool, occlusionGroup, s, ARTF_CHUNKS);
    for (uint32_t i = 0; i < late; ++i) {
        if (s->hidden[early + i]) ++s->hiddenCount;
        else f->visible[f->visibleCount++] = s->candidates[i];
    }
    f->firstVisible = early;
    f->geometryAppend = true;
    countMeshlets(f, early);
    artfPoolRun(s->pool, geometryJob, s, ARTF_CHUNKS);
    f->firstVisible = 0;
    f->geometryAppend = false;
}

/* A tile's pixels gather this frame's samples. Each pixel's come from its own numbers, so that neither
 * threads nor tiles change them. */
static void traceJob(void* user, uint32_t tile, uint32_t worker) {
    (void)worker;
    Software* s = user;
    ArtfSoftFrame* f = &s->f;
    const int ox = (int)(tile % (uint32_t)f->tilesX) * ARTF_TILE, oy = (int)(tile / (uint32_t)f->tilesX) * ARTF_TILE;
    const int x1 = artfMini(ox + ARTF_TILE, f->width), y1 = artfMini(oy + ARTF_TILE, f->height);
    for (int y = oy; y < y1; ++y)
        for (int x = ox; x < x1; ++x) {
            const size_t at = ((size_t)y * f->width + x) * 4;
            if (s->denoising) {
                /* (the first sample says what the surface is) */
                ArtfTraceGuide g;
                memset(&g, 0, sizeof g);
                float* light = s->denoiser.noisy + at;
                memset(light, 0, 16);
                for (uint32_t i = 0; i < s->traceAdding; ++i) artfTraceSample(f->scene, &f->data, &s->traceFrame, (uint32_t)x, (uint32_t)y, f->traceDone + i, light, i ? NULL : &g);
                memcpy(s->denoiser.guide + at, g.place, 16);
                memcpy(s->denoiser.albedo + at, g.albedo, 16);
                memcpy(s->denoiser.shift + at, g.shift, 16);
                continue;
            }
            for (uint32_t i = 0; i < s->traceAdding; ++i)
                artfTraceSample(f->scene, &f->data, &s->traceFrame, (uint32_t)x, (uint32_t)y, f->traceDone + i, f->traceSum + at, NULL);
        }
}

/* All that is left to do of a frame with rays: what they gather, and no triangles. */
static void drawTraced(Software* s, ArtfScene* scene) {
    ArtfSoftFrame* f = &s->f;
    artfSceneTraceSync(scene);
    const uint64_t key = artfSceneTraceKey(scene, &f->data);
    const size_t bytes = (size_t)f->width * f->height * 16;
    if (!f->traceSum) f->traceSum = artfMalloc(bytes);
    const ArtfTraceDesc* d = &scene->trace.desc;
    s->denoising = d->denoise;
    if (d->denoise) {
        /* What changed does not start over here: the filter finds what still holds. */
        artfDenoiserResize(&s->denoiser, f->width, f->height);
        if (key != s->traceKey) {
            s->traceKey = key;
            s->traceCounted = 0;
        }
        if (d->maxSamples > 0 && s->traceCounted >= (uint32_t)d->maxSamples) return;
        s->traceAdding = (uint32_t)d->samples;
        artfSceneTraceFrame(scene, &f->data, f->width, f->height, s->traceFrames, &s->traceFrame);
        f->traceDone = s->traceFrames; /* (each frame's samples must be others) */
        artfPoolRun(s->pool, traceJob, s, (uint32_t)(f->tilesX * f->tilesY));
        artfDenoiserRun(&s->denoiser, s->pool, &f->data, s->traceAdding, scene->trace.motion.items, scene->trace.motion.len / 12);
        memcpy(f->traceSum, s->denoiser.noisy, bytes);
        s->traceFrames += s->traceAdding;
        s->traceCounted += s->traceAdding;
        f->traceDone = 1; /* (what the sum holds is one clean frame) */
        return;
    }
    if (key != s->traceKey || !f->traceDone) {
        memset(f->traceSum, 0, bytes);
        f->traceDone = 0;
        s->traceKey = key;
    }
    s->traceAdding = (uint32_t)d->samples;
    if (d->maxSamples > 0) s->traceAdding = f->traceDone >= (uint32_t)d->maxSamples ? 0 : (uint32_t)artfMini(d->samples, d->maxSamples - (int)f->traceDone);
    if (s->traceAdding) {
        artfSceneTraceFrame(scene, &f->data, f->width, f->height, f->traceDone, &s->traceFrame);
        artfPoolRun(s->pool, traceJob, s, (uint32_t)(f->tilesX * f->tilesY));
        f->traceDone += s->traceAdding;
    }
}

/* What shows what is behind it goes after all that is solid, the farthest first, in a pass of its own:
 * so that, reading the chunks in order, a tile meets the layers in order. */
static void addLayers(Software* s, const ArtfScene* scene) {
    ArtfSoftFrame* f = &s->f;
    f->solidCount = f->visibleCount;
    if (!scene->translucentCount) return;
    artfVecReserve(&s->layers, scene->translucentCount);
    const uint32_t count = artfSceneLayers(scene, &f->data, s->layers.items);
    for (uint32_t i = 0; i < count; ++i)
        if (placeInstance(f, s->layers.items[i].instance, &f->visible[f->visibleCount])) ++f->visibleCount;
    if (f->visibleCount == f->solidCount) return;
    f->firstVisible = f->solidCount;
    f->geometryAppend = true;
    countMeshlets(f, f->solidCount);
    artfPoolRun(s->pool, geometryJob, s, ARTF_CHUNKS);
    f->firstVisible = 0;
    f->geometryAppend = false;
}

static bool softDraw(ArtfRenderer* r, ArtfScene* scene) {
    Software* s = (Software*)r;
    ArtfSoftFrame* f = &s->f;
    const double t0 = artfNowMs();
    f->scene = scene;
    artfSceneDecodeTextures(scene);
    artfScenePosesApply(scene);
    artfSceneFaceParticles(scene);
    artfSceneFrame(scene, f->width, f->height, &f->data);
    if (f->post) {
        /* the frame before, and with temporal antialiasing, a place within the pixel */
        float now[16];
        memcpy(now, f->data.viewProj, sizeof now);
        if (s->hasPrevious) memcpy(f->data.previous, s->previous, sizeof s->previous);
        memcpy(s->previous, now, sizeof now);
        s->hasPrevious = true;
        if (scene->post.temporal) artfFrameJitter(&f->data, s->postFrames++, f->width, f->height);
    } else {
        memset(f->data.post, 0, sizeof f->data.post);
    }
    artfSceneResidencyStep(scene, &f->data, f->height);
    if (f->data.indirect[0][3] > 0 && scene->trace.desc.mode == ARTF_TRACE_NONE) artfSceneIndirectStep(scene, &f->data, s->pool);
    f->extraCount = 0;
    if (f->data.eye[2][3] > 0) {
        if (!f->extra) f->extra = artfMalloc(ARTF_MAX_LIGHTS * sizeof *f->extra);
        f->extraCount = artfSceneExtraLights(scene, &f->data, f->extra);
        artfFree(f->tileLights);
        f->tileLights = artfMalloc((size_t)f->tilesX * f->tilesY * (1 + ARTF_TILE_LIGHTS) * 4);
        artfSceneLightTiles(&f->data, f->width, f->height, f->extra, f->extraCount, f->tileLights);
    }
    f->nearW = scene->camera.near_;
    if (!f->decals) f->decals = artfMalloc(ARTF_MAX_DECALS * sizeof *f->decals);
    f->decalCount = artfSceneDecals(scene, &f->data, f->height, f->decals);
    const bool occlusion = scene->features[ARTF_FEATURE_OCCLUSION];
    const float* bg = f->data.background;
    f->background = (uint32_t)(artfClampf(bg[3], 0, 1) * 255.f + .5f) << 24 | (uint32_t)(artfClampf(bg[0], 0, 1) * 255.f + .5f) << 16 | (uint32_t)(artfClampf(bg[1], 0, 1) * 255.f + .5f) << 8 | (uint32_t)(artfClampf(bg[2], 0, 1) * 255.f + .5f);
    f->solidCount = UINT32_MAX;

    /* room for every instance, by groups, then packed into one list */
    const uint32_t count = (uint32_t)scene->instances.len;
    if (count * 2 + ARTF_CHUNKS > s->visibleCapacity) {
        s->visibleCapacity = (count + ARTF_CHUNKS) * 2; /* (what fades goes in twice) */
        artfFree(f->visible);
        artfFree(f->meshletStarts);
        artfFree(s->candidates);
        artfFree(s->hidden);
        f->visible = artfMalloc((size_t)s->visibleCapacity * sizeof *f->visible);
        f->meshletStarts = artfMalloc(((size_t)s->visibleCapacity + 1) * 4);
        s->candidates = artfMalloc((size_t)s->visibleCapacity * sizeof *s->candidates);
        s->hidden = artfMalloc(s->visibleCapacity);
    }
    /* each group writes from where its instances start: the list has holes until it is packed */
    f->tracing = scene->trace.desc.mode != ARTF_TRACE_NONE;
    f->postFog = f->post && !f->tracing && f->data.post[1][1] > 0 && f->data.look[1] > 0;
    for (uint32_t g = 0; g < ARTF_CHUNKS; ++g) {
        s->groups[g] = f->visible + (size_t)count * g / ARTF_CHUNKS;
        s->groupCounts[g] = 0;
    }
    if (!f->tracing) artfPoolRun(s->pool, cullGroup, s, ARTF_CHUNKS);
    uint32_t visible = 0;
    for (uint32_t g = 0; g < ARTF_CHUNKS; ++g) {
        if (f->visible + visible != s->groups[g]) memmove(f->visible + visible, s->groups[g], (size_t)s->groupCounts[g] * sizeof *f->visible);
        visible += s->groupCounts[g];
    }
    /* what is between two levels: the other one, in the other pixels */
    for (uint32_t i = 0, solid = visible; i < solid && f->data.look[3] > 0; ++i) {
        const ArtfVisible* v = &f->visible[i];
        if (!v->fade) continue;
        ArtfVisible* other = &f->visible[visible++];
        *other = *v;
        other->mesh = v->fadeFrom;
        other->fade = v->fade & ~1u;
    }
    f->visibleCount = visible;
    const double t1 = artfNowMs();

    s->hiddenCount = 0;
    if (f->tracing) {
        f->meshletStarts[0] = 0;
        countMeshlets(f, 0);
        artfPoolRun(s->pool, geometryJob, s, ARTF_CHUNKS);
        f->solidCount = 0;
        drawTraced(s, scene);
    } else if (occlusion) {
        drawWithOcclusion(s, count);
    } else {
        f->meshletStarts[0] = 0;
        countMeshlets(f, 0);
        artfPoolRun(s->pool, geometryJob, s, ARTF_CHUNKS);
    }
    if (!f->tracing) addLayers(s, scene);
    f->shadows = !f->tracing && scene->shadows.resolution ? artfShadowMapsDraw(s->shadowMaps, scene, &f->data) : NULL;
    if (f->postFog) artfSoftFroxels(&s->post, f);
    const double t2 = artfNowMs();

    f->hole = scene->hole.on;
    if (f->hole) drawHole(s, scene);
    const double tHole = artfNowMs();

    memset(s->tilesSkipped, 0, sizeof s->tilesSkipped);
    artfPoolRun(s->pool, tileJob, s, (uint32_t)(f->tilesX * f->tilesY));
    if (f->post) artfSoftPostRun(&s->post, f, s->pixels);
    const double t3 = artfNowMs();

    s->cullMs = t1 - t0;
    s->geometryMs = t2 - t1;
    s->lensMs = tHole - t2;
    s->rasterMs = t3 - tHole;
    s->cpuMs = t3 - t0;
    return true;
}

static bool softReadPixels(ArtfRenderer* r, uint32_t* dst) {
    Software* s = (Software*)r;
    memcpy(dst, s->pixels, (size_t)s->f.width * s->f.height * 4);
    return true;
}

static uint32_t* softPixels(ArtfRenderer* r) { return ((Software*)r)->pixels; }

static const char* softDescription(const ArtfRenderer* r) { return ((const Software*)r)->description; }

static void softStats(const ArtfRenderer* r, ArtfFrameStats* out) {
    const Software* s = (const Software*)r;
    out->cpuMs = s->cpuMs;
    out->cullMs = s->cullMs;
    out->geometryMs = s->geometryMs;
    out->rasterMs = s->rasterMs;
    out->lensMs = s->lensMs;
    out->shadowMapsDrawn = artfShadowMapsDrawn(s->shadowMaps);
    out->instancesDrawn = s->f.visibleCount;
    out->instancesHidden = s->hiddenCount;
    out->traceSamples = s->f.tracing ? (s->denoising ? s->traceCounted : s->f.traceDone) : 0;
    for (int i = 0; i < ARTF_CHUNKS; ++i) out->trianglesBinned += (uint32_t)s->f.chunks[i].tris.len;
    for (int i = 0; i < 16; ++i) out->tilesSkipped += s->tilesSkipped[i];
}

static void softFree(ArtfRenderer* r) {
    Software* s = (Software*)r;
    ArtfSoftFrame* f = &s->f;
    const int workers = artfPoolSize(s->pool);
    artfShadowMapsFree(s->shadowMaps);
    artfDenoiserFree(&s->denoiser);
    artfPoolFree(s->pool);
    for (int i = 0; i < ARTF_CHUNKS; ++i) {
        artfVecFree(&f->chunks[i].tris);
        artfVecFree(&f->chunks[i].blocks);
        artfFree(f->chunks[i].heads);
    }
    for (int i = 0; i < workers; ++i) {
        artfAlignedFree(f->workers[i].depth);
        artfAlignedFree(f->workers[i].ids);
        artfAlignedFree(f->workers[i].color);
        artfAlignedFree(f->workers[i].layer);
        artfAlignedFree(f->workers[i].postDepth);
        artfFree(f->workers[i].solidDepth);
        artfFree(f->workers[i].fragmentDepth);
        artfFree(f->workers[i].fragmentColor);
        artfFree(f->workers[i].fragmentCount);
        artfVecFree(&f->workers[i].tris);
        artfVecFree(&f->workers[i].layers);
        artfVecFree(&f->workers[i].seen);
    }
    artfFree(f->chunks);
    artfFree(f->workers);
    artfFree(f->visible);
    artfFree(f->decals);
    artfFree(f->meshletStarts);
    artfFree(f->tileClass);
    artfFree(f->tileDirty);
    artfFree(f->hizCells);
    artfFree(f->hizTiles);
    artfFree(s->candidates);
    artfFree(s->hidden);
    artfFree(s->wasVisible);
    artfVecFree(&s->layers);
    artfFree(f->traceSum);
    for (int c = 0; c < 3; ++c) {
        artfAlignedFree(f->hdr[c]);
        artfAlignedFree(f->bloom[c]);
    }
    for (int c = 0; c < 4; ++c) artfFree(f->postLight[c]);
    artfFree(f->postDepth);
    artfFree(f->postSurface);
    artfSoftPostFree(&s->post);
    artfFree(f->extra);
    artfFree(f->tileLights);
    artfAlignedFree(s->pixels);
    artfFree(s);
}

static const ArtfRendererVtbl softTable = {softDescription, softResize, softSetOverlay, softDraw, softReadPixels, softPixels, softStats, softFree};

ArtfRenderer* artfSoftwareNew(const ArtfRendererOptions* o) {
    Software* s = ARTF_NEW(Software);
    s->base.vt = &softTable;
    s->base.backend = ARTF_BACKEND_SOFTWARE;
    s->pool = artfPoolNew(o->threads);
    s->kernels = chooseKernels(o->simd);
    s->shadowMaps = artfShadowMapsNew(s->pool);
    const int workers = artfPoolSize(s->pool);
    snprintf(s->description, sizeof s->description, "software, %s, %d thread%s", s->kernels->name, workers, workers == 1 ? "" : "s");
    ArtfSoftFrame* f = &s->f;
    f->chunks = artfCalloc(ARTF_CHUNKS, sizeof *f->chunks);
    f->workers = artfCalloc((size_t)workers, sizeof *f->workers);
    f->samples = o->antialias ? 4 : 1;
    f->post = o->hdr;
    s->post.pool = s->pool;
    const size_t planes = (size_t)ARTF_TILE_PIXELS * 4 * f->samples;
    /* (with a frame in floats, each color has its alpha and surface after it: see storeLit) */
    const size_t colors = f->post ? (size_t)ARTF_TILE_PIXELS * 4 * 12 : planes;
    for (int i = 0; i < workers; ++i) {
        f->workers[i].depth = artfAlignedMalloc(planes, 64);
        f->workers[i].ids = artfAlignedMalloc(planes, 64);
        f->workers[i].color = artfAlignedMalloc(colors, 64);
        f->workers[i].layer = artfAlignedMalloc(colors, 64);
        if (f->post) f->workers[i].postDepth = artfAlignedMalloc(ARTF_TILE_PIXELS * 4, 64);
    }
    allocateTargets(s, artfMaxi(o->width, 1), artfMaxi(o->height, 1));
    return &s->base;
}
