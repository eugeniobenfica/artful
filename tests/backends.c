/* artful-tests: the GPU against the CPU, and a device that is lost or refuses. */
#include "tests.h"

void testVulkan(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) { puts("no Vulkan here: its tests were not run"); return; }
    printf("on %s\n", artfRendererDescription(gpu));
    o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    ArtfScene* s = richScene();
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);

    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(gpu, a));
    CHECK(artfRendererReadPixels(cpu, b));
    const double far = differentShare(a, b, 24);
    printf("pixels that differ by more than 24 of 255: %.2f%%\n", far * 100);
    CHECK(far < .02);

    /* the scene changes: what the GPU keeps follows */
    ArtfMesh more = artfSceneAddCube(s, 1);
    ArtfTransform t = artfTransformAt(0, 1.5f, 1, 1);
    ArtfMaterialDesc d = {artfVec3(.1f, .2f, .9f), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfInstance added = artfSceneAddInstance(s, more, artfSceneAddMaterial(s, &d), &t);
    for (int i = 0; i < 2000; ++i) { /* past what the buffers started with */
        ArtfTransform far2 = artfTransformAt((float)i, 0, 500, 1);
        artfSceneAddInstance(s, more, 0, &far2);
    }
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(gpu, a));
    CHECK(artfRendererReadPixels(cpu, b));
    CHECK(differentShare(a, b, 24) < .02);
    artfSceneRemoveInstance(s, added);
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(gpu, a));
    CHECK(artfRendererReadPixels(cpu, b));
    CHECK(differentShare(a, b, 24) < .02);

    /* the overlay, whole and then in part */
    uint32_t* over = artfCalloc((size_t)W * H, 4);
    for (int y = 10; y < 60; ++y)
        for (int x = 10; x < 110; ++x) over[y * W + x] = 0x80800000u; /* half red, premultiplied */
    ArtfOverlay overlay = {over, W, H, 0, false, NULL, 0};
    artfRendererSetOverlay(gpu, &overlay);
    artfRendererSetOverlay(cpu, &overlay);
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(gpu, a));
    CHECK(artfRendererReadPixels(cpu, b));
    CHECK(differentShare(a, b, 24) < .02);
    for (int y = 100; y < 130; ++y)
        for (int x = 200; x < 300; ++x) over[y * W + x] = 0xFF00FF00u;
    ArtfRect damage = {200, 100, 100, 30};
    ArtfOverlay partial = {over, W, H, 0, true, &damage, 1};
    artfRendererSetOverlay(gpu, &partial);
    artfRendererSetOverlay(cpu, &partial);
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(gpu, a));
    CHECK(artfRendererReadPixels(cpu, b));
    CHECK(a[110 * W + 250] == 0xFF00FF00u);
    CHECK(differentShare(a, b, 24) < .02);

    /* a size of its own */
    artfRendererResize(gpu, 200, 120);
    artfRendererSetOverlay(gpu, NULL);
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererReadPixels(gpu, a));

    artfFree(over);
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
    artfSceneFree(s);
}

/* What WebGPU and Metal draw past the plain frame, each a scene against the CPU's (the plan's 6.1 and 6.2): kept by
 * which backend has it yet. */
typedef ArtfScene* (*FeatureScene)(void);

/* A material of the program's own: waves by position and roughness by depth, on a sun's scene. */
static ArtfScene* graphFeatureScene(void) {
    ArtfScene* s = shadowScene();
    ArtfGraph* g = artfGraphNew();
    ArtfExpr p = artfExprPosition(g);
    ArtfExpr wave = artfExprSin(g, artfExprAdd(g, artfExprMul(g, artfExprX(g, p), artfExprNumber(g, 3)), artfExprParam(g, 0)));
    artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprMix(g, artfExprConst(g, .1f, .2f, .8f, 1), artfExprConst(g, .9f, .7f, .1f, 1), artfExprAdd(g, artfExprMul(g, wave, artfExprNumber(g, .5f)), artfExprNumber(g, .5f))));
    artfGraphSetOutput(g, ARTF_OUT_ROUGHNESS, artfExprFract(g, artfExprZ(g, p)));
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    d.graph = artfSceneAddGraph(s, g);
    artfGraphFree(g);
    const ArtfMaterial m = artfSceneAddMaterial(s, &d);
    artfInstanceSetMaterial(s, 1, m);
    artfMaterialSetParam(s, m, 0, 1.f, 0, 0, 0);
    return s;
}

/* A coat with sheen, on a ball in the sun (materials past glTF's core). */
static ArtfScene* coatFeatureScene(void) {
    ArtfScene* s = shadowScene();
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.5f, .05f, .05f);
    d.roughness = .8f;
    d.clearcoat = 1;
    d.clearcoatRoughness = .1f;
    d.sheen = artfVec3(.4f, .4f, .4f);
    d.sheenRoughness = .5f;
    artfInstanceSetMaterial(s, 1, artfSceneAddMaterial(s, &d));
    return s;
}

/* Soft shadows of a sun with a size. */
static ArtfScene* softFeatureScene(void) {
    ArtfScene* s = shadowScene();
    const ArtfShadowDesc soft = {1024, 0, true, false, false};
    artfSceneSetShadows(s, &soft);
    artfSceneSetSunSize(s, .05f);
    return s;
}

/* A light with a shape: a rectangle, facing down, over the floor. */
static ArtfScene* shapeFeatureScene(void) {
    ArtfScene* s = shadowScene();
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    const ArtfLight l = artfSceneAddLight(s, artfVec3(.5f, 1.2f, .5f), artfVec3(3, 3, 3), 6);
    artfLightSetShape(s, l, artfVec3(1, 0, 0), artfVec3(0, -1, 0), 1.5f, .5f);
    return s;
}

/* Light from all around, a sky from the sun. */
static ArtfScene* skyFeatureScene(void) {
    ArtfScene* s = shadowScene();
    const ArtfSkyDesc day = {{3, 3, 3}, 0, 0, {0, 0, 0}, 128, false, false};
    artfSceneSetSky(s, &day);
    return s;
}

static void otherGpuFeatures(ArtfRenderer* gpu, ArtfRenderer* cpu, ArtfBackend backend, uint32_t* a, uint32_t* b) {
    static const struct { const char* name; FeatureScene make; bool webgpu, metal; } features[] = {
        {"a material of its own", graphFeatureScene, true, false},
        {"layers of transparency", layeredScene, true, false},
        {"a coat with sheen", coatFeatureScene, true, false},
        {"soft shadows", softFeatureScene, true, false},
        {"a light with a shape", shapeFeatureScene, true, false},
        {"light from all around (a sky)", skyFeatureScene, true, false},
    };
    /* (and each shows: what is built on the sun's scene is not that scene, drawn on the same GPU) */
    uint32_t* base = artfMalloc((size_t)W * H * 4);
    ArtfScene* plain = shadowScene();
    drawSamples(gpu, plain, 2, base);
    artfSceneFree(plain);
    for (size_t k = 0; k < ARTF_COUNT(features); ++k) {
        if (!(backend == ARTF_BACKEND_WEBGPU ? features[k].webgpu : features[k].metal)) continue;
        ArtfScene* s = features[k].make();
        drawSamples(gpu, s, 2, a);
        drawSamples(cpu, s, 1, b);
        const double far = differentShare(a, b, 24), shows = features[k].make == layeredScene ? 1 : differentShare(a, base, 8);
        printf("%s against the CPU, %s: %.2f%% of pixels differ by more than 24 of 255 (%.1f%% from the sun's scene)\n", artfBackendName(backend), features[k].name, far * 100, shows * 100);
        CHECK(far < .02 && shows > .002);
        artfSceneFree(s);
    }
    artfFree(base);
}

