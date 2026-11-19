/* FLIP in C (flip.h): the same steps, constants and order as NVIDIA's FLIP.h. */
#include "flip.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265358979f

/* FLIP's constants */
static const float qc = .7f, pc = .4f, pt = .95f, gw = .082f, qf = .5f;
static const float illuminant[3] = {.950428545f, 1.f, 1.088900371f};
static const float invIlluminant[3] = {1.052156925f, 1.f, .918357670f};
/* the eye's contrast sensitivity, per channel of YCxCz: a1 sqrt(pi / b1) e^(-pi^2 x^2 / b1) + the same of a2, b2 */
static const float a1[3] = {1.f, 1.f, 34.1f}, b1[3] = {.0047f, .0053f, .04f};
static const float a2[3] = {0.f, 0.f, 13.5f}, b2[3] = {1e-5f, 1e-5f, .025f};

float flipDefaultPpd(void) { return .7f * (3840.f / .7f) * (PI_F / 180.f); }

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static void rgbToXyz(const float* c, float* out) {
    const float r = c[0], g = c[1], b = c[2];
    out[0] = (10135552.f / 24577794.f) * r + (8788810.f / 24577794.f) * g + (4435075.f / 24577794.f) * b;
    out[1] = (2613072.f / 12288897.f) * r + (8788810.f / 12288897.f) * g + (887015.f / 12288897.f) * b;
    out[2] = (1425312.f / 73733382.f) * r + (8788810.f / 73733382.f) * g + (70074185.f / 73733382.f) * b;
}

static void xyzToRgb(const float* c, float* out) {
    const float x = c[0], y = c[1], z = c[2];
    out[0] = 3.241003275f * x - 1.537398934f * y - .498615861f * z;
    out[1] = -.969224334f * x + 1.875930071f * y + .041554224f * z;
    out[2] = .055639423f * x - .204011202f * y + 1.057148933f * z;
}

static void xyzToYcxcz(const float* c, float* out) {
    const float x = c[0] * invIlluminant[0], y = c[1] * invIlluminant[1], z = c[2] * invIlluminant[2];
    out[0] = 116.f * y - 16.f;
    out[1] = 500.f * (x - y);
    out[2] = 200.f * (y - z);
}

static void ycxczToXyz(const float* c, float* out) {
    const float y = (c[0] + 16.f) / 116.f;
    out[0] = (y + c[1] / 500.f) * illuminant[0];
    out[1] = y * illuminant[1];
    out[2] = (y - c[2] / 200.f) * illuminant[2];
}

static float labF(float t) {
    const float delta = 6.f / 29.f;
    return t > delta * delta * delta ? powf(t, 1.f / 3.f) : t / (3.f * delta * delta) + 4.f / 29.f;
}

static void xyzToLab(const float* c, float* out) {
    const float x = labF(c[0] * invIlluminant[0]), y = labF(c[1] * invIlluminant[1]), z = labF(c[2] * invIlluminant[2]);
    out[0] = 116.f * y - 16.f;
    out[1] = 500.f * (x - y);
    out[2] = 200.f * (y - z);
}

/* Lab with its chroma scaled by lightness (Hunt's effect) */
static void huntLab(const float* rgb, float* out) {
    float xyz[3];
    rgbToXyz(rgb, xyz);
    xyzToLab(xyz, out);
    out[1] *= .01f * out[0];
    out[2] *= .01f * out[0];
}

static float hyab(const float* a, const float* b) {
    const float da = a[1] - b[1], db = a[2] - b[2];
    return fabsf(a[0] - b[0]) + sqrtf(da * da + db * db);
}

static float cmaxOf(void) {
    const float green[3] = {0, 1, 0}, blue[3] = {0, 0, 1};
    float g[3], b[3];
    huntLab(green, g);
    huntLab(blue, b);
    return powf(hyab(g, b), qc);
}

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static float gaussianOf(float x2, float a, float b) { return a * sqrtf(PI_F / b) * expf(-PI_F * PI_F * x2 / b); }
static float gaussianSqrtOf(float x2, float a, float b) { return sqrtf(a * sqrtf(PI_F / b)) * expf(-PI_F * PI_F * x2 / b); }

static void toYcxcz(const float* rgb, int n, float* out) {
    for (int i = 0; i < n; ++i) {
        float c[3] = {clamp01(rgb[i * 3]), clamp01(rgb[i * 3 + 1]), clamp01(rgb[i * 3 + 2])}, xyz[3];
        rgbToXyz(c, xyz);
        xyzToYcxcz(xyz, out + i * 3);
    }
}

