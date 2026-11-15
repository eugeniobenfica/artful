/* artful-tests: rays: tracing, the ray tracing hardware, rays while drawing, denoising, probes. */
#include "tests.h"

/* ---- noise ---- */

/* How far a picture is from another, on average, in steps of 255. */
static double errorOf(const uint32_t* a, const uint32_t* b) {
    double sum = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i)
        for (int shift = 0; shift < 24; shift += 8) sum += abs(CHANNEL(a[i], shift) - CHANNEL(b[i], shift));
    return sum / ((double)W * H * 3);
}

void testDenoise(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    artfSceneSetSunSize(s, .15f); /* (a wide sun: shadows that are all noise) */
    /* the truth, from many samples, and four samples with and without the filter */
    const ArtfTraceDesc truth = {ARTF_TRACE_PATHS, 8, 512, 0, false};
    artfSceneSetTrace(s, &truth);
    for (int i = 0; i < 64; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, c));
    const ArtfTraceDesc noisy = {ARTF_TRACE_PATHS, 1, 4, 0, false};
    artfSceneSetTrace(s, &noisy);
    artfRendererFree(r); /* (what was gathered would go on being shown) */
    r = artfRendererNew(&o);
    for (int i = 0; i < 4; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    const ArtfTraceDesc clean = {ARTF_TRACE_PATHS, 1, 0, 0, true};
    artfSceneSetTrace(s, &clean);
    for (int i = 0; i < 4; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, b));
    const double before = errorOf(a, c), after = errorOf(b, c);
    printf("from 512 samples: four samples are %.2f away, four filtered %.2f\n", before, after);
    CHECK(after < before * .6);

    /* the eye moves, and the picture does not start over: a frame later it is still closer than noise is */
    for (int i = 0; i < 12; ++i) CHECK(artfRendererDraw(r, s));
    setCamera(s, 2.05f, 5, 9, 0, 0, 0);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, b));
    artfSceneSetTrace(s, &truth);
    for (int i = 0; i < 64; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, c));
    const double moved = errorOf(b, c);
    printf("a frame after moving: %.2f away\n", moved);
    CHECK(moved < before * .6);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfFree(c);
    artfRendererFree(r);
}

/* A light that changes over what stays: what was gathered must not go on being shown. */
void testDenoiseChange(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    for (int kind = 0; kind < 2; ++kind) {
        ArtfScene* s = shadowScene();
        artfSceneSetSunSize(s, .05f);
        const ArtfVec3 sun = kind ? artfVec3(.7f, -1, .3f) : artfVec3(-.5f, -1, -.3f);
        const ArtfVec3 color = kind ? artfVec3(.9f, .9f, .9f) : artfVec3(0, 0, 0);
        artfSceneSetSun(s, sun, color);
        const ArtfTraceDesc truth = {ARTF_TRACE_PATHS, 8, 512, 0, false};
        artfSceneSetTrace(s, &truth);
        for (int i = 0; i < 64; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, c));
        artfRendererFree(r);
        r = artfRendererNew(&o);
        artfSceneSetSun(s, artfVec3(-.5f, -1, -.3f), artfVec3(.9f, .9f, .9f));
        const ArtfTraceDesc clean = {ARTF_TRACE_PATHS, 1, 0, 0, true};
        artfSceneSetTrace(s, &clean);
        for (int i = 0; i < 120; ++i) CHECK(artfRendererDraw(r, s));
        artfSceneSetSun(s, sun, color);
        for (int i = 0; i < 8; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        const double away = errorOf(a, c);
        printf("eight frames after the sun %s: %.2f away\n", kind ? "turned" : "went out", away);
        CHECK(away < 4);
        artfSceneFree(s);
    }
    artfFree(a);
    artfFree(c);
    artfRendererFree(r);
}

/* What moves keeps what it gathered: it goes with its object. */
void testDenoiseMotion(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    artfSceneSetSunSize(s, .15f);
    ArtfMaterialDesc m = {artfVec3(.8f, .3f, .3f), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfTransform at = artfTransformAt(-2, 2, 2, 1);
    const ArtfInstance moving = artfSceneAddInstance(s, artfSceneAddCube(s, 1), artfSceneAddMaterial(s, &m), &at);
    const ArtfTraceDesc clean = {ARTF_TRACE_PATHS, 1, 0, 0, true};
    artfSceneSetTrace(s, &clean);
    for (int i = 0; i < 40; ++i) {
        at = artfTransformAt(-2.f + (float)i * .05f, 2, 2, 1);
        artfInstanceSetTransform(s, moving, &at);
        CHECK(artfRendererDraw(r, s));
    }
    CHECK(artfRendererReadPixels(r, a));
    artfRendererFree(r);
    r = artfRendererNew(&o);
    const ArtfTraceDesc truth = {ARTF_TRACE_PATHS, 8, 512, 0, false};
    artfSceneSetTrace(s, &truth);
    for (int i = 0; i < 64; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, c));
    const double away = errorOf(a, c);
    printf("after forty frames of motion: %.2f away\n", away);
    CHECK(away < 3);
    artfSceneFree(s);
    artfFree(a);
    artfFree(c);
    artfRendererFree(r);
}

