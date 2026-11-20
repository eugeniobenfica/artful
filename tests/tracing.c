/* artful-tests: rays: tracing, the ray tracing hardware, rays while drawing, denoising, probes. */
#include "tests.h"
#include "scene/sampling.h"

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
    /* (four samples were 2.2 away by a hash's numbers, and are 1.4 by Sobol's: the filter takes them to about what 512
     * samples are from the truth themselves) */
    CHECK(after < before * .8 && after < 1.2);

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
    CHECK(moved < before * .8 && moved < 1.2);
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
        FLIP_ALIKE(0.0115); /* (measured 0.0015 on a Radeon RX 9060 XT) */
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
    if (!hardware) missing("rt");
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
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
            FLIP_ALIKE(0.0133); /* (measured 0.0033 on a Radeon RX 9060 XT) */
            CHECK(apart < .01);
        }
        artfFree(frames[k][0]);
        artfFree(frames[k][1]);
    }
    printf(hardware ? "(shadows from rays went through the ray tracing hardware)\n" : "(no ray tracing hardware here)\n");
    if (!hardware) missing("rt");
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
        FLIP_ALIKE(0.01); /* (measured 0.0000 on a Radeon RX 9060 XT) */
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
#ifdef ARTFUL_VULKAN /* (its counters and switches are Vulkan's: without it, nothing to run) */
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
        FLIP_ALIKE(0.0106); /* (measured 0.0006 on a Radeon RX 9060 XT) */
        CHECK(worst < .005);
        CHECK(worstFresh == 0);
        /* (with the hardware: the floor, the cube, the strip and the strip's own copy the instance bends, built once, and
         * the copy refitted each frame after) */
        const bool hardware = built > 0;
        if (!hardware) missing("rt");
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
            FLIP_ALIKE(0.0105); /* (measured 0.0005 on a Radeon RX 9060 XT) */
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
#else
    missing("vulkan");
#endif
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

/* Light that bounced goes through walls little: inside a closed box of walls 10 cm thick under a bright sky, with probes
 * 1.7 m apart, a point 20 cm from a wall takes an eighth of what is just outside (0.126, before probes inside something
 * counted for nothing as after: a probe's map of 8 x 8 directions sees past the box's edges, near them). */
void testProbesWalls(void) {
    ArtfScene* s = artfSceneNew();
    setCamera(s, 0, 0, 10, 0, 0, 0);
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(1, 1, 1), artfVec3(1, 1, 1));
    /* (a hollow box of grey walls 10 cm thick: inside a solid cube is no room, and its probes count for nothing; and a
     * closed room of white walls keeps all it is given, however little leaks in) */
    {
        const ArtfMesh cube = artfSceneAddCube(s, 1);
        const ArtfMaterialDesc greyDesc = {artfVec3(.6f, .6f, .6f), 0, 0, 1, artfVec3(0, 0, 0)};
        const ArtfMaterial white = artfSceneAddMaterial(s, &greyDesc);
        for (int axis = 0; axis < 3; ++axis)
            for (int side = -1; side <= 1; side += 2) {
                float at[3] = {0, 0, 0}, size[3] = {4.2f, 4.2f, 4.2f};
                at[axis] = 2.05f * (float)side;
                size[axis] = .1f;
                const ArtfTransform t = artfTransformTrs(artfVec3(at[0], at[1], at[2]), artfQuatIdentity(), artfVec3(size[0], size[1], size[2]));
                artfSceneAddInstance(s, cube, white, &t);
            }
    }
    const ArtfIndirectDesc probes = {{8, 8, 8}, 128, .5f, 1, {-6, -6, -6}, {6, 6, 6}};
    artfSceneSetIndirect(s, &probes);
    ArtfFrameData frame;
    artfSceneFrame(s, W, H, &frame);
    for (int k = 0; k < 40; ++k) artfSceneIndirectStep(s, &frame, NULL);
    const float inside[3] = {1.8f, 0, 0}, inward[3] = {-1, 0, 0}, outside[3] = {2.3f, 0, 0}, outward[3] = {1, 0, 0};
    float in[3], out[3];
    artfIndirectAt(&s->indirect, inside, inward, in);
    artfIndirectAt(&s->indirect, outside, outward, out);
    printf("light that bounced, inside a box by its wall: %.3f of what is just outside\n", in[1] / out[1]);
    CHECK(out[1] > .5f && in[1] < out[1] * .15f);
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

/* Probes where it is hard for them, at the same rays a step (the CPU's, which are the reference):
 * - two closed rooms side by side, a wall 6 cm thick between them, a lamp in one: what the floor of the other takes by
 *   the wall, against what the lit one's takes by it (traced, nothing reaches it: the rooms are closed);
 * - a lamp turned on in a room, and a door opened onto a bright sky: how many steps until the light that bounced is
 *   nine tenths of where it settles. */
