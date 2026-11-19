/* artful-tests: what the tests share (tests.h). */
#include "tests.h"

int checks = 0, failures = 0;

uint64_t hashOf(const uint32_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

ArtfRendererOptions options(ArtfBackend backend, int threads, bool portable) {
    ArtfRendererOptions o;
    memset(&o, 0, sizeof o);
    o.backend = backend;
    o.width = W;
    o.height = H;
    o.threads = threads;
    o.simd = portable ? ARTF_SIMD_PORTABLE : ARTF_SIMD_AUTO;
    return o;
}

void setCamera(ArtfScene* s, float x, float y, float z, float tx, float ty, float tz) {
    ArtfCamera c = {artfVec3(x, y, z), artfVec3(tx, ty, tz), artfVec3(0, 1, 0), 1.f, .1f};
    artfSceneSetCamera(s, &c);
}

/* the same scene as a test of everything at once: texture, lights, overlapping objects */
ArtfScene* richScene(void) {
    ArtfScene* s = artfSceneNew();
    static uint32_t px[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) px[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0xFFD0C8B0u : 0xFF404858u;
    ArtfMaterialDesc floorDesc = {artfVec3(.9f, .9f, .9f), artfSceneAddTexture(s, px, 64, 64, 0), .2f, .33f, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 8), artfSceneAddMaterial(s, &floorDesc), &t);
    ArtfMaterialDesc red = {artfVec3(.8f, .15f, .1f), 0, .7f, .17f, artfVec3(0, 0, 0)};
    ArtfMaterialDesc glow = {artfVec3(.1f, .3f, .2f), 0, 0, 1, artfVec3(.05f, .2f, .1f)};
    ArtfMesh sphere = artfSceneAddSphere(s, 1, 32, 48), torus = artfSceneAddTorus(s, 1, .3f, 48, 24), cube = artfSceneAddCube(s, 1.4f);
    t = artfTransformAt(-1.8f, 0, 0, 1);
    artfSceneAddInstance(s, sphere, artfSceneAddMaterial(s, &red), &t);
    t = artfTransformTrs(artfVec3(.4f, 0, .3f), artfQuatAxisAngle(artfVec3(1, 0, .3f), 1.f), artfVec3(1.1f, .8f, 1.3f)); /* stretched */
    artfSceneAddInstance(s, torus, artfSceneAddMaterial(s, &glow), &t);
    t = artfTransformTrs(artfVec3(2, 0, -.5f), artfQuatAxisAngle(artfVec3(0, 1, 0), .5f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, cube, 0, &t);
    artfSceneAddLight(s, artfVec3(0, 1.5f, 2.5f), artfVec3(.2f, .5f, 1.f), 6.f);
    artfSceneAddLight(s, artfVec3(-3, .5f, 1), artfVec3(1.f, .3f, .1f), 3.f);
    setCamera(s, 0, 2, 6, 0, 0, 0);
    return s;
}

/* How far apart two pictures are: a GPU rounds and samples in its own way, and its edges fall on
 * slightly other pixels, so what is compared is how many pixels differ by much. */
double differentShare(const uint32_t* a, const uint32_t* b, int threshold) {
    int different = 0;
    for (int i = 0; i < W * H; ++i) {
        int worst = 0;
        for (int c = 0; c < 24; c += 8) worst = artfMaxi(worst, abs((int)(a[i] >> c & 255) - (int)(b[i] >> c & 255)));
        different += worst > threshold;
    }
    const double share = (double)different / (W * H);
    flipRecord(a, b, threshold, share);
    return share;
}

/* FLIP beside the share, for every comparison: written to ARTFUL_FLIP_LOG, to set thresholds by it later. */
static FILE* flipLog;
static bool flipLogOpened;
static int flipCompared;

void flipRecord(const uint32_t* a, const uint32_t* b, int threshold, double share) {
    if (!flipLogOpened) {
        flipLogOpened = true;
        const char* path = getenv("ARTFUL_FLIP_LOG");
        if (path && *path) {
            flipLog = fopen(path, "a");
            if (flipLog && ftell(flipLog) == 0) fputs("test,threshold,share,flipMean,flipP99,flipMax\n", flipLog);
        }
    }
    if (!flipLog) return;
    const FlipStats f = flipFrames(a, b, W, H, 0, NULL);
    fprintf(flipLog, "%s,%d,%.6f,%.6f,%.6f,%.6f\n", currentTest(), threshold, share, f.mean, f.p99, f.max);
    ++flipCompared;
}

void flipSummary(void) {
    if (!flipLog) return;
    printf("FLIP: %d comparisons written to %s\n", flipCompared, getenv("ARTFUL_FLIP_LOG"));
    fclose(flipLog);
    flipLog = NULL;
}

int shownByte(double light) {
    light = light < 0 ? 0 : light > 1 ? 1 : light;
    const double shown = light <= .0031308 ? light * 12.92 : 1.055 * pow(light, 1 / 2.4) - .055;
    return (int)(shown * 255.0 + .5);
}

/* Light that gives a surface's own color back: what is drawn is what was asked for. */
ArtfScene* plainScene(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(1, 1, 1), artfVec3(1, 1, 1));
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    setCamera(s, 0, 0, 5, 0, 0, 0);
    return s;
}

ArtfMaterial whiteMaterial(ArtfScene* s) {
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    return artfSceneAddMaterial(s, &d);
}

/* A square of side 2 in the xy plane, facing the eye; its corners from the bottom left, going round. */
ArtfMesh addSquare(ArtfScene* s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    const ArtfVertex v[4] = {{-1, -1, 0, 0, 0, 1, 0, 0, a}, {1, -1, 0, 0, 0, 1, 1, 0, b}, {1, 1, 0, 0, 0, 1, 1, 1, c}, {-1, 1, 0, 0, 0, 1, 0, 1, d}};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    return artfSceneAddMesh(s, v, 4, idx, 6);
}

ArtfInstance addAt(ArtfScene* s, ArtfMesh mesh, ArtfMaterial material, float x, float y, float z, float scale) {
    const ArtfTransform t = artfTransformAt(x, y, z, scale);
    return artfSceneAddInstance(s, mesh, material, &t);
}

void drawInto(ArtfRenderer* r, ArtfScene* s, uint32_t* frame) {
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
}

bool near_(int a, int b, int margin) { return abs(a - b) <= margin; }

/* A scene of everything at once, for what must not change with how it is drawn. */
ArtfScene* layeredScene(void) {
    ArtfScene* s = richScene();
    ArtfMaterial white = whiteMaterial(s);
    ArtfInstance glass = addAt(s, artfSceneAddSphere(s, 1, 24, 32), white, 1.f, .4f, 1.5f, .9f);
    artfInstanceSetOpacity(s, glass, .45f);
    ArtfInstance fade = addAt(s, artfSceneAddTorus(s, 1, .3f, 32, 16), white, -1.2f, .8f, 1.5f, .7f);
    artfInstanceSetOpacity(s, fade, .7f);
    addAt(s, addSquare(s, 0x60FF2000u, 0xC020FF00u, 0xFF0020FFu, 0x30FFFF00u), white, 0, -.2f, 2.5f, .8f);
    addAt(s, addSquare(s, 0xFFFF2000u, 0xFF20FF00u, 0xFF0020FFu, 0xFFFFFF00u), white, 1.8f, 1.4f, 0, .6f);
    /* a fence, with holes and a back */
    static uint32_t bars[32 * 32];
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) bars[y * 32 + x] = (x / 4 + y / 4) & 1 ? 0xFFC08040u : 0x00FFFFFFu;
    ArtfMaterialDesc fence = {artfVec3(1, 1, 1), artfSceneAddTexture(s, bars, 32, 32, 0), .3f, .33f, artfVec3(0, 0, 0), ARTF_ALPHA_MASK, .5f, 0, true};
    const ArtfTransform t = artfTransformTrs(artfVec3(-2.2f, 0, 1), artfQuatAxisAngle(artfVec3(0, 1, 0), 2.6f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &fence), &t);
    return s;
}

