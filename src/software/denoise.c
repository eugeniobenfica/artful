/* artful — what is left of noise in a traced frame, taken out on the CPU: the arithmetic the GPU's shaders
 * (artfShaderDenoise) repeat. Each pixel has what its first surface was: where, which way it faced and what color.
 * Light over color is what is noisy, and what is smooth across a surface.
 *
 *   time    a pixel finds where its surface was in the frame before, by how the eye and that object moved, and adds
 *           to what was gathered there; what was hidden starts over. What was gathered must look like this frame's light around
 *           the pixel, one by one and on the whole: when a light or a shadow changed over a surface that stayed,
 *           it does not, and is cut back
 *   space   SVGF's (Schied et al., 2017): how much each pixel's light varies, from the moments gathered with it (or,
 *           with a short history, from those around it), then five passes of a wavelet filter (Dammertz et al., 2010),
 *           each reading twice as far, five taps a side, over what faces the same way at the same distance and has
 *           light near its own, near by as many deviations as it varies: where it is still noisy it is smoothed more,
 *           where it has settled, hardly
 *   change  A-SVGF's temporal gradients (Schied et al., 2018): where the eye stood still, a pixel of each 3 x 3 traced
 *           again with the frame before's first sample says whether the light there changed, in its own noise's
 *           deviations; where it surely did, what was gathered goes toward this frame's light around
 *           (artfDenoiserGradients) */
#include "software/denoise.h"
#include "core/threads.h"

#include <stdlib.h>

#define HISTORY_LIMIT 256.f
#define SVGF_PASSES 5
#define SVGF_LIGHT 4.f      /* how many deviations of a pixel's light another's may be from it, and still count */
#define SVGF_SHORT 4.f      /* a history shorter than this takes its variance from around it */
#define SPREAD 1.25f   /* how many deviations of this frame's light around a pixel what was gathered may be from it */
#define CUT_BACK 8.f   /* what is left of a history that was found out */
#define DRIFT 4.f      /* how many standard errors the mean of a neighbourhood may be from what was gathered there */
/* A-SVGF: a change of fewer deviations than CHANGE_FROM is noise; from CHANGE_TO on, it is sure; between, as sure as
 * that. One square alone must be twice as sure as the 3 x 3 around it. */
#define CHANGE_FROM 2.f
#define CHANGE_TO 6.f
#define CHANGE_ALONE 2.f

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

/* ---- temporal gradients (A-SVGF) ---- */

typedef struct Gradients {
    ArtfDenoiser* d;
    ArtfDenoiserTrace trace;
    void* user;
    uint32_t turn;
} Gradients;

/* (a pixel's luminance of light over its color: what moments and the filter are in) */
static inline float albedoLuma(const float* albedo) {
    return (.2126f * artfMaxf(albedo[0], .02f) + .7152f * artfMaxf(albedo[1], .02f)) + .0722f * artfMaxf(albedo[2], .02f);
}

/* Each square's change and its noise, as light over color: (change, deviation); a deviation below 0 says nothing was
 * found there. */
static void gradientRow(void* user, uint32_t sy, uint32_t worker) {
    (void)worker;
    Gradients* g = user;
    ArtfDenoiser* d = g->d;
    for (int sx = 0; sx < d->squaresX; ++sx) {
        float* out = d->changeRaw + ((size_t)sy * d->squaresX + (size_t)sx) * 2;
        out[0] = 0;
        out[1] = -1.f;
        const int x = artfMini(sx * 3 + (int)(g->turn % 3), d->width - 1), y = artfMini((int)sy * 3 + (int)(g->turn / 3 % 3), d->height - 1);
        const size_t i = (size_t)y * d->width + (size_t)x;
        const float* then = d->previousGuide + i * 4;
        float pn[3];
        if (!normalOf(then, pn)) continue;
        float place[4] = {0, 0, 0, 0};
        const float now = g->trace(g->user, (uint32_t)x, (uint32_t)y, d->firstSampleBefore, place);
        /* (the same surface, where the sample met it: else what is there moved, and says nothing of light) */
        float qn[3];
        const float ex = then[0] - d->eye[0], ey = then[1] - d->eye[1], ez = then[2] - d->eye[2];
        const float tolerance = 1e-4f * sqrtf(ex * ex + ey * ey + ez * ez) + 1e-5f;
        const float dx = place[0] - then[0], dy = place[1] - then[1], dz = place[2] - then[2];
        if (!normalOf(place, qn) || dx * dx + dy * dy + dz * dz > tolerance * tolerance || qn[0] * pn[0] + qn[1] * pn[1] + qn[2] * pn[2] < .99f) continue;
        const float scale = 1.f / albedoLuma(d->albedo + i * 4);
        const float m1 = d->moments[i * 2], m2 = d->moments[i * 2 + 1];
        out[0] = (now - d->firstLightBefore[i]) * scale;
        out[1] = sqrtf(artfMaxf(m2 - m1 * m1, 0.f));
    }
}