static ArtfInstance slab(ArtfScene* s, ArtfMesh cube, ArtfMaterial m, float x, float y, float z, float sx, float sy, float sz) {
    const ArtfTransform t = artfTransformTrs(artfVec3(x, y, z), artfQuatIdentity(), artfVec3(sx, sy, sz));
    return artfSceneAddInstance(s, cube, m, &t);
}
/* A room 6 x 3 x 4 of slabs 10 cm thick; with a wall down its middle (divided), or a door 1 x 2 in its +x wall,
 * closed by *door. */
static ArtfScene* roomScene(bool divided, ArtfInstance* door) {
    ArtfScene* s = artfSceneNew();
    setCamera(s, 0, 1.5f, 1.8f, 0, 1, 0);
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(divided ? 0.f : 1.f, divided ? 0.f : 1.f, divided ? 0.f : 1.f), artfVec3(0, 0, 0));
    const ArtfMesh cube = artfSceneAddCube(s, 1);
    /* (grey: a closed room of white walls keeps all the light it is given, and never settles) */
    const ArtfMaterialDesc greyDesc = {artfVec3(.6f, .6f, .6f), 0, 0, 1, artfVec3(0, 0, 0)};
    const ArtfMaterial white = artfSceneAddMaterial(s, &greyDesc);
    slab(s, cube, white, 0, -.05f, 0, 6.2f, .1f, 4.2f);
    slab(s, cube, white, 0, 3.05f, 0, 6.2f, .1f, 4.2f);
    slab(s, cube, white, -3.05f, 1.5f, 0, .1f, 3, 4.2f);
    slab(s, cube, white, 0, 1.5f, -2.05f, 6.2f, 3, .1f);
    slab(s, cube, white, 0, 1.5f, 2.05f, 6.2f, 3, .1f);
    if (door) {
        /* (the +x wall around a door from z -.5 to .5, y 0 to 2) */
        slab(s, cube, white, 3.05f, 1.5f, -1.275f, .1f, 3, 1.55f);
        slab(s, cube, white, 3.05f, 1.5f, 1.275f, .1f, 3, 1.55f);
        slab(s, cube, white, 3.05f, 2.5f, 0, .1f, 1, 1);
        *door = slab(s, cube, white, 3.05f, 1, 0, .1f, 2, 1);
    } else {
        slab(s, cube, white, 3.05f, 1.5f, 0, .1f, 3, 4.2f);
    }
    if (divided) slab(s, cube, white, 0, 1.5f, 0, .06f, 3, 4);
    /* probes every .75 m: a row of them in the middle wall's plane */
    const ArtfIndirectDesc probes = {{9, 5, 6}, 64, 0, 1, {-3, 0, -2}, {3, 3, 2}};
    artfSceneSetIndirect(s, &probes);
    return s;
}
/* What the floor takes at some places, the mean of green. */
static float floorLight(const ArtfScene* s, float x0, float x1) {
    float sum = 0;
    int n = 0;
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 5; ++k, ++n) {
            const float p[3] = {x0 + (x1 - x0) * (float)i / 3.f, .001f, -1.5f + .75f * (float)k}, up[3] = {0, 1, 0};
            float got[3];
            artfIndirectAt(&s->indirect, p, up, got);
            sum += got[1];
        }
    return sum / (float)n;
}
static void stepProbes(ArtfScene* s, int steps) {
    ArtfFrameData frame;
    artfSceneFrame(s, W, H, &frame);
    for (int k = 0; k < steps; ++k) artfSceneIndirectStep(s, &frame, NULL);
}
/* After a change: the steps until the light is nine tenths of the way to where it settles (after 240). */
static int settling(ArtfScene* s, float before) {
    float seen[240];
    ArtfFrameData frame;
    artfSceneFrame(s, W, H, &frame);
    for (int k = 0; k < 240; ++k) {
        artfSceneIndirectStep(s, &frame, NULL);
        seen[k] = floorLight(s, -2.5f, 2.5f);
    }
    const float settled = seen[239], aim = before + (settled - before) * .9f;
    for (int k = 0; k < 240; ++k)
        if (settled > before ? seen[k] >= aim : seen[k] <= aim) return k + 1;
    return 240;
}

void testProbesHard(void) {
    /* the wall */
    ArtfScene* s = roomScene(true, NULL);
    artfSceneAddLight(s, artfVec3(-1.5f, 2, 0), artfVec3(4, 4, 4), 8);
    stepProbes(s, 120);
    const float lit = floorLight(s, -.6f, -.1f), dark = floorLight(s, .1f, .6f);
    artfSceneFree(s);
    /* the lamp */
    s = roomScene(false, NULL);
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    stepProbes(s, 20);
    const float off = floorLight(s, -2.5f, 2.5f);
    artfSceneAddLight(s, artfVec3(0, 2.5f, 0), artfVec3(3, 3, 3), 8);
    const int lamp = settling(s, off);
    artfSceneFree(s);
    /* the door */
    ArtfInstance door;
    s = roomScene(false, &door);
    stepProbes(s, 60);
    const float shut = floorLight(s, -2.5f, 2.5f);
    artfSceneRemoveInstance(s, door);
    const int opened = settling(s, shut);
    artfSceneFree(s);
    printf("probes: by a wall 6 cm thick, the dark room's floor takes %.5f of the lit one's (traced: 0); a lamp turned on settles in %d steps, a door opened in %d\n", dark / lit, lamp,
           opened);
    /* (before probes inside something counted for nothing and kept less of what they had when the light changed:
     * 0.0004, 47 and 30 steps; the plan's mark, no more than twice as many steps) */
    CHECK(dark / lit < 1e-4f && lamp <= 47 && opened <= 30);
}

