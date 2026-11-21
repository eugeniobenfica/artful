/* artful-tests: what things are made of: materials, materials of your own, lights with a size and a shape. */
#include "tests.h"

/* ---- what things are made of ---- */

/* A square with v going down the picture, as a file's textures have it. */
static ArtfMesh addPictureSquare(ArtfScene* s) {
    const ArtfVertex v[4] = {{-1, -1, 0, 0, 0, 1, 0, 1, 0}, {1, -1, 0, 0, 0, 1, 1, 1, 0}, {1, 1, 0, 0, 0, 1, 1, 0, 0}, {-1, 1, 0, 0, 0, 1, 0, 0, 0}};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    return artfSceneAddMesh(s, v, 4, idx, 6);
}

/* Draws, and gives the middle's pixel: by triangles, and then by rays. */
static uint32_t middleOf(ArtfRenderer* r, ArtfScene* s, uint32_t* frame, bool traced) {
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 8, 1};
    artfSceneSetTrace(s, traced ? &rays : NULL);
    for (int i = 0; i < 9; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
    return frame[(size_t)(H / 2) * W + W / 2];
}

void testPhysical(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    for (int traced = 0; traced < 2; ++traced) {
        /* a metal in a white sky, seen head on, shows its own color */
        ArtfScene* s = plainScene();
        ArtfMaterialDesc d = artfMaterialDefaults();
        d.baseColor = artfVec3(1, .5f, .25f);
        d.metallic = 1.f;
        d.roughness = .2f;
        ArtfMaterial metal = artfSceneAddMaterial(s, &d);
        addAt(s, addSquare(s, 0, 0, 0, 0), metal, 0, 0, 0, 1);
        uint32_t p = middleOf(r, s, a, traced);
        CHECK(near_(CHANNEL(p, 16), 255, 3) && near_(CHANNEL(p, 8), shownByte(.5), 3) && near_(CHANNEL(p, 0), shownByte(.25), 3));
        /* and what is not metal, and black, reflects 4% of it */
        d.baseColor = artfVec3(0, 0, 0);
        d.metallic = 0;
        artfMaterialSet(s, metal, &d);
        p = middleOf(r, s, a, traced);
        CHECK(near_(CHANNEL(p, 8), shownByte(.04), 3));
        artfSceneFree(s);

        /* a highlight, on black: the smoother, the brighter its peak */
        int peaks[2];
        for (int smooth = 0; smooth < 2; ++smooth) {
            s = plainScene();
            artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
            artfSceneSetSun(s, artfVec3(0, 0, -1), artfVec3(1, 1, 1));
            d = artfMaterialDefaults();
            d.baseColor = artfVec3(0, 0, 0);
            d.roughness = smooth ? .2f : .7f;
            addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
            peaks[smooth] = CHANNEL(middleOf(r, s, a, traced), 8);
            artfSceneFree(s);
        }
        CHECK(peaks[1] > peaks[0] + 30);

        /* a normal map: a plain square, with the sun above it, tells up from down */
        int bent[3];
        const uint32_t maps[3] = {0xFF8080FFu, 0xFF80FFFFu, 0xFF8000FFu}; /* flat; +y, up the picture; -y */
        for (int i = 0; i < 3; ++i) {
            s = plainScene();
            artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
            artfSceneSetSun(s, artfVec3(0, -1, -.2f), artfVec3(1, 1, 1));
            d = artfMaterialDefaults();
            d.specular = 0;
            d.normalMap = oneColor(s, maps[i]);
            addAt(s, addPictureSquare(s), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
            bent[i] = CHANNEL(middleOf(r, s, a, traced), 8);
            artfSceneFree(s);
        }
        CHECK(bent[1] > bent[0] + 40);
        CHECK(bent[2] < bent[0]);

        /* a surface map: blue says metal, and green multiplies roughness */
        s = plainScene();
        d = artfMaterialDefaults();
        d.baseColor = artfVec3(1, .5f, .25f);
        d.metallic = 1.f;
        d.surfaceMap = oneColor(s, 0xFF000000u); /* (no blue: not metal) */
        addAt(s, addPictureSquare(s), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
        p = middleOf(r, s, a, traced);
        CHECK(CHANNEL(p, 16) > 240); /* (the same color, now diffuse) */
        artfSceneFree(s);

        /* an occlusion map keeps light from all around out, and not the sun's */
        s = plainScene();
        d = artfMaterialDefaults();
        d.specular = 0;
        d.occlusionMap = oneColor(s, 0xFF400000u); /* (a quarter open) */
        addAt(s, addPictureSquare(s), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
        p = middleOf(r, s, a, traced);
        CHECK(near_(CHANNEL(p, 8), shownByte(.25), 4));
        artfSceneFree(s);
    }
    artfFree(a);
    artfRendererFree(r);
}

static ArtfMaterial graphMaterial(ArtfScene* s, ArtfGraph* g, ArtfAlphaMode mode) {
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    d.alphaMode = mode;
    d.graph = artfSceneAddGraph(s, g);
    artfGraphFree(g);
    return artfSceneAddMaterial(s, &d);
}

void testGraphs(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    for (int traced = 0; traced < 2; ++traced) {
        /* a color by position on the surface: drawn, and traced, stripes where they should be */
        ArtfScene* s = plainScene();
        ArtfGraph* g = artfGraphNew();
        artfGraphSetOutput(g, ARTF_OUT_COLOR, stripes(g, 2.f));
        addAt(s, addSquare(s, 0, 0, 0, 0), graphMaterial(s, g, ARTF_ALPHA_OPAQUE), 0, 0, 0, 2);
        (void)middleOf(r, s, a, traced);
        /* the square spans 146 pixels: at a quarter of u it is white, at three eighths still, past a half black */
        const size_t row = (size_t)(H / 2) * W;
        CHECK(CHANNEL(a[row + W / 2 - 73 + 26], 8) == 0 && CHANNEL(a[row + W / 2 - 73 + 55], 8) > 240 && CHANNEL(a[row + W / 2 - 73 + 100], 8) == 0);
        artfSceneFree(s);

        /* a parameter: changes what is drawn, with no new shader */
        s = plainScene();
        g = artfGraphNew();
        artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprConst(g, 0, 0, 0, 1));
        artfGraphSetOutput(g, ARTF_OUT_EMISSIVE, artfExprParam(g, 1));
        ArtfMaterial m = graphMaterial(s, g, ARTF_ALPHA_OPAQUE);
        addAt(s, addSquare(s, 0, 0, 0, 0), m, 0, 0, 0, 1);
        artfMaterialSetParam(s, m, 1, .25f, 0, 0, 1);
        uint32_t p = middleOf(r, s, a, traced);
        CHECK(near_(CHANNEL(p, 16), shownByte(.25), 3) && CHANNEL(p, 8) == 0);
        artfMaterialSetParam(s, m, 1, 0, 1, 0, 1);
        p = middleOf(r, s, a, traced);
        CHECK(CHANNEL(p, 16) == 0 && CHANNEL(p, 8) > 250);
        artfSceneFree(s);

        /* sine and cosine agree with the library's, near enough */
        s = plainScene();
        g = artfGraphNew();
        ArtfExpr e = artfExprSin(g, artfExprNumber(g, 2.f));
        ArtfExpr c = artfExprCos(g, artfExprNumber(g, 2.f));
        artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprConst(g, 0, 0, 0, 1));
        artfGraphSetOutput(g, ARTF_OUT_EMISSIVE, artfExprVec(g, e, artfExprMul(g, c, c), artfExprNumber(g, 0)));
        addAt(s, addSquare(s, 0, 0, 0, 0), graphMaterial(s, g, ARTF_ALPHA_OPAQUE), 0, 0, 0, 1);
        p = middleOf(r, s, a, traced);
        CHECK(near_(CHANNEL(p, 16), shownByte(sinf(2.f)), 3) && near_(CHANNEL(p, 8), shownByte(cosf(2.f) * cosf(2.f)), 3));
        artfSceneFree(s);

        /* alpha, roughness and metal, and a texture */
        s = plainScene();
        g = artfGraphNew();
        artfGraphSetOutput(g, ARTF_OUT_ALPHA, artfExprNumber(g, .5f));
        artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprTexture(g, oneColor(s, 0xFF808080u), artfExprUv(g)));
        addAt(s, addSquare(s, 0, 0, 0, 0), graphMaterial(s, g, ARTF_ALPHA_BLEND), 0, 0, 0, 1);
        const ArtfTraceDesc many = {ARTF_TRACE_RAYS, 1, 64, 1};
        artfSceneSetTrace(s, traced ? &many : NULL);
        for (int i = 0; i < (traced ? 64 : 3); ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        CHECK(near_((int)meanOf(a, W / 2, H / 2, 8), 64, 12));
        artfSceneFree(s);
        (void)mid;
    }
    artfFree(a);
    artfRendererFree(r);
}

/* One definition, on both backends and on all kernels. */
void testGraphsBothBackends(void) {
    ArtfScene* s = shadowScene();
    ArtfGraph* g = artfGraphNew();
    ArtfExpr p = artfExprPosition(g);
    ArtfExpr wave = artfExprSin(g, artfExprAdd(g, artfExprMul(g, artfExprX(g, p), artfExprNumber(g, 3)), artfExprParam(g, 0)));
    artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprMix(g, artfExprConst(g, .1f, .2f, .8f, 1), artfExprConst(g, .9f, .7f, .1f, 1), artfExprAdd(g, artfExprMul(g, wave, artfExprNumber(g, .5f)), artfExprNumber(g, .5f))));
    artfGraphSetOutput(g, ARTF_OUT_ROUGHNESS, artfExprFract(g, artfExprZ(g, p)));
    ArtfMaterial m = graphMaterial(s, g, ARTF_ALPHA_OPAQUE);
    artfInstanceSetMaterial(s, 1, m);
    artfMaterialSetParam(s, m, 0, 1.f, 0, 0, 0);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {3, ARTF_SIMD_128}};
    uint64_t reference = 0;
    for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
        o.simd = runs[i].simd;
        ArtfRenderer* r = artfRendererNew(&o);
        drawInto(r, s, b);
        const uint64_t h = hashOf(b, (size_t)W * H);
        if (!i) reference = h;
        CHECK(h == reference);
        artfRendererFree(r);
    }
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawInto(gpu, s, a);
        double far = differentShare(a, b, 24);
        printf("a material in C, pixels that differ by more than 24 of 255: %.2f%%\n", far * 100);
        CHECK(far < .02);
        /* a graph added later: new shaders, and the old ones go on being right */
        ArtfGraph* later = artfGraphNew();
        artfGraphSetOutput(later, ARTF_OUT_COLOR, artfExprConst(later, 1, 0, 0, 1));
        artfInstanceSetMaterial(s, 2, graphMaterial(s, later, ARTF_ALPHA_OPAQUE));
        drawInto(gpu, s, a);
        ArtfRendererOptions c = options(ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* cpu = artfRendererNew(&c);
        drawInto(cpu, s, b);
        CHECK(differentShare(a, b, 24) < .02);
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    artfSceneFree(s);
}

