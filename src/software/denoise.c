/* artful — what is left of noise in a traced frame, taken out on the CPU: the arithmetic the GPU's shaders
 * (artfShaderDenoise) repeat. Each pixel has what its first surface was: where, which way it faced and what color.
 * Light over color is what is noisy, and what is smooth across a surface.
 *
 *   time    a pixel finds where its surface was in the frame before, by how the eye and that object moved, and adds
 *           to what was gathered there; what was hidden starts over. What was gathered must look like this frame's light around
 *           the pixel, one by one and on the whole: when a light or a shadow changed over a surface that stayed,
 *           it does not, and is cut back
 *   space   three passes of a wavelet filter (Dammertz et al., 2010), over what faces the same way at the same
 *           distance: each pass reads twice as far, with five taps a side */
#include "software/denoise.h"
#include "core/threads.h"

#include <stdlib.h>

#define HISTORY_LIMIT 256.f
#define SETTLED 48.f /* gathered this much, a pixel needs no help from its neighbours */
#define SPREAD 1.25f   /* how many deviations of this frame's light around a pixel what was gathered may be from it */
#define CUT_BACK 8.f   /* what is left of a history that was found out */
#define DRIFT 4.f      /* how many standard errors the mean of a neighbourhood may be from what was gathered there */

uint32_t artfNormalPack(const float* n) {
    uint32_t bits = 0;
    for (int k = 0; k < 3; ++k) bits |= (uint32_t)((n[k] * .5f + .5f) * 1023.f + .5f) << (k * 10);
    return bits;
}

static void normalUnpack(uint32_t bits, float* n) {
    for (int k = 0; k < 3; ++k) n[k] = (float)(bits >> (k * 10) & 1023) * (2.f / 1023.f) - 1.f;
}

static inline float normalOf(const float* guide, float* n) {
    uint32_t bits;
    memcpy(&bits, &guide[3], 4);
    normalUnpack(bits, n);
    return bits ? 1.f : 0.f;
}

typedef struct Pass {
    ArtfDenoiser* d;
    const ArtfFrameData* frame;
    const float* previous; /* last frame's view and projection */
    uint32_t samples;
    int step;
    const float* from;
    float* to;
    bool first;
    const float* motion;
    size_t motionCount;
} Pass;

/* This frame's light over color, in ping: the fourth number says whether there is a surface. */
static void lightRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    Pass* p = user;
    ArtfDenoiser* d = p->d;
    for (int x = 0; x < d->width; ++x) {
        const size_t i = (size_t)y * d->width + (size_t)x;
        const float* noisy = d->noisy + i * 4;
        const float* albedo = d->albedo + i * 4;
        float* out = d->ping + i * 4;
        uint32_t bits;
        memcpy(&bits, &d->guide[i * 4 + 3], 4);
        const float scale = 1.f / artfMaxf(noisy[3], 1.f);
        for (int k = 0; k < 3; ++k) out[k] = noisy[k] * scale / artfMaxf(albedo[k], .02f);
        out[3] = bits ? 1.f : 0.f;
    }
}

/* What this frame's light says a pixel can be: its mean around, give or take. And whether, taken together, the
 * neighbourhood has drifted from what was gathered around (jx, jy): a change too small to show in one noisy pixel. */
static bool lightRange(const ArtfDenoiser* d, int x, int y, int jx, int jy, float* low, float* high) {
    float sum[3] = {0, 0, 0}, squares[3] = {0, 0, 0}, count = 0;
    float nowSum[3] = {0, 0, 0}, thenSum[3] = {0, 0, 0}, pairs = 0;
    for (int dy = -2; dy <= 2; ++dy) {
        const int qy = y + dy;
        if (qy < 0 || qy >= d->height) continue;
        for (int dx = -2; dx <= 2; ++dx) {
            const int qx = x + dx;
            if (qx < 0 || qx >= d->width) continue;
            const float* q = d->ping + ((size_t)qy * d->width + (size_t)qx) * 4;
            for (int k = 0; k < 3; ++k) {
                sum[k] += q[k] * q[3];
                squares[k] += q[k] * q[k] * q[3];
            }
            count += q[3];
            const int hx = artfClampi(jx + dx, 0, d->width - 1), hy = artfClampi(jy + dy, 0, d->height - 1);
            const size_t j = (size_t)hy * d->width + (size_t)hx;
            uint32_t bits;
            memcpy(&bits, &d->previousGuide[j * 4 + 3], 4);
            const float pair = bits ? q[3] : 0.f;
            for (int k = 0; k < 3; ++k) {
                nowSum[k] += q[k] * pair;
                thenSum[k] += d->history[j * 4 + k] * pair;
            }
            pairs += pair;
        }
    }
    const float inverse = 1.f / artfMaxf(count, 1.f), pairInverse = 1.f / artfMaxf(pairs, 1.f);
    const float error = DRIFT * sqrtf(pairInverse);
    bool drifted = false;
    for (int k = 0; k < 3; ++k) {
        const float mean = sum[k] * inverse;
        const float deviation = sqrtf(artfMaxf(squares[k] * inverse - mean * mean, 0));
        const float reach = SPREAD * deviation + .01f + .05f * mean;
        low[k] = mean - reach;
        high[k] = mean + reach;
        const float apart = (nowSum[k] - thenSum[k]) * pairInverse;
        drifted = drifted || apart * apart > (error * deviation + .002f + .01f * mean) * (error * deviation + .002f + .01f * mean);
    }
    return drifted;
}