/* Probes the scene keeps: the GPU's go back to the scene now and then, so that a renderer that comes after (a device
 * lost, a window on another GPU) goes on from them. Its first frame is near the last of the one before (FLIP), where
 * from nothing it had no light that bounced. */
void testProbesKept(void) {
#ifdef ARTFUL_VULKAN
    ArtfScene* s = bleedScene();
    artfSceneAddLight(s, artfVec3(.5f, 1.5f, 1), artfVec3(2, 1.6f, 1.2f), 6);
    /* (few rays, kept long: one step from nothing is far from what many make) */
    const ArtfIndirectDesc d = {{8, 4, 8}, 16, .95f, 1, {-4, -1.5f, -3}, {4, 3, 3}, false};
    artfSceneSetIndirect(s, &d);
    const ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) {
        artfSceneFree(s);
        return;
    }
    uint32_t* before = artfMalloc(W * H * 4);
    uint32_t* first = artfMalloc(W * H * 4);
    /* the scene keeps them every 30 steps: after 120 frames, those of the 90th; the frame after it is what a renderer
     * that goes on from them draws first */
    for (int k = 0; k < 91; ++k) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, before));
    for (int k = 91; k < 120; ++k) CHECK(artfRendererDraw(r, s));
    CHECK(s->indirect.frames == 90);
    artfRendererFree(r);
    r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s) && artfRendererReadPixels(r, first));
    const FlipStats kept = flipFrames(before, first, W, H, 0, NULL);
    artfRendererFree(r);
    /* from nothing */
    artfSceneSetIndirect(s, &d);
    r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s) && artfRendererReadPixels(r, first));
    const FlipStats none = flipFrames(before, first, W, H, 0, NULL);
    artfRendererFree(r);
    printf("a new renderer's first frame against the frame it goes on from: FLIP %.4f from what the scene kept, %.4f from nothing\n", kept.mean, none.mean);
    CHECK(kept.mean < .005 && none.mean > .03);
    artfFree(before);
    artfFree(first);
    artfSceneFree(s);
#endif
}

/* Light that bounced, gathered by the GPU (artfShaderProbes) against the CPU's step, which is the reference: the same
 * rays the same ways, so the same probes but for roundings and a ray that grazes an edge. A floor, a red wall, a closed
 * box with probes inside it, a lamp and the sun; with walls and through them, through the hardware where there is
 * some and through the trees. */
