/* artful-tests: colors, layers, alpha, samples and decals. */
#include "tests.h"

void testColorData(void) {
    ArtfScene* s = artfSceneNew();
    const uint32_t plain = addSquare(s, 0, 0, 0, 0) - 1, white = addSquare(s, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0xFFFFFFFFu) - 1;
    CHECK(!s->meshes.items[plain].colors && !s->meshes.items[plain].hasAlpha);
    CHECK(!s->meshes.items[white].colors);
    const uint32_t solid = addSquare(s, 0xFF102030u, 0, 0xFF405060u, 0xFF708090u) - 1;
    const uint32_t clear = addSquare(s, 0x80102030u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu) - 1;
    const ArtfMeshData* m = &s->meshes.items[solid];
    CHECK(m->colors && !m->hasAlpha);
    CHECK(s->meshes.items[clear].colors && s->meshes.items[clear].hasAlpha);
    CHECK(sizeof(ArtfPackedVertex) == 16 && sizeof(ArtfInstanceData) == 64);
    /* what a mesh is unpacked to, for its lighter versions, keeps them; 0 is white */
    ArtfVertex back[4];
    artfMeshUnpack(m, back);
    int found = 0;
    for (int i = 0; i < 4; ++i) found += back[i].color == 0xFF102030u || back[i].color == 0xFFFFFFFFu || back[i].color == 0xFF405060u || back[i].color == 0xFF708090u;
    CHECK(found == 4);

    /* a layer is one while it needs to be, and only that long */
    ArtfInstance a = addAt(s, solid + 1, 0, 0, 0, 0, 1), b = addAt(s, clear + 1, 0, 0, 0, 0, 1);
    CHECK(s->translucentCount == 1);
    artfInstanceSetOpacity(s, a, .5f);
    CHECK(s->translucentCount == 2);
    artfInstanceSetOpacity(s, a, 1.f);
    artfInstanceSetOpacity(s, b, 1.f); /* (its colors still ask for it) */
    CHECK(s->translucentCount == 1);
    artfSceneRemoveInstance(s, b);
    CHECK(s->translucentCount == 0);
    artfSceneFree(s);

    /* with the lighter versions, too: a colored mesh's levels are colored */
    s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    enum { N = 24 };
    static ArtfVertex v[(N + 1) * (N + 1)];
    static uint32_t idx[N * N * 6];
    uint32_t n = 0;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            ArtfVertex q = {(float)x, (float)y, 0, 0, 0, 1, 0, 0, 0xFF00FF00u};
            v[y * (N + 1) + x] = q;
        }
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            const uint32_t a0 = y * (N + 1) + x, b0 = a0 + 1, d0 = a0 + N + 1, e0 = d0 + 1;
            idx[n++] = a0; idx[n++] = b0; idx[n++] = e0;
            idx[n++] = a0; idx[n++] = e0; idx[n++] = d0;
        }
    const uint32_t first = artfSceneAddMesh(s, v, (N + 1) * (N + 1), idx, n) - 1;
    const ArtfMeshData* base = &s->meshes.items[first];
    CHECK(base->lodCount > 0);
    for (uint32_t i = 0; i < base->lodCount; ++i) {
        const ArtfMeshData* level = &s->meshes.items[base->lods[i]];
        CHECK(level->colors && level->colors[0] == 0xFF00FF00u);
    }
    artfSceneFree(s);
}

