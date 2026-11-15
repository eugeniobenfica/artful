/* artful-tests: what comes and goes: removing, residency, streaming. */
#include "tests.h"

/* ---- what comes and goes ---- */

void testRemoving(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    artfSetLog(countLog, &logged);
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    /* in use, it stays; once nothing uses it, it goes; and nothing else is touched */
    ArtfMesh keep = addSquare(s, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu);
    addAt(s, keep, white, 0, 0, -1, 2);
    ArtfMesh m = addSquare(s, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u);
    ArtfInstance i = addAt(s, m, white, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFFFF0000u);
    logged = 0;
    CHECK(!artfSceneRemoveMesh(s, m));
    CHECK(logged == 1);
    artfSceneRemoveInstance(s, i);
    CHECK(artfSceneRemoveMesh(s, m));
    CHECK(!artfSceneRemoveMesh(s, m));
    CHECK(artfSceneAddInstance(s, m, white, NULL) == 0);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF0000FFu);

    /* many come and go, with what stays right; this is what a GPU's memory must take */
    ArtfSceneMemory before, after;
    artfSceneMemory(s, &before);
    for (int round = 0; round < 40; ++round) {
        ArtfMesh sphere = artfSceneAddSphere(s, 1, 8 + round % 5, 12);
        ArtfMesh green = addSquare(s, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u);
        ArtfInstance one = addAt(s, sphere, white, 0, 3, 0, 1), two = addAt(s, green, white, 0, 0, 0, 1);
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        CHECK(a[mid] == 0xFF00FF00u);
        artfSceneRemoveInstance(s, one);
        artfSceneRemoveInstance(s, two);
        CHECK(artfSceneRemoveMesh(s, sphere));
        CHECK(artfSceneRemoveMesh(s, green));
    }
    artfSceneMemory(s, &after);
    CHECK(after.meshes == before.meshes);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF0000FFu);
    /* traced, as well */
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 2, 0};
    artfSceneSetTrace(s, &rays);
    drawSamples(r, s, 2, a);
    CHECK(a[mid] == 0xFF0000FFu);
    artfSceneFree(s);

    /* a texture's place is used again, and what uses a texture keeps it */
    s = plainScene();
    ArtfTexture t = oneColor(s, 0xFFFF0000u);
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), t, 0, 1, artfVec3(0, 0, 0)};
    ArtfMaterial tm = artfSceneAddMaterial(s, &d);
    addAt(s, addSquare(s, 0, 0, 0, 0), tm, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFFFF0000u);
    CHECK(!artfSceneRemoveTexture(s, t));
    d.texture = 0;
    artfMaterialSet(s, tm, &d);
    CHECK(artfSceneRemoveTexture(s, t));
    ArtfTexture again = oneColor(s, 0xFF00FF00u);
    CHECK(again == t);
    d.texture = again;
    artfMaterialSet(s, tm, &d);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF00FF00u);
    artfSceneFree(s);

    /* a bent instance's copy goes with it */
    s = plainScene();
    white = whiteMaterial(s);
    ArtfMesh strip = addStrip(s);
    ArtfInstance one = addAt(s, strip, white, 0, 0, 0, 1);
    CHECK(artfRendererDraw(r, s));
    CHECK(!artfSceneRemoveMesh(s, strip));
    artfSceneRemoveInstance(s, one);
    CHECK(s->deforming.len == 0);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfSceneRemoveMesh(s, strip));
    artfSceneFree(s);
    artfSetLog(NULL, NULL);
    artfFree(a);
    artfRendererFree(r);
}