/* A clear coat and sheen: each changes the picture, where it should; the two backends agree, drawing and tracing; and
 * glTF's extensions reach them. */
static ArtfScene* coatScene(float coat, float sheen) {
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(-.5f, -1, -.6f), artfVec3(2, 2, 2));
    artfSceneSetAmbient(s, artfVec3(.2f, .25f, .3f), artfVec3(.05f, .05f, .05f));
    setCamera(s, 0, .5f, 4, 0, 0, 0);
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.5f, .05f, .05f);
    d.roughness = .8f;
    d.clearcoat = coat;
    d.clearcoatRoughness = .1f;
    d.sheen = artfVec3(sheen, sheen, sheen);
    d.sheenRoughness = .5f;
    addAt(s, artfSceneAddSphere(s, 1.2f, 48, 64), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
    return s;
}

void testCoat(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* plain = coatScene(0, 0);
        ArtfScene* coated = coatScene(1, 0);
        ArtfScene* velvet = coatScene(0, 1);
        drawInto(r, plain, a);
        drawInto(r, coated, b);
        /* a sharp highlight where there was a dull one */
        int brightest[2] = {0, 0};
        for (int i = 0; i < W * H; ++i) {
            brightest[0] = artfMaxi(brightest[0], CHANNEL(a[i], 8));
            brightest[1] = artfMaxi(brightest[1], CHANNEL(b[i], 8));
        }
        printf("%s, a clear coat: its highlight's green %d, %d without\n", backend ? "GPU" : "CPU", brightest[1], brightest[0]);
        CHECK(brightest[1] > brightest[0] + 60 && differentShare(a, b, 2) > .05);
        /* sheen: the rim, not the middle */
        drawInto(r, velvet, b);
        const int rimX = W / 2 + (int)(H * .28f), midX = W / 2;
        const double rim = meanOf(b, rimX, H / 2, 16) - meanOf(a, rimX, H / 2, 16), middle = meanOf(b, midX, H / 2, 16) - meanOf(a, midX, H / 2, 16);
        printf("%s, sheen: the rim %.1f brighter, the middle %.1f\n", backend ? "GPU" : "CPU", rim, middle);
        CHECK(rim > 8 && rim > middle * 2);
        artfSceneFree(plain);
        artfSceneFree(coated);
        artfSceneFree(velvet);
        artfRendererFree(r);
    }
    /* both at once, on both backends: drawn, and traced */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        o.backend = ARTF_BACKEND_SOFTWARE;
        ArtfRenderer* cpu = artfRendererNew(&o);
        ArtfScene* s = coatScene(.8f, .6f);
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("a clear coat and sheen, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0111); /* (measured 0.0011 on a Radeon RX 9060 XT) */
        CHECK(apart < .005);
        ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 32, 0};
        artfSceneSetTrace(s, &rays);
        for (int k = 0; k < 32; ++k) drawInto(cpu, s, a);
        for (int k = 0; k < 32; ++k) drawInto(gpu, s, b);
        const double tracedApart = differentShare(a, b, 24);
        printf("a clear coat and sheen traced, the GPU against the CPU: %.2f%% of pixels apart\n", tracedApart * 100);
        FLIP_ALIKE(0.0109); /* (measured 0.0009 on a Radeon RX 9060 XT) */
        CHECK(tracedApart < .01);
        artfSceneFree(s);
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    /* from glTF */
    static const char file[] =
        "{\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"KHR_materials_clearcoat\",\"KHR_materials_sheen\"],"
        "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\","
        "\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]}],\"materials\":[{\"extensions\":{\"KHR_materials_clearcoat\":{\"clearcoatFactor\":0.75,"
        "\"clearcoatRoughnessFactor\":0.25},\"KHR_materials_sheen\":{\"sheenColorFactor\":[0.5,0.25,1]}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]}";
    ArtfScene* s = artfSceneNew();
    CHECK(artfSceneLoadModelMemory(s, file, sizeof file - 1, NULL, NULL) != 0);
    const ArtfMaterialMore* more = &s->materialsMore.items[s->materialsMore.len - 1];
    CHECK(s->materialsMore.len == s->materials.len && more->coat == .75f && more->coatRoughness == .25f);
    CHECK(more->sheen[0] == .5f && more->sheen[1] == .25f && more->sheen[2] == 1.f && s->layeredMaterials == 1);
    ArtfMaterialDesc none = artfMaterialDefaults();
    artfMaterialSet(s, (ArtfMaterial)(s->materials.len - 1), &none);
    CHECK(s->layeredMaterials == 0);
    artfSceneFree(s);
}

static int brightCount(const uint32_t* f, int over) {
    int n = 0;
    for (int i = 0; i < W * H; ++i) n += CHANNEL(f[i], 8) > over;
    return n;
}

/* A light with a size: its highlight on something smooth is wider, the same on both backends; traced, its shadow is
 * softer. */
