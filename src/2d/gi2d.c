/* artful-2d — light that glowing sprites give off and walls send on, by radiance cascades (Sannikov, 2023).
 *
 * The frame is cut into texels of gi (a side of `scale` pixels). What glows and what casts shadows is solid there,
 * and each solid texel has the light it sends: what glows, plus what was on a wall in the frame before, times its
 * color and the bounce. A field of distances to the nearest solid texel (Felzenszwalb and Huttenlocher's, exact) lets
 * rays leap across what is empty.
 *
 * Cascade 0 has a probe at each texel, with 4 rays a short way out; each next cascade has probes twice as far apart,
 * with 4 times as many rays, each going 4 times as far, from where the one below stopped. So each cascade is as much
 * work as the first, and a probe sees near things sharply and far things as broadly as they need. From the top down,
 * what a ray did not hit is taken from the cascade above: its 4 rays nearest in direction, at its 4 probes nearest,
 * blended. Cascade 0's 4 rays, averaged, are the light at a texel.
 *
 * Each probe is one thread's, and reads only what is above: the same bytes, whatever the threads. */
#include "2d/internal2d.h"
#include "core/trig.h"

#include <float.h>

#define MAX_CASCADES 10

struct Gi2D {
    int width, height, scale;
    uint8_t* solid;
    float* source;   /* 3 a texel */
    float* distance;
    float* light;    /* 3 a texel */
    float* lightTmp;
    float* reflected[2]; /* 4 a texel: what the tiles of the last frame left, and what this frame's leave */
    int which;
    float* rays[2];
    size_t rayCap;
    int cascades;
    float* dirs[MAX_CASCADES]; /* cos and sin of each ray of a cascade */
    float* scratch;  /* the distance transform's, per worker */
    size_t scratchCap;
};

Gi2D* artf2DGiNew(void) { return ARTF_NEW(Gi2D); }

static void dropGrid(Gi2D* g) {
    artfFree(g->solid);
    artfFree(g->source);
    artfFree(g->distance);
    artfFree(g->light);
    artfFree(g->lightTmp);
    artfFree(g->reflected[0]);
    artfFree(g->reflected[1]);
    g->solid = NULL;
    g->source = g->distance = g->light = g->lightTmp = g->reflected[0] = g->reflected[1] = NULL;
}

void artf2DGiFree(Gi2D* g) {
    if (!g) return;
    dropGrid(g);
    artfFree(g->rays[0]);
    artfFree(g->rays[1]);
    for (int c = 0; c < MAX_CASCADES; ++c) artfFree(g->dirs[c]);
    artfFree(g->scratch);
    artfFree(g);
}

/* ---- what is solid, and what it sends ---- */

typedef struct GiRun {
    Gi2D* g;
    const Frame2D* f;
    float bounce, intensity;
    int cascade;
    int workers;
} GiRun;