/* What the scene's colors, layers and alpha come to, on one backend. */
void testLayers(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    const int margin = backend == ARTF_BACKEND_VULKAN ? 3 : 2;

    /* a vertex's color is what shows */
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    addAt(s, addSquare(s, 0xFF804020u, 0xFF804020u, 0xFF804020u, 0xFF804020u), white, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 16), 0x80, margin) && near_(CHANNEL(a[mid], 8), 0x40, margin) && near_(CHANNEL(a[mid], 0), 0x20, margin));
    CHECK(a[mid] >> 24 == 255);
    artfSceneFree(s);

    /* and it goes between vertices as light does: half of red and half of blue, as a screen shows them */
    s = plainScene();
    white = whiteMaterial(s);
    addAt(s, addSquare(s, 0xFFFF0000u, 0xFF0000FFu, 0xFF0000FFu, 0xFFFF0000u), white, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 16), shownByte(.5), 6) && near_(CHANNEL(a[mid], 0), shownByte(.5), 6) && CHANNEL(a[mid], 8) == 0);
    artfSceneFree(s);

    /* it multiplies the material, and its texture */
    s = plainScene();
    ArtfMaterialDesc half = {artfVec3(.25f, .25f, .25f), 0, 0, 1, artfVec3(0, 0, 0)};
    addAt(s, addSquare(s, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF808080u, 0xFF808080u), artfSceneAddMaterial(s, &half), 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(CHANNEL(a[mid], 8) < 128 && CHANNEL(a[mid], 8) > 80);
    artfSceneFree(s);

    /* opacity: over black, half of it */
    s = plainScene();
    white = whiteMaterial(s);
    ArtfMesh plain = addSquare(s, 0, 0, 0, 0);
    ArtfInstance front = addAt(s, plain, white, 0, 0, 0, 1);
    drawInto(r, s, a);
    const uint64_t solid = hashOf(a, (size_t)W * H);
    CHECK(a[mid] == 0xFFFFFFFFu);
    artfInstanceSetOpacity(s, front, .5f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), 128, margin) && a[mid] >> 24 == 255);
    /* what opacity 1 and 0 come to is the same bytes as never having a layer, or having nothing */
    artfInstanceSetOpacity(s, front, 1.f);
    drawInto(r, s, a);
    CHECK(hashOf(a, (size_t)W * H) == solid);
    artfInstanceSetOpacity(s, front, 0.f);
    drawInto(r, s, a);
    bool nothing = true;
    for (size_t i = 0; i < (size_t)W * H; ++i) nothing = nothing && a[i] == 0xFF000000u;
    CHECK(nothing);
    artfSceneFree(s);

    /* layers go by distance, whatever order they were added in; what is solid is behind or hides them */
    for (int order = 0; order < 2; ++order) {
        s = plainScene();
        white = whiteMaterial(s);
        ArtfMesh red = addSquare(s, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u), blue = addSquare(s, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu);
        ArtfInstance near1 = addAt(s, order ? blue : red, white, 0, 0, order ? 1.f : 0.f, 1), near2 = addAt(s, order ? red : blue, white, 0, 0, order ? 0.f : 1.f, 1);
        artfInstanceSetOpacity(s, near1, .5f);
        artfInstanceSetOpacity(s, near2, .5f);
        drawInto(r, s, a);
        CHECK(near_(CHANNEL(a[mid], 0), 128, margin) && near_(CHANNEL(a[mid], 16), 64, margin));
        artfSceneFree(s);
    }
    s = plainScene();
    white = whiteMaterial(s);
    ArtfMesh green = addSquare(s, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u);
    ArtfInstance hidden = addAt(s, addSquare(s, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u), white, 0, 0, 0, 1);
    artfInstanceSetOpacity(s, hidden, .5f);
    addAt(s, green, white, 0, 0, 1, 1); /* in front of the layer: nothing of it shows */
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF00FF00u);
    artfSceneFree(s);

    /* a fading object shows its nearest surface alone, not its insides */
    s = plainScene();
    white = whiteMaterial(s);
    ArtfTransform turned = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(1, 1, 0), .7f), artfVec3(1, 1, 1));
    ArtfInstance cube = artfSceneAddInstance(s, artfSceneAddCube(s, 2), white, &turned);
    drawInto(r, s, a);
    artfInstanceSetOpacity(s, cube, .5f);
    drawInto(r, s, b);
    int off = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) off = artfMaxi(off, abs(CHANNEL(a[i], 8) / 2 - CHANNEL(b[i], 8)));
    CHECK(off <= margin);
    artfSceneFree(s);

    /* alpha in a vertex's color: a layer without being asked, with no more opacity than it says */
    s = plainScene();
    white = whiteMaterial(s);
    addAt(s, addSquare(s, 0x80FFFFFFu, 0x80FFFFFFu, 0x80FFFFFFu, 0x80FFFFFFu), white, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), 128, margin));
    artfSceneFree(s);

    /* no background: what the frame has is what the scene covered, premultiplied */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetBackground(s, artfVec3(1, 0, 0));
    artfSceneSetBackgroundAlpha(s, 0);
    ArtfInstance layer = addAt(s, addSquare(s, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFF0000u), white, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(a[0] == 0 && a[(size_t)W * H - 1] == 0);
    CHECK(a[mid] >> 24 == 255);
    artfInstanceSetOpacity(s, layer, .5f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 24), 128, margin));
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        const int alpha = CHANNEL(a[i], 24);
        if (CHANNEL(a[i], 16) > alpha + 1 || CHANNEL(a[i], 8) > alpha + 1 || CHANNEL(a[i], 0) > alpha + 1) { CHECK(!"past its alpha"); break; }
    }
    /* half a background is that much of its color, and of alpha */
    artfSceneSetBackgroundAlpha(s, .5f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[0], 24), 128, 1) && near_(CHANNEL(a[0], 16), 128, 1) && CHANNEL(a[0], 8) == 0);
    artfSceneFree(s);

    /* an overlay over nothing keeps its own alpha, and adds to what is there */
    s = plainScene();
    artfSceneSetBackgroundAlpha(s, 0);
    uint32_t* over = artfCalloc((size_t)W * H, 4);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x) over[y * W + x] = 0x80400000u;
    ArtfOverlay overlay = {over, W, H, 0, false, NULL, 0};
    artfRendererSetOverlay(r, &overlay);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[0], 24), 0x80, 1) && near_(CHANNEL(a[0], 16), 0x40, 1));
    CHECK(a[(size_t)100 * W + 100] == 0);
    artfRendererSetOverlay(r, NULL);
    artfFree(over);
    artfSceneFree(s);

    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* What a material can say about alpha and backs, on one backend. */