void testSizedLights(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* point = bulbScene(0);
        ArtfScene* bulb = bulbScene(.5f);
        drawInto(r, point, a);
        drawInto(r, bulb, b);
        printf("%s, a light with a size: %d bright pixels, %d from a point\n", backend ? "GPU" : "CPU", brightCount(b, 150), brightCount(a, 150));
        CHECK(brightCount(b, 150) > brightCount(a, 150) * 2);
        artfSceneFree(point);
        artfSceneFree(bulb);
        artfRendererFree(r);
    }
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    ArtfScene* s = bulbScene(.5f);
    if (gpu) {
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("a light with a size, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0127); /* (measured 0.0027 on a Radeon RX 9060 XT) */
        CHECK(apart < .005);
    }
    /* traced: the cube's shadow has a soft edge */
    ArtfScene* point = bulbScene(0);
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 64, 0};
    artfSceneSetTrace(s, &rays);
    artfSceneSetTrace(point, &rays);
    for (int k = 0; k < 64; ++k) drawInto(cpu, point, a);
    for (int k = 0; k < 64; ++k) drawInto(cpu, s, b);
    printf("traced, a light with a size: %d pixels between light and shadow, %d from a point\n", greyCount(b), greyCount(a));
    CHECK(greyCount(b) > greyCount(a) + 200);
    if (gpu) {
        for (int k = 0; k < 64; ++k) drawInto(gpu, s, a);
        const double apart = differentShare(a, b, 24);
        printf("traced, a light with a size, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0126); /* (measured 0.0026 on a Radeon RX 9060 XT) */
        CHECK(apart < .02);
    }
    /* removed, it is a point again when its place is taken */
    artfSceneRemoveLight(point, 1);
    CHECK(point->lightSize[0] == 0);
    artfSceneFree(point);
    artfSceneFree(s);
    artfRendererFree(cpu);
    artfRendererFree(gpu);
    artfFree(a);
    artfFree(b);
}
/* ---- lights with a shape, profiles, and materials past glTF's core ---- */

/* bulbScene's light as a point (0), a tube along x (1), a rectangle facing down (2) or up (3). */
static ArtfScene* shapeScene(int kind) {
    ArtfScene* s = bulbScene(0);
    if (kind == 1) artfLightSetShape(s, 1, artfVec3(1, 0, 0), artfVec3(0, 0, 0), 3, 0);
    if (kind >= 2) artfLightSetShape(s, 1, artfVec3(1, 0, 0), artfVec3(0, kind == 2 ? -1.f : 1.f, 0), 3, 1);
    return s;
}

/* How wide and how tall what is brighter than over is, in green. */
static void brightBox(const uint32_t* f, int over, int* width, int* height) {
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (CHANNEL(f[y * W + x], 8) > over) {
                x0 = artfMini(x0, x);
                x1 = artfMaxi(x1, x);
                y0 = artfMini(y0, y);
                y1 = artfMaxi(y1, y);
            }
    *width = x1 - x0 + 1;
    *height = y1 - y0 + 1;
}


/* A tube's highlight is long, a rectangle's long and wide, and a rectangle lights only the side it faces; drawn the same
 * on both backends, and traced, with soft shadows. A shape taken away leaves the bytes as they were. */
void testLightShapes(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        int bright[4], wide[4], tall[4];
        double under[4];
        for (int kind = 0; kind < 4; ++kind) {
            ArtfScene* s = shapeScene(kind);
            drawInto(r, s, a);
            bright[kind] = brightCount(a, 150);
            brightBox(a, 150, &wide[kind], &tall[kind]);
            under[kind] = meanOf(a, W / 2, H * 3 / 4, 8);
            artfSceneFree(s);
        }
        printf("%s, light shapes: bright pixels %d, %d, %d (point, tube, rectangle), as wide as %d, %d, %d; under a rectangle %.0f, %.0f facing away\n", backend ? "GPU" : "CPU", bright[0],
               bright[1], bright[2], wide[0], wide[1], wide[2], under[2], under[3]);
        CHECK(bright[1] > bright[0] * 2 && bright[2] > bright[0] * 2 && wide[1] > wide[0] * 3 / 2 && wide[2] > wide[0] * 3 / 2);
        CHECK(under[3] < under[2] * .25 && under[3] < under[0] * .25);
        artfRendererFree(r);
    }
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    for (int kind = 1; kind < 3 && gpu; ++kind) {
        ArtfScene* s = shapeScene(kind);
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("a %s, the GPU against the CPU: %.2f%% of pixels apart\n", kind == 1 ? "tube" : "rectangle", apart * 100);
        FLIP_ALIKE(0.0128); /* (measured 0.0028 on a Radeon RX 9060 XT) */
        CHECK(apart < .005);
        artfSceneFree(s);
    }
    /* traced: soft shadows, and the same on both */
    ArtfScene* point = shapeScene(0);
    ArtfScene* panel = shapeScene(2);
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 64, 0};
    artfSceneSetTrace(point, &rays);
    artfSceneSetTrace(panel, &rays);
    for (int k = 0; k < 24; ++k) drawInto(cpu, point, a);
    for (int k = 0; k < 24; ++k) drawInto(cpu, panel, b);
    printf("traced, a rectangle: %d pixels between light and shadow, %d from a point\n", greyCount(b), greyCount(a));
    CHECK(greyCount(b) > greyCount(a) + 200);
    if (gpu) {
        for (int k = 0; k < 24; ++k) drawInto(gpu, panel, a);
        const double apart = differentShare(a, b, 24);
        printf("traced, a rectangle, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0122); /* (measured 0.0022 on a Radeon RX 9060 XT) */
        CHECK(apart < .02);
    }
    artfSceneFree(point);
    artfSceneFree(panel);
    /* none again: the same bytes; and a light with a shape out of reach changes none */
    ArtfScene* s = shapeScene(0);
    drawInto(cpu, s, a);
    const uint64_t before = hashOf(a, W * H);
    artfLightSetShape(s, 1, artfVec3(1, 0, 0), artfVec3(0, -1, 0), 3, 1);
    drawInto(cpu, s, a);
    artfLightSetShape(s, 1, artfVec3(1, 0, 0), artfVec3(0, -1, 0), 0, 0);
    const ArtfLight far = artfSceneAddLight(s, artfVec3(0, 50, 0), artfVec3(1, 1, 1), 1);
    artfLightSetShape(s, far, artfVec3(0, 0, 1), artfVec3(0, 0, 0), 2, 0);
    drawInto(cpu, s, a);
    CHECK(hashOf(a, W * H) == before);
    artfSceneRemoveLight(s, far);
    CHECK(s->lightShape[far - 1][3] == 0);
    artfSceneFree(s);
    artfRendererFree(cpu);
    artfRendererFree(gpu);
    artfFree(a);
    artfFree(b);
}

/* Light from a rectangle and a tube, drawn, against the same traced (the mean of points over the shape, many paths):
 * how far the drawn picture is from it, as a share of its light, on a floor rough and less so. Lit from a point of the
 * shape (before linearly transformed cosines), the rectangle was 54% to 58% of its light apart and the tube 22%; over
 * all of it, no more than 12% and 2% (4.0, 8.2, 9.2; 0.3, 0.5, 0.4 when it was written). The GPU draws it as the CPU. */
static ArtfScene* areaScene(int kind, float rough) {
    ArtfScene* s = plainScene();
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    setCamera(s, 0, 1.2f, 3.5f, 0, -1, -.6f);
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.45f, .45f, .45f);
    d.roughness = rough;
    const ArtfTransform floor = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 20, 1), artfSceneAddMaterial(s, &d), &floor);
    const ArtfLight l = artfSceneAddLight(s, artfVec3(0, -.2f, -.8f), artfVec3(.8f, .8f, .8f), 6);
    if (kind == 0) artfLightSetShape(s, l, artfVec3(1, 0, 0), artfVec3(0, -1, -.2f), 2.f, 1.f);
    else {
        artfLightSetShape(s, l, artfVec3(1, 0, .3f), artfVec3(0, 0, 0), 2.4f, 0);
        artfLightSetSize(s, l, .05f);
    }
    return s;
}

