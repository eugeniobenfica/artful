/* artful-tests: the mesh's data, the CPU's promises, levels of detail, occlusion, handles and lifetimes. */
#include "tests.h"

static int compareEdgeKeys(const void* a, const void* b) {
    const uint64_t *x = a, *y = b;
    if (x[0] != y[0]) return x[0] < y[0] ? -1 : 1;
    return x[1] < y[1] ? -1 : x[1] > y[1];
}

/* ---- the mesh's data ---- */

void testPacking(void) {
    /* halves and the octahedron come back close to what went in */
    ArtfScene* s = artfSceneNew();
    const ArtfVertex v[3] = {{0, 0, 0, 0, 0, 1, .25f, .5f}, {10, 0, 0, .6f, 0, .8f, 1.5f, -2.f}, {0, 4, 2, 0, -1, 0, 0, 1}};
    const uint32_t idx[3] = {0, 1, 2};
    ArtfMesh mesh = artfSceneAddMesh(s, v, 3, idx, 3);
    CHECK(mesh == 1);
    const ArtfMeshData* m = &s->meshes.items[0];
    CHECK(m->vertexCount == 3 && m->indexCount == 3 && m->meshletCount == 1);
    for (uint32_t i = 0; i < 3; ++i) {
        const ArtfPackedVertex* p = &m->vertices[i];
        const float u = artfHalfToFloat(p->u), w = artfHalfToFloat(p->v);
        const ArtfVec3 n = artfVec3Normalize(artfOctUnfold(p->nx, p->ny));
        float x = (float)p->px / 65535.f * m->scale[0] + m->offset[0];
        bool found = false;
        for (int k = 0; k < 3; ++k)
            found = found || (fabsf(x - v[k].px) < 1e-3f && fabsf(u - v[k].u) < 1e-3f && fabsf(w - v[k].v) < 1e-3f &&
                              fabsf(n.x - v[k].nx) < 1e-3f && fabsf(n.y - v[k].ny) < 1e-3f && fabsf(n.z - v[k].nz) < 1e-3f);
        CHECK(found);
    }
    artfSceneFree(s);
    CHECK(sizeof(ArtfPackedVertex) == 16 && sizeof(ArtfInstanceData) == 64 && sizeof(ArtfMaterialData) == 144 && sizeof(ArtfFrameData) == 183 * 16);
}

void testMeshlets(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneAddSphere(s, 1, 64, 96);
    const ArtfMeshData* m = &s->meshes.items[0];
    uint64_t triangles = 0;
    for (uint32_t i = 0; i < m->meshletCount; ++i) {
        const ArtfMeshlet* l = &m->meshlets[i];
        CHECK(l->vertexCount <= ARTF_MESHLET_VERTICES && l->triangleCount <= ARTF_MESHLET_TRIANGLES);
        CHECK((l->vertexOffset & 7) == 0);
        triangles += l->triangleCount;
        for (uint32_t t = 0; t < l->triangleCount * 3; ++t) CHECK(m->meshletTriangles[(size_t)l->triangleOffset * 3 + t] < l->vertexCount);
    }
    CHECK(triangles == m->indexCount / 3);
    /* the order is for the cache: a 16-entry FIFO should miss well under one vertex a triangle */
    uint32_t fifo[16], head = 0, misses = 0;
    memset(fifo, 0xFF, sizeof fifo);
    for (uint32_t i = 0; i < m->indexCount; ++i) {
        bool hit = false;
        for (int k = 0; k < 16; ++k) hit = hit || fifo[k] == m->indices[i];
        if (!hit) { fifo[head++ & 15] = m->indices[i]; ++misses; }
    }
    const float acmr = (float)misses / (float)(m->indexCount / 3);
    CHECK(acmr < .85f);
    artfSceneFree(s);
}

void testInstances(void) {
    ArtfScene* s = artfSceneNew();
    ArtfMesh cube = artfSceneAddCube(s, 1);
    ArtfInstance a = artfSceneAddInstance(s, cube, 0, NULL), b = artfSceneAddInstance(s, cube, 0, NULL), c = artfSceneAddInstance(s, cube, 0, NULL);
    CHECK(artfSceneInstanceCount(s) == 3);
    artfSceneRemoveInstance(s, a);
    CHECK(artfSceneInstanceCount(s) == 2);
    ArtfTransform t = artfTransformAt(7, 8, 9, 1);
    artfInstanceSetTransform(s, c, &t); /* c moved in the dense list: its handle still finds it */
    CHECK(s->instances.items[s->handleSlots.items[c - 1]].m[3] == 7.f);
    CHECK(s->instances.items[s->handleSlots.items[b - 1]].m[3] == 0.f);
    ArtfInstance d = artfSceneAddInstance(s, cube, 0, NULL);
    CHECK((d & 0xFFFFFFu) == (a & 0xFFFFFFu) && d != a); /* places come back, and handles do not */
    CHECK(artfSceneAddInstance(s, 99, 0, NULL) == 0);
    artfSceneFree(s);
}

