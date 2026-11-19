/* artful-tests: every test, in order. With words given, only the tests whose names hold one of them run:
 * artful-tests shadows vulkan runs the shadows' tests and all on Vulkan. --list names them, and runs none.
 * --exact name runs that one alone, as CTest does each: it exits 77 when what it needs was not here (skipped).
 *
 * A test that could not reach what it needs (missing: "vulkan", "rt", "window") is skipped, not passed, even when
 * the rest of it ran. ARTFUL_REQUIRE=vulkan,rt,window makes each it names a failure instead: for a machine that
 * is meant to have it. */
#include "tests.h"

static int wordCount;
static char** words;
static bool listing, exact;
static int skippedTests;
static char skippedNames[4096];
static const char* missed[8];
static int missedCount;
static const char* testName = "";

static bool chosen(const char* name) {
    if (listing) {
        puts(name);
        return false;
    }
    if (exact) return !strcmp(name, words[0]);
    if (!wordCount) return true;
    for (int i = 0; i < wordCount; ++i)
        if (strstr(name, words[i])) return true;
    return false;
}

void missing(const char* what) {
    for (int i = 0; i < missedCount; ++i)
        if (!strcmp(missed[i], what)) return;
    if (missedCount < (int)ARTF_COUNT(missed)) missed[missedCount++] = what;
}

const char* currentTest(void) { return testName; }

/* Whether ARTFUL_REQUIRE names it: a list of words, by commas. */
static bool required(const char* what) {
    const char* list = getenv("ARTFUL_REQUIRE");
    if (!list) return false;
    const size_t n = strlen(what);
    for (const char* p = list; (p = strstr(p, what)) != NULL; p += n)
        if ((p == list || p[-1] == ',') && (p[n] == ',' || p[n] == 0)) return true;
    return false;
}

static int failuresBefore;

static void begin(const char* name) {
    testName = name;
    missedCount = 0;
    failuresBefore = failures;
}

static void end(void) {
    for (int i = 0; i < missedCount; ++i)
        if (required(missed[i])) {
            printf("%s: needs %s, which is not here (ARTFUL_REQUIRE)\n", testName, missed[i]);
            ++failures;
        }
    if (missedCount && failures == failuresBefore) {
        ++skippedTests;
        size_t at = strlen(skippedNames);
        snprintf(skippedNames + at, sizeof skippedNames - at, "%s  %s (no %s", at ? "\n" : "", testName, missed[0]);
        for (int i = 1; i < missedCount; ++i) {
            at = strlen(skippedNames);
            snprintf(skippedNames + at, sizeof skippedNames - at, ", %s", missed[i]);
        }
        at = strlen(skippedNames);
        snprintf(skippedNames + at, sizeof skippedNames - at, ")");
    }
    testName = "";
}

#define RUN(name, call) do { if (chosen(name)) { begin(name); call; end(); } } while (0)

