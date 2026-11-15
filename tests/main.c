/* artful-tests: every test, in order. With words given, only the tests whose names hold one of them run:
 * artful-tests shadows vulkan runs the shadows' tests and all on Vulkan. --list names them, and runs none. */
#include "tests.h"

static int wordCount;
static char** words;
static bool listing;

static bool chosen(const char* name) {
    if (listing) {
        puts(name);
        return false;
    }
    if (!wordCount) return true;
    for (int i = 0; i < wordCount; ++i)
        if (strstr(name, words[i])) return true;
    return false;
}

int main(int argc, char** argv) {
    listing = argc > 1 && !strcmp(argv[1], "--list");
    if (!listing) {
        wordCount = argc - 1;
        words = argv + 1;
    }
#ifdef ARTFUL_VULKAN
    /* (a feature asked for is drawn on the next frame, as each test expects; testShadersAhead checks the other way) */
    artfVulkanShadersWait = true;
#endif
    if (chosen("core/packing")) testPacking();
    if (chosen("core/meshlets")) testMeshlets();
    if (chosen("core/instances")) testInstances();
    if (chosen("core/samePixels")) testSamePixels();
    if (chosen("core/noGaps")) testNoGaps();
    if (chosen("core/clipping")) testClipping();
    if (chosen("core/culling")) testCulling();
    if (chosen("core/overlay")) testOverlay();
    if (chosen("core/resize")) testResize();
    if (chosen("core/simplify")) testSimplify();
    if (chosen("core/lod")) testLod();
    if (chosen("core/occlusion/software")) testOcclusion(ARTF_BACKEND_SOFTWARE);
    if (chosen("core/occlusionUnderOverlay")) testOcclusionUnderOverlay();
    if (chosen("core/occlusion/vulkan")) testOcclusion(ARTF_BACKEND_VULKAN);
    if (chosen("backends/vulkan")) testVulkan();
    if (chosen("layers/colorData")) testColorData();
    if (chosen("layers/layers/software")) testLayers(ARTF_BACKEND_SOFTWARE);
    if (chosen("layers/layers/vulkan")) testLayers(ARTF_BACKEND_VULKAN);
    if (chosen("layers/materials/software")) testMaterials(ARTF_BACKEND_SOFTWARE, false);
    if (chosen("layers/materials/software/antialiased")) testMaterials(ARTF_BACKEND_SOFTWARE, true);
    if (chosen("layers/materials/vulkan")) testMaterials(ARTF_BACKEND_VULKAN, false);
    if (chosen("layers/materials/vulkan/antialiased")) testMaterials(ARTF_BACKEND_VULKAN, true);
    if (chosen("layers/samePixelsOfLayers")) testSamePixelsOfLayers();
    if (chosen("layers/antialias/software")) testAntialias(ARTF_BACKEND_SOFTWARE);
    if (chosen("layers/antialias/vulkan")) testAntialias(ARTF_BACKEND_VULKAN);
    if (chosen("layers/layersBothBackends")) testLayersBothBackends();
    if (chosen("core/pictures")) testPictures();
    if (chosen("tracing/traceData")) testTraceData();
    if (chosen("tracing/trace/software")) testTrace(ARTF_BACKEND_SOFTWARE);
    if (chosen("tracing/trace/vulkan")) testTrace(ARTF_BACKEND_VULKAN);
    if (chosen("tracing/traceBothBackends")) testTraceBothBackends();
    if (chosen("core/handles")) testHandles();
    if (chosen("post/look/software")) testLook(ARTF_BACKEND_SOFTWARE);
    if (chosen("post/look/vulkan")) testLook(ARTF_BACKEND_VULKAN);
    if (chosen("core/lifetimes")) testLifetimes();
    if (chosen("materials/physical/software")) testPhysical(ARTF_BACKEND_SOFTWARE);
    if (chosen("materials/physical/vulkan")) testPhysical(ARTF_BACKEND_VULKAN);
    if (chosen("shadows/shadowBytes")) testShadowBytes();
    if (chosen("shadows/shadows/software")) testShadows(ARTF_BACKEND_SOFTWARE);
    if (chosen("shadows/shadows/vulkan")) testShadows(ARTF_BACKEND_VULKAN);
    if (chosen("shadows/shadowsBothBackends")) testShadowsBothBackends();
    if (chosen("files/gltf")) testGltf();
    if (chosen("motion/clips")) testClips();
    if (chosen("motion/skin/software")) testSkin(ARTF_BACKEND_SOFTWARE);
    if (chosen("motion/skin/vulkan")) testSkin(ARTF_BACKEND_VULKAN);
    if (chosen("motion/skinBothBackends")) testSkinBothBackends();
    if (chosen("motion/morph/software")) testMorph(ARTF_BACKEND_SOFTWARE);
    if (chosen("motion/morph/vulkan")) testMorph(ARTF_BACKEND_VULKAN);
    if (chosen("tracing/denoiseBytes")) testDenoiseBytes();
    if (chosen("tracing/denoise/software")) testDenoise(ARTF_BACKEND_SOFTWARE);
    if (chosen("tracing/denoise/vulkan")) testDenoise(ARTF_BACKEND_VULKAN);
    if (chosen("tracing/denoiseChange/software")) testDenoiseChange(ARTF_BACKEND_SOFTWARE);
    if (chosen("tracing/denoiseChange/vulkan")) testDenoiseChange(ARTF_BACKEND_VULKAN);
    if (chosen("tracing/denoiseMotion/software")) testDenoiseMotion(ARTF_BACKEND_SOFTWARE);
    if (chosen("tracing/denoiseMotion/vulkan")) testDenoiseMotion(ARTF_BACKEND_VULKAN);
    if (chosen("tracing/denoiseBones/software")) testDenoiseBones(ARTF_BACKEND_SOFTWARE);
    if (chosen("tracing/denoiseBones/vulkan")) testDenoiseBones(ARTF_BACKEND_VULKAN);
    if (chosen("core/occlusionAuto/software")) testOcclusionAuto(ARTF_BACKEND_SOFTWARE);
    if (chosen("core/occlusionAuto/vulkan")) testOcclusionAuto(ARTF_BACKEND_VULKAN);
    if (chosen("shadows/shadowCache/software")) testShadowCache(ARTF_BACKEND_SOFTWARE);
    if (chosen("shadows/shadowCache/vulkan")) testShadowCache(ARTF_BACKEND_VULKAN);
    if (chosen("shadows/shadowRegions/software")) testShadowRegions(ARTF_BACKEND_SOFTWARE);
    if (chosen("shadows/shadowRegions/vulkan")) testShadowRegions(ARTF_BACKEND_VULKAN);
    if (chosen("core/lodFade/software")) testLodFade(ARTF_BACKEND_SOFTWARE);
    if (chosen("core/lodFade/vulkan")) testLodFade(ARTF_BACKEND_VULKAN);
    if (chosen("core/gpuMeshlets")) testGpuMeshlets();
    if (chosen("layers/layersBehindSolid")) testLayersBehindSolid();
    if (chosen("layers/layersSorted/software")) testLayersSorted(ARTF_BACKEND_SOFTWARE);
    if (chosen("layers/layersSorted/vulkan")) testLayersSorted(ARTF_BACKEND_VULKAN);
    if (chosen("files/blockDecoders")) testBlockDecoders();
    if (chosen("streaming/streamedTexture/software")) testStreamedTexture(ARTF_BACKEND_SOFTWARE);
    if (chosen("streaming/streamedTexture/vulkan")) testStreamedTexture(ARTF_BACKEND_VULKAN);
    if (chosen("streaming/residencyGraph/software")) testResidencyGraph(ARTF_BACKEND_SOFTWARE);
    if (chosen("streaming/residencyGraph/vulkan")) testResidencyGraph(ARTF_BACKEND_VULKAN);
    if (chosen("streaming/meshPlaces/software")) testMeshPlaces(ARTF_BACKEND_SOFTWARE);
    if (chosen("streaming/meshPlaces/vulkan")) testMeshPlaces(ARTF_BACKEND_VULKAN);
    if (chosen("motion/slerp")) testSlerp();
    if (chosen("files/gltfSparse")) testGltfSparse();
    if (chosen("files/gltfTargets")) testGltfTargets();
    if (chosen("files/gltfExtensions")) testGltfExtensions();
    if (chosen("files/gltfMeshopt")) testGltfMeshopt();
    if (chosen("sky/environment")) testEnvironment();
    if (chosen("post/floats")) testFloats();
    if (chosen("post/grading")) testGrading();
    if (chosen("materials/coat")) testCoat();
    if (chosen("materials/lightShapes")) testLightShapes();
    if (chosen("materials/profiles")) testProfiles();
    if (chosen("materials/moreMaterials")) testMoreMaterials();
    if (chosen("files/gltfMore")) testGltfMore();
    if (chosen("sky/sky")) testSky();
    if (chosen("sky/skyMore")) testSkyMore();
    if (chosen("post/lookMore")) testLookMore();
    if (chosen("materials/sizedLights")) testSizedLights();
    if (chosen("motion/motion")) testMotion();
    if (chosen("post/upscale")) testUpscale();
    if (chosen("motion/skinnedMotion")) testSkinnedMotion();
    if (chosen("motion/blurEdges")) testBlurEdges();
    if (chosen("shadows/softShadowsTurning")) testSoftShadowsTurning();
    if (chosen("post/temporalBytes")) testTemporalBytes();
    if (chosen("shadows/softShadows")) testSoftShadows();
    if (chosen("tracing/probeReflections")) testProbeReflections();
    if (chosen("core/cullSize")) testCullSize();
    if (chosen("scale/particleForces")) testParticleForces();
    if (chosen("scale/terrainLayers")) testTerrainLayers();
    if (chosen("scale/hlod")) testHlod();
    if (chosen("scale/skinLod")) testSkinLod();
    if (chosen("scale/swarm")) testSwarm();
    if (chosen("streaming/meshStreaming/software")) testMeshStreaming(ARTF_BACKEND_SOFTWARE);
    if (chosen("streaming/meshStreaming/vulkan")) testMeshStreaming(ARTF_BACKEND_VULKAN);
    if (chosen("tracing/traceHardware")) testTraceHardware();
    if (chosen("views/views")) testViews();
    if (chosen("backends/shadersAhead")) testShadersAhead();
    if (chosen("post/postKernels")) testPostKernels();
    if (chosen("views/passTimes")) testPassTimes();
    if (chosen("views/orthographic")) testOrthographic();
    if (chosen("views/pick")) testPick();
    if (chosen("tracing/tracedShadows")) testTracedShadows();
    if (chosen("tracing/tracedReflections")) testTracedReflections();
    if (chosen("tracing/tracedMoved")) testTracedMoved();
    if (chosen("tracing/tracedRefit")) testTracedRefit();
    if (chosen("tracing/probesWalls")) testProbesWalls();
    if (chosen("tracing/probesGpu")) testProbesGpu();
    if (chosen("shadows/extraShadows")) testExtraShadows();
    if (chosen("post/floatsSamples")) testFloatsSamples();
    if (chosen("layers/decals")) testDecals();
    if (chosen("scale/extras")) testExtras();
    if (chosen("backends/lostDevice")) testLostDevice();
    if (chosen("backends/refusedSwapchain")) testRefusedSwapchain();
    if (chosen("files/blocks")) testBlocks();
    if (chosen("streaming/prepared")) testPrepared();
    if (chosen("streaming/removing/software")) testRemoving(ARTF_BACKEND_SOFTWARE);
    if (chosen("streaming/removing/vulkan")) testRemoving(ARTF_BACKEND_VULKAN);
    if (chosen("streaming/residency/software")) testResidency(ARTF_BACKEND_SOFTWARE);
    if (chosen("streaming/residency/vulkan")) testResidency(ARTF_BACKEND_VULKAN);
    if (chosen("materials/graphs/software")) testGraphs(ARTF_BACKEND_SOFTWARE);
    if (chosen("materials/graphs/vulkan")) testGraphs(ARTF_BACKEND_VULKAN);
    if (chosen("materials/graphsBothBackends")) testGraphsBothBackends();
    if (chosen("blackhole/fluid")) testFluid();
    if (chosen("blackhole/backgroundPass")) testBackgroundPass();
    if (chosen("blackhole/blackHole")) testBlackHole();
    if (listing) return 0;
#ifdef ARTFUL_COUNT_ALLOCATIONS
    printf("allocations still alive: %ld\n", artfAllocationsAlive());
    CHECK(artfAllocationsAlive() == 0);
#endif
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