/* What the eye makes of each picture's color, filtered, then how far apart in Lab: per pixel, 0 to about 1. */
static void colorDifference(const float* ycc[2], int w, int h, float ppd, float* out) {
    const float pi2 = PI_F * PI_F;
    float maxB = 0;
    for (int c = 0; c < 3; ++c) {
        if (b1[c] > maxB) maxB = b1[c];
        if (b2[c] > maxB) maxB = b2[c];
    }
    const int radius = (int)ceilf(3.f * sqrtf(maxB / (2.f * pi2)) * ppd), width = 2 * radius + 1;
    float* k = malloc(sizeof(float) * 4 * (size_t)width); /* Y, Cx, Cz's two */
    float sumY = 0, sumCx = 0, sumCz1 = 0, sumCz2 = 0;
    for (int x = 0; x < width; ++x) {
        const float ix = (float)(x - radius) / ppd, ix2 = ix * ix;
        k[x * 4] = gaussianOf(ix2, a1[0], b1[0]);
        k[x * 4 + 1] = gaussianOf(ix2, a1[1], b1[1]);
        k[x * 4 + 2] = gaussianSqrtOf(ix2, a1[2], b1[2]);
        k[x * 4 + 3] = gaussianSqrtOf(ix2, a2[2], b2[2]);
        sumY += k[x * 4];
        sumCx += k[x * 4 + 1];
        sumCz1 += k[x * 4 + 2];
        sumCz2 += k[x * 4 + 3];
    }
    const float normCz = 1.f / sqrtf(sumCz1 * sumCz1 + sumCz2 * sumCz2);
    for (int x = 0; x < width; ++x) {
        k[x * 4] /= sumY;
        k[x * 4 + 1] /= sumCx;
        k[x * 4 + 2] *= normCz;
        k[x * 4 + 3] *= normCz;
    }
    const size_t n = (size_t)w * h;
    float* across = malloc(sizeof(float) * 4 * n);
    float* lab[2] = {malloc(sizeof(float) * 3 * n), malloc(sizeof(float) * 3 * n)};
    for (int im = 0; im < 2; ++im) {
        const float* src = ycc[im];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float s[4] = {0, 0, 0, 0};
                for (int i = -radius; i <= radius; ++i) {
                    const float* p = src + ((size_t)y * w + clampi(x + i, 0, w - 1)) * 3;
                    const float* q = k + (i + radius) * 4;
                    s[0] += q[0] * p[0];
                    s[1] += q[1] * p[1];
                    s[2] += q[2] * p[2];
                    s[3] += q[3] * p[2];
                }
                memcpy(across + ((size_t)y * w + x) * 4, s, sizeof s);
            }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float s[4] = {0, 0, 0, 0};
                for (int i = -radius; i <= radius; ++i) {
                    const float* p = across + ((size_t)clampi(y + i, 0, h - 1) * w + x) * 4;
                    const float* q = k + (i + radius) * 4;
                    s[0] += q[0] * p[0];
                    s[1] += q[1] * p[1];
                    s[2] += q[2] * p[2];
                    s[3] += q[3] * p[3];
                }
                const float filtered[3] = {s[0], s[1], s[2] + s[3]};
                float xyz[3], rgb[3];
                ycxczToXyz(filtered, xyz);
                xyzToRgb(xyz, rgb);
                for (int c = 0; c < 3; ++c) rgb[c] = clamp01(rgb[c]);
                huntLab(rgb, lab[im] + ((size_t)y * w + x) * 3);
            }
    }
    const float cmax = cmaxOf(), pccmax = pc * cmax;
    for (size_t i = 0; i < n; ++i) {
        float d = powf(hyab(lab[0] + i * 3, lab[1] + i * 3), qc);
        d = d < pccmax ? d * (pt / pccmax) : pt + ((d - pccmax) / (cmax - pccmax)) * (1.f - pt);
        out[i] = d;
    }
    free(k);
    free(across);
    free(lab[0]);
    free(lab[1]);
}

