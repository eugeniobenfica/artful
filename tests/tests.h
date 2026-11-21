/* artful-tests: what the tests share. Each area has a file of its own (core.c, post.c...); main.c runs them in turn,
 * or those whose names hold a word given (artful-tests --list names them). The GPU's part runs where Vulkan does, and
 * says so where it does not. */
#ifndef ARTFUL_TESTS_H
#define ARTFUL_TESTS_H

#include "artful/artful.h"
#include "scene/sceneInternal.h"
#include "render/rendererInternal.h"
#include "core/threads.h"
#include "holeInternal.h" /* (extensions/blackhole) */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

#define W 320
#define H 200
#define CHANNEL(p, shift) ((int)((p) >> (shift) & 255))

#ifdef ARTFUL_COUNT_ALLOCATIONS
long artfAllocationsAlive(void);
#endif

/* ---- main.c: what a test could not reach here ("vulkan", "rt" for ray tracing hardware, "window"): the test is
 * then skipped rather than passed, unless ARTFUL_REQUIRE names it ---- */
void missing(const char* what);
const char* currentTest(void);

/* A renderer asked for by name that cannot be made here says so to missing(): every test makes its renderers
 * through these (common.c). */
ArtfRenderer* testRendererNew(const ArtfRendererOptions* o);
#define artfRendererNew testRendererNew
#include "artful/2d.h"
ArtfRenderer2D* testRenderer2DNew(const ArtfRenderer2DOptions* o);
#define artfRenderer2DNew testRenderer2DNew

/* ---- how far apart two frames look: FLIP (tools/flip.h), 0 alike to about 1 ---- */
#include "flip.h"
/* FLIP of the last frames differentShare compared; and that its mean is at most so much (the plan's 0.4: a case's
 * threshold by FLIP, beside its old one). */
extern FlipStats lastFlip;
bool flipAlike(double meanAtMost, const char* file, int line);
#define FLIP_ALIKE(meanAtMost) CHECK(flipAlike(meanAtMost, __FILE__, __LINE__))
/* With ARTFUL_FLIP_LOG=file, every pair of frames differentShare compares is measured by FLIP too and written to it
 * (test, threshold, share, FLIP's mean, 99th percentile and most): what thresholds by FLIP will be set from. */
void flipSummary(void);
void flipRecord(const uint32_t* a, const uint32_t* b, int threshold, double share);

