/* artful-tests: a frame in floats and what comes after it: the look, temporal antialiasing, upscaling. */
#include "tests.h"

/* Fog, film, spots: on both backends, the same way. */
void testLook(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    const int margin = 4;

    /* fog: at distance 5 with density .2, air 1 keeps 1 / 2.5 of what the surface shows */
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    addAt(s, addSquare(s, 0, 0, 0, 0), white, 0, 0, 0, 1);
    artfSceneSetFog(s, artfVec3(0, 0, 0), .2f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), shownByte(.4), margin));
    ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 16, 0};
    artfSceneSetTrace(s, &rays);
    for (int i = 0; i < 16; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    CHECK(near_(CHANNEL(a[mid], 8), shownByte(.4), margin));
    artfSceneSetTrace(s, NULL);
    artfSceneSetFog(s, artfVec3(0, 0, 0), 0);

    /* film: light of 1, exposed at 1, comes to the curve's .8 */
    artfSceneSetExposure(s, 1.f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), shownByte(2.54 / 3.16), margin));
    artfSceneSetTrace(s, &rays);
    for (int i = 0; i < 16; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    CHECK(near_(CHANNEL(a[mid], 8), shownByte(2.54 / 3.16), margin));
    artfSceneFree(s);

    /* a spot: full in its cone, nothing past it, and the same point light without one */
    s = plainScene();
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    white = whiteMaterial(s);
    addAt(s, addSquare(s, 0, 0, 0, 0), white, 0, 0, 0, 2);
    ArtfLight l = artfSceneAddLight(s, artfVec3(0, 0, 3), artfVec3(1, 1, 1), 100.f);
    artfLightSetSpot(s, l, artfVec3(0, 0, -1), .1f, .2f);
    const size_t aside = (size_t)(H / 2) * W + W / 2 + 40;
    for (int traced = 0; traced < 2; ++traced) {
        if (traced) artfSceneSetTrace(s, &rays);
        for (int i = 0; i < 16; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        CHECK(CHANNEL(a[mid], 8) > 230);
        CHECK(CHANNEL(a[aside], 8) == 0);
    }
    artfSceneSetTrace(s, NULL);
    artfLightSet(s, l, artfVec3(0, 0, 3), artfVec3(1, 1, 1), 100.f);
    drawInto(r, s, a);
    CHECK(CHANNEL(a[aside], 8) > 200);
    artfSceneFree(s);

    /* a soft sun: an edge that was hard goes through shades */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(.9f, .9f, .9f));
    setCamera(s, 0, 6, .01f, 0, 0, 0);
    const ArtfTransform ground = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 1), white, &ground);
    const ArtfTransform bar = artfTransformTrs(artfVec3(0, 2, 0), artfQuatAxisAngle(artfVec3(1, 0, 0), -1.5708f), artfVec3(1, 8, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), white, &bar);
    artfSceneSetSun(s, artfVec3(-.5f, -1, 0), artfVec3(.9f, .9f, .9f));
    int shades[2];
    for (int soft = 0; soft < 2; ++soft) {
        artfSceneSetSunSize(s, soft ? .15f : 0.f);
        ArtfTraceDesc many = {ARTF_TRACE_RAYS, 1, 48, 0};
        artfSceneSetTrace(s, &many);
        for (int i = 0; i < 48; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        shades[soft] = 0;
        for (int x = 0; x < W; ++x) {
            const int v = CHANNEL(a[(size_t)(H - 8) * W + x], 8);
            shades[soft] += v > 110 && v < 215;
        }
    }
    CHECK(shades[1] > shades[0] + 6);
    artfSceneFree(s);

    /* glass bends: a ball between the eye and a wall, red to the left and blue to the right, turns it around */
    s = plainScene();
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    white = whiteMaterial(s);
    addAt(s, addSquare(s, 0xFFFF0000u, 0xFF0000FFu, 0xFF0000FFu, 0xFFFF0000u), white, 0, 0, -6, 12);
    ArtfMaterialDesc glass = artfMaterialDefaults();
    glass.alphaMode = ARTF_ALPHA_BLEND;
    glass.transparency = 1.f;
    glass.specular = 0;
    glass.roughness = 1.f;
    addAt(s, artfSceneAddSphere(s, 1, 48, 64), artfSceneAddMaterial(s, &glass), 0, 0, 0, 1);
    ArtfTraceDesc through = {ARTF_TRACE_RAYS, 1, 8, 0};
    artfSceneSetTrace(s, &through);
    for (int i = 0; i < 8; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    const int x = W / 2 - 15;
    CHECK(meanOf(a, x, H / 2, 16) > meanOf(a, x, H / 2, 0)); /* straight: more red on the left */
    glass.ior = 1.5f;
    artfMaterialSet(s, s->instances.items[1].material, &glass);
    for (int i = 0; i < 8; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    CHECK(meanOf(a, x, H / 2, 0) > meanOf(a, x, H / 2, 16));
    artfSceneFree(s);

    artfFree(a);
    artfRendererFree(r);
}

/* A frame in floats: the same picture as before with nothing after it; each effect the same on both backends; lights
 * past the frame's own; light that bounced. */
void testFloats(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* plain = artfRendererNew(&o);
        o.hdr = true;
        ArtfRenderer* floats = artfRendererNew(&o);
        if (!plain || !floats) {
            artfRendererFree(plain);
            artfRendererFree(floats);
            continue;
        }
        ArtfScene* s = bleedScene();
        drawInto(plain, s, a);
        drawInto(floats, s, b);
        /* nothing after it: the picture as it was, to a level */
        int worst = 0;
        for (int i = 0; i < W * H; ++i)
            for (int k = 0; k < 24; k += 8) worst = artfMaxi(worst, abs((int)(a[i] >> k & 255) - (int)(b[i] >> k & 255)));
        printf("%s, a frame in floats against one in bytes: %d at most\n", backend ? "GPU" : "CPU", worst);
        CHECK(worst <= 1);
        /* each effect changes the picture, and the same way every time */
        const ArtfPostDesc each[6] = {{.lens.bloom = .5f, .lens.bloomThreshold = .1f}, {.screen.occlusion = 1, .screen.occlusionRadius = .8f}, {.screen.reflections = 1}, {.lens.focus = 3, .lens.aperture = 10},
                                      {.screen.volumetric = 1, .screen.scattering = .5f}, {.screen.temporal = true}};
        for (int e = 0; e < 6; ++e) {
            if (getenv("ARTFUL_TEST_TRACE")) printf("(effect %d)\n", e); /* (which one, if the driver falls over) */
            artfSceneSetPost(s, &each[e]);
            if (e == 4) artfSceneSetFog(s, artfVec3(.5f, .5f, .55f), .05f);
            drawInto(floats, s, c);
            const double changed = differentShare(b, c, 2);
            if (changed <= (e == 5 || e == 2 ? .001 : .01)) printf("effect %d changed %.4f of the pixels\n", e, changed);
            CHECK(changed > (e == 5 || e == 2 ? .001 : .01));
            if (e < 5) {
                drawInto(floats, s, a);
                CHECK(differentShare(a, c, 0) == 0);
            }
            if (e == 4) artfSceneSetFog(s, artfVec3(0, 0, 0), 0);
        }
        artfSceneSetPost(s, NULL);
        /* light that bounced: the floor by the red wall turns red */
        const int fx = W / 2 + 28, fy = H / 2 + 34;
        drawInto(plain, s, a);
        const ArtfIndirectDesc probes = {{8, 4, 8}, 64, .5f, 1, {-4, -1.5f, -3}, {4, 3, 3}};
        artfSceneSetIndirect(s, &probes);
        for (int k = 0; k < 6; ++k) drawInto(plain, s, b);
        const double before = meanOf(a, fx, fy, 16) - meanOf(a, fx, fy, 8), after = meanOf(b, fx, fy, 16) - meanOf(b, fx, fy, 8);
        printf("%s, light that bounced: red over green by the wall, %.1f without, %.1f with\n", backend ? "GPU" : "CPU", before, after);
        CHECK(after > before + 3);
        artfSceneFree(s);
        artfRendererFree(plain);
        artfRendererFree(floats);
    }
    /* the backends agree, on each effect and on light that bounced */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        o.backend = ARTF_BACKEND_SOFTWARE;
        ArtfRenderer* cpu = artfRendererNew(&o);
        const ArtfPostDesc all = {.lens.bloom = .2f, .screen.occlusion = 1, .screen.reflections = 1, .lens.focus = 6, .lens.aperture = 6};
        ArtfScene* s = bleedScene();
        artfSceneSetPost(s, &all);
        const ArtfIndirectDesc probes = {{8, 4, 8}, 64, .5f, 1, {-4, -1.5f, -3}, {4, 3, 3}};
        artfSceneSetIndirect(s, &probes);
        drawInto(cpu, s, a);
        artfSceneSetIndirect(s, &probes); /* (the probes start over, so both see the same steps) */
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 16);
        printf("all effects and probes, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0244); /* (measured 0.0061 on a Radeon RX 9060 XT) */
        CHECK(apart < .03);
        /* lights past the frame's own */
        artfSceneSetPost(s, NULL);
        artfSceneSetIndirect(s, NULL);
        artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
        uint32_t st = 5;
        for (int i = 0; i < 100; ++i) {
            st = st * 747796405u + 2891336453u;
            const float x = (float)(st >> 8 & 255) / 255.f * 8.f - 4.f, z = (float)(st >> 16 & 255) / 255.f * 6.f - 3.f;
            CHECK(artfSceneAddLight(s, artfVec3(x, -.6f, z), artfVec3((float)(i % 3 == 0), (float)(i % 3 == 1), (float)(i % 3 == 2)), 1.5f) != 0);
        }
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        CHECK(meanOf(a, W / 2, H * 3 / 4, 16) > 30);
        const double lightsApart = differentShare(a, b, 16);
        printf("100 lights past the frame's own, the GPU against the CPU: %.2f%% of pixels apart\n", lightsApart * 100);
        FLIP_ALIKE(0.0144); /* (measured 0.0036 on a Radeon RX 9060 XT) */
        CHECK(lightsApart < .02);
        artfSceneFree(s);
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* Lights in clusters: a corridor of three floors, 360 lights down it, seen from the middle one. Far down it, a square
 * of the frame is reached by more lights than one list holds; by slices of depth, none is full, the GPU leaves none
 * out (ArtfFrameStats.lightsOmitted), and its picture is the CPU's, which lists them all. And the lists are quick to
 * make, at 1920 x 1080. */
static ArtfScene* corridorScene(void) {
    ArtfScene* s = plainScene();
    artfSceneSetAmbient(s, artfVec3(.02f, .02f, .02f), artfVec3(.02f, .02f, .02f));
    const ArtfMaterialDesc grey = {artfVec3(.8f, .8f, .8f), 0, .6f, .04f, artfVec3(0, 0, 0)};
    const ArtfMaterial m = artfSceneAddMaterial(s, &grey);
    const ArtfMesh floor = artfSceneAddPlane(s, 30, 1);
    for (int level = 0; level < 3; ++level) {
        const ArtfTransform t = artfTransformAt(0, (float)level * 3.f, -40, 3);
        artfSceneAddInstance(s, floor, m, &t);
    }
    for (int level = 0; level < 3; ++level)
        for (int k = 0; k < 60; ++k)
            for (int side = 0; side < 2; ++side) {
                const float hue = (float)((k + level) % 3);
                CHECK(artfSceneAddLight(s, artfVec3(side ? 1.2f : -1.2f, (float)level * 3.f + 1.f, -1.f - (float)k * 1.25f), artfVec3(hue == 0, hue == 1, hue == 2), 2.5f) != 0);
            }
    setCamera(s, 0, 4.6f, 1, 0, 4.f, -60);
    return s;
}

void testLightClusters(void) {
    ArtfScene* s = corridorScene();
    /* the lists, as the backends make them */
    ArtfFrameData f;
    artfSceneFrame(s, 1920, 1080, &f);
    ArtfLightData* lights = artfMalloc(ARTF_MAX_LIGHTS * sizeof *lights);
    const uint32_t count = artfSceneExtraLights(s, &f, lights);
    CHECK(count == 360 - ARTF_FRAME_LIGHTS);
    const int tiles = ((1920 + ARTF_LIGHT_TILE - 1) / ARTF_LIGHT_TILE) * ((1080 + ARTF_LIGHT_TILE - 1) / ARTF_LIGHT_TILE);
    uint32_t* all = artfMalloc((size_t)tiles * (1 + count) * 4);
    uint32_t* clusters = NULL;
    size_t room = 0;
    uint32_t omitted = 1;
    artfSceneLightSlices(&f, lights, count);
    const size_t length = artfSceneLightTiles(&f, 1920, 1080, lights, count, all, &clusters, &room, &omitted);
    /* a square is reached by more than the 64 a list once held; a cluster, by far fewer */
    uint32_t most = 0, fullest = 0;
    for (int t = 0; t < tiles; ++t) {
        const uint32_t n = all[(size_t)t * (1 + count)];
        if (n > most) most = n;
    }
    for (int c = 0; c < tiles * ARTF_LIGHT_SLICES; ++c) {
        const uint32_t n = clusters[(size_t)c * 2 + 1];
        if (n > fullest) fullest = n;
    }
    printf("a corridor of 3 floors, 344 lights past the frame's: a square reached by %u at most, a cluster by %u; %zu numbers, %u left out\n", most, fullest, length, omitted);
    CHECK(most > 64 && fullest < most / 2);
    CHECK(omitted == 0);
    /* every light a square lists is in the cluster of each point of it that it reaches, and in their order: none is
     * lost by slicing. At each depth its ball spans. */
    int lost = 0, unordered = 0;
    for (int t = 0; t < tiles; ++t) {
        const uint32_t* list = all + (size_t)t * (1 + count);
        for (uint32_t k = 0; k < list[0]; ++k) {
            const ArtfLightData* L = &lights[list[1 + k]];
            const float z = (L->position[0] - f.cameraPos[0]) * f.eye[2][0] + (L->position[1] - f.cameraPos[1]) * f.eye[2][1] + (L->position[2] - f.cameraPos[2]) * f.eye[2][2];
            for (int step = 0; step <= 32; ++step) {
                const float at = z - L->radius + L->radius * 2.f * (float)step / 32.f;
                const int slice = artfClampi((int)floorf(artfLightSliceAt(&f, at)), 0, ARTF_LIGHT_SLICES - 1);
                const uint32_t* head = clusters + ((size_t)t * ARTF_LIGHT_SLICES + (size_t)slice) * 2;
                bool there = false;
                for (uint32_t j = 0; j < head[1] && !there; ++j) there = clusters[head[0] + j] == list[1 + k];
                lost += !there;
            }
        }
        for (int k = 0; k < ARTF_LIGHT_SLICES; ++k) {
            const uint32_t* head = clusters + ((size_t)t * ARTF_LIGHT_SLICES + (size_t)k) * 2;
            for (uint32_t j = 1; j < head[1]; ++j) unordered += clusters[head[0] + j] <= clusters[head[0] + j - 1];
        }
    }
    CHECK(lost == 0 && unordered == 0);
    /* how long they take to make */
    const int rounds = 50;
    const double t0 = artfNowMs();
    for (int k = 0; k < rounds; ++k) {
        artfSceneLightSlices(&f, lights, count);
        artfSceneLightTiles(&f, 1920, 1080, lights, count, NULL, &clusters, &room, &omitted);
    }
    const double ms = (artfNowMs() - t0) / rounds;
    printf("the clusters of 344 lights at 1920 x 1080: %.3f ms to make\n", ms);
    CHECK(ms < 2.0); /* (0.2 is the aim, in a build that is optimized: this is for any) */
    artfFree(all);
    artfFree(clusters);
    artfFree(lights);
    /* on the GPU, none left out, and the picture is the CPU's */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        o.backend = ARTF_BACKEND_SOFTWARE;
        ArtfRenderer* cpu = artfRendererNew(&o);
        uint32_t* a = artfMalloc((size_t)W * H * 4);
        uint32_t* b = artfMalloc((size_t)W * H * 4);
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        ArtfFrameStats stats;
        artfRendererStats(gpu, &stats);
        const double apart = differentShare(a, b, 16);
        printf("the corridor, the GPU against the CPU: %.2f%% of pixels apart; lights left out on the GPU: %u; lit: %.1f\n", apart * 100, stats.lightsOmitted, meanOf(a, W / 2, H * 3 / 5, 8));
        FLIP_ALIKE(0.0121); /* (measured 0.0021 on a Radeon RX 9060 XT) */
        CHECK(stats.lightsOmitted == 0);
        CHECK(apart < .02);
        CHECK(meanOf(a, W / 2, H * 3 / 5, 8) > 10);
        artfFree(a);
        artfFree(b);
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfSceneFree(s);
}

/* The look: log2 and 2^x as the metering has them; each setting changes the picture the same way every time, on both
 * backends, and they agree; auto exposure brings a dim scene and a bright one together. */
static double frameMean(const uint32_t* f) {
    double sum = 0;
    for (int i = 0; i < W * H; ++i) sum += CHANNEL(f[i], 0) + CHANNEL(f[i], 8) + CHANNEL(f[i], 16);
    return sum / (3.0 * W * H);
}

void testGrading(void) {
    double worstLog = 0, worstExp = 0;
    for (float x = 1.f / 8192.f; x < 8192.f; x *= 1.0137f) worstLog = fmax(worstLog, fabs(artfLog2Fast(x) - log2(x)));
    for (float x = -30.f; x < 30.f; x += .0173f) worstExp = fmax(worstExp, fabs(artfExp2Fast(x) / exp2(x) - 1));
    printf("log2 within %.2g, 2^x within %.2g of its value\n", worstLog, worstExp);
    CHECK(worstLog < 5e-5 && worstExp < 2e-5);

    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    const ArtfPostDesc each[9] = {{.look.toneMap = ARTF_TONE_NEUTRAL}, {.look.toneMap = ARTF_TONE_REINHARD}, {.look.toneMap = ARTF_TONE_NONE},
                                  {.look.temperature = .8f}, {.look.tint = -.6f}, {.look.saturation = -.7f}, {.look.contrast = .5f}, {.lens.vignette = 1}, {.lens.chromatic = 6}};
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* s = bleedScene();
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(3, 3, 3));
        artfSceneSetExposure(s, 1);
        drawInto(r, s, b);
        for (int e = 0; e < 9; ++e) {
            artfSceneSetPost(s, &each[e]);
            drawInto(r, s, c);
            const double changed = differentShare(b, c, 2);
            if (changed <= .01) printf("%s, look %d changed %.4f of the pixels\n", backend ? "GPU" : "CPU", e, changed);
            CHECK(changed > .01);
            drawInto(r, s, a);
            CHECK(differentShare(a, c, 0) == 0);
        }
        /* nothing asked: the same bytes as before there was a look */
        const ArtfPostDesc none = {0};
        artfSceneSetPost(s, &none);
        drawInto(r, s, c);
        CHECK(differentShare(b, c, 0) == 0);
        /* auto exposure: the sun eight times as bright, nearly the same picture */
        const ArtfPostDesc meter = {.exposure.automatic = true, .exposure.speed = 1};
        artfSceneSetPost(s, &meter);
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(.5f, .5f, .5f));
        artfSceneSetAmbient(s, artfVec3(.02f, .02f, .02f), artfVec3(.02f, .02f, .02f));
        drawInto(r, s, a);
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(4, 4, 4));
        artfSceneSetAmbient(s, artfVec3(.16f, .16f, .16f), artfVec3(.16f, .16f, .16f));
        drawInto(r, s, b);
        const double dim = frameMean(a), bright = frameMean(b);
        artfSceneSetPost(s, NULL);
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(.5f, .5f, .5f));
        artfSceneSetAmbient(s, artfVec3(.02f, .02f, .02f), artfVec3(.02f, .02f, .02f));
        drawInto(r, s, c);
        printf("%s, auto exposure: a sun 8 times as bright shows %.1f against %.1f; the dim one %.1f without it\n", backend ? "GPU" : "CPU", bright, dim, frameMean(c));
        CHECK(fabs(bright - dim) < 8 && frameMean(c) < dim - 30);
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(4, 4, 4));
        artfSceneSetAmbient(s, artfVec3(.16f, .16f, .16f), artfVec3(.16f, .16f, .16f));
        /* and slowly: a twentieth of the way a frame */
        const ArtfPostDesc slow = {.exposure.automatic = true};
        artfSceneSetPost(s, &slow);
        drawInto(r, s, a); /* (from what it had: a state goes on while the renderer meters) */
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(.5f, .5f, .5f));
        artfSceneSetAmbient(s, artfVec3(.02f, .02f, .02f), artfVec3(.02f, .02f, .02f));
        drawInto(r, s, b);
        const double first = frameMean(b);
        for (int k = 0; k < 60; ++k) drawInto(r, s, b);
        printf("%s, auto exposure going up: %.1f, then %.1f after 60 frames\n", backend ? "GPU" : "CPU", first, frameMean(b));
        CHECK(frameMean(b) > first + 10);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    /* the backends agree, all of it at once */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        o.backend = ARTF_BACKEND_SOFTWARE;
        ArtfRenderer* cpu = artfRendererNew(&o);
        for (int curve = 0; curve < 4; ++curve) {
            ArtfScene* s = bleedScene();
            artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(3, 3, 3));
            const ArtfPostDesc all = {.lens.bloom = .1f, .look.toneMap = (ArtfToneMap)curve, .exposure.automatic = true, .exposure.speed = 1, .look.temperature = .4f,
                                      .look.tint = .2f, .look.saturation = .3f, .look.contrast = .2f, .lens.vignette = .6f, .lens.chromatic = 3};
            artfSceneSetPost(s, &all);
            drawInto(cpu, s, a);
            drawInto(gpu, s, b);
            const double apart = differentShare(a, b, 16);
            printf("the look with curve %d, the GPU against the CPU: %.2f%% of pixels apart\n", curve, apart * 100);
            FLIP_ALIKE(0.0136); /* (measured 0.0034 on a Radeon RX 9060 XT) */
            CHECK(apart < .02);
            artfSceneFree(s);
        }
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* The look, as Unreal's: a histogram leaves out a small bright light that the mean follows; the exposure falls at its
 * own speed; a look-up table that changes nothing changes nothing, and one that swaps red and blue swaps them; lift,
 * gamma, gain, the wheels, grain and flare each change the picture; grain is new each frame; .cube files are read; the
 * backends agree; and none of it asked is the same bytes as before. */