void testProbesGpu(void) {
#ifdef ARTFUL_VULKAN /* (its counters and switches are Vulkan's: without it, nothing to run) */
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
#else
    missing("vulkan");
#endif
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

/* Light from all around, aimed at (artfEnvAim) as well as bounced into, each weighed by the other (MIS): nothing is
 * counted twice or left out. A ball that only gives light back diffusely, under light the same from everywhere, shows
 * its albedo times that light wherever it is seen, as nothing of it hides another part (the furnace). The tables aim
 * where the light is: under a small bright sun, nearly every aim goes to it. */
void testEnvironmentAimed(ArtfBackend backend) {
    enum { PW = 64, PH = 32 };
    static float even[PW * PH * 3], sunny[PW * PH * 3];
    for (int i = 0; i < PW * PH * 3; ++i) even[i] = .8f;
    for (int i = 0; i < PW * PH * 3; ++i) sunny[i] = .05f;
    for (int c = 0; c < 3; ++c) sunny[(8 * PW + 20) * 3 + c] = 4000.f;
    ArtfScene* s = plainScene(); /* (no sun) */
    CHECK(artfSceneSetEnvironment(s, sunny, PW, PH, 1.f, false));
    /* (the tables: aimed, the sun's texels; and how likely each aim was, as aiming says) */
    int atSun = 0;
    bool agrees = true;
    for (int i = 0; i < 256; ++i) {
        float u[4], d[3];
        artfSobol4((uint32_t)i, 12345u, u);
        const float likely = artfEnvAim(s->environment, u[0], u[1], d);
        float around[3];
        artfEnvSample(s->environment, 0, d[0], d[1], d[2], around);
        atSun += around[0] > 100.f;
        const float again = artfEnvAimDensity(s->environment, d);
        agrees = agrees && likely > 0 && fabsf(again - likely) <= likely * 1e-3f;
    }
    printf("aimed at a small sun: %d of 256 aims there\n", atSun);
    CHECK(atSun > 230 && agrees);
    /* light the same from everywhere: every direction as likely as any other, 1 / (4 pi), which is what makes sure the
     * share of all around each texel covers is right */
    CHECK(artfSceneSetEnvironment(s, even, PW, PH, 1.f, false));
    double least = 1e9, most = 0;
    for (int i = 0; i < 4096; ++i) {
        float u[4], d[3];
        artfSobol4((uint32_t)i, 777u, u);
        const float z = 1.f - 2.f * u[0], across = sqrtf(1.f - z * z), turn = 6.28318531f * u[1];
        d[0] = across * cosf(turn);
        d[1] = z;
        d[2] = across * sinf(turn);
        const double times = artfEnvAimDensity(s->environment, d) * 4.0 * 3.14159265;
        least = fmin(least, times);
        most = fmax(most, times);
    }
    printf("aiming under even light: from %.3f to %.3f of 1 / (4 pi)\n", least, most);
    CHECK(least > .9 && most < 1.1);
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.5f, .5f, .5f);
    d.specular = 0;
    d.roughness = 1;
    addAt(s, artfSceneAddSphere(s, 1, 48, 64), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
    setCamera(s, 0, 0, 4, 0, 0, 0);
    const ArtfTraceDesc paths = {ARTF_TRACE_PATHS, 4, 256, 0, false};
    artfSceneSetTrace(s, &paths);
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) {
        artfSceneFree(s);
        return;
    }
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    drawSamples(r, s, 64, frame);
    const int want = shownByte(.5 * .8);
    const double middle = meanOf(frame, W / 2, H / 2, 8), side = meanOf(frame, W / 2 + 30, H / 2, 8);
    printf("%s, the furnace: %.1f and %.1f where %d is the truth\n", backend == ARTF_BACKEND_VULKAN ? "GPU" : "CPU", middle, side, want);
    CHECK(fabs(middle - want) < 2.5 && fabs(side - want) < 2.5);
    artfFree(frame);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* The white furnace, for highlights: a ball of white metal under light the same from everywhere gives back all of it,
 * however rough, as what its lobe leaves out (light that meets a second facet) is put back (shading.inc's Gain). Without
 * it, a roughness of 1 seen straight on gives back a third. Light at the middle, where the lobe loses most, and toward
 * the side; to 2% of the truth. */
static double lightOf(const uint32_t* frame, int x, int y) {
    double sum = 0;
    for (int j = -3; j <= 3; ++j)
        for (int i = -3; i <= 3; ++i)
            for (int c = 0; c < 24; c += 8) sum += artfSrgbDecode((float)(frame[(size_t)(y + j) * W + x + i] >> c & 255) / 255.f);
    return sum / (49 * 3);
}

void testMetalFurnace(ArtfBackend backend) {
    enum { PW = 64, PH = 32 };
    static float even[PW * PH * 3];
    for (int i = 0; i < PW * PH * 3; ++i) even[i] = .8f;
    const float roughness[2] = {1.f, .5f};
    for (int k = 0; k < 2; ++k) {
        ArtfScene* s = plainScene();
        CHECK(artfSceneSetEnvironment(s, even, PW, PH, 1.f, false));
        ArtfMaterialDesc d = artfMaterialDefaults();
        d.baseColor = artfVec3(1, 1, 1);
        d.metallic = 1;
        d.roughness = roughness[k];
        addAt(s, artfSceneAddSphere(s, 1, 48, 64), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
        setCamera(s, 0, 0, 4, 0, 0, 0);
        const ArtfTraceDesc paths = {ARTF_TRACE_PATHS, 4, 256, 0, false};
        artfSceneSetTrace(s, &paths);
        ArtfRendererOptions o = options(backend, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) {
            artfSceneFree(s);
            return;
        }
        uint32_t* frame = artfMalloc((size_t)W * H * 4);
        drawSamples(r, s, 64, frame);
        const double middle = lightOf(frame, W / 2, H / 2) / .8, side = lightOf(frame, W / 2 + 30, H / 2) / .8;
        printf("%s, white metal of roughness %.1f under even light: %.3f and %.3f of it back\n", backend == ARTF_BACKEND_VULKAN ? "GPU" : "CPU", roughness[k], middle, side);
        CHECK(fabs(middle - 1) < .02 && fabs(side - 1) < .02);
        artfFree(frame);
        artfRendererFree(r);
        artfSceneFree(s);
    }
}

/* Aiming and bouncing, weighed lobe by lobe, add up to what bouncing alone finds: the same light, with less noise. A
 * shiny ball and a rough one on a floor under a sky with a sun, by the CPU's paths with aiming and without. */
void testAimUnbiased(void) {
    enum { PW = 64, PH = 32 };
    static float sky[PW * PH * 3];
    for (int y = 0; y < PH; ++y)
        for (int x = 0; x < PW; ++x)
            for (int c = 0; c < 3; ++c) sky[(y * PW + x) * 3 + c] = y < PH / 2 ? .3f + .2f * (float)c : .05f;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            for (int c = 0; c < 3; ++c) sky[((6 + dy) * PW + 20 + dx) * 3 + c] = 40.f;
    ArtfScene* s = plainScene();
    CHECK(artfSceneSetEnvironment(s, sky, PW, PH, 1.f, false));
    ArtfMaterialDesc shiny = artfMaterialDefaults(), rough = artfMaterialDefaults(), floor_ = artfMaterialDefaults();
    shiny.baseColor = artfVec3(.9f, .7f, .4f);
    shiny.metallic = 1;
    shiny.roughness = .35f;
    rough.baseColor = artfVec3(.3f, .5f, .7f);
    rough.roughness = .6f;
    floor_.baseColor = artfVec3(.6f, .6f, .6f);
    const ArtfMesh ball = artfSceneAddSphere(s, .8f, 32, 48);
    addAt(s, ball, artfSceneAddMaterial(s, &shiny), -1, 0, 0, 1);
    addAt(s, ball, artfSceneAddMaterial(s, &rough), 1, 0, 0, 1);
    const ArtfTransform t = artfTransformAt(0, -.8f, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 10, 1), artfSceneAddMaterial(s, &floor_), &t);
    setCamera(s, 0, 1.2f, 4.5f, 0, 0, 0);
    const ArtfTraceDesc paths = {ARTF_TRACE_PATHS, 1, 768, 3, false};
    artfSceneSetTrace(s, &paths);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    o.width = W / 4;
    o.height = H / 4;
    double means[2], noise[2];
    for (int aim = 0; aim < 2; ++aim) {
        artfTraceAim = aim != 0;
        ArtfRenderer* r = artfRendererNew(&o);
        uint32_t* early = artfMalloc((size_t)o.width * o.height * 4);
        uint32_t* frame = artfMalloc((size_t)o.width * o.height * 4);
        for (int i = 0; i < 768; ++i) {
            CHECK(artfRendererDraw(r, s));
            if (i == 15) CHECK(artfRendererReadPixels(r, early));
        }
        CHECK(artfRendererReadPixels(r, frame));
        double sum = 0, apart = 0;
        for (int i = 0; i < o.width * o.height; ++i)
            for (int c = 0; c < 24; c += 8) {
                const double light = artfSrgbDecode((float)(frame[i] >> c & 255) / 255.f), first = artfSrgbDecode((float)(early[i] >> c & 255) / 255.f);
                sum += light;
                apart += (first - light) * (first - light);
            }
        means[aim] = sum / (o.width * o.height * 3);
        noise[aim] = sqrt(apart / (o.width * o.height * 3));
        artfFree(early);
        artfFree(frame);
        artfRendererFree(r);
    }
    artfTraceAim = true;
    printf("bouncing alone %.4f, aiming too %.4f (%.2f%% apart); at sixteen samples %.4f and %.4f from their own end\n", means[0], means[1], (means[1] / means[0] - 1) * 100, noise[0], noise[1]);
    CHECK(fabs(means[1] / means[0] - 1) < .01);
    CHECK(noise[1] < noise[0]);
    artfSceneFree(s);
}

/* Ray cones: a texture read as wide as the ray's cone where it lands, as the rasterizer reads its mipmaps. A floor of
 * fine stripes far off is grey where the cone covers many: one sample a pixel is then near what 256 make, where without
 * cones each sample hits a stripe or the gap between and the pixel is noise. And its light is the stripes' own, half of
 * white, as their mipmaps are means of light (artfSceneTextureRoles: only a color uses them); without cones the
 * finest level, read between texels as bytes, is darker. The GPU, which asks the texture its size, reads the same. */
void testRayCones(void) {
    static uint32_t stripes[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) stripes[y * 64 + x] = (x & 1) ? 0xFFFFFFFFu : 0xFF000000u;
    ArtfScene* s = plainScene();
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.texture = artfSceneAddTexture(s, stripes, 64, 64, 0);
    d.specular = 0;
    const ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 60, 30), artfSceneAddMaterial(s, &d), &t);
    setCamera(s, 0, .5f, 8, 0, -1, -20);
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 256, 0, false};
    artfSceneSetTrace(s, &rays);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    double noise[2], mean[2];
    uint32_t* first = artfMalloc((size_t)W * H * 4);
    uint32_t* settled = artfMalloc((size_t)W * H * 4);
    for (int cones = 0; cones < 2; ++cones) {
        artfTraceCones = cones != 0;
        ArtfRenderer* r = artfRendererNew(&o);
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, first));
        drawSamples(r, s, 255, settled);
        double apart = 0, sum = 0;
        int counted = 0;
        for (int i = W * H / 2; i < W * H; ++i) {
            const double a = artfSrgbDecode((float)CHANNEL(first[i], 8) / 255.f), b = artfSrgbDecode((float)CHANNEL(settled[i], 8) / 255.f);
            apart += (a - b) * (a - b);
            sum += b;
            ++counted;
        }
        noise[cones] = sqrt(apart / counted);
        mean[cones] = sum / counted;
        artfRendererFree(r);
    }
    artfTraceCones = true;
    printf("ray cones: one sample is %.4f from 256 with them, %.4f without; the floor's light %.4f and %.4f\n", noise[1], noise[0], mean[1], mean[0]);
    CHECK(noise[1] < noise[0] * .5);
    CHECK(fabs(mean[1] - .5) < .05 && fabs(mean[1] - .5) < fabs(mean[0] - .5));
    o.backend = ARTF_BACKEND_VULKAN;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawSamples(gpu, s, 64, first);
        o.backend = ARTF_BACKEND_SOFTWARE;
        ArtfRenderer* cpu = artfRendererNew(&o);
        drawSamples(cpu, s, 64, settled);
        const double far = differentShare(first, settled, 24);
        printf("ray cones, the GPU against the CPU: %.2f%% of pixels apart\n", far * 100);
        FLIP_ALIKE(0.0106); /* (measured 0.0006 on a Radeon RX 9060 XT) */
        CHECK(far < .01);
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(first);
    artfFree(settled);
    artfSceneFree(s);
}