void testPrepared(void) {
    /* what is prepared is what is added, with its levels */
    ArtfScene* a = artfSceneNew();
    ArtfScene* b = artfSceneNew();
    artfSceneSetFeature(a, ARTF_FEATURE_LOD, true);
    enum { N = 24 };
    static ArtfVertex v[(N + 1) * (N + 1)];
    static uint32_t idx[N * N * 6];
    uint32_t n = 0;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            const ArtfVertex q = {(float)x, (float)y, 0, 0, 0, 1, 0, 0, 0};
            v[y * (N + 1) + x] = q;
        }
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            const uint32_t p = y * (N + 1) + x, q = p + 1, d = p + N + 1, e = d + 1;
            idx[n++] = p; idx[n++] = q; idx[n++] = e;
            idx[n++] = p; idx[n++] = e; idx[n++] = d;
        }
    const ArtfMesh direct = artfSceneAddMesh(a, v, (N + 1) * (N + 1), idx, n);
    const ArtfMesh made = artfSceneAddPreparedMesh(b, artfPrepareMesh(v, (N + 1) * (N + 1), idx, n, true));
    CHECK(direct && made);
    CHECK(a->meshes.items[direct - 1].lodCount == b->meshes.items[made - 1].lodCount);
    CHECK(b->meshes.items[made - 1].lodCount > 0);
    static uint32_t px[32 * 32];
    for (int i = 0; i < 32 * 32; ++i) px[i] = 0xFF204060u;
    const ArtfTexture t = artfSceneAddPreparedTexture(b, artfPrepareTexture(px, 32, 32, 0));
    CHECK(t && b->textures.items[t].mipCount == 6);
    artfSceneFree(a);
    artfSceneFree(b);
}

static int reports;
static void onResidency(void* user, ArtfTexture t, int mip) {
    ArtfScene* s = user;
    ++reports;
    artfTextureSetResidentMip(s, t, mip);
}

void testResidency(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    enum { SIDE = 256 };
    static uint32_t px[SIDE * SIDE];
    for (int i = 0; i < SIDE * SIDE; ++i) px[i] = 0xFF4080C0u;
    ArtfScene* s = plainScene();
    const ArtfTexture t = artfSceneAddTexture(s, px, SIDE, SIDE, 0);
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), t, 0, 1, artfVec3(0, 0, 0)};
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
    ArtfSceneMemory m;
    artfSceneMemory(s, &m);
    CHECK(m.textures == m.residentTextures && m.textures > (size_t)SIDE * SIDE * 4);

    /* none: nothing happens by itself, and a call does */
    CHECK(artfRendererDraw(r, s));
    CHECK(s->textures.items[t].residentMip == 0);
    artfTextureSetResidentMip(s, t, 3);
    artfSceneMemory(s, &m);
    CHECK(m.residentTextures < m.textures / 32);
    drawInto(r, s, a);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    CHECK(near_(CHANNEL(a[mid], 8), 0x80, 2) && near_(CHANNEL(a[mid], 16), 0x40, 2));
    artfTextureSetResidentMip(s, t, 0);

    /* assisted: Artful says, and the program does */
    ArtfResidencyDesc desc = {ARTF_RESIDENCY_ASSISTED, 4096, 1, onResidency, s};
    reports = 0;
    artfSceneSetResidency(s, &desc);
    drawInto(r, s, a);
    CHECK(reports > 0);
    artfSceneMemory(s, &m);
    CHECK(m.residentTextures <= 4096);
    artfTextureSetResidentMip(s, t, 0);

    /* automatic: within its budget, and what is far needs less */
    desc.mode = ARTF_RESIDENCY_AUTOMATIC;
    desc.report = NULL;
    artfSceneSetResidency(s, &desc);
    drawInto(r, s, a);
    artfSceneMemory(s, &m);
    CHECK(m.residentTextures <= 4096);
    CHECK(near_(CHANNEL(a[mid], 8), 0x80, 2));
    desc.budget = 0;
    artfSceneSetResidency(s, &desc);
    setCamera(s, 0, 0, 200, 0, 0, 0);
    drawInto(r, s, a);
    CHECK(s->textures.items[t].residentMip >= 3);
    setCamera(s, 0, 0, 2, 0, 0, 0);
    drawInto(r, s, a);
    CHECK(s->textures.items[t].residentMip == 0);
    artfSceneFree(s);
    artfFree(a);
    artfRendererFree(r);
}