void testMaterials(ArtfBackend backend, bool antialias) {
    ArtfRendererOptions o = options(backend, 0, false);
    o.antialias = antialias;
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    const int margin = backend == ARTF_BACKEND_VULKAN ? 3 : 2;
    /* a texture whose left half has alpha 0, and its right half 255; both white */
    static uint32_t px[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) px[y * 64 + x] = x < 32 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    const size_t left = (size_t)(H / 2) * W + W / 2 - 20, right = (size_t)(H / 2) * W + W / 2 + 20;

    /* opaque: alpha counts for nothing */
    ArtfScene* s = plainScene();
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTexture(s, px, 64, 64, 0), 0, 1, artfVec3(0, 0, 0)};
    ArtfMesh square = addSquare(s, 0, 0, 0, 0), under = addSquare(s, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u);
    ArtfMaterial m = artfSceneAddMaterial(s, &d), plain = whiteMaterial(s);
    addAt(s, under, plain, 0, 0, -1, 2);
    addAt(s, square, m, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(a[left] == 0xFFFFFFFFu && a[right] == 0xFFFFFFFFu);
    CHECK(s->cutoutCount == 0 && s->translucentCount == 0);

    /* a mask: what is below shows through its holes, and the surface hides it elsewhere */
    d.alphaMode = ARTF_ALPHA_MASK;
    artfMaterialSet(s, m, &d);
    CHECK(s->cutoutCount == 1);
    drawInto(r, s, a);
    CHECK(a[left] == 0xFF00FF00u && a[right] == 0xFFFFFFFFu);

    /* blending by the texture's alpha, and by the material's own */
    d.alphaMode = ARTF_ALPHA_BLEND;
    d.transparency = .5f;
    artfMaterialSet(s, m, &d);
    CHECK(s->cutoutCount == 0 && s->translucentCount == 1);
    drawInto(r, s, a);
    CHECK(a[left] == 0xFF00FF00u);
    CHECK(near_(CHANNEL(a[right], 16), 128, margin) && near_(CHANNEL(a[right], 8), 255, margin));
    artfSceneFree(s);

    /* backs: not drawn, unless asked; then lit as a front */
    s = plainScene();
    ArtfMaterialDesc two = {artfVec3(.5f, .5f, .5f), 0, 0, 1, artfVec3(0, 0, 0)};
    artfSceneSetAmbient(s, artfVec3(1, 1, 1), artfVec3(0, 0, 0)); /* (a back would be dark if it were not turned) */
    const ArtfTransform turned = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 1, 0), 3.14159265f), artfVec3(1, 1, 1));
    ArtfMaterial back = artfSceneAddMaterial(s, &two);
    artfSceneAddInstance(s, square = addSquare(s, 0, 0, 0, 0), back, &turned);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF000000u);
    two.doubleSided = true;
    artfMaterialSet(s, back, &two);
    CHECK(s->cutoutCount == 1);
    drawInto(r, s, a);
    const uint32_t shown = a[mid];
    CHECK(CHANNEL(shown, 8) > 100);
    artfSceneFree(s);

    /* a material with a mask and an instance that fades: its holes stay as they were */
    s = plainScene();
    ArtfMaterialDesc cut = {artfVec3(1, 1, 1), artfSceneAddTexture(s, px, 64, 64, 0), 0, 1, artfVec3(0, 0, 0), ARTF_ALPHA_MASK, 0, 0, false};
    ArtfMaterial leaf = artfSceneAddMaterial(s, &cut);
    ArtfInstance i = addAt(s, addSquare(s, 0, 0, 0, 0), leaf, 0, 0, 0, 1);
    artfInstanceSetOpacity(s, i, .5f);
    drawInto(r, s, a);
    CHECK(a[left] == 0xFF000000u);
    CHECK(near_(CHANNEL(a[right], 8), 128, margin));
    artfSceneFree(s);

    artfFree(a);
    artfRendererFree(r);
}

