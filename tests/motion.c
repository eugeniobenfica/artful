/* artful-tests: what bends and moves: bones, targets, clips, motion followed, motion blur. */
#include "tests.h"

/* What moves, followed: with nothing moving, motion blur changes no byte; a sphere going sideways is blurred along its
 * way and not across it; temporal antialiasing follows it, so it leaves no trail; both backends alike. */
void testMotion(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    uint32_t* blurred[2] = {artfCalloc(W * H, 4), artfCalloc(W * H, 4)}; /* (zeros: where Vulkan does not run, the GPU's stays so) */
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfInstance ball;
        ArtfScene* s = movingScene(&ball);
        /* still: not a byte changes */
        drawInto(r, s, a);
        const ArtfPostDesc blur = {.lens.motionBlur = .5f};
        artfSceneSetPost(s, &blur);
        drawInto(r, s, b);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 0) == 0);
        /* going sideways: blurred along its way; above and below it, nothing changes */
        placeBall(s, ball, -.6f);
        drawOnce(r, s, blurred[backend]);
        artfSceneSetPost(s, NULL);
        drawInto(r, s, a);
        const double changed = differentShare(a, blurred[backend], 8);
        int across = 0;
        for (int y = 0; y < H / 5; ++y)
            for (int x = 0; x < W; ++x) across += a[y * W + x] != blurred[backend][y * W + x];
        printf("%s, motion blur: %.2f%% of pixels blurred, %d across its way\n", backend ? "GPU" : "CPU", changed * 100, across);
        CHECK(changed > .003 && changed < .1 && across == 0);
        /* temporal antialiasing, the sphere going on: what it shows against the sphere drawn still there */
        const ArtfPostDesc temporal = {.screen.temporal = true};
        artfSceneSetPost(s, &temporal);
        for (int k = 0; k < 12; ++k) {
            placeBall(s, ball, -.9f + (float)k * .15f);
            drawOnce(r, s, b);
        }
        artfSceneSetPost(s, NULL);
        drawInto(r, s, c);
        const double trail = meanApart(b, c);
        printf("%s, temporal antialiasing on a moving sphere: %.2f of 255 from it drawn still, on average\n", backend ? "GPU" : "CPU", trail);
        CHECK(trail < 1.5);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (blurred[1][0] || blurred[1][W * H / 2]) {
        const double apart = differentShare(blurred[0], blurred[1], 24);
        printf("motion blur, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0108); /* (measured 0.0008 on a Radeon RX 9060 XT) */
        CHECK(apart < .005);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c);
    artfFree(blurred[0]);
    artfFree(blurred[1]);
}

/* What bones bend is followed by its vertices: a strip bending is blurred where it moves, and temporal antialiasing
 * leaves no trail behind it; both backends alike. */
/* addStrip's, wider, with a picture of stripes across it: what is followed wrong smears them. */
static ArtfInstance addStripedStrip(ArtfScene* s) {
    enum { ROWS = 12 };
    static ArtfVertex v[(ROWS + 1) * 2];
    static ArtfVertexSkin k[(ROWS + 1) * 2];
    static uint32_t idx[ROWS * 6];
    static uint32_t stripes[8 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 8; ++x) stripes[y * 8 + x] = (y / 4) & 1 ? 0xFFFFFFFFu : 0xFF202020u;
    for (int r = 0; r <= ROWS; ++r)
        for (int c = 0; c < 2; ++c) {
            const float y = (float)r / ROWS * 2.f;
            const ArtfVertex q = {c ? .5f : -.5f, y, 0, 0, 0, 1, (float)c, y * .5f, 0};
            const float top = artfClampf((y - .8f) / .4f, 0, 1);
            const ArtfVertexSkin w = {{0, 1, 0, 0}, {1.f - top, top, 0, 0}};
            v[r * 2 + c] = q;
            k[r * 2 + c] = w;
        }
    uint32_t n = 0;
    for (uint32_t r = 0; r < ROWS; ++r) {
        const uint32_t a = r * 2;
        idx[n++] = a; idx[n++] = a + 1; idx[n++] = a + 3;
        idx[n++] = a; idx[n++] = a + 3; idx[n++] = a + 2;
    }
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTexture(s, stripes, 8, 64, 0), 0, 1, artfVec3(0, 0, 0)};
    return addAt(s, artfSceneAddSkinnedMesh(s, v, (ROWS + 1) * 2, idx, n, k), artfSceneAddMaterial(s, &d), 0, -1, 0, 1);
}