static void temporalRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    Pass* p = user;
    ArtfDenoiser* d = p->d;
    const int W = d->width, H = d->height;
    for (int x = 0; x < W; ++x) {
        const size_t i = (size_t)y * W + (size_t)x;
        const float* noisy = d->noisy + i * 4;
        const float* guide = d->guide + i * 4;
        const float* light = d->ping + i * 4;
        float* out = d->nextHistory + i * 4;
        const float coverage = noisy[3] / (float)p->samples;
        float n[3];
        if (!normalOf(guide, n)) {
            /* nothing here: its coverage alone goes on */
            out[0] = out[1] = out[2] = 0;
            out[3] = 1.f + artfMinf(coverage, .999f);
            continue;
        }
        const float* m = p->previous;
        float wx = guide[0], wy = guide[1], wz = guide[2];
        /* (where its object was, and the way it faced) */
        const uint32_t instance = (uint32_t)d->albedo[i * 4 + 3];
        if (instance && instance <= p->motionCount) {
            const float* k = p->motion + (size_t)(instance - 1) * 12;
            const float* back = d->shift + i * 4;
            const float x0 = wx + back[0], y0 = wy + back[1], z0 = wz + back[2], n0 = n[0], n1 = n[1], n2 = n[2];
            wx = k[0] * x0 + k[1] * y0 + k[2] * z0 + k[3];
            wy = k[4] * x0 + k[5] * y0 + k[6] * z0 + k[7];
            wz = k[8] * x0 + k[9] * y0 + k[10] * z0 + k[11];
            n[0] = k[0] * n0 + k[1] * n1 + k[2] * n2;
            n[1] = k[4] * n0 + k[5] * n1 + k[6] * n2;
            n[2] = k[8] * n0 + k[9] * n1 + k[10] * n2;
        }
        const float cx = m[0] * wx + m[4] * wy + m[8] * wz + m[12], cy = m[1] * wx + m[5] * wy + m[9] * wz + m[13], cw = m[3] * wx + m[7] * wy + m[11] * wz + m[15];
        float count = 1.f, before[3] = {0, 0, 0}, alphaBefore = coverage;
        if (!p->first && cw > 0) {
            const float px = (cx / cw * .5f + .5f) * (float)W, py = (.5f - cy / cw * .5f) * (float)H;
            if (px >= 0 && py >= 0 && px < (float)W && py < (float)H) {
                const size_t j = (size_t)(int)py * W + (size_t)(int)px;
                const float* then = d->previousGuide + j * 4;
                float pn[3];
                const float dx = then[0] - wx, dy = then[1] - wy, dz = then[2] - wz;
                const float ex = wx - d->eye[0], ey = wy - d->eye[1], ez = wz - d->eye[2];
                const float tolerance = .01f * sqrtf(ex * ex + ey * ey + ez * ez) + 1e-3f;
                /* (how far off its plane, not how far: a sample lands anywhere in its pixel, and at a grazing angle
                 * a pixel is long) */
                const float off = dx * n[0] + dy * n[1] + dz * n[2];
                if (normalOf(then, pn) && off * off < tolerance * tolerance && n[0] * pn[0] + n[1] * pn[1] + n[2] * pn[2] > .9f) {
                    const float* h = d->history + j * 4;
                    const float whole = floorf(h[3]);
                    float low[3], high[3];
                    bool found = lightRange(d, x, (int)y, (int)(j % (size_t)W), (int)(j / (size_t)W), low, high);
                    for (int k = 0; k < 3; ++k) {
                        before[k] = artfMinf(artfMaxf(h[k], low[k]), high[k]);
                        found = found || before[k] != h[k];
                    }
                    count = artfMinf(whole + 1.f, found ? CUT_BACK : HISTORY_LIMIT);
                    alphaBefore = h[3] - whole;
                }
            }
        }
        const float share = 1.f / count;
        for (int k = 0; k < 3; ++k) out[k] = before[k] + (light[k] - before[k]) * share;
        out[3] = count + artfMinf(alphaBefore + (coverage - alphaBefore) * share, .999f);
    }
}