/* What bones bend keeps what it gathered, as what moves whole does. */
void testDenoiseBones(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    artfSceneSetSunSize(s, .15f);
    ArtfMaterialDesc m = {artfVec3(.8f, .3f, .3f), 0, 0, 1, artfVec3(0, 0, 0)};
    m.doubleSided = true;
    ArtfTransform at = artfTransformTrs(artfVec3(0, -1, 3), artfQuatAxisAngle(artfVec3(0, 1, 0), 0), artfVec3(9, 4, 9));
    const ArtfInstance strip = artfSceneAddInstance(s, addStrip(s), artfSceneAddMaterial(s, &m), &at);
    const ArtfTraceDesc clean = {ARTF_TRACE_PATHS, 1, 0, 0, true};
    artfSceneSetTrace(s, &clean);
    for (int i = 0; i < 20; ++i) {
        /* (carried along by its bones, straight off its surface: nothing else would find it again) */
        ArtfTransform joints[2] = {artfTransformIdentity(), artfTransformIdentity()};
        joints[0].m[11] = joints[1].m[11] = (float)(i - 19) * .05f;
        artfInstanceSetJoints(s, strip, joints, 2);
        CHECK(artfRendererDraw(r, s));
    }
    CHECK(artfRendererReadPixels(r, a));
    /* where a ray lands on it, the guide knows how far back it was: .05 of the object, stretched nine times */
    if (backend == ARTF_BACKEND_SOFTWARE) {
        ArtfFrameData frame;
        ArtfTraceFrame traced;
        artfSceneFrame(s, W, H, &frame);
        artfSceneTraceFrame(s, &frame, W, H, 0, &traced);
        int found = 0;
        for (uint32_t y = 0; y < H; y += 8)
            for (uint32_t x = 0; x < W; x += 8) {
                float light[4] = {0, 0, 0, 0};
                ArtfTraceGuide guide;
                memset(&guide, 0, sizeof guide);
                artfTraceSample(s, &frame, &traced, x, y, 0, light, &guide);
                found += fabsf(guide.shift[2] + .45f) < .05f;
            }
        printf("pixels whose guide knows: %d\n", found);
        CHECK(found > 10);
    }
    artfRendererFree(r);
    r = artfRendererNew(&o);
    const ArtfTraceDesc truth = {ARTF_TRACE_PATHS, 8, 512, 0, false};
    artfSceneSetTrace(s, &truth);
    for (int i = 0; i < 64; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, c));
    const double away = errorOf(a, c);
    printf("after twenty frames of being carried by bones: %.2f away\n", away);
    CHECK(away < 3);
    artfSceneFree(s);
    artfFree(a);
    artfFree(c);
    artfRendererFree(r);
}

void testDenoiseBytes(void) {
    ArtfScene* s = shadowScene();
    const ArtfTraceDesc clean = {ARTF_TRACE_RAYS, 1, 0, 0, true};
    artfSceneSetTrace(s, &clean);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    uint64_t reference = 0;
    for (int threads = 1; threads <= 4; threads += 3) {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, threads, false);
        ArtfRenderer* r = artfRendererNew(&o);
        drawSamples(r, s, 6, a);
        const uint64_t h = hashOf(a, (size_t)W * H);
        if (threads == 1) reference = h;
        CHECK(h == reference);
        artfRendererFree(r);
    }
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawSamples(gpu, s, 6, b);
        const double far = differentShare(a, b, 24);
        printf("the filter on both backends, pixels that differ by more than 24 of 255: %.2f%%\n", far * 100);
        CHECK(far < .02);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    artfSceneFree(s);
}

/* Light that bounced, in what a surface reflects: a metal floor, which takes no light of its own, by a red wall turns
 * red once probes are on; both backends alike. */
void testProbeReflections(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b[2] = {artfCalloc(W * H, 4), artfCalloc(W * H, 4)}; /* (zeros: where Vulkan does not run, the GPU's stays so) */
    const int fx = W / 2 + 28, fy = H / 2 + 34;
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* s = bleedScene();
        ArtfMaterialDesc metal = artfMaterialDefaults();
        metal.baseColor = artfVec3(.9f, .9f, .9f);
        metal.metallic = 1;
        metal.roughness = .35f;
        artfMaterialSet(s, 1, &metal);
        drawInto(r, s, a);
        const ArtfIndirectDesc probes = {{8, 4, 8}, 64, .5f, 1, {-4, -1.5f, -3}, {4, 3, 3}};
        artfSceneSetIndirect(s, &probes);
        for (int k = 0; k < 4; ++k) drawInto(r, s, b[backend]);
        const double before = meanOf(a, fx, fy, 16) - meanOf(a, fx, fy, 8), after = meanOf(b[backend], fx, fy, 16) - meanOf(b[backend], fx, fy, 8);
        printf("%s, light that bounced in a metal floor: red over green %.1f without probes, %.1f with\n", backend ? "GPU" : "CPU", before, after);
        CHECK(after > before + 3);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (b[1][0] || b[1][W * H / 2]) {
        const double apart = differentShare(b[0], b[1], 24);
        printf("light that bounced in a metal floor, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .02);
    }
    artfFree(a);
    artfFree(b[0]);
    artfFree(b[1]);
}

/* Rays through the ray tracing hardware, where the GPU has some: the same picture as through the trees, rays and paths,
 * with what has one side and two, and after something moves. */
static void sawHardware(void* user, const char* message) {
    if (strstr(message, "ray tracing hardware")) *(bool*)user = true;
}

void testTraceHardware(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    bool hardware = false;
    artfSetLog(sawHardware, &hardware);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) {
        artfSetLog(NULL, NULL);
        return;
    }
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    for (int mode = 0; mode < 2; ++mode) {
        ArtfScene* s = bleedScene();
        ArtfMaterialDesc twoSided = artfMaterialDefaults();
        twoSided.baseColor = artfVec3(.2f, .4f, .9f);
        twoSided.doubleSided = true;
        const ArtfTransform card = artfTransformTrs(artfVec3(-2.4f, .2f, -.5f), artfQuatAxisAngle(artfVec3(0, 1, 0), 2.6f), artfVec3(1, 1, 1));
        const ArtfInstance back = artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &twoSided), &card);
        ArtfTraceDesc d = {mode ? ARTF_TRACE_PATHS : ARTF_TRACE_RAYS, 4, 64, 0, false, false};
        for (int moved = 0; moved < 2; ++moved) {
            if (moved) {
                const ArtfTransform there = artfTransformTrs(artfVec3(-2.f, .6f, -.2f), artfQuatAxisAngle(artfVec3(0, 1, 0), 2.2f), artfVec3(1, 1, 1));
                artfInstanceSetTransform(s, back, &there);
            }
            d.hardware = false;
            artfSceneSetTrace(s, &d);
            for (int k = 0; k < 16; ++k) drawInto(gpu, s, a);
            d.hardware = true;
            artfSceneSetTrace(s, &d);
            for (int k = 0; k < 16; ++k) drawInto(gpu, s, b);
            const double apart = differentShare(a, b, 24);
            printf("%s%s through the ray tracing hardware against the trees: %.2f%% of pixels apart\n", mode ? "paths" : "rays", moved ? ", moved," : "", apart * 100);
            CHECK(apart < .01);
        }
        artfSceneFree(s);
    }
    artfSetLog(NULL, NULL);
    printf(hardware ? "(rays went through the ray tracing hardware)\n" : "(no ray tracing hardware here: the trees were held to themselves)\n");
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
}