void testSkinnedMotion(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* blurred[2] = {artfCalloc(W * H, 4), artfCalloc(W * H, 4)}; /* (zeros: where Vulkan does not run, the GPU's stays so) */
    memset(blurred[1], 0, W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* s = plainScene();
        const ArtfInstance strip = addAt(s, addStrip(s), whiteMaterial(s), 0, -1, 0, 1);
        /* blurred: only where it bends */
        const ArtfPostDesc blur = {.lens.motionBlur = .5f};
        artfSceneSetPost(s, &blur);
        bendStrip(s, strip, 0);
        drawInto(r, s, a);
        bendStrip(s, strip, .5f);
        drawOnce(r, s, blurred[backend]);
        artfSceneSetPost(s, NULL);
        drawInto(r, s, b);
        int moved = 0, below = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const bool differs = abs(CHANNEL(b[y * W + x], 16) - CHANNEL(blurred[backend][y * W + x], 16)) > 8;
                moved += differs;
                below += differs && y > H / 2 + 10; /* (the strip's foot, which the bone does not move) */
            }
        printf("%s, a strip that bones bend, blurred: %d pixels changed, %d where it does not move\n", backend ? "GPU" : "CPU", moved, below);
        CHECK(moved > 150 && below == 0);
        /* followed: no trail */
        const ArtfPostDesc temporal = {.screen.temporal = true};
        artfSceneFree(s);
        s = plainScene();
        const ArtfInstance striped = addStripedStrip(s);
        artfSceneSetPost(s, &temporal);
        for (int k = 0; k < 16; ++k) {
            bendStrip(s, striped, (float)k * .1f);
            drawOnce(r, s, a);
        }
        /* (against the same, still, once temporal antialiasing has settled on it) */
        drawFrames(r, s, 32, b);
        const double trail = meanApart(a, b), far = differentShare(a, b, 48);
        printf("%s, temporal antialiasing on a striped strip that bones bend: %.2f of 255 from it settled still, %.2f%% of pixels by more than 48\n", backend ? "GPU" : "CPU",
               trail, far * 100);
        CHECK(trail < .6 && far < .004);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (blurred[1][0] || blurred[1][W * H / 2]) {
        const double apart = differentShare(blurred[0], blurred[1], 24);
        printf("a bending strip blurred, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0101); /* (measured 0.0001 on a Radeon RX 9060 XT) */
        CHECK(apart < .005);
    }
    artfFree(a);
    artfFree(b);
    artfFree(blurred[0]);
    artfFree(blurred[1]);
}

/* One renderer, scene after scene, each with skins of its own and a vertex buffer of another size: what the GPU's bones
 * read and write is made again for each, and its frame stays the CPU's. (A buffer made again can have the handle of the
 * one it replaces: the bones' bindings went stale then, and read or wrote memory given back, which lost the device.) */
void testSkinScenes(void) {
    ArtfRendererOptions so = options(ARTF_BACKEND_SOFTWARE, 0, false), vo = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&vo);
    if (!gpu) return;
    ArtfRenderer* cpu = artfRendererNew(&so);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    double worst = 0;
    for (int k = 0; k < 24; ++k) {
        ArtfScene* s = plainScene();
        /* (meshes that are not drawn, but take room in the vertex buffer: more or fewer each time) */
        for (int j = 0; j < k % 4; ++j) artfSceneAddSphere(s, .1f, 8 + 4 * (k % 7), 16);
        const int strips = 1 + k % 3;
        for (int j = 0; j < strips; ++j) bendStrip(s, addAt(s, addStrip(s), whiteMaterial(s), (float)j - (float)(strips - 1) * .5f, -1, 0, 1), .15f * (float)(k + j));
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        worst = apart > worst ? apart : worst;
        artfSceneFree(s);
    }
    printf("bones on the GPU, 24 scenes in turn on one renderer: at most %.2f%% of pixels apart from the CPU\n", worst * 100);
    CHECK(worst < .005);
    artfFree(a);
    artfFree(b);
    artfRendererFree(cpu);
    artfRendererFree(gpu);
}

