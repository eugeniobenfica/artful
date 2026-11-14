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
/* size: how wide the light is, for soft shadows (ArtfShadowDesc.soft): the sun's angle, a lamp's radius; 0: sharp.
 * turn: -1, all of the soft lookup's reads, the same every frame; 0 to 15 (artfShadowTurn), fewer, turned so, for
 * temporal antialiasing to gather. */
float artfShadowOfSun(const ArtfShadowSource* s, const float* eye, const float* world, const float* normal, float size, int turn);
float artfShadowOfLight(const ArtfShadowSource* s, uint32_t light, const float* position, const float* world, const float* normal, float size, int turn);
/* Which way a pixel's soft lookup turns in a frame: a 4 x 4 pattern that goes through all 16 in 16 frames. */
int artfShadowTurn(int x, int y, uint32_t frame);
/* How far light from the sun, or a light of the frame's, goes inside what it meets first to get to a place, from the
 * depth its map keeps: for light under a surface. Less than 0: its maps do not say. */
float artfShadowThroughSun(const ArtfShadowSource* s, const float* eye, const float* world);
float artfShadowThroughLight(const ArtfShadowSource* s, uint32_t light, const float* position, const float* world);
/* Of the k-th light past the frame's own (artfSceneExtraLights's order). */
float artfShadowOfExtra(const ArtfShadowSource* s, uint32_t k, const float* position, const float* world, const float* normal);

#endif
