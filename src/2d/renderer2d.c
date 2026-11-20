/* artful-2d — a frame: the scene brought into the frame's pixels (sprites, their outlines, lights), put into tiles;
 * the lights' shadow maps, one a thread; gi; the tiles, one a thread; then what comes after. */
#include "core/threads.h"
#include "2d/internal2d.h"
#include "core/trig.h"
#ifdef ARTFUL_VULKAN
#include "2d/vulkan2d.h"
#else
typedef struct Gpu2D Gpu2D;
#endif

#include <stdio.h>
#include <stdlib.h>

#define BIN_SIZES 13 /* shadow maps of 2^6 to 2^12 bins */

struct ArtfRenderer2D {
    int width, height;
    ArtfPool* pool;
    uint32_t* pixels;
    float* hdr;
    Gi2D* gi;
    Frame2D frame;
    ShadowMap2D* maps;
    size_t mapCount;
    float* binCos[BIN_SIZES];
    float* binSin[BIN_SIZES];
    TileScratch2D* scratch;
    int scratchEdges;
    ArtfFrameStats2D stats;
    Gpu2D* gpu; /* NULL: the CPU draws */
    bool pixelsFresh; /* the GPU's last frame is in pixels */
    bool everDrawn;
    char description[300];
};

ArtfRenderer2D* artfRenderer2DNew(const ArtfRenderer2DOptions* options) {
    artf2DTables();
    Gpu2D* gpu = NULL;
    if (options->backend != ARTF_BACKEND2D_SOFTWARE) {
#ifdef ARTFUL_VULKAN
        gpu = artf2DVulkanNew(options->surface.kind != ARTF_SURFACE_NONE ? &options->surface : NULL, options->noVsync, options->width, options->height);
#endif
        if (!gpu && options->backend == ARTF_BACKEND2D_VULKAN) return NULL;
    }
    ArtfRenderer2D* r = ARTF_NEW(ArtfRenderer2D);
    r->gpu = gpu;
    r->pool = artfPoolNew(options->threads);
    r->gi = artf2DGiNew();
    const int workers = artfPoolSize(r->pool);
    r->scratch = artfCalloc((size_t)workers, sizeof(TileScratch2D));
    for (int i = 0; i < workers; ++i) {
        r->scratch[i].color = artfMalloc(sizeof(float) * TILE2D * TILE2D * 4);
        r->scratch[i].cover = artfMalloc(sizeof(float) * TILE2D);
    }
    artfRenderer2DResize(r, options->width, options->height);
#ifdef ARTFUL_VULKAN
    if (gpu) snprintf(r->description, sizeof r->description, "%s", artf2DVulkanName(gpu));
    else
#endif
        snprintf(r->description, sizeof r->description, "software, %d thread%s", workers, workers == 1 ? "" : "s");
    return r;
}

ArtfBackend2D artfRenderer2DBackend(const ArtfRenderer2D* r) { return r->gpu ? ARTF_BACKEND2D_VULKAN : ARTF_BACKEND2D_SOFTWARE; }
const char* artfRenderer2DDescription(const ArtfRenderer2D* r) { return r->description; }

static void freeMap(ShadowMap2D* m) {
    artfFree(m->near_);
    artfFree(m->owner);
    artfFree(m->past);
    memset(m, 0, sizeof *m);
}

static void freeTiles(Frame2D* f) {
    const int tiles = f->tilesX * f->tilesY;
    for (int i = 0; i < tiles && f->tileItems; ++i) {
        artfFree(f->tileItems[i]);
        artfFree(f->tileLamps[i]);
    }
    artfFree(f->tileItems);
    artfFree(f->tileItemCount);
    artfFree(f->tileLamps);
    artfFree(f->tileLampCount);
    f->tileItems = NULL;
    f->tileLamps = NULL;
    f->tileItemCount = f->tileLampCount = NULL;
}