/* A Vulkan renderer as on a device with no ray tracing hardware. */
static ArtfRenderer* gpuWithoutRayQueries(const ArtfRendererOptions* o) {
#ifdef _WIN32
    _putenv("ARTFUL_NO_RAY_QUERY=1");
#else
    setenv("ARTFUL_NO_RAY_QUERY", "1", 1);
#endif
    ArtfRenderer* r = artfRendererNew(o);
#ifdef _WIN32
    _putenv("ARTFUL_NO_RAY_QUERY=");
#else
    unsetenv("ARTFUL_NO_RAY_QUERY");
#endif
    return r;
}

/* A floor, a cube over it, a pane over it that lets half the light through, a light that casts, and the sun. */
static ArtfScene* tracedScene(float sunSize, float lampSize) {
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(.3f, -1, .2f), artfVec3(1.2f, 1.2f, 1.2f));
    artfSceneSetSunSize(s, sunSize);
    artfSceneSetAmbient(s, artfVec3(.05f, .05f, .05f), artfVec3(.05f, .05f, .05f));
    setCamera(s, 0, 4, 6, 0, 0, 0);
    ArtfMaterialDesc grey = artfMaterialDefaults();
    grey.baseColor = artfVec3(.6f, .6f, .6f);
    addAt(s, artfSceneAddPlane(s, 20, 1), artfSceneAddMaterial(s, &grey), 0, -1, 0, 1);
    addAt(s, artfSceneAddCube(s, 1), whiteMaterial(s), -1, 0, 0, 1);
    const ArtfTransform flat = artfTransformTrs(artfVec3(1.5f, .5f, 0), artfQuatAxisAngle(artfVec3(1, 0, 0), -1.5708f), artfVec3(1, 1, 1));
    const ArtfInstance pane = artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), whiteMaterial(s), &flat);
    artfInstanceSetOpacity(s, pane, .5f);
    const ArtfLight lamp = artfSceneAddLight(s, artfVec3(1.5f, 2.5f, 0), artfVec3(.8f, .8f, .8f), 9);
    artfLightSetShadow(s, lamp, true);
    artfLightSetSize(s, lamp, lampSize);
    return s;
}

/* Shadows from rays (ArtfShadowDesc.traced): exact, through what shows through as much as it is not there, as the
 * tracer's own; soft by the lights' sizes; the same on the CPU, on the GPU through the ray tracing hardware, and on a GPU
 * without; and with them off again, the bytes as before. */
void testTracedShadows(void) {
    const ArtfShadowDesc maps = {2048, 20, false, false}, traced = {0, 0, false, true};
    {
        /* where the trees say: under the cube nothing of the sun, under the pane half, in the open all of it */
        ArtfScene* s = tracedScene(0, 0);
        artfSceneSetShadows(s, &traced);
        artfSceneTraceSync(s);
        ArtfFrameData f;
        artfSceneFrame(s, W, H, &f);
        const float up[3] = {0, 1, 0}, underCube[3] = {-.7f, -1, .2f}, underPane[3] = {1.95f, -1, .3f}, open[3] = {3, -1, -3}, belowLamp[3] = {1.5f, -1, 0};
        const float a = artfTracedShadow(s, &f, underCube, up, -1), b = artfTracedShadow(s, &f, underPane, up, -1), c = artfTracedShadow(s, &f, open, up, -1);
        const float d = artfTracedShadow(s, &f, belowLamp, up, 0);
        printf("shadows from rays: under a cube %.3f of the sun, under a pane of half %.3f, in the open %.3f; under the pane, of the lamp %.3f\n", a, b, c, d);
        CHECK(a == 0 && c == 1 && fabsf(b - .5f) < .01f && fabsf(d - .5f) < .01f);
        CHECK(f.tracedCasts == (ARTF_TRACED_SUN | 1u) && f.traced == ARTF_TRACED_SHADOWS);
        artfSceneFree(s);
    }
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* frames[3][2];
    for (int k = 0; k < 3; ++k) frames[k][0] = frames[k][1] = NULL;
    bool hardware = false;
    artfSetLog(sawHardware, &hardware);
    for (int backend = 0; backend < 3; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = backend == 2 ? gpuWithoutRayQueries(&o) : artfRendererNew(&o);
        if (!r) continue;
        const char* name = backend == 0 ? "CPU" : backend == 1 ? "GPU" : "GPU without ray queries";
        ArtfScene* s = tracedScene(0, 0);
        /* against maps: the same shadows, but at their edges */
        artfSceneSetShadows(s, &maps);
        drawInto(r, s, a);
        artfSceneSetShadows(s, &traced);
        drawInto(r, s, b);
        const double apart = differentShare(a, b, 24);
        printf("%s, shadows from rays against maps: %.2f%% of pixels apart\n", name, apart * 100);
        CHECK(apart < .03);
        uint32_t** hard = &frames[0][backend > 0];
        if (backend < 2) {
            *hard = artfMalloc(W * H * 4);
            memcpy(*hard, b, W * H * 4);
        } else if (*hard) {
            const double rq = differentShare(*hard, b, 24);
            printf("shadows from rays on the GPU, through the trees against the hardware: %.2f%% of pixels apart\n", rq * 100);
            CHECK(rq < .005);
        }
        /* off again: as they were, to the byte */
        artfSceneSetShadows(s, &maps);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 0) == 0);
        artfSceneFree(s);
        /* soft: the light's size and the sun's make their shadows wider */
        for (int sun = 0; sun < 2; ++sun) {
            ArtfScene* bulb = bulbScene(.5f);
            if (sun) {
                artfSceneRemoveLight(bulb, 1);
                artfSceneSetSun(bulb, artfVec3(.3f, -1, .4f), artfVec3(1.5f, 1.5f, 1.5f));
                artfSceneSetSunSize(bulb, .25f);
            } else {
                artfLightSetShadow(bulb, 1, true);
            }
            artfSceneSetShadows(bulb, &traced);
            drawInto(r, bulb, b);
            if (sun) artfSceneSetSunSize(bulb, 0);
            else artfLightSetSize(bulb, 1, 0);
            drawInto(r, bulb, a);
            printf("%s, soft shadows from rays, from %s: %d pixels between light and shadow, %d sharp\n", name, sun ? "the sun" : "a light", greyCount(b), greyCount(a));
            CHECK(greyCount(b) > greyCount(a) + 100);
            uint32_t** kept = &frames[1 + sun][backend > 0];
            if (backend < 2) {
                *kept = artfMalloc(W * H * 4);
                memcpy(*kept, b, W * H * 4);
            } else if (*kept) {
                const double rq = differentShare(*kept, b, 24);
                printf("soft shadows from rays on the GPU, through the trees against the hardware: %.2f%% of pixels apart\n", rq * 100);
                CHECK(rq < .005);
            }
            artfSceneFree(bulb);
        }
        artfRendererFree(r);
    }
    artfSetLog(NULL, NULL);
    for (int k = 0; k < 3; ++k) {
        if (frames[k][0] && frames[k][1]) {
            const double apart = differentShare(frames[k][0], frames[k][1], 24);
            printf("shadows from rays%s, the GPU against the CPU: %.2f%% of pixels apart\n", k == 0 ? "" : k == 1 ? ", soft from a light" : ", soft from the sun", apart * 100);
            CHECK(apart < .01);
        }
        artfFree(frames[k][0]);
        artfFree(frames[k][1]);
    }
    printf(hardware ? "(shadows from rays went through the ray tracing hardware)\n" : "(no ray tracing hardware here)\n");
    artfFree(a);
    artfFree(b);
}