/* WebGPU (the plan's 6.2) and Metal (6.1) against the CPU: a textured scene with its lights; a sun's shadows; what
 * has holes; the overlay; a size of its own; and what each draws past that (otherGpuFeatures). Where one is not built,
 * or has no device, its test is skipped. */
static void otherGpu(ArtfBackend backend, const char* what) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) { missing(what); return; }
    printf("on %s\n", artfRendererDescription(gpu));
    o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* scenes[3] = {richScene(), shadowScene(), artfSceneNew()};
    /* (the third: a fence of bars, cut out by alpha, over a floor) */
    {
        ArtfScene* s = scenes[2];
        static uint32_t bars[32 * 32];
        for (int i = 0; i < 32 * 32; ++i) bars[i] = (i % 32) / 4 % 2 ? 0xFFB04020u : 0x00000000u;
        ArtfMaterialDesc fence = {artfVec3(1, 1, 1), artfSceneAddTexture(s, bars, 32, 32, 0), .3f, .5f, artfVec3(0, 0, 0), ARTF_ALPHA_MASK, .5f, 0, true};
        addAt(s, artfSceneAddCube(s, 1.5f), artfSceneAddMaterial(s, &fence), 0, 0, 0, 1);
        ArtfMaterialDesc floorDesc = {artfVec3(.5f, .6f, .7f), 0, 0, .8f, artfVec3(0, 0, 0)};
        addAt(s, artfSceneAddPlane(s, 10, 1), artfSceneAddMaterial(s, &floorDesc), 0, -1, 0, 1);
        artfSceneAddLight(s, artfVec3(1, 3, 3), artfVec3(1, 1, 1), 20.f);
        setCamera(s, 1, 2, 5, 0, 0, 0);
    }
    static const char* names[3] = {"textures and lights", "a sun's shadows", "what has holes"};
    for (int k = 0; k < 3; ++k) {
        drawSamples(gpu, scenes[k], 2, a);
        drawSamples(cpu, scenes[k], 1, b);
        const double far = differentShare(a, b, 24);
        printf("%s against the CPU, %s: %.2f%% of pixels differ by more than 24 of 255\n", artfBackendName(backend), names[k], far * 100);
        CHECK(far < .02);
    }
    otherGpuFeatures(gpu, cpu, backend, a, b);
    if (backend == ARTF_BACKEND_WEBGPU) {
        /* traced: paths, sixteen frames; and with the filter */
        for (int filtered = 0; filtered < 2; ++filtered) {
            ArtfScene* s = coatFeatureScene();
            const ArtfTraceDesc paths = {ARTF_TRACE_PATHS, 1, 16, 0, filtered != 0};
            artfSceneSetTrace(s, &paths);
            for (int f = 0; f < 16; ++f) CHECK(artfRendererDraw(gpu, s));
            CHECK(artfRendererReadPixels(gpu, a));
            for (int f = 0; f < 16; ++f) CHECK(artfRendererDraw(cpu, s));
            CHECK(artfRendererReadPixels(cpu, b));
            const double far = differentShare(a, b, 24);
            printf("%s against the CPU, traced%s: %.2f%% of pixels differ by more than 24 of 255\n", artfBackendName(backend), filtered ? " with the filter" : "", far * 100);
            CHECK(far < .02);
            artfSceneFree(s);
        }
    }
    /* the overlay */
    uint32_t* over = artfCalloc((size_t)W * H, 4);
    for (int y = 10; y < 60; ++y)
        for (int x = 10; x < 110; ++x) over[y * W + x] = 0x80800000u;
    ArtfOverlay overlay = {over, W, H, 0, false, NULL, 0};
    artfRendererSetOverlay(gpu, &overlay);
    artfRendererSetOverlay(cpu, &overlay);
    drawSamples(gpu, scenes[0], 1, a);
    drawSamples(cpu, scenes[0], 1, b);
    CHECK(differentShare(a, b, 24) < .02);
    CHECK(CHANNEL(a[30 * W + 50], 16) > CHANNEL(b[30 * W + 50], 8) - 1);
    /* a size of its own */
    artfRendererSetOverlay(gpu, NULL);
    artfRendererResize(gpu, 200, 120);
    CHECK(artfRendererDraw(gpu, scenes[0]));
    CHECK(artfRendererReadPixels(gpu, a));
    for (int k = 0; k < 3; ++k) artfSceneFree(scenes[k]);
    artfFree(over);
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}
void testWebGpu(void) { otherGpu(ARTF_BACKEND_WEBGPU, "webgpu"); }

/* A frame in floats on WebGPU (the plan's 6.2) or Metal (6.1), against the CPU's: as it is; each effect; temporal
 * antialiasing and motion blur on a ball that moves; auto exposure; layers. */