static ArtfScene* lampScene(bool lamp) {
    ArtfScene* s = bleedScene();
    artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(2, 2, 2));
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    artfSceneSetExposure(s, 1);
    if (lamp) {
        ArtfMaterialDesc m = artfMaterialDefaults();
        m.baseColor = artfVec3(0, 0, 0);
        m.emissive = artfVec3(3000, 3000, 3000);
        addAt(s, artfSceneAddSphere(s, 1.f, 16, 24), artfSceneAddMaterial(s, &m), 0, 1.2f, 0, 1);
    }
    return s;
}

/* The mean of the left quarter of a frame: away from the lamp. */
static double leftMean(const uint32_t* f) {
    double sum = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W / 4; ++x) sum += CHANNEL(f[y * W + x], 8);
    return sum / (H * (W / 4));
}

static float* swapLut(int n, bool swap) {
    float* lut = artfMalloc((size_t)n * n * n * 12);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                float* o = lut + 3 * (((size_t)b * n + g) * n + r);
                o[0] = (float)(swap ? b : r) / (float)(n - 1);
                o[1] = (float)g / (float)(n - 1);
                o[2] = (float)(swap ? r : b) / (float)(n - 1);
            }
    return lut;
}

void testLookMore(void) {
    /* .cube files */
    static const char cube[] = "# a table\nTITLE \"swap\"\nLUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n"
                               "0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n";
    int side = 0;
    float* read = artfLutDecodeCube(cube, sizeof cube - 1, &side);
    float* known = swapLut(2, true);
    CHECK(read && side == 2 && !memcmp(read, known, 8 * 12));
    artfFree(read);
    artfFree(known);
    CHECK(!artfLutDecodeCube("LUT_3D_SIZE 2\n0 0 0\n", 20, &side));
    CHECK(!artfLutDecodeCube("LUT_1D_SIZE 2\n0 0 0\n1 1 1\n", 26, &side));

    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        const char* name = backend ? "GPU" : "CPU";
        /* a histogram: a lamp in the middle moves the mean, not what is left between the shares */
        ArtfScene* dark = lampScene(false);
        ArtfScene* lit = lampScene(true);
        const ArtfPostDesc mean = {.exposure.automatic = true, .exposure.speed = 1};
        ArtfPostDesc histogram = mean;
        histogram.exposure.low = .1f;
        histogram.exposure.high = .1f;
        artfSceneSetPost(dark, &mean);
        artfSceneSetPost(lit, &mean);
        drawInto(r, dark, a);
        drawInto(r, lit, b);
        const double byMean = leftMean(a) - leftMean(b);
        artfSceneSetPost(dark, &histogram);
        artfSceneSetPost(lit, &histogram);
        drawInto(r, dark, a);
        drawInto(r, lit, b);
        const double byHistogram = fabs(leftMean(a) - leftMean(b));
        printf("%s, a bright lamp darkens the rest by %.1f metered by the mean, %.1f by a histogram\n", name, byMean, byHistogram);
        CHECK(byMean > 2 && byHistogram < byMean * .3);
        /* down at its own speed: from dark to bright at once, back up slowly */
        ArtfPostDesc speeds = histogram;
        speeds.exposure.speed = .05f;
        speeds.exposure.speedDown = 1;
        artfSceneSetPost(dark, &speeds);
        artfSceneSetSun(dark, artfVec3(-.6f, -1, -.3f), artfVec3(.2f, .2f, .2f));
        drawInto(r, dark, a);
        for (int k = 0; k < 20; ++k) drawInto(r, dark, a);
        artfSceneSetSun(dark, artfVec3(-.6f, -1, -.3f), artfVec3(2, 2, 2));
        CHECK(artfRendererDraw(r, dark));
        CHECK(artfRendererReadPixels(r, b));
        artfSceneSetPost(dark, &histogram);
        drawInto(r, dark, c);
        printf("%s, the exposure falls at once: %.1f, as metered %.1f\n", name, leftMean(b), leftMean(c));
        CHECK(fabs(leftMean(b) - leftMean(c)) < 3);
        artfSceneFree(dark);
        artfSceneFree(lit);
        /* the look-up tables */
        ArtfScene* s = lampScene(false);
        artfSceneSetPost(s, NULL);
        drawInto(r, s, a);
        float* identity = swapLut(17, false);
        float* swapped = swapLut(17, true);
        /* (in ACEScct, as tables are by default: nothing changed, to a level, and what is grey stays grey) */
        ArtfPostDesc table = {.look.lut = identity, .look.lutSize = 17};
        artfSceneSetPost(s, &table);
        drawInto(r, s, b);
        printf("%s, a look-up table in ACEScct that changes nothing: %.4f%% of pixels apart\n", name, differentShare(a, b, 1) * 100);
        CHECK(differentShare(a, b, 1) == 0);
        table.look.lut = swapped;
        artfSceneSetPost(s, &table);
        drawInto(r, s, b);
        int greys = 0, stayed = 0;
        for (int i = 0; i < W * H; ++i) {
            const int r0 = CHANNEL(a[i], 0), g0 = CHANNEL(a[i], 8), b0 = CHANNEL(a[i], 16);
            if (abs(r0 - g0) > 1 || abs(g0 - b0) > 1) continue;
            ++greys;
            const int r1 = CHANNEL(b[i], 0), g1 = CHANNEL(b[i], 8), b1 = CHANNEL(b[i], 16);
            stayed += abs(r1 - g1) <= 3 && abs(g1 - b1) <= 3 && abs(g1 - g0) <= 3;
        }
        printf("%s, red and blue swapped in ACEScct: %d of %d grey pixels stay grey\n", name, stayed, greys);
        CHECK(greys > 100 && stayed == greys);
        /* and as before 1.0, in what a screen shows */
        table.look.lut = identity;
        table.look.gradeShown = true;
        artfSceneSetPost(s, &table);
        drawInto(r, s, b);
        printf("%s, a look-up table in what is shown that changes nothing: %.4f%% of pixels apart\n", name, differentShare(a, b, 1) * 100);
        CHECK(differentShare(a, b, 1) == 0);
        table.look.lut = swapped;
        artfSceneSetPost(s, &table);
        drawInto(r, s, b);
        int worst = 0;
        for (int i = 0; i < W * H; ++i) {
            worst = artfMaxi(worst, abs(CHANNEL(a[i], 16) - CHANNEL(b[i], 0)));
            worst = artfMaxi(worst, abs(CHANNEL(a[i], 0) - CHANNEL(b[i], 16)));
            worst = artfMaxi(worst, abs(CHANNEL(a[i], 8) - CHANNEL(b[i], 8)));
        }
        printf("%s, a look-up table that swaps red and blue: within %d\n", name, worst);
        CHECK(worst <= 1);
        artfFree(identity);
        artfFree(swapped);
        /* each of the rest changes the picture, the same way each time (grain: each frame) */
        const ArtfPostDesc each[8] = {{.look.lift = {.1f, 0, 0}}, {.look.gamma = {1.5f, 1.5f, 1.5f}}, {.look.gain = {1, .7f, 1}}, {.look.shadows = {1, 1, 1.6f}},
                                      {.look.midtones = {1.3f, 1, 1}}, {.look.highlights = {.6f, .8f, 1}}, {.lens.bloom = .1f, .lens.flare = 1}, {.lens.grain = .5f}};
        for (int e = 0; e < 8; ++e) {
            ArtfPostDesc plain = {0};
            plain.lens.bloom = each[e].lens.bloom;
            artfSceneSetPost(s, &plain);
            drawInto(r, s, a);
            artfSceneSetPost(s, &each[e]);
            drawInto(r, s, b);
            const double changed = differentShare(a, b, 2);
            if (changed <= .01) printf("%s, look %d changed %.4f of the pixels\n", name, e, changed);
            CHECK(changed > .01);
            drawInto(r, s, c);
            CHECK((differentShare(b, c, 0) == 0) == (e != 7));
        }
        /* none of it: the same bytes */
        artfSceneSetPost(s, NULL);
        drawInto(r, s, a);
        const ArtfPostDesc none = {.look.lutAmount = .5f, .look.lutSize = 17};
        artfSceneSetPost(s, &none);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 0) == 0);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    /* the backends agree, all of it at once */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        o.backend = ARTF_BACKEND_SOFTWARE;
        ArtfRenderer* cpu = artfRendererNew(&o);
        float* swapped = swapLut(9, true);
        ArtfScene* s = lampScene(true);
        const ArtfPostDesc all = {.lens.bloom = .1f, .exposure.automatic = true, .exposure.speed = .5f, .exposure.speedDown = 1, .exposure.low = .1f, .exposure.high = .05f,
                                  .look.lift = {.03f, 0, .02f}, .look.gamma = {1.2f, 1.1f, 1}, .look.gain = {1, .95f, .9f}, .look.shadows = {.9f, 1, 1.2f}, .look.highlights = {1.1f, 1, .9f},
                                  .look.lut = swapped, .look.lutSize = 9, .look.lutAmount = .7f, .lens.grain = .3f, .lens.flare = .5f};
        artfSceneSetPost(s, &all);
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 16);
        printf("the look's histogram, grading, table, grain and flare, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0276); /* (measured 0.0069 on a Radeon RX 9060 XT) */
        CHECK(apart < .02);
        artfSceneFree(s);
        artfFree(swapped);
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* What the kernels do after the frame (grading, temporal antialiasing, upscaling, with something that moves): the same
 * bytes on every instruction set, with any number of threads. */
void testPostKernels(void) {
    uint32_t* frame = artfMalloc(W * H * 4);
    float* swapped = swapLut(9, true);
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {3, ARTF_SIMD_128}};
    for (int scaled = 0; scaled < 2; ++scaled) {
        uint64_t reference = 0;
        for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
            ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
            o.simd = runs[i].simd;
            o.hdr = true;
            o.renderScale = scaled ? .6f : 0;
            ArtfRenderer* r = artfRendererNew(&o);
            ArtfInstance moving;
            ArtfScene* s = viewScene(&moving);
            const ArtfPostDesc all = {.lens.bloom = .1f, .screen.temporal = true, .look.lift = {.03f, 0, .02f}, .look.gamma = {1.2f, 1.1f, .9f}, .look.gain = {1, .95f, .9f},
                                      .look.shadows = {.9f, 1, 1.2f}, .look.highlights = {1.1f, 1, .9f}, .look.lut = swapped, .look.lutSize = 9, .look.lutAmount = .7f, .lens.grain = .3f, .lens.flare = .5f};
            artfSceneSetPost(s, &all);
            for (int f = 0; f < 4; ++f) {
                const ArtfTransform t = artfTransformAt(1.5f + .1f * (float)f, .3f, 2.f, 1);
                artfInstanceSetTransform(s, moving, &t);
                CHECK(artfRendererDraw(r, s));
            }
            CHECK(artfRendererReadPixels(r, frame));
            const uint64_t h = hashOf(frame, (size_t)W * H);
            if (!i) reference = h;
            CHECK(h == reference);
            artfSceneFree(s);
            artfRendererFree(r);
        }
    }
    artfFree(swapped);
    artfFree(frame);
}