/* A texture whose mipmaps come when they are needed: each a color of its own, to see which is drawn. */
static int mipReads;
static bool readMip(void* user, int mip, uint32_t* pixels) {
    (void)user;
    ++mipReads;
    const int side = artfMaxi(64 >> mip, 1);
    for (int i = 0; i < side * side; ++i) pixels[i] = 0xFF000000u | (uint32_t)(mip * 40) << 8 | 0x10;
    return true;
}

void testStreamedTexture(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    mipReads = 0;
    const ArtfTexture t = artfSceneAddTextureStreamed(s, 64, 64, 0, 3, readMip, NULL);
    CHECK(t && mipReads == 4); /* (eight, four, two and one pixels) */
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), t, 0, 1, artfVec3(0, 0, 0)};
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
    ArtfSceneMemory m;
    artfSceneMemory(s, &m);
    const size_t few = m.residentTextures;
    CHECK(few < 64 * 64);
    drawInto(r, s, a);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    CHECK(near_(CHANNEL(a[mid], 8), 120, 2));

    /* more of it: the rest is read, what was there is not */
    artfTextureSetResidentMip(s, t, 0);
    CHECK(mipReads == 7);
    drawInto(r, s, a);
    CHECK(CHANNEL(a[mid], 8) < 120);
    artfSceneMemory(s, &m);
    CHECK(m.residentTextures > few);

    /* less: the memory goes */
    artfTextureSetResidentMip(s, t, 5);
    artfSceneMemory(s, &m);
    CHECK(m.residentTextures < few);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), 200, 2));
    artfSceneFree(s);
    artfFree(a);
    artfRendererFree(r);
}

/* What a material in C reads keeps its detail, however little the material itself would need. */
void testResidencyGraph(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    enum { SIDE = 256 };
    static uint32_t px[SIDE * SIDE];
    for (int y = 0; y < SIDE; ++y)
        for (int x = 0; x < SIDE; ++x) px[y * SIDE + x] = (x / 4 + y / 4) & 1 ? 0xFFFFFFFFu : 0xFF000000u;
    ArtfScene* s = plainScene();
    const ArtfTexture t = artfSceneAddTexture(s, px, SIDE, SIDE, 0);
    ArtfGraph* g = artfGraphNew();
    artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprTexture(g, t, artfExprUv(g)));
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.graph = artfSceneAddGraph(s, g);
    artfGraphFree(g);
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
    ArtfResidencyDesc desc = {ARTF_RESIDENCY_AUTOMATIC, 0, 1, NULL, NULL};
    artfSceneSetResidency(s, &desc);
    for (int i = 0; i < 3; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(s->textures.items[t].residentMip == 0);
    artfSceneFree(s);
    artfRendererFree(r);
}

/* A place that was left is taken again, and what is drawn from it is what is there now. */
void testMeshPlaces(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    ArtfMesh mesh = artfSceneAddSphere(s, 1, 16, 24);
    ArtfTransform at = artfTransformAt(0, 0, 0, 1);
    ArtfInstance i = artfSceneAddInstance(s, mesh, white, &at);
    drawInto(r, s, a);
    const size_t places = s->meshes.len;
    for (int round = 0; round < 6; ++round) {
        artfSceneRemoveInstance(s, i);
        CHECK(artfSceneRemoveMesh(s, mesh));
        mesh = artfSceneAddCube(s, 1.2f);
        i = artfSceneAddInstance(s, mesh, white, &at);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 24) > .001);
        artfSceneRemoveInstance(s, i);
        CHECK(artfSceneRemoveMesh(s, mesh));
        mesh = artfSceneAddSphere(s, 1, 16, 24);
        i = artfSceneAddInstance(s, mesh, white, &at);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 24) < .001);
    }
    CHECK(s->meshes.len <= places + 1);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* A ball of 48 x 64 squares, the same each time it is read; how many times it was. */