int main(int argc, char** argv) {
    listing = argc > 1 && !strcmp(argv[1], "--list");
    exact = argc > 2 && !strcmp(argv[1], "--exact");
    if (exact) {
        wordCount = 1;
        words = argv + 2;
    } else if (!listing) {
        wordCount = argc - 1;
        words = argv + 1;
    }
#ifdef ARTFUL_VULKAN
    /* (a feature asked for is drawn on the next frame, as each test expects; testShadersAhead checks the other way) */
    artfVulkanShadersWait = true;
#endif
    RUN("metrics/flip", testFlip());
    RUN("metrics/srgb", testSrgb());
    RUN("core/packing", testPacking());
    RUN("core/meshlets", testMeshlets());
    RUN("core/instances", testInstances());
    RUN("core/samePixels", testSamePixels());
    RUN("core/noGaps", testNoGaps());
    RUN("core/clipping", testClipping());
    RUN("core/culling", testCulling());
    RUN("core/overlay", testOverlay());
    RUN("core/resize", testResize());
    RUN("core/simplify", testSimplify());
    RUN("core/lod", testLod());
    RUN("core/occlusion/software", testOcclusion(ARTF_BACKEND_SOFTWARE));
    RUN("core/occlusionUnderOverlay", testOcclusionUnderOverlay());
    RUN("core/occlusion/vulkan", testOcclusion(ARTF_BACKEND_VULKAN));
    RUN("backends/vulkan", testVulkan());
    RUN("layers/colorData", testColorData());
    RUN("layers/layers/software", testLayers(ARTF_BACKEND_SOFTWARE));
    RUN("layers/layers/vulkan", testLayers(ARTF_BACKEND_VULKAN));
    RUN("layers/materials/software", testMaterials(ARTF_BACKEND_SOFTWARE, false));
    RUN("layers/materials/software/antialiased", testMaterials(ARTF_BACKEND_SOFTWARE, true));
    RUN("layers/materials/vulkan", testMaterials(ARTF_BACKEND_VULKAN, false));
    RUN("layers/materials/vulkan/antialiased", testMaterials(ARTF_BACKEND_VULKAN, true));
    RUN("layers/samePixelsOfLayers", testSamePixelsOfLayers());
    RUN("layers/antialias/software", testAntialias(ARTF_BACKEND_SOFTWARE));
    RUN("layers/antialias/vulkan", testAntialias(ARTF_BACKEND_VULKAN));
    RUN("layers/layersBothBackends", testLayersBothBackends());
    RUN("core/pictures", testPictures());
    RUN("tracing/traceData", testTraceData());
    RUN("tracing/trace/software", testTrace(ARTF_BACKEND_SOFTWARE));
    RUN("tracing/trace/vulkan", testTrace(ARTF_BACKEND_VULKAN));
    RUN("tracing/traceBothBackends", testTraceBothBackends());
    RUN("core/handles", testHandles());
    RUN("post/look/software", testLook(ARTF_BACKEND_SOFTWARE));
    RUN("post/look/vulkan", testLook(ARTF_BACKEND_VULKAN));
    RUN("core/lifetimes", testLifetimes());
    RUN("core/threadRule", testThreadRule());
    RUN("materials/physical/software", testPhysical(ARTF_BACKEND_SOFTWARE));
    RUN("materials/physical/vulkan", testPhysical(ARTF_BACKEND_VULKAN));
    RUN("shadows/shadowBytes", testShadowBytes());
    RUN("shadows/shadows/software", testShadows(ARTF_BACKEND_SOFTWARE));
    RUN("shadows/shadows/vulkan", testShadows(ARTF_BACKEND_VULKAN));
    RUN("shadows/shadowsBothBackends", testShadowsBothBackends());
    RUN("files/gltf", testGltf());
    RUN("motion/clips", testClips());
    RUN("motion/skin/software", testSkin(ARTF_BACKEND_SOFTWARE));
    RUN("motion/skin/vulkan", testSkin(ARTF_BACKEND_VULKAN));
    RUN("motion/skinBothBackends", testSkinBothBackends());
    RUN("motion/morph/software", testMorph(ARTF_BACKEND_SOFTWARE));
    RUN("motion/morph/vulkan", testMorph(ARTF_BACKEND_VULKAN));
    RUN("tracing/denoiseBytes", testDenoiseBytes());
    RUN("tracing/denoise/software", testDenoise(ARTF_BACKEND_SOFTWARE));
    RUN("tracing/denoise/vulkan", testDenoise(ARTF_BACKEND_VULKAN));
    RUN("tracing/denoiseChange/software", testDenoiseChange(ARTF_BACKEND_SOFTWARE));
    RUN("tracing/denoiseChange/vulkan", testDenoiseChange(ARTF_BACKEND_VULKAN));
    RUN("tracing/denoiseMotion/software", testDenoiseMotion(ARTF_BACKEND_SOFTWARE));
    RUN("tracing/denoiseMotion/vulkan", testDenoiseMotion(ARTF_BACKEND_VULKAN));
    RUN("tracing/denoiseBones/software", testDenoiseBones(ARTF_BACKEND_SOFTWARE));
    RUN("tracing/denoiseBones/vulkan", testDenoiseBones(ARTF_BACKEND_VULKAN));
    RUN("core/occlusionAuto/software", testOcclusionAuto(ARTF_BACKEND_SOFTWARE));
    RUN("core/occlusionAuto/vulkan", testOcclusionAuto(ARTF_BACKEND_VULKAN));
    RUN("shadows/shadowCache/software", testShadowCache(ARTF_BACKEND_SOFTWARE));
    RUN("shadows/shadowCache/vulkan", testShadowCache(ARTF_BACKEND_VULKAN));
    RUN("shadows/shadowRegions/software", testShadowRegions(ARTF_BACKEND_SOFTWARE));
    RUN("shadows/shadowRegions/vulkan", testShadowRegions(ARTF_BACKEND_VULKAN));
    RUN("core/lodFade/software", testLodFade(ARTF_BACKEND_SOFTWARE));
    RUN("core/lodFade/vulkan", testLodFade(ARTF_BACKEND_VULKAN));
    RUN("core/gpuMeshlets", testGpuMeshlets());
    RUN("layers/layersBehindSolid", testLayersBehindSolid());
    RUN("layers/layersSorted/software", testLayersSorted(ARTF_BACKEND_SOFTWARE));
    RUN("layers/layersSorted/vulkan", testLayersSorted(ARTF_BACKEND_VULKAN));
    RUN("files/blockDecoders", testBlockDecoders());
    RUN("streaming/streamedTexture/software", testStreamedTexture(ARTF_BACKEND_SOFTWARE));
    RUN("streaming/streamedTexture/vulkan", testStreamedTexture(ARTF_BACKEND_VULKAN));
    RUN("streaming/residencyGraph/software", testResidencyGraph(ARTF_BACKEND_SOFTWARE));
    RUN("streaming/residencyGraph/vulkan", testResidencyGraph(ARTF_BACKEND_VULKAN));
    RUN("streaming/meshPlaces/software", testMeshPlaces(ARTF_BACKEND_SOFTWARE));
    RUN("streaming/meshPlaces/vulkan", testMeshPlaces(ARTF_BACKEND_VULKAN));
    RUN("motion/slerp", testSlerp());
    RUN("files/gltfSparse", testGltfSparse());
    RUN("files/gltfTargets", testGltfTargets());
    RUN("files/gltfExtensions", testGltfExtensions());
    RUN("files/gltfMeshopt", testGltfMeshopt());
    RUN("sky/environment", testEnvironment());
    RUN("post/floats", testFloats());
    RUN("post/grading", testGrading());
    RUN("materials/coat", testCoat());
    RUN("materials/lightShapes", testLightShapes());
    RUN("materials/profiles", testProfiles());
    RUN("materials/moreMaterials", testMoreMaterials());
    RUN("files/gltfMore", testGltfMore());
    RUN("sky/sky", testSky());
    RUN("sky/skyMore", testSkyMore());
    RUN("post/lookMore", testLookMore());
    RUN("materials/sizedLights", testSizedLights());
    RUN("motion/motion", testMotion());
    RUN("post/upscale", testUpscale());
    RUN("motion/skinnedMotion", testSkinnedMotion());
    RUN("motion/skinScenes", testSkinScenes());
    RUN("motion/blurEdges", testBlurEdges());
    RUN("shadows/softShadowsTurning", testSoftShadowsTurning());
    RUN("post/temporalBytes", testTemporalBytes());
    RUN("shadows/softShadows", testSoftShadows());
    RUN("tracing/probeReflections", testProbeReflections());
    RUN("core/cullSize", testCullSize());
    RUN("scale/particleForces", testParticleForces());
    RUN("scale/terrainLayers", testTerrainLayers());
    RUN("scale/hlod", testHlod());
    RUN("scale/skinLod", testSkinLod());
    RUN("scale/tracedSkinLod", testTracedSkinLod());
    RUN("scale/swarm", testSwarm());
    RUN("streaming/meshStreaming/software", testMeshStreaming(ARTF_BACKEND_SOFTWARE));
    RUN("streaming/meshStreaming/vulkan", testMeshStreaming(ARTF_BACKEND_VULKAN));
    RUN("streaming/meshHandles", testMeshHandles());
    RUN("tracing/traceHardware", testTraceHardware());
    RUN("views/views", testViews());
    RUN("backends/shadersAhead", testShadersAhead());
    RUN("backends/pipelinesAhead", testPipelinesAhead());
    RUN("post/postKernels", testPostKernels());
    RUN("views/passTimes", testPassTimes());
    RUN("views/orthographic", testOrthographic());
    RUN("views/pick", testPick());
    RUN("tracing/tracedShadows", testTracedShadows());
    RUN("tracing/tracedReflections", testTracedReflections());
    RUN("tracing/tracedMoved", testTracedMoved());
    RUN("tracing/tracedRefit", testTracedRefit());
    RUN("tracing/probesWalls", testProbesWalls());
    RUN("tracing/probesGpu", testProbesGpu());
    RUN("shadows/extraShadows", testExtraShadows());
    RUN("post/floatsSamples", testFloatsSamples());
    RUN("layers/decals", testDecals());
    RUN("scale/extras", testExtras());
    RUN("backends/lostDevice", testLostDevice());
    RUN("backends/textureMemory", testTextureMemory());
    RUN("backends/refusedSwapchain", testRefusedSwapchain());
    RUN("files/blocks", testBlocks());
    RUN("streaming/prepared", testPrepared());
    RUN("streaming/removing/software", testRemoving(ARTF_BACKEND_SOFTWARE));
    RUN("streaming/removing/vulkan", testRemoving(ARTF_BACKEND_VULKAN));
    RUN("streaming/residency/software", testResidency(ARTF_BACKEND_SOFTWARE));
    RUN("streaming/residency/vulkan", testResidency(ARTF_BACKEND_VULKAN));
    RUN("materials/graphs/software", testGraphs(ARTF_BACKEND_SOFTWARE));
    RUN("materials/graphs/vulkan", testGraphs(ARTF_BACKEND_VULKAN));
    RUN("materials/graphsBothBackends", testGraphsBothBackends());
    RUN("blackhole/fluid", testFluid());
    RUN("blackhole/backgroundPass", testBackgroundPass());
    RUN("blackhole/blackHole", testBlackHole());
    RUN("2d/atan", test2DAtan());
    RUN("2d/shapes", test2DShapes());
    RUN("2d/textures", test2DTextures());
    RUN("2d/lights", test2DLights());
    RUN("2d/shadows", test2DShadows());
    RUN("2d/gi", test2DGI());
    RUN("2d/post", test2DPost());
    RUN("2d/sameBytes", test2DSameBytes());
    RUN("2d/handles", test2DHandles());
    RUN("2d/over3D", test2DOver3D());
    RUN("2d/vulkan", test2DVulkan());
    RUN("2d/vulkanFrames", test2DVulkanFrames());
    RUN("2d/window", test2DWindow());
    if (listing) return 0;
#ifdef ARTFUL_COUNT_ALLOCATIONS
    printf("allocations still alive: %ld\n", artfAllocationsAlive());
    CHECK(artfAllocationsAlive() == 0);
#endif
    flipSummary();
    if (skippedTests) printf("skipped, for what is not here:\n%s\n", skippedNames);
    printf("%d checks, %d failures, %d skipped\n", checks, failures, skippedTests);
    if (failures) return 1;
    return exact && skippedTests ? 77 : 0;
}