static void spatialRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    static const float kernel[3] = {.375f, .25f, .0625f};
    Pass* p = user;
    ArtfDenoiser* d = p->d;
    const int W = d->width, H = d->height, step = p->step;
    for (int x = 0; x < W; ++x) {
        const size_t i = (size_t)y * W + (size_t)x;
        const float* guide = d->guide + i * 4;
        const float* center = p->from + i * 4;
        float* out = p->to + i * 4;
        float n[3];
        if (!normalOf(guide, n) || floorf(d->nextHistory[i * 4 + 3]) >= SETTLED) {
            memcpy(out, center, 16);
            continue;
        }
        const float ex = guide[0] - d->eye[0], ey = guide[1] - d->eye[1], ez = guide[2] - d->eye[2];
        const float distance = sqrtf(ex * ex + ey * ey + ez * ez), reach = distance * .02f + 1e-3f;
        float sum[3] = {0, 0, 0}, total = 0;
        for (int dy = -2; dy <= 2; ++dy) {
            const int qy = (int)y + dy * step;
            if (qy < 0 || qy >= H) continue;
            for (int dx = -2; dx <= 2; ++dx) {
                const int qx = x + dx * step;
                if (qx < 0 || qx >= W) continue;
                const size_t j = (size_t)qy * W + (size_t)qx;
                const float* other = d->guide + j * 4;
                float qn[3];
                if (!normalOf(other, qn)) continue;
                const float qex = other[0] - d->eye[0], qey = other[1] - d->eye[1], qez = other[2] - d->eye[2];
                const float apart = (sqrtf(qex * qex + qey * qey + qez * qez) - distance) / reach;
                const float facing = artfMaxf(n[0] * qn[0] + n[1] * qn[1] + n[2] * qn[2], 0), f2 = facing * facing;
                const float weight = kernel[abs(dx)] * kernel[abs(dy)] * f2 * f2 / (1.f + apart * apart);
                const float* q = p->from + j * 4;
                for (int k = 0; k < 3; ++k) sum[k] += q[k] * weight;
                total += weight;
            }
        }
        const float inverse = total > 0 ? 1.f / total : 0.f;
        for (int k = 0; k < 3; ++k) out[k] = sum[k] * inverse;
        out[3] = center[3];
    }
}

static void finishRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    Pass* p = user;
    ArtfDenoiser* d = p->d;
    for (int x = 0; x < d->width; ++x) {
        const size_t i = (size_t)y * d->width + (size_t)x;
        const float* from = p->from + i * 4;
        const float* albedo = d->albedo + i * 4;
        float* out = d->noisy + i * 4;
        const float alpha = from[3] - floorf(from[3]);
        for (int k = 0; k < 3; ++k) out[k] = from[k] * artfMaxf(albedo[k], .02f) * alpha;
        out[3] = alpha;
    }
}

void artfDenoiserResize(ArtfDenoiser* d, int width, int height) {
    if (d->width == width && d->height == height) return;
    artfDenoiserFree(d);
    const size_t bytes = (size_t)width * height * 16;
    d->width = width;
    d->height = height;
    d->noisy = artfCalloc(1, bytes);
    d->guide = artfCalloc(1, bytes);
    d->albedo = artfCalloc(1, bytes);
    d->shift = artfCalloc(1, bytes);
    d->previousGuide = artfCalloc(1, bytes);
    d->history = artfCalloc(1, bytes);
    d->nextHistory = artfCalloc(1, bytes);
    d->ping = artfCalloc(1, bytes);
    d->pong = artfCalloc(1, bytes);
    d->first = true;
}

void artfDenoiserFree(ArtfDenoiser* d) {
    artfFree(d->noisy);
    artfFree(d->guide);
    artfFree(d->albedo);
    artfFree(d->shift);
    artfFree(d->previousGuide);
    artfFree(d->history);
    artfFree(d->nextHistory);
    artfFree(d->ping);
    artfFree(d->pong);
    memset(d, 0, sizeof *d);
}

void artfDenoiserRun(ArtfDenoiser* d, ArtfPool* pool, const ArtfFrameData* frame, uint32_t samples, const float* motion, size_t motionCount) {
    memcpy(d->eye, frame->cameraPos, 12);
    Pass p = {d, frame, d->viewProj, samples, 1, NULL, NULL, d->first, motion, motionCount};
    artfPoolRun(pool, lightRow, &p, (uint32_t)d->height);
    artfPoolRun(pool, temporalRow, &p, (uint32_t)d->height);
    p.from = d->nextHistory;
    p.to = d->ping;
    for (int i = 0; i < 3; ++i) {
        p.step = 1 << i;
        artfPoolRun(pool, spatialRow, &p, (uint32_t)d->height);
        p.from = p.to;
        p.to = p.to == d->ping ? d->pong : d->ping;
    }
    artfPoolRun(pool, finishRow, &p, (uint32_t)d->height);
    /* what this frame found is what the next one looks back on */
    float* t = d->history;
    d->history = d->nextHistory;
    d->nextHistory = t;
    t = d->previousGuide;
    d->previousGuide = d->guide;
    d->guide = t;
    memcpy(d->viewProj, frame->viewProj, sizeof d->viewProj);
    d->first = false;
}