/* Motion blur keeps the edges of what is in front: a ball standing still before a wall that moves stays sharp, and
 * none of it is smeared over the wall around it; both backends alike. */
static ArtfScene* movingWallScene(ArtfInstance* wall, ArtfInstance* ball) {
    ArtfScene* s = plainScene();
    setCamera(s, 0, 0, 6, 0, 0, 0);
    static uint32_t stripes[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) stripes[y * 64 + x] = (x / 4) & 1 ? 0xFF206020u : 0xFF102010u;
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTexture(s, stripes, 64, 64, 0), 0, 1, artfVec3(0, 0, 0)};
    *wall = addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, -4, 6);
    ArtfMaterialDesc white = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    *ball = addAt(s, artfSceneAddSphere(s, .8f, 32, 48), artfSceneAddMaterial(s, &white), 0, 0, 0, 1);
    return s;
}

void testBlurEdges(void) {
    uint32_t* still = artfMalloc(W * H * 4);
    uint32_t* bare = artfMalloc(W * H * 4);
    uint32_t* blurred[2] = {artfCalloc(W * H, 4), artfCalloc(W * H, 4)}; /* (zeros: where Vulkan does not run, the GPU's stays so) */
    uint32_t* wallOnly = artfMalloc(W * H * 4);
    memset(blurred[1], 0, W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfInstance wall, ball;
        ArtfScene* s = movingWallScene(&wall, &ball);
        const ArtfPostDesc blur = {.lens.motionBlur = 1};
        const ArtfTransform from = artfTransformAt(0, 0, -4, 6), to = artfTransformAt(.6f, 0, -4, 6);
        /* the ball still, and the wall moving behind it */
        drawInto(r, s, still);
        artfSceneSetPost(s, &blur);
        artfInstanceSetTransform(s, wall, &from);
        drawInto(r, s, bare);
        artfInstanceSetTransform(s, wall, &to);
        drawOnce(r, s, blurred[backend]);
        /* the wall alone, as it moved */
        const ArtfTransform away = artfTransformAt(0, 40, 0, 1);
        artfInstanceSetTransform(s, ball, &away);
        artfInstanceSetTransform(s, wall, &from);
        drawInto(r, s, bare);
        artfInstanceSetTransform(s, wall, &to);
        drawOnce(r, s, wallOnly);
        /* the ball is where the still frame is white; around it, a ring of 1 to 4 pixels */
        int inside = 0, insideChanged = 0, ring = 0;
        double ringApart = 0;
        for (int y = 4; y < H - 4; ++y)
            for (int x = 4; x < W - 4; ++x) {
                const bool white = CHANNEL(still[y * W + x], 16) > 200;
                bool near = false, deep = white;
                for (int dy = -4; dy <= 4; ++dy)
                    for (int dx = -4; dx <= 4; ++dx) {
                        const bool there = CHANNEL(still[(y + dy) * W + x + dx], 16) > 200;
                        near |= there;
                        if (abs(dx) <= 2 && abs(dy) <= 2) deep &= there;
                    }
                const uint32_t p = blurred[backend][y * W + x];
                if (deep) {
                    ++inside;
                    insideChanged += abs(CHANNEL(p, 16) - CHANNEL(still[y * W + x], 16)) > 2;
                } else if (near && !white) {
                    ++ring;
                    ringApart += abs(CHANNEL(p, 8) - CHANNEL(wallOnly[y * W + x], 8)) + abs(CHANNEL(p, 16) - CHANNEL(wallOnly[y * W + x], 16));
                }
            }
        ringApart /= ring > 0 ? ring : 1;
        printf("%s, motion blur before a still ball: %d of %d pixels of it changed; around it, %.1f of 255 from the wall alone\n", backend ? "GPU" : "CPU", insideChanged, inside,
               ringApart);
        CHECK(inside > 500 && insideChanged == 0 && ringApart < 8);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (blurred[1][0] || blurred[1][W * H / 2]) {
        const double apart = differentShare(blurred[0], blurred[1], 24);
        printf("motion blur before a still ball, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0124); /* (measured 0.0024 on a Radeon RX 9060 XT) */
        CHECK(apart < .01);
    }
    artfFree(still);
    artfFree(bare);
    artfFree(wallOnly);
    artfFree(blurred[0]);
    artfFree(blurred[1]);
}

void testSlerp(void) {
    /* around y, at a steady speed, even where the two are far apart */
    const float half = 1.3089969f; /* 150 degrees, halved */
    const float a[4] = {0, 0, 0, 1}, b[4] = {0, sinf(half), 0, cosf(half)};
    for (int i = 1; i < 8; ++i) {
        float q[4];
        artfQuatSlerp(a, b, (float)i / 8.f, q);
        CHECK(fabsf(q[1] - sinf(half * (float)i / 8.f)) < 1e-5f && fabsf(q[3] - cosf(half * (float)i / 8.f)) < 1e-5f);
    }
}

void testSkin(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    ArtfMesh strip = addStrip(s);
    ArtfInstance one = addAt(s, strip, white, -1, -1, 0, 1), two = addAt(s, strip, white, 1, -1, 0, 1);
    CHECK(artfSceneInstanceCount(s) == 2);
    /* a pixel high up on the left strip: there when straight, gone when bent; the other stays */
    const size_t high = (size_t)(H / 2 - 30) * W + W / 2 - 36, other = (size_t)(H / 2 - 30) * W + W / 2 + 36;
    for (int traced = 0; traced < 2; ++traced) {
        const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 6, 0};
        artfSceneSetTrace(s, traced ? &rays : NULL);
        bendStrip(s, one, 0);
        for (int i = 0; i < 7; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        CHECK(a[high] == 0xFFFFFFFFu && a[other] == 0xFFFFFFFFu);
        bendStrip(s, one, 1.4f);
        for (int i = 0; i < 7; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, b));
        CHECK(b[high] == 0xFF000000u && b[other] == 0xFFFFFFFFu);
    }
    artfSceneSetTrace(s, NULL);
    /* a wrong call says so */
    artfSetLog(countLog, &logged);
    logged = 0;
    ArtfInstance plain = addAt(s, addSquare(s, 0, 0, 0, 0), white, 0, 0, -3, 1);
    bendStrip(s, plain, 1.f);
    CHECK(logged == 1);
    artfSetLog(NULL, NULL);
    (void)two;
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* The two backends, drawn and traced, and shadows, see the same pose. */
/* A square that a target stretches up, and bones that take it far from where it was modelled. */
void testMorph(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    const ArtfVertex v[4] = {{-.3f, -.3f, 0, 0, 0, 1, 0, 0, 0}, {.3f, -.3f, 0, 0, 0, 1, 0, 0, 0}, {.3f, .3f, 0, 0, 0, 1, 0, 0, 0}, {-.3f, .3f, 0, 0, 0, 1, 0, 0, 0}};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    const float up[12] = {0, 0, 0, 0, 0, 0, 0, .8f, 0, 0, .8f, 0};
    const ArtfMorphTarget target = {up, NULL};
    ArtfVertexSkin k[4];
    memset(k, 0, sizeof k);
    for (int i = 0; i < 4; ++i) {
        k[i].joints[5] = 1; /* (in the second half of what a vertex has) */
        k[i].weights[5] = 1;
    }
    const ArtfMesh mesh = artfSceneAddMorphMesh(s, v, 4, idx, 6, k, &target, 1);
    CHECK(mesh != 0);
    const ArtfInstance i = addAt(s, mesh, whiteMaterial(s), 0, 0, 0, 1);
    const size_t high = (size_t)(H / 2 - 30) * W + W / 2, mid = (size_t)(H / 2) * W + W / 2;
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFFFFFFFFu && a[high] == 0xFF000000u);
    const float full = 1.f;
    artfInstanceSetMorphWeights(s, i, &full, 1);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFFFFFFFFu && a[high] == 0xFFFFFFFFu);

    /* far out, and back: the box its bits stand for follows */
    ArtfTransform joints[2] = {artfTransformIdentity(), artfTransformIdentity()};
    joints[1].m[3] = 500.f;
    artfInstanceSetJoints(s, i, joints, 2);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF000000u);
    setCamera(s, 500, 0, 5, 500, 0, 0);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFFFFFFFFu && a[high] == 0xFFFFFFFFu);
    artfSceneFree(s);
    artfFree(a);
    artfRendererFree(r);
}