/* ---- common.c: helpers and scenes more than one area takes ---- */
uint64_t hashOf(const uint32_t* p, size_t n);
ArtfRendererOptions options(ArtfBackend backend, int threads, bool portable);
void setCamera(ArtfScene* s, float x, float y, float z, float tx, float ty, float tz);
ArtfScene* richScene(void);
double differentShare(const uint32_t* a, const uint32_t* b, int threshold);
/* What a screen shows of light, as a byte, rounded: the sRGB curve itself, not the library's arithmetic for it. */
int shownByte(double light);
ArtfScene* plainScene(void);
ArtfMaterial whiteMaterial(ArtfScene* s);
ArtfMesh addSquare(ArtfScene* s, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
ArtfInstance addAt(ArtfScene* s, ArtfMesh mesh, ArtfMaterial material, float x, float y, float z, float scale);
void drawInto(ArtfRenderer* r, ArtfScene* s, uint32_t* frame);
bool near_(int a, int b, int margin);
ArtfScene* layeredScene(void);
extern int logged;
void countLog(void* user, const char* message);
ArtfTexture oneColor(ArtfScene* s, uint32_t bgra);
ArtfScene* shadowScene(void);
ArtfExpr stripes(ArtfGraph* g, float count);
ArtfScene* bleedScene(void);
ArtfScene* bulbScene(float size);
int greyCount(const uint32_t* f);
ArtfScene* movingScene(ArtfInstance* ball);
void drawOnce(ArtfRenderer* r, ArtfScene* s, uint32_t* frame);
void placeBall(ArtfScene* s, ArtfInstance ball, float x);
double meanApart(const uint32_t* a, const uint32_t* b);
ArtfScene* viewScene(ArtfInstance* moving);
int litCount(const uint32_t* f);
void drawFrames(ArtfRenderer* r, ArtfScene* s, int n, uint32_t* frame);
ArtfMesh addStrip(ArtfScene* s);
void bendStrip(ArtfScene* s, ArtfInstance i, float angle);
void put32le(uint8_t* p, uint32_t v);
void drawSamples(ArtfRenderer* r, ArtfScene* s, int samples, uint32_t* frame);
double meanOf(const uint32_t* frame, int x, int y, int shift);
ArtfRenderer* gpuWithoutRayQueries(const ArtfRendererOptions* o); /* as on a device with no ray tracing hardware */

/* ---- the tests, by area ---- */
/* metrics.c: what pictures are measured with */
void testFlip(void);
void testSrgb(void);
void testSobol(void);
/* core.c: the mesh's data, the CPU's promises, levels of detail, occlusion, handles and lifetimes */
void testPacking(void);
void testMeshlets(void);
void testInstances(void);
void testSamePixels(void);
void testNoGaps(void);
void testClipping(void);
void testCulling(void);
void testOverlay(void);
void testResize(void);
void testSimplify(void);
void testLod(void);
void testOcclusion(ArtfBackend backend);
void testOcclusionUnderOverlay(void);
void testPictures(void);
void testHandles(void);
void testLifetimes(void);
void testThreadRule(void);
void testOcclusionAuto(ArtfBackend backend);
void testLodFade(ArtfBackend backend);
void testGpuMeshlets(void);
void testCullSize(void);
/* backends.c: the GPU against the CPU, and a device that is lost or refuses */
void testVulkan(void);
void testLostDevice(void);
void testUniformTextures(void);
void testWebGpu(void);
void testMetal(void);
void testShaderIr(void);
void testSpirvRoundTrip(void);
void testFrameGraph(void);
void testFramePasses(void);
void testTextureMemory(void);
void testGpuMemory(void);
void testRefusedSwapchain(void);
void testShadersAhead(void);
void testPipelinesAhead(void);
/* layers.c: colors, layers, alpha, samples and decals */
void testColorData(void);
void testLightClusters(void);
void testAreaLights(void);
void testUpscaleQuality(void);
void testPacing(void);
void testPreRotate(void);
void testPostAliasing(void);
void testSharpHistory(void);
void testDisplayOutput(void);
void testReactive(void);
void testProbesHard(void);
void test2DThreadRule(void);
void testTextureHandles(void);
void testFewSamples(void);
void testLightMis(void);
void testLayers(ArtfBackend backend);
void testMaterials(ArtfBackend backend, bool antialias);
void testSamePixelsOfLayers(void);
void testAntialias(ArtfBackend backend);
void testLayersBothBackends(void);
void testLayersSorted(ArtfBackend backend);
void testLayersBehindSolid(void);
void testLayersTransmittance(void);
void testDecals(void);
/* materials.c: what things are made of: materials, materials of your own, lights with a size and a shape */
void testPhysical(ArtfBackend backend);
void testGraphs(ArtfBackend backend);
void testGraphsBothBackends(void);
void testCoat(void);
void testSizedLights(void);
void testLightShapes(void);
void testProfiles(void);
void testMoreMaterials(void);
void testSpecularMaps(void);
void testFilmMaps(void);
void testFilmBelcour(void);
void testOpenPbr(void);
void testUnderSpread(void);
void testTextureRoles(void);
void testHighlightFlicker(void);
/* files.c: glTF, its extensions and compressed blocks */
void testBlocks(void);
void testBlockDecoders(void);
void testBasis(void);
void testBasisAstc(void);
void testGltfSparse(void);
void testGltfExtensions(void);
void testGltfMeshopt(void);
void testGltfMore(void);
void testGltfTargets(void);
void testGltf(void);
void testFuzzFindings(void);
/* streaming.c: what comes and goes: removing, residency, streaming */
void testRemoving(ArtfBackend backend);
void testPrepared(void);
void testResidency(ArtfBackend backend);
void testStreamedTexture(ArtfBackend backend);
void testResidencyGraph(ArtfBackend backend);
void testMeshPlaces(ArtfBackend backend);
void testMeshStreaming(ArtfBackend backend);
void testMeshHandles(void);
/* shadows.c: shadows: maps, cascades, soft, and lights past the sixteen */
void testShadows(ArtfBackend backend);
void testShadowsBothBackends(void);
void testShadowBytes(void);
void testShadowCache(ArtfBackend backend);
void testShadowRegions(ArtfBackend backend);
void testSoftShadows(void);
void testSoftShadowsTurning(void);
void testExtraShadows(void);
/* post.c: a frame in floats and what comes after it: the look, temporal antialiasing, upscaling */
void testLook(ArtfBackend backend);
void testFloats(void);
void testGrading(void);
void testLookMore(void);
void testPostKernels(void);
void testUpscale(void);
void testTemporalBytes(void);
void testFloatsSamples(void);
/* tracing.c: rays: tracing, the ray tracing hardware, rays while drawing, denoising, probes */
void testDenoise(ArtfBackend backend);
void testDenoiseChange(ArtfBackend backend);
void testDenoiseMotion(ArtfBackend backend);
void testDenoiseBones(ArtfBackend backend);
void testDenoiseBytes(void);
void testProbeReflections(void);
void testTraceHardware(void);
void testTracedShadows(void);
void testTracedMoved(void);
void testTracedRefit(void);
void testTracedReflections(void);
void testProbesWalls(void);
void testProbesGpu(void);
void testProbesKept(void);
void testProbesFollow(void);
void testProbeCascades(void);
void testTrace(ArtfBackend backend);
void testTraceData(void);
void testTraceBothBackends(void);
void testTreesFourWays(void);
void testExtraTraced(void);
void testSampledLights(void);
void testSampledMoving(void);
void testGpuLeftAlone(void);
void testEnvironmentAimed(ArtfBackend backend);
void testMetalFurnace(ArtfBackend backend);
void testAimUnbiased(void);
void testRayCones(void);
/* sky.c: the sky, and light from all around from a picture */
void testEnvironment(void);
void testSky(void);
void testSkyMore(void);
void testAerial(void);
void testCloudTower(void);
/* motion.c: what bends and moves: bones, targets, clips, motion followed, motion blur */
void testMotion(void);
void testSkinnedMotion(void);
void testSkinScenes(void);
void testBlurEdges(void);
void testSlerp(void);
void testSkin(ArtfBackend backend);
void testMorph(ArtfBackend backend);
void testSkinBothBackends(void);
void testClips(void);
/* scale.c: particles, terrain, many as one, emitters of many */
void testParticleForces(void);
void testTerrainLayers(void);
void testHlod(void);
void testClusterDag(void);
void testClusterDraw(void);
void testCoarseRays(void);
void testDrawnLift(void);
void testHlodCooked(void);
void testSkinLod(void);
void testTracedSkinLod(void);
void testSwarm(void);
void testExtras(void);
/* views.c: views, what passes take, an orthographic camera, picking */
void testViews(void);
void testPassTimes(void);
void testOrthographic(void);
void testPick(void);
/* blackhole.c: a fluid, a background pass and the black hole (extensions/blackhole) */
void testFluid(void);
void testBackgroundPass(void);
void testBlackHole(void);
/* twod.c: drawing in 2D (artful-2d) */
void test2DAtan(void);
void test2DShapes(void);
void test2DTextures(void);
void test2DLights(void);
void test2DShadows(void);
void test2DGI(void);
void test2DPost(void);
void test2DSameBytes(void);
void test2DHandles(void);
void test2DOver3D(void);
void test2DVulkan(void);
void test2DVulkanFrames(void);
void test2DWindow(void);

#endif