void testSamePixelsOfLayers(void) {
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {3, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {4, ARTF_SIMD_PORTABLE}, {2, ARTF_SIMD_128}, {3, ARTF_SIMD_256}};
    for (int antialias = 0; antialias < 2; ++antialias)
        for (int clear = 0; clear < 2; ++clear) {
            ArtfScene* s = layeredScene();
            if (clear) artfSceneSetBackgroundAlpha(s, 0);
            uint64_t reference = 0;
            for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
                ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
                o.simd = runs[i].simd;
                o.antialias = antialias != 0;
                ArtfRenderer* r = artfRendererNew(&o);
                drawInto(r, s, frame);
                const uint64_t h = hashOf(frame, (size_t)W * H);
                if (!i) reference = h;
                CHECK(h == reference);
                artfRendererFree(r);
            }
            /* what occlusion does never shows, with layers and samples too */
            artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
            ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
            o.antialias = antialias != 0;
            ArtfRenderer* r = artfRendererNew(&o);
            drawInto(r, s, frame);
            CHECK(hashOf(frame, (size_t)W * H) == reference);
            artfRendererFree(r);
            artfSceneFree(s);
        }
    artfFree(frame);
}

void testAntialias(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* hard = artfRendererNew(&o);
    if (!hard) return;
    o.antialias = true;
    ArtfRenderer* soft = artfRendererNew(&o);
    CHECK(soft != NULL);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);

    /* a white shape on nothing: without samples each pixel is in or out, with them an edge is between */
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    artfSceneSetBackgroundAlpha(s, 0);
    ArtfTransform turned = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 0, 1), .4f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), white, &turned);
    drawInto(hard, s, a);
    drawInto(soft, s, b);
    int between = 0, inside = 0, moved = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        const int ha = CHANNEL(a[i], 24), sa = CHANNEL(b[i], 24);
        CHECK(ha == 0 || ha == 255);
        between += sa > 0 && sa < 255;
        inside += ha == 255 && sa == 255;
        moved += abs(ha - sa) > 192; /* (nothing can be as far off as that) */
        if (sa && sa < 255) { CHECK(sa == 64 || sa == 128 || sa == 191 || near_(sa, 64, 1) || near_(sa, 128, 1) || near_(sa, 191, 1)); }
        if (CHANNEL(b[i], 8) > sa + 1) { CHECK(!"past its alpha"); break; }
    }
    CHECK(between > 200);
    CHECK(inside > 2000);
    CHECK(moved == 0);
    artfSceneFree(s);

    /* over a solid, a surface's inside is what it was without samples */
    s = richScene();
    drawInto(hard, s, a);
    drawInto(soft, s, b);
    int same = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) same += near_(CHANNEL(a[i], 8), CHANNEL(b[i], 8), 2) && near_(CHANNEL(a[i], 16), CHANNEL(b[i], 16), 2);
    CHECK(same > W * H * 9 / 10);
    artfSceneFree(s);

    /* triangles that share an edge leave nothing between them, in any sample */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetBackgroundAlpha(s, 0);
    enum { N = 40 };
    static ArtfVertex v[(N + 1) * (N + 1)];
    static uint32_t idx[N * N * 6];
    uint32_t n = 0, state = 12345;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            state = state * 1664525u + 1013904223u;
            const float jx = x && x < N ? (float)(state >> 8 & 255) / 700.f : 0, jy = y && y < N ? (float)(state >> 16 & 255) / 700.f : 0;
            ArtfVertex q = {((float)x + jx) / N * 40.f - 20.f, ((float)y + jy) / N * 40.f - 20.f, 0, 0, 0, 1, 0, 0, 0};
            v[y * (N + 1) + x] = q;
        }
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            const uint32_t p = y * (N + 1) + x, q = p + 1, d = p + N + 1, e = d + 1;
            idx[n++] = p; idx[n++] = q; idx[n++] = e;
            idx[n++] = p; idx[n++] = e; idx[n++] = d;
        }
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    addAt(s, artfSceneAddMesh(s, v, (N + 1) * (N + 1), idx, n), white, 0, 0, 0, 1);
    drawInto(soft, s, b);
    bool whole = true;
    for (size_t i = 0; i < (size_t)W * H; ++i) whole = whole && b[i] >> 24 == 255;
    CHECK(whole);
    artfSceneFree(s);

    /* what falls between pixels' centers is not lost: this sliver holds a row of samples and no center */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetBackgroundAlpha(s, 0);
    const float thin = .002f;
    const ArtfVertex sliver[4] = {{-1, -thin, 0, 0, 0, 1, 0, 0, 0}, {1, -thin, 0, 0, 0, 1, 0, 0, 0}, {1, thin, 0, 0, 0, 1, 0, 0, 0}, {-1, thin, 0, 0, 0, 1, 0, 0, 0}};
    const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
    addAt(s, artfSceneAddMesh(s, sliver, 4, quad, 6), white, 0, .01024f, 0, 1);
    drawInto(hard, s, a);
    drawInto(soft, s, b);
    int touched = 0, touchedHard = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        touched += b[i] != 0;
        touchedHard += a[i] != 0;
    }
    CHECK(touched > 20);
    CHECK(touchedHard == 0);
    artfSceneFree(s);

    /* a new size, and a scene with layers */
    s = layeredScene();
    artfRendererResize(soft, W / 2, H / 2);
    CHECK(artfRendererDraw(soft, s));
    CHECK(artfRendererDraw(soft, s));
    CHECK(artfRendererReadPixels(soft, b));
    artfRendererResize(soft, W, H);
    drawInto(soft, s, b);
    artfSceneFree(s);

    artfFree(a);
    artfFree(b);
    artfRendererFree(hard);
    artfRendererFree(soft);
}