/* ---- the CPU's promises ---- */

void testSamePixels(void) {
    /* however many threads, with AVX2 or without */
    ArtfScene* s = richScene();
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    uint64_t reference = 0;
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {3, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {4, ARTF_SIMD_PORTABLE}, {2, ARTF_SIMD_128}, {3, ARTF_SIMD_256}};
    for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
        o.simd = runs[i].simd;
        ArtfRenderer* r = artfRendererNew(&o);
        for (int k = 0; k < 2; ++k) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, frame));
        uint64_t h = hashOf(frame, (size_t)W * H);
        if (!i) reference = h;
        CHECK(h == reference);
        artfRendererFree(r);
    }
    artfFree(frame);
    artfSceneFree(s);
}

static uint32_t backgroundOf(ArtfVec3 c) {
    return 0xFF000000u | (uint32_t)(c.x * 255.f + .5f) << 16 | (uint32_t)(c.y * 255.f + .5f) << 8 | (uint32_t)(c.z * 255.f + .5f);
}

/* Triangles that share an edge leave no gap: a mesh of many, seen whole, covers everything. */
void testNoGaps(void) {
    ArtfScene* s = artfSceneNew();
    const ArtfVec3 bg = artfVec3(1, 0, 1);
    artfSceneSetBackground(s, bg);
    enum { N = 48 };
    static ArtfVertex v[(N + 1) * (N + 1)];
    static uint32_t idx[N * N * 6];
    uint32_t n = 0;
    uint32_t seed = 1;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            seed = seed * 1664525u + 1013904223u;
            float jx = x && x < N ? ((float)(seed >> 16 & 255) / 255.f - .5f) * .5f : 0, jy = y && y < N ? ((float)(seed >> 24) / 255.f - .5f) * .5f : 0;
            ArtfVertex q = {((float)x + jx) / N * 40.f - 20.f, 0, ((float)y + jy) / N * 40.f - 20.f, 0, 1, 0, 0, 0};
            v[y * (N + 1) + x] = q;
        }
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            uint32_t a = y * (N + 1) + x, b = a + 1, c = a + N + 1, d = c + 1;
            idx[n++] = a; idx[n++] = c; idx[n++] = b;
            idx[n++] = b; idx[n++] = c; idx[n++] = d;
        }
    artfSceneAddInstance(s, artfSceneAddMesh(s, v, (N + 1) * (N + 1), idx, n), 0, NULL);
    setCamera(s, 0, 5, .01f, 0, 0, 0);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    const uint32_t* p = artfRendererPixels(r), want = backgroundOf(bg);
    int holes = 0;
    for (int i = 0; i < W * H; ++i) holes += p[i] == want;
    CHECK(holes == 0);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* A floor from under the eye to far away: its triangles cross the near plane and go past every edge. */
void testClipping(void) {
    ArtfScene* s = artfSceneNew();
    const ArtfVec3 bg = artfVec3(1, 0, 1);
    artfSceneSetBackground(s, bg);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 100000.f, 1.f), 0, NULL);
    setCamera(s, 0, 1, 0, 0, -.4f, -1);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    const uint32_t* p = artfRendererPixels(r), want = backgroundOf(bg);
    /* floor all over, with no crack along the edge its two triangles share */
    int holes = 0;
    for (int i = 0; i < W * H; ++i) holes += p[i] == want;
    CHECK(holes == 0);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* What is behind the eye and what is outside the view draws nothing. */
void testCulling(void) {
    ArtfScene* s = artfSceneNew();
    ArtfMesh cube = artfSceneAddCube(s, 1);
    ArtfTransform behind = artfTransformAt(0, 0, 50, 1), inView = artfTransformAt(0, 0, -10, 1), aside = artfTransformAt(500, 0, -10, 1);
    artfSceneAddInstance(s, cube, 0, &behind);
    artfSceneAddInstance(s, cube, 0, &inView);
    artfSceneAddInstance(s, cube, 0, &aside);
    setCamera(s, 0, 0, 0, 0, 0, -1);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    ArtfFrameStats st;
    artfRendererStats(r, &st);
    CHECK(st.instancesDrawn == 1);
    CHECK(st.trianglesBinned >= 2 && st.trianglesBinned <= 6); /* the faces in view, not the back ones */
    artfRendererFree(r);
    artfSceneFree(s);
}