static void scene(void* user, uint32_t gy, uint32_t worker) {
    (void)worker;
    const GiRun* run = user;
    Gi2D* g = run->g;
    const Frame2D* f = run->f;
    const int w = g->width, scale = g->scale;
    uint8_t* solid = g->solid + (size_t)gy * w;
    float* source = g->source + (size_t)gy * w * 3;
    memset(solid, 0, (size_t)w);
    memset(source, 0, (size_t)w * 3 * sizeof(float));
    const float y = ((float)gy + .5f) * (float)scale;

    /* outlines: closed ones as their inside (non-zero winding), open ones as lines a texel wide */
    for (size_t o = 0; o < f->outlines.len; ++o) {
        const Outline2D* ol = &f->outlines.items[o];
        const float reach = (float)scale * .75f;
        if (y < ol->y0 - reach || y > ol->y1 + reach) continue;
        const ArtfVec2* p = f->outlinePoints.items + ol->start;
        if (ol->closed) {
            const int gx0 = artfMaxi((int)(ol->x0 / (float)scale), 0), gx1 = artfMini((int)(ol->x1 / (float)scale) + 1, w);
            for (int gx = gx0; gx < gx1; ++gx) {
                const float x = ((float)gx + .5f) * (float)scale;
                int winding = 0;
                for (int i = 0; i < ol->count; ++i) {
                    const ArtfVec2 a = p[i], b = p[(i + 1) % ol->count];
                    if ((a.y <= y) == (b.y <= y)) continue;
                    if (a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x) > x) winding += b.y > a.y ? 1 : -1;
                }
                if (winding) solid[gx] = 1;
            }
        }
        if (ol->closed && ol->count > 2) continue;
        for (int i = 0; i + 1 < ol->count; ++i) {
            const ArtfVec2 a = p[i], b = p[i + 1];
            const int gx0 = artfMaxi((int)((artfMinf(a.x, b.x) - reach) / (float)scale), 0);
            const int gx1 = artfMini((int)((artfMaxf(a.x, b.x) + reach) / (float)scale) + 1, w);
            const float ex = b.x - a.x, ey = b.y - a.y, len2 = ex * ex + ey * ey;
            for (int gx = gx0; gx < gx1; ++gx) {
                const float x = ((float)gx + .5f) * (float)scale;
                const float t = len2 > 0 ? artfClampf(((x - a.x) * ex + (y - a.y) * ey) / len2, 0, 1) : 0;
                const float dx = x - a.x - ex * t, dy = y - a.y - ey * t;
                if (dx * dx + dy * dy <= reach * reach) solid[gx] = 1;
            }
        }
    }
    /* what walls send on, from the frame before */
    const float* back = g->reflected[g->which] + (size_t)gy * w * 4; /* (the one the last frame's tiles had) */
    if (run->bounce > 0)
        for (int gx = 0; gx < w; ++gx)
            if (solid[gx] && back[gx * 4 + 3] > 0) {
                const float k = run->bounce / back[gx * 4 + 3];
                for (int c = 0; c < 3; ++c) source[gx * 3 + c] = back[gx * 4 + c] * k;
            }
    /* what glows */
    for (size_t n = 0; n < f->items.len; ++n) {
        const Item2D* it = &f->items.items[n];
        if (!it->emits || y < (float)it->y0 || y >= (float)it->y1) continue;
        const int gx0 = artfMaxi(it->x0 / scale, 0), gx1 = artfMini((it->x1 + scale - 1) / scale, w);
        for (int gx = gx0; gx < gx1; ++gx) {
            const float x = ((float)gx + .5f) * (float)scale;
            const ArtfVec2 l = artf2DApply(&it->toLocal, x, y);
            const float cover = artf2DCoverAt(it->sp, l.x, l.y, 1.f / it->zoom);
            if (cover < .5f) continue;
            float rgba[4], emit[3];
            artf2DSample(it, l.x, l.y, 1, rgba, emit, NULL);
            if (emit[0] + emit[1] + emit[2] <= 0) continue;
            solid[gx] = 1;
            for (int c = 0; c < 3; ++c) source[gx * 3 + c] += emit[c];
        }
    }
}

/* ---- distances ---- */

/* The lower envelope of parabolas (Felzenszwalb and Huttenlocher, 2012): squared distances along a line, in place. */
static void distanceLine(float* line, int n, size_t step, float* tmp) {
    float* d = tmp;
    float* z = tmp + n;
    int* v = (int*)(tmp + 2 * n + 1);
    for (int q = 0; q < n; ++q) d[q] = line[q * step];
    /* (only what is not empty makes a parabola) */
    int k = -1;
    for (int q = 0; q < n; ++q) {
        if (d[q] >= 1e19f) continue;
        if (k < 0) {
            k = 0;
            v[0] = q;
            z[0] = -FLT_MAX;
            z[1] = FLT_MAX;
            continue;
        }
        float s;
        for (;;) {
            s = ((d[q] + (float)q * q) - (d[v[k]] + (float)v[k] * v[k])) / (2.f * (float)(q - v[k]));
            if (s > z[k]) break;
            --k; /* (z[0] is the lowest float: k stays at 0 or above) */
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = FLT_MAX;
    }
    if (k < 0) {
        for (int q = 0; q < n; ++q) line[q * step] = 1e20f;
        return;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < (float)q) ++k;
        const float dq = (float)(q - v[k]);
        line[q * step] = dq * dq + d[v[k]];
    }
}

static void distanceRows(void* user, uint32_t y, uint32_t worker) {
    const GiRun* run = user;
    Gi2D* g = run->g;
    float* line = g->distance + (size_t)y * g->width;
    const uint8_t* solid = g->solid + (size_t)y * g->width;
    for (int x = 0; x < g->width; ++x) line[x] = solid[x] ? 0 : 1e20f;
    distanceLine(line, g->width, 1, g->scratch + worker * (g->scratchCap / (size_t)run->workers));
}

static void distanceColumns(void* user, uint32_t x, uint32_t worker) {
    const GiRun* run = user;
    Gi2D* g = run->g;
    float* column = g->distance + x;
    distanceLine(column, g->height, (size_t)g->width, g->scratch + worker * (g->scratchCap / (size_t)run->workers));
    for (int y = 0; y < g->height; ++y) column[(size_t)y * g->width] = sqrtf(column[(size_t)y * g->width]);
}