static void otherGpuFloats(ArtfBackend backend, const char* what) {
    ArtfRendererOptions o = options(backend, 0, false);
    o.hdr = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) { missing(what); return; }
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    uint32_t* plain = artfMalloc((size_t)W * H * 4);
    static const struct { const char* name; ArtfPostDesc post; int frames; bool moving, layered, fog; double most; } cases[] = {
        {"as it is", {0}, 1, false, false, false, .03},
        {"bloom, ambient occlusion, reflections, depth of field", {.lens.bloom = .2f, .screen.occlusion = 1, .screen.reflections = 1, .lens.focus = 6, .lens.aperture = 6}, 1, false, false, false, .03},
        {"temporal antialiasing and motion blur, a ball moving", {.lens.bloom = .3f, .screen.temporal = true, .lens.motionBlur = .5f}, 6, true, false, false, .05},
        {"auto exposure", {.exposure.automatic = true, .exposure.speed = 1}, 4, false, false, false, .03},
        {"fog that light goes through", {.screen.volumetric = 1, .screen.scattering = .4f}, 1, false, false, true, .03},
        {"layers", {.lens.bloom = .1f}, 1, false, true, false, .03},
    };
    for (size_t k = 0; k < ARTF_COUNT(cases); ++k) {
        ArtfRenderer* both[2] = {gpu, cpu};
        uint32_t* into[2] = {a, b};
        for (int r = 0; r < 2; ++r) {
            ArtfScene* s = cases[k].layered ? layeredScene() : bleedScene();
            ArtfInstance ball = 0;
            if (cases[k].moving) ball = addAt(s, artfSceneAddSphere(s, .5f, 16, 24), whiteMaterial(s), 0, 0, 0, 1);
            artfSceneSetPost(s, &cases[k].post);
            if (cases[k].fog) artfSceneSetFog(s, artfVec3(.5f, .5f, .55f), .03f);
            for (int f = 0; f < cases[k].frames; ++f) {
                if (ball) {
                    const ArtfTransform t = artfTransformAt(.2f * (float)f, 0, 0, 1);
                    artfInstanceSetTransform(s, ball, &t);
                }
                CHECK(artfRendererDraw(both[r], s));
            }
            CHECK(artfRendererReadPixels(both[r], into[r]));
            artfSceneFree(s);
        }
        const double apart = differentShare(a, b, 16);
        /* (and each effect shows: the GPU's frame is not the one as it is) */
        const double changed = k ? differentShare(a, plain, 16) : 1;
        if (!k) memcpy(plain, a, (size_t)W * H * 4);
        printf("%s in floats against the CPU, %s: %.2f%% of pixels apart by more than 16 of 255 (%.1f%% from it as it is)\n", artfBackendName(backend), cases[k].name, apart * 100, k ? changed * 100 : 0.0);
        CHECK(apart < cases[k].most && changed > .01);
    }
    artfFree(plain);
    /* drawn at half the frame's size and made whole (with temporal antialiasing), as the CPU does */
    artfRendererFree(gpu);
    artfRendererFree(cpu);
    o = options(backend, 0, false);
    o.hdr = true;
    o.renderScale = .5f;
    gpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_SOFTWARE;
    cpu = artfRendererNew(&o);
    ArtfRenderer* both[2] = {gpu, cpu};
    uint32_t* into[2] = {a, b};
    for (int r = 0; r < 2 && gpu; ++r) {
        ArtfScene* s = bleedScene();
        const ArtfPostDesc temporal = {.screen.temporal = true};
        artfSceneSetPost(s, &temporal);
        for (int f = 0; f < 6; ++f) CHECK(artfRendererDraw(both[r], s));
        CHECK(artfRendererReadPixels(both[r], into[r]));
        artfSceneFree(s);
    }
    const double scaled = differentShare(a, b, 16);
    printf("%s in floats against the CPU, drawn at half its size: %.2f%% of pixels apart by more than 16 of 255\n", artfBackendName(backend), scaled * 100);
    CHECK(scaled < .03);
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}
void testWebGpuFloats(void) { otherGpuFloats(ARTF_BACKEND_WEBGPU, "webgpu"); }
void testMetal(void) { otherGpu(ARTF_BACKEND_METAL, "metal"); }

/* (Without Vulkan, what follows has nothing to run on: its switches are Vulkan's, and the tests are empty.) */
#ifdef ARTFUL_VULKAN
/* A GPU without non-uniform texture indexing (ARTFUL_UNIFORM_TEXTURES, as one): forty textured cubes, each its own
 * texture, a stripe of a color of its own, beside the plain GPU's and the CPU's. The plan's 6.3. */
void testUniformTextures(void) {
    ArtfScene* s = artfSceneNew();
    static uint32_t px[40][16 * 16];
    for (int k = 0; k < 40; ++k) {
        const uint32_t color = 0xFF000000u | (uint32_t)(k * 53 % 256) << 16 | (uint32_t)(k * 97 % 256) << 8 | (uint32_t)(255 - k * 6);
        for (int i = 0; i < 16 * 16; ++i) px[k][i] = (i / 16 + i % 16) % 4 < 2 ? color : 0xFF202020u;
        ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTexture(s, px[k], 16, 16, 0), 0, .6f, artfVec3(0, 0, 0)};
        addAt(s, artfSceneAddCube(s, .8f), artfSceneAddMaterial(s, &d), (float)(k % 8) - 3.5f, (float)(k / 8) - 2.f, 0, 1);
    }
    artfSceneAddLight(s, artfVec3(0, 2, 6), artfVec3(1, 1, 1), 40.f);
    setCamera(s, 0, 0, 9, 0, 0, 0);
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* plain = artfRendererNew(&o);
    if (!plain) { artfSceneFree(s); return; }
#ifdef _WIN32
    _putenv("ARTFUL_UNIFORM_TEXTURES=1");
#else
    setenv("ARTFUL_UNIFORM_TEXTURES", "1", 1);
#endif
    ArtfRenderer* uniform = artfRendererNew(&o);
#ifdef _WIN32
    _putenv("ARTFUL_UNIFORM_TEXTURES=");
#else
    unsetenv("ARTFUL_UNIFORM_TEXTURES");
#endif
    o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    CHECK(uniform && artfVulkanUniformTextures(uniform) && !artfVulkanUniformTextures(plain));
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    drawSamples(plain, s, 2, a);
    drawSamples(uniform, s, 2, b);
    drawSamples(cpu, s, 1, c);
    const double apart = differentShare(a, b, 2), fromCpu = differentShare(b, c, 24);
    printf("without non-uniform indexing, pixels that differ by more than 2 of 255 from with it: %.3f%%; from the CPU (24): %.2f%%\n", apart * 100, fromCpu * 100);
    CHECK(apart < .001);
    CHECK(fromCpu < .02);
    /* (and colored: the textures were read, not left white) */
    int colored = 0;
    for (int i = 0; i < W * H; ++i) colored += abs(CHANNEL(b[i], 16) - CHANNEL(b[i], 0)) > 40;
    CHECK(colored > W * H / 20);
    artfFree(a);
    artfFree(b);
    artfFree(c);
    artfRendererFree(plain);
    artfRendererFree(uniform);
    artfRendererFree(cpu);
    artfSceneFree(s);
}

/* A device lost in the middle of it all: false, and false after that; the renderer can be freed; and the scene
 * goes to a new one, which draws what the first did. */
/* What events say: each kind, how many, and what of the last. */
static int eventCounts[8];
static const void* eventSource;
static char eventWords[1024];
static void countEvent(void* user, const ArtfEvent* e) {
    (void)user;
    if ((int)e->kind < 8) ++eventCounts[e->kind];
    eventSource = e->source;
    snprintf(eventWords, sizeof eventWords, "%s", e->message);
}

