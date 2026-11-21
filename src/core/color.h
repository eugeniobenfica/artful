/* artful — log2 and 2^x by bits and polynomials, and what a screen shows of light (sRGB) by them: the same on every
 * processor, and in the shaders, which write the same arithmetic (shaders.c's log2Of, exp2Of, srgbDecodeOf...; the
 * kernels' lanes, kernels.inc). No pow, exp or log of a C library: those give other bytes on other systems. */
#ifndef ARTFUL_COLOR_H
#define ARTFUL_COLOR_H

#include "artful/core.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* log2 of x > 0, and 2^x for x within +-100: within a few millionths. */
ARTF_INLINE float artfLog2Fast(float x) {
    uint32_t bits;
    memcpy(&bits, &x, 4);
    const float e = (float)(int32_t)((bits >> 23) & 255) - 127.f;
    bits = (bits & 0x7FFFFFu) | 0x3F800000u;
    float m;
    memcpy(&m, &bits, 4);
    const float z = (m - 1.f) / (m + 1.f), z2 = z * z; /* log2 m = 2 / ln 2 atanh z */
    return e + z * 2.8853901f * (1.f + z2 * (.33333333f + z2 * (.2f + z2 * .14285714f)));
}
ARTF_INLINE float artfExp2Fast(float x) {
    x = x < -100.f ? -100.f : x > 100.f ? 100.f : x;
    const int32_t i = (int32_t)(x + 128.f) - 128; /* floor */
    const float u = (x - (float)i) * .69314718f;
    const float f = 1.f + u * (1.f + u * (.5f + u * (.16666667f + u * (.041666668f + u * (.0083333333f + u * .0013888889f)))));
    const uint32_t bits = (uint32_t)(i + 127) << 23;
    float scale;
    memcpy(&scale, &bits, 4);
    return f * scale;
}

/* sRGB (IEC 61966-2-1): what a byte of a picture means as light, and back. Shown is 0 to 1; light below 0 is 0
 * (callers clamp where they write bytes). Each is a polynomial fitted to the curve where it is a power (in the shown
 * value; in the fourth root of light), within 8e-6 of it: a byte goes to light and back to itself. The same
 * polynomials in the same order run eight lanes at a time (kernels.inc's srgbDecodeLanes) and in the shaders
 * (spvSrgbDecode, spirv.c), with no table and no pow: the same bytes on every processor. */
#define ARTF_SRGB_DECODE {0.000919755024f, 0.0329489373f, 0.513254464f, 0.710048199f, -0.421929181f, 0.215906158f, -0.0511523038f}
#define ARTF_SRGB_ENCODE {-0.0614833347f, 0.163495496f, 1.24977994f, -0.56738776f, 0.28097105f, -0.0653806403f}
ARTF_INLINE float artfSrgbDecode(float shown) {
    if (shown <= .04045f) return shown * (1.f / 12.92f);
    static const float c[7] = ARTF_SRGB_DECODE;
    float p = c[6];
    for (int k = 5; k >= 0; --k) p = p * shown + c[k];
    return p;
}
ARTF_INLINE float artfSrgbEncode(float light) {
    if (light <= .0031308f) return light > 0 ? light * 12.92f : 0.f;
    static const float c[6] = ARTF_SRGB_ENCODE;
    const float t = sqrtf(sqrtf(light));
    float p = c[5];
    for (int k = 4; k >= 0; --k) p = p * t + c[k];
    return p;
}

/* From light as sRGB shows it (Rec. 709's primaries, 0 to 1) to what a screen takes, 0 to 1 (ArtfOutput, as ARTF_DISPLAY_):
 * Display-P3's primaries and sRGB's curve; or Rec. 2020's and PQ, light of 1 being peak (nits / 10000); or scRGB,
 * light itself in Rec. 709's primaries, 1 being 80 nits (peak: nits / 80). The GPU's (shaders.c's displaySpv) does the
 * same arithmetic in the same order. */
enum { ARTF_DISPLAY_SRGB, ARTF_DISPLAY_P3, ARTF_DISPLAY_HDR10, ARTF_DISPLAY_SCRGB };
ARTF_INLINE float artfPq(float y) {
    if (!(y > 0)) return 0.f;
    const float p = artfExp2Fast(artfLog2Fast(artfMinf(y, 1.f)) * .1593017578125f);
    return artfExp2Fast(artfLog2Fast((.8359375f + 18.8515625f * p) / (1.f + 18.6875f * p)) * 78.84375f);
}
ARTF_INLINE void artfDisplayEncode(int kind, float peak, const float* light, float* out) {
    static const float p3[3][3] = {{.82246197f, .17753803f, 0.f}, {.03319420f, .96680580f, 0.f}, {.01708263f, .07239744f, .91051993f}};
    static const float bt2020[3][3] = {{.62740390f, .32928304f, .04331307f}, {.06909729f, .91954040f, .01136232f}, {.01639144f, .08801331f, .89559525f}};
    if (kind == ARTF_DISPLAY_SCRGB) {
        for (int c = 0; c < 3; ++c) out[c] = artfMaxf(light[c], 0.f) * peak;
        return;
    }
    const float(*m)[3] = kind == ARTF_DISPLAY_HDR10 ? bt2020 : p3;
    for (int c = 0; c < 3; ++c) {
        const float v = (m[c][0] * light[0] + m[c][1] * light[1]) + m[c][2] * light[2];
        out[c] = kind == ARTF_DISPLAY_HDR10 ? artfPq(artfMaxf(v, 0.f) * peak) : artfSrgbEncode(artfClampf(v, 0.f, 1.f));
    }
}

/* A float as a half, rounded to the nearest (ties to even), as a GPU writes one to a float16 target; past the largest
 * half, the largest; not a number: 0. */
ARTF_INLINE uint16_t artfHalfOf(float f) {
    if (!(f == f)) return 0;
    uint32_t b;
    memcpy(&b, &f, 4);
    const uint32_t sign = (b >> 16) & 0x8000u, rest = b & 0x7FFFFFFFu;
    if (rest >= 0x477FF000u) return (uint16_t)(sign | 0x7BFFu); /* (65520 and over: would round past the largest) */
    if (rest < 0x38800000u) {                                     /* (below the smallest normal half: a subnormal) */
        if (rest < 0x33000000u) return (uint16_t)sign;
        const uint32_t man = (rest & 0x7FFFFFu) | 0x800000u, shift = 126u - (rest >> 23);
        const uint32_t h = man >> shift, left = man & ((1u << shift) - 1u), mid = 1u << (shift - 1u);
        return (uint16_t)(sign | (h + (left > mid || (left == mid && (h & 1u)))));
    }
    const uint32_t h = ((rest >> 13) - 0x1C000u), left = rest & 0x1FFFu;
    return (uint16_t)(sign | (h + (left > 0x1000u || (left == 0x1000u && (h & 1u)))));
}

#endif