/* How surely each square's light changed: its change in its own deviations, halved, or the 3 x 3 squares' around in
 * the mean's, whichever says more; 0 below CHANGE_FROM, 1 from CHANGE_TO. */
static void gradientSureRow(void* user, uint32_t sy, uint32_t worker) {
    (void)worker;
    Gradients* g = user;
    ArtfDenoiser* d = g->d;
    for (int sx = 0; sx < d->squaresX; ++sx) {
        const float* own = d->changeRaw + ((size_t)sy * d->squaresX + (size_t)sx) * 2;
        float sum = 0, variance = 0, n = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int qx = sx + dx, qy = (int)sy + dy;
                if (qx < 0 || qy < 0 || qx >= d->squaresX || qy >= d->squaresY) continue;
                const float* q = d->changeRaw + ((size_t)qy * d->squaresX + (size_t)qx) * 2;
                if (q[1] < 0) continue;
                sum += q[0];
                variance += q[1] * q[1];
                n += 1.f;
            }
        float sure = 0;
        if (own[1] >= 0) sure = fabsf(own[0]) / (own[1] + 1e-3f) / CHANGE_ALONE;
        if (n > 0) sure = artfMaxf(sure, fabsf(sum / n) / (sqrtf(variance) / n + 1e-3f));
        d->change[(size_t)sy * d->squaresX + (size_t)sx] = artfClampf((sure - CHANGE_FROM) / (CHANGE_TO - CHANGE_FROM), 0.f, 1.f);
    }
}

void artfDenoiserGradients(ArtfDenoiser* d, ArtfPool* pool, const ArtfFrameData* frame, uint32_t firstSample, uint32_t frameNumber, ArtfDenoiserTrace trace, void* user) {
    const int squaresX = (d->width + 2) / 3, squaresY = (d->height + 2) / 3;
    if (d->squaresX != squaresX || d->squaresY != squaresY || !d->change) {
        artfFree(d->change);
        artfFree(d->changeRaw);
        d->change = artfCalloc((size_t)squaresX * squaresY + 1, 4);
        d->changeRaw = artfCalloc((size_t)squaresX * squaresY * 2 + 2, 4);
        d->squaresX = squaresX;
        d->squaresY = squaresY;
    }
    /* (only where the eye stood still, and with a frame before: a sample traced again must go the way it went) */
    d->gradients = !d->first && !memcmp(d->viewProj, frame->viewProj, sizeof d->viewProj);
    if (d->gradients) {
        memcpy(d->eye, frame->cameraPos, 12);
        Gradients g = {d, trace, user, frameNumber % 9u};
        artfPoolRun(pool, gradientRow, &g, (uint32_t)squaresY);
        artfPoolRun(pool, gradientSureRow, &g, (uint32_t)squaresY);
    }
    d->firstSampleBefore = firstSample;
}

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
        float count = 1.f, before[3] = {0, 0, 0}, alphaBefore = coverage, momentsBefore[2] = {0, 0};
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
                    if (d->gradients) {
                        /* (where the light surely changed, by the temporal gradients: what was gathered goes toward this
                         * frame's light around, its mean over 5 x 5, by as much, rather than starting over from one
                         * noisy sample; and it is cut back as a light that drifted is) */
                        const float sure = (d->change[(size_t)artfMini((int)y / 3, d->squaresY - 1) * d->squaresX + artfMini(x / 3, d->squaresX - 1)]);
                        if (sure > 0) {
                            for (int k = 0; k < 3; ++k) before[k] += ((low[k] + high[k]) * .5f - before[k]) * sure;
                            count = artfMinf(count, CUT_BACK);
                        }
                    }
                    alphaBefore = h[3] - whole;
                    momentsBefore[0] = d->moments[j * 2];
                    momentsBefore[1] = d->moments[j * 2 + 1];
                }
            }
        }
        const float share = 1.f / count;
        for (int k = 0; k < 3; ++k) out[k] = before[k] + (light[k] - before[k]) * share;
        out[3] = count + artfMinf(alphaBefore + (coverage - alphaBefore) * share, .999f);
        /* (the luminance's moments, gathered the same way: count 1 starts them over) */
        const float l = (.2126f * light[0] + .7152f * light[1]) + .0722f * light[2];
        d->nextMoments[i * 2] = momentsBefore[0] + (l - momentsBefore[0]) * share;
        d->nextMoments[i * 2 + 1] = momentsBefore[1] + (l * l - momentsBefore[1]) * share;
    }
}

