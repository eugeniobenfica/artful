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
} ArtfDenoiser;

void artfDenoiserResize(ArtfDenoiser* d, int width, int height);
void artfDenoiserFree(ArtfDenoiser* d);
/* samples: how many this frame's light is of. motion: of each traced instance, how a point of it goes to where it was. */
void artfDenoiserRun(ArtfDenoiser* d, ArtfPool* pool, const ArtfFrameData* frame, uint32_t samples, const float* motion, size_t motionCount);

#endif