/* A few samples a pixel against many: how far, by FLIP (which sees how the error is spread: blue noise reads as less
 * than white of the same size) and by the mean of what differs, on both backends; paths, the light that bounced. */
static void tracedAt(ArtfRenderer* r, int samples, int frames, uint32_t* out) {
    ArtfScene* s = bleedScene();
    const ArtfTraceDesc paths = {ARTF_TRACE_PATHS, samples, 0, 4, false, false};
    artfSceneSetTrace(s, &paths);
    for (int k = 0; k < frames; ++k) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, out));
    artfSceneFree(s);
}
void testFewSamples(void) {
    uint32_t* truth = artfMalloc(W * H * 4);
    uint32_t* few = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        tracedAt(r, 64, 16, truth);
        double flip[2], mean[2];
        for (int k = 0; k < 2; ++k) {
            tracedAt(r, k ? 4 : 1, 1, few);
            const FlipStats f = flipFrames(truth, few, W, H, 0, NULL);
            flip[k] = f.mean;
            mean[k] = meanApart(truth, few);
        }
        printf("%s, paths of 1 and 4 samples against 1,024: FLIP %.4f and %.4f, %.2f and %.2f of 255 apart\n", backend ? "GPU" : "CPU", flip[0], flip[1], mean[0], mean[1]);
        /* (each pixel with a sequence of its own, white noise: 0.0839 and 0.0454 on the CPU, 0.0858 and 0.0473 on the GPU) */
        CHECK(flip[0] < (backend ? .0858 : .0839) * .9 && flip[1] < (backend ? .0473 : .0454) * 1.01);
        artfRendererFree(r);
    }
    artfFree(truth);
    artfFree(few);
}