void artfRenderer2DFree(ArtfRenderer2D* r) {
    if (!r) return;
#ifdef ARTFUL_VULKAN
    artf2DVulkanFree(r->gpu);
#endif
    const int workers = artfPoolSize(r->pool);
    for (int i = 0; i < workers; ++i) {
        artfFree(r->scratch[i].color);
        artfFree(r->scratch[i].cover);
        artfFree(r->scratch[i].crossX);
        artfFree(r->scratch[i].crossDir);
    }
    artfFree(r->scratch);
    for (size_t i = 0; i < r->mapCount; ++i) freeMap(&r->maps[i]);
    artfFree(r->maps);
    for (int i = 0; i < BIN_SIZES; ++i) {
        artfFree(r->binCos[i]);
        artfFree(r->binSin[i]);
    }
    Frame2D* f = &r->frame;
    freeTiles(f);
    artfVecFree(&f->items);
    artfVecFree(&f->edges);
    artfVecFree(&f->outlinePoints);
    artfVecFree(&f->outlines);
    artfVecFree(&f->lamps);
    artf2DGiFree(r->gi);
    artfFree(r->pixels);
    artfFree(r->hdr);
    artfPoolFree(r->pool);
    artfFree(r);
}

void artfRenderer2DResize(ArtfRenderer2D* r, int width, int height) {
    width = artfMaxi(width, 1);
    height = artfMaxi(height, 1);
    if (width == r->width && height == r->height && r->pixels) return;
    freeTiles(&r->frame);
    r->width = width;
    r->height = height;
    artfFree(r->pixels);
    artfFree(r->hdr);
    r->hdr = NULL;
    r->pixels = artfCalloc((size_t)width * height, 4);
    r->everDrawn = r->pixelsFresh = false;
#ifdef ARTFUL_VULKAN
    if (r->gpu) artf2DVulkanResize(r->gpu, width, height);
#endif
    Frame2D* f = &r->frame;
    f->tilesX = (width + TILE2D - 1) / TILE2D;
    f->tilesY = (height + TILE2D - 1) / TILE2D;
    const int tiles = f->tilesX * f->tilesY;
    f->tileItems = artfCalloc((size_t)tiles, sizeof(uint32_t*));
    f->tileItemCount = artfCalloc((size_t)tiles, sizeof(uint32_t));
    f->tileLamps = artfCalloc((size_t)tiles, sizeof(uint16_t*));
    f->tileLampCount = artfCalloc((size_t)tiles, sizeof(uint32_t));
}

const uint32_t* artfRenderer2DPixels(ArtfRenderer2D* r) {
#ifdef ARTFUL_VULKAN
    if (r->gpu) {
        if (artf2DVulkanShows(r->gpu)) return NULL;
        if (!r->pixelsFresh && r->everDrawn) r->pixelsFresh = artf2DVulkanPixels(r->gpu, r->pixels, r->width, r->height);
    }
#endif
    return r->pixels;
}

bool artfRenderer2DReadPixels(ArtfRenderer2D* r, uint32_t* dst) {
    if (!r->everDrawn) return false;
#ifdef ARTFUL_VULKAN
    if (r->gpu) {
        if (r->pixelsFresh) {
            memcpy(dst, r->pixels, (size_t)r->width * r->height * 4);
            return true;
        }
        return artf2DVulkanPixels(r->gpu, dst, r->width, r->height);
    }
#endif
    memcpy(dst, r->pixels, (size_t)r->width * r->height * 4);
    return true;
}

void artfRenderer2DStats(const ArtfRenderer2D* r, ArtfFrameStats2D* out) { *out = r->stats; }

/* ---- the scene, in the frame's pixels ---- */

static int compareItems(const void* a, const void* b) {
    const Item2D *x = a, *y = b;
    if (x->sp->desc.layer != y->sp->desc.layer) return x->sp->desc.layer < y->sp->desc.layer ? -1 : 1;
    return x->sp->order < y->sp->order ? -1 : x->sp->order > y->sp->order ? 1 : 0;
}

/* floor(log2(x)) for x > 0, from its bits (frexp gives the same everywhere) */
static int log2Floor(float x) {
    int e;
    frexpf(x, &e);
    return e - 1;
}