void testOverlay(void) {
    ArtfScene* s = richScene();
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    uint32_t* plain = artfMalloc((size_t)W * H * 4);
    memcpy(plain, artfRendererPixels(r), (size_t)W * H * 4);

    /* an empty overlay changes nothing */
    uint32_t* over = artfCalloc((size_t)W * H, 4);
    ArtfOverlay overlay = {over, W, H, 0, false, NULL, 0};
    artfRendererSetOverlay(r, &overlay);
    CHECK(artfRendererDraw(r, s));
    CHECK(!memcmp(plain, artfRendererPixels(r), (size_t)W * H * 4));

    /* an opaque corner shows as it is, and its tiles are not drawn */
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) over[y * W + x] = 0xFF112233u;
    /* a translucent pixel: premultiplied white at half */
    over[150 * W + 200] = 0x80808080u;
    ArtfRect damage[2] = {{0, 0, 128, 128}, {200, 150, 1, 1}};
    ArtfOverlay partial = {over, W, H, 0, true, damage, 2};
    artfRendererSetOverlay(r, &partial);
    CHECK(artfRendererDraw(r, s));
    const uint32_t* p = artfRendererPixels(r);
    ArtfFrameStats st;
    artfRendererStats(r, &st);
    CHECK(st.tilesSkipped == 4);
    CHECK(p[0] == 0xFF112233u && p[127 * W + 127] == 0xFF112233u);
    CHECK(p[128] == plain[128]);
    for (int c = 0; c < 24; c += 8) {
        const int base = (int)(plain[150 * W + 200] >> c & 255), got = (int)(p[150 * W + 200] >> c & 255);
        const int want = 128 + base * 127 / 255;
        CHECK(abs(got - want) <= 1);
    }
    CHECK(p[150 * W + 200] >> 24 == 255);

    /* the overlay goes, and the scene is back */
    artfRendererSetOverlay(r, NULL);
    CHECK(artfRendererDraw(r, s));
    CHECK(!memcmp(plain, artfRendererPixels(r), (size_t)W * H * 4));

    /* a smaller one covers only its corner */
    uint32_t small[16 * 16];
    for (int i = 0; i < 256; ++i) small[i] = 0xFFFF0000u;
    ArtfOverlay little = {small, 16, 16, 0, false, NULL, 0};
    artfRendererSetOverlay(r, &little);
    CHECK(artfRendererDraw(r, s));
    p = artfRendererPixels(r);
    CHECK(p[15 * W + 15] == 0xFFFF0000u && p[16] == plain[16] && p[16 * W] == plain[16 * W]);

    artfFree(plain);
    artfFree(over);
    artfRendererFree(r);
    artfSceneFree(s);
}

void testResize(void) {
    ArtfScene* s = richScene();
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 2, false);
    ArtfRenderer* r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    artfRendererResize(r, 97, 53); /* not a whole number of tiles */
    CHECK(artfRendererDraw(r, s));
    uint32_t* a = artfMalloc(97 * 53 * 4);
    memcpy(a, artfRendererPixels(r), 97 * 53 * 4);
    /* a new renderer draws the same scene the same: nothing of it lived in the old one */
    artfRendererFree(r);
    o.width = 97;
    o.height = 53;
    r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    CHECK(!memcmp(a, artfRendererPixels(r), 97 * 53 * 4));
    artfFree(a);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* ---- levels of detail ---- */