/* Lights with a size, both ways (MIS): a shiny floor under a lamp with a size and a sun with one. Aimed at alone, a
 * sharp highlight of a large light is found by few paths, and those bright: at 16 samples a frame is far from where it
 * settles. Weighed against the bounce's own way, the same frame is nearer, and where both settle is the same light. */
static ArtfScene* lampFloorScene(float roughness) {
    ArtfScene* s = plainScene();
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    artfSceneSetSun(s, artfVec3(-.4f, -1, -.3f), artfVec3(1.2f, 1.1f, 1));
    artfSceneSetSunSize(s, .15f);
    setCamera(s, 0, 1.6f, 3.5f, 0, -.6f, 0);
    ArtfMaterialDesc floor = artfMaterialDefaults();
    floor.baseColor = artfVec3(.5f, .5f, .5f);
    floor.roughness = roughness;
    floor.metallic = 1;
    const ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 20, 1), artfSceneAddMaterial(s, &floor), &t);
    const ArtfLight lamp = artfSceneAddLight(s, artfVec3(.3f, -.3f, -1.2f), artfVec3(2, 1.8f, 1.6f), 7);
    artfLightSetSize(s, lamp, .6f);
    return s;
}
static void lampFloorAt(ArtfRenderer* r, int ways, float roughness, int samples, int frames, uint32_t* out, double* light) {
    artfTraceLightWays = ways;
    ArtfScene* s = lampFloorScene(roughness);
    const ArtfTraceDesc paths = {ARTF_TRACE_PATHS, samples, 0, 3, false, false};
    artfSceneSetTrace(s, &paths);
    for (int k = 0; k < frames; ++k) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, out));
    double sum = 0;
    for (int i = 0; i < W * H; ++i) sum += CHANNEL(out[i], 8);
    *light = sum / (W * H);
    artfSceneFree(s);
    artfTraceLightWays = 0;
}
void testLightMis(void) {
    uint32_t* truth = artfMalloc(W * H * 4);
    uint32_t* few = artfMalloc(W * H * 4);
    double cpuSettled = 0;
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        double settled, light;
        lampFloorAt(r, 0, .1f, 64, 32, truth, &settled);
        lampFloorAt(r, 0, .1f, 16, 1, few, &light);
        const double apart = meanApart(truth, few);
        if (!backend) {
            /* aimed at alone: 16 samples, from where both ways settle */
            cpuSettled = settled;
            lampFloorAt(r, 1, .1f, 16, 1, few, &light);
            const double aimedApart = meanApart(truth, few);
            /* where each settles, on a floor whose highlight aiming alone settles on in 2,048 samples (on the shinier one
             * it has not: what is left of its noise, under the tone curve, makes it darker) */
            double aimedOnly, both;
            lampFloorAt(r, 1, .3f, 64, 32, few, &aimedOnly);
            lampFloorAt(r, 0, .3f, 64, 32, few, &both);
            printf("CPU, a lamp and a sun with a size on a shiny floor: 16 samples %.2f of 255 from 2,048 aimed alone, %.2f both ways; on a rougher one, settled %.2f and %.2f\n",
                   aimedApart, apart, aimedOnly, both);
            CHECK(apart < aimedApart * .5 && fabs(both - aimedOnly) < aimedOnly * .01);
        } else {
            printf("GPU, the same both ways: 16 samples %.2f of 255 from 2,048; settled %.2f, the CPU's %.2f\n", apart, settled, cpuSettled);
            CHECK(fabs(settled - cpuSettled) < cpuSettled * .02);
        }
        artfRendererFree(r);
    }
    artfFree(truth);
    artfFree(few);
}