/* Edges and points that one picture has and the other has not, in lightness: per pixel, 0 to 1, as an exponent. */
static void featureDifference(const float* ycc[2], int w, int h, float ppd, float* out) {
    const float sd = .5f * gw * ppd;
    const int radius = (int)ceilf(3.f * sd), width = 2 * radius + 1;
    float* k = malloc(sizeof(float) * 3 * (size_t)width); /* the Gaussian, its first and its second derivative */
    float gSum = 0, dPos = 0, dNeg = 0, ddPos = 0, ddNeg = 0;
    for (int x = 0; x < width; ++x) {
        const float xx = (float)(x - radius);
        const float g = expf(-(xx * xx) / (2.f * sd * sd)), dg = -xx * g, ddg = (xx * xx / (sd * sd) - 1.f) * g;
        gSum += g;
        if (dg > 0) dPos += dg; else dNeg -= dg;
        if (ddg > 0) ddPos += ddg; else ddNeg -= ddg;
        k[x * 3] = g;
        k[x * 3 + 1] = dg;
        k[x * 3 + 2] = ddg;
    }
    for (int x = 0; x < width; ++x) {
        k[x * 3] /= gSum;
        k[x * 3 + 1] /= k[x * 3 + 1] > 0 ? dPos : dNeg;
        k[x * 3 + 2] /= k[x * 3 + 2] > 0 ? ddPos : ddNeg;
    }
    const size_t n = (size_t)w * h;
    float* across = malloc(sizeof(float) * 6 * n); /* for each picture: d/dx, d2/dx2 and the Gaussian, across */
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s[6] = {0, 0, 0, 0, 0, 0};
            for (int i = -radius; i <= radius; ++i) {
                const size_t at = (size_t)y * w + clampi(x + i, 0, w - 1);
                const float* q = k + (i + radius) * 3;
                for (int im = 0; im < 2; ++im) {
                    const float yn = ycc[im][at * 3] * (1.f / 116.f) + 16.f / 116.f;
                    s[im * 3] += q[1] * yn;
                    s[im * 3 + 1] += q[2] * yn;
                    s[im * 3 + 2] += q[0] * yn;
                }
            }
            memcpy(across + ((size_t)y * w + x) * 6, s, sizeof s);
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float dx[2] = {0, 0}, ddx[2] = {0, 0}, dy[2] = {0, 0}, ddy[2] = {0, 0};
            for (int i = -radius; i <= radius; ++i) {
                const float* p = across + ((size_t)clampi(y + i, 0, h - 1) * w + x) * 6;
                const float* q = k + (i + radius) * 3;
                for (int im = 0; im < 2; ++im) {
                    dx[im] += q[0] * p[im * 3];
                    ddx[im] += q[0] * p[im * 3 + 1];
                    dy[im] += q[1] * p[im * 3 + 2];
                    ddy[im] += q[2] * p[im * 3 + 2];
                }
            }
            const float edge = fabsf(sqrtf(dx[0] * dx[0] + dy[0] * dy[0]) - sqrtf(dx[1] * dx[1] + dy[1] * dy[1]));
            const float point = fabsf(sqrtf(ddx[0] * ddx[0] + ddy[0] * ddy[0]) - sqrtf(ddx[1] * ddx[1] + ddy[1] * ddy[1]));
            out[(size_t)y * w + x] = powf((1.f / sqrtf(2.f)) * (edge > point ? edge : point), qf);
        }
    free(k);
    free(across);
}