void testLostDevice(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    drawInto(r, s, a);
    /* (what the GPU's memory holds for it is counted, and goes when what it holds goes) */
    ArtfFrameStats stats;
    artfRendererStats(r, &stats);
    const uint64_t heldBefore = stats.gpuBytes;
    uint32_t* big = artfMalloc((size_t)1024 * 1024 * 4);
    memset(big, 0x80, (size_t)1024 * 1024 * 4);
    const ArtfTexture texture = artfSceneAddTexture(s, big, 1024, 1024, 0);
    ArtfMaterialDesc textured = artfMaterialDefaults();
    textured.texture = texture;
    const ArtfTransform away = artfTransformAt(0, -50, 0, 1);
    const ArtfMaterial texturedMaterial = artfSceneAddMaterial(s, &textured);
    const ArtfInstance shown = artfSceneAddInstance(s, artfSceneAddCube(s, 1), texturedMaterial, &away);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    const uint64_t heldWith = stats.gpuBytes;
    artfSceneRemoveInstance(s, shown);
    textured.texture = 0;
    artfMaterialSet(s, texturedMaterial, &textured);
    CHECK(artfSceneRemoveTexture(s, texture));
    for (int k = 0; k < 4; ++k) CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    printf("the GPU's memory held: %.1f MB, %.1f with a texture of 4 MB, %.1f once it went\n", heldBefore / 1048576.0, heldWith / 1048576.0, stats.gpuBytes / 1048576.0);
    CHECK(heldBefore > 0 && heldWith >= heldBefore + 4 * 1048576 && stats.gpuBytes < heldWith);
    artfFree(big);
    drawInto(r, s, a);
    memset(eventCounts, 0, sizeof eventCounts);
    artfSetEvents(countEvent, NULL);
    artfVulkanSubmitsLeft = 2;
    CHECK(artfRendererDraw(r, s) && artfRendererDraw(r, s));
    CHECK(!artfRendererDraw(r, s));
    artfVulkanSubmitsLeft = -1;
    CHECK(!artfRendererDraw(r, s));
    /* (said once, by the renderer that lost it) */
    CHECK(eventCounts[ARTF_EVENT_DEVICE_LOST] == 1 && eventSource == r);
    printf("lost, %s: %s\n", artfVulkanFaultReports(r) ? "the driver asked why" : "no VK_EXT_device_fault", eventWords);
    /* (a loss made up asks the driver nothing: it may be asked only of a device really lost) */
    CHECK(strstr(eventWords, "(the driver: ") == NULL);
    artfSetEvents(NULL, NULL);
    CHECK(!artfRendererReadPixels(r, b));
    artfRendererFree(r);
    r = artfRendererNew(&o);
    CHECK(r != NULL);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfRendererFree(r);

    /* the same, while tracing */
    r = artfRendererNew(&o);
    const ArtfTraceDesc clean = {ARTF_TRACE_PATHS, 1, 0, 0, true};
    artfSceneSetTrace(s, &clean);
    CHECK(artfRendererDraw(r, s));
    artfVulkanSubmitsLeft = 0;
    CHECK(!artfRendererDraw(r, s));
    artfVulkanSubmitsLeft = -1;
    artfRendererFree(r);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
}

#ifdef _WIN32
__declspec(dllimport) void* __stdcall CreateWindowExA(unsigned long, const char*, const char*, unsigned long, int, int, int, int, void*, void*, void*, void*);
__declspec(dllimport) int __stdcall DestroyWindow(void*);
/* A driver that refuses a swapchain, first and on a resize: the window is free again for whatever comes next. */
void testRefusedSwapchain(void) {
    void* window = CreateWindowExA(0, "STATIC", "artful", 0xCF0000u, 0, 0, W, H, NULL, NULL, NULL, NULL);
    if (!window) { missing("window"); return; }
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.surface.kind = ARTF_SURFACE_WIN32;
    o.surface.a = window;
    ArtfScene* s = shadowScene();
    artfVulkanRefuseSwapchain = true;
    ArtfRenderer* r = artfRendererNew(&o);
    artfVulkanRefuseSwapchain = false;
    CHECK(r == NULL || !artfRendererDraw(r, s));
    artfRendererFree(r);
    r = artfRendererNew(&o);
    if (r) {
        CHECK(artfRendererDraw(r, s));
        artfVulkanRefuseSwapchain = true;
        artfRendererResize(r, W / 2, H / 2);
        CHECK(!artfRendererDraw(r, s));
        artfVulkanRefuseSwapchain = false;
        CHECK(!artfRendererDraw(r, s));
        artfRendererFree(r);
        r = artfRendererNew(&o);
        CHECK(r != NULL && artfRendererDraw(r, s));
        artfRendererFree(r);
    }
    artfSceneFree(s);
    DestroyWindow(window);
}
#else
void testRefusedSwapchain(void) { missing("window"); }
#endif

/* The GPU's memory run out, for a texture: it goes up without its largest mipmaps, or, when not even its smallest
 * fits, it is white; each said by an event, and the frame goes on. */
void testTextureMemory(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    ArtfScene* s = plainScene();
    static uint32_t red[256 * 256], green[256 * 256];
    for (int i = 0; i < 256 * 256; ++i) {
        red[i] = 0xFFFF0000u;
        green[i] = 0xFF00FF00u;
    }
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    drawInto(r, s, frame);
    memset(eventCounts, 0, sizeof eventCounts);
    artfSetEvents(countEvent, NULL);
    /* (room for 128 x 128 and what is under it, not for 256 x 256) */
    artfVulkanTextureMemoryLimit = 128 * 128 * 4 * 2;
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTexture(s, red, 256, 256, 0), 0, 1, artfVec3(0, 0, 0)};
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), -1.2f, 0, 0, 1);
    drawInto(r, s, frame);
    CHECK(eventCounts[ARTF_EVENT_OUT_OF_MEMORY] >= 1 && eventCounts[ARTF_EVENT_REFUSED] == 0 && eventCounts[ARTF_EVENT_DEVICE_LOST] == 0);
    const uint32_t left = frame[(H / 2) * W + W / 2 - 50];
    CHECK(CHANNEL(left, 16) > 200 && CHANNEL(left, 8) < 40); /* (red, from a smaller mipmap) */
    /* not even its smallest: white */
    artfVulkanTextureMemoryLimit = 0;
    d.texture = artfSceneAddTexture(s, green, 256, 256, 0);
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 1.2f, 0, 0, 1);
    drawInto(r, s, frame);
    CHECK(eventCounts[ARTF_EVENT_REFUSED] == 1 && eventCounts[ARTF_EVENT_DEVICE_LOST] == 0);
    const uint32_t right = frame[(H / 2) * W + W / 2 + 50];
    CHECK(CHANNEL(right, 16) > 200 && CHANNEL(right, 8) > 200 && CHANNEL(right, 0) > 200);
    /* and it is not tried again each frame */
    drawInto(r, s, frame);
    CHECK(eventCounts[ARTF_EVENT_REFUSED] == 1);
    artfVulkanTextureMemoryLimit = -1;
    artfSceneFree(s);
    /* with residency automatic, the textures there make room: a texture that did not fit is white at first, then, as
     * the others give up levels (artfSceneResidencyPress), there in its color */
    s = plainScene();
    const ArtfResidencyDesc automatic = {ARTF_RESIDENCY_AUTOMATIC, 0, 1, NULL, NULL};
    artfSceneSetResidency(s, &automatic);
    static uint32_t blue[512 * 512], yellow[256 * 256];
    for (int i = 0; i < 512 * 512; ++i) blue[i] = 0xFF0000FFu;
    for (int i = 0; i < 256 * 256; ++i) yellow[i] = 0xFFFFFF00u;
    /* (two of them near the eye, about 256 pixels each: they hold their second mipmap; and the square in front of them) */
    for (int k = 0; k < 2; ++k) {
        ArtfMaterialDesc b = {artfVec3(1, 1, 1), artfSceneAddTexture(s, blue, 512, 512, 0), 0, 1, artfVec3(0, 0, 0)};
        addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &b), k ? 1.4f : -1.4f, 0, 3, 1.4f);
    }
    /* (the square first, plain: what is short is the texture's room alone, 32 KB past what is there) */
    const ArtfInstance square = addAt(s, addSquare(s, 0, 0, 0, 0), whiteMaterial(s), 0, -.5f, 3.5f, .3f);
    for (int k = 0; k < 4; ++k) drawInto(r, s, frame);
    ArtfFrameStats stats;
    artfRendererStats(r, &stats);
    artfVulkanMemoryBudget = (long long)stats.gpuBytes + 32 * 1024;
    memset(eventCounts, 0, sizeof eventCounts);
    ArtfMaterialDesc y = {artfVec3(1, 1, 1), artfSceneAddTexture(s, yellow, 256, 256, 0), 0, 1, artfVec3(0, 0, 0)};
    artfInstanceSetMaterial(s, square, artfSceneAddMaterial(s, &y));
    CHECK(artfRendererDraw(r, s) && artfRendererReadPixels(r, frame));
    const uint32_t first = frame[(H / 2 + 61) * W + W / 2];
    for (int k = 0; k < 3; ++k) drawInto(r, s, frame);
    const uint32_t later = frame[(H / 2 + 61) * W + W / 2];
    printf("a texture past the GPU's memory, residency automatic: %s at first, %s once the others gave up levels (%u bytes the textures may hold now)\n", CHANNEL(first, 0) > 200 ? "white" : "yellow",
           CHANNEL(later, 0) < 80 ? "yellow" : "white", (unsigned)s->residencyCeiling);
    CHECK(eventCounts[ARTF_EVENT_OUT_OF_MEMORY] >= 1 && CHANNEL(first, 0) > 200 && s->residencyCeiling > 0);
    CHECK(CHANNEL(later, 16) > 200 && CHANNEL(later, 8) > 200 && CHANNEL(later, 0) < 80 && eventCounts[ARTF_EVENT_DEVICE_LOST] == 0);
    artfVulkanMemoryBudget = -1;
    artfSetEvents(NULL, NULL);
    artfFree(frame);
    artfSceneFree(s);
    artfRendererFree(r);
}