void testSimplify(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says) */
    ArtfMesh cube = artfSceneAddCube(s, 1), sphere = artfSceneAddSphere(s, 1, 64, 96);
    CHECK(s->meshes.items[artfMeshIndexOf(s, cube)].lodCount == 0); /* too small to be worth it */
    const ArtfMeshData* m = &s->meshes.items[artfMeshIndexOf(s, sphere)];
    CHECK(m->lodCount >= 3); /* (its seam goes too, on both sides at once; its poles stay) */
    uint32_t triangles = m->indexCount / 3;
    float error = 0;
    for (uint32_t i = 0; i < m->lodCount; ++i) {
        const ArtfMeshData* l = &s->meshes.items[m->lods[i]];
        CHECK(l->isLod);
        CHECK(l->indexCount / 3 <= triangles / 3); /* about a quarter of the one before */
        CHECK(m->lodErrors[i] >= error);
        triangles = l->indexCount / 3;
        error = m->lodErrors[i];
        /* the surface stayed where it was: every vertex still on the sphere, within what was reported */
        for (uint32_t v = 0; v < l->vertexCount; ++v) {
            const ArtfPackedVertex* p = &l->vertices[v];
            const float x = (float)p->px / 65535.f * l->scale[0] + l->offset[0], y = (float)p->py / 65535.f * l->scale[1] + l->offset[1], z = (float)p->pz / 65535.f * l->scale[2] + l->offset[2];
            CHECK(fabsf(sqrtf(x * x + y * y + z * z) - 1.f) < .01f);
        }
        CHECK(fabsf(l->radius - 1.f) < .05f);
    }
    /* The cut stays closed: by where they are, each edge of each level has two sides, as at the start (where the
     * smallest triangles at the poles round to nothing, a few do not). */
    uint32_t openAtFirst = 0;
    for (uint32_t i = 0; i <= m->lodCount; ++i) {
        const ArtfMeshData* l = i ? &s->meshes.items[m->lods[i - 1]] : m;
        const uint32_t edges = l->indexCount;
        uint64_t* keys = artfMalloc((size_t)edges * 16);
        for (uint32_t c = 0; c < edges; ++c) {
            const ArtfPackedVertex *a = &l->vertices[l->indices[c]], *b = &l->vertices[l->indices[c / 3 * 3 + (c + 1) % 3]];
            const uint64_t ka = (uint64_t)a->px << 32 | (uint64_t)a->py << 16 | a->pz, kb = (uint64_t)b->px << 32 | (uint64_t)b->py << 16 | b->pz;
            keys[c * 2] = ka < kb ? ka : kb;
            keys[c * 2 + 1] = ka < kb ? kb : ka;
        }
        qsort(keys, edges, 16, compareEdgeKeys);
        uint32_t open = 0;
        for (uint32_t c = 0; c < edges;) {
            uint32_t j = c + 1;
            while (j < edges && keys[j * 2] == keys[c * 2] && keys[j * 2 + 1] == keys[c * 2 + 1]) ++j;
            open += (j - c) & 1;
            c = j;
        }
        if (!i) openAtFirst = open;
        CHECK(open <= openAtFirst);
        artfFree(keys);
    }
    /* What is reported is honest: the middle of a chord of the lightest level really is that far in.
     * For triangles of this size on a unit sphere the sag is about 1 / 8 of an edge squared. */
    {
        const ArtfMeshData* l = &s->meshes.items[m->lods[m->lodCount - 1]];
        double edges = 0;
        for (uint32_t i = 0; i + 1 < l->indexCount; i += 3) {
            const ArtfPackedVertex *a = &l->vertices[l->indices[i]], *b = &l->vertices[l->indices[i + 1]];
            const double dx = ((double)a->px - b->px) / 65535.0 * l->scale[0], dy = ((double)a->py - b->py) / 65535.0 * l->scale[1], dz = ((double)a->pz - b->pz) / 65535.0 * l->scale[2];
            edges += sqrt(dx * dx + dy * dy + dz * dz);
        }
        const double mean = edges / (l->indexCount / 3), sag = mean * mean / 8;
        CHECK(m->lodErrors[m->lodCount - 1] > sag * .5);
    }
    artfSceneFree(s);

    /* a sheet with a border keeps its outline: its corners are still where they were */
    s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    enum { N = 40 };
    static ArtfVertex v[(N + 1) * (N + 1)];
    static uint32_t idx[N * N * 6];
    uint32_t n = 0;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            ArtfVertex q = {(float)x, 0, (float)y, 0, 1, 0, 0, 0};
            v[y * (N + 1) + x] = q;
        }
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            uint32_t a = y * (N + 1) + x, b = a + 1, c = a + N + 1, d = c + 1;
            idx[n++] = a; idx[n++] = c; idx[n++] = b;
            idx[n++] = b; idx[n++] = c; idx[n++] = d;
        }
    ArtfMesh sheet = artfSceneAddMesh(s, v, (N + 1) * (N + 1), idx, n);
    const ArtfMeshData* base = &s->meshes.items[artfMeshIndexOf(s, sheet)];
    CHECK(base->lodCount >= 1);
    const ArtfMeshData* light = &s->meshes.items[base->lods[0]];
    CHECK(light->indexCount < base->indexCount / 2);
    CHECK(light->scale[0] == base->scale[0] && light->scale[2] == base->scale[2]);
    CHECK(base->lodErrors[0] < 1e-3f); /* a flat sheet loses nothing */
    artfSceneFree(s);
}