/* ---- temporal quality and speed ---- */

#define TEMPORAL_BYTES 0x98f3d695e3cc3878ull /* (what testTemporalBytes hashed before these came; made again for sRGB, and for highlights that get back what their lobe loses) */

/* Upscaling: the scene drawn at half the size and made whole by temporal antialiasing is close to it drawn at the
 * frame's size, and without temporal antialiasing too, a little softer; both backends alike. The overlay and what is
 * read back are the frame's size. */
void testUpscale(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* half[2] = {artfCalloc(W * H, 4), artfCalloc(W * H, 4)}; /* (zeros: where Vulkan does not run, the GPU's stays so) */
    uint32_t* corner = artfCalloc(W * H, 4);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 30; ++x) corner[y * W + x] = 0xFF2040A0u;
    memset(half[1], 0, W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* whole = artfRendererNew(&o);
        o.renderScale = .5f;
        ArtfRenderer* scaled = artfRendererNew(&o);
        if (!whole || !scaled) {
            artfRendererFree(whole);
            artfRendererFree(scaled);
            continue;
        }
        ArtfScene* s = bleedScene();
        const ArtfPostDesc temporal = {.screen.temporal = true};
        artfSceneSetPost(s, &temporal);
        drawFrames(whole, s, 24, a);
        drawFrames(scaled, s, 24, b);
        const double apart = meanApart(a, b), far = differentShare(a, b, 48);
        printf("%s, upscaled from half the size with temporal antialiasing: %.2f of 255 from the whole size, %.2f%% of pixels by more than 48\n", backend ? "GPU" : "CPU", apart,
               far * 100);
        CHECK(apart < 2.5 && far < .02);
        memcpy(half[backend], b, W * H * 4);
        /* without: read between its pixels */
        artfSceneSetPost(s, NULL);
        drawInto(whole, s, a);
        drawInto(scaled, s, b);
        const double plain = meanApart(a, b);
        printf("%s, upscaled without: %.2f of 255 from the whole size\n", backend ? "GPU" : "CPU", plain);
        CHECK(plain < 4);
        /* the overlay goes over the frame shown */
        const ArtfOverlay over = {corner, W, H, 0, false, NULL, 0};
        artfRendererSetOverlay(scaled, &over);
        drawInto(scaled, s, b);
        CHECK(b[5 * W + 5] == 0xFF2040A0u && b[19 * W + 29] == 0xFF2040A0u && b[20 * W + 30] != 0xFF2040A0u);
        artfSceneFree(s);
        artfRendererFree(whole);
        artfRendererFree(scaled);
    }
    if (half[1][0] || half[1][W * H / 2]) {
        const double apart = differentShare(half[0], half[1], 24);
        printf("upscaled, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0224); /* (measured 0.0016 on a Radeon RX 9060 XT, 0.0179 on lavapipe) */
        CHECK(apart < .02);
    }
    /* a scale of 1 is no scale */
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    o.hdr = true;
    ArtfRenderer* none = artfRendererNew(&o);
    o.renderScale = 1;
    ArtfRenderer* one = artfRendererNew(&o);
    ArtfScene* s = bleedScene();
    drawInto(none, s, a);
    drawInto(one, s, b);
    CHECK(differentShare(a, b, 0) == 0);
    artfSceneFree(s);
    artfRendererFree(none);
    artfRendererFree(one);
    artfFree(a);
    artfFree(b);
    artfFree(half[0]);
    artfFree(half[1]);
    artfFree(corner);
}

