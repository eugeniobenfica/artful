/* artful-tests: shadows: maps, cascades, soft, and lights past the sixteen. */
#include "tests.h"

void testShadows(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);

    /* the maps come close to what rays say; with none, nothing is dark */
    ArtfScene* s = shadowScene();
    drawInto(r, s, a);
    const ArtfShadowDesc desc = {1024, 40};
    artfSceneSetShadows(s, &desc);
    drawInto(r, s, b);
    int darker = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) darker += CHANNEL(b[i], 8) + 8 < CHANNEL(a[i], 8);
    CHECK(darker > 600);
    ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 32, 0};
    artfSceneSetTrace(s, &rays);
    drawSamples(r, s, 32, a);
    artfSceneSetTrace(s, NULL);
    double far = differentShare(a, b, 40);
    printf("the sun's shadows against rays, pixels that differ by more than 40 of 255: %.2f%%\n", far * 100);
    CHECK(far < .015);
    artfSceneFree(s);

    /* a spot, and a point light: what stands next to them throws the same shadow as a ray says */
    for (int kind = 0; kind < 2; ++kind) {
        s = shadowScene();
        artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
        ArtfLight l = artfSceneAddLight(s, artfVec3(2.5f, 4, 1), artfVec3(2, 2, 2), 20.f);
        if (kind) artfLightSetSpot(s, l, artfVec3(-.45f, -1, -.2f), .5f, .8f);
        artfLightSetShadow(s, l, true);
        artfSceneSetShadows(s, &desc);
        drawInto(r, s, b);
        artfSceneSetTrace(s, &rays);
        drawSamples(r, s, 32, a);
        artfSceneSetTrace(s, NULL);
        far = differentShare(a, b, 40);
        printf("%s against rays, pixels that differ by more than 40 of 255: %.2f%%\n", kind ? "a spot" : "a point light", far * 100);
        CHECK(far < .02);
        artfSceneFree(s);
    }

    /* holes let light through: half a square throws half a shadow, as rays say */
    s = artfSceneNew();
    setCamera(s, 0, 8, .01f, 0, 0, 0);
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    artfSceneSetSun(s, artfVec3(-.8f, -1, 0), artfVec3(1, 1, 1));
    ArtfMaterialDesc matte = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 1), artfSceneAddMaterial(s, &matte), &t);
    static uint32_t half[64 * 64];
    for (int i = 0; i < 64 * 64; ++i) half[i] = i % 64 < 32 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    ArtfMaterialDesc cut = {artfVec3(1, 1, 1), artfSceneAddTexture(s, half, 64, 64, 0), 0, 1, artfVec3(0, 0, 0), ARTF_ALPHA_MASK, 0, 0, true};
    const ArtfTransform flat = artfTransformTrs(artfVec3(0, 1, 0), artfQuatAxisAngle(artfVec3(1, 0, 0), -1.5708f), artfVec3(1.5f, 1.5f, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &cut), &flat);
    artfSceneSetShadows(s, &desc);
    drawInto(r, s, b);
    artfSceneSetTrace(s, &rays);
    drawSamples(r, s, 32, a);
    artfSceneSetTrace(s, NULL);
    far = differentShare(a, b, 40);
    printf("a shadow with holes against rays, pixels that differ by more than 40 of 255: %.2f%%\n", far * 100);
    CHECK(far < .01);
    artfSceneFree(s);

    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

void testShadowsBothBackends(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    const ArtfShadowDesc desc = {1024, 40};
    for (int kind = 0; kind < 3; ++kind) {
        ArtfScene* s = shadowScene();
        if (kind) {
            ArtfLight l = artfSceneAddLight(s, artfVec3(2.5f, 4, 1), artfVec3(1, 1, 1), 20.f);
            if (kind == 2) artfLightSetSpot(s, l, artfVec3(-.45f, -1, -.2f), .5f, .8f);
            artfLightSetShadow(s, l, true);
        }
        artfSceneSetShadows(s, &desc);
        drawInto(gpu, s, a);
        drawInto(cpu, s, b);
        const double far = differentShare(a, b, 24);
        printf("shadows, %d: pixels that differ by more than 24 of 255: %.2f%%\n", kind, far * 100);
        CHECK(far < .02);
        artfSceneFree(s);
    }
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}