int logged;
void countLog(void* user, const char* message) { (void)message; ++*(int*)user; }

ArtfTexture oneColor(ArtfScene* s, uint32_t bgra) {
    static uint32_t px[4 * 4];
    for (int i = 0; i < 16; ++i) px[i] = bgra;
    return artfSceneAddTexture(s, px, 4, 4, 0);
}

/* A floor, a ball over it and a wall of bars, as a test of what maps can get wrong. */
ArtfScene* shadowScene(void) {
    ArtfScene* s = artfSceneNew();
    setCamera(s, 2, 5, 9, 0, 0, 0);
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    artfSceneSetSun(s, artfVec3(-.5f, -1, -.3f), artfVec3(.9f, .9f, .9f));
    ArtfMaterialDesc matte = {artfVec3(.8f, .8f, .8f), 0, 0, 1, artfVec3(0, 0, 0)};
    const ArtfMaterial m = artfSceneAddMaterial(s, &matte);
    ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 24, 1), m, &t);
    addAt(s, artfSceneAddSphere(s, 1, 32, 48), m, 0, .6f, 0, 1);
    addAt(s, artfSceneAddCube(s, 1.2f), m, -2.5f, 0, 1, 1);
    return s;
}

/* The program's own C function, as a material is meant to be written: stripes across u. */
ArtfExpr stripes(ArtfGraph* g, float count) {
    ArtfExpr x = artfExprFract(g, artfExprMul(g, artfExprX(g, artfExprUv(g)), artfExprNumber(g, count)));
    return artfExprIf(g, artfExprSub(g, x, artfExprNumber(g, .5f)), artfExprConst(g, 1, 1, 1, 1), artfExprConst(g, 0, 0, 0, 1));
}

