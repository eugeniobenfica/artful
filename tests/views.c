/* artful-tests: views, what passes take, an orthographic camera, picking. */
#include "tests.h"

/* ---- views and what passes take ---- */

/* What a view shows of a count (artfViewHeat), as a frame holds it: with floats, as light, which shows by sRGB. */
static void heatBytes(uint32_t count, bool floats, int* out) {
    float c[3];
    artfViewHeat(count, c);
    for (int k = 0; k < 3; ++k) out[k] = floats ? shownByte(c[k]) : (int)(c[k] * 255.f + .5f);
}

static bool rgbNear(uint32_t p, const int* rgb, int margin) {
    return near_(CHANNEL(p, 16), rgb[0], margin) && near_(CHANNEL(p, 8), rgb[1], margin) && near_(CHANNEL(p, 0), rgb[2], margin);
}

/* Each view (ArtfView) drawn: what was asked for comes out, on each backend and with a frame in floats; the two
 * backends agree; and a view put back leaves the picture as it was, to the byte. */
void testViews(void) {
    uint32_t* frame = artfMalloc(W * H * 4);
    uint32_t* frames[2][2][ARTF_VIEW_COUNT];
    memset(frames, 0, sizeof frames);
    CHECK(!strcmp(artfViewName(ARTF_VIEW_BASE_COLOR), "base-color") && !artfViewName(ARTF_VIEW_COUNT));
    ArtfView named = ARTF_VIEW_LIT;
    CHECK(artfViewFromName("overdraw", &named) && named == ARTF_VIEW_OVERDRAW && !artfViewFromName("lit up", &named));
    for (int backend = 0; backend < 2; ++backend)
        for (int floats = 0; floats < 2; ++floats) {
            ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
            o.hdr = floats;
            ArtfRenderer* r = artfRendererNew(&o);
            if (!r) continue;
            /* a square facing the eye at 5, with colors whose bytes are known (shownByte) */
            ArtfScene* s = plainScene();
            ArtfMaterialDesc d = {artfVec3(.36f, .64f, .04f), 0, 0, .6f, artfVec3(.36f, 0, 0)};
            d.metallic = .2f;
            const ArtfInstance square = addAt(s, addSquare(s, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
            for (int i = 0; i < 3; ++i) artfSceneAddLight(s, artfVec3((float)i - 1.f, 0, 1), artfVec3(1, 1, 1), 10.f);
            artfSceneAddLight(s, artfVec3(0, 0, -30), artfVec3(1, 1, 1), 5.f); /* (out of reach) */
            float c[3];
            int colors[ARTF_VIEW_COUNT][3];
            const int base[3] = {shownByte(.36), shownByte(.64), shownByte(.04)};
            memcpy(colors[ARTF_VIEW_BASE_COLOR], base, sizeof base);
            memcpy(colors[ARTF_VIEW_AMBIENT], base, sizeof base); /* (a sky and ground of 1: what all around gives is the color) */
            const int normal[3] = {128, 128, 255}, rough[3] = {153, 153, 153}, metal[3] = {51, 51, 51}, own[3] = {shownByte(.36), 0, 0}, white[3] = {255, 255, 255}, depth[3] = {170, 170, 170}, still[3] = {128, 128, 128};
            memcpy(colors[ARTF_VIEW_NORMALS], normal, sizeof normal);
            memcpy(colors[ARTF_VIEW_ROUGHNESS], rough, sizeof rough);
            memcpy(colors[ARTF_VIEW_METALLIC], metal, sizeof metal);
            memcpy(colors[ARTF_VIEW_EMISSIVE], own, sizeof own);
            memcpy(colors[ARTF_VIEW_LOD], white, sizeof white);
            memcpy(colors[ARTF_VIEW_DEPTH], depth, sizeof depth); /* (10 / (10 + 5)) */
            memcpy(colors[ARTF_VIEW_MOTION], still, sizeof still);
            artfViewColor(0, c);
            for (int k = 0; k < 3; ++k) colors[ARTF_VIEW_INSTANCES][k] = colors[ARTF_VIEW_MESHLETS][k] = (int)(c[k] * 255.f + .5f);
            heatBytes(1, floats, colors[ARTF_VIEW_OVERDRAW]);
            artfViewHeat(3, c);
            for (int k = 0; k < 3; ++k) colors[ARTF_VIEW_LIGHTS][k] = (int)(c[k] * 255.f + .5f);
            const int black[3] = {0, 0, 0};
            for (int v = ARTF_VIEW_BASE_COLOR; v < ARTF_VIEW_COUNT; ++v) {
                artfRendererSetView(r, (ArtfView)v);
                CHECK(artfRendererView(r) == (ArtfView)v);
                drawInto(r, s, frame);
                const uint32_t at = frame[(H / 2) * W + W / 2 + 12], behind = frame[2 * W + 2];
                const bool good = rgbNear(at, colors[v], 1) && rgbNear(behind, black, 0);
                if (!good) printf("%s%s, view %s: %d %d %d, behind %06x\n", backend ? "GPU" : "CPU", floats ? " in floats" : "", artfViewName((ArtfView)v), CHANNEL(at, 16), CHANNEL(at, 8), CHANNEL(at, 0), behind & 0xFFFFFF);
                CHECK(good);
            }
            /* motion: the square goes right, so red is past grey; up and down, nothing */
            artfRendererSetView(r, ARTF_VIEW_MOTION);
            const ArtfTransform right = artfTransformAt(.1f, 0, 0, 1);
            artfInstanceSetTransform(s, square, &right);
            drawOnce(r, s, frame);
            const uint32_t went = frame[(H / 2) * W + W / 2 + 12];
            printf("%s%s, a square going right 3.7 pixels: red %d, green %d\n", backend ? "GPU" : "CPU", floats ? " in floats" : "", CHANNEL(went, 16), CHANNEL(went, 8));
            CHECK(CHANNEL(went, 16) > 145 && CHANNEL(went, 16) < 170 && near_(CHANNEL(went, 8), 128, 1));
            artfSceneFree(s);

            /* a fuller scene, each view, for the two backends to agree on */
            ArtfInstance moving;
            s = viewScene(&moving);
            for (int v = 0; v < ARTF_VIEW_COUNT; ++v) {
                artfRendererSetView(r, (ArtfView)v);
                drawInto(r, s, frame);
                const ArtfTransform t = artfTransformAt(1.6f, .3f, 2.f, 1);
                if (v == ARTF_VIEW_MOTION) {
                    artfInstanceSetTransform(s, moving, &t);
                    drawOnce(r, s, frame);
                }
                frames[backend][floats][v] = artfMalloc(W * H * 4);
                memcpy(frames[backend][floats][v], frame, W * H * 4);
            }
            /* back to the picture: what it was, to the byte, as if no view had been */
            artfRendererSetView(r, ARTF_VIEW_LIT);
            drawInto(r, s, frame);
            artfSceneFree(s);
            artfRendererFree(r);
            r = artfRendererNew(&o);
            s = viewScene(&moving);
            const ArtfTransform t = artfTransformAt(1.6f, .3f, 2.f, 1);
            artfInstanceSetTransform(s, moving, &t);
            uint32_t* fresh = artfMalloc(W * H * 4);
            drawInto(r, s, fresh);
            CHECK(!memcmp(frame, fresh, W * H * 4));
            artfFree(fresh);
            artfSceneFree(s);
            artfRendererFree(r);
        }
    for (int floats = 0; floats < 2; ++floats) {
        if (!frames[1][floats][0]) break;
        for (int v = 1; v < ARTF_VIEW_COUNT; ++v) {
            const double apart = differentShare(frames[0][floats][v], frames[1][floats][v], 24);
            if (apart > .002) printf("the view %s%s, the GPU against the CPU: %.3f%% of pixels apart\n", artfViewName((ArtfView)v), floats ? " in floats" : "", apart * 100);
            FLIP_ALIKE(0.076); /* (measured 0.0190 on a Radeon RX 9060 XT) */
            CHECK(apart < .01);
        }
        /* (and not all the same picture; the spheres far away are lighter levels) */
        CHECK(differentShare(frames[0][floats][ARTF_VIEW_INSTANCES], frames[0][floats][ARTF_VIEW_MESHLETS], 24) > .01);
        int lighter = 0;
        for (int i = 0; i < W * H; ++i) {
            const uint32_t p = frames[0][floats][ARTF_VIEW_LOD][i];
            lighter += CHANNEL(p, 16) == 64 && CHANNEL(p, 0) == 64 && CHANNEL(p, 8) == 255;
        }
        CHECK(lighter > 0);
    }
    for (int i = 0; i < 2 * 2 * ARTF_VIEW_COUNT; ++i) artfFree((&frames[0][0][0])[i]);
    artfFree(frame);
}

/* What each pass took: listed, in order, adding up to the frame. */
void testPassTimes(void) {
    uint32_t* frame = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend)
        for (int floats = 0; floats < 2; ++floats) {
            ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
            o.hdr = floats;
            ArtfRenderer* r = artfRendererNew(&o);
            if (!r) continue;
            ArtfScene* s = richScene();
            const ArtfShadowDesc shadows = {512, 0, false};
            artfSceneSetShadows(s, &shadows);
            const ArtfPostDesc post = {.lens.bloom = .5f, .screen.occlusion = 1};
            artfSceneSetPost(s, &post);
            drawInto(r, s, frame);
            ArtfFrameStats st;
            artfRendererStats(r, &st);
            double sum = 0;
            bool named = st.passCount > 0 && st.passCount <= ARTF_PASS_TIMES, scene = false, bloom = false;
            for (uint32_t k = 0; k < st.passCount; ++k) {
                named = named && st.passes[k].name && st.passes[k].ms >= 0;
                if (!st.passes[k].name) continue;
                sum += st.passes[k].ms;
                scene |= !strncmp(st.passes[k].name, backend ? "scene" : "tiles", 5); /* (with occlusion, "scene, early" and "scene, late") */
                bloom |= !strcmp(st.passes[k].name, "bloom");
            }
            const double whole = backend ? st.gpuMs : st.cpuMs;
            printf("%s%s: %u passes, %.3f ms of %.3f\n", backend ? "GPU" : "CPU", floats ? " in floats" : "", st.passCount, sum, whole);
            CHECK(named && scene && bloom == (bool)floats);
            CHECK(whole > 0 && sum <= whole * 1.001 + .001 && sum >= whole * .9 - .05);
            artfSceneFree(s);
            artfRendererFree(r);
        }
    artfFree(frame);
}

/* ---- an orthographic camera ---- */

/* A scene for it: a floor, things on it that shine and that do not, the sun with shadows, a lamp. */
static ArtfScene* orthoScene(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says: the hash is of the defaults) */
    artfSceneSetFeature(s, ARTF_FEATURE_CONES, true);
    static uint32_t px[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) px[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0xFFD0C8B0u : 0xFF404858u;
    ArtfMaterialDesc floorDesc = artfMaterialDefaults();
    floorDesc.texture = artfSceneAddTexture(s, px, 64, 64, 0);
    floorDesc.roughness = .25f;
    ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 8), artfSceneAddMaterial(s, &floorDesc), &t);
    ArtfMaterialDesc metal = artfMaterialDefaults();
    metal.baseColor = artfVec3(.9f, .7f, .3f);
    metal.metallic = 1;
    metal.roughness = .15f;
    ArtfMaterialDesc red = artfMaterialDefaults();
    red.baseColor = artfVec3(.8f, .15f, .1f);
    red.roughness = .4f;
    t = artfTransformAt(-1.8f, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddSphere(s, 1, 32, 48), artfSceneAddMaterial(s, &metal), &t);
    t = artfTransformTrs(artfVec3(.4f, 0, .3f), artfQuatAxisAngle(artfVec3(1, 0, .3f), 1.f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, artfSceneAddTorus(s, 1, .3f, 48, 24), artfSceneAddMaterial(s, &red), &t);
    t = artfTransformTrs(artfVec3(2, 0, -.5f), artfQuatAxisAngle(artfVec3(0, 1, 0), .5f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, artfSceneAddCube(s, 1.4f), 0, &t);
    artfSceneSetSun(s, artfVec3(-.4f, -1, -.3f), artfVec3(2, 1.9f, 1.7f));
    artfSceneAddLight(s, artfVec3(0, 1.5f, 2.5f), artfVec3(.2f, .5f, 1.f), 6.f);
    const ArtfShadowDesc shadows = {512, 20, false};
    artfSceneSetShadows(s, &shadows);
    const ArtfCamera eye = {artfVec3(1, 3, 7), artfVec3(0, 0, 0), artfVec3(0, 1, 0), .9f, .1f};
    artfSceneSetCamera(s, &eye);
    return s;
}

/* That scene drawn as drawn, through all that comes after a frame in floats, with fog that light goes through, and
 * traced: by a camera, or the scene's (NULL). */
#define ORTHO_SCENARIOS 4
static bool orthoPortable; /* the CPU's kernels as plain loops, on three threads */
static void orthoFrame(ArtfBackend backend, int k, const ArtfCamera* camera, uint32_t* frame, bool* drawn) {
    ArtfRendererOptions o = options(backend, orthoPortable ? 3 : 0, orthoPortable);
    o.hdr = k == 1 || k == 2;
    ArtfRenderer* r = artfRendererNew(&o);
    *drawn = r != NULL;
    if (!r) return;
    ArtfScene* s = orthoScene();
    if (camera) artfSceneSetCamera(s, camera);
    if (k == 1) {
        const ArtfPostDesc all = {.lens.bloom = .2f, .screen.occlusion = 1, .screen.reflections = 1, .screen.temporal = true, .lens.focus = 6, .lens.aperture = 6};
        artfSceneSetPost(s, &all);
    } else if (k == 2) {
        const ArtfPostDesc air = {.screen.volumetric = 1, .screen.scattering = .5f};
        artfSceneSetPost(s, &air);
        artfSceneSetFog(s, artfVec3(.5f, .5f, .55f), .05f);
    } else if (k == 3) {
        const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, camera ? 8 : 2, 0, 0, false, false};
        artfSceneSetTrace(s, &rays);
    }
    for (int i = 0; i < (k == 3 && camera ? 8 : 3); ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
    artfSceneFree(s);
    artfRendererFree(r);
}

/* How many pixels are lit (green above 40), and their mean column and row. */
static int litPixels(const uint32_t* p, float* meanX, float* meanY) {
    int n = 0;
    double sx = 0, sy = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (CHANNEL(p[y * W + x], 8) > 40) {
                ++n;
                sx += x;
                sy += y;
            }
    if (meanX) *meanX = n ? (float)(sx / n) : 0;
    if (meanY) *meanY = n ? (float)(sy / n) : 0;
    return n;
}

/* An orthographic camera: perspective frames are what they were, to the byte; all rays go one way, so things keep their
 * size however far, and moving the eye back changes nothing; occlusion changes no pixel; and the backends agree on
 * all that comes after a frame and on tracing. */
void testOrthographic(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    bool drawn = false, gpuDrawn = false;
    /* perspective, as before there was this: the hashes of the frames the commit before it drew (made again when frames
     * became sRGB, when highlights got back what their lobe loses, and when traced pixels took blue noise: FLIP 0.028) */
    static const uint64_t before[ORTHO_SCENARIOS] = {0x5C41276CACD61032ull, 0x998C2A18722A14FFull, 0x106CDB6E5BF5B726ull, 0xD22AA2BFE65F1308ull};
    for (int k = 0; k < ORTHO_SCENARIOS; ++k) {
        orthoFrame(ARTF_BACKEND_SOFTWARE, k, NULL, a, &drawn);
        if (hashOf(a, W * H) != before[k]) printf("perspective %d: %016llx, where %016llx was\n", k, (unsigned long long)hashOf(a, W * H), (unsigned long long)before[k]);
        CHECK(hashOf(a, W * H) == before[k]);
    }

    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        /* a cube 2 high, in a view 4 high: half the frame's height, from 10 away or 30 */
        ArtfScene* s = plainScene();
        addAt(s, artfSceneAddCube(s, 2), whiteMaterial(s), 0, 0, 0, 1);
        ArtfCamera eye = {artfVec3(0, 0, 10), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, 4.f, 0};
        artfSceneSetCamera(s, &eye);
        drawInto(r, s, a);
        int rows = 0;
        for (int y = 0; y < H; ++y) rows += CHANNEL(a[y * W + W / 2], 8) > 40;
        printf("%s, orthographic: a cube 2 high in a view 4 high takes %d of %d rows\n", backend ? "GPU" : "CPU", rows, H);
        CHECK(abs(rows - H / 2) <= 1);
        eye.position.z = 30;
        artfSceneSetCamera(s, &eye);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 0) == 0);
        artfSceneFree(s);
        /* two balls, one far behind the other: the same size */
        s = plainScene();
        const ArtfMesh ball = artfSceneAddSphere(s, 1, 24, 32);
        const ArtfInstance near = addAt(s, ball, whiteMaterial(s), -2.f, 0, 0, 1);
        const ArtfInstance far = addAt(s, ball, whiteMaterial(s), 2.f, 0, -40, 1);
        eye.position.z = 10;
        eye.orthoHeight = 8;
        artfSceneSetCamera(s, &eye);
        artfInstanceSetOpacity(s, far, 0);
        drawInto(r, s, a);
        artfInstanceSetOpacity(s, far, 1);
        artfInstanceSetOpacity(s, near, 0);
        drawInto(r, s, b);
        float ax, ay, bx, by;
        const int nearPixels = litPixels(a, &ax, &ay), farPixels = litPixels(b, &bx, &by);
        printf("%s, orthographic: a ball 10 away takes %d pixels, one 50 away %d\n", backend ? "GPU" : "CPU", nearPixels, farPixels);
        CHECK(nearPixels > 1000 && abs(nearPixels - farPixels) <= nearPixels / 200 && fabsf(ay - by) < .5f && fabsf(bx - ax - 100.f) < .5f);
        artfSceneFree(s);
        /* what is hidden is not drawn, and no pixel changes for it; lighter meshes as far as the view is wide */
        s = orthoScene();
        const ArtfCamera iso = {artfVec3(6, 5, 6), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, 6.f, 0};
        artfSceneSetCamera(s, &iso);
        ArtfTransform wall = artfTransformTrs(artfVec3(1.5f, 0, 1.5f), artfQuatAxisAngle(artfVec3(0, 1, 0), .785f), artfVec3(6, 6, .2f));
        artfSceneAddInstance(s, artfSceneAddCube(s, 1), 0, &wall);
        const ArtfMesh small = artfSceneAddSphere(s, .2f, 8, 12);
        for (int i = 0; i < 64; ++i) addAt(s, small, 0, -1.f - (float)(i % 8) * .3f, -.5f, -1.f - (float)(i / 8) * .3f, 1);
        drawInto(r, s, a);
        artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
        for (int i = 0; i < 3; ++i) drawInto(r, s, b);
        ArtfFrameStats st;
        artfRendererStats(r, &st);
        if (!backend) printf("CPU, orthographic with occlusion: %u instances drawn of %u\n", st.instancesDrawn, artfSceneInstanceCount(s));
        CHECK(differentShare(a, b, 0) == 0);
        artfSceneFree(s);
        artfRendererFree(r);
    }

    /* lighter meshes as the view widens, by how big things are in it and not by how far */
    {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        ArtfScene* s = plainScene();
        const ArtfMesh ball = artfSceneAddSphere(s, 1, 64, 96);
        addAt(s, ball, whiteMaterial(s), 0, 0, 0, 1);
        uint32_t binned[2];
        for (int wide = 0; wide < 2; ++wide) {
            const ArtfCamera eye = {artfVec3(0, 0, 5), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, wide ? 200.f : 3.f, 0};
            artfSceneSetCamera(s, &eye);
            drawInto(r, s, a);
            ArtfFrameStats st;
            artfRendererStats(r, &st);
            binned[wide] = st.trianglesBinned;
        }
        printf("orthographic, levels of detail: a ball in a view 3 high bins %u triangles, in one 200 high %u\n", binned[0], binned[1]);
        CHECK(binned[1] * 16 < binned[0]);
        artfSceneFree(s);
        artfRendererFree(r);
    }

    /* the backends agree with four samples a pixel */
    {
        ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
        o.antialias = true;
        ArtfRenderer* gpu = artfRendererNew(&o);
        if (gpu) {
            o.backend = ARTF_BACKEND_SOFTWARE;
            ArtfRenderer* cpu = artfRendererNew(&o);
            ArtfScene* s = orthoScene();
            const ArtfCamera iso = {artfVec3(6, 5, 6), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, 7.f, 0};
            artfSceneSetCamera(s, &iso);
            drawInto(cpu, s, a);
            drawInto(gpu, s, b);
            const double apart = differentShare(a, b, 24);
            printf("orthographic, four samples a pixel, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
            FLIP_ALIKE(0.136); /* (measured 0.0339 on a Radeon RX 9060 XT) */
            CHECK(apart < .005);
            artfSceneFree(s);
            artfRendererFree(cpu);
            artfRendererFree(gpu);
        }
    }

    /* the backends agree, on what is drawn, all that comes after, fog that light goes through, and rays */
    const ArtfCamera iso = {artfVec3(6, 5, 6), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, 7.f, 0};
    static const double most[ORTHO_SCENARIOS] = {.003, .03, .01, .02};
    for (int k = 0; k < ORTHO_SCENARIOS; ++k) {
        orthoFrame(ARTF_BACKEND_VULKAN, k, &iso, b, &gpuDrawn);
        if (!gpuDrawn) break;
        orthoFrame(ARTF_BACKEND_SOFTWARE, k, &iso, a, &drawn);
        /* (and the CPU's bytes are the same whatever its kernels and threads) */
        orthoPortable = true;
        orthoFrame(ARTF_BACKEND_SOFTWARE, k, &iso, c, &drawn);
        orthoPortable = false;
        CHECK(hashOf(a, W * H) == hashOf(c, W * H));
        const double apart = differentShare(a, b, 24);
        printf("orthographic, %s, the GPU against the CPU: %.2f%% of pixels apart\n", k == 0 ? "drawn" : k == 1 ? "all after the frame" : k == 2 ? "fog that light goes through" : "traced", apart * 100);
        FLIP_ALIKE(0.147); /* (measured 0.0367 on a Radeon RX 9060 XT) */
        CHECK(apart < most[k]);
        /* and not the perspective's picture */
        orthoFrame(ARTF_BACKEND_SOFTWARE, k, NULL, b, &drawn);
        CHECK(differentShare(a, b, 24) > .2);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* ---- picking ---- */


/* What is under a point of the frame, and along any ray: the right instance, place, normal and distance; through holes,
 * what is mostly not there and particles; bones as posed; in agreement with the frame drawn; and cheap. */
void testPick(void) {
    ArtfScene* s = plainScene();
    const ArtfMaterial white = whiteMaterial(s);
    const ArtfInstance cube = addAt(s, artfSceneAddCube(s, 2), white, 0, 0, 0, 1);
    const ArtfInstance ball = addAt(s, artfSceneAddSphere(s, 1, 32, 48), white, 3, 0, 0, 1);
    ArtfPickHit hit;
    /* the middle of the frame: the cube's front, 4 from the eye */
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit));
    CHECK(hit.instance == cube && fabsf(hit.position.z - 1.f) < 1e-4f && fabsf(hit.distance - 4.f) < 1e-3f);
    CHECK(fabsf(hit.normal.z - 1.f) < 1e-5f && hit.u >= 0 && hit.u <= 1 && hit.v >= 0 && hit.v <= 1);
    CHECK(!artfScenePick(s, NULL, 1, 1, W, H, &hit));
    /* down onto the ball's top */
    CHECK(artfSceneRaycast(s, artfVec3(3, 5, 0), artfVec3(0, -2, 0), 0, &hit));
    CHECK(hit.instance == ball && fabsf(hit.position.y - 1.f) < .01f && hit.normal.y > .99f && fabsf(hit.distance - 4.f) < .01f);
    CHECK(!artfSceneRaycast(s, artfVec3(3, 5, 0), artfVec3(0, -1, 0), 3.5f, &hit));
    CHECK(!artfSceneRaycast(s, artfVec3(3, 5, 0), artfVec3(0, 1, 0), 0, &hit));
    /* a square in front of the cube: with holes where its texture has none, faded, shown, and of particles */
    ArtfMaterialDesc holes = {artfVec3(1, 1, 1), oneColor(s, 0x00FFFFFFu), 0, 1, artfVec3(0, 0, 0), ARTF_ALPHA_MASK, .5f, 0, false};
    const ArtfMaterial cut = artfSceneAddMaterial(s, &holes);
    const ArtfInstance square = addAt(s, addSquare(s, 0, 0, 0, 0), cut, 0, 0, 2, .5f);
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit) && hit.instance == cube);
    artfInstanceSetMaterial(s, square, white);
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit) && hit.instance == square && fabsf(hit.distance - 3.f) < 1e-3f);
    artfInstanceSetOpacity(s, square, .3f);
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit) && hit.instance == cube);
    artfInstanceSetOpacity(s, square, .8f);
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit) && hit.instance == square);
    artfSceneRemoveInstance(s, square);
    ArtfEmitterDesc smoke;
    memset(&smoke, 0, sizeof smoke);
    smoke.position = artfVec3(0, 0, 2.5f);
    smoke.size = smoke.sizeEnd = 1.5f;
    smoke.color = smoke.colorEnd = artfVec3(1, 1, 1);
    smoke.opacity = smoke.opacityEnd = 1;
    smoke.life = 10;
    const ArtfEmitter e = artfSceneAddEmitter(s, &smoke);
    artfEmitterBurst(s, e, 8);
    artfEmitterStep(s, e, .01f);
    CHECK(artfEmitterAlive(s, e) == 8);
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit) && hit.instance == cube);
    artfSceneRemoveEmitter(s, e);

    /* as the frame shows it, pixel by pixel, perspective and orthographic */
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* frame = artfMalloc(W * H * 4);
    for (int ortho = 0; ortho < 2; ++ortho) {
        const ArtfCamera eye = {artfVec3(1, 2, 6), artfVec3(1, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, ortho ? 6.f : 0.f, 0};
        artfSceneSetCamera(s, &eye);
        drawInto(r, s, frame);
        int wrong = 0, shown = 0;
        for (int y = 0; y < H; y += 2)
            for (int x = 0; x < W; x += 2) {
                const bool there = artfScenePick(s, NULL, (float)x + .5f, (float)y + .5f, W, H, &hit), lit = CHANNEL(frame[y * W + x], 8) > 0;
                wrong += there != lit;
                shown += lit;
            }
        printf("picking, %s: %d of %d points that show differ from the frame\n", ortho ? "orthographic" : "perspective", wrong, shown);
        CHECK(shown > 1500 && wrong <= shown / 100);
    }
    /* orthographic: from the view's plane, straight ahead */
    const ArtfCamera plan = {artfVec3(0, 0, 10), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .5f, 4.f, 0};
    CHECK(artfScenePick(s, &plan, W * .375f, H * .5f, W, H, &hit) && hit.instance == cube && fabsf(hit.distance - 9.f) < 1e-3f && fabsf(hit.position.x + .8f) < 1e-3f);

    /* cheap: no tree is made again while nothing changes */
    const uint64_t top = s->trace.topVersion;
    const double t0 = artfNowMs();
    int found = 0;
    for (int i = 0; i < 10000; ++i) found += artfScenePick(s, NULL, (float)(i * 7 % W), (float)(i * 13 % H), W, H, &hit);
    const double took = artfNowMs() - t0;
    printf("picking: 10,000 picks in %.1f ms\n", took);
    CHECK(found > 0 && s->trace.topVersion == top);

    /* what moved, when a pick saw it first, is still seen to have moved by the frame after */
    const ArtfTransform moved = artfTransformAt(3, .5f, 0, 1);
    artfInstanceSetTransform(s, ball, &moved);
    CHECK(artfSceneRaycast(s, artfVec3(3, 5, 0), artfVec3(0, -1, 0), 0, &hit) && fabsf(hit.position.y - 1.5f) < .01f);
    artfSceneTraceSync(s);
    bool still = true;
    for (size_t i = 0; i < s->trace.instanceHandles.len; ++i)
        if (s->trace.instanceHandles.items[i] == ball) still = s->trace.motion.items[i * 12 + 7] == 0;
    CHECK(!still);

    /* bones, as posed */
    artfSceneRemoveInstance(s, ball);
    const ArtfInstance strip = addAt(s, addStrip(s), white, 4, -1, 0, 1);
    bendStrip(s, strip, 0);
    CHECK(artfSceneRaycast(s, artfVec3(4, .8f, 5), artfVec3(0, 0, -1), 0, &hit) && hit.instance == strip);
    bendStrip(s, strip, 1.4f);
    CHECK(!artfSceneRaycast(s, artfVec3(4, .8f, 5), artfVec3(0, 0, -1), 0, &hit) || hit.instance != strip);

    artfFree(frame);
    artfRendererFree(r);
    artfSceneFree(s);
}