/* The two backends draw these the same. */
void testLayersBothBackends(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* probe = artfRendererNew(&o);
    if (!probe) return;
    artfRendererFree(probe);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    for (int antialias = 0; antialias < 2; ++antialias)
        for (int clear = 0; clear < 2; ++clear) {
            o = options(ARTF_BACKEND_VULKAN, 0, false);
            o.antialias = antialias != 0;
            ArtfRenderer* gpu = artfRendererNew(&o);
            o.backend = ARTF_BACKEND_SOFTWARE;
            ArtfRenderer* cpu = artfRendererNew(&o);
            ArtfScene* s = layeredScene();
            if (clear) artfSceneSetBackgroundAlpha(s, 0);
            drawInto(gpu, s, a);
            drawInto(cpu, s, b);
            const double far = differentShare(a, b, 24);
            int alphaOff = 0;
            for (size_t i = 0; i < (size_t)W * H; ++i) alphaOff += abs(CHANNEL(a[i], 24) - CHANNEL(b[i], 24)) > 24;
            printf("layers, %s, %s: pixels that differ by more than 24 of 255: %.2f%%, in alpha %.2f%%\n", antialias ? "antialiased" : "hard edges", clear ? "no background" : "a background", far * 100, (double)alphaOff / (W * H) * 100);
            CHECK(far < .02);
            CHECK(alphaOff < W * H / 50);
            /* occlusion, on the GPU too, with samples or without */
            artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
            drawInto(gpu, s, b);
            CHECK(differentShare(a, b, 0) < .0001);
            artfRendererFree(gpu);
            artfRendererFree(cpu);
            artfSceneFree(s);
        }
    artfFree(a);
    artfFree(b);
}