static double areaError(ArtfRenderer* r, int kind, float rough, uint32_t* a, uint32_t* b) {
    ArtfScene* s = areaScene(kind, rough);
    drawInto(r, s, a);
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 64, 0, 1, false, false};
    artfSceneSetTrace(s, &rays);
    for (int k = 0; k < 40; ++k) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, b));
    artfSceneFree(s);
    double apart = 0, lit = 0;
    for (int i = 0; i < W * H; ++i)
        for (int k = 0; k < 24; k += 8) {
            apart += abs((int)(a[i] >> k & 255) - (int)(b[i] >> k & 255));
            lit += (double)(b[i] >> k & 255);
        }
    return apart / (lit > 1 ? lit : 1.);
}

void testAreaLights(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    static const float roughs[3] = {.2f, .45f, .8f};
    for (int kind = 0; kind < 2; ++kind)
        for (int k = 0; k < 3; ++k) {
            const double e = areaError(cpu, kind, roughs[k], a, b);
            printf("a %s, roughness %.2f: drawn against traced, %.1f%% of its light apart\n", kind ? "tube" : "rectangle", roughs[k], e * 100);
            CHECK(e < (kind ? .02 : .12));
        }
    o.backend = ARTF_BACKEND_VULKAN;
    ArtfRenderer* gpu = artfRendererNew(&o);
    for (int kind = 0; kind < 2 && gpu; ++kind)
        for (int k = 0; k < 3; ++k) {
            ArtfScene* s = areaScene(kind, roughs[k]);
            drawInto(cpu, s, a);
            drawInto(gpu, s, b);
            const double apart = differentShare(a, b, 8);
            if (apart >= .005) printf("a %s, roughness %.2f, the GPU against the CPU: %.2f%% of pixels apart\n", kind ? "tube" : "rectangle", roughs[k], apart * 100);
            FLIP_ALIKE(0.0115); /* (measured 0.0015 on a Radeon RX 9060 XT) */
            CHECK(apart < .005);
            artfSceneFree(s);
        }
    artfRendererFree(gpu);
    artfRendererFree(cpu);
    artfFree(a);
    artfFree(b);
}

/* IES files read, by their symmetry; a light with a narrow profile lights a small round below it, on both backends, drawn
 * and traced; taken away, the bytes are as they were. */
void testProfiles(void) {
    static const char narrow[] = "IESNA:LM-63-2002\r\n[TEST] a narrow beam\r\n[MANUFAC] none\r\nTILT=NONE\r\n1 1000 1 7 1 1 1 0 0 0\r\n1 1 100\r\n0 10 20 30 40 60 90\r\n0\r\n"
                                 "1000 900 400 50 10 0 0\r\n";
    int vertical = 0, horizontal = 0;
    float* table = artfIesDecode(narrow, sizeof narrow - 1, &vertical, &horizontal);
    CHECK(table && vertical == 181 && horizontal == 1);
    if (!table) return;
    CHECK(table[0] == 1.f && fabsf(table[10] - .9f) < 1e-5f && fabsf(table[15] - .65f) < 1e-5f && table[90] == 0 && table[180] == 0);
    /* a quarter, mirrored round; tilted lamps' data skipped; commas between numbers */
    static const char quarter[] = "IESNA91\nTILT=INCLUDE\n1\n2\n0 90\n1 1\n1, 100, 1, 2, 3, 1, 1, 0, 0, 0\n1 1 50\n0 90\n0 45 90\n10 0\n20 0\n40 0\n";
    int qv = 0, qh = 0;
    float* q = artfIesDecode(quarter, sizeof quarter - 1, &qv, &qh);
    CHECK(q && qv == 181 && qh == 72);
    if (q) CHECK(q[0] == .25f && q[18 * 181] == 1.f && q[36 * 181] == .25f && q[54 * 181] == 1.f && fabsf(q[9 * 181] - .5f) < 1e-5f && q[181 + 120] == 0);
    artfFree(q);
    CHECK(!artfIesDecode("nothing here", 12, &qv, &qh) && !artfIesDecode("TILT=NONE\n1 2 3", 15, &qv, &qh));

    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_VULKAN;
    ArtfRenderer* gpu = artfRendererNew(&o);
    ArtfScene* s = bulbScene(0);
    artfLightSet(s, 1, artfVec3(0, 2, 0), artfVec3(2, 2, 2), 12);
    drawInto(cpu, s, a);
    const uint64_t before = hashOf(a, W * H);
    const double wide = meanOf(a, W / 2 + 70, H * 3 / 4, 8);
    artfLightSetProfile(s, 1, table, vertical, horizontal);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRenderer* r = backend ? gpu : cpu;
        if (!r) continue;
        drawInto(r, s, backend ? b : a);
        const uint32_t* f = backend ? b : a;
        printf("%s, a narrow profile: %.0f below it, %.0f to the side, %.0f there with none\n", backend ? "GPU" : "CPU", meanOf(f, W / 2, H * 3 / 4, 8), meanOf(f, W / 2 + 70, H * 3 / 4, 8), wide);
        CHECK(meanOf(f, W / 2 + 70, H * 3 / 4, 8) < wide * .5 && meanOf(f, W / 2, H * 3 / 4, 8) > meanOf(f, W / 2 + 70, H * 3 / 4, 8) * 2);
    }
    if (gpu) {
        const double apart = differentShare(a, b, 24);
        printf("a profile, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.011); /* (measured 0.0010 on a Radeon RX 9060 XT) */
        CHECK(apart < .005);
        const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 64, 0};
        artfSceneSetTrace(s, &rays);
        for (int k = 0; k < 24; ++k) drawInto(cpu, s, a);
        for (int k = 0; k < 24; ++k) drawInto(gpu, s, b);
        const double traced = differentShare(a, b, 24);
        printf("a profile traced, the GPU against the CPU: %.2f%% of pixels apart; %.0f to the side\n", traced * 100, meanOf(a, W / 2 + 70, H * 3 / 4, 8));
        FLIP_ALIKE(0.0108); /* (measured 0.0008 on a Radeon RX 9060 XT) */
        CHECK(traced < .02 && meanOf(a, W / 2 + 70, H * 3 / 4, 8) < wide * .5);
        const ArtfTraceDesc none = {ARTF_TRACE_NONE, 0, 0, 0};
        artfSceneSetTrace(s, &none);
    }
    artfLightSetProfile(s, 1, NULL, 0, 0);
    drawInto(cpu, s, a);
    CHECK(hashOf(a, W * H) == before && !s->lightProfile[0]);
    artfFree(table);
    artfSceneFree(s);
    artfRendererFree(cpu);
    artfRendererFree(gpu);
    artfFree(a);
    artfFree(b);
}

/* coatScene's sphere made of more: each changes the picture where it should, on both backends alike, drawn and traced. */
static ArtfScene* moreScene(int kind, ArtfMaterial* made) {
    ArtfScene* s = coatScene(0, 0);
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.5f, .05f, .05f);
    d.roughness = .35f;
    if (kind == 1) d.specularColor = artfVec3(.2f, .5f, 3.f);
    if (kind == 2) {
        d.metallic = 1;
        d.baseColor = artfVec3(.9f, .9f, .9f);
        d.anisotropy = .9f;
    }
    if (kind == 3) {
        d.iridescence = 1;
        d.iridescenceThickness = 550;
    }
    if (kind == 4) {
        d.subsurface = artfVec3(1, .4f, .3f);
        d.subsurfaceRadius = .5f;
    }
    if (kind == 5) {
        /* all at once, and a coat with textures */
        static uint32_t checks[16 * 16];
        for (int i = 0; i < 16 * 16; ++i) checks[i] = (i / 16 / 4 + i % 16 / 4) & 1 ? 0xFFFFFFFFu : 0x40404040u;
        const ArtfTexture t = artfSceneAddTexture(s, checks, 16, 16, 0);
        d.clearcoat = 1;
        d.clearcoatRoughness = .1f;
        d.clearcoatMap = t;
        d.sheen = artfVec3(.5f, .5f, .5f);
        d.sheenRoughness = .4f;
        d.sheenMap = t;
        d.specularColor = artfVec3(1, .5f, .5f);
        d.anisotropy = .5f;
        d.iridescence = .5f;
        d.subsurface = artfVec3(.5f, .2f, .1f);
    }
    if (kind == 6) {
        d.clearcoat = 1;
        d.clearcoatRoughness = .1f;
    }
    *made = artfSceneAddMaterial(s, &d);
    artfInstanceSetMaterial(s, s->instanceHandles.items[0], *made);
    return s;
}