static uint32_t ballVertices(void) { return 49 * 65; }
static uint32_t ballIndices(void) { return 48 * 64 * 6; }
static bool readBall(void* user, ArtfVertex* v, uint32_t* idx) {
    ++*(int*)user;
    for (int r = 0; r <= 48; ++r)
        for (int a = 0; a <= 64; ++a) {
            const float up = (float)r / 48 * 3.1415927f, turn = (float)a / 64 * 6.2831853f;
            const float y = artfCosFast(up), ring = artfSinFast(up);
            const ArtfVertex q = {ring * artfCosFast(turn), y, ring * artfSinFast(turn), ring * artfCosFast(turn), y, ring * artfSinFast(turn), (float)a / 64, (float)r / 48, 0};
            v[r * 65 + a] = q;
        }
    uint32_t n = 0;
    for (uint32_t r = 0; r < 48; ++r)
        for (uint32_t a = 0; a < 64; ++a) {
            const uint32_t p = r * 65 + a, q = p + 65;
            idx[n++] = p; idx[n++] = p + 1; idx[n++] = q + 1;
            idx[n++] = p; idx[n++] = q + 1; idx[n++] = q;
        }
    return true;
}

/* Meshes that come and go: only the lightest level held at first; near, all of it, read again, and the same bytes as
 * the mesh given whole; far, the heavy levels leave, memory too; near again, the same bytes. A budget holds them out. */
void testMeshStreaming(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    int reads = 0;
    ArtfScene* whole = plainScene();
    artfSceneSetFeature(whole, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says) */
    ArtfVertex* v = artfMalloc(ballVertices() * sizeof *v);
    uint32_t* idx = artfMalloc(ballIndices() * 4);
    readBall(&reads, v, idx);
    addAt(whole, artfSceneAddMesh(whole, v, ballVertices(), idx, ballIndices()), whiteMaterial(whole), 0, 0, 0, 1);
    setCamera(whole, 0, 0, 3, 0, 0, 0);
    drawInto(r, whole, a);
    artfFree(v);
    artfFree(idx);

    reads = 0;
    ArtfScene* s = plainScene();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says) */
    const ArtfMesh mesh = artfSceneAddMeshStreamed(s, ballVertices(), ballIndices(), readBall, &reads);
    CHECK(mesh && reads == 1);
    const int lightest = (int)s->meshes.items[mesh - 1].lodCount;
    CHECK(lightest >= 2 && artfMeshResidentLevel(s, mesh) == lightest);
    ArtfSceneMemory memory;
    artfSceneMemory(s, &memory);
    const size_t held = memory.residentMeshes;
    CHECK(held < memory.meshes / 2);
    addAt(s, mesh, whiteMaterial(s), 0, 0, 0, 1);
    ArtfResidencyDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.mode = ARTF_RESIDENCY_AUTOMATIC;
    rd.interval = 1;
    rd.meshes = true;
    artfSceneSetResidency(s, &rd);
    setCamera(s, 0, 0, 3, 0, 0, 0);
    drawInto(r, s, b);
    CHECK(artfMeshResidentLevel(s, mesh) == 0 && reads == 2);
    CHECK(hashOf(a, (size_t)W * H) == hashOf(b, (size_t)W * H));
    setCamera(s, 0, 0, 300, 0, 0, 0);
    drawInto(r, s, b);
    CHECK(artfMeshResidentLevel(s, mesh) > 0);
    artfSceneMemory(s, &memory);
    CHECK(memory.residentMeshes < memory.meshes);
    setCamera(s, 0, 0, 3, 0, 0, 0);
    drawInto(r, s, b);
    CHECK(artfMeshResidentLevel(s, mesh) == 0 && reads == 3);
    CHECK(hashOf(a, (size_t)W * H) == hashOf(b, (size_t)W * H));
    /* over the budget: the lightest, whatever the distance, and drawn */
    rd.meshBudget = 1;
    artfSceneSetResidency(s, &rd);
    drawInto(r, s, b);
    CHECK(artfMeshResidentLevel(s, mesh) == lightest);
    CHECK(litCount(b) > 1000 && hashOf(a, (size_t)W * H) != hashOf(b, (size_t)W * H));
    /* traced, all of it */
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 1, 0};
    artfSceneSetTrace(s, &rays);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfMeshResidentLevel(s, mesh) == 0);
    artfSceneFree(s);
    artfSceneFree(whole);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}