void testShadowBytes(void) {
    ArtfScene* s = shadowScene();
    const ArtfShadowDesc desc = {512, 40};
    artfSceneSetShadows(s, &desc);
    ArtfLight l = artfSceneAddLight(s, artfVec3(2.5f, 4, 1), artfVec3(1, 1, 1), 20.f);
    artfLightSetShadow(s, l, true);
    /* more than fits gets none, and says so */
    artfSetLog(countLog, &logged);
    logged = 0;
    for (int i = 0; i < 3; ++i) artfLightSetShadow(s, artfSceneAddLight(s, artfVec3((float)i, 3, 0), artfVec3(.2f, .2f, .2f), 9.f), true);
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {3, ARTF_SIMD_128}};
    uint64_t reference = 0;
    for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
        o.simd = runs[i].simd;
        ArtfRenderer* r = artfRendererNew(&o);
        drawInto(r, s, frame);
        const uint64_t h = hashOf(frame, (size_t)W * H);
        if (!i) reference = h;
        CHECK(h == reference);
        artfRendererFree(r);
    }
    CHECK(logged > 0);
    artfSetLog(NULL, NULL);
    artfFree(frame);
    artfSceneFree(s);
}

/* A map that did not change stays as it is; one that did comes out as a new renderer draws it. */
void testShadowCache(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    ArtfMaterialDesc m = {artfVec3(.8f, .8f, .8f), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfTransform at = artfTransformAt(2, 0, 2, 1);
    const ArtfInstance moving = artfSceneAddInstance(s, artfSceneAddCube(s, 1), artfSceneAddMaterial(s, &m), &at);
    for (int i = 0; i < 40; ++i) addAt(s, artfSceneAddCube(s, .3f), 0, (float)(i % 8) * 12.f - 40.f, 0, (float)(i / 8) * 12.f - 30.f, 1);
    const ArtfShadowDesc maps = {256, 0};
    artfSceneSetShadows(s, &maps);
    ArtfFrameStats stats;
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn >= 1);
    drawInto(r, s, a);
    drawInto(r, s, b);
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn == 0);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    at = artfTransformAt(-1, 0, 3, 1);
    artfInstanceSetTransform(s, moving, &at);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn >= 1);
    drawInto(r, s, a);
    artfRendererFree(r);
    r = artfRendererNew(&o);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* Two lamps apart, and something that moves by one of them: the other's maps stay, and what comes out is what a new
 * renderer draws. */
void testShadowRegions(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    setCamera(s, 0, 6, 14, 0, 0, 0);
    ArtfMaterial white = whiteMaterial(s);
    ArtfTransform at = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 40, 1), white, &at);
    const ArtfMesh cube = artfSceneAddCube(s, 1);
    addAt(s, cube, white, 8, 0, 0, 1);
    at = artfTransformAt(-8, 0, 0, 1);
    const ArtfInstance moving = artfSceneAddInstance(s, cube, white, &at);
    const ArtfShadowDesc maps = {256, 0};
    artfSceneSetShadows(s, &maps);
    artfLightSetShadow(s, artfSceneAddLight(s, artfVec3(8, 3, 0), artfVec3(9, 9, 9), 6), true);
    artfLightSetShadow(s, artfSceneAddLight(s, artfVec3(-8, 3, 0), artfVec3(9, 9, 9), 6), true);
    ArtfFrameStats stats;
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn == 12);
    CHECK(artfRendererDraw(r, s));
    at = artfTransformAt(-7, 0, 1, 1);
    artfInstanceSetTransform(s, moving, &at);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn >= 1 && stats.shadowMapsDrawn <= 6);
    drawInto(r, s, a);
    artfRendererFree(r);
    r = artfRendererNew(&o);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* Soft shadows, drawn: as wide as their light, for the sun and a light with a size; sharp ones as they were; both
 * backends alike. */
void testSoftShadows(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c[2] = {artfCalloc(W * H, 4), artfCalloc(W * H, 4)}; /* (zeros: where Vulkan does not run, the GPU's stays so) */
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        for (int sun = 0; sun < 2; ++sun) {
            ArtfScene* s = bulbScene(.5f);
            if (sun) {
                artfSceneRemoveLight(s, 1);
                artfSceneSetSun(s, artfVec3(.3f, -1, .4f), artfVec3(1.5f, 1.5f, 1.5f));
                artfSceneSetSunSize(s, .08f);
            } else {
                artfLightSetShadow(s, 1, true);
            }
            const ArtfShadowDesc hard = {1024, 20, false}, soft = {1024, 20, true};
            artfSceneSetShadows(s, &hard);
            drawInto(r, s, a);
            artfSceneSetShadows(s, &soft);
            drawInto(r, s, b);
            printf("%s, soft shadows from %s: %d pixels between light and shadow, %d sharp\n", backend ? "GPU" : "CPU", sun ? "the sun" : "a light", greyCount(b), greyCount(a));
            CHECK(greyCount(b) > greyCount(a) + 100);
            if (sun) memcpy(c[backend], b, W * H * 4);
            /* with no size, sharp as they were */
            if (sun) artfSceneSetSunSize(s, 0);
            else artfLightSetSize(s, 1, 0);
            drawInto(r, s, b);
            artfSceneSetShadows(s, &hard);
            drawInto(r, s, a);
            CHECK(differentShare(a, b, 0) == 0);
            artfSceneFree(s);
        }
        artfRendererFree(r);
    }
    if (c[1][0] || c[1][W * H / 2]) {
        const double apart = differentShare(c[0], c[1], 24);
        printf("soft shadows, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0112); /* (measured 0.0012 on a Radeon RX 9060 XT) */
        CHECK(apart < .01);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c[0]);
    artfFree(c[1]);
}