/* The GPU's memory run out for what is not a texture (artfVulkanMemoryBudget, as if the GPU held only so much):
 * - a mesh that does not fit is refused (ARTF_EVENT_REFUSED), not drawn, and the rest is; when a mesh goes, it is tried
 *   again, and drawn when it fits;
 * - instances that outgrow their room, and post buffers that do not fit, leave a frame undrawn, said, and the renderer
 *   goes on: when there is room again, it draws them. */
static int countGreen(const uint32_t* f) {
    int n = 0;
    for (int i = 0; i < W * H; ++i) n += CHANNEL(f[i], 8) > 150 && CHANNEL(f[i], 16) < 80;
    return n;
}
static void oomLog(void* u, const char* m) { (void)u; printf("LOG %s\n", m); }
void testGpuMemory(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    ArtfScene* s = plainScene();
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    const ArtfMaterial white = whiteMaterial(s);
    const ArtfMaterialDesc greenDesc = {artfVec3(0, 1, 0), 0, 0, 1, artfVec3(0, 0, 0)};
    const ArtfMaterial green = artfSceneAddMaterial(s, &greenDesc);
    addAt(s, addSquare(s, 0, 0, 0, 0), white, -1.5f, 0, 0, .8f);
    const ArtfMesh spare = addSquare(s, 0, 0, 0, 0);
    drawInto(r, s, frame);
    memset(eventCounts, 0, sizeof eventCounts);
    artfSetEvents(countEvent, NULL);
    ArtfFrameStats stats;
    artfRendererStats(r, &stats);
    /* (room for what is there and a little: not for a ball of 600,000 triangles) */
    artfVulkanMemoryBudget = (long long)stats.gpuBytes + 4 * 1024 * 1024;
    const ArtfMesh ball = artfSceneAddSphere(s, 1, 512, 600);
    addAt(s, ball, green, 1.2f, 0, 0, 1);
    CHECK(artfRendererDraw(r, s) && artfRendererDraw(r, s) && artfRendererReadPixels(r, frame));
    const uint32_t left = frame[(H / 2) * W + W / 2 - 75];
    printf("a mesh past the GPU's memory: refused %d times, %d of its pixels drawn, the square beside it %s\n", eventCounts[ARTF_EVENT_REFUSED], countGreen(frame),
           CHANNEL(left, 8) > 150 ? "drawn" : "not drawn");
    /* (the ball, and its lighter levels: each refused once) */
    const int refused = eventCounts[ARTF_EVENT_REFUSED];
    CHECK(refused >= 1 && eventCounts[ARTF_EVENT_DEVICE_LOST] == 0 && countGreen(frame) == 0 && CHANNEL(left, 8) > 150);
    /* not tried again each frame; when a mesh goes, it is, and with room it is drawn */
    drawInto(r, s, frame);
    CHECK(eventCounts[ARTF_EVENT_REFUSED] == refused);
    artfVulkanMemoryBudget = -1;
    artfSceneRemoveMesh(s, spare);
    drawInto(r, s, frame);
    CHECK(countGreen(frame) > 1000);
    /* instances past their room: a frame given up (the one before stays: not lost), then drawn */
    artfRendererStats(r, &stats);
    artfVulkanMemoryBudget = (long long)stats.gpuBytes;
    memset(eventCounts, 0, sizeof eventCounts);
    for (int k = 0; k < 4000; ++k) addAt(s, ball, green, 40.f + (float)k, 0, 0, .1f);
    const bool drawnWithout = artfRendererDraw(r, s);
    CHECK(drawnWithout && eventCounts[ARTF_EVENT_OUT_OF_MEMORY] >= 1 && eventCounts[ARTF_EVENT_DEVICE_LOST] == 0);
    artfVulkanMemoryBudget = -1;
    drawInto(r, s, frame);
    CHECK(countGreen(frame) > 1000);
    printf("instances past the GPU's memory: %d events, the device %s; then drawn\n", eventCounts[ARTF_EVENT_OUT_OF_MEMORY], eventCounts[ARTF_EVENT_DEVICE_LOST] ? "lost" : "not lost");
    artfSetEvents(NULL, NULL);
    artfFree(frame);
    artfSceneFree(s);
    artfRendererFree(r);
    /* post buffers that do not fit: with floats and temporal antialiasing, at once */
    o.hdr = true;
    r = artfRendererNew(&o);
    if (!r) return;
    s = plainScene();
    frame = artfMalloc((size_t)W * H * 4);
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &greenDesc), 0, 0, 0, 1);
    drawInto(r, s, frame);
    artfRendererStats(r, &stats);
    artfVulkanMemoryBudget = (long long)stats.gpuBytes;
    const ArtfPostDesc temporal = {.screen.temporal = true, .screen.occlusion = 1};
    artfSceneSetPost(s, &temporal);
    memset(eventCounts, 0, sizeof eventCounts);
    artfSetEvents(countEvent, NULL);
    const bool postDrawn = artfRendererDraw(r, s);
    artfVulkanMemoryBudget = -1;
    drawInto(r, s, frame);
    printf("post buffers past the GPU's memory: %d events, the device %s; then drawn with %d green pixels\n", eventCounts[ARTF_EVENT_OUT_OF_MEMORY],
           eventCounts[ARTF_EVENT_DEVICE_LOST] ? "lost" : "not lost", countGreen(frame));
    CHECK(postDrawn && eventCounts[ARTF_EVENT_OUT_OF_MEMORY] >= 1 && eventCounts[ARTF_EVENT_DEVICE_LOST] == 0 && countGreen(frame) > 1000);
    artfSetEvents(NULL, NULL);
    artfFree(frame);
    artfSceneFree(s);
    artfRendererFree(r);
}