/* (where a guide says a pixel is, along the eye, and its normal: 0 when nothing is there) */
static inline float placeOf(const ArtfDenoiser* d, size_t i, float* n) {
    const float* guide = d->guide + i * 4;
    if (!normalOf(guide, n)) return 0.f;
    const float ex = guide[0] - d->eye[0], ey = guide[1] - d->eye[1], ez = guide[2] - d->eye[2];
    return sqrtf(ex * ex + ey * ey + ez * ez);
}

/* What a neighbour's place and facing are worth: its normal's cosine to the 128th (multiplied out), and 1 / (1 + its
 * distance's difference over 2% of this one's, squared). */
static inline float placeWeight(const float* n, float distance, const float* qn, float qDistance) {
    const float reach = distance * .02f + 1e-3f, apart = (qDistance - distance) / reach;
    float facing = artfMaxf((n[0] * qn[0] + n[1] * qn[1]) + n[2] * qn[2], 0.f);
    for (int k = 0; k < 7; ++k) facing *= facing;
    return facing / (1.f + apart * apart);
}

/* How much each pixel's light varies: its moments' variance; with a short history, that of the moments around it
 * (5 x 5, by place and facing), made larger by how short it is, as SVGF does. */
static void varianceRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    Pass* p = user;
    ArtfDenoiser* d = p->d;
    const int W = d->width, H = d->height;
    for (int x = 0; x < W; ++x) {
        const size_t i = (size_t)y * W + (size_t)x;
        float n[3];
        const float distance = placeOf(d, i, n), count = floorf(d->nextHistory[i * 4 + 3]);
        if (distance <= 0) {
            d->variance[i] = 0;
            continue;
        }
        if (count >= SVGF_SHORT) {
            d->variance[i] = artfMaxf(d->nextMoments[i * 2 + 1] - d->nextMoments[i * 2] * d->nextMoments[i * 2], 0.f);
            continue;
        }
        float m1 = 0, m2 = 0, total = 0;
        for (int dy = -2; dy <= 2; ++dy) {
            const int qy = (int)y + dy;
            if (qy < 0 || qy >= H) continue;
            for (int dx = -2; dx <= 2; ++dx) {
                const int qx = x + dx;
                if (qx < 0 || qx >= W) continue;
                const size_t j = (size_t)qy * W + (size_t)qx;
                float qn[3];
                const float qDistance = placeOf(d, j, qn);
                if (qDistance <= 0) continue;
                const float w = placeWeight(n, distance, qn, qDistance);
                m1 += d->nextMoments[j * 2] * w;
                m2 += d->nextMoments[j * 2 + 1] * w;
                total += w;
            }
        }
        const float inverse = total > 0 ? 1.f / total : 0.f;
        m1 *= inverse;
        m2 *= inverse;
        d->variance[i] = artfMaxf(m2 - m1 * m1, 0.f) * (SVGF_SHORT / artfMaxf(count, 1.f));
    }
}

/* A pass of the filter, step apart: SVGF's weights (place, facing, light by its deviation), and its variance carried
 * through, as the sum of each weight squared times what it took, over the weights' sum squared. */