/* What upscaling makes of what is hard for it, at half the size:
 * - a grille of lines a fifth of the scene's pixel wide, the eye still: how much a pixel changes from one frame to the
 *   next, once it has settled (it should not: nothing moves);
 * - a veil of smoke over a black and white board, gone at once: for how many frames what it was stays. */
static ArtfScene* grilleScene(void) {
    ArtfScene* s = plainScene();
    artfSceneSetAmbient(s, artfVec3(.8f, .8f, .8f), artfVec3(.8f, .8f, .8f));
    enum { LINES = 31 };
    static ArtfVertex v[LINES * 2 * 4];
    static uint32_t idx[LINES * 2 * 6];
    int nv = 0, ni = 0;
    for (int k = 0; k < LINES; ++k)
        for (int across = 0; across < 2; ++across) {
            const float at = -1.5f + (float)k * .1f, half = .006f;
            const float x0 = across ? -1.5f : at - half, x1 = across ? 1.5f : at + half;
            const float y0 = across ? at - half : -1.5f, y1 = across ? at + half : 1.5f;
            const float px[4] = {x0, x1, x1, x0}, py[4] = {y0, y0, y1, y1};
            for (int c = 0; c < 4; ++c) v[nv + c] = (ArtfVertex){px[c], py[c], 0, 0, 0, 1, 0, 0, 0};
            const uint32_t q[6] = {0, 1, 2, 0, 2, 3};
            for (int c = 0; c < 6; ++c) idx[ni + c] = (uint32_t)nv + q[c];
            nv += 4;
            ni += 6;
        }
    addAt(s, artfSceneAddMesh(s, v, (uint32_t)nv, idx, (uint32_t)ni), whiteMaterial(s), 0, 0, 0, 1);
    return s;
}