/* coatScene's sphere with highlights of KHR_materials_specular, strong to be seen: kind 0 by its factor alone, as the
 * textures of kind 1 would make it; 1 by a color texture (bytes FF 80 40) and a strength texture (alpha 128), each of
 * one color; 2 a strength texture of checks, 0 and all. */
static ArtfScene* specularScene(int kind) {
    ArtfScene* s = coatScene(0, 0);
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.5f, .05f, .05f);
    d.roughness = .35f;
    d.specularColor = artfVec3(3, 3, 3);
    static uint32_t tinted[4 * 4], half[4 * 4], checks[16 * 16];
    for (int i = 0; i < 16; ++i) {
        tinted[i] = 0xFFFF8040u;
        half[i] = 0x80FFFFFFu;
    }
    for (int i = 0; i < 16 * 16; ++i) checks[i] = (i / 16 / 4 + i % 16 / 4) & 1 ? 0xFFFFFFFFu : 0x00FFFFFFu;
    if (kind == 0) {
        const float strength = 128.f / 255.f;
        d.specularColor = artfVec3(3 * artfSrgbDecode(1.f) * strength, 3 * artfSrgbDecode(128.f / 255.f) * strength, 3 * artfSrgbDecode(64.f / 255.f) * strength);
    }
    if (kind == 1) {
        d.specularColorMap = artfSceneAddTexture(s, tinted, 4, 4, 0);
        d.specularMap = artfSceneAddTexture(s, half, 4, 4, 0);
    }
    if (kind == 2) d.specularMap = artfSceneAddTexture(s, checks, 16, 16, 0);
    artfInstanceSetMaterial(s, s->instanceHandles.items[0], artfSceneAddMaterial(s, &d));
    return s;
}

/* KHR_materials_specular's textures: one of a color and strength draws as the factor that says as much, drawn and
 * traced, on both backends alike; checks of strength show. */
void testSpecularMaps(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* cpuMapped[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        for (int traced = 0; traced < 2; ++traced) {
            ArtfScene* factor = specularScene(0);
            ArtfScene* mapped = specularScene(1);
            ArtfScene* checked = specularScene(2);
            const ArtfTraceDesc paths = {ARTF_TRACE_PATHS, 1, 16, 0, true};
            if (traced) {
                artfSceneSetTrace(factor, &paths);
                artfSceneSetTrace(mapped, &paths);
                artfSceneSetTrace(checked, &paths);
            }
            const int frames = traced ? 16 : 1;
            for (int k = 0; k < frames; ++k) drawInto(r, factor, a);
            for (int k = 0; k < frames; ++k) drawInto(r, mapped, b);
            const double same = differentShare(a, b, traced ? 24 : 4);
            for (int k = 0; k < frames; ++k) drawInto(r, checked, a);
            const double checkered = differentShare(a, b, 8);
            double across = 0;
            if (!backend) memcpy(cpuMapped[traced], b, W * H * 4);
            else across = differentShare(cpuMapped[traced], b, 8);
            printf("%s%s: textures against the factor %.3f%% of pixels apart; checks of strength change %.2f%%; the GPU against the CPU %.3f%%\n", backend ? "GPU" : "CPU",
                   traced ? ", traced" : "", same * 100, checkered * 100, across * 100);
            CHECK(same < .002 && checkered > .01);
            if (backend) CHECK(across < .01);
            artfSceneFree(factor);
            artfSceneFree(mapped);
            artfSceneFree(checked);
        }
        artfRendererFree(r);
    }
    artfFree(a);
    artfFree(b);
    artfFree(cpuMapped[0]);
    artfFree(cpuMapped[1]);
}

/* coatScene's sphere with a thin film or drawn highlights, by factors (kind 0, 2) or by textures that say as much
 * (kind 1: the film's thickness texture at its least, 250 nm; kind 3: drawn along u, at full strength), or by checks
 * (kind 4: the film's amount; kind 5: the highlights' strength). */
static ArtfScene* filmScene(int kind) {
    ArtfScene* s = coatScene(0, 0);
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.5f, .05f, .05f);
    d.roughness = .35f;
    static uint32_t thin[16], along[16], checks[16 * 16], strong[16 * 16];
    for (int i = 0; i < 16; ++i) {
        thin[i] = 0xFFFF00FFu;  /* (green 0: the least thickness) */
        along[i] = 0xFFFF80FFu; /* (red 1, green .5: along u; blue 1: full strength) */
    }
    for (int i = 0; i < 16 * 16; ++i) {
        const bool on = (i / 16 / 4 + i % 16 / 4) & 1;
        checks[i] = on ? 0xFFFFFFFFu : 0xFF00FFFFu; /* (red: the amount) */
        strong[i] = on ? 0xFFFF80FFu : 0xFFFF8000u; /* (blue: the strength) */
    }
    if (kind <= 1 || kind == 4) {
        d.iridescence = 1;
        d.iridescenceThickness = kind == 0 ? 250.f : 550.f;
        d.iridescenceThicknessMin = 250.f;
        if (kind == 1) d.iridescenceThicknessMap = artfSceneAddTexture(s, thin, 4, 4, 0);
        if (kind == 4) d.iridescenceMap = artfSceneAddTexture(s, checks, 16, 16, 0);
    } else {
        d.metallic = 1;
        d.baseColor = artfVec3(.9f, .9f, .9f);
        d.anisotropy = .9f;
        if (kind == 3) d.anisotropyMap = artfSceneAddTexture(s, along, 4, 4, 0);
        if (kind == 5) d.anisotropyMap = artfSceneAddTexture(s, strong, 16, 16, 0);
    }
    artfInstanceSetMaterial(s, s->instanceHandles.items[0], artfSceneAddMaterial(s, &d));
    return s;
}

/* KHR_materials_iridescence's and KHR_materials_anisotropy's textures: what says as much as a factor draws as it,
 * checks show, drawn and traced, both backends alike. */
void testFilmMaps(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* cpu[2][2];
    for (int i = 0; i < 4; ++i) cpu[i / 2][i % 2] = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        for (int traced = 0; traced < 2; ++traced)
            for (int which = 0; which < 2; ++which) {
                ArtfScene* scenes[3] = {filmScene(which * 2), filmScene(which * 2 + 1), filmScene(which + 4)};
                const ArtfTraceDesc paths = {ARTF_TRACE_PATHS, 1, 16, 0, true};
                for (int k = 0; k < 3 && traced; ++k) artfSceneSetTrace(scenes[k], &paths);
                const int frames = traced ? 16 : 1;
                for (int k = 0; k < frames; ++k) drawInto(r, scenes[0], a);
                for (int k = 0; k < frames; ++k) drawInto(r, scenes[1], b);
                const double same = differentShare(a, b, traced ? 24 : 4);
                for (int k = 0; k < frames; ++k) drawInto(r, scenes[2], a);
                const double checkered = differentShare(a, b, 8);
                double across = 0;
                if (!backend) memcpy(cpu[traced][which], b, W * H * 4);
                else across = differentShare(cpu[traced][which], b, 8);
                printf("%s%s, %s: textures against factors %.3f%% of pixels apart; checks change %.2f%%; the GPU against the CPU %.3f%%\n", backend ? "GPU" : "CPU", traced ? ", traced" : "",
                       which ? "drawn highlights" : "a thin film", same * 100, checkered * 100, across * 100);
                CHECK(same < .003 && checkered > .01);
                if (backend) CHECK(across < .01);
                for (int k = 0; k < 3; ++k) artfSceneFree(scenes[k]);
            }
        artfRendererFree(r);
    }
    artfFree(a);
    artfFree(b);
    for (int i = 0; i < 4; ++i) artfFree(cpu[i / 2][i % 2]);
}

