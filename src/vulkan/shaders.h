/* artful — the GPU's shaders: each returns SPIR-V, the caller's (artfFree), and its size in bytes. */
#ifndef ARTFUL_SHADERS_H
#define ARTFUL_SHADERS_H

#include "artful/core.h"

typedef struct ArtfGraphData ArtfGraphData;

typedef enum ArtfCullPhase {
    ARTF_CULL_ALL,   /* no occlusion: all that is in view */
    ARTF_CULL_EARLY, /* what is in view and showed last frame */
    ARTF_CULL_LATE   /* what is in view and not behind what the early part drew */
} ArtfCullPhase;

uint32_t* artfShaderCull(size_t* size, ArtfCullPhase phase);
/* A level of the depth pyramid, from the level below, or from the depth buffer of that many samples. */
uint32_t* artfShaderHiz(size_t* size, bool fromDepth, int samples);
uint32_t* artfShaderSceneVertex(size_t* size);
/* cutout: for what plain solid surfaces are not: holes, backs and alpha. It leaves pixels, which the plain
 * one must not, so that a GPU can test depth before it. */
/* lists: how many samples a pixel has, if a layer's pixels go to their pixel's list, for artfShaderLayerResolve, and not
 * to the frame; 0: they do not. */
/* What a scene's pixel shader holds beside the rest: a frame in floats, light from probes (what costs its registers, so
 * that a scene without probes does not pay for them). */
enum { ARTF_SHADER_FLOATS = 1, ARTF_SHADER_PROBES = 2, ARTF_SHADER_WALLS = 4 /* the probes' maps of how far they see */, ARTF_SHADER_FROXELS = 8 };
uint32_t* artfShaderSceneFragment(size_t* size, uint32_t maxTextures, bool cutout, const ArtfGraphData* graphs, uint32_t graphCount, int lists, uint32_t variant);
uint32_t* artfShaderLayerResolve(size_t* size, int samples);
/* Shadow maps: depth from a light; and, for what has holes, what leaves pixels. */
uint32_t* artfShaderShadowVertex(size_t* size);
uint32_t* artfShaderShadowFragment(size_t* size, uint32_t maxTextures);
uint32_t* artfShaderOverlayVertex(size_t* size);
/* Tracing: a sample more for each pixel; and what was gathered, shown over what is there. */
uint32_t* artfShaderTrace(size_t* size, uint32_t maxTextures, const ArtfGraphData* graphs, uint32_t graphCount, bool guides);
/* Taking out noise: time; a pass of the filter in space; and back to light. */
uint32_t* artfShaderSkin(size_t* size);
uint32_t* artfShaderMeshletCull(size_t* size, ArtfCullPhase phase);
uint32_t* artfShaderShadowCull(size_t* size);
uint32_t* artfShaderDenoiseLight(size_t* size);
uint32_t* artfShaderDenoiseTime(size_t* size);
uint32_t* artfShaderDenoiseSpace(size_t* size, int pass);
uint32_t* artfShaderDenoiseEnd(size_t* size);
uint32_t* artfShaderTraceShow(size_t* size, bool hdr);
uint32_t* artfShaderOverlayFragment(size_t* size);
/* A black hole: its rays, then the bloom's levels going down and coming back, then the frame's
 * background. (Its vertices are the overlay's: one triangle.) */
uint32_t* artfShaderHoleTrace(size_t* size);
uint32_t* artfShaderBloomDown(size_t* size, bool fromLight);
uint32_t* artfShaderBloomUp(size_t* size);
uint32_t* artfShaderHoleBackdrop(size_t* size, bool hdr);
uint32_t* artfShaderEnvironmentBackdrop(size_t* size, bool hdr);
/* What comes after a frame in floats (postShaders.inc): post.c's passes. */
uint32_t* artfShaderPostGather(size_t* size, int samples);
uint32_t* artfShaderPostOcclusion(size_t* size);
uint32_t* artfShaderPostOcclusionBlur(size_t* size);
uint32_t* artfShaderPostReflection(size_t* size);
uint32_t* artfShaderPostReflectionApply(size_t* size);
uint32_t* artfShaderPostFroxels(size_t* size);
/* Fog that light goes through, over what is behind the scene (with the overlay's vertices). */
uint32_t* artfShaderFogBackdrop(size_t* size);
uint32_t* artfShaderPostTemporal(size_t* size);
uint32_t* artfShaderPostFocus(size_t* size);
uint32_t* artfShaderPostBloomDown(size_t* size);
uint32_t* artfShaderPostBloomUp(size_t* size);
uint32_t* artfShaderPostFinal(size_t* size);

#endif