/* Vulkan's pixel shaders of another variant, made off the frame: a view asked for after the first frame comes a few
 * frames later, and after artfRendererWarm the frame is the one that waiting for them makes. */
void testShadersAhead(void) {
    ArtfRenderer* r = artfRendererNew(&(ArtfRendererOptions){.backend = ARTF_BACKEND_VULKAN, .width = W, .height = H});
    if (!r) return;
    ArtfScene* s = plainScene();
    ArtfMaterialDesc d = {artfVec3(.36f, .64f, .04f), 0, 0, .6f, artfVec3(.36f, 0, 0)};
    addAt(s, addSquare(s, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
    artfSceneAddLight(s, artfVec3(0, 0, 1), artfVec3(1, 1, 1), 10.f);
    uint32_t* waited = artfMalloc(W * H * 4);
    uint32_t* ahead = artfMalloc(W * H * 4);
    drawInto(r, s, waited);
    artfRendererSetView(r, ARTF_VIEW_NORMALS);
    drawInto(r, s, waited); /* (artfVulkanShadersWait: made before the frame) */
    artfRendererSetView(r, ARTF_VIEW_LIT);
    drawInto(r, s, ahead);
    artfVulkanShadersWait = false;
    memset(eventCounts, 0, sizeof eventCounts);
    artfSetEvents(countEvent, NULL);
    artfRendererSetView(r, ARTF_VIEW_NORMALS);
    double longest = 0;
    for (int i = 0; i < 3; ++i) {
        const double t0 = artfNowMs();
        CHECK(artfRendererDraw(r, s));
        const double took = artfNowMs() - t0;
        if (took > longest) longest = took;
    }
    artfRendererWarm(r);
    drawInto(r, s, ahead);
    CHECK(eventCounts[ARTF_EVENT_SHADERS_READY] == 1 && eventSource == r); /* (the frame after they were made) */
    artfSetEvents(NULL, NULL);
    printf("pixel shaders made off the frame: the longest frame after asking for a view %.2f ms\n", longest);
    CHECK(!memcmp(waited, ahead, W * H * 4));
    artfVulkanShadersWait = true;
    artfFree(waited);
    artfFree(ahead);
    artfSceneFree(s);
    artfRendererFree(r);
}

/* What else a feature turned on mid-run asks Vulkan to make: shadows from rays, a material of the program's own, the
 * tracing shader, temporal antialiasing and motion blur, with their passes. Made off the frame, frames go on until
 * they are made: the first after asking is the one before it (with the shadow maps, rasterized, without antialiasing
 * over time or blur), or for the material, the one its own values without the graph make; after artfRendererWarm, the
 * frame is the one that waiting for them makes. The longest frame after asking, each way. */
enum { AHEAD_TRACED, AHEAD_MATERIAL, AHEAD_TRACING, AHEAD_TEMPORAL, AHEAD_BLUR, AHEAD_COUNT };

/* plain: the material without its graph */
static void aheadTurnOn(ArtfScene* s, int k, bool plain) {
    if (k == AHEAD_TRACED) {
        const ArtfShadowDesc traced = {.resolution = 1024, .traced = true};
        artfSceneSetShadows(s, &traced);
    } else if (k == AHEAD_MATERIAL) {
        ArtfMaterialDesc d = {artfVec3(.2f, .5f, .9f), 0, 0, .7f, artfVec3(0, 0, 0)};
        if (!plain) {
            ArtfGraph* g = artfGraphNew();
            artfGraphSetOutput(g, ARTF_OUT_COLOR, stripes(g, 4.f));
            d.graph = artfSceneAddGraph(s, g);
            artfGraphFree(g);
        }
        addAt(s, addSquare(s, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu), artfSceneAddMaterial(s, &d), 1.5f, 1, 2, 1.5f);
    } else if (k == AHEAD_TRACING) {
        const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 0, 2};
        artfSceneSetTrace(s, &rays);
    } else {
        const ArtfPostDesc post = {.screen.temporal = k == AHEAD_TEMPORAL, .lens.motionBlur = k == AHEAD_BLUR ? .5f : 0};
        artfSceneSetPost(s, &post);
    }
}

typedef struct Ahead {
    double longest;    /* from asking, to artfRendererWarm when ahead */
    double taking;     /* ahead: the frame after artfRendererWarm, that takes what was made */
    uint32_t temporal; /* the frames temporal antialiasing took */
} Ahead;

/* ahead: frames go on while what the feature asks for is made (standIn: the first, or the frame before asking), then
 * artfRendererWarm and three frames. Otherwise made before the frame that asks, and as many frames from there as ahead
 * took (three, or until temporal antialiasing has taken as many); plain: aheadTurnOn's. */
static bool aheadRun(int k, bool ahead, bool plain, uint32_t* frame, uint32_t* standIn, Ahead* a) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return false;
    ArtfScene* s = shadowScene();
    const ArtfShadowDesc maps = {.resolution = 1024};
    artfSceneSetShadows(s, &maps);
    for (int i = 0; i < 3; ++i) CHECK(artfRendererDraw(r, s));
    uint32_t* before = artfMalloc(W * H * 4);
    CHECK(artfRendererReadPixels(r, before));
    artfVulkanShadersWait = !ahead;
    aheadTurnOn(s, k, plain);
    a->longest = 0;
    for (int i = 0; i < (ahead ? 4 : 1); ++i) {
        const double t0 = artfNowMs();
        CHECK(artfRendererDraw(r, s)); /* (frames go on) */
        a->longest = fmax(a->longest, artfNowMs() - t0);
        /* (the first frame after asking cannot have taken what it started) */
        if (ahead && !i) {
            CHECK(artfRendererReadPixels(r, standIn));
            if (k != AHEAD_MATERIAL) CHECK(!memcmp(before, standIn, W * H * 4));
        }
    }
    if (ahead) {
        artfRendererWarm(r);
        const double t0 = artfNowMs();
        CHECK(artfRendererDraw(r, s));
        a->taking = artfNowMs() - t0;
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererDraw(r, s));
        a->temporal = artfVulkanTemporalFrames(r);
    } else if (a->temporal) {
        while (artfVulkanTemporalFrames(r) < a->temporal) CHECK(artfRendererDraw(r, s));
    } else {
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererDraw(r, s));
    }
    CHECK(artfRendererReadPixels(r, frame));
    artfVulkanShadersWait = true;
    artfFree(before);
    artfSceneFree(s);
    artfRendererFree(r);
    return true;
}