static int mipFor(const Texture2D* t, const Item2D* it) {
    if (!t) return 0;
    const float perPixelU = (it->uv[1] < 0 ? -it->uv[1] : it->uv[1]) * (float)t->width[0] / it->zoom;
    const float perPixelV = (it->uv[3] < 0 ? -it->uv[3] : it->uv[3]) * (float)t->height[0] / it->zoom;
    const float most = artfMaxf(perPixelU, perPixelV);
    return most > 1 ? artfMini(log2Floor(most), t->levels - 1) : 0;
}

/* A sprite's box in its own space. */
static void localBox(const Sprite2D* sp, float* x0, float* y0, float* x1, float* y1) {
    const ArtfSprite2DDesc* d = &sp->desc;
    if (d->shape == ARTF_SHAPE_RECT || d->shape == ARTF_SHAPE_ELLIPSE) {
        float hw, hh;
        artf2DSpriteHalf(sp, &hw, &hh);
        *x0 = -hw, *y0 = -hh, *x1 = hw, *y1 = hh;
        return;
    }
    *x0 = *y0 = 1e30f;
    *x1 = *y1 = -1e30f;
    for (int i = 0; i < d->pointCount; ++i) {
        *x0 = artfMinf(*x0, sp->points[i].x);
        *y0 = artfMinf(*y0, sp->points[i].y);
        *x1 = artfMaxf(*x1, sp->points[i].x);
        *y1 = artfMaxf(*y1, sp->points[i].y);
    }
    if (d->shape == ARTF_SHAPE_LINE) {
        const float t = d->thickness * .5f;
        *x0 -= t, *y0 -= t, *x1 += t, *y1 += t;
    }
    if (*x0 > *x1) *x0 = *y0 = *x1 = *y1 = 0;
}

/* Its outline, in its own space, for shadows and gi. */
static int outlineOf(const Sprite2D* sp, ArtfVec2* out, int cap) {
    const ArtfSprite2DDesc* d = &sp->desc;
    float hw, hh;
    artf2DSpriteHalf(sp, &hw, &hh);
    switch (d->shape) {
    case ARTF_SHAPE_RECT:
        out[0] = artfVec2(-hw, -hh), out[1] = artfVec2(hw, -hh), out[2] = artfVec2(hw, hh), out[3] = artfVec2(-hw, hh);
        return 4;
    case ARTF_SHAPE_ELLIPSE:
        for (int i = 0; i < 32; ++i) {
            const float a = (float)i * (2.f * PI2D / 32.f);
            out[i] = artfVec2(hw * artfCos(a), hh * artfSin(a));
        }
        return 32;
    case ARTF_SHAPE_LINE: {
        if (d->pointCount < 2) return 0;
        const ArtfVec2 a = sp->points[0], b = sp->points[1];
        const float ex = b.x - a.x, ey = b.y - a.y, len = sqrtf(ex * ex + ey * ey);
        if (len <= 0) return 0;
        const float t = d->thickness * .5f, nx = -ey / len * t, ny = ex / len * t, ax = -ex / len * t, ay = -ey / len * t;
        out[0] = artfVec2(a.x + nx + ax, a.y + ny + ay), out[1] = artfVec2(b.x + nx - ax, b.y + ny - ay);
        out[2] = artfVec2(b.x - nx - ax, b.y - ny - ay), out[3] = artfVec2(a.x - nx + ax, a.y - ny + ay);
        return 4;
    }
    case ARTF_SHAPE_POLYGON: {
        const int n = artfMini(d->pointCount, cap);
        memcpy(out, sp->points, sizeof(ArtfVec2) * n);
        return n;
    }
    }
    return 0;
}