static ArtfScene* smokeScene(ArtfInstance* veil) {
    ArtfScene* s = plainScene();
    static uint32_t board[8 * 8];
    for (int i = 0; i < 64; ++i) board[i] = ((i % 8) + (i / 8)) & 1 ? 0xFFFFFFFFu : 0xFF000000u;
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTexture(s, board, 8, 8, 0), 1, 0, artfVec3(0, 0, 0)};
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, -.5f, 2.5f);
    const ArtfMaterialDesc grey = {artfVec3(.6f, .6f, .6f), 0, 1, 0, artfVec3(0, 0, 0)};
    *veil = addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &grey), 0, 0, .5f, .8f);
    artfInstanceSetOpacity(s, *veil, .6f);
    return s;
}

void testUpscaleQuality(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* still = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        o.renderScale = .5f;
        ArtfRenderer* r = artfRendererNew(&o);
        ArtfRenderer* plain = artfRendererNew(&o);
        if (!r || !plain) {
            artfRendererFree(r);
            artfRendererFree(plain);
            continue;
        }
        const char* name = backend ? "GPU" : "CPU";
        const ArtfPostDesc temporal = {.screen.temporal = true};
        /* the grille: the mean change of a pixel, frame to frame, over 16 frames after 32 */
        ArtfScene* s = grilleScene();
        artfSceneSetPost(s, &temporal);
        drawFrames(r, s, 32, a);
        double change = 0;
        for (int k = 0; k < 16; ++k) {
            drawFrames(r, s, 1, b);
            change += meanApart(a, b);
            memcpy(a, b, W * H * 4);
        }
        change /= 16;
        artfSceneFree(s);
        /* the smoke: the board alone, then with the veil for 24 frames, then gone; how many frames until what it
         * covered is the board's again, within 4 of 255 on the mean */
        ArtfInstance veil;
        s = smokeScene(&veil);
        artfSceneSetPost(s, &temporal);
        const ArtfTransform away = artfTransformAt(40, 0, .5f, .8f);
        artfInstanceSetTransform(s, veil, &away);
        drawInto(plain, s, still);
        const ArtfTransform here = artfTransformAt(0, 0, .5f, .8f);
        artfInstanceSetTransform(s, veil, &here);
        drawFrames(r, s, 24, a);
        artfInstanceSetTransform(s, veil, &away);
        int trail = 0;
        for (int k = 1; k <= 16; ++k) {
            drawFrames(r, s, 1, b);
            double sum = 0;
            int n = 0;
            for (int y = H / 2 - 30; y < H / 2 + 30; ++y)
                for (int x = W / 2 - 30; x < W / 2 + 30; ++x, ++n) sum += abs(CHANNEL(b[y * W + x], 8) - CHANNEL(still[y * W + x], 8));
            if (sum / n > 4) trail = k;
        }
        /* something solid going across the board: how far from it drawn without history, once it has gone some way */
        artfInstanceSetOpacity(s, veil, 1);
        for (int k = 0; k < 24; ++k) {
            const ArtfTransform at = artfTransformAt(-1.f + .06f * (float)k, 0, .5f, .8f);
            artfInstanceSetTransform(s, veil, &at);
            drawFrames(r, s, 1, a);
        }
        drawInto(plain, s, b);
        const double moving = meanApart(a, b);
        artfSceneFree(s);
        printf("%s, upscaled from half: a still grille changes %.2f of 255 a frame; smoke stays %d frames after it went; something moving is %.2f from drawn without history\n", name, change, trail,
               moving);
        /* (before locks, a box held to the deviation and the blend by luma: 4.90 and 5.01, 10 frames, 1.48 and 1.22) */
        CHECK(change < 2.45 && trail <= 4 && moving < 1.5);
        artfRendererFree(r);
        artfRendererFree(plain);
    }
    artfFree(a);
    artfFree(b);
    artfFree(still);
}