/* OpenPBR's layers from a graph (ARTF_OUT_COAT...): kind 0, the material's own; 1, a graph that says the same; 2, a
 * graph whose coat goes in stripes. */
static ArtfScene* openPbrScene(int kind) {
    ArtfScene* s = coatScene(0, 0);
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.5f, .15f, .05f);
    d.roughness = .6f;
    if (kind == 0) {
        d.clearcoat = .8f;
        d.clearcoatRoughness = .2f;
        d.sheen = artfVec3(.3f, .2f, .1f);
        d.sheenRoughness = .5f;
        d.specularColor = artfVec3(1.f, .5f, .25f);
        d.iridescence = .7f;
        d.iridescenceThickness = 400.f;
        d.subsurface = artfVec3(.2f, .1f, .05f);
    } else {
        ArtfGraph* g = artfGraphNew();
        ArtfExpr coat = artfExprNumber(g, .8f);
        if (kind == 2) {
            /* (stripes along u: none, then all) */
            ArtfExpr u = artfExprX(g, artfExprUv(g));
            coat = artfExprFloor(g, artfExprMul(g, artfExprFract(g, artfExprMul(g, u, artfExprNumber(g, 8))), artfExprNumber(g, 2)));
        }
        artfGraphSetOutput(g, ARTF_OUT_COAT, coat);
        artfGraphSetOutput(g, ARTF_OUT_COAT_ROUGHNESS, artfExprNumber(g, .2f));
        artfGraphSetOutput(g, ARTF_OUT_FUZZ, artfExprConst(g, .3f, .2f, .1f, 1));
        artfGraphSetOutput(g, ARTF_OUT_FUZZ_ROUGHNESS, artfExprNumber(g, .5f));
        artfGraphSetOutput(g, ARTF_OUT_SPECULAR, artfExprConst(g, 1.f, .5f, .25f, 1));
        artfGraphSetOutput(g, ARTF_OUT_THIN_FILM, artfExprConst(g, .7f, 400.f, 0, 0));
        artfGraphSetOutput(g, ARTF_OUT_SUBSURFACE, artfExprConst(g, .2f, .1f, .05f, 1));
        d.graph = artfSceneAddGraph(s, g);
        artfGraphFree(g);
    }
    artfInstanceSetMaterial(s, s->instanceHandles.items[0], artfSceneAddMaterial(s, &d));
    return s;
}

/* OpenPBR's layers in a material of the program's own: a graph that sets a coat, fuzz, the specular color, a thin film
 * and light under the surface draws as the material that has them, drawn and traced, both backends alike; a coat in
 * stripes shows. */
void testOpenPbr(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* cpu[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        for (int traced = 0; traced < 2; ++traced) {
            ArtfScene* scenes[3] = {openPbrScene(0), openPbrScene(1), openPbrScene(2)};
            const ArtfTraceDesc paths = {ARTF_TRACE_PATHS, 1, 16, 0, true};
            for (int k = 0; k < 3 && traced; ++k) artfSceneSetTrace(scenes[k], &paths);
            const int frames = traced ? 16 : 1;
            for (int k = 0; k < frames; ++k) drawInto(r, scenes[0], a);
            for (int k = 0; k < frames; ++k) drawInto(r, scenes[1], b);
            const double same = differentShare(a, b, traced ? 24 : 4);
            for (int k = 0; k < frames; ++k) drawInto(r, scenes[2], a);
            const double striped = differentShare(a, b, 8);
            double across = 0;
            if (!backend) memcpy(cpu[traced], b, W * H * 4);
            else across = differentShare(cpu[traced], b, 8);
            printf("%s%s: the graph's layers against the material's %.3f%% of pixels apart; a coat in stripes changes %.2f%%; the GPU against the CPU %.3f%%\n",
                   backend ? "GPU" : "CPU", traced ? ", traced" : "", same * 100, striped * 100, across * 100);
            CHECK(same < .003 && striped > .01);
            if (backend) CHECK(across < .01);
            for (int k = 0; k < 3; ++k) artfSceneFree(scenes[k]);
        }
        artfRendererFree(r);
    }
    artfFree(a);
    artfFree(b);
    artfFree(cpu[0]);
    artfFree(cpu[1]);
}