static void addOutline(Frame2D* f, const ArtfVec2* points, int count, bool closed, uint32_t owner, const Affine2D* m) {
    if (count < 2) return;
    Outline2D o = {.start = (int)f->outlinePoints.len, .count = count, .closed = closed && count > 2, .owner = owner,
                   .x0 = 1e30f, .y0 = 1e30f, .x1 = -1e30f, .y1 = -1e30f};
    for (int i = 0; i < count; ++i) {
        const ArtfVec2 p = artf2DApply(m, points[i].x, points[i].y);
        artfVecPush(&f->outlinePoints, p);
        o.x0 = artfMinf(o.x0, p.x), o.y0 = artfMinf(o.y0, p.y);
        o.x1 = artfMaxf(o.x1, p.x), o.y1 = artfMaxf(o.y1, p.y);
    }
    artfVecPush(&f->outlines, o);
}

static void prepare(ArtfRenderer2D* r, const ArtfScene2D* s) {
    Frame2D* f = &r->frame;
    f->scene = s;
    f->width = r->width;
    f->height = r->height;
    f->items.len = f->edges.len = f->outlinePoints.len = f->outlines.len = f->lamps.len = 0;
    const Affine2D camera = artf2DCameraMap(&s->camera, r->width, r->height);
    const float zoom = s->camera.zoom > 0 ? s->camera.zoom : 1;

    /* sprites, and the outlines of those that cast shadows */
    uint32_t owners = 0;
    ARTF_VEC(ArtfVec2) local = {0};
    for (size_t i = 0; i < s->sprites.len; ++i) {
        const Sprite2D* sp = &s->sprites.items[i];
        const ArtfSprite2DDesc* d = &sp->desc;
        if (!sp->slot.alive || d->hidden) continue;
        const Affine2D toFrame = artf2DMul(camera, artf2DSpriteMap(sp));
        uint32_t owner = 0;
        if (d->castsShadow) {
            const int cap = artfMaxi(d->pointCount, 32);
            artfVecReserve(&local, (size_t)cap);
            const int n = outlineOf(sp, local.items, cap);
            if (n >= 2) {
                owner = ++owners;
                addOutline(f, local.items, n, true, owner, &toFrame);
            }
        }
        float bx0, by0, bx1, by1;
        localBox(sp, &bx0, &by0, &bx1, &by1);
        const ArtfVec2 corner[4] = {artf2DApply(&toFrame, bx0, by0), artf2DApply(&toFrame, bx1, by0), artf2DApply(&toFrame, bx0, by1),
                                    artf2DApply(&toFrame, bx1, by1)};
        float fx0 = corner[0].x, fy0 = corner[0].y, fx1 = fx0, fy1 = fy0;
        for (int k = 1; k < 4; ++k) {
            fx0 = artfMinf(fx0, corner[k].x), fy0 = artfMinf(fy0, corner[k].y);
            fx1 = artfMaxf(fx1, corner[k].x), fy1 = artfMaxf(fy1, corner[k].y);
        }
        Item2D it = {.sp = sp, .owner = owner};
        it.x0 = artfMaxi((int)floorf(fx0) - 1, 0), it.y0 = artfMaxi((int)floorf(fy0) - 1, 0);
        it.x1 = (int)artfMinf(ceilf(fx1) + 1, (float)r->width), it.y1 = (int)artfMinf(ceilf(fy1) + 1, (float)r->height);
        if (it.x0 >= it.x1 || it.y0 >= it.y1) continue;
        it.handle = (uint32_t)sp->slot.generation << 24 | (uint32_t)(i + 1);
        it.toLocal = artf2DInverse(toFrame);
        const float det = toFrame.m[0] * toFrame.m[4] - toFrame.m[1] * toFrame.m[3];
        it.zoom = sqrtf(det < 0 ? -det : det);
        if (!(it.zoom > 0)) continue;
        const float turn = d->rotation - s->camera.rotation;
        it.turnCos = artfCos(turn);
        it.turnSin = artfSin(turn);
        artf2DSpriteHalf(sp, &it.hw, &it.hh);
        /* the texture over the shape's box */
        float u0 = d->uvMin.x, v0 = d->uvMin.y, u1 = d->uvMax.x, v1 = d->uvMax.y;
        if (u0 == 0 && v0 == 0 && u1 == 0 && v1 == 0) u1 = v1 = 1;
        float lx0 = bx0, ly0 = by0, lx1 = bx1, ly1 = by1;
        if (d->shape == ARTF_SHAPE_LINE) {
            const float t = d->thickness * .5f;
            lx0 += t, ly0 += t, lx1 -= t, ly1 -= t;
            if (lx1 <= lx0) lx0 -= t, lx1 += t;
            if (ly1 <= ly0) ly0 -= t, ly1 += t;
        }
        it.uv[1] = lx1 > lx0 ? (u1 - u0) / (lx1 - lx0) : 0;
        it.uv[0] = u0 - lx0 * it.uv[1];
        it.uv[3] = ly1 > ly0 ? (v1 - v0) / (ly1 - ly0) : 0;
        it.uv[2] = v0 - ly0 * it.uv[3];
        const ArtfTexture2D handles[3] = {d->texture, d->normalMap, d->emissiveMap};
        for (int k = 0; k < 3; ++k) {
            it.tex[k] = handles[k] ? artf2DTexture(s, handles[k]) : NULL;
            it.mip[k] = mipFor(it.tex[k], &it);
        }
        it.emits = d->emissive.x > 0 || d->emissive.y > 0 || d->emissive.z > 0;
        it.lit = !d->unlit;
        it.polygon = d->shape == ARTF_SHAPE_POLYGON;
        if (it.polygon) {
            it.edgeStart = (int)f->edges.len;
            for (int k = 0; k < d->pointCount; ++k) {
                const ArtfVec2 a = artf2DApply(&toFrame, sp->points[k].x, sp->points[k].y);
                const ArtfVec2 b = artf2DApply(&toFrame, sp->points[(k + 1) % d->pointCount].x, sp->points[(k + 1) % d->pointCount].y);
                if (a.y == b.y) continue;
                Edge2D e = {a.x, a.y, b.x, b.y};
                artfVecPush(&f->edges, e);
            }
            it.edgeCount = (int)f->edges.len - it.edgeStart;
            if (!it.edgeCount) continue;
        }
        artfVecPush(&f->items, it);
    }
    artfVecFree(&local);
    if (f->items.len > 1) qsort(f->items.items, f->items.len, sizeof(Item2D), compareItems);

    const Affine2D none = camera;
    for (size_t i = 0; i < s->occluders.len; ++i) {
        const Occluder2D* o = &s->occluders.items[i];
        if (o->slot.alive) addOutline(f, o->points, o->count, o->closed, ++owners, &none);
    }

    /* lights */
    for (size_t i = 0; i < s->lights.len; ++i) {
        const Light2D* light = &s->lights.items[i];
        if (!light->slot.alive) continue;
        const ArtfLight2DDesc* d = &light->desc;
        Lamp2D l = {.kind = d->kind, .mask = d->mask};
        const float intensity = d->intensity;
        l.color[0] = d->color.x * intensity, l.color[1] = d->color.y * intensity, l.color[2] = d->color.z * intensity;
        if (l.color[0] <= 0 && l.color[1] <= 0 && l.color[2] <= 0) continue;
        const float angle = d->direction - s->camera.rotation;
        if (d->kind == ARTF_LIGHT2D_SUN) {
            const float zz = d->height > 0 ? d->height : 1;
            const float gx = artfCos(angle), gy = artfSin(angle), len = sqrtf(1.f + zz * zz);
            l.dirX = -gx / len, l.dirY = -gy / len, l.dirZ = zz / len;
            l.radius = artfMaxf(d->radius, 0);
            l.x0 = l.y0 = 0, l.x1 = r->width, l.y1 = r->height;
        } else {
            const ArtfVec2 p = artf2DApply(&camera, d->position.x, d->position.y);
            l.x = p.x, l.y = p.y;
            l.range = artfMaxf(d->range, 0) * zoom;
            if (l.range <= 0) continue;
            l.range2 = l.range * l.range;
            l.inverseRange2 = 1.f / l.range2;
            l.radius = artfMaxf(d->radius, 0) * zoom;
            l.height = (d->height > 0 ? d->height : d->range * .25f) * zoom;
            l.dirX = artfCos(angle), l.dirY = artfSin(angle);
            const float inner = artfMinf(d->coneInner, d->coneOuter), outer = artfMaxf(d->coneInner, d->coneOuter);
            l.cosInner = artfCos(inner), l.cosOuter = artfCos(outer);
            if (l.cosInner <= l.cosOuter) l.cosInner = l.cosOuter + 1e-4f;
            l.x0 = (int)artfMaxf(floorf(l.x - l.range), 0), l.y0 = (int)artfMaxf(floorf(l.y - l.range), 0);
            l.x1 = (int)artfMinf(ceilf(l.x + l.range), (float)r->width), l.y1 = (int)artfMinf(ceilf(l.y + l.range), (float)r->height);
            if (l.x0 >= l.x1 || l.y0 >= l.y1) continue;
        }
        l.shadow = d->shadows && f->outlines.len ? (ShadowMap2D*)1 : NULL; /* (given a map below) */
        artfVecPush(&f->lamps, l);
    }
    if (f->lamps.len > 65535) f->lamps.len = 65535;

    /* tiles: what of the sprites and lights each has */
    const int tiles = f->tilesX * f->tilesY;
    for (int t = 0; t < tiles; ++t) {
        f->tileItemCount[t] = 0;
        f->tileLampCount[t] = 0;
    }
    static const int pass = 2;
    for (int round = 0; round < pass; ++round) {
        for (size_t n = 0; n < f->items.len; ++n) {
            const Item2D* it = &f->items.items[n];
            for (int ty = it->y0 / TILE2D; ty <= (it->y1 - 1) / TILE2D; ++ty)
                for (int tx = it->x0 / TILE2D; tx <= (it->x1 - 1) / TILE2D; ++tx) {
                    const int t = ty * f->tilesX + tx;
                    if (round) f->tileItems[t][f->tileItemCount[t]] = (uint32_t)n;
                    ++f->tileItemCount[t];
                }
        }
        for (size_t n = 0; n < f->lamps.len; ++n) {
            const Lamp2D* l = &f->lamps.items[n];
            for (int ty = l->y0 / TILE2D; ty <= (l->y1 - 1) / TILE2D; ++ty)
                for (int tx = l->x0 / TILE2D; tx <= (l->x1 - 1) / TILE2D; ++tx) {
                    const int t = ty * f->tilesX + tx;
                    if (round) f->tileLamps[t][f->tileLampCount[t]] = (uint16_t)n;
                    ++f->tileLampCount[t];
                }
        }
        if (round) break;
        for (int t = 0; t < tiles; ++t) {
            f->tileItems[t] = artfRealloc(f->tileItems[t], sizeof(uint32_t) * (f->tileItemCount[t] + 1));
            f->tileLamps[t] = artfRealloc(f->tileLamps[t], sizeof(uint16_t) * (f->tileLampCount[t] + 1));
            f->tileItemCount[t] = 0;
            f->tileLampCount[t] = 0;
        }
    }

    f->ambient[0] = s->ambient.x, f->ambient[1] = s->ambient.y, f->ambient[2] = s->ambient.z;
    const float a = s->backgroundAlpha;
    f->background[0] = s->background.x * a, f->background[1] = s->background.y * a, f->background[2] = s->background.z * a;
    f->background[3] = a;
    f->exposure = s->post.exposure > 0 ? s->post.exposure : 1;
    f->tone = s->post.tone;
}