void testLod(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    ArtfMesh ball = artfSceneAddSphere(s, 1, 96, 128);
    ArtfTransform t = artfTransformAt(0, 0, -60, 1);
    artfSceneAddInstance(s, ball, 0, &t);
    setCamera(s, 0, 0, 0, 0, 0, -1);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* full = artfMalloc((size_t)W * H * 4);

    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    CHECK(!artfSceneFeature(s, ARTF_FEATURE_LOD));
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, full));
    ArtfFrameStats off, on;
    artfRendererStats(r, &off);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &on);
    CHECK(on.geometryMs >= 0);
    const uint32_t* p = artfRendererPixels(r);
    /* the outline has not moved by much: what differs is little */
    int different = 0;
    for (int i = 0; i < W * H; ++i) {
        int worst = 0;
        for (int c = 0; c < 24; c += 8) worst = artfMaxi(worst, abs((int)(p[i] >> c & 255) - (int)(full[i] >> c & 255)));
        different += worst > 24;
    }
    CHECK(different < W * H / 200);
    /* close by, and asked for little error, no lighter mesh fits: the pixels are the full mesh's */
    ArtfTransform close = artfTransformAt(0, 0, -4, 1);
    artfInstanceSetTransform(s, 1, &close);
    artfSceneSetLodError(s, .01f);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, full));
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    CHECK(artfRendererDraw(r, s));
    CHECK(!memcmp(full, artfRendererPixels(r), (size_t)W * H * 4));

    /* a mesh added with the feature off gets its levels when it comes on */
    ArtfScene* later = artfSceneNew();
    artfSceneSetFeature(later, ARTF_FEATURE_LOD, false);
    ArtfMesh mesh = artfSceneAddSphere(later, 1, 32, 48);
    CHECK(later->meshes.items[artfMeshIndexOf(later, mesh)].lodCount == 0);
    artfSceneSetFeature(later, ARTF_FEATURE_LOD, true);
    CHECK(later->meshes.items[artfMeshIndexOf(later, mesh)].lodCount > 0);
    artfSceneFree(later);

    artfFree(full);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* ---- occlusion ---- */

/* a wall, with a crowd behind it and a few things before it */
static ArtfScene* wallScene(ArtfInstance* wall) {
    ArtfScene* s = artfSceneNew();
    ArtfMesh cube = artfSceneAddCube(s, 1), ball = artfSceneAddSphere(s, .4f, 12, 16);
    ArtfTransform t = artfTransformTrs(artfVec3(0, 0, 0), artfQuatIdentity(), artfVec3(60, 40, 1));
    *wall = artfSceneAddInstance(s, cube, 0, &t);
    ArtfMaterialDesc d = {artfVec3(.9f, .3f, .2f), 0, .4f, .24f, artfVec3(0, 0, 0)};
    ArtfMaterial m = artfSceneAddMaterial(s, &d);
    for (int z = 0; z < 20; ++z)
        for (int x = 0; x < 20; ++x) {
            t = artfTransformAt((float)x - 10.f, (float)(z % 5) - 2.f, -3.f - (float)z, 1);
            artfSceneAddInstance(s, ball, m, &t);
        }
    t = artfTransformAt(-1, 0, 4, 1);
    artfSceneAddInstance(s, ball, m, &t);
    setCamera(s, 0, 0, 12, 0, 0, 0);
    return s;
}

void testOcclusion(ArtfBackend backend) {
    ArtfInstance wall;
    ArtfScene* s = wallScene(&wall);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) { artfSceneFree(s); return; }
    uint32_t* plain = artfMalloc((size_t)W * H * 4);
    uint32_t* got = artfMalloc((size_t)W * H * 4);

    /* each frame, over several, against the same scene drawn without */
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, false);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, plain));
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
    for (int i = 0; i < 4; ++i) {
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, got));
        CHECK(!memcmp(plain, got, (size_t)W * H * 4));
    }
    ArtfFrameStats st;
    artfRendererStats(r, &st);
    if (backend == ARTF_BACKEND_SOFTWARE) {
        CHECK(st.instancesHidden > 300);  /* the crowd */
        CHECK(st.instancesDrawn < 50);
    }

    /* the wall goes: what it hid must be there in this very frame, not the next */
    ArtfTransform away = artfTransformAt(0, 300, 0, 1);
    artfInstanceSetTransform(s, wall, &away);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, false);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, plain));
    /* (a second scene state would be needed to come back: so the feature goes on with the wall back) */
    ArtfTransform back = artfTransformTrs(artfVec3(0, 0, 0), artfQuatIdentity(), artfVec3(60, 40, 1));
    artfInstanceSetTransform(s, wall, &back);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
    for (int i = 0; i < 3; ++i) CHECK(artfRendererDraw(r, s)); /* the crowd is hidden again */
    artfInstanceSetTransform(s, wall, &away);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, got));
    CHECK(!memcmp(plain, got, (size_t)W * H * 4));

    /* instances going while it runs: the flags of the old order cannot lose one */
    artfInstanceSetTransform(s, wall, &back);
    for (int i = 0; i < 2; ++i) CHECK(artfRendererDraw(r, s));
    artfSceneRemoveInstance(s, wall);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, got));
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, false);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, plain));
    CHECK(!memcmp(plain, got, (size_t)W * H * 4));

    artfFree(plain);
    artfFree(got);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* What an opaque overlay covers hides the scene as a wall does. */