/* ---- cascades ---- */

static int spacing(int c) { return 1 << c; }
static int raysOf(int c) { return 4 << (2 * c); }
static float startOf(int c) { return (float)((1 << (2 * c)) - 1) / 3.f; }

/* Along a ray from (ox, oy) from t0 to t1: the light it meets, or none (*through 1). */
static void march(const Gi2D* g, float ox, float oy, float dx, float dy, float t0, float t1, float out[3], float* through) {
    float t = t0;
    const int w = g->width, h = g->height;
    while (t < t1) {
        const float x = ox + dx * t, y = oy + dy * t;
        if (x < 0 || y < 0 || x >= (float)w || y >= (float)h) break;
        const size_t i = (size_t)(int)y * w + (int)x;
        if (g->solid[i]) {
            out[0] = g->source[i * 3];
            out[1] = g->source[i * 3 + 1];
            out[2] = g->source[i * 3 + 2];
            *through = 0;
            return;
        }
        t += artfMaxf(g->distance[i] - 1.5f, .5f);
    }
    out[0] = out[1] = out[2] = 0;
    *through = 1;
}

static void cascadeRow(void* user, uint32_t py, uint32_t worker) {
    (void)worker;
    const GiRun* run = user;
    const Gi2D* g = run->g;
    const int c = run->cascade, s = spacing(c), n = raysOf(c);
    const int pw = (g->width + s - 1) / s;
    const bool top = c == g->cascades - 1;
    const int us = s * 2, upw = (g->width + us - 1) / us, uph = (g->height + us - 1) / us, un = n * 4;
    const float* up = g->rays[(c + 1) & 1];
    float* out = g->rays[c & 1];
    const float t0 = startOf(c), t1 = startOf(c + 1);
    const float* dir = g->dirs[c];
    for (int px = 0; px < pw; ++px) {
        const float ox = ((float)px + .5f) * (float)s, oy = ((float)py + .5f) * (float)s;
        int ui[2] = {0, 0}, uj[2] = {0, 0};
        float wx = 0, wy = 0;
        if (!top) {
            const float fx = ox / (float)us - .5f, fy = oy / (float)us - .5f;
            const float flx = floorf(fx), fly = floorf(fy);
            wx = artfClampf(fx - flx, 0, 1);
            wy = artfClampf(fy - fly, 0, 1);
            ui[0] = artfClampi((int)flx, 0, upw - 1);
            ui[1] = artfClampi((int)flx + 1, 0, upw - 1);
            uj[0] = artfClampi((int)fly, 0, uph - 1);
            uj[1] = artfClampi((int)fly + 1, 0, uph - 1);
        }
        float* probe = out + ((size_t)py * pw + px) * n * 3;
        for (int k = 0; k < n; ++k) {
            float r[3], through;
            march(g, ox, oy, dir[k * 2], dir[k * 2 + 1], t0, t1, r, &through);
            if (through > 0 && !top) {
                /* (the four rays above nearest in direction, at the four probes above, blended) */
                float far[3] = {0, 0, 0};
                const float weight[4] = {(1 - wx) * (1 - wy), wx * (1 - wy), (1 - wx) * wy, wx * wy};
                for (int q = 0; q < 4; ++q) {
                    if (weight[q] == 0) continue;
                    const float* above = up + ((size_t)uj[q >> 1] * upw + ui[q & 1]) * un * 3 + (size_t)k * 4 * 3;
                    for (int c3 = 0; c3 < 3; ++c3)
                        far[c3] += weight[q] * .25f * (above[c3] + above[3 + c3] + above[6 + c3] + above[9 + c3]);
                }
                for (int c3 = 0; c3 < 3; ++c3) r[c3] += far[c3] * through;
            }
            probe[k * 3] = r[0];
            probe[k * 3 + 1] = r[1];
            probe[k * 3 + 2] = r[2];
        }
    }
}

/* Light at each texel, from cascade 0; then into what is solid, from what is next to it. */
static void gather(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    const GiRun* run = user;
    Gi2D* g = run->g;
    const float* rays = g->rays[0];
    for (int x = 0; x < g->width; ++x) {
        const size_t i = (size_t)y * g->width + x;
        const float* p = rays + i * 4 * 3;
        for (int c = 0; c < 3; ++c) g->light[i * 3 + c] = (p[c] + p[3 + c] + p[6 + c] + p[9 + c]) * .25f * run->intensity;
    }
}