/* What bones bend has its bottom level refitted each frame it moves, alone, and that draws as building all does: a
 * strip bent over a floor, its shadow from rays, posed step by step on one GPU, and at the end drawn by another that
 * builds all at once. (A refit of a level not built to allow one changed nothing on a Radeon RX 9060 XT: this was
 * 0.09% of pixels apart. See slotMeshSync in vulkan.c, and testTracedRefit.) */
void testTracedMoved(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* stepped = artfRendererNew(&o);
    ArtfRenderer* fresh = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    const ArtfShadowDesc traced = {0, 0, false, true};
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(.5f, -1, -.4f), artfVec3(1.2f, 1.2f, 1.2f));
    artfSceneSetAmbient(s, artfVec3(.05f, .05f, .05f), artfVec3(.05f, .05f, .05f));
    setCamera(s, 0, 3, 5, 0, .5f, 0);
    addAt(s, artfSceneAddPlane(s, 20, 1), whiteMaterial(s), 0, 0, 0, 1);
    const ArtfInstance strip = addAt(s, addStrip(s), whiteMaterial(s), 0, 0, 0, 1);
    artfSceneSetShadows(s, &traced);
    if (stepped && fresh && cpu) {
        for (int step = 0; step <= 6; ++step) {
            bendStrip(s, strip, .15f * (float)step);
            CHECK(artfRendererDraw(stepped, s));
            CHECK(artfRendererDraw(cpu, s));
        }
        drawInto(stepped, s, a);
        drawInto(fresh, s, b);
        drawInto(cpu, s, c);
        const double apart = differentShare(a, b, 0), cpuApart = differentShare(b, c, 24);
        printf("shadows from rays of what bones bend, refitted as it moved, against built whole at once: %.3f%% of pixels apart; the GPU against the CPU: %.2f%%\n", apart * 100, cpuApart * 100);
        CHECK(apart == 0);
        CHECK(cpuApart < .01);
        /* and it was bent: the shadow is not where it was */
        bendStrip(s, strip, 0);
        drawInto(fresh, s, c);
        CHECK(differentShare(b, c, 24) > .002);
    }
    artfSceneFree(s);
    artfRendererFree(stepped);
    artfRendererFree(fresh);
    artfRendererFree(cpu);
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* addStrip's bones: both carried to (x, 0, z), the second bent around the joint by angle. */
static void carryStrip(ArtfScene* s, ArtfInstance i, float x, float z, float angle) {
    const ArtfTransform turn = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 0, 1), angle), artfVec3(1, 1, 1));
    ArtfTransform joints[2] = {artfTransformIdentity(), turn};
    joints[1].m[3] = -turn.m[1];
    joints[1].m[7] = 1.f - turn.m[5];
    for (int k = 0; k < 2; ++k) {
        joints[k].m[3] += x;
        joints[k].m[11] += z;
    }
    artfInstanceSetJoints(s, i, joints, 2);
}

/* The hardware's refit of what bones move: a strip that its bones carry far across a floor, bending as they go, its
 * shadow from rays drawn frame by frame by one GPU, which refits it, against the CPU each frame, and now and then
 * against a GPU that builds it whole. A refit from the wrong positions, or one that did nothing, leaves a shadow where
 * the strip was. Then a mesh that comes is built alone, and one that goes takes none built again but what bends. */
