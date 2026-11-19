/* artful-2d — after the frame: bloom, exposure, a tone curve, and bytes. */
#include "2d/internal2d.h"

/* ---- bloom: a chain of halves, each blurred into the one above on the way back (Jimenez, 2014) ---- */

#define BLOOM_LEVELS 8

typedef struct Bloom2D {
    Frame2D* f;
    float* level[BLOOM_LEVELS + 1]; /* 1..levels: rgb, each half the one before */
    int width[BLOOM_LEVELS + 1], height[BLOOM_LEVELS + 1];
    int at, levels;
    float amount;
} Bloom2D;

/* Level `at` from the one above it: the mean of its four. */
static void down(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    const Bloom2D* b = user;
    const int l = b->at, w = b->width[l];
    const bool first = l == 1;
    const float* src = first ? b->f->hdr : b->level[l - 1];
    const int sw = first ? b->f->width : b->width[l - 1], sh = first ? b->f->height : b->height[l - 1], step = first ? 4 : 3;
    const float* rows[2] = {src + (size_t)artfMini((int)y * 2, sh - 1) * sw * step, src + (size_t)artfMini((int)y * 2 + 1, sh - 1) * sw * step};
    float* dst = b->level[l] + (size_t)y * w * 3;
    for (int x = 0; x < w; ++x) {
        const int x0 = artfMini(x * 2, sw - 1) * step, x1 = artfMini(x * 2 + 1, sw - 1) * step;
        for (int c = 0; c < 3; ++c) dst[x * 3 + c] = (rows[0][x0 + c] + rows[0][x1 + c] + rows[1][x0 + c] + rows[1][x1 + c]) * .25f;
    }
}

/* Where a texel x of a level twice as large reads from this one, and how much: a tent of three (1 4 3 or 3 4 1, over
 * eight), or with `two`, between two (1 3 or 3 1, over four). */
typedef struct Taps2D {
    int at[3];
    float weight[3];
} Taps2D;

static inline Taps2D tapsFor(int x, int n, bool two) {
    const int i = x >> 1;
    Taps2D t;
    t.at[0] = artfMaxi(i - 1, 0), t.at[1] = artfMini(i, n - 1), t.at[2] = artfMini(i + 1, n - 1);
    if (two) {
        t.weight[0] = x & 1 ? 0 : .25f, t.weight[1] = .75f, t.weight[2] = x & 1 ? .25f : 0;
    } else {
        t.weight[0] = x & 1 ? .125f : .375f, t.weight[1] = .5f, t.weight[2] = x & 1 ? .375f : .125f;
    }
    return t;
}

/* A texel of a level twice as large as src, from it. */
static inline void upsample(const float* src, int sw, const Taps2D* ty, const Taps2D* tx, float out[3]) {
    out[0] = out[1] = out[2] = 0;
    for (int j = 0; j < 3; ++j) {
        if (ty->weight[j] == 0) continue;
        const float* row = src + (size_t)ty->at[j] * sw * 3;
        for (int k = 0; k < 3; ++k) {
            const float wgt = ty->weight[j] * tx->weight[k];
            if (wgt == 0) continue;
            const float* p = row + tx->at[k] * 3;
            out[0] += p[0] * wgt, out[1] += p[1] * wgt, out[2] += p[2] * wgt;
        }
    }
}

static void up(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    const Bloom2D* b = user;
    const int l = b->at, w = b->width[l];
    const Taps2D ty = tapsFor((int)y, b->height[l + 1], false);
    float* dst = b->level[l] + (size_t)y * w * 3;
    for (int x = 0; x < w; ++x) {
        const Taps2D tx = tapsFor(x, b->width[l + 1], false);
        float t[3];
        upsample(b->level[l + 1], b->width[l + 1], &ty, &tx, t);
        for (int c = 0; c < 3; ++c) dst[x * 3 + c] += t[c];
    }
}

/* The frame: its light mixed with the bloom's (level 1, half the frame, between its texels), then out to bytes. */
static void finish(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    const Bloom2D* b = user;
    Frame2D* f = b->f;
    const float share = b->amount / (float)b->levels, keep = 1.f - b->amount;
    const Taps2D ty = tapsFor((int)y, b->height[1], true);
    for (int x = 0; x < f->width; ++x) {
        const Taps2D tx = tapsFor(x, b->width[1], true);
        float glow[3];
        upsample(b->level[1], b->width[1], &ty, &tx, glow);
        const float* p = f->hdr + ((size_t)y * f->width + x) * 4;
        const float v[4] = {p[0] * keep + glow[0] * share, p[1] * keep + glow[1] * share, p[2] * keep + glow[2] * share, p[3]};
        artf2DTone(f, v, f->pixels + (size_t)y * f->width + x);
    }
}

static void plain(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    Frame2D* f = user;
    for (int x = 0; x < f->width; ++x) {
        const size_t i = (size_t)y * f->width + x;
        artf2DTone(f, f->hdr + i * 4, f->pixels + i);
    }
}

void artf2DBloom(Frame2D* f, float amount, float radius, ArtfPool* pool) {
    if (amount <= 0) {
        artfPoolRun(pool, plain, f, (uint32_t)f->height);
        return;
    }
    Bloom2D b = {.f = f, .amount = artfClampf(amount, 0, 1)};
    /* as many halvings as reach the radius */
    const float reach = artfMaxf(radius > 0 ? radius : .05f, .001f) * (float)f->height;
    int levels = 1;
    while (levels < BLOOM_LEVELS && (float)(2 << levels) < reach * 2.f) ++levels;
    int w = f->width, h = f->height;
    for (int l = 1; l <= levels; ++l) {
        w = artfMaxi((w + 1) / 2, 1);
        h = artfMaxi((h + 1) / 2, 1);
        b.width[l] = w;
        b.height[l] = h;
        b.level[l] = artfMalloc((size_t)w * h * 3 * sizeof(float));
        b.levels = l;
        if (w == 1 && h == 1) break;
    }
    levels = b.levels;
    for (int l = 1; l <= levels; ++l) {
        b.at = l;
        artfPoolRun(pool, down, &b, (uint32_t)b.height[l]);
    }
    for (int l = levels - 1; l >= 1; --l) {
        b.at = l;
        artfPoolRun(pool, up, &b, (uint32_t)b.height[l]);
    }
    artfPoolRun(pool, finish, &b, (uint32_t)f->height);
    for (int l = 1; l <= levels; ++l) artfFree(b.level[l]);
}