/* ---- shadow maps ---- */

static const float* binTable(ArtfRenderer2D* r, int bins, bool sine) {
    const int k = log2Floor((float)bins);
    if (!r->binCos[k]) {
        r->binCos[k] = artfMalloc(sizeof(float) * bins);
        r->binSin[k] = artfMalloc(sizeof(float) * bins);
        for (int i = 0; i < bins; ++i) {
            const float a = ((float)i + .5f) / (float)bins * 2.f * PI2D - PI2D;
            r->binCos[k][i] = artfCos(a);
            r->binSin[k][i] = artfSin(a);
        }
    }
    return sine ? r->binSin[k] : r->binCos[k];
}

typedef struct MapJob {
    ArtfRenderer2D* r;
    uint32_t* lamps; /* those with maps */
} MapJob;

static void buildMap(void* user, uint32_t index, uint32_t worker) {
    (void)worker;
    MapJob* job = user;
    Frame2D* f = &job->r->frame;
    Lamp2D* l = &f->lamps.items[job->lamps[index]];
    if (l->kind == ARTF_LIGHT2D_SUN) {
        artf2DShadowBuild(f, l, NULL, NULL);
        return;
    }
    const int k = log2Floor((float)l->shadow->bins);
    artf2DShadowBuild(f, l, job->r->binCos[k], job->r->binSin[k]);
}