ArtfScene* bleedScene(void) {
    ArtfScene* s = plainScene();
    const ArtfMaterial white = whiteMaterial(s);
    artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(.5f, .5f, .5f));
    artfSceneSetAmbient(s, artfVec3(.05f, .05f, .05f), artfVec3(.05f, .05f, .05f));
    setCamera(s, 0, 3, 7, 0, 0, 0);
    ArtfMaterialDesc redWall = {artfVec3(1, .02f, .02f), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 40, 1), white, &t);
    const ArtfTransform wall = artfTransformTrs(artfVec3(2.2f, 1, 0), artfQuatAxisAngle(artfVec3(0, 1, 0), -1.5708f), artfVec3(4, 2, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &redWall), &wall);
    const ArtfMesh ball = artfSceneAddSphere(s, .8f, 24, 32);
    ArtfMaterialDesc mirrorDesc = artfMaterialDefaults();
    mirrorDesc.baseColor = artfVec3(.9f, .9f, .9f);
    mirrorDesc.metallic = 1;
    mirrorDesc.roughness = .1f;
    addAt(s, ball, artfSceneAddMaterial(s, &mirrorDesc), -1, -.2f, 0, 1);
    return s;
}

ArtfScene* bulbScene(float size) {
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(.02f, .02f, .02f), artfVec3(.02f, .02f, .02f));
    setCamera(s, 0, 2.5f, 5, 0, 0, 0);
    ArtfMaterialDesc smooth = artfMaterialDefaults();
    smooth.baseColor = artfVec3(.2f, .2f, .25f);
    smooth.roughness = .08f;
    const ArtfTransform floor = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 20, 1), artfSceneAddMaterial(s, &smooth), &floor);
    addAt(s, artfSceneAddCube(s, .6f), whiteMaterial(s), 0, .1f, 0, 1);
    const ArtfLight bulb = artfSceneAddLight(s, artfVec3(0, 1.6f, -.6f), artfVec3(1.5f, 1.5f, 1.5f), 12);
    artfLightSetSize(s, bulb, size);
    return s;
}
int greyCount(const uint32_t* f) {
    int n = 0;
    for (int i = 0; i < W * H; ++i) n += CHANNEL(f[i], 8) > 25 && CHANNEL(f[i], 8) < 99; /* (light from .0096 to .125) */
    return n;
}

ArtfScene* movingScene(ArtfInstance* ball) {
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(-.3f, -1, -.4f), artfVec3(1.5f, 1.5f, 1.5f));
    setCamera(s, 0, 0, 6, 0, 0, 0);
    ArtfMaterialDesc red = artfMaterialDefaults();
    red.baseColor = artfVec3(.9f, .1f, .1f);
    *ball = addAt(s, artfSceneAddSphere(s, .8f, 32, 48), artfSceneAddMaterial(s, &red), -1.2f, 0, 0, 1);
    return s;
}

/* One frame, and what it shows: drawInto draws three, and what moved has stopped by the last. */
void drawOnce(ArtfRenderer* r, ArtfScene* s, uint32_t* frame) {
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
}

void placeBall(ArtfScene* s, ArtfInstance ball, float x) {
    const ArtfTransform at = artfTransformAt(x, 0, 0, 1);
    artfInstanceSetTransform(s, ball, &at);
}

/* The mean difference of a channel, over the frame. */
double meanApart(const uint32_t* a, const uint32_t* b) {
    double sum = 0;
    for (int i = 0; i < W * H; ++i) sum += abs(CHANNEL(a[i], 16) - CHANNEL(b[i], 16));
    return sum / (W * H);
}