void testSkinBothBackends(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    ArtfMesh strip = addStrip(s);
    ArtfInstance i = addAt(s, strip, 0, 2, -1, 1, 1);
    const ArtfShadowDesc desc = {1024, 40};
    artfSceneSetShadows(s, &desc);
    for (int step = 0; step < 4; ++step) {
        bendStrip(s, i, (float)step * .5f);
        drawInto(gpu, s, a);
        drawInto(cpu, s, b);
        CHECK(differentShare(a, b, 24) < .01);
    }
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}

/* A .glb with a clip: a node that moves from x = 0 to x = 2 in 2 seconds; and a skin of two bones, the second
 * turning a quarter around z in the same time. */
static size_t makeAnimatedGlb(uint8_t* out) {
    static const float times[2] = {0, 2}, moves[6] = {0, 0, 0, 2, 0, 0};
    static const float turns[8] = {0, 0, 0, 1, 0, 0, .70710678f, .70710678f};
    static const float positions[12] = {-.5f, 0, 0, .5f, 0, 0, .5f, 2, 0, -.5f, 2, 0};
    static const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    static const uint8_t joints[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    static const float weights[16] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float inverses[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
    static uint8_t bin[512];
    size_t at = 0;
    size_t offsets[8], lengths[8];
#define PUT(i, p, n) do { offsets[i] = at; lengths[i] = n; memcpy(bin + at, p, n); at += ((size_t)(n) + 3) & ~(size_t)3; } while (0)
    PUT(0, times, 8); PUT(1, moves, 24); PUT(2, turns, 32); PUT(3, positions, 48); PUT(4, indices, 12); PUT(5, joints, 16); PUT(6, weights, 64); PUT(7, inverses, 128);
#undef PUT
    char views[900];
    int v = 0;
    for (int i = 0; i < 8; ++i) v += snprintf(views + v, sizeof views - (size_t)v, "%s{\"buffer\":0,\"byteOffset\":%d,\"byteLength\":%d}", i ? "," : "", (int)offsets[i], (int)lengths[i]);
    char json[3000];
    int n = snprintf(json, sizeof json,
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0,1,3]}],"
        "\"nodes\":[{\"mesh\":0,\"translation\":[0,0,0]},{\"children\":[2]},{\"name\":\"elbow\",\"translation\":[0,1,0]},{\"mesh\":0,\"skin\":0}],"
        "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":7}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":3,\"JOINTS_0\":5,\"WEIGHTS_0\":6},\"indices\":4}]}],"
        "\"animations\":[{\"name\":\"go\",\"samplers\":[{\"input\":0,\"output\":1},{\"input\":0,\"output\":2}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},{\"sampler\":1,\"target\":{\"node\":2,\"path\":\"rotation\"}}]}],"
        "\"buffers\":[{\"byteLength\":%d}],\"bufferViews\":[%s],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"},{\"bufferView\":3,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":4,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"},{\"bufferView\":5,\"componentType\":5121,\"count\":4,\"type\":\"VEC4\"},"
        "{\"bufferView\":6,\"componentType\":5126,\"count\":4,\"type\":\"VEC4\"},{\"bufferView\":7,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"}]}",
        (int)at, views);
    while (n & 3) json[n++] = ' ';
    memcpy(out, "glTF", 4);
    put32le(out + 4, 2);
    put32le(out + 8, (uint32_t)(12 + 8 + n + 8) + (uint32_t)at);
    put32le(out + 12, (uint32_t)n);
    put32le(out + 16, 0x4E4F534Au);
    memcpy(out + 20, json, (size_t)n);
    uint8_t* chunk = out + 20 + n;
    put32le(chunk, (uint32_t)at);
    put32le(chunk + 4, 0x004E4942u);
    memcpy(chunk + 8, bin, at);
    return (size_t)(20 + n + 8) + at;
}