static int shadowMaps(ArtfRenderer2D* r) {
    Frame2D* f = &r->frame;
    uint32_t count = 0;
    for (size_t i = 0; i < f->lamps.len; ++i)
        if (f->lamps.items[i].shadow) ++count;
    if (!count) return 0;
    if (r->mapCount < count) {
        r->maps = artfRealloc(r->maps, sizeof(ShadowMap2D) * count);
        memset(r->maps + r->mapCount, 0, sizeof(ShadowMap2D) * (count - r->mapCount));
        r->mapCount = count;
    }
    uint32_t* lamps = artfMalloc(sizeof(uint32_t) * count);
    uint32_t n = 0;
    for (size_t i = 0; i < f->lamps.len; ++i) {
        Lamp2D* l = &f->lamps.items[i];
        if (!l->shadow) continue;
        ShadowMap2D* m = &r->maps[n];
        int bins;
        if (l->kind == ARTF_LIGHT2D_SUN) {
            /* (across the sun's light, a bin a pixel, over the whole frame) */
            const float gx = -l->dirX, gy = -l->dirY, planar = sqrtf(gx * gx + gy * gy);
            if (planar <= 0) {
                l->shadow = NULL;
                continue;
            }
            const float ax = gx / planar, ay = gy / planar;
            float lo = 1e30f, hi = -1e30f;
            for (int c = 0; c < 4; ++c) {
                const float px = c & 1 ? (float)r->width : 0, py = c & 2 ? (float)r->height : 0, u = -ay * px + ax * py;
                lo = artfMinf(lo, u), hi = artfMaxf(hi, u);
            }
            bins = artfMaxi((int)ceilf(hi - lo) + 4, 4);
            m->uMin = lo - 2.f;
            m->binsPerUnit = 1;
        } else {
            /* (enough that a bin at the end of its range is a pixel and a half across) */
            const float around = 2.f * PI2D * l->range / 1.5f;
            bins = 64;
            while (bins < 4096 && (float)bins < around) bins *= 2;
            binTable(r, bins, false);
        }
        if (m->bins < bins) {
            freeMap(m);
            m->near_ = artfMalloc(sizeof(float) * bins);
            m->past = artfMalloc(sizeof(float) * bins);
            m->owner = artfMalloc(sizeof(uint32_t) * bins);
        }
        m->bins = bins;
        l->shadow = m;
        lamps[n++] = (uint32_t)i;
    }
    /* (a map that was given more bins before keeps its memory: only bins are used) */
    MapJob job = {r, lamps};
    artfPoolRun(r->pool, buildMap, &job, n);
    artfFree(lamps);
    return (int)n;
}

