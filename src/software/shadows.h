/* artful — shadow maps on the CPU (shadows.c), and what kernels.inc asks of them. */
#ifndef ARTFUL_SHADOWS_H
#define ARTFUL_SHADOWS_H

#include "scene/sceneInternal.h"
#include "core/threads.h"

typedef struct ArtfShadowSource {
    const ArtfShadowFrame* frame;
    const float* atlas;
    int width;
} ArtfShadowSource;

typedef struct ArtfShadowMaps ArtfShadowMaps;

ArtfShadowMaps* artfShadowMapsNew(ArtfPool* pool);
void artfShadowMapsFree(ArtfShadowMaps* m);
/* Draws all the scene's shadow maps for this frame; NULL when it has none. Valid until the next call. */
/* How many maps the last draw made again. */
uint32_t artfShadowMapsDrawn(const ArtfShadowMaps* m);
const ArtfShadowSource* artfShadowMapsDraw(ArtfShadowMaps* m, const ArtfScene* scene, const ArtfFrameData* frame);

/* How much light gets to a place, 0 to 1. */
float artfShadowOfSun(const ArtfShadowSource* s, const float* eye, const float* world, const float* normal);
float artfShadowOfLight(const ArtfShadowSource* s, uint32_t light, const float* position, const float* world, const float* normal);

#endif
