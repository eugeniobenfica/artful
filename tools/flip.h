/* How far apart two pictures look: FLIP (Andersson, Nilsson, Akenine-Möller, Oskarsson, Åström and Fairchild,
 * "FLIP: A Difference Evaluator for Alternating Images", 2020; HDR-FLIP, 2021), written again in C from NVIDIA's
 * reference (github.com/NVlabs/flip, FLIP.h, BSD-3-Clause), for the tests and artful-flip. Not part of the library:
 * it asks the C library for powf and expf, as nothing drawn may.
 *
 * Each pixel gets an error from 0 (alike) to about 1 (as far apart as green from blue), from color, as an eye sees
 * it at ppd pixels a degree, and from edges and points that one has and the other has not. */
#ifndef ARTFUL_FLIP_H
#define ARTFUL_FLIP_H

#include <stdint.h>

typedef struct FlipStats {
    double mean, p99, max;
} FlipStats;

/* FLIP's own viewing: 0.7 m from a monitor 0.7 m wide of 3840 pixels, about 67 pixels a degree. */
float flipDefaultPpd(void);

/* Linear RGB, three floats a pixel, w * h of them; LDR-FLIP clamps them to 0..1. map: w * h errors, or NULL.
 * ppd: 0 for the default. */
FlipStats flipLinear(const float* reference, const float* test, int w, int h, float ppd, float* map);

/* The same, for pictures in light past 1: the most of LDR-FLIP over exposures from where the reference's brightest
 * pixel to where its median reaches the top of an ACES-like curve, as HDR-FLIP does. */
FlipStats flipHdr(const float* reference, const float* test, int w, int h, float ppd, float* map);

/* Bytes as a screen shows them: BGRA (alpha left out), sRGB. */
FlipStats flipFrames(const uint32_t* reference, const uint32_t* test, int w, int h, float ppd, float* map);

#endif