void testOcclusionUnderOverlay(void) {
    ArtfInstance wall;
    ArtfScene* s = wallScene(&wall);
    artfSceneRemoveInstance(s, wall);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* over = artfMalloc((size_t)W * H * 4);
    for (int i = 0; i < W * H; ++i) over[i] = 0xFF204060u;
    ArtfOverlay overlay = {over, W, H, 0, false, NULL, 0};
    artfRendererSetOverlay(r, &overlay);
    for (int i = 0; i < 3; ++i) CHECK(artfRendererDraw(r, s));
    ArtfFrameStats st;
    artfRendererStats(r, &st);
    CHECK(st.instancesDrawn <= 1);
    CHECK(artfRendererPixels(r)[0] == 0xFF204060u);
    artfFree(over);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* ---- pictures that must not change ----
 * The CPU's bytes are the same on every processor, so a picture can be held to a number: a change to any
 * of these is a change to what programs draw, and should be one that was meant. */

static uint64_t pictureOf(ArtfScene* s, bool antialias) {
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, false);
    artfSceneSetFeature(s, ARTF_FEATURE_CONES, true);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    o.antialias = antialias;
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    drawInto(r, s, frame);
    const uint64_t h = hashOf(frame, (size_t)W * H);
    artfFree(frame);
    artfRendererFree(r);
    artfSceneFree(s);
    return h;
}

void testPictures(void) {
    const uint64_t rich = pictureOf(richScene(), false), layered = pictureOf(layeredScene(), false), smooth = pictureOf(layeredScene(), true);
    ArtfScene* s = layeredScene();
    artfSceneSetBackgroundAlpha(s, 0);
    const uint64_t clear = pictureOf(s, true);
    /* (since highlights get back the light their lobe loses, and widen where the normal turns over a pixel; reviewed
     * against the commit before by FLIP: docs/plano-de-correcao.md, phase 2.4) */
    const uint64_t want[4] = {0x4F420EF73EC7BC08ull, 0xEC797E77232CFFE0ull, 0x73C79609EF4F9431ull, 0xC16A0A9EAACD5FDDull};
    const uint64_t got[4] = {rich, layered, smooth, clear};
    for (int k = 0; k < 4; ++k) {
        if (got[k] != want[k]) printf("picture %d: %016llx, where %016llx was\n", k, (unsigned long long)got[k], (unsigned long long)want[k]);
        CHECK(got[k] == want[k]);
    }
}

void testHandles(void) {
    artfSetLog(countLog, &logged);
    ArtfScene* s = plainScene();
    ArtfMesh m = addSquare(s, 0, 0, 0, 0);
    ArtfInstance a = addAt(s, m, 0, 0, 0, 0, 1), b = addAt(s, m, 0, 1, 0, 0, 1);
    artfSceneRemoveInstance(s, a);
    const ArtfTransform t = artfTransformAt(9, 9, 9, 1);
    /* a handle that is gone does nothing, even when its place is taken */
    ArtfInstance c = addAt(s, m, 0, 2, 0, 0, 1);
    CHECK(c != a);
    const uint32_t before = artfSceneInstanceCount(s);
    logged = 0;
    artfInstanceSetTransform(s, a, &t);
    artfInstanceSetOpacity(s, a, .5f);
    artfInstanceSetMaterial(s, a, 0);
    artfSceneRemoveInstance(s, a);
    artfSceneRemoveInstance(s, 0x00FFFFFFu);
    artfInstanceSetTransform(s, 0, &t);
    CHECK(logged == 6);
    CHECK(artfSceneInstanceCount(s) == before);
    CHECK(s->translucentCount == 0);
    for (size_t i = 0; i < s->instances.len; ++i) CHECK(s->instances.items[i].m[3] != 9.f);
    artfInstanceSetTransform(s, b, &t);
    CHECK(logged == 6);
    artfSceneFree(s);
    artfSetLog(NULL, NULL);

    ArtfMaterialDesc d = artfMaterialDefaults();
    CHECK(d.baseColor.x == 1.f && d.specular == 1.f && d.roughness == .5f && d.alphaMode == ARTF_ALPHA_OPAQUE);
    ArtfRendererOptions o = artfRendererDefaults(W, H);
    CHECK(o.width == W && o.height == H && o.backend == ARTF_BACKEND_AUTO && !o.antialias);
}