/* ---- a frame ---- */

typedef struct TileJob {
    ArtfRenderer2D* r;
} TileJob;

static void drawTile(void* user, uint32_t index, uint32_t worker) {
    TileJob* job = user;
    artf2DDrawTile(&job->r->frame, index, &job->r->scratch[worker]);
}

static bool draw2D(ArtfRenderer2D* r, ArtfScene2D* s) {
    const double start = artfNowMs();
    Frame2D* f = &r->frame;
    prepare(r, s);
    /* (room for the crossings of the polygon with the most edges) */
    int edges = 0;
    for (size_t i = 0; i < f->items.len; ++i) edges = artfMaxi(edges, f->items.items[i].edgeCount);
    if (edges > r->scratchEdges) {
        r->scratchEdges = edges;
        for (int i = 0; i < artfPoolSize(r->pool); ++i) {
            r->scratch[i].crossX = artfRealloc(r->scratch[i].crossX, sizeof(float) * edges);
            r->scratch[i].crossDir = artfRealloc(r->scratch[i].crossDir, sizeof(int) * edges);
        }
    }
    const double prepared = artfNowMs();
    const int maps = shadowMaps(r);
    const double shadowed = artfNowMs();
    f->giLight = NULL;
    f->reflected = NULL;
    /* (on the GPU, gi is the GPU's) */
    if (s->gi.enabled && !r->gpu) artf2DGiRun(r->gi, f, &s->gi, r->pool);
    const double gathered = artfNowMs();

    const bool bloom = s->post.bloom > 0;
    f->pixels = r->pixels;
    r->stats.gpuMs = 0;
    r->everDrawn = true;
    r->pixelsFresh = false;
    bool shown = true;
    double drawn;
#ifdef ARTFUL_VULKAN
    if (r->gpu && !artf2DVulkanDraw(r->gpu, f, s->gi.enabled ? &s->gi : NULL, !artf2DVulkanShows(r->gpu))) {
        /* (a device lost: the CPU takes over, from this frame on, into memory) */
        artfEventSay(ARTF_EVENT_DEVICE_LOST, r, 0, 0, "artful-2d: the device is lost: the CPU draws from now on");
        shown = !artf2DVulkanShows(r->gpu);
        artf2DVulkanFree(r->gpu);
        r->gpu = NULL;
        snprintf(r->description, sizeof r->description, "software, %d thread%s", artfPoolSize(r->pool), artfPoolSize(r->pool) == 1 ? "" : "s");
        if (s->gi.enabled) artf2DGiRun(r->gi, f, &s->gi, r->pool);
    } else if (r->gpu) {
        drawn = artfNowMs();
        r->stats.cpuMs = drawn - start;
        r->stats.shadowsMs = shadowed - prepared;
        r->stats.giMs = gathered - shadowed;
        r->stats.drawMs = drawn - gathered;
        r->stats.postMs = 0;
        r->stats.spritesDrawn = (uint32_t)f->items.len;
        r->stats.lightsDrawn = (uint32_t)f->lamps.len;
        r->stats.shadowMaps = (uint32_t)maps;
        r->stats.gpuMs = artf2DVulkanGpuMs(r->gpu);
        return true;
    }
#endif
    if (bloom && !r->hdr) r->hdr = artfMalloc((size_t)r->width * r->height * 4 * sizeof(float));
    f->hdr = bloom ? r->hdr : NULL;
    TileJob job = {r};
    artfPoolRun(r->pool, drawTile, &job, (uint32_t)(f->tilesX * f->tilesY));
    drawn = artfNowMs();
    if (bloom) artf2DBloom(f, s->post.bloom, s->post.bloomRadius, r->pool);
    const double end = artfNowMs();

    r->stats.cpuMs = end - start;
    r->stats.shadowsMs = shadowed - prepared;
    r->stats.giMs = gathered - shadowed;
    r->stats.drawMs = drawn - gathered;
    r->stats.postMs = end - drawn;
    r->stats.spritesDrawn = (uint32_t)f->items.len;
    r->stats.lightsDrawn = (uint32_t)f->lamps.len;
    r->stats.shadowMaps = (uint32_t)maps;
    return shown;
}

bool artfRenderer2DDraw(ArtfRenderer2D* r, ArtfScene2D* s) {
    s->drawingThread = artfThreadId();
    const bool drawn = draw2D(r, s);
    s->drawingThread = 0;
    return drawn;
}