void testPipelinesAhead(void) {
    static const char* const names[AHEAD_COUNT] = {"shadows from rays", "a material of the program's own", "tracing", "temporal antialiasing", "motion blur"};
    uint32_t* waited = artfMalloc(W * H * 4);
    uint32_t* ahead = artfMalloc(W * H * 4);
    uint32_t* standIn = artfMalloc(W * H * 4);
    for (int k = 0; k < AHEAD_COUNT; ++k) {
        Ahead a, w;
        /* (ahead first: the driver's own cache is colder for it) */
        if (!aheadRun(k, true, false, ahead, standIn, &a)) break;
        w.temporal = a.temporal;
        aheadRun(k, false, false, waited, NULL, &w);
        printf("%s turned on: the longest frame %.2f ms made off the frame (%.2f taking them), %.2f ms made before it\n", names[k], a.longest, a.taking, w.longest);
        CHECK(!memcmp(waited, ahead, W * H * 4));
        if (k == AHEAD_MATERIAL) {
            /* until its shader is made, the material is drawn by its own values, as without its graph */
            aheadRun(k, false, true, waited, NULL, &w);
            CHECK(!memcmp(waited, standIn, W * H * 4));
            CHECK(memcmp(ahead, standIn, W * H * 4));
        }
    }
    artfFree(waited);
    artfFree(ahead);
    artfFree(standIn);
}
#else
void testLostDevice(void) { missing("vulkan"); }
void testUniformTextures(void) { missing("vulkan"); }
void testTextureMemory(void) { missing("vulkan"); }
void testGpuMemory(void) { missing("vulkan"); }
void testRefusedSwapchain(void) { missing("vulkan"); }
void testShadersAhead(void) { missing("vulkan"); }
void testPipelinesAhead(void) { missing("vulkan"); }
#endif

#ifdef ARTFUL_VULKAN
#include "shaderir/ir.h"
#include "shaderir/msl.h"
#include "shaderir/wgsl.h"
#include "vulkan/shaders.h"
#include "vulkan/spirv.h"

/* Every shader the tests build, written as Metal too (shaderir/msl.h): all but ray queries must be. */
int metalFailures = 0;
void writeMetal(const IrModule* m) {
    char why[256];
    char* metal = irMsl(m, NULL, NULL, why, sizeof why);
    if (metal) artfFree(metal);
    else if (!strstr(why, "ray queries")) {
        ++metalFailures;
        printf("a shader not written as Metal: %s\n", why);
    }
}
#endif

/* Shaders as an IR (shaderir/ir.h): what the SPIR-V writer builds, read back as instructions with their kinds and
 * types; and what is wrong with a module, found. (That every shader's IR is sound and writes the words it wrote before
 * is checked by artful-shaders, and while these tests run: artfSpvCheck.) */
void testShaderIr(void) {
#ifdef ARTFUL_VULKAN
    Spv b, *s = &b;
    spvInit(s);
    SpvId out = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, out, SPV_DECORATE_Location, 0);
    SpvId main = spvBeginMain(s);
    SpvId half = spvConstFloat(s, .5f);
    SpvId sum = spvOp2(s, SPV_OP_FAdd, s->floatType, half, half);
    const SpvId parts[4] = {sum, sum, sum, half};
    spvStore(s, out, spvCompose(s, s->vec4, parts, 4));
    spvEndMain(s);
    spvEntryPoint(s, SPV_MODEL_Fragment, main, &out, 1);
    char why[256];
    CHECK(irCheck(&s->ir, why, sizeof why));
    /* the sum's type and what made it; the constant's value a literal, the entry point's name a string */
    const IrInst* add = irDef(&s->ir, sum);
    CHECK(add && add->op == SPV_OP_FAdd && irTypeOf(&s->ir, sum) == s->floatType && irKinds(&s->ir, add)[0] == IR_OPERAND_ID);
    const IrInst* constant = irDef(&s->ir, half);
    CHECK(constant && irKinds(&s->ir, constant)[0] == IR_OPERAND_LITERAL && irOperands(&s->ir, constant)[0] == 0x3F000000u);
    bool named = false;
    for (size_t k = 0; k < s->ir.insts.len; ++k) {
        const IrInst* i = &s->ir.insts.items[k];
        if (i->op == 15) named = irKinds(&s->ir, i)[0] == IR_OPERAND_LITERAL && irKinds(&s->ir, i)[1] == IR_OPERAND_ID && irKinds(&s->ir, i)[2] == IR_OPERAND_STRING &&
                                 irKinds(&s->ir, i)[i->count - 1] == IR_OPERAND_ID;
    }
    CHECK(named);
    /* as Metal: the entry point and what goes out, and the arithmetic */
    char* metal = irMsl(&s->ir, NULL, NULL, why, sizeof why);
    CHECK(metal && strstr(metal, "fragment main0_out main0(") && strstr(metal, "[[color(0)]]") && strstr(metal, "0.5f + 0.5f") && strstr(metal, "return out;"));
    artfFree(metal);
    /* as WGSL: the same, in its words (shaderir/wgsl.h) */
    char* wgsl = irWgsl(&s->ir, NULL, NULL, why, sizeof why);
    CHECK(wgsl && strstr(wgsl, "@fragment") && strstr(wgsl, "@location(0) m_") && strstr(wgsl, "0.5f + 0.5f") && strstr(wgsl, "return o;"));
    artfFree(wgsl);
    /* written again from the IR: the same words, and their header */
    size_t words;
    uint32_t* code = irSpirv(&s->ir, &words);
    CHECK(code[0] == 0x07230203 && code[3] == s->ir.bound + 1 && words > 5);
    artfFree(code);
    /* what is wrong is found: an id nothing makes; a block that does not end; a type that is not one */
    spvEmit(&s->code, SPV_OP_FAdd, SPV_WORDS(s->floatType, spvNewId(s), half, 999u));
    CHECK(!irCheck(&s->ir, why, sizeof why) && strstr(why, "made by nothing"));
    printf("an IR with what is wrong: %s\n", why);
    spvFini(s);
    spvInit(s);
    spvBeginMain(s);
    spvEmit(&s->code, 56 /* OpFunctionEnd */, NULL, 0);
    CHECK(!irCheck(&s->ir, why, sizeof why) && strstr(why, "inside a block"));
    spvFini(s);
    spvInit(s);
    spvBeginMain(s);
    spvOp2(s, SPV_OP_FAdd, spvConstFloat(s, 1.f), spvConstFloat(s, 1.f), spvConstFloat(s, 1.f));
    spvEndMain(s);
    CHECK(!irCheck(&s->ir, why, sizeof why) && strstr(why, "not a type"));
    spvFini(s);
#endif
}

#ifdef ARTFUL_VULKAN
#include "vulkan/frameGraph.h"
#endif

/* The GPU's frame as a graph (vulkan/frameGraph.h): every pass a frame takes is described, in its order, with what its
 * shaders read and write as their IR says, whether every effect is on or only some, and with occlusion's two halves;
 * and what is said of memory adds up. */