static void spatialRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    static const float kernel[3] = {.375f, .25f, .0625f}, blur[2] = {.25f, .125f};
    Pass* p = user;
    ArtfDenoiser* d = p->d;
    const int W = d->width, H = d->height, step = p->step;
    for (int x = 0; x < W; ++x) {
        const size_t i = (size_t)y * W + (size_t)x;
        const float* center = p->from + i * 4;
        float* out = p->to + i * 4;
        float n[3];
        const float distance = placeOf(d, i, n);
        if (distance <= 0) {
            memcpy(out, center, 16);
            d->varianceNext[i] = d->variance[i];
            continue;
        }
        /* (the variance around, 3 x 3 by a small Gaussian: one pixel's alone is too noisy to judge by) */
        float around = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int qx = artfClampi(x + dx, 0, W - 1), qy = artfClampi((int)y + dy, 0, H - 1);
                around += d->variance[(size_t)qy * W + (size_t)qx] * (dx == 0 && dy == 0 ? blur[0] : dx == 0 || dy == 0 ? blur[1] : .0625f);
            }
        const float luma = (.2126f * center[0] + .7152f * center[1]) + .0722f * center[2];
        const float scale = 1.f / (SVGF_LIGHT * sqrtf(artfMaxf(around, 0.f)) + 1e-10f);
        float sum[3] = {0, 0, 0}, total = 0, varianceSum = 0;
        for (int dy = -2; dy <= 2; ++dy) {
            const int qy = (int)y + dy * step;
            if (qy < 0 || qy >= H) continue;
            for (int dx = -2; dx <= 2; ++dx) {
                const int qx = x + dx * step;
                if (qx < 0 || qx >= W) continue;
                const size_t j = (size_t)qy * W + (size_t)qx;
                float qn[3];
                const float qDistance = placeOf(d, j, qn);
                if (qDistance <= 0) continue;
                const float* q = p->from + j * 4;
                const float ql = (.2126f * q[0] + .7152f * q[1]) + .0722f * q[2];
                const float weight = kernel[abs(dx)] * kernel[abs(dy)] * placeWeight(n, distance, qn, qDistance) * artfExp2Fast(-fabsf(ql - luma) * scale * 1.44269504f);
                for (int k = 0; k < 3; ++k) sum[k] += q[k] * weight;
                total += weight;
                varianceSum += weight * weight * d->variance[j];
            }
        }
        const float inverse = total > 0 ? 1.f / total : 0.f;
        for (int k = 0; k < 3; ++k) out[k] = sum[k] * inverse;
        out[3] = center[3];
        d->varianceNext[i] = varianceSum * inverse * inverse;
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
    d->moments = artfCalloc((size_t)width * height * 2 + 2, 4);
    d->nextMoments = artfCalloc((size_t)width * height * 2 + 2, 4);
    d->variance = artfCalloc((size_t)width * height + 1, 4);
    d->varianceNext = artfCalloc((size_t)width * height + 1, 4);
    d->firstLight = artfCalloc((size_t)width * height + 1, 4);
    d->firstLightBefore = artfCalloc((size_t)width * height + 1, 4);
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
    artfFree(d->moments);
    artfFree(d->nextMoments);
    artfFree(d->variance);
    artfFree(d->varianceNext);
    artfFree(d->firstLight);
    artfFree(d->firstLightBefore);
    artfFree(d->change);
    artfFree(d->changeRaw);
    memset(d, 0, sizeof *d);
}

void artfDenoiserRun(ArtfDenoiser* d, ArtfPool* pool, const ArtfFrameData* frame, uint32_t samples, const float* motion, size_t motionCount) {
    memcpy(d->eye, frame->cameraPos, 12);
    Pass p = {d, frame, d->viewProj, samples, 1, NULL, NULL, d->first, motion, motionCount};
    artfPoolRun(pool, lightRow, &p, (uint32_t)d->height);
    artfPoolRun(pool, temporalRow, &p, (uint32_t)d->height);
    artfPoolRun(pool, varianceRow, &p, (uint32_t)d->height);
    p.from = d->nextHistory;
    p.to = d->ping;
    for (int i = 0; i < SVGF_PASSES; ++i) {
        p.step = 1 << i;
        artfPoolRun(pool, spatialRow, &p, (uint32_t)d->height);
        p.from = p.to;
        p.to = p.to == d->ping ? d->pong : d->ping;
        float* v = d->variance;
        d->variance = d->varianceNext;
        d->varianceNext = v;
    }
    artfPoolRun(pool, finishRow, &p, (uint32_t)d->height);
    /* what this frame found is what the next one looks back on */
    float* t = d->history;
    d->history = d->nextHistory;
    d->nextHistory = t;
    t = d->previousGuide;
    d->previousGuide = d->guide;
    d->guide = t;
    t = d->moments;
    d->moments = d->nextMoments;
    d->nextMoments = t;
    memcpy(d->viewProj, frame->viewProj, sizeof d->viewProj);
    d->first = false;
    /* (this frame's first samples are what the next one's gradients look back on) */
    t = d->firstLightBefore;
    d->firstLightBefore = d->firstLight;
    d->firstLight = t;
    d->gradients = false;
}