void testMoreMaterials(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfMaterial m;
        ArtfScene* plain = moreScene(0, &m);
        drawInto(r, plain, a);
        double apart[7] = {0};
        for (int kind = 1; kind < 7; ++kind) {
            ArtfScene* s = moreScene(kind, &m);
            drawInto(r, s, b);
            apart[kind] = differentShare(a, b, 8);
            if (kind == 1) {
                /* what it reflects goes blue */
                long blue = 0;
                for (int i = 0; i < W * H; ++i) blue += CHANNEL(b[i], 0) - CHANNEL(a[i], 0) - (CHANNEL(b[i], 16) - CHANNEL(a[i], 16));
                CHECK(blue > W * H / 100);
            }
            if (kind == 6) {
                /* textured, the coat shows its checks: no other coat shows them */
                ArtfScene* t = moreScene(5, &m);
                drawInto(r, t, a);
                apart[0] = differentShare(a, b, 8);
                drawInto(r, plain, a);
                artfSceneFree(t);
            }
            artfSceneFree(s);
        }
        printf("%s, past glTF's core, shares of pixels changed: specular %.2f, anisotropy %.2f, iridescence %.2f, under the surface %.2f, all %.2f\n", backend ? "GPU" : "CPU", apart[1],
               apart[2], apart[3], apart[4], apart[5]);
        for (int kind = 1; kind < 6; ++kind) CHECK(apart[kind] > .01);
        CHECK(apart[0] > .01);
        artfSceneFree(plain);
        artfRendererFree(r);
    }
    /* light under the surface comes through from behind, where the sun's map says it is thin */
    {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* cpu = artfRendererNew(&o);
        double back[2];
        for (int k = 0; k < 2; ++k) {
            ArtfScene* s = plainScene();
            artfSceneSetAmbient(s, artfVec3(.05f, .05f, .05f), artfVec3(.05f, .05f, .05f));
            artfSceneSetSun(s, artfVec3(0, 0, 1), artfVec3(2, 2, 2)); /* from behind, toward the eye */
            setCamera(s, 0, .5f, 4, 0, 0, 0);
            const ArtfShadowDesc maps = {1024, 20, false};
            artfSceneSetShadows(s, &maps);
            ArtfMaterialDesc thin = artfMaterialDefaults();
            thin.baseColor = artfVec3(.6f, .5f, .4f);
            thin.subsurface = k ? artfVec3(1, .5f, .4f) : artfVec3(0, 0, 0);
            thin.subsurfaceRadius = .2f;
            thin.doubleSided = true;
            /* (a thin plate, its back to the sun) */
            const ArtfTransform at = artfTransformTrs(artfVec3(0, 0, 1.8f), artfQuatIdentity(), artfVec3(1.4f, 1.4f, .03f));
            artfSceneAddInstance(s, artfSceneAddCube(s, 1), artfSceneAddMaterial(s, &thin), &at);
            drawInto(cpu, s, a);
            back[k] = meanOf(a, W / 2, H / 2 - 40, 16);
            artfSceneFree(s);
        }
        printf("light through a thin plate, from behind: %.0f, %.0f with none\n", back[1], back[0]);
        CHECK(back[1] > back[0] + 20);
        artfRendererFree(cpu);
    }
    /* both at once, on both backends: drawn and traced */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    ArtfMaterial m;
    for (int kind = 2; kind < 6 && gpu; ++kind) {
        ArtfScene* s = moreScene(kind, &m);
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("past glTF's core (%d), the GPU against the CPU: %.2f%% of pixels apart\n", kind, apart * 100);
        FLIP_ALIKE(0.0115); /* (measured 0.0015 on a Radeon RX 9060 XT) */
        CHECK(apart < .005);
        if (kind == 5 || kind == 4) {
            ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 32, 0};
            artfSceneSetTrace(s, &rays);
            for (int k = 0; k < 16; ++k) drawInto(cpu, s, a);
            for (int k = 0; k < 16; ++k) drawInto(gpu, s, b);
            const double traced = differentShare(a, b, 24);
            printf("past glTF's core (%d) traced, the GPU against the CPU: %.2f%% of pixels apart\n", kind, traced * 100);
            FLIP_ALIKE(0.0109); /* (measured 0.0009 on a Radeon RX 9060 XT) */
            CHECK(traced < .02);
        }
        artfSceneFree(s);
    }
    /* what a glass's inside takes: traced, not drawn */
    {
        double green[2];
        uint64_t drawn[2];
        for (int k = 0; k < 2; ++k) {
            ArtfScene* s = moreScene(0, &m);
            /* (what is seen through it: a white wall) */
            ArtfMaterialDesc wall = artfMaterialDefaults();
            wall.baseColor = artfVec3(0, 0, 0);
            wall.emissive = artfVec3(1, 1, 1);
            addAt(s, artfSceneAddCube(s, 1), artfSceneAddMaterial(s, &wall), 0, 0, -7, 8);
            ArtfMaterialDesc glass = artfMaterialDefaults();
            glass.alphaMode = ARTF_ALPHA_BLEND;
            glass.transparency = .9f;
            glass.ior = 1.5f;
            glass.attenuation = k ? artfVec3(.1f, .9f, .1f) : artfVec3(0, 0, 0);
            glass.attenuationDistance = k ? .5f : 0.f;
            artfMaterialSet(s, m, &glass);
            drawInto(cpu, s, a);
            drawn[k] = hashOf(a, W * H);
            ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 16, 0};
            artfSceneSetTrace(s, &rays);
            for (int j = 0; j < 8; ++j) drawInto(cpu, s, a);
            green[k] = meanOf(a, W / 2, H / 2 - 20, 8) - meanOf(a, W / 2, H / 2 - 20, 16);
            if (k && gpu) {
                for (int j = 0; j < 8; ++j) drawInto(gpu, s, b);
                const double traced = differentShare(a, b, 24);
                printf("a glass's inside traced, the GPU against the CPU: %.2f%% of pixels apart\n", traced * 100);
                FLIP_ALIKE(0.0112); /* (measured 0.0012 on a Radeon RX 9060 XT) */
                CHECK(traced < .02);
            }
            artfSceneFree(s);
        }
        printf("traced through a green glass: green over red %.0f, %.0f clear\n", green[1], green[0]);
        CHECK(green[1] > green[0] + 20 && drawn[0] == drawn[1]);
    }
    /* the same bytes with every instruction set: all of it, under a rectangle with a profile and the sun's shadows */
    {
        ArtfScene* s = moreScene(5, &m);
        const ArtfShadowDesc maps = {1024, 20, true};
        artfSceneSetShadows(s, &maps);
        const ArtfLight panel = artfSceneAddLight(s, artfVec3(.5f, 2, 1), artfVec3(1, .8f, .6f), 8);
        artfLightSetShape(s, panel, artfVec3(1, 0, 0), artfVec3(0, -1, 0), 1.5f, .5f);
        const float beam[3] = {1, .5f, 0};
        artfLightSetProfile(s, panel, beam, 3, 1);
        drawInto(cpu, s, a);
        ArtfRendererOptions plain = options(ARTF_BACKEND_SOFTWARE, 0, true);
        ArtfRenderer* portable = artfRendererNew(&plain);
        drawInto(portable, s, b);
        CHECK(hashOf(a, W * H) == hashOf(b, W * H));
        artfRendererFree(portable);
        artfSceneFree(s);
    }
    /* none again: the same bytes as a material that never had any */
    {
        ArtfScene* s = moreScene(0, &m);
        drawInto(cpu, s, a);
        const uint64_t before = hashOf(a, W * H);
        ArtfMaterial all;
        ArtfScene* rich = moreScene(5, &all);
        ArtfMaterialDesc d = artfMaterialDefaults();
        d.baseColor = artfVec3(.5f, .05f, .05f);
        d.roughness = .35f;
        artfMaterialSet(rich, all, &d);
        CHECK(rich->moreMaterials == 0 && rich->layeredMaterials == 0);
        drawInto(cpu, rich, a);
        CHECK(hashOf(a, W * H) == before);
        artfSceneFree(rich);
        artfSceneFree(s);
    }
    artfRendererFree(cpu);
    artfRendererFree(gpu);
    artfFree(a);
    artfFree(b);
}

/* What a texture is, by what uses it (artfSceneTextureRoles): one only colors use has mipmaps that are means of light,
 * so that black and white stripes come to the grey of half their light; one data use stays means of bytes, as normals and
 * roughness must; one used both ways is data, and said so once; and what changes its use makes its mipmaps again. */
void testTextureRoles(void) {
    static uint32_t stripes[4 * 4];
    for (int i = 0; i < 16; ++i) stripes[i] = (i & 1) ? 0xFFFFFFFFu : 0xFF000000u;
    ArtfScene* s = plainScene();
    const ArtfTexture color = artfSceneAddTexture(s, stripes, 4, 4, 0), data = artfSceneAddTexture(s, stripes, 4, 4, 0);
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.texture = color;
    d.normalMap = data;
    const ArtfMaterial m = artfSceneAddMaterial(s, &d);
    artfSceneTextureRoles(s);
    const ArtfTextureData* c = &s->textures.items[color];
    const ArtfTextureData* a = &s->textures.items[data];
    const uint32_t greyOfLight = (uint32_t)shownByte(.5), greyOfBytes = 128;
    CHECK(c->light && (c->pixels[c->offsets[1]] & 255) == greyOfLight);
    CHECK(!a->light && (a->pixels[a->offsets[1]] & 255) == greyOfBytes);
    /* both ways: data's */
    logged = 0;
    artfSetLog(countLog, &logged);
    ArtfMaterialDesc both = artfMaterialDefaults();
    both.surfaceMap = color;
    const ArtfMaterial second = artfSceneAddMaterial(s, &both);
    artfSceneTextureRoles(s);
    artfSceneTextureRoles(s);
    CHECK(!s->textures.items[color].light && (s->textures.items[color].pixels[s->textures.items[color].offsets[1]] & 255) == greyOfBytes);
    CHECK(logged == 1);
    artfSetLog(NULL, NULL);
    /* the data use goes: a color again */
    both.surfaceMap = 0;
    artfMaterialSet(s, second, &both);
    const uint32_t generation = s->textures.items[color].generation;
    artfSceneTextureRoles(s);
    CHECK(s->textures.items[color].light && s->textures.items[color].generation != generation);
    (void)m;
    artfSceneFree(s);
}

/* Highlights smaller than a pixel (shading.inc's Widened): a shiny floor with a fine normal map, its texels a third of a
 * pixel, under a sun it mirrors toward the eye. Moved by a quarter of a pixel at a time, as a frame's jitter does,
 * without widening each pixel catches the sun on some texels and not others, and flickers; widened by the variance of
 * the texels it covers (Toksvig's, from how much shorter their mean is), it flickers less. Its light is near what eight
 * times as many pixels a side show (each a texel or less, not widened): GGX's lobe has a longer tail than these
 * texels, which turn no more than so far, and sends a little of the sun out of the frame. The sun is faint, so that no
 * highlight is more than white. The CPU's, where it can be turned off (artfWidenHighlights). */