void testFrameGraph(void) {
#ifdef ARTFUL_VULKAN
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    ArtfScene* s = shadowScene();
    const ArtfPostDesc all = {.lens.bloom = .2f, .lens.focus = 6, .lens.aperture = 4, .lens.motionBlur = .5f, .screen.occlusion = 1, .screen.reflections = 1,
                              .screen.temporal = true, .screen.sharpen = .5f, .exposure.automatic = true};
    const ArtfPostDesc some = {.lens.bloom = .2f};
    const ArtfPostDesc* each[2] = {&all, &some};
    for (int k = 0; k < 4; ++k) {
        artfSceneSetPost(s, each[k & 1]);
        artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, k >= 2);
        for (int f = 0; f < 3; ++f) CHECK(artfRendererDraw(r, s));
        const uint64_t saved = artfFrameGraphReport(r, NULL, NULL);
        CHECK(artfFrameGraphUndescribed == 0);
        if (k == 0) {
            /* (occlusion and velocities already live in the scene's light: what is left to share is the rest) */
            uint64_t now[FG_TARGETS];
            artfVulkanTargetBytes(r, now);
            printf("the frame as a graph, every effect on: occlusion and velocities in the scene's light (%.1f MB of their own), "
                   "%.1f MB more could share memory\n", (double)now[FG_OCCLUSION] / 1048576.0, (double)saved / 1048576.0);
            CHECK(now[FG_OCCLUSION] == 0 && now[FG_FLOAT_COLOR] > 0);
        }
    }
    uint64_t bytes[FG_TARGETS], sum = 0;
    artfVulkanTargetBytes(r, bytes);
    for (int t = 0; t < FG_TARGETS; ++t) sum += bytes[t];
    ArtfFrameStats stats;
    artfRendererStats(r, &stats);
    CHECK(sum > 0 && sum < stats.gpuBytes); /* (what the graph counts is part of what the GPU holds) */
    artfRendererFree(r);
    artfSceneFree(s);
#endif
}

/* SPIR-V read back into the IR (irFromSpirv): every shader's words, read and written again, the same words; and what
 * a post pass reads and writes, from its words alone. */
#ifdef ARTFUL_VULKAN
static uint32_t* postFinalTurned(size_t* size) { return artfShaderPostFinal(size, true); } /* (preRotate's) */
#endif
void testSpirvRoundTrip(void) {
#ifdef ARTFUL_VULKAN
    uint32_t* (*const shaders[6])(size_t*) = {artfShaderPostTemporal, artfShaderPostBloomDown, postFinalTurned, artfShaderPostMotionBlur, artfShaderShadowVertex, artfShaderSkin};
    for (int k = 0; k < 6; ++k) {
        size_t size;
        uint32_t* words = shaders[k](&size);
        IrModule m;
        irInit(&m);
        CHECK(irFromSpirv(&m, words, size / 4));
        char why[256];
        CHECK(irCheck(&m, why, sizeof why));
        size_t again;
        uint32_t* back = irSpirv(&m, &again);
        CHECK(again == size / 4 && !memcmp(back, words, size));
        if (k == 0) {
            /* temporal antialiasing: reads the light (set 1, 0) and the history (1, 2), writes the second (1, 1) */
            IrUse uses[32];
            const int n = irUses(&m, uses, 32);
            bool light = false, history = false, second = false;
            for (int i = 0; i < n && i < 32; ++i) {
                if (uses[i].set == 1 && uses[i].binding == 0) light = uses[i].read && !uses[i].write;
                if (uses[i].set == 1 && uses[i].binding == 2) history = uses[i].read && !uses[i].write;
                if (uses[i].set == 1 && uses[i].binding == 1) second = uses[i].write;
            }
            CHECK(light && history && second);
        }
        artfFree(back);
        artfFree(words);
        irFree(&m);
    }
#endif
}

/* Both backends' frames keep to one graph of passes (render/framePasses.h): every pass they mark is in it, and none reads
 * what a pass after it in the frame writes; with every effect, with occlusion, with layers, shadows and fog, and traced. */
#include "render/framePasses.h"
void testFramePasses(void) {
    /* (orders the graph does not allow are found: the scene before the shadows it reads, the gather before the scene) */
    static const char* const good[] = {"cull", "shadows", "fog grid", "scene", "gather", "bloom", "tone map and out"};
    static const char* const early[] = {"cull", "scene", "shadows", "gather", "tone map and out"};
    static const char* const late[] = {"cull", "gather", "scene", "tone map and out"};
    static const char* const unknown[] = {"cull", "something else"};
    char said[256];
    CHECK(artfFramePassesOrder(good, 7, ARTF_BACKEND_VULKAN, said, sizeof said) == 0);
    CHECK(artfFramePassesOrder(early, 5, ARTF_BACKEND_VULKAN, said, sizeof said) > 0 && strstr(said, "shadows"));
    CHECK(artfFramePassesOrder(late, 4, ARTF_BACKEND_VULKAN, said, sizeof said) > 0 && strstr(said, "gather"));
    CHECK(artfFramePassesOrder(unknown, 2, ARTF_BACKEND_SOFTWARE, said, sizeof said) == 1);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) {
            missing("vulkan");
            continue;
        }
        ArtfScene* s = shadowScene();
        ArtfMaterialDesc glass = artfMaterialDefaults();
        glass.alphaMode = ARTF_ALPHA_BLEND;
        glass.transparency = .5f;
        addAt(s, artfSceneAddSphere(s, .4f, 12, 16), artfSceneAddMaterial(s, &glass), 0, .5f, 1, 1);
        const ArtfPostDesc all = {.lens.bloom = .2f, .lens.focus = 6, .lens.aperture = 4, .lens.motionBlur = .5f, .screen.occlusion = 1, .screen.reflections = 1,
                                  .screen.temporal = true, .screen.sharpen = .5f, .exposure.automatic = true, .screen.volumetric = 1};
        artfSceneSetPost(s, &all);
        artfSceneSetFog(s, artfVec3(.5f, .5f, .55f), .05f);
        int wrong = 0, frames = 0;
        char why[256] = "";
        for (int k = 0; k < 3; ++k) {
            artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, k == 1);
            if (k == 2) {
                const ArtfTraceDesc paths = {ARTF_TRACE_PATHS, 1, 0, 2, false, false};
                artfSceneSetTrace(s, &paths);
            }
            for (int f = 0; f < 3; ++f) {
                CHECK(artfRendererDraw(r, s));
                char said[256];
                const int n = artfFramePassesCheck(r, said, sizeof said);
                if (n && !wrong) memcpy(why, said, sizeof why);
                wrong += n;
                ++frames;
            }
        }
        printf("%s, %d frames against the graph of passes: %d out of it%s%s\n", backend ? "GPU" : "CPU", frames, wrong, wrong ? ", the first: " : "", why);
        CHECK(wrong == 0);
        artfSceneFree(s);
        artfRendererFree(r);
    }
}

#ifdef ARTFUL_VULKAN
extern uint32_t artfVulkanDevicesLost;
#endif
/* After a few devices the driver lost, no renderer is made on the GPU again in the process: Vulkan by name gives none,
 * and the automatic choice the CPU. (Without making the GPU lose anything: the count is set.) */
void testGpuLeftAlone(void) {
#ifdef ARTFUL_VULKAN
    const uint32_t was = artfVulkanDevicesLost;
    artfVulkanDevicesLost = 3;
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    CHECK(!r);
    o.backend = ARTF_BACKEND_AUTO;
    r = artfRendererNew(&o);
    CHECK(r && artfRendererBackend(r) == ARTF_BACKEND_SOFTWARE);
    artfRendererFree(r);
    artfVulkanDevicesLost = was;
#else
    missing("no Vulkan in this build");
#endif
}