/* Probes that follow the camera (ArtfIndirectDesc.follow): the volume is around the camera, light bounces from the red
 * wall onto the floor as with a box; a step of the camera across a cell starts one slab of probes over and keeps the
 * rest, as their stamps say; and the GPU's probes, after steps away and back, are the CPU's. */
void testProbesFollow(void) {
    const ArtfIndirectDesc d = {{8, 4, 8}, 64, .5f, 1, {0, 0, 0}, {16, 6, 16}, false, true};
    /* (the CPU's: the slab a step starts over) */
    {
        ArtfScene* s = bleedScene();
        artfSceneSetIndirect(s, &d);
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        for (int k = 0; k < 4; ++k) CHECK(artfRendererDraw(r, s));
        const ArtfIndirect* g = &s->indirect;
        const int was = g->base[0];
        setCamera(s, g->spacing[0], 3, 7, g->spacing[0], 0, 0);
        artfIndirectFollow(s);
        int fresh = 0;
        for (uint32_t i = 0; i < artfIndirectCount(g); ++i) {
            int cell[3];
            float origin[3], spacing[3];
            fresh += g->probes[(size_t)i * 16 + 7] != artfIndirectStampAt(g, i, cell, origin, spacing);
        }
        printf("probes that follow: a step of a cell moves the volume %d cell, and starts %d probes over, a slab of %d\n", g->base[0] - was, fresh, g->dims[1] * g->dims[2]);
        CHECK(g->base[0] == was + 1 && fresh == g->dims[1] * g->dims[2]);
        artfRendererFree(r);
        artfSceneFree(s);
    }
    uint32_t* frames[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    float* probes[2] = {NULL, NULL};
    double bled[2] = {0, 0};
    bool drawn[2] = {false, false};
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) {
            missing("vulkan");
            continue;
        }
        ArtfScene* s = bleedScene();
        artfSceneSetIndirect(s, &d);
        /* (along x and back, a cell and a half each way, with steps at each place) */
        const float stops[5] = {0, 1.5f, 3.f, 1.5f, 0};
        for (int k = 0; k < 5; ++k) {
            const float x = stops[k] * s->indirect.spacing[0];
            setCamera(s, x, 3, 7, x, 0, 0);
            for (int f = 0; f < 4; ++f) CHECK(artfRendererDraw(r, s));
        }
        CHECK(artfRendererReadPixels(r, frames[backend]));
        const int fx = W / 2 + 28, fy = H / 2 + 34;
        bled[backend] = meanOf(frames[backend], fx, fy, 16) - meanOf(frames[backend], fx, fy, 8);
        const size_t floats = (size_t)artfIndirectCount(&s->indirect) * ARTF_PROBE_FLOATS;
        probes[backend] = artfMalloc(floats * 4);
#ifdef ARTFUL_VULKAN
        if (backend) CHECK(artfVulkanProbesRead(r, probes[backend], floats));
        else
#endif
            memcpy(probes[backend], s->indirect.probes, floats * 4);
        drawn[backend] = true;
        artfSceneFree(s);
        artfRendererFree(r);
    }
    printf("probes that follow, after steps away and back: red over green by the wall %.1f (CPU)", bled[0]);
    if (drawn[1]) {
        double most = 0, light = 0;
        const uint32_t count = 8 * 4 * 8;
        for (uint32_t i = 0; i < count; ++i)
            for (int k = 0; k < 3; ++k) {
                most = fmax(most, fabs(probes[0][i * 16 + k]));
                light = fmax(light, fabs(probes[0][i * 16 + k] - probes[1][i * 16 + k]));
            }
        int stamps = 0;
        for (uint32_t i = 0; i < count; ++i) stamps += probes[0][i * 16 + 7] == probes[1][i * 16 + 7];
        const double apart = differentShare(frames[0], frames[1], 16);
        printf(", %.1f (GPU); the GPU's probes against the CPU's, %.2g of the brightest at most, %d of %u stamps the same; %.2f%% of pixels apart\n", bled[1], light / fmax(most, 1e-9), stamps,
               count, apart * 100);
        CHECK(light <= most * 1e-4 && stamps == (int)count && apart < .01 && bled[1] > 3);
    } else {
        printf("\n");
    }
    CHECK(bled[0] > 3);
    for (int k = 0; k < 2; ++k) {
        artfFree(frames[k]);
        artfFree(probes[k]);
    }
}