/* Soft shadows with temporal antialiasing take fewer reads a frame, turned from one frame to the next: what the frames
 * gather comes to what all the reads give; both backends alike. */
void testSoftShadowsTurning(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* turned[2] = {artfCalloc(W * H, 4), artfCalloc(W * H, 4)}; /* (zeros: where Vulkan does not run, the GPU's stays so) */
    memset(turned[1], 0, W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        for (int sun = 0; sun < 2; ++sun) {
            ArtfScene* s = bulbScene(.5f);
            if (sun) {
                artfSceneRemoveLight(s, 1);
                artfSceneSetSun(s, artfVec3(.3f, -1, .4f), artfVec3(1.5f, 1.5f, 1.5f));
                artfSceneSetSunSize(s, .08f);
            } else {
                artfLightSetShadow(s, 1, true);
            }
            /* what temporal antialiasing itself changes, with sharp shadows: the edges it smooths */
            const ArtfShadowDesc hard = {1024, 20, false}, soft = {1024, 20, true};
            const ArtfPostDesc temporal = {.screen.temporal = true};
            artfSceneSetShadows(s, &hard);
            drawInto(r, s, a);
            artfSceneSetPost(s, &temporal);
            drawFrames(r, s, 48, b);
            const double smoothed = meanApart(a, b), smoothedFar = differentShare(a, b, 32);
            /* all the reads, then turned and gathered over frames */
            artfSceneSetPost(s, NULL);
            artfSceneSetShadows(s, &soft);
            drawInto(r, s, a);
            artfSceneSetPost(s, &temporal);
            drawFrames(r, s, 48, b);
            const double apart = meanApart(a, b), far = differentShare(a, b, 32);
            printf("%s, soft shadows from %s, turned over 48 frames: %.2f of 255 from all the reads, %.2f%% of pixels by more than 32 (sharp ones, %.2f and %.2f%%)\n",
                   backend ? "GPU" : "CPU", sun ? "the sun" : "a light", apart, far * 100, smoothed, smoothedFar * 100);
            CHECK(apart < smoothed + .5 && far < smoothedFar + .004);
            if (!sun) memcpy(turned[backend], b, W * H * 4);
            artfSceneFree(s);
        }
        artfRendererFree(r);
    }
    if (turned[1][0] || turned[1][W * H / 2]) {
        const double apart = differentShare(turned[0], turned[1], 24);
        printf("soft shadows turned, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0168); /* (measured 0.0042 on a Radeon RX 9060 XT) */
        CHECK(apart < .01);
    }
    artfFree(a);
    artfFree(b);
    artfFree(turned[0]);
    artfFree(turned[1]);
}

/* Lights past the frame's sixteen that ask for shadows get them, in the atlas's small maps: on both backends alike. */
void testExtraShadows(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(.02f, .02f, .02f), artfVec3(.02f, .02f, .02f));
    setCamera(s, 0, 6, 6, 0, 0, 0);
    const ArtfMaterial white = whiteMaterial(s);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 20, 1), white, NULL);
    const ArtfTransform above = artfTransformAt(0, 1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddCube(s, 1), white, &above);
    /* the frame's sixteen: far off and dark */
    for (int i = 0; i < 16; ++i) artfSceneAddLight(s, artfVec3(100.f + (float)i, 50, 0), artfVec3(0, 0, 0), 1);
    /* behind the cube and above it: its shadow falls toward the eye */
    const ArtfLight lamp = artfSceneAddLight(s, artfVec3(0, 3, -1.5f), artfVec3(1, 1, 1), 8);
    const ArtfShadowDesc shadows = {1024, 30};
    artfSceneSetShadows(s, &shadows);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    drawInto(cpu, s, a);
    artfLightSetShadow(s, lamp, true);
    drawInto(cpu, s, b);
    /* under the cube, by the eye's side of it: darker with the shadow */
    const double without = meanOf(a, W / 2, H * 2 / 3, 8), with = meanOf(b, W / 2, H * 2 / 3, 8);
    printf("a light past the sixteen, under a cube: %.1f without its shadow, %.1f with\n", without, with);
    CHECK(with < without * .6);
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawInto(gpu, s, a);
        const double apart = differentShare(a, b, 24);
        printf("its shadow, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0121); /* (measured 0.0021 on a Radeon RX 9060 XT) */
        CHECK(apart < .02);
        artfRendererFree(gpu);
    }
    artfRendererFree(cpu);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
}