/* Two layers that cut through each other: each is in front on its own side. */
void testLayersSorted(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    const ArtfVertex v[8] = {{-2, -2, -1, 0, 0, 1, 0, 0, 0}, {2, -2, 1, 0, 0, 1, 0, 0, 0}, {2, 2, 1, 0, 0, 1, 0, 0, 0}, {-2, 2, -1, 0, 0, 1, 0, 0, 0},
                             {-2, -2, 1, 0, 0, 1, 0, 0, 0}, {2, -2, -1, 0, 0, 1, 0, 0, 0}, {2, 2, -1, 0, 0, 1, 0, 0, 0}, {-2, 2, 1, 0, 0, 1, 0, 0, 0}};
    const uint32_t first[6] = {0, 1, 2, 0, 2, 3}, second[6] = {4, 5, 6, 4, 6, 7};
    ArtfMaterialDesc red = artfMaterialDefaults(), blue = artfMaterialDefaults();
    red.baseColor = artfVec3(1, 0, 0);
    blue.baseColor = artfVec3(0, 0, 1);
    red.alphaMode = blue.alphaMode = ARTF_ALPHA_BLEND;
    red.transparency = blue.transparency = .25f;
    red.specular = blue.specular = 0;
    red.doubleSided = blue.doubleSided = true;
    const ArtfTransform at = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddMesh(s, v, 8, first, 6), artfSceneAddMaterial(s, &red), &at);
    artfSceneAddInstance(s, artfSceneAddMesh(s, v, 8, second, 6), artfSceneAddMaterial(s, &blue), &at);
    artfSceneSetFeature(s, ARTF_FEATURE_LAYERS_SORTED, true);
    drawInto(r, s, a);
    /* on the right the first is nearer, on the left the second: more of what is in front shows */
    const uint32_t left = a[(size_t)(H / 2) * W + W / 2 - 30], right = a[(size_t)(H / 2) * W + W / 2 + 30];
    printf("sorted layers: on the left red %u, blue %u; on the right red %u, blue %u\n", CHANNEL(left, 16), CHANNEL(left, 0), CHANNEL(right, 16), CHANNEL(right, 0));
    CHECK(CHANNEL(right, 16) > CHANNEL(right, 0) + 40);
    CHECK(CHANNEL(left, 0) > CHANNEL(left, 16) + 40);

    /* the same with four samples a pixel, and it agrees with what one sample gave, but for edges */
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfRendererOptions smooth = o;
    smooth.antialias = true;
    ArtfRenderer* many = artfRendererNew(&smooth);
    drawInto(many, s, b);
    const double apart = differentShare(a, b, 24);
    printf("sorted layers, with samples and without, pixels that differ by more than 24 of 255: %.2f%%\n", apart * 100);
    CHECK(apart < .02);
    artfRendererFree(many);
    artfFree(b);
    artfSceneFree(s);
    artfFree(a);
    artfRendererFree(r);
}

/* A layer behind what is solid, with four samples a pixel: at the edge it stays behind, on both backends. */
void testLayersBehindSolid(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.antialias = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    ArtfMaterialDesc red = artfMaterialDefaults(), solid = artfMaterialDefaults();
    red.baseColor = artfVec3(1, .05f, .05f);
    red.alphaMode = ARTF_ALPHA_BLEND;
    red.transparency = .2f;
    red.specular = solid.specular = 0;
    solid.baseColor = artfVec3(.1f, .2f, 1);
    addAt(s, artfSceneAddCube(s, 6), artfSceneAddMaterial(s, &red), 0, 0, -6, 1);
    addAt(s, artfSceneAddSphere(s, 1.5f, 32, 48), artfSceneAddMaterial(s, &solid), 0, 0, 0, 1);
    /* (the backends do not agree on every edge pixel in any case: this may add nothing to that) */
    drawInto(gpu, s, a);
    drawInto(cpu, s, b);
    const double whole = differentShare(a, b, 24);
    artfSceneSetFeature(s, ARTF_FEATURE_LAYERS_SORTED, true);
    drawInto(gpu, s, a);
    drawInto(cpu, s, b);
    const double apart = differentShare(a, b, 24);
    printf("a layer behind an edge, pixels that differ by more than 24 of 255: %.3f%%, and %.3f%% with layers whole\n", apart * 100, whole * 100);
    CHECK(apart <= whole * 1.1 + .0002);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(cpu);
    artfRendererFree(gpu);
}

