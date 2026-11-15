/* artful — sin, cos and tan of a float, for what is worked out once (a sphere's vertices, a camera, a spot's cone),
 * where artfSinFast's 4e-6 would show. The C library's give other bytes on other systems: MinGW's sinf and cosf are a
 * step away from the nearest float for about one float in 13,000 (tanf one in 80,000), and Android's and Apple's are
 * their own. These are additions, multiplications and a division in doubles (fdlibm's polynomials, after taking away
 * the nearest multiple of pi / 2 in three parts), rounded once to a float: the same bytes on every processor, as long
 * as nothing fuses a*b+c (the build says -ffp-contract=off). For every float from -32 to 32 they give the float nearest
 * the true value, as a long double's sinl, cosl and tanl rounded to a float do. Not for a pixel's loop. */
#ifndef ARTFUL_TRIG_H
#define ARTFUL_TRIG_H

#include <stdint.h>

/* x less the nearest multiple k of pi / 2, and k's last two bits: exact enough for |x| up to about 2^20 (past that,
 * the same bytes everywhere, but no longer accurate). */
static inline double artfTrigReduce(float angle, int* quadrant) {
    const double x = angle;
    const double k = (double)(int64_t)(x * 6.36619772367581382433e-01 + (x < 0 ? -.5 : .5));
    /* (pi / 2 in three parts, each short enough that k times it is exact while k is) */
    const double r = ((x - k * 1.57079632673412561417e+00) - k * 6.07710050630396597660e-11) - k * 2.02226624871116645580e-21;
    *quadrant = (int)((int64_t)k & 3);
    return r;
}
/* sin and cos on [-pi / 4, pi / 4], as fdlibm's __kernel_sin and __kernel_cos (with no tail) */
static inline double artfTrigSin(double x) {
    const double z = x * x, v = z * x;
    const double r = 8.33333333332248946124e-03 + z * (-1.98412698298579493134e-04 + z * (2.75573137070700676789e-06 + z * (-2.50507602534068634195e-08 + z * 1.58969099521155010221e-10)));
    return x + v * (-1.66666666666666324348e-01 + z * r);
}
static inline double artfTrigCos(double x) {
    const double z = x * x;
    const double r = z * (4.16666666666666019037e-02 + z * (-1.38888888888741095749e-03 + z * (2.48015872894767294178e-05 + z * (-2.75573143513906633035e-07 + z * (2.08757232129817482790e-09 + z * -1.13596475577881948265e-11)))));
    const double hz = .5 * z, w = 1. - hz;
    return w + (((1. - w) - hz) + z * r);
}

static inline float artfSin(float angle) {
    if (angle == 0) return angle; /* (-0 as well) */
    int q;
    const double r = artfTrigReduce(angle, &q);
    const double s = q & 1 ? artfTrigCos(r) : artfTrigSin(r);
    return (float)(q & 2 ? -s : s);
}
static inline float artfCos(float angle) {
    int q;
    const double r = artfTrigReduce(angle, &q);
    const double c = q & 1 ? artfTrigSin(r) : artfTrigCos(r);
    return (float)((q + 1) & 2 ? -c : c);
}
static inline float artfTan(float angle) {
    if (angle == 0) return angle;
    int q;
    const double r = artfTrigReduce(angle, &q);
    const double s = artfTrigSin(r), c = artfTrigCos(r);
    return (float)(q & 1 ? -c / s : s / c);
}

#endif