void testTracedRefit(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* stepped = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_VULKAN;
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    const ArtfShadowDesc traced = {0, 0, false, true};
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(.3f, -1, -.5f), artfVec3(1.2f, 1.2f, 1.2f));
    artfSceneSetAmbient(s, artfVec3(.05f, .05f, .05f), artfVec3(.05f, .05f, .05f));
    setCamera(s, 1.5f, 5, 6, 1.5f, 0, -.5f);
    addAt(s, artfSceneAddPlane(s, 30, 1), whiteMaterial(s), 0, 0, 0, 1);
    const ArtfMesh cube = artfSceneAddCube(s, .5f);
    const ArtfInstance block = addAt(s, cube, whiteMaterial(s), -1.5f, .5f, -1, 1);
    const ArtfInstance strip = addAt(s, addStrip(s), whiteMaterial(s), 0, 0, 0, 1);
    artfSceneSetShadows(s, &traced);
    if (stepped && cpu) {
        uint64_t before[2] = {artfVulkanBottomLevels[0], artfVulkanBottomLevels[1]};
        double worst = 0, worstFresh = 0;
        int frames = 0;
        for (int step = 0; step <= 20; ++step) {
            /* (3 across and 1.5 back, much more than the strip's own size) */
            carryStrip(s, strip, .15f * (float)step, -.075f * (float)step, .6f * sinf((float)step * .5f));
            CHECK(artfRendererDraw(stepped, s));
            CHECK(artfRendererReadPixels(stepped, a));
            CHECK(artfRendererDraw(cpu, s));
            CHECK(artfRendererReadPixels(cpu, b));
            const double apart = differentShare(a, b, 24);
            worst = apart > worst ? apart : worst;
            ++frames;
            if (step % 5 == 4) {
                /* (what it builds is not the stepped one's) */
                const uint64_t own[2] = {artfVulkanBottomLevels[0], artfVulkanBottomLevels[1]};
                ArtfRenderer* fresh = artfRendererNew(&o);
                CHECK(fresh != NULL);
                if (!fresh) break;
                CHECK(artfRendererDraw(fresh, s));
                CHECK(artfRendererReadPixels(fresh, c));
                const double off = differentShare(a, c, 0);
                worstFresh = off > worstFresh ? off : worstFresh;
                artfRendererFree(fresh);
                for (int k = 0; k < 2; ++k) before[k] += artfVulkanBottomLevels[k] - own[k];
            }
        }
        const uint64_t built = artfVulkanBottomLevels[0] - before[0], refitted = artfVulkanBottomLevels[1] - before[1];
        printf("shadows from rays of a strip that bones carry far, %d frames: the GPU against the CPU at worst %.3f%% of pixels apart, "
               "refitted against built whole %.3f%%; %u bottom levels built, %u refitted\n",
               frames, worst * 100, worstFresh * 100, (unsigned)built, (unsigned)refitted);
        CHECK(worst < .005);
        CHECK(worstFresh == 0);
        /* (with the hardware: the floor, the cube, the strip and the strip's own copy the instance bends, built once, and
         * the copy refitted each frame after) */
        const bool hardware = built > 0;
        if (hardware) {
            CHECK(built == 4);
            CHECK(refitted == (uint64_t)frames - 1);
            /* a mesh that comes is built alone, and nothing moved is touched */
            addAt(s, artfSceneAddSphere(s, .4f, 8, 12), whiteMaterial(s), 3, .4f, 1, 1);
            CHECK(artfRendererDraw(stepped, s));
            CHECK(artfVulkanBottomLevels[0] - before[0] == 5 && artfVulkanBottomLevels[1] - before[1] == refitted);
            /* one that goes: none built again but the two that bend, as tracing makes its trees over */
            artfSceneRemoveInstance(s, block);
            CHECK(artfSceneRemoveMesh(s, cube));
            CHECK(artfRendererDraw(stepped, s));
            CHECK(artfVulkanBottomLevels[0] - before[0] == 7 && artfVulkanBottomLevels[1] - before[1] == refitted);
            CHECK(artfRendererReadPixels(stepped, a));
            drawInto(cpu, s, b);
            const double after = differentShare(a, b, 24);
            printf("after a mesh came and one went: the GPU against the CPU %.3f%% of pixels apart\n", after * 100);
            CHECK(after < .005);
            /* a refit moves the boxes, not the tree: after 60 the strip is built whole again (vulkan.c's RT_REFITS) */
            const uint64_t was[2] = {artfVulkanBottomLevels[0], artfVulkanBottomLevels[1]};
            for (int step = 0; step < 65; ++step) {
                carryStrip(s, strip, 3.f - .05f * (float)step, -1.5f + .02f * (float)step, .6f * sinf((float)step * .3f));
                CHECK(artfRendererDraw(stepped, s));
            }
            CHECK(artfVulkanBottomLevels[0] - was[0] == 1 && artfVulkanBottomLevels[1] - was[1] == 64);
            CHECK(artfRendererReadPixels(stepped, a));
            drawInto(cpu, s, b);
            CHECK(differentShare(a, b, 24) < .005);
        }
    }
    artfSceneFree(s);
    artfRendererFree(stepped);
    artfRendererFree(cpu);
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* How much redder than green a part of the frame is. */
static double rednessOf(const uint32_t* f, int x0, int y0, int x1, int y1) {
    double sum = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) sum += (double)CHANNEL(f[y * W + x], 16) - (double)CHANNEL(f[y * W + x], 8);
    return sum / ((double)(x1 - x0) * (y1 - y0));
}

/* Reflections from rays (ArtfPostDesc.screen.tracedReflections): a mirror floor shows a red wall above the frame, which the
 * march across the frame cannot; the same on the CPU and the GPU, with ray tracing hardware and without. */