/* A renderer that goes while its work is in flight, many times, and a cache that survives. */
void testLifetimes(void) {
    ArtfScene* s = layeredScene();
    for (int i = 0; i < 6; ++i) {
        ArtfRendererOptions o = options(i & 1 ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.antialias = i & 2;
        if (i == 3) { ArtfTraceDesc d = {ARTF_TRACE_PATHS, 1, 0, 0}; artfSceneSetTrace(s, &d); }
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        for (int k = 0; k < 3; ++k) CHECK(artfRendererDraw(r, s)); /* (nothing waited for) */
        artfRendererFree(r);
    }
    artfSceneFree(s);

    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.pipelineCache = "artful-test-pipelines.bin";
    remove(o.pipelineCache);
    for (int i = 0; i < 2; ++i) {
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) return;
        s = richScene();
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererDraw(r, s));
        ArtfFrameStats st;
        artfRendererStats(r, &st);
        CHECK(st.gpuMs > 0 && st.gpuMs < 1000);
        artfRendererFree(r);
        artfSceneFree(s);
        FILE* f = fopen(o.pipelineCache, "rb");
        CHECK(f != NULL);
        if (f) fclose(f);
    }
    remove(o.pipelineCache);
}

/* Occlusion tried both ways: the frames do not change, and it goes back and forth only in its rounds. */
void testOcclusionAuto(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    drawInto(r, s, a);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION_AUTO, true);
    int changes = 0;
    bool was = artfSceneFeature(s, ARTF_FEATURE_OCCLUSION);
    for (int i = 0; i < 260; ++i) {
        CHECK(artfRendererDraw(r, s));
        changes += artfSceneFeature(s, ARTF_FEATURE_OCCLUSION) != was;
        was = artfSceneFeature(s, ARTF_FEATURE_OCCLUSION);
    }
    CHECK(changes >= 1 && changes <= 4);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* Between two levels both are drawn, each in its pixels: nothing is left open, and the picture is between the two. */
void testLodFade(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says) */
    const ArtfMesh mesh = artfSceneAddSphere(s, 1, 64, 96);
    const ArtfMeshData* m = &s->meshes.items[artfMeshIndexOf(s, mesh)];
    CHECK(m->lodCount >= 1);
    addAt(s, mesh, whiteMaterial(s), 0, 0, 0, 1);
    /* where the first level starts: a little past it, it is in its fade */
    ArtfFrameData frame;
    artfSceneFrame(s, W, H, &frame);
    const float at = m->lodErrors[0] * frame.lodScale * 1.07f + m->radius;
    setCamera(s, 0, 0, at, 0, 0, 0);
    drawInto(r, s, a);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD_FADE, true);
    artfSceneFrame(s, W, H, &frame);
    uint32_t other;
    const uint32_t fade = artfMeshFade(s, mesh - 1, frame.lodScale, 1, at - m->radius, frame.look[3], &other);
    CHECK(fade > 60 && fade < 200 && other == mesh - 1);
    drawInto(r, s, b);
    /* the middle of the disc is covered, as it was */
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    int open = 0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) open += b[mid + (size_t)(y * W) + (size_t)x] == 0xFF000000u;
    CHECK(open == 0);
    CHECK(differentShare(a, b, 40) < .01);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD_FADE, false);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* Meshlets dropped by the GPU change nothing that shows. */