/* Decals: a picture over the floor, through a box; the same on both backends, and gone when removed. */
void testDecals(void) {
    ArtfScene* s = plainScene();
    setCamera(s, 0, 4, 3, 0, 0, 0);
    const ArtfTransform floor = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 20, 1), whiteMaterial(s), &floor);
    /* a checker, red where it shows and clear between */
    static uint32_t checker[32 * 32];
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) checker[y * 32 + x] = ((x / 8 + y / 8) & 1) ? 0xFFFF2020u : 0x00000000u;
    ArtfDecalDesc d;
    memset(&d, 0, sizeof d);
    d.transform = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 1, 0), .3f), artfVec3(2, 1, 2));
    d.texture = artfSceneAddTexture(s, checker, 32, 32, 0);
    d.color = artfVec3(1, 1, 1);
    d.opacity = 1;
    const ArtfDecal decal = artfSceneAddDecal(s, &d);
    CHECK(decal != 0);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    drawInto(cpu, s, a);
    /* red and white squares where the box meets the floor */
    int red = 0;
    for (int i = 0; i < W * H; ++i) red += (a[i] >> 16 & 255) > 200 && (a[i] >> 8 & 255) < 120;
    printf("a decal: %d pixels red\n", red);
    CHECK(red > W * H / 50);
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("a decal, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        FLIP_ALIKE(0.0104); /* (measured 0.0004 on a Radeon RX 9060 XT) */
        CHECK(apart < .01);
        artfRendererFree(gpu);
    }
    artfSceneRemoveDecal(s, decal);
    drawInto(cpu, s, b);
    red = 0;
    for (int i = 0; i < W * H; ++i) red += (b[i] >> 16 & 255) > 200 && (b[i] >> 8 & 255) < 120;
    CHECK(red == 0);
    artfRendererFree(cpu);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
}

/* Layers by transmittance: three that do not cut through each other, one behind another, come out as they do whole, the
 * farthest first (when what is in front of a surface is all of its column, the weights are what the layers over it
 * leave); and the CPU's and the GPU's are the same. */
void testLayersTransmittance(void) {
    ArtfScene* s = plainScene();
    const uint32_t colors[3] = {0xFFFF2020u, 0xFF20FF20u, 0xFF2020FFu};
    for (int k = 0; k < 3; ++k) {
        const float z = (float)k - 1.f;
        const ArtfVertex v[4] = {{-2, -2, z, 0, 0, 1, 0, 0, 0}, {2, -2, z, 0, 0, 1, 0, 0, 0}, {2, 2, z, 0, 0, 1, 0, 0, 0}, {-2, 2, z, 0, 0, 1, 0, 0, 0}};
        const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        ArtfMaterialDesc m = artfMaterialDefaults();
        m.baseColor = artfVec3((float)(colors[k] >> 16 & 255) / 255.f, (float)(colors[k] >> 8 & 255) / 255.f, (float)(colors[k] & 255) / 255.f);
        m.alphaMode = ARTF_ALPHA_BLEND;
        m.transparency = .4f + .1f * (float)k;
        m.specular = 0;
        m.doubleSided = true;
        const ArtfTransform at = artfTransformAt(0, 0, 0, 1);
        artfSceneAddInstance(s, artfSceneAddMesh(s, v, 4, idx, 6), artfSceneAddMaterial(s, &m), &at);
    }
    uint32_t* whole = artfMalloc((size_t)W * H * 4);
    uint32_t* cpu = artfMalloc((size_t)W * H * 4);
    uint32_t* gpu = artfMalloc((size_t)W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    drawInto(r, s, whole);
    artfSceneSetFeature(s, ARTF_FEATURE_LAYERS_SORTED, true);
    drawInto(r, s, cpu);
    const double apart = differentShare(whole, cpu, 8);
    printf("layers by transmittance against whole, the farthest first: %.3f%% of pixels differ by more than 8 of 255\n", apart * 100);
    CHECK(apart < .01);
    artfRendererFree(r);
    o.backend = ARTF_BACKEND_VULKAN;
    r = artfRendererNew(&o);
    if (r) {
        drawInto(r, s, gpu);
        const double backends = differentShare(cpu, gpu, 8);
        /* (what the backends differ by whole, at edges and in light: this may add little to it) */
        artfSceneSetFeature(s, ARTF_FEATURE_LAYERS_SORTED, false);
        drawInto(r, s, cpu);
        const double wholeBackends = differentShare(whole, cpu, 8);
        printf("layers by transmittance, the GPU against the CPU: %.3f%% of pixels differ by more than 8 of 255, and %.3f%% whole\n", backends * 100, wholeBackends * 100);
        CHECK(backends <= wholeBackends * 1.1 + .002);
        artfRendererFree(r);
    }
    artfFree(whole);
    artfFree(cpu);
    artfFree(gpu);
    artfSceneFree(s);
}