void testTracedReflections(void) {
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(0, -1, -.3f), artfVec3(.3f, .3f, .3f));
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    setCamera(s, 0, 2, 6, 0, 0, 0);
    ArtfMaterialDesc mirror = artfMaterialDefaults();
    mirror.baseColor = artfVec3(.9f, .9f, .9f);
    mirror.metallic = 1;
    mirror.roughness = .05f;
    addAt(s, artfSceneAddPlane(s, 40, 1), artfSceneAddMaterial(s, &mirror), 0, 0, 0, 1);
    ArtfMaterialDesc red = artfMaterialDefaults();
    red.baseColor = artfVec3(0, 0, 0);
    red.emissive = artfVec3(3, 0, 0);
    const ArtfTransform wall = artfTransformTrs(artfVec3(0, 10, -6), artfQuatIdentity(), artfVec3(8, 4, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &red), &wall);
    addAt(s, artfSceneAddSphere(s, .6f, 16, 24), whiteMaterial(s), 1.5f, .6f, 1, 1);
    uint32_t* frames[3] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    uint32_t* marched = artfMalloc(W * H * 4);
    bool drawn[3] = {false, false, false};
    for (int backend = 0; backend < 3; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = backend == 2 ? gpuWithoutRayQueries(&o) : artfRendererNew(&o);
        if (!r) continue;
        ArtfPostDesc post = {.screen.reflections = 1};
        artfSceneSetPost(s, &post);
        drawInto(r, s, marched);
        post.screen.tracedReflections = true;
        artfSceneSetPost(s, &post);
        drawInto(r, s, frames[backend]);
        drawn[backend] = true;
        const double before = rednessOf(marched, 120, 160, 200, 195), after = rednessOf(frames[backend], 120, 160, 200, 195);
        printf("%s, a wall above the frame in a mirror floor: red over green %.1f marched across the frame, %.1f from rays\n", backend == 0 ? "CPU" : backend == 1 ? "GPU" : "GPU without ray queries", before, after);
        CHECK(after > before + 30);
        artfRendererFree(r);
    }
    for (int k = 1; k < 3; ++k)
        if (drawn[0] && drawn[k]) {
            const double apart = differentShare(frames[0], frames[k], 24);
            printf("reflections from rays, the GPU%s against the CPU: %.2f%% of pixels apart\n", k == 2 ? " without ray queries" : "", apart * 100);
            CHECK(apart < .01);
        }
    artfSceneFree(s);
    for (int k = 0; k < 3; ++k) artfFree(frames[k]);
    artfFree(marched);
}

/* Light that bounced does not go through walls: inside a closed box under a bright sky, a point by the wall takes
 * next to nothing, though a probe outside is nearer to it than one inside. */
void testProbesWalls(void) {
    ArtfScene* s = artfSceneNew();
    setCamera(s, 0, 0, 10, 0, 0, 0);
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(1, 1, 1), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, artfSceneAddCube(s, 4), whiteMaterial(s), NULL);
    const ArtfIndirectDesc probes = {{8, 8, 8}, 128, .5f, 1, {-6, -6, -6}, {6, 6, 6}};
    artfSceneSetIndirect(s, &probes);
    ArtfFrameData frame;
    artfSceneFrame(s, W, H, &frame);
    for (int k = 0; k < 8; ++k) artfSceneIndirectStep(s, &frame, NULL);
    const float inside[3] = {1.8f, 0, 0}, inward[3] = {-1, 0, 0}, outside[3] = {2.3f, 0, 0}, outward[3] = {1, 0, 0};
    float in[3], out[3];
    artfIndirectAt(&s->indirect, inside, inward, in);
    artfIndirectAt(&s->indirect, outside, outward, out);
    printf("light that bounced, inside a box by its wall: %.3f of what is just outside\n", in[1] / out[1]);
    CHECK(out[1] > .5f && in[1] < out[1] * .1f);
    artfSceneFree(s);
    /* the kernels read the probes eight at a time: the same bytes as the portable ones, with walls and without */
    for (int through = 0; through < 2; ++through) {
        s = bleedScene();
        ArtfIndirectDesc d = {{8, 4, 8}, 64, .5f, 1, {-4, -1.5f, -3}, {4, 3, 3}, through != 0};
        uint32_t* a = artfMalloc(W * H * 4);
        uint32_t* b = artfMalloc(W * H * 4);
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* fast = artfRendererNew(&o);
        artfSceneSetIndirect(s, &d);
        drawInto(fast, s, a);
        o = options(ARTF_BACKEND_SOFTWARE, 0, true);
        ArtfRenderer* plain = artfRendererNew(&o);
        artfSceneSetIndirect(s, &d); /* (the probes start over: the same steps) */
        drawInto(plain, s, b);
        CHECK(differentShare(a, b, 0) == 0);
        artfRendererFree(fast);
        artfRendererFree(plain);
        artfSceneFree(s);
        artfFree(a);
        artfFree(b);
    }
}

/* Light that bounced, gathered by the GPU (artfShaderProbes) against the CPU's step, which is the reference: the same
 * rays the same ways, so the same probes but for roundings and a ray that grazes an edge. A floor, a red wall, a closed
 * box with probes inside it, a lamp and the sun; with walls and through them, through the hardware where there is
 * some and through the trees. */
