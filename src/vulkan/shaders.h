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
/* motion: where what a pose bent was a frame ago, from binding 65 (a frame in floats) */
uint32_t* artfShaderSceneVertex(size_t* size, bool motion);
/* cutout: for what plain solid surfaces are not: holes, backs and alpha. It leaves pixels, which the plain
 * one must not, so that a GPU can test depth before it. */
/* lists: how many samples a pixel has, if a layer's pixels go to their pixel's list, for artfShaderLayerResolve, and not
 * to the frame; 0: they do not. */
/* What a scene's pixel shader holds beside the rest: a frame in floats, light from probes (what costs its registers, so
 * that a scene without probes does not pay for them). */
enum { ARTF_SHADER_FLOATS = 1, ARTF_SHADER_PROBES = 2, ARTF_SHADER_WALLS = 4 /* the probes' maps of how far they see */, ARTF_SHADER_FROXELS = 8,
       ARTF_SHADER_COAT = 16 /* materials with a clear coat, sheen or more past glTF's core (ArtfMaterialMore) */, ARTF_SHADER_SOFT = 32 /* soft shadows */,
       /* what a view shows (ArtfView, the frame's view); and with it, each triangle's meshlet, by its number (geometry
        * shaders' PrimitiveId) in a table (binding 65) */
       ARTF_SHADER_VIEW = 64, ARTF_SHADER_MESHLETS = 128,

       ARTF_SHADER_TRACED = 256 /* shadows from rays, through the trees */, ARTF_SHADER_RAY_QUERY = 512 /* and those through the ray tracing hardware */,
       ARTF_SHADER_SHAPES = 1024 /* lights with a shape or a profile */ };
uint32_t* artfShaderSceneFragment(size_t* size, uint32_t maxTextures, bool cutout, const ArtfGraphData* graphs, uint32_t graphCount, int lists, uint32_t variant);
/* The overdraw view: a step of a count, added up (as artfViewHeat's); what is a back that is not to show is left. */
uint32_t* artfShaderOverdraw(size_t* size, bool hdr);
uint32_t* artfShaderLayerResolve(size_t* size, int samples);
/* Shadow maps: depth from a light; and, for what has holes, what leaves pixels. */
uint32_t* artfShaderShadowVertex(size_t* size);
uint32_t* artfShaderShadowFragment(size_t* size, uint32_t maxTextures);
uint32_t* artfShaderOverlayVertex(size_t* size);
/* Tracing: a sample more for each pixel; and what was gathered, shown over what is there. */
/* hardware: rays through the ray tracing hardware (binding 64, the top level), not the trees */
/* more: materials past glTF's core, lights with a shape or a profile (the scene has some) */
uint32_t* artfShaderTrace(size_t* size, uint32_t maxTextures, const ArtfGraphData* graphs, uint32_t graphCount, bool guides, bool hardware, bool more);
/* Light that bounced: a step of the probes (trace.c's probeJob), a group of 64 for each, dims[0] x dims[1] groups by
 * dims[2]; from binding 62, what they held, into 70; the push constant is the step's number. walls: artfIndirectAt's
 * test of what each probe sees, for the light that bounced before; rays and keep: ArtfIndirectDesc's. */
uint32_t* artfShaderProbes(size_t* size, uint32_t maxTextures, const ArtfGraphData* graphs, uint32_t graphCount, bool hardware, bool more, bool walls, int rays, float keep);
/* Taking out noise: time; a pass of the filter in space; and back to light. */
uint32_t* artfShaderSkin(size_t* size);
/* Emitters of many: each particle moved by the steps its emitter made, then an instance of the quad facing the eye. */
uint32_t* artfShaderSwarm(size_t* size);
uint32_t* artfShaderMeshletCull(size_t* size, ArtfCullPhase phase);
uint32_t* artfShaderShadowCull(size_t* size);
uint32_t* artfShaderDenoiseLight(size_t* size);
uint32_t* artfShaderDenoiseTime(size_t* size);
uint32_t* artfShaderDenoiseSpace(size_t* size, int pass);
uint32_t* artfShaderDenoiseEnd(size_t* size);
uint32_t* artfShaderTraceShow(size_t* size, bool hdr);
/* turns: what preRotate needs (a push constant at 8 says how the image is turned); false: as before it was there */
uint32_t* artfShaderOverlayFragment(size_t* size, bool turns);
/* What the frame is made for (ArtfRendererOptions.output): ARTF_DISPLAY_, and artfDisplayNumbers' inverse and peak. */
typedef struct ArtfShaderDisplay { int kind; float inverse, peak; } ArtfShaderDisplay;
/* The same, laid over a frame made for that (NULL: sRGB, as above). */
uint32_t* artfShaderOverlayFragmentFor(size_t* size, bool turns, const ArtfShaderDisplay* display);
/* A background pass: after the program's shader, the bloom's levels going down and coming back, then the frame's
 * background. (Its vertices are the overlay's: one triangle.) */
uint32_t* artfShaderBloomDown(size_t* size, bool fromLight);
uint32_t* artfShaderBloomUp(size_t* size);
uint32_t* artfShaderBackgroundBackdrop(size_t* size, bool hdr);
uint32_t* artfShaderEnvironmentBackdrop(size_t* size, bool hdr);
/* What comes after a frame in floats (postShaders.inc): post.c's passes. */
uint32_t* artfShaderPostGather(size_t* size, int samples);
uint32_t* artfShaderPostOcclusion(size_t* size);
uint32_t* artfShaderPostOcclusionBlur(size_t* size);
uint32_t* artfShaderPostReflection(size_t* size);
uint32_t* artfShaderPostReflectionApply(size_t* size);
/* Reflections from rays (ArtfPostDesc.screen.tracedReflections), through the trees or, with hardware, the top level. */
uint32_t* artfShaderPostReflectionTraced(size_t* size, uint32_t maxTextures, const ArtfGraphData* graphs, uint32_t graphCount, bool hardware);
uint32_t* artfShaderPostFroxels(size_t* size);
/* Fog that light goes through, over what is behind the scene (with the overlay's vertices). */
uint32_t* artfShaderFogBackdrop(size_t* size);
uint32_t* artfShaderPostTemporal(size_t* size);
uint32_t* artfShaderPostFocus(size_t* size);
uint32_t* artfShaderPostBloomDown(size_t* size);
uint32_t* artfShaderPostBloomUp(size_t* size);
uint32_t* artfShaderPostFinal(size_t* size, bool turns);
uint32_t* artfShaderPostFinalFor(size_t* size, bool turns, const ArtfShaderDisplay* display);
uint32_t* artfShaderPostMeter(size_t* size); /* auto exposure, one invocation */
uint32_t* artfShaderPostMotionBlur(size_t* size);
uint32_t* artfShaderPostVelocity(size_t* size);  /* motion blur's: each pixel's, */
uint32_t* artfShaderPostTileMost(size_t* size);  /* the largest of each tile (push 0), of its neighbours (push 1) */
uint32_t* artfShaderPostUpscale(size_t* size);   /* the scene drawn smaller, made whole */
uint32_t* artfShaderPostSharpen(size_t* size);

#endif