/* What is off is as it was, to the byte: a frame in floats with soft shadows and no temporal antialiasing, and one
 * with temporal antialiasing following a moving instance, on the CPU, hash as they did before upscaling, vertices'
 * motion and turned shadows came. */
void testTemporalBytes(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 2, false);
    o.hdr = true;
    ArtfRenderer* r = artfRendererNew(&o);
    ArtfScene* s = richScene();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says: the hash is of the defaults) */
    artfSceneSetFeature(s, ARTF_FEATURE_CONES, true);
    const ArtfShadowDesc soft = {512, 20, true};
    artfSceneSetShadows(s, &soft);
    artfSceneSetSunSize(s, .05f);
    artfLightSetShadow(s, 1, true);
    artfLightSetSize(s, 1, .3f);
    const ArtfPostDesc some = {.lens.bloom = .2f, .screen.occlusion = .5f};
    artfSceneSetPost(s, &some);
    drawInto(r, s, a);
    uint64_t h = hashOf(a, (size_t)W * H);
    const ArtfPostDesc temporal = {.screen.temporal = true};
    ArtfInstance ball;
    ArtfScene* moving = movingScene(&ball);
    artfSceneSetFeature(moving, ARTF_FEATURE_LOD, true);
    artfSceneSetFeature(moving, ARTF_FEATURE_CONES, true);
    artfSceneSetPost(moving, &temporal);
    for (int k = 0; k < 6; ++k) {
        placeBall(moving, ball, -.6f + (float)k * .2f);
        drawOnce(r, moving, a);
    }
    h = h * 31 + hashOf(a, (size_t)W * H);
    printf("what is off, to the byte: %016llx\n", (unsigned long long)h);
    CHECK(h == TEMPORAL_BYTES);
    artfSceneFree(s);
    artfSceneFree(moving);
    artfRendererFree(r);
    artfFree(a);
}

/* A frame in floats with four samples a pixel: on the GPU as on the CPU, with what comes after and fog that light goes
 * through; and smoother than with one. */
void testFloatsSamples(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    o.antialias = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    o.antialias = false;
    ArtfRenderer* single = artfRendererNew(&o);
    ArtfScene* s = bleedScene();
    const ArtfPostDesc all = {.lens.bloom = .2f, .screen.occlusion = 1, .screen.reflections = 1, .screen.volumetric = 1, .screen.scattering = .4f};
    artfSceneSetPost(s, &all);
    artfSceneSetFog(s, artfVec3(.5f, .5f, .55f), .03f);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    drawInto(cpu, s, a);
    drawInto(gpu, s, b);
    const double apart = differentShare(a, b, 16);
    printf("four samples in floats, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
    FLIP_ALIKE(0.0432); /* (measured 0.0108 on a Radeon RX 9060 XT) */
    CHECK(apart < .03);
    drawInto(single, s, b);
    CHECK(differentShare(a, b, 4) > .002); /* (edges come out otherwise) */
    artfSceneFree(s);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
    artfRendererFree(single);
    artfFree(a);
    artfFree(b);
}

/* Pacing: a budget no frame can keep takes the scene down to the least it may be at once; one every frame keeps
 * takes it back up to renderScale; and the heat a device says it is near makes the same budget too small. */
void testPacing(void) {
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        o.renderScale = .9f;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        const char* name = backend ? "GPU" : "CPU";
        ArtfScene* s = bleedScene();
        CHECK(fabsf(artfRendererRenderScale(r) - .9f) < 1e-6f);
        for (int i = 0; i < 4; ++i) CHECK(artfRendererDraw(r, s));
        artfRendererSetPacing(r, .001f, .5f);
        for (int i = 0; i < 4; ++i) CHECK(artfRendererDraw(r, s));
        const float down = artfRendererRenderScale(r);
        artfRendererSetPacing(r, 1e6f, .5f);
        int frames = 0;
        while (artfRendererRenderScale(r) < .9f && frames < 400) {
            CHECK(artfRendererDraw(r, s));
            ++frames;
        }
        const float up = artfRendererRenderScale(r);
        /* (a frame said to take 8 ms, at .9: a budget of 10 holds it; near throttling, 10 is 6, and the scene goes to
         * .9 times the square root of 6 / 8, by thirty-seconds down: .75) */
        artfPaceTestMs = 8;
        artfRendererSetPacing(r, 10, .5f);
        for (int i = 0; i < 40; ++i) CHECK(artfRendererDraw(r, s));
        const float cool = artfRendererRenderScale(r);
        artfRendererSetThermalHeadroom(r, 1);
        for (int i = 0; i < 40; ++i) CHECK(artfRendererDraw(r, s));
        const float hot = artfRendererRenderScale(r);
        artfPaceTestMs = 0;
        uint32_t* a = artfMalloc(W * H * 4);
        CHECK(artfRendererReadPixels(r, a)); /* (the frame keeps its size) */
        artfFree(a);
        printf("%s, pacing: at a budget no frame keeps, %.3f of the width at once; back to %.3f in %d frames; 8 ms of 10 "
               "held at %.3f, and near throttling, %.3f\n", name, down, up, frames, cool, hot);
        CHECK(fabsf(down - .5f) < 1e-6f && fabsf(up - .9f) < 1e-6f && frames < 300 && fabsf(cool - .9f) < 1e-6f && fabsf(hot - .75f) < 1e-6f);
        artfSceneFree(s);
        artfRendererFree(r);
    }
}

