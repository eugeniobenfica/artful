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

/* (Without Vulkan, what follows has nothing to run on: its switches are Vulkan's, and the tests are empty.) */
#ifdef ARTFUL_VULKAN
/* A device lost in the middle of it all: false, and false after that; the renderer can be freed; and the scene
 * goes to a new one, which draws what the first did. */
void testLostDevice(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    drawInto(r, s, a);
    artfVulkanSubmitsLeft = 2;
    CHECK(artfRendererDraw(r, s) && artfRendererDraw(r, s));
    CHECK(!artfRendererDraw(r, s));
    artfVulkanSubmitsLeft = -1;
    CHECK(!artfRendererDraw(r, s));
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
    if (!window) return;
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
void testRefusedSwapchain(void) {}
#endif

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
void testLostDevice(void) {}
void testRefusedSwapchain(void) {}
void testShadersAhead(void) {}
void testPipelinesAhead(void) {}
#endif
