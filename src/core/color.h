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

#endif