void testProbesGpu(void) {
    enum { STEPS = 6 };
    for (int through = 0; through < 2; ++through)
        for (int way = 0; way < 2; ++way) {
            ArtfScene* s = bleedScene();
            addAt(s, artfSceneAddCube(s, 2), whiteMaterial(s), -2, .4f, 1.5f, 1); /* (no probe on its faces) */
            artfSceneAddLight(s, artfVec3(.5f, 1.5f, 1), artfVec3(2, 1.6f, 1.2f), 6);
            const ArtfIndirectDesc d = {{8, 4, 8}, 64, .5f, 1, {-4, -1.5f, -3}, {4, 3, 3}, through != 0};
            artfSceneSetIndirect(s, &d);
            const ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
            ArtfRenderer* r = way ? gpuWithoutRayQueries(&o) : artfRendererNew(&o);
            if (!r) {
                artfSceneFree(s);
                printf("(no Vulkan here: the GPU's probes were not gathered)\n");
                return;
            }
            for (int k = 0; k < STEPS; ++k) CHECK(artfRendererDraw(r, s));
            const uint32_t count = artfIndirectCount(&s->indirect);
            const size_t floats = (size_t)count * ARTF_PROBE_FLOATS;
            float* gpu = artfMalloc(floats * 4);
            const bool read = artfVulkanProbesRead(r, gpu, floats);
            CHECK(read);
            if (!way && !through) {
                /* the same renderer with the probes gathered on the CPU and sent up, as before: the same picture; and
                 * back on the GPU, from the start, the same probes to the bit */
                uint32_t* a = artfMalloc(W * H * 4);
                uint32_t* b = artfMalloc(W * H * 4);
                float* again = artfMalloc(floats * 4);
                CHECK(artfRendererReadPixels(r, a));
                artfVulkanProbesOnCpu = true;
                artfSceneSetIndirect(s, &d);
                for (int k = 0; k < STEPS; ++k) CHECK(artfRendererDraw(r, s));
                CHECK(artfRendererReadPixels(r, b));
                artfVulkanProbesOnCpu = false;
                const double apart = differentShare(a, b, 2);
                printf("light that bounced, gathered by the GPU against by the CPU, drawn by the GPU: %.3f%% of pixels apart\n", apart * 100);
                CHECK(apart < .001);
                artfSceneSetIndirect(s, &d);
                for (int k = 0; k < STEPS; ++k) CHECK(artfRendererDraw(r, s));
                CHECK(artfVulkanProbesRead(r, again, floats) && !memcmp(again, gpu, floats * 4));
                artfFree(a);
                artfFree(b);
                artfFree(again);
            }
            artfRendererFree(r);
            /* the CPU's, from the start, from the same frame */
            artfSceneSetIndirect(s, &d);
            ArtfFrameData frame;
            artfSceneFrame(s, W, H, &frame);
            for (int k = 0; k < STEPS; ++k) artfSceneIndirectStep(s, &frame, NULL);
            const float* cpu = s->indirect.probes;
            if (read) {
                /* the light, by how bright the brightest probe is; how far each sees, by itself */
                double most = 0, worst = 0, sum = 0, mapSum = 0;
                uint32_t apart = 0, inside = 0;
                for (uint32_t i = 0; i < count; ++i)
                    for (int k = 0; k < 3; ++k) most = fmax(most, cpu[i * 16 + k]);
                for (uint32_t i = 0; i < count; ++i) {
                    for (int k = 0; k < 16; ++k) {
                        const double e = fabs((double)gpu[i * 16 + k] - cpu[i * 16 + k]) / most;
                        worst = fmax(worst, e);
                        sum += e;
                    }
                    inside += cpu[i * 16] + cpu[i * 16 + 1] + cpu[i * 16 + 2] == 0;
                }
                const float* gpuMaps = gpu + (size_t)count * 16;
                const float* cpuMaps = cpu + (size_t)count * 16;
                for (size_t k = 0; k < (size_t)count * ARTF_PROBE_MAP; ++k) {
                    const double e = fabs((double)gpuMaps[k] - cpuMaps[k]) / fmax(fabs((double)cpuMaps[k]), 1e-3);
                    mapSum += e;
                    apart += e > .01;
                }
                const double mean = sum / (count * 16.0), mapMean = mapSum / ((double)count * ARTF_PROBE_MAP);
                printf("probes gathered by the GPU%s%s against the CPU, after %d steps: light %.1e apart on average, %.1e at most "
                       "(of the brightest); how far they see, %.1e on average, %u of %u texels past 1%%; %u probes in the dark\n",
                       way ? " without ray queries" : "", through ? ", through walls," : "", STEPS, mean, worst, mapMean, apart, count * ARTF_PROBE_MAP, inside);
                CHECK(mean < 1e-4 && worst < .01);
                CHECK(mapMean < 1e-4 && apart < count * ARTF_PROBE_MAP / 500);
                CHECK(inside > 0 && inside < count); /* (the box's: probes inside it see its backs) */
            }
            artfFree(gpu);
            artfSceneFree(s);
        }
}

/* ---- rays ---- */

static void traceWith(ArtfScene* s, ArtfTraceMode mode, int samples, int bounces) {
    ArtfTraceDesc d = {mode, 1, samples, bounces};
    artfSceneSetTrace(s, &d);
}