void testGpuMeshlets(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    const ArtfMesh big = artfSceneAddSphere(s, 2, 256, 384);
    for (int i = 0; i < 12; ++i) addAt(s, big, 0, (float)(i % 4) * 4.f - 6.f, 1.5f, (float)(i / 4) * -5.f + 3.f, 1);
    setCamera(s, 1, 2, 7, 3, 1, 0);
    drawInto(r, s, a);
    artfSceneSetFeature(s, ARTF_FEATURE_CONES, false);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    /* and with occlusion, whichever pass each thing comes in, and what a wall hides now and shows next */
    artfSceneSetFeature(s, ARTF_FEATURE_CONES, true);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
    ArtfTransform at = artfTransformAt(0, 1, 5, 1);
    const ArtfInstance wall = artfSceneAddInstance(s, artfSceneAddCube(s, 3), 0, &at);
    for (int i = 0; i < 6; ++i) {
        setCamera(s, 1 + (float)i, 2, 8, 3, 1, 0);
        at = artfTransformAt((float)i * 2.f - 4.f, 1, 5, 1);
        artfInstanceSetTransform(s, wall, &at);
        drawInto(r, s, a);
        artfSceneSetFeature(s, ARTF_FEATURE_CONES, false);
        drawInto(r, s, b);
        artfSceneSetFeature(s, ARTF_FEATURE_CONES, true);
        CHECK(!memcmp(a, b, (size_t)W * H * 4));
    }
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* What is too small to see is not drawn: a far ball goes, a near one stays, on both backends alike. */
void testCullSize(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b[2] = {artfCalloc(W * H, 4), artfCalloc(W * H, 4)}; /* (zeros: where Vulkan does not run, the GPU's stays so) */
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* s = plainScene();
        const ArtfMesh ball = artfSceneAddSphere(s, .3f, 16, 24);
        addAt(s, ball, whiteMaterial(s), -.6f, 0, 2, 1);   /* near: tens of pixels */
        addAt(s, ball, whiteMaterial(s), 3, 0, -40, 1);    /* far: a few */
        drawInto(r, s, a);
        artfSceneSetCullSize(s, 12);
        drawInto(r, s, b[backend]);
        int lit[2] = {0, 0}, litLeft = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const bool was = CHANNEL(a[y * W + x], 8) > 40, is = CHANNEL(b[backend][y * W + x], 8) > 40;
                lit[0] += was;
                lit[1] += is;
                litLeft += is && x < W / 2;
            }
        printf("%s, too small to draw: %d pixels lit, %d before\n", backend ? "GPU" : "CPU", lit[1], lit[0]);
        CHECK(lit[1] < lit[0] && lit[1] == litLeft && litLeft > 200);
        artfSceneSetCullSize(s, 0);
        drawInto(r, s, b[backend]);
        CHECK(differentShare(a, b[backend], 0) == 0);
        artfSceneSetCullSize(s, 12);
        drawInto(r, s, b[backend]);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (b[1][0] || b[1][W * H / 2]) CHECK(differentShare(b[0], b[1], 24) < .002);
    artfFree(a);
    artfFree(b[0]);
    artfFree(b[1]);
}

/* A scene belongs to one thread at a time: a change from another thread while it is drawn is said; one from the
 * drawing thread itself, or from another between frames, is not. */
typedef struct ChangeFromAfar {
    ArtfScene* scene;
    ArtfInstance instance;
    bool fromAfar;
} ChangeFromAfar;

static void moveIt(void* user) {
    ChangeFromAfar* c = user;
    const ArtfTransform t = artfTransformAt(.1f, 0, 0, 1);
    artfInstanceSetTransform(c->scene, c->instance, &t);
}

static void frameThatChanges(void* user, const ArtfCamera* camera, int width, int height, void* params) {
    (void)camera; (void)width; (void)height; (void)params;
    ChangeFromAfar* c = user;
    if (c->fromAfar) artfTaskJoin(artfTaskStart(moveIt, c));
    else moveIt(c);
}

static void rowsDark(void* user, const void* params, const ArtfBackgroundRows* rows) { (void)user; (void)params; (void)rows; }

void testThreadRule(void) {
    ArtfScene* s = plainScene();
    ChangeFromAfar c = {s, addAt(s, artfSceneAddCube(s, 1), whiteMaterial(s), 0, 0, 0, 1), false};
    ArtfBackgroundPass pass;
    memset(&pass, 0, sizeof pass);
    pass.user = &c;
    pass.frame = frameThatChanges;
    pass.rows = rowsDark;
    artfSceneSetBackgroundPass(s, &pass);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    logged = 0;
    artfSetLog(countLog, &logged);
    CHECK(artfRendererDraw(r, s));
    CHECK(logged == 0); /* (the drawing thread's own) */
    artfTaskJoin(artfTaskStart(moveIt, &c));
    CHECK(logged == 0); /* (another thread, between frames: it belongs to that one then) */
    c.fromAfar = true;
    CHECK(artfRendererDraw(r, s));
    CHECK(logged == 1);
    artfSetLog(NULL, NULL);
    artfRendererFree(r);
    artfSceneFree(s);
}