void testClips(void) {
    static uint8_t glb[8192];
    const size_t size = makeAnimatedGlb(glb);
    ArtfScene* s = plainScene();
    const ArtfModel m = artfSceneLoadModelMemory(s, glb, size, NULL, NULL);
    CHECK(m != 0);
    if (!m) { artfSceneFree(s); return; }
    CHECK(artfModelClipCount(s, m) == 1);
    CHECK(!strcmp(artfModelClipName(s, m, 0), "go"));
    CHECK(artfModelClipDuration(s, m, 0) == 2.f);
    CHECK(artfModelInstanceCount(s, m) == 2);
    /* a rigid part moves with its node; time goes round */
    artfAnimationSet(s, m, 0, .5f);
    CHECK(fabsf(s->instances.items[0].m[3] - .5f) < 1e-4f);
    artfAnimationSet(s, m, 0, 2.5f);
    CHECK(fabsf(s->instances.items[0].m[3] - .5f) < 1e-4f);
    /* a skinned part's top goes with its bone: from (x, 2) to around the joint at (0, 1): a quarter turn */
    const uint32_t bent = s->instances.items[1].mesh;
    artfAnimationSet(s, m, 0, 1.9999f);
    artfScenePosesApply(s);
    const ArtfMeshData* mesh = &s->meshes.items[bent];
    ArtfVertex v[4];
    artfMeshUnpack(mesh, v);
    float leftmost = 9;
    for (int i = 0; i < 4; ++i) leftmost = artfMinf(leftmost, v[i].px);
    CHECK(fabsf(leftmost + 1.f) < .02f);
    /* clips mixed: by their weights; only below a node; one of weight 1 is the clip */
    const ArtfClipMix one = {0, .5f, 1, NULL};
    CHECK(artfAnimationMix(s, m, &one, 1));
    CHECK(fabsf(s->instances.items[0].m[3] - .5f) < 1e-4f);
    const ArtfClipMix halves[2] = {{0, .5f, 1, NULL}, {0, 1.5f, 1, NULL}};
    CHECK(artfAnimationMix(s, m, halves, 2));
    CHECK(fabsf(s->instances.items[0].m[3] - 1.f) < 1e-4f);
    const ArtfClipMix weighed[2] = {{0, .5f, 3, NULL}, {0, 1.5f, 1, NULL}};
    CHECK(artfAnimationMix(s, m, weighed, 2));
    CHECK(fabsf(s->instances.items[0].m[3] - .75f) < 1e-4f);
    /* the second clip only moves the elbow: the part on node 0 is where the first puts it, and the arm halfway */
    const ArtfClipMix layered[2] = {{0, .5f, 1, NULL}, {0, 1.9999f, 1, "elbow"}};
    CHECK(artfAnimationMix(s, m, layered, 2));
    CHECK(fabsf(s->instances.items[0].m[3] - .5f) < 1e-4f);
    artfScenePosesApply(s);
    artfMeshUnpack(&s->meshes.items[bent], v);
    float mixedLeft = 9;
    for (int i = 0; i < 4; ++i) mixedLeft = artfMinf(mixedLeft, v[i].px);
    artfAnimationSet(s, m, 0, .5f);
    artfScenePosesApply(s);
    artfMeshUnpack(&s->meshes.items[bent], v);
    float plainLeft = 9;
    for (int i = 0; i < 4; ++i) plainLeft = artfMinf(plainLeft, v[i].px);
    CHECK(mixedLeft < plainLeft - .05f); /* (turned further: -1.11 against -.84, as its corner swings past the joint) */
    /* a matching clip in a scene with no clip asked for stays as it is */
    artfSetLog(countLog, &logged);
    logged = 0;
    artfAnimationSet(s, m, 9, 0);
    artfAnimationSet(s, 99, 0, 0);
    const ArtfClipMix noClip = {9, 0, 1, NULL}, noNode = {0, 0, 1, "wrist"};
    CHECK(!artfAnimationMix(s, m, &noClip, 1) && !artfAnimationMix(s, m, &noNode, 1));
    CHECK(logged == 4);
    artfSetLog(NULL, NULL);
    const ArtfTransform moved = artfTransformAt(5, 0, 0, 1);
    artfModelSetTransform(s, m, &moved);
    CHECK(s->instances.items[0].m[3] > 5.f);
    /* the file broken anywhere: no crash */
    static uint8_t bad[8192];
    for (size_t i = 0; i < size; i += 5) {
        memcpy(bad, glb, size);
        bad[i] ^= 0x5A;
        artfSetLog(countLog, &logged);
        ArtfScene* t = artfSceneNew();
        const ArtfModel made = artfSceneLoadModelMemory(t, bad, size, NULL, NULL);
        if (made && artfModelClipCount(t, made)) artfAnimationSet(t, made, 0, 1.f);
        artfSceneFree(t);
        artfSetLog(NULL, NULL);
    }
    artfSceneFree(s);
}