void testTrace(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;

    /* with nothing to reflect and no shadow to cast, rays show what the rasterizer shows, but for edges */
    ArtfScene* s = plainScene();
    artfSceneSetAmbient(s, artfVec3(.9f, .8f, .7f), artfVec3(.1f, .2f, .3f));
    ArtfMaterialDesc matte = {artfVec3(.8f, .4f, .3f), 0, 0, 1, artfVec3(.02f, 0, 0)};
    addAt(s, artfSceneAddSphere(s, 1, 32, 48), artfSceneAddMaterial(s, &matte), -1.5f, 0, 0, 1);
    addAt(s, addSquare(s, 0xFFFF8040u, 0xFF40FF80u, 0xFF8040FFu, 0xFFFFFFFFu), whiteMaterial(s), 1.5f, 0, 0, 1);
    drawInto(r, s, a);
    traceWith(s, ARTF_TRACE_RAYS, 16, 0);
    drawSamples(r, s, 16, b);
    const double far = differentShare(a, b, 24);
    printf("rays against triangles, pixels that differ by more than 24 of 255: %.2f%%\n", far * 100);
    CHECK(far < .01);
    ArtfFrameStats stats;
    artfRendererStats(r, &stats);
    CHECK(stats.traceSamples == 16);
    artfSceneFree(s);

    /* a shadow: a floor with a ball between it and the sun, on both sides of the shadow's edge */
    s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(.9f, .9f, .9f));
    setCamera(s, 0, 6, .01f, 0, 0, 0);
    ArtfTransform t = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 1), white, &t);
    addAt(s, artfSceneAddSphere(s, 1, 32, 32), white, 2.5f, 1.5f, 0, 1);
    traceWith(s, ARTF_TRACE_RAYS, 2, 0);
    drawSamples(r, s, 2, a);
    const double lit = meanOf(a, W / 2 - 60, H / 2, 8);
    CHECK(lit > 200);
    artfSceneSetSun(s, artfVec3(-.7f, -1, 0), artfVec3(.9f, .9f, .9f));
    drawSamples(r, s, 2, a);
    int dark = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) dark += CHANNEL(a[i], 8) < 110 && CHANNEL(a[i], 8) > 60;
    CHECK(dark > 300);
    artfSceneFree(s);

    /* a mirror shows what it faces, and a surface that does not shine does not */
    s = plainScene();
    white = whiteMaterial(s);
    ArtfMaterialDesc mirror = {.baseColor = {1, 1, 1}, .specular = 1.f, .roughness = .05f, .metallic = 1.f};
    ArtfMaterialDesc lamp = {artfVec3(0, 0, 0), 0, 0, 1, artfVec3(1, 0, 0)};
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &mirror), 0, 0, 0, 2);
    const ArtfTransform behind = artfTransformTrs(artfVec3(0, 0, 8), artfQuatAxisAngle(artfVec3(0, 1, 0), 3.14159265f), artfVec3(8, 8, 8));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &lamp), &behind);
    traceWith(s, ARTF_TRACE_RAYS, 8, 0);
    drawSamples(r, s, 8, a);
    CHECK(meanOf(a, W / 2, H / 2, 16) > 200);
    artfSceneFree(s);

    /* what is half there lets half through, and a hole all of it; no background gives alpha */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetBackgroundAlpha(s, 0);
    ArtfInstance half = addAt(s, addSquare(s, 0, 0, 0, 0), white, 0, 0, 0, 1);
    artfInstanceSetOpacity(s, half, .5f);
    traceWith(s, ARTF_TRACE_RAYS, 128, 0);
    drawSamples(r, s, 128, a);
    CHECK(a[0] == 0);
    CHECK(near_((int)meanOf(a, W / 2, H / 2, 24), 128, 14));
    artfSceneFree(s);

    /* paths: light from a red wall reaches a white floor that rays alone leave white */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(1, 1, 1));
    artfSceneSetAmbient(s, artfVec3(.05f, .05f, .05f), artfVec3(.05f, .05f, .05f));
    setCamera(s, 0, 3, 7, 0, 0, 0);
    ArtfMaterialDesc redWall = {artfVec3(1, .02f, .02f), 0, 0, 1, artfVec3(0, 0, 0)};
    t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 40, 1), white, &t);
    const ArtfTransform wall = artfTransformTrs(artfVec3(2.2f, 1, 0), artfQuatAxisAngle(artfVec3(0, 1, 0), -1.5708f), artfVec3(4, 2, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &redWall), &wall);
    traceWith(s, ARTF_TRACE_RAYS, 4, 0);
    drawSamples(r, s, 4, a);
    traceWith(s, ARTF_TRACE_PATHS, 128, 0);
    drawSamples(r, s, 128, b);
    const int fx = W / 2 + 28, fy = H / 2 + 34;
    const double rayTint = meanOf(a, fx, fy, 16) - meanOf(a, fx, fy, 8), pathTint = meanOf(b, fx, fy, 16) - meanOf(b, fx, fy, 8);
    printf("a white floor by a red wall, red over green: %.1f with rays, %.1f with paths\n", rayTint, pathTint);
    CHECK(pathTint > rayTint + 8);
    /* a still scene gathers, and a change starts over */
    artfRendererStats(r, &stats);
    CHECK(stats.traceSamples == 128);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.traceSamples == 128);
    setCamera(s, 0, 3, 6.9f, 0, 0, 0);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.traceSamples == 1);
    artfSceneSetTrace(s, NULL);
    drawInto(r, s, a);
    artfSceneFree(s);
    (void)mid;

    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* Rays do not depend on threads or kernels; the trees hold what they hold. */
void testTraceData(void) {
    ArtfScene* s = layeredScene();
    artfSceneTraceSync(s);
    const ArtfTrace* t = &s->trace;
    CHECK(t->instances.len == s->instances.len);
    /* every node's box holds its children's, and all leaves together hold each triangle once */
    uint8_t* seen = artfCalloc(t->triangles.len, 1);
    for (size_t i = 0; i < t->nodes.len; ++i) {
        const ArtfTraceNode* n = &t->nodes.items[i];
        if (n->count) {
            CHECK(n->count <= 4);
            for (uint32_t k = 0; k < n->count; ++k) ++seen[n->first + k];
        } else {
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 3; ++k) {
                    if (t->nodes.items[n->first + (uint32_t)c].lo[k] < n->lo[k] || t->nodes.items[n->first + (uint32_t)c].hi[k] > n->hi[k]) { CHECK(!"a child outside its box"); }
                }
        }
    }
    bool once = true;
    for (size_t i = 0; i < t->triangles.len; ++i) once = once && seen[i] == 1;
    CHECK(once);
    artfFree(seen);

    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    traceWith(s, ARTF_TRACE_PATHS, 3, 0);
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {3, ARTF_SIMD_128}};
    uint64_t reference = 0;
    for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
        o.simd = runs[i].simd;
        ArtfRenderer* r = artfRendererNew(&o);
        drawSamples(r, s, 3, frame);
        const uint64_t h = hashOf(frame, (size_t)W * H);
        if (!i) reference = h;
        CHECK(h == reference);
        artfRendererFree(r);
    }
    artfFree(frame);
    artfSceneFree(s);
}

/* The GPU's shader against this C: the same picture, once the noise is gone. */
void testTraceBothBackends(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    for (int mode = ARTF_TRACE_RAYS; mode <= ARTF_TRACE_PATHS; ++mode)
        for (int clear = 0; clear < 2; ++clear) {
            ArtfScene* s = layeredScene();
            if (clear) artfSceneSetBackgroundAlpha(s, 0);
            traceWith(s, (ArtfTraceMode)mode, 96, 0);
            drawSamples(gpu, s, 96, a);
            drawSamples(cpu, s, 96, b);
            const double far = differentShare(a, b, 24);
            printf("%s, %s: pixels that differ by more than 24 of 255: %.2f%%\n", mode == ARTF_TRACE_RAYS ? "rays" : "paths", clear ? "no background" : "a background", far * 100);
            CHECK(far < .02);
            artfSceneFree(s);
        }
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}