static double flickerOf(ArtfScene* s, double* mean) {
    enum { FRAMES = 8 };
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    static double sum[W * H], square[W * H];
    memset(sum, 0, sizeof sum);
    memset(square, 0, sizeof square);
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    const float pixel = 4.47f * 2.f * tanf(.5f) / (float)H;
    for (int k = 0; k < FRAMES; ++k) {
        const float dx = ((float)(k & 3) - 1.5f) * .25f * pixel, dy = ((float)(k >> 2) - .5f) * .5f * pixel;
        setCamera(s, dx, 2 + dy, 4, dx, dy, 0);
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, frame));
        for (int i = 0; i < W * H; ++i) {
            double light = 0;
            for (int c = 0; c < 24; c += 8) light += artfSrgbDecode((float)(frame[i] >> c & 255) / 255.f) / 3;
            sum[i] += light;
            square[i] += light * light;
        }
    }
    double apart = 0, all = 0;
    for (int i = 0; i < W * H; ++i) {
        const double m = sum[i] / FRAMES;
        apart += sqrt(fmax(square[i] / FRAMES - m * m, 0));
        all += m;
    }
    artfFree(frame);
    artfRendererFree(r);
    *mean = all / (W * H);
    return apart / (W * H);
}

void testHighlightFlicker(void) {
    static uint32_t bumps[32 * 32];
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float nx = (float)((x * 7 + y * 13) % 5 - 2) * .2f, ny = (float)((x * 11 + y * 3) % 5 - 2) * .2f, nz = sqrtf(1.f - nx * nx - ny * ny);
            bumps[y * 32 + x] = 0xFF000000u | (uint32_t)((nx * .5f + .5f) * 255.f + .5f) << 16 | (uint32_t)((ny * .5f + .5f) * 255.f + .5f) << 8 | (uint32_t)((nz * .5f + .5f) * 255.f + .5f);
        }
    ArtfScene* s = plainScene();
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    artfSceneSetSun(s, artfVec3(0, -.447f, .894f), artfVec3(.03f, .03f, .03f));
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.9f, .9f, .9f);
    d.metallic = 1;
    d.roughness = .35f;
    d.normalMap = artfSceneAddTexture(s, bumps, 32, 32, 0);
    const ArtfTransform t = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 8, 32), artfSceneAddMaterial(s, &d), &t);
    double flicker[2], mean[2];
    for (int widen = 0; widen < 2; ++widen) {
        artfWidenHighlights = widen != 0;
        flicker[widen] = flickerOf(s, &mean[widen]);
    }
    /* the truth: eight times as many pixels a side, each a texel or less, not widened */
    artfWidenHighlights = false;
    enum { FINE = 8 };
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    o.width = W * FINE;
    o.height = H * FINE;
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* frame = artfMalloc((size_t)o.width * o.height * 4);
    setCamera(s, 0, 2, 4, 0, 0, 0);
    CHECK(artfRendererDraw(r, s) && artfRendererReadPixels(r, frame));
    double truth = 0;
    for (int i = 0; i < o.width * o.height; ++i)
        for (int c = 0; c < 24; c += 8) truth += artfSrgbDecode((float)(frame[i] >> c & 255) / 255.f) / 3;
    truth /= (double)o.width * o.height;
    artfFree(frame);
    artfRendererFree(r);
    artfWidenHighlights = true;
    printf("a fine normal map moved by quarter pixels: it flickers by %.4f, %.4f widened; its light %.4f and %.4f, where %.4f is the truth\n", flicker[0], flicker[1], mean[0], mean[1], truth);
    CHECK(flicker[1] < flicker[0] * .5);
    CHECK(mean[1] > truth * .6 && mean[1] < truth * 1.2);
    artfSceneFree(s);
}

/* A thin film (KHR_materials_iridescence) is Belcour and Barla's, as glTF's reference shader has it: its colors at an
 * index of 1.3, head on and toward grazing, over a dielectric and over a colored metal, are the reference's (the same
 * formula in doubles) to 1e-5. */
void testFilmBelcour(void) {
    static const struct { float base[3], cosine, thickness, want[3]; } cases[3] = {
        {{.04f, .04f, .04f}, 1.f, 300.f, {.024653f, .004652f, .021335f}},
        {{.9f, .6f, .3f}, .6f, 550.f, {.901340f, .600838f, .117934f}},
        {{.9f, .6f, .3f}, .2f, 300.f, {1.010293f, .800436f, .608388f}},
    };
    for (int k = 0; k < 3; ++k) {
        float f0[3] = {cases[k].base[0], cases[k].base[1], cases[k].base[2]};
        artfIridescence(f0, cases[k].cosine, 1.f, 1.3f, cases[k].thickness);
        for (int c = 0; c < 3; ++c) CHECK(fabsf(f0[c] - cases[k].want[c]) < 1e-5f);
    }
    /* (none of it, and no film: as it was) */
    float f0[3] = {.5f, .4f, .3f};
    artfIridescence(f0, .7f, 0.f, 1.3f, 400.f);
    CHECK(f0[0] == .5f && f0[1] == .4f && f0[2] == .3f);
}

/* (a floor that light goes under, by a sharp shadow's edge: seen from above; under: its color under the surface) */
static ArtfScene* underScene(float under) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetSun(s, artfVec3(.3f, -1.f, 0), artfVec3(2, 2, 2));
    const ArtfShadowDesc shadows = {1024, 20};
    artfSceneSetShadows(s, &shadows);
    ArtfMaterialDesc skin = artfMaterialDefaults();
    skin.baseColor = artfVec3(.8f, .6f, .5f);
    skin.specular = 0;
    skin.subsurface = artfVec3(under, under * .4f, under * .3f);
    skin.subsurfaceRadius = .15f;
    const ArtfTransform floor = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 8, 1), artfSceneAddMaterial(s, &skin), &floor);
    ArtfMaterialDesc grey = artfMaterialDefaults();
    const ArtfTransform wall = artfTransformTrs(artfVec3(-.5f, 1.f, 0), artfQuatAxisAngle(artfVec3(0, 1, 0), 0), artfVec3(.2f, 2.f, 4.f));
    artfSceneAddInstance(s, artfSceneAddCube(s, 1), artfSceneAddMaterial(s, &grey), &wall);
    setCamera(s, 0, 3, .01f, 0, 0, 0);
    return s;
}

/* (along the middle row, how many pixels are between a shadow's two sides: past 10% of the way and short of 90%) */
static int edgeWidth(const uint32_t* frame) {
    const int y = H / 2;
    float lo = 1e9f, hi = -1e9f, v[W];
    for (int x = 0; x < W; ++x) {
        const uint32_t p = frame[y * W + x];
        v[x] = (float)((p >> 16 & 255) + (p >> 8 & 255) + (p & 255));
    }
    /* (the two sides: the right quarter is lit, the left of the middle in shadow) */
    for (int x = W * 3 / 4; x < W; ++x) hi = fmaxf(hi, v[x]);
    for (int x = W / 8; x < W / 3; ++x) lo = fminf(lo, v[x]);
    int n = 0;
    for (int x = W / 8; x < W * 7 / 8; ++x) n += v[x] > lo + (hi - lo) * .1f && v[x] < lo + (hi - lo) * .9f;
    return n;
}

/* Light under the surface spreads across it (5.12: separable, of two Gaussians as wide as its radius on the screen):
 * a sharp shadow's edge on a floor that light goes under is wider than on one that it does not; both backends
 * alike. */
void testUnderSpread(void) {
    uint32_t* plain = artfMalloc(W * H * 4);
    uint32_t* spread = artfMalloc(W * H * 4);
    uint32_t* cpu = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* s = underScene(0);
        drawInto(r, s, plain);
        artfSceneFree(s);
        s = underScene(.5f);
        drawInto(r, s, spread);
        artfSceneFree(s);
        const int sharp = edgeWidth(plain), soft = edgeWidth(spread);
        printf("%s: a shadow's edge on a floor, %d pixels wide; %d with light under the surface\n", backend ? "GPU" : "CPU", sharp, soft);
        CHECK(soft >= sharp + 4);
        if (!backend) memcpy(cpu, spread, W * H * 4);
        else {
            const double apart = differentShare(cpu, spread, 8);
            printf("light under the surface, the GPU against the CPU: %.3f%% of pixels apart\n", apart * 100);
            CHECK(apart < .01);
        }
        artfRendererFree(r);
    }
    artfFree(plain);
    artfFree(spread);
    artfFree(cpu);
}
