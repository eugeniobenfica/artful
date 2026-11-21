/* artful — taking the noise out of traced frames, on the CPU (denoise.c). */
#ifndef ARTFUL_DENOISE_H
#define ARTFUL_DENOISE_H

#include "scene/sceneInternal.h"
#include "core/threads.h"

/* Four floats a pixel in each. noisy: this frame's light, as tracing adds it, and in the end the clean frame, in
 * the same form. guide: where the first surface is, and its normal, packed. albedo: its color. shift: what bones
 * moved it by, backwards. */
typedef struct ArtfDenoiser {
    int width, height;
    float *noisy, *guide, *albedo, *shift, *previousGuide, *history, *nextHistory, *ping, *pong;
    float viewProj[16], eye[3];
    bool first;
    /* SVGF (Schied et al., 2017): of each pixel, its light's luminance and its square, gathered as its light is (the
     * frame before's and this one's); and how much that light varies, for the filter's passes, in turn. */
    float *moments, *nextMoments, *variance, *varianceNext;
    /* A-SVGF's temporal gradients (Schied et al., 2018): of each pixel, its first sample's luminance, this frame's and
     * the frame before's, and the frame before's first sample's number; of each square of 3 x 3 pixels, how surely the
     * light changed there, 0 to 1 (artfDenoiserGradients), and whether that was found this frame. */
    float *firstLight, *firstLightBefore, *change, *changeRaw;
    int squaresX, squaresY;
    uint32_t firstSampleBefore;
    bool gradients;
} ArtfDenoiser;

void artfDenoiserResize(ArtfDenoiser* d, int width, int height);
void artfDenoiserFree(ArtfDenoiser* d);
/* samples: how many this frame's light is of. motion: of each traced instance, how a point of it goes to where it was. */
void artfDenoiserRun(ArtfDenoiser* d, ArtfPool* pool, const ArtfFrameData* frame, uint32_t samples, const float* motion, size_t motionCount);
/* The temporal gradients, before artfDenoiserRun: where the eye stood still, one pixel of each square of 3 x 3 (which one
 * turning with frame) traced again with the frame before's first sample, against what that sample gave then. Where the
 * sample meets another place (what moved, what bones bent), it says nothing. A change is counted in deviations of that
 * pixel's own noise (its moments), alone and over the 3 x 3 squares around, so that a soft shadow's noise is not one:
 * where the light surely changed, what was gathered goes toward this frame's light around, by as much, and is cut back
 * as a light that drifted is. trace: one sample of a pixel, its luminance, and where its ray first met a surface (place,
 * as a guide's). */
typedef float (*ArtfDenoiserTrace)(void* user, uint32_t x, uint32_t y, uint32_t sample, float* place);
void artfDenoiserGradients(ArtfDenoiser* d, ArtfPool* pool, const ArtfFrameData* frame, uint32_t firstSample, uint32_t frameNumber, ArtfDenoiserTrace trace, void* user);

#endif