/* Turned as it is made (ArtfRendererOptions.preRotate): with the swapchain's turn a quarter, a half and three quarters
 * clockwise, the image is the frame unturned, turned so, to the pixel, the overlay with it. (A test hook stands for a
 * turned screen: what the device's compositor then does with it is checked on a device.) */
void testPreRotate(void) {
#ifdef ARTFUL_VULKAN
    uint32_t* frames[4];
    for (int k = 0; k < 4; ++k) frames[k] = artfMalloc(W * H * 4);
    uint32_t* corner = artfMalloc(W * H * 4);
    for (int i = 0; i < W * H; ++i) corner[i] = (i % W < 40 && i / W < 20) ? 0xFF2040C0u : 0; /* (a bar at the top left) */
    const ArtfOverlay overlay = {corner, W, H, 0, false, NULL, 0};
    bool drawn = true;
    for (int turn = 0; turn < 4 && drawn; ++turn) {
        ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
        o.hdr = true;
        o.preRotate = true;
        artfVulkanTestTurn = turn;
        ArtfRenderer* r = artfRendererNew(&o);
        artfVulkanTestTurn = 0;
        if (!r) {
            drawn = false;
            break;
        }
        ArtfScene* s = bleedScene();
        artfRendererSetOverlay(r, &overlay);
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, frames[turn]));
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (drawn) {
        int apart[4] = {0, 0, 0, 0};
        for (int turn = 1; turn < 4; ++turn) {
            const int across = turn & 1 ? H : W; /* (the image's width) */
            for (int ny = 0; ny < (turn & 1 ? W : H); ++ny)
                for (int nx = 0; nx < across; ++nx) {
                    const int x = turn == 1 ? ny : turn == 2 ? W - 1 - nx : W - 1 - ny, y = turn == 1 ? H - 1 - nx : turn == 2 ? H - 1 - ny : nx;
                    apart[turn] += frames[turn][ny * across + nx] != frames[0][y * W + x];
                }
        }
        printf("turned as made, a quarter, a half, three quarters: %d, %d and %d pixels unlike the frame turned\n", apart[1], apart[2], apart[3]);
        CHECK(apart[1] == 0 && apart[2] == 0 && apart[3] == 0);
        /* (and the frame shows the overlay at all) */
        CHECK(frames[0][5 * W + 5] != frames[0][100 * W + 100]);
    }
    for (int k = 0; k < 4; ++k) artfFree(frames[k]);
    artfFree(corner);
#endif
}

/* Occlusion and motion blur's velocities in the memory of the scene's light (the frame graph's aliasing): the same
 * bytes as with a buffer of their own, every effect on, over frames (what the frame before wrote is there when the
 * scene draws again). */
void testPostAliasing(void) {
#ifdef ARTFUL_VULKAN
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    bool drawn = true;
    for (int alone = 0; alone < 2 && drawn; ++alone) {
        artfVulkanNoAliasing = alone != 0;
        ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        artfVulkanNoAliasing = false;
        if (!r) {
            drawn = false;
            break;
        }
        ArtfScene* s = bleedScene();
        const ArtfInstance ball = addAt(s, artfSceneAddSphere(s, .5f, 16, 24), whiteMaterial(s), 0, 0, 0, 1);
        const ArtfPostDesc all = {.lens.bloom = .3f, .screen.occlusion = 1, .screen.reflections = 1, .screen.temporal = true, .lens.motionBlur = .5f};
        artfSceneSetPost(s, &all);
        for (int f = 0; f < 6; ++f) {
            const ArtfTransform t = artfTransformAt(.2f * (float)f, 0, 0, 1);
            artfInstanceSetTransform(s, ball, &t);
            CHECK(artfRendererDraw(r, s));
        }
        CHECK(artfRendererReadPixels(r, alone ? b : a));
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (drawn) {
        printf("occlusion and velocities over the scene's light, against a buffer of their own: %.4f%% of pixels apart\n", differentShare(a, b, 0) * 100);
        CHECK(differentShare(a, b, 0) == 0);
    }
    artfFree(a);
    artfFree(b);
#endif
}

/* A board of fine squares, a few pixels each, filling the view: what reading the history between pixels blurs. */
static ArtfScene* fineBoardScene(ArtfInstance* board) {
    ArtfScene* s = plainScene();
    static uint32_t squares[64 * 64];
    for (int i = 0; i < 64 * 64; ++i) squares[i] = ((i % 64) + (i / 64)) & 1 ? 0xFFFFFFFFu : 0xFF202020u;
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTexture(s, squares, 64, 64, 0), 1, 0, artfVec3(0, 0, 0)};
    *board = addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, -.5f, 2.5f);
    return s;
}

/* The history by Catmull-Rom's cubic (ArtfScreenDesc.sharpHistory): with temporal antialiasing at the frame's size and
 * upscaled from half, a board of fine squares going across is nearer the truth (it still, drawn at 4 x 4 and taken
 * down) than when its history is read between the nearest four; the same on both backends; and on the CPU, its lanes
 * and a pixel at a time make the same bytes. */