static void spread(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    const GiRun* run = user;
    Gi2D* g = run->g;
    const int w = g->width, h = g->height;
    for (int x = 0; x < w; ++x) {
        const size_t i = (size_t)y * w + x;
        float* dst = g->lightTmp + i * 3;
        if (!g->solid[i]) {
            memcpy(dst, g->light + i * 3, 3 * sizeof(float));
            continue;
        }
        float sum[3] = {0, 0, 0};
        int n = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = (int)y + dy;
                if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const size_t j = (size_t)ny * w + nx;
                if (g->solid[j] && g->distance[j] == 0 && run->cascade == 0) continue;
                for (int c = 0; c < 3; ++c) sum[c] += g->light[j * 3 + c];
                ++n;
            }
        for (int c = 0; c < 3; ++c) dst[c] = n ? sum[c] / (float)n : 0;
    }
}

void artf2DGiRun(Gi2D* g, Frame2D* f, const ArtfGI2DDesc* desc, ArtfPool* pool) {
    const int scale = desc->scale;
    const int w = (f->width + scale - 1) / scale, h = (f->height + scale - 1) / scale;
    if (w != g->width || h != g->height || scale != g->scale) {
        dropGrid(g);
        g->width = w;
        g->height = h;
        g->scale = scale;
        const size_t texels = (size_t)w * h;
        g->solid = artfCalloc(texels, 1);
        g->source = artfCalloc(texels * 3, sizeof(float));
        g->distance = artfCalloc(texels, sizeof(float));
        g->light = artfCalloc(texels * 3, sizeof(float));
        g->lightTmp = artfCalloc(texels * 3, sizeof(float));
        g->reflected[0] = artfCalloc(texels * 4, sizeof(float));
        g->reflected[1] = artfCalloc(texels * 4, sizeof(float));
    }
    const int workers = artfPoolSize(pool);
    const size_t perWorker = (size_t)artfMaxi(w, h) * 4 + 4;
    if (g->scratchCap < perWorker * workers) {
        artfFree(g->scratch);
        g->scratchCap = perWorker * workers;
        g->scratch = artfMalloc(g->scratchCap * sizeof(float));
    }
    /* how many cascades: until a ray reaches across all of it */
    const float across = sqrtf((float)w * w + (float)h * h);
    int cascades = 1;
    while (cascades < MAX_CASCADES && startOf(cascades) < across) ++cascades;
    g->cascades = cascades;
    size_t most = 0;
    for (int c = 0; c < cascades; ++c) {
        const int s = spacing(c);
        const size_t rays = (size_t)((w + s - 1) / s) * ((h + s - 1) / s) * raysOf(c);
        if (rays > most) most = rays;
        if (!g->dirs[c]) {
            const int n = raysOf(c);
            g->dirs[c] = artfMalloc(sizeof(float) * 2 * n);
            for (int k = 0; k < n; ++k) {
                const float a = ((float)k + .5f) / (float)n * 2.f * PI2D;
                g->dirs[c][k * 2] = artfCos(a);
                g->dirs[c][k * 2 + 1] = artfSin(a);
            }
        }
    }
    if (most * 3 > g->rayCap) {
        artfFree(g->rays[0]);
        artfFree(g->rays[1]);
        g->rayCap = most * 3;
        g->rays[0] = artfMalloc(g->rayCap * sizeof(float));
        g->rays[1] = artfMalloc(g->rayCap * sizeof(float));
    }

    GiRun run = {.g = g, .f = f, .bounce = desc->bounce, .intensity = desc->intensity, .workers = workers};
    artfPoolRun(pool, scene, &run, (uint32_t)h);
    artfPoolRun(pool, distanceRows, &run, (uint32_t)h);
    artfPoolRun(pool, distanceColumns, &run, (uint32_t)w);
    for (int c = cascades - 1; c >= 0; --c) {
        run.cascade = c;
        const int s = spacing(c);
        artfPoolRun(pool, cascadeRow, &run, (uint32_t)((h + s - 1) / s));
    }
    artfPoolRun(pool, gather, &run, (uint32_t)h);
    /* (twice into what is solid: once from what is open, then from that) */
    for (int pass = 0; pass < 2; ++pass) {
        run.cascade = pass;
        artfPoolRun(pool, spread, &run, (uint32_t)h);
        float* t = g->light;
        g->light = g->lightTmp;
        g->lightTmp = t;
    }

    /* what this frame's tiles leave for the next */
    g->which ^= 1;
    memset(g->reflected[g->which], 0, (size_t)w * h * 4 * sizeof(float));
    f->giScale = scale;
    f->giWidth = w;
    f->giHeight = h;
    f->giLight = g->light;
    f->reflected = g->reflected[g->which];
}