/* Cascades (ArtfIndirectDesc.cascades): three volumes around the camera, each twice as coarse; a point near the red
 * wall, far from the camera and past the finest volume, takes red from the coarser ones, where one volume gives it
 * only what its edge holds; and the GPU's probes of all three, after the camera's steps, are the CPU's. */
void testProbeCascades(void) {
    ArtfIndirectDesc d = {{8, 4, 8}, 64, .5f, 1, {0, 0, 0}, {6, 4, 6}, false, true, 3};
    const float at[3] = {1.9f, -.98f, .3f}, up[3] = {0, 1, 0};
    double red[2] = {0, 0};
    for (int many = 0; many < 2; ++many) {
        d.cascades = many ? 3 : 1;
        ArtfScene* s = bleedScene();
        setCamera(s, -9, 3, 7, -9, 0, 0);
        artfSceneSetIndirect(s, &d);
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        for (int k = 0; k < 8; ++k) CHECK(artfRendererDraw(r, s));
        float got[3];
        artfIndirectAt(&s->indirect, at, up, got);
        red[many] = got[0] - got[1];
        artfRendererFree(r);
        artfSceneFree(s);
    }
    printf("cascades: by the red wall, 9 m from the camera, red over green %.4f with one volume, %.4f with three\n", red[0], red[1]);
    CHECK(red[1] > red[0] * 2 && red[1] > .01);
    uint32_t* frames[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    float* probes[2] = {NULL, NULL};
    uint32_t count = 0;
    bool drawn[2] = {false, false};
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) {
            missing("vulkan");
            continue;
        }
        ArtfScene* s = bleedScene();
        artfSceneSetIndirect(s, &d);
        const float stops[4] = {-2.5f, -1.f, .5f, 0};
        for (int k = 0; k < 4; ++k) {
            setCamera(s, stops[k], 3, 7, stops[k], 0, 0);
            for (int f = 0; f < 3; ++f) CHECK(artfRendererDraw(r, s));
        }
        CHECK(artfRendererReadPixels(r, frames[backend]));
        count = artfIndirectCount(&s->indirect);
        const size_t floats = (size_t)count * ARTF_PROBE_FLOATS;
        probes[backend] = artfMalloc(floats * 4);
#ifdef ARTFUL_VULKAN
        if (backend) CHECK(artfVulkanProbesRead(r, probes[backend], floats));
        else
#endif
            memcpy(probes[backend], s->indirect.probes, floats * 4);
        drawn[backend] = true;
        artfSceneFree(s);
        artfRendererFree(r);
    }
    CHECK(count == 3 * 8 * 4 * 8);
    /* (the CPU's lanes, of each instruction set, and a pixel at a time: the same bytes) */
    {
        static const ArtfSimd sets[3] = {ARTF_SIMD_AUTO, ARTF_SIMD_128, ARTF_SIMD_PORTABLE};
        uint64_t hashes[3];
        uint32_t* each = artfMalloc(W * H * 4);
        for (int k = 0; k < 3; ++k) {
            ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, k == 2 ? 1 : 0, false);
            o.simd = sets[k];
            ArtfRenderer* r = artfRendererNew(&o);
            ArtfScene* s = bleedScene();
            artfSceneSetIndirect(s, &d);
            setCamera(s, -2.5f, 3, 7, -2.5f, 0, 0);
            for (int f = 0; f < 3; ++f) CHECK(artfRendererDraw(r, s));
            CHECK(artfRendererReadPixels(r, each));
            hashes[k] = hashOf(each, (size_t)W * H);
            artfSceneFree(s);
            artfRendererFree(r);
        }
        CHECK(hashes[0] == hashes[1] && hashes[1] == hashes[2]);
        artfFree(each);
    }
    if (drawn[1]) {
        double most = 0, apartLight = 0;
        int stamps = 0;
        for (uint32_t i = 0; i < count; ++i) {
            for (int k = 0; k < 3; ++k) {
                most = fmax(most, fabs(probes[0][i * 16 + k]));
                apartLight = fmax(apartLight, fabs(probes[0][i * 16 + k] - probes[1][i * 16 + k]));
            }
            stamps += probes[0][i * 16 + 7] == probes[1][i * 16 + 7];
        }
        const double apart = differentShare(frames[0], frames[1], 16);
        printf("cascades, the GPU against the CPU after the camera's steps: probes %.2g of the brightest at most, %d of %u stamps the same; %.2f%% of pixels apart\n",
               apartLight / fmax(most, 1e-9), stamps, count, apart * 100);
        CHECK(apartLight <= most * 1e-4 && stamps == (int)count && apart < .01);
    }
    for (int k = 0; k < 2; ++k) {
        artfFree(frames[k]);
        artfFree(probes[k]);
    }
}