static int byFloat(const void* a, const void* b) {
    const float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

static FlipStats statsOf(const float* map, size_t n) {
    FlipStats s = {0, 0, 0};
    if (!n) return s;
    float* sorted = malloc(sizeof(float) * n);
    memcpy(sorted, map, sizeof(float) * n);
    qsort(sorted, n, sizeof(float), byFloat);
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += sorted[i];
    s.mean = sum / (double)n;
    s.p99 = sorted[(size_t)((double)(n - 1) * .99)];
    s.max = sorted[n - 1];
    free(sorted);
    return s;
}

/* LDR-FLIP into map (which must be there) */
static void ldrInto(const float* reference, const float* test, int w, int h, float ppd, float* map) {
    const size_t n = (size_t)w * h;
    float* ycc[2] = {malloc(sizeof(float) * 3 * n), malloc(sizeof(float) * 3 * n)};
    toYcxcz(reference, (int)n, ycc[0]);
    toYcxcz(test, (int)n, ycc[1]);
    float* features = malloc(sizeof(float) * n);
    const float* both[2] = {ycc[0], ycc[1]};
    colorDifference(both, w, h, ppd, map);
    featureDifference(both, w, h, ppd, features);
    for (size_t i = 0; i < n; ++i) map[i] = powf(map[i], 1.f - features[i]);
    free(features);
    free(ycc[0]);
    free(ycc[1]);
}

FlipStats flipLinear(const float* reference, const float* test, int w, int h, float ppd, float* map) {
    const size_t n = (size_t)w * h;
    if (ppd <= 0) ppd = flipDefaultPpd();
    float* own = map ? map : malloc(sizeof(float) * n);
    ldrInto(reference, test, w, h, ppd, own);
    const FlipStats s = statsOf(own, n);
    if (!map) free(own);
    return s;
}

/* FLIP's ACES fit: (c^2 t0 + c t1 + t2) / (c^2 t3 + c t4 + t5) */
static const float aces[6] = {.6f * .6f * 2.51f, .6f * .03f, 0.f, .6f * .6f * 2.43f, .6f * .59f, .14f};

static float acesOf(float c) { return (c * c * aces[0] + c * aces[1] + aces[2]) / (c * c * aces[3] + c * aces[4] + aces[5]); }

FlipStats flipHdr(const float* reference, const float* test, int w, int h, float ppd, float* map) {
    const size_t n = (size_t)w * h;
    if (ppd <= 0) ppd = flipDefaultPpd();
    /* where the curve reaches .85: the larger root of (t0 - .85 t3) x^2 + (t1 - .85 t4) x + (t2 - .85 t5) */
    const float t = .85f, a = aces[0] - t * aces[3], b = aces[1] - t * aces[4], c = aces[2] - t * aces[5];
    float xMax;
    if (a == 0) xMax = -c / b;
    else {
        const float d1 = -.5f * (b / a), d2 = sqrtf(d1 * d1 - c / a);
        xMax = d1 + d2;
    }
    float* luminance = malloc(sizeof(float) * n);
    float yMax = -1e30f;
    for (size_t i = 0; i < n; ++i) {
        luminance[i] = .2126f * reference[i * 3] + .7152f * reference[i * 3 + 1] + .0722f * reference[i * 3 + 2];
        if (luminance[i] > yMax) yMax = luminance[i];
    }
    qsort(luminance, n, sizeof(float), byFloat);
    float median = luminance[n / 2];
    if (median < FLT_EPSILON) median = FLT_EPSILON;
    free(luminance);
    if (yMax < FLT_EPSILON) yMax = FLT_EPSILON; /* (FLIP.h has no guard: an all-black reference) */
    const float start = log2f(xMax / yMax), stop = log2f(xMax / median);
    int count = (int)ceilf(stop - start);
    if (count < 2) count = 2;
    const float step = (stop - start) / (float)(count - 1);
    float* worst = map ? map : malloc(sizeof(float) * n);
    float* one = malloc(sizeof(float) * n);
    float* exposed[2] = {malloc(sizeof(float) * 3 * n), malloc(sizeof(float) * 3 * n)};
    for (size_t i = 0; i < n; ++i) worst[i] = 0;
    for (int e = 0; e < count; ++e) {
        const float m = powf(2.f, start + (float)e * step);
        for (size_t i = 0; i < 3 * n; ++i) {
            exposed[0][i] = clamp01(acesOf(reference[i] * m));
            exposed[1][i] = clamp01(acesOf(test[i] * m));
        }
        ldrInto(exposed[0], exposed[1], w, h, ppd, one);
        for (size_t i = 0; i < n; ++i)
            if (one[i] > worst[i]) worst[i] = one[i];
    }
    const FlipStats s = statsOf(worst, n);
    free(one);
    free(exposed[0]);
    free(exposed[1]);
    if (!map) free(worst);
    return s;
}

FlipStats flipFrames(const uint32_t* reference, const uint32_t* test, int w, int h, float ppd, float* map) {
    float table[256];
    for (int i = 0; i < 256; ++i) {
        const float s = (float)i / 255.f;
        table[i] = s <= .04045f ? s / 12.92f : powf((s + .055f) / 1.055f, 2.4f);
    }
    const size_t n = (size_t)w * h;
    float* lin[2] = {malloc(sizeof(float) * 3 * n), malloc(sizeof(float) * 3 * n)};
    for (int im = 0; im < 2; ++im) {
        const uint32_t* p = im ? test : reference;
        for (size_t i = 0; i < n; ++i) {
            lin[im][i * 3] = table[p[i] >> 16 & 255];
            lin[im][i * 3 + 1] = table[p[i] >> 8 & 255];
            lin[im][i * 3 + 2] = table[p[i] & 255];
        }
    }
    const FlipStats s = flipLinear(lin[0], lin[1], w, h, ppd, map);
    free(lin[0]);
    free(lin[1]);
    return s;
}