void testSharpHistory(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* last[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    double moving[2][2][2] = {{{0}}};
    bool drawn[2] = {false, false};
    for (int backend = 0; backend < 2; ++backend)
        for (int scaled = 0; scaled < 2; ++scaled)
            for (int sharp = 0; sharp < 2; ++sharp) {
                ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
                o.hdr = true;
                o.renderScale = scaled ? .5f : 0;
                ArtfRenderer* r = artfRendererNew(&o);
                ArtfRenderer* plain = artfRendererNew(&o);
                if (!r || !plain) {
                    artfRendererFree(r);
                    artfRendererFree(plain);
                    continue;
                }
                const ArtfPostDesc post = {.screen.temporal = true, .screen.sharpHistory = sharp != 0};
                ArtfInstance board;
                ArtfScene* s = fineBoardScene(&board);
                artfSceneSetPost(s, &post);
                for (int k = 0; k < 24; ++k) {
                    const ArtfTransform at = artfTransformAt(-.3f + .0137f * (float)k, 0, -.5f, 2.5f);
                    artfInstanceSetTransform(s, board, &at);
                    drawFrames(r, s, 1, a);
                }
                /* (against the truth: it still where it ended, drawn at 4 x 4 the size, each 4 x 4 a pixel by its mean in
                 * light, the sRGB of what its sixteen show) */
                {
                    ArtfRendererOptions big = options(ARTF_BACKEND_SOFTWARE, 0, false);
                    big.width = W * 4;
                    big.height = H * 4;
                    big.hdr = true;
                    ArtfRenderer* fine = artfRendererNew(&big);
                    uint32_t* many = artfMalloc((size_t)W * H * 16 * 4);
                    artfSceneSetPost(s, NULL);
                    CHECK(artfRendererDraw(fine, s));
                    CHECK(artfRendererReadPixels(fine, many));
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x) {
                            float sum[3] = {0, 0, 0};
                            for (int j = 0; j < 4; ++j)
                                for (int i = 0; i < 4; ++i)
                                    for (int c = 0; c < 3; ++c) sum[c] += artfSrgbDecode((float)CHANNEL(many[(size_t)(y * 4 + j) * W * 4 + x * 4 + i], 8 * c) / 255.f);
                            uint32_t px = 0xFF000000u;
                            for (int c = 0; c < 3; ++c) px |= (uint32_t)(artfSrgbEncode(sum[c] / 16.f) * 255.f + .5f) << (8 * c);
                            b[y * W + x] = px;
                        }
                    artfFree(many);
                    artfRendererFree(fine);
                }
                moving[backend][scaled][sharp] = meanApart(a, b);
                if (sharp && !scaled) memcpy(last[backend], a, W * H * 4);
                drawn[backend] = true;
                artfSceneFree(s);
                artfRendererFree(r);
                artfRendererFree(plain);
            }
    for (int backend = 0; backend < 2; ++backend) {
        if (!drawn[backend]) continue;
        printf("%s, a fine board going across, from the truth (drawn at 4 x 4): %.2f bilinear, %.2f cubic; upscaled from half, %.2f and %.2f\n", backend ? "GPU" : "CPU", moving[backend][0][0],
               moving[backend][0][1], moving[backend][1][0], moving[backend][1][1]);
        CHECK(moving[backend][0][1] < moving[backend][0][0] && moving[backend][1][1] < moving[backend][1][0]);
    }
    if (drawn[1]) {
        const double apart = differentShare(last[0], last[1], 16);
        printf("the cubic history, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .01);
    }
    /* (the CPU's lanes of each instruction set, and a pixel at a time) */
    static const ArtfSimd sets[3] = {ARTF_SIMD_AUTO, ARTF_SIMD_128, ARTF_SIMD_PORTABLE};
    for (int scaled = 0; scaled < 2; ++scaled) {
        uint64_t hashes[3];
        for (int k = 0; k < 3; ++k) {
            ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, k == 2 ? 1 : 0, false);
            o.hdr = true;
            o.simd = sets[k];
            o.renderScale = scaled ? .5f : 0;
            ArtfRenderer* r = artfRendererNew(&o);
            const ArtfPostDesc post = {.screen.temporal = true, .screen.sharpHistory = true};
            ArtfInstance board;
            ArtfScene* s = fineBoardScene(&board);
            artfSceneSetPost(s, &post);
            for (int f = 0; f < 6; ++f) {
                const ArtfTransform at = artfTransformAt(-.3f + .0137f * (float)f, 0, -.5f, 2.5f);
                artfInstanceSetTransform(s, board, &at);
                drawFrames(r, s, 1, a);
            }
            hashes[k] = hashOf(a, (size_t)W * H);
            artfSceneFree(s);
            artfRendererFree(r);
        }
        CHECK(hashes[0] == hashes[1] && hashes[1] == hashes[2]);
    }
    artfFree(a);
    artfFree(b);
    artfFree(last[0]);
    artfFree(last[1]);
}

/* PQ (SMPTE ST 2084) of nits, by the standard's own arithmetic in doubles, as a 10-bit code. */
static int pqCode(double nits) {
    const double y = pow(nits / 10000.0, .1593017578125);
    return (int)(pow((.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375) * 1023.0 + .5);
}

/* Frames made for other screens (ArtfRendererOptions.output): sRGB's red in Display-P3 is 234 51 35; in HDR10 white is
 * paper white (203 nits) and red Rec. 2020's share of it; an overlay's white is paper white over it; both backends. */
void testDisplayOutput(void) {
    uint32_t* frame = artfMalloc(W * H * 4);
    uint32_t* overlay = artfMalloc(W * H * 4);
    for (int i = 0; i < W * H; ++i) overlay[i] = i % W >= W / 2 ? 0xFFFFFFFFu : 0;
    /* what HDR10 should give: red is (.6274, .0691, .0164) of white in Rec. 2020 */
    const int white = pqCode(203.0), red[3] = {pqCode(203.0 * .6274039), pqCode(203.0 * .0690973), pqCode(203.0 * .0163914)};
    uint32_t got[2][4]; /* (of each backend: P3's red, HDR10's red, white, the overlay) */
    for (int backend = 0; backend < 2; ++backend) {
        for (int output = 1; output < 3; ++output) {
            ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
            o.hdr = true;
            o.output = (ArtfOutput)output;
            ArtfRenderer* r = artfRendererNew(&o);
            if (!r) break;
            CHECK(artfRendererOutput(r) == (ArtfOutput)output);
            ArtfScene* s = artfSceneNew();
            setCamera(s, 0, 0, 3, 0, 0, 0);
            artfSceneSetBackground(s, artfVec3(1, 0, 0));
            drawInto(r, s, frame);
            const uint32_t p = frame[(H / 2) * W + W / 4];
            if (output == 1) {
                got[backend][0] = p;
                printf("%s, Display-P3: sRGB's red is %u %u %u\n", backend ? "GPU" : "CPU", p >> 16 & 255, p >> 8 & 255, p & 255);
                CHECK(abs((int)(p >> 16 & 255) - 234) <= 1 && abs((int)(p >> 8 & 255) - 51) <= 1 && abs((int)(p & 255) - 35) <= 1 && p >> 24 == 255);
            } else {
                got[backend][1] = p;
                const int codes[3] = {(int)(p >> 20 & 1023), (int)(p >> 10 & 1023), (int)(p & 1023)};
                artfSceneSetBackground(s, artfVec3(1, 1, 1));
                drawInto(r, s, frame);
                const uint32_t w = got[backend][2] = frame[(H / 2) * W + W / 4];
                const ArtfOverlay over = {overlay, W, H, 0, false, NULL, 0};
                artfSceneSetBackground(s, artfVec3(1, 0, 0));
                artfRendererSetOverlay(r, &over);
                drawInto(r, s, frame);
                artfRendererSetOverlay(r, NULL);
                const uint32_t q = got[backend][3] = frame[(H / 2) * W + W * 3 / 4];
                printf("%s, HDR10: red %d %d %d (%d %d %d expected), white %u (%d), the overlay's white over red %u %u %u\n", backend ? "GPU" : "CPU", codes[0], codes[1], codes[2],
                       red[0], red[1], red[2], w & 1023, white, q >> 20 & 1023, q >> 10 & 1023, q & 1023);
                for (int c = 0; c < 3; ++c) CHECK(abs(codes[c] - red[c]) <= 2);
                CHECK(abs((int)(w & 1023) - white) <= 2 && abs((int)(w >> 10 & 1023) - white) <= 2 && abs((int)(w >> 20 & 1023) - white) <= 2 && w >> 30 == 3);
                CHECK(abs((int)(q & 1023) - white) <= 2 && abs((int)(q >> 20 & 1023) - white) <= 2 && q >> 30 == 3);
                CHECK((frame[(H / 2) * W + W / 4] & 0x3FFFFFFFu) == (p & 0x3FFFFFFFu));
            }
            artfSceneFree(s);
            artfRendererFree(r);
        }
    }
    {
        /* without hdr, sRGB all the same */
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
        o.output = ARTF_OUTPUT_HDR10;
        ArtfRenderer* r = artfRendererNew(&o);
        CHECK(artfRendererOutput(r) == ARTF_OUTPUT_SRGB);
        artfRendererFree(r);
    }
    artfFree(frame);
    artfFree(overlay);
}