/* Lighter levels, a layer, lights past the frame's sixteen and something that moves: what the views tell apart. */
ArtfScene* viewScene(ArtfInstance* moving) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    ArtfMaterialDesc floorDesc = {artfVec3(.6f, .6f, .6f), 0, .2f, .5f, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 40, 8), artfSceneAddMaterial(s, &floorDesc), &t);
    ArtfMaterialDesc red = {artfVec3(.8f, .15f, .1f), 0, .7f, .3f, artfVec3(0, 0, 0)};
    red.metallic = .6f;
    ArtfMaterialDesc glow = {artfVec3(.1f, .3f, .2f), 0, 0, 1, artfVec3(.05f, .4f, .1f)};
    const ArtfMaterial redMaterial = artfSceneAddMaterial(s, &red), glowMaterial = artfSceneAddMaterial(s, &glow);
    const ArtfMesh sphere = artfSceneAddSphere(s, 1, 48, 64);
    for (int i = 0; i < 5; ++i) addAt(s, sphere, i & 1 ? glowMaterial : redMaterial, -3.f + (float)i * 1.6f, 0, -(float)i * 6.f, 1);
    *moving = addAt(s, artfSceneAddTorus(s, .7f, .25f, 48, 24), glowMaterial, 1.5f, .3f, 2.f, 1);
    artfInstanceSetOpacity(s, addAt(s, artfSceneAddCube(s, 1.2f), redMaterial, -1.2f, 0, 2.2f, 1), .5f);
    for (int i = 0; i < 24; ++i) artfSceneAddLight(s, artfVec3(-6.f + (float)i * .5f, -.5f, 1.f - (float)(i % 5) * 3.f), artfVec3(.4f, .3f, .2f), 2.5f);
    setCamera(s, 0, 1.5f, 7, 0, 0, -2);
    return s;
}

int litCount(const uint32_t* f) {
    int n = 0;
    for (int i = 0; i < W * H; ++i) n += (f[i] & 0xFFFFFF) != 0;
    return n;
}

/* Some frames, the last read back. */
void drawFrames(ArtfRenderer* r, ArtfScene* s, int n, uint32_t* frame) {
    for (int i = 0; i < n; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
}

/* A strip standing up, of two bones: what is below y = 1 stays with the first, what is above goes with the second. */
ArtfMesh addStrip(ArtfScene* s) {
    enum { ROWS = 12 };
    static ArtfVertex v[(ROWS + 1) * 2];
    static ArtfVertexSkin k[(ROWS + 1) * 2];
    static uint32_t idx[ROWS * 6];
    for (int r = 0; r <= ROWS; ++r)
        for (int c = 0; c < 2; ++c) {
            const float y = (float)r / ROWS * 2.f;
            const ArtfVertex q = {c ? .3f : -.3f, y, 0, 0, 0, 1, 0, 0, 0};
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
    return artfSceneAddSkinnedMesh(s, v, (ROWS + 1) * 2, idx, n, k);
}

/* The second bone bent around (0, 1, 0), by this much around z. */
void bendStrip(ArtfScene* s, ArtfInstance i, float angle) {
    const ArtfTransform turn = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 0, 1), angle), artfVec3(1, 1, 1));
    ArtfTransform joints[2] = {artfTransformIdentity(), turn};
    /* about the joint: move it to the origin, turn, and back */
    const float* m = turn.m;
    joints[1].m[3] = -m[1];
    joints[1].m[7] = 1.f - m[5];
    artfInstanceSetJoints(s, i, joints, 2);
}

void put32le(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

void drawSamples(ArtfRenderer* r, ArtfScene* s, int samples, uint32_t* frame) {
    for (int i = 0; i < samples; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
}

/* A mean of a little square of pixels around (x, y), for what is noisy. */
double meanOf(const uint32_t* frame, int x, int y, int shift) {
    double sum = 0;
    for (int j = -3; j <= 3; ++j)
        for (int i = -3; i <= 3; ++i) sum += CHANNEL(frame[(size_t)(y + j) * W + x + i], shift);
    return sum / 49;
}

/* A Vulkan renderer as on a device with no ray tracing hardware. */
ArtfRenderer* gpuWithoutRayQueries(const ArtfRendererOptions* o) {
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

/* (the library's own, under what tests.h puts in their place) */
#undef artfRendererNew
#undef artfRenderer2DNew

ArtfRenderer* testRendererNew(const ArtfRendererOptions* o) {
    ArtfRenderer* r = artfRendererNew(o);
    bool refused = false; /* (a test that makes Vulkan refuse on purpose) */
#ifdef ARTFUL_VULKAN
    refused = artfVulkanRefuseSwapchain;
#endif
    if (!r && o->backend == ARTF_BACKEND_VULKAN && !refused) missing("vulkan");
    return r;
}

ArtfRenderer2D* testRenderer2DNew(const ArtfRenderer2DOptions* o) {
    ArtfRenderer2D* r = artfRenderer2DNew(o);
    if (!r && o->backend == ARTF_BACKEND2D_VULKAN) missing("vulkan");
    return r;
}
