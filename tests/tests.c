/* artful-tests: the scene's data, the rasterizer's promises, and the two backends against each other.
 * The GPU's part runs where Vulkan does, and says so where it does not. */
#include "artful/artful.h"
#include "scene/sceneInternal.h"
#include "render/rendererInternal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

#define W 320
#define H 200

static int compareEdgeKeys(const void* a, const void* b) {
    const uint64_t *x = a, *y = b;
    if (x[0] != y[0]) return x[0] < y[0] ? -1 : 1;
    return x[1] < y[1] ? -1 : x[1] > y[1];
}

static uint64_t hashOf(const uint32_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static ArtfRendererOptions options(ArtfBackend backend, int threads, bool portable) {
    ArtfRendererOptions o;
    memset(&o, 0, sizeof o);
    o.backend = backend;
    o.width = W;
    o.height = H;
    o.threads = threads;
    o.simd = portable ? ARTF_SIMD_PORTABLE : ARTF_SIMD_AUTO;
    return o;
}

static void setCamera(ArtfScene* s, float x, float y, float z, float tx, float ty, float tz) {
    ArtfCamera c = {artfVec3(x, y, z), artfVec3(tx, ty, tz), artfVec3(0, 1, 0), 1.f, .1f};
    artfSceneSetCamera(s, &c);
}

/* the same scene as a test of everything at once: texture, lights, overlapping objects */
static ArtfScene* richScene(void) {
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

/* ---- the mesh's data ---- */

static void testPacking(void) {
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
    CHECK(sizeof(ArtfPackedVertex) == 16 && sizeof(ArtfInstanceData) == 64 && sizeof(ArtfMaterialData) == 144 && sizeof(ArtfFrameData) == 95 * 16);
}

static void testMeshlets(void) {
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

static void testInstances(void) {
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

static void testSamePixels(void) {
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
static void testNoGaps(void) {
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
static void testClipping(void) {
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
static void testCulling(void) {
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

static void testOverlay(void) {
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

static void testResize(void) {
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

static void testSimplify(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says) */
    ArtfMesh cube = artfSceneAddCube(s, 1), sphere = artfSceneAddSphere(s, 1, 64, 96);
    CHECK(s->meshes.items[cube - 1].lodCount == 0); /* too small to be worth it */
    const ArtfMeshData* m = &s->meshes.items[sphere - 1];
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
    const ArtfMeshData* base = &s->meshes.items[sheet - 1];
    CHECK(base->lodCount >= 1);
    const ArtfMeshData* light = &s->meshes.items[base->lods[0]];
    CHECK(light->indexCount < base->indexCount / 2);
    CHECK(light->scale[0] == base->scale[0] && light->scale[2] == base->scale[2]);
    CHECK(base->lodErrors[0] < 1e-3f); /* a flat sheet loses nothing */
    artfSceneFree(s);
}

static void testLod(void) {
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
    CHECK(later->meshes.items[mesh - 1].lodCount == 0);
    artfSceneSetFeature(later, ARTF_FEATURE_LOD, true);
    CHECK(later->meshes.items[mesh - 1].lodCount > 0);
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

static void testOcclusion(ArtfBackend backend) {
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
static void testOcclusionUnderOverlay(void) {
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

/* ---- the GPU against the CPU ---- */

/* How far apart two pictures are: a GPU rounds and samples in its own way, and its edges fall on
 * slightly other pixels, so what is compared is how many pixels differ by much. */
static double differentShare(const uint32_t* a, const uint32_t* b, int threshold) {
    int different = 0;
    for (int i = 0; i < W * H; ++i) {
        int worst = 0;
        for (int c = 0; c < 24; c += 8) worst = artfMaxi(worst, abs((int)(a[i] >> c & 255) - (int)(b[i] >> c & 255)));
        different += worst > threshold;
    }
    return (double)different / (W * H);
}

static void testVulkan(void) {
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

#ifdef ARTFUL_COUNT_ALLOCATIONS
long artfAllocationsAlive(void);
#endif

/* ---- a black hole ---- */

static ArtfScene* holeScene(float distance, float tilt, float doppler, float stars) {
    ArtfScene* s = artfSceneNew();
    ArtfBlackHoleDesc d = artfBlackHoleDefaults();
    d.spin = 0;
    d.axis = artfVec3(0, 1, 0);
    d.doppler = doppler;
    d.stars = stars;
    d.bloom = 0;
    artfSceneSetBlackHole(s, &d);
    ArtfCamera c = {artfVec3(0, sinf(tilt) * distance, cosf(tilt) * distance), artfVec3(0, 0, 0), artfVec3(0, 0, -1), .9f, .1f};
    if (fabsf(tilt) < 1.f) c.up = artfVec3(0, 1, 0);
    artfSceneSetCamera(s, &c);
    return s;
}

/* How far the black in the middle of a frame reaches, in pixels: the least of what it does in sixteen
 * directions, as the gas that lights its edge has gaps. */
static float shadowRadius(ArtfRenderer* r, ArtfScene* s, uint32_t* a) {
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    float least = 1e9f;
    for (int i = 0; i < 16; ++i) {
        const float dx = cosf((float)i * .3926991f), dy = sinf((float)i * .3926991f);
        for (float at = 1.f; at < (float)H * .5f; at += .25f) {
            const int x = (int)((float)W * .5f + dx * at), y = (int)((float)H * .5f + dy * at);
            if (((a[(size_t)y * W + x] >> 16) & 255) >= 24) { least = artfMinf(least, at); break; }
        }
    }
    return least;
}

/* The mean brightness of a column range. */
static double brightness(const uint32_t* a, int x0, int x1) {
    double sum = 0;
    for (int y = 0; y < H; ++y)
        for (int x = x0; x < x1; ++x) {
            const uint32_t p = a[y * W + x];
            sum += (double)((p >> 16 & 255) + (p >> 8 & 255) + (p & 255));
        }
    return sum / ((double)H * (x1 - x0));
}

/* The fluid: chaotic, and yet the same every time; bounded however long it runs; and what it does is
 * what gas in orbit does. */
/* ---- colors, layers, alpha and samples ---- */

#define CHANNEL(p, shift) ((int)((p) >> (shift) & 255))

/* Light that gives a surface's own color back: what is drawn is what was asked for. */
static ArtfScene* plainScene(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(1, 1, 1), artfVec3(1, 1, 1));
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    setCamera(s, 0, 0, 5, 0, 0, 0);
    return s;
}

static ArtfMaterial whiteMaterial(ArtfScene* s) {
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    return artfSceneAddMaterial(s, &d);
}

/* A square of side 2 in the xy plane, facing the eye; its corners from the bottom left, going round. */
static ArtfMesh addSquare(ArtfScene* s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    const ArtfVertex v[4] = {{-1, -1, 0, 0, 0, 1, 0, 0, a}, {1, -1, 0, 0, 0, 1, 1, 0, b}, {1, 1, 0, 0, 0, 1, 1, 1, c}, {-1, 1, 0, 0, 0, 1, 0, 1, d}};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    return artfSceneAddMesh(s, v, 4, idx, 6);
}

static ArtfInstance addAt(ArtfScene* s, ArtfMesh mesh, ArtfMaterial material, float x, float y, float z, float scale) {
    const ArtfTransform t = artfTransformAt(x, y, z, scale);
    return artfSceneAddInstance(s, mesh, material, &t);
}

static void drawInto(ArtfRenderer* r, ArtfScene* s, uint32_t* frame) {
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
}

static bool near_(int a, int b, int margin) { return abs(a - b) <= margin; }

static void testColorData(void) {
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
static void testLayers(ArtfBackend backend) {
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

    /* and it goes between vertices as light does: half of red and half of blue, in the frame's colors */
    s = plainScene();
    white = whiteMaterial(s);
    addAt(s, addSquare(s, 0xFFFF0000u, 0xFF0000FFu, 0xFF0000FFu, 0xFFFF0000u), white, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 16), 180, 6) && near_(CHANNEL(a[mid], 0), 180, 6) && CHANNEL(a[mid], 8) == 0);
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
static void testMaterials(ArtfBackend backend, bool antialias) {
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

/* A scene of everything at once, for what must not change with how it is drawn. */
static ArtfScene* layeredScene(void) {
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

static void testSamePixelsOfLayers(void) {
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

static void testAntialias(ArtfBackend backend) {
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
static void testLayersBothBackends(void) {
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

static void testPictures(void) {
    const uint64_t rich = pictureOf(richScene(), false), layered = pictureOf(layeredScene(), false), smooth = pictureOf(layeredScene(), true);
    ArtfScene* s = layeredScene();
    artfSceneSetBackgroundAlpha(s, 0);
    const uint64_t clear = pictureOf(s, true);
    CHECK(rich == 0x818E1B446A10E12Bull);
    CHECK(layered == 0x3A886E17A40AE14ull);
    CHECK(smooth == 0x8DB1290E706CF72Full);
    CHECK(clear == 0x3D52D43351D2EB2Full);
}

/* ---- what a program can get wrong, and what it can ask for ---- */

static double meanOf(const uint32_t* frame, int x, int y, int shift);
static void put32le(uint8_t* p, uint32_t v);
static ArtfMesh addStrip(ArtfScene* s);
static void bendStrip(ArtfScene* s, ArtfInstance i, float angle);
static void drawSamples(ArtfRenderer* r, ArtfScene* s, int samples, uint32_t* frame);

static int logged;
static void countLog(void* user, const char* message) { (void)message; ++*(int*)user; }

static void testHandles(void) {
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

/* Fog, film, spots: on both backends, the same way. */
static void testLook(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    const int margin = 4;

    /* fog: at distance 5 with density .2, air 1 keeps 1 / 2.5 of what the surface shows */
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    addAt(s, addSquare(s, 0, 0, 0, 0), white, 0, 0, 0, 1);
    artfSceneSetFog(s, artfVec3(0, 0, 0), .2f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), (int)(sqrtf(.4f) * 255.f), margin));
    ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 16, 0};
    artfSceneSetTrace(s, &rays);
    for (int i = 0; i < 16; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    CHECK(near_(CHANNEL(a[mid], 8), (int)(sqrtf(.4f) * 255.f), margin));
    artfSceneSetTrace(s, NULL);
    artfSceneSetFog(s, artfVec3(0, 0, 0), 0);

    /* film: light of 1, exposed at 1, comes to the curve's .8 */
    artfSceneSetExposure(s, 1.f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), (int)(sqrtf(2.54f / 3.16f) * 255.f), margin));
    artfSceneSetTrace(s, &rays);
    for (int i = 0; i < 16; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    CHECK(near_(CHANNEL(a[mid], 8), (int)(sqrtf(2.54f / 3.16f) * 255.f), margin));
    artfSceneFree(s);

    /* a spot: full in its cone, nothing past it, and the same point light without one */
    s = plainScene();
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    white = whiteMaterial(s);
    addAt(s, addSquare(s, 0, 0, 0, 0), white, 0, 0, 0, 2);
    ArtfLight l = artfSceneAddLight(s, artfVec3(0, 0, 3), artfVec3(1, 1, 1), 100.f);
    artfLightSetSpot(s, l, artfVec3(0, 0, -1), .1f, .2f);
    const size_t aside = (size_t)(H / 2) * W + W / 2 + 40;
    for (int traced = 0; traced < 2; ++traced) {
        if (traced) artfSceneSetTrace(s, &rays);
        for (int i = 0; i < 16; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        CHECK(CHANNEL(a[mid], 8) > 230);
        CHECK(CHANNEL(a[aside], 8) == 0);
    }
    artfSceneSetTrace(s, NULL);
    artfLightSet(s, l, artfVec3(0, 0, 3), artfVec3(1, 1, 1), 100.f);
    drawInto(r, s, a);
    CHECK(CHANNEL(a[aside], 8) > 200);
    artfSceneFree(s);

    /* a soft sun: an edge that was hard goes through shades */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(.9f, .9f, .9f));
    setCamera(s, 0, 6, .01f, 0, 0, 0);
    const ArtfTransform ground = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 1), white, &ground);
    const ArtfTransform bar = artfTransformTrs(artfVec3(0, 2, 0), artfQuatAxisAngle(artfVec3(1, 0, 0), -1.5708f), artfVec3(1, 8, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), white, &bar);
    artfSceneSetSun(s, artfVec3(-.5f, -1, 0), artfVec3(.9f, .9f, .9f));
    int shades[2];
    for (int soft = 0; soft < 2; ++soft) {
        artfSceneSetSunSize(s, soft ? .15f : 0.f);
        ArtfTraceDesc many = {ARTF_TRACE_RAYS, 1, 48, 0};
        artfSceneSetTrace(s, &many);
        for (int i = 0; i < 48; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        shades[soft] = 0;
        for (int x = 0; x < W; ++x) {
            const int v = CHANNEL(a[(size_t)(H - 8) * W + x], 8);
            shades[soft] += v > 110 && v < 215;
        }
    }
    CHECK(shades[1] > shades[0] + 6);
    artfSceneFree(s);

    /* glass bends: a ball between the eye and a wall, red to the left and blue to the right, turns it around */
    s = plainScene();
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    white = whiteMaterial(s);
    addAt(s, addSquare(s, 0xFFFF0000u, 0xFF0000FFu, 0xFF0000FFu, 0xFFFF0000u), white, 0, 0, -6, 12);
    ArtfMaterialDesc glass = artfMaterialDefaults();
    glass.alphaMode = ARTF_ALPHA_BLEND;
    glass.transparency = 1.f;
    glass.specular = 0;
    glass.roughness = 1.f;
    addAt(s, artfSceneAddSphere(s, 1, 48, 64), artfSceneAddMaterial(s, &glass), 0, 0, 0, 1);
    ArtfTraceDesc through = {ARTF_TRACE_RAYS, 1, 8, 0};
    artfSceneSetTrace(s, &through);
    for (int i = 0; i < 8; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    const int x = W / 2 - 15;
    CHECK(meanOf(a, x, H / 2, 16) > meanOf(a, x, H / 2, 0)); /* straight: more red on the left */
    glass.ior = 1.5f;
    artfMaterialSet(s, s->instances.items[1].material, &glass);
    for (int i = 0; i < 8; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    CHECK(meanOf(a, x, H / 2, 0) > meanOf(a, x, H / 2, 16));
    artfSceneFree(s);

    artfFree(a);
    artfRendererFree(r);
}

/* A renderer that goes while its work is in flight, many times, and a cache that survives. */
static void testLifetimes(void) {
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

/* ---- what things are made of ---- */

/* A square with v going down the picture, as a file's textures have it. */
static ArtfMesh addPictureSquare(ArtfScene* s) {
    const ArtfVertex v[4] = {{-1, -1, 0, 0, 0, 1, 0, 1, 0}, {1, -1, 0, 0, 0, 1, 1, 1, 0}, {1, 1, 0, 0, 0, 1, 1, 0, 0}, {-1, 1, 0, 0, 0, 1, 0, 0, 0}};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    return artfSceneAddMesh(s, v, 4, idx, 6);
}

static ArtfTexture oneColor(ArtfScene* s, uint32_t bgra) {
    static uint32_t px[4 * 4];
    for (int i = 0; i < 16; ++i) px[i] = bgra;
    return artfSceneAddTexture(s, px, 4, 4, 0);
}

/* Draws, and gives the middle's pixel: by triangles, and then by rays. */
static uint32_t middleOf(ArtfRenderer* r, ArtfScene* s, uint32_t* frame, bool traced) {
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 8, 1};
    artfSceneSetTrace(s, traced ? &rays : NULL);
    for (int i = 0; i < 9; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
    return frame[(size_t)(H / 2) * W + W / 2];
}

static void testPhysical(ArtfBackend backend) {
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
        CHECK(near_(CHANNEL(p, 16), 255, 3) && near_(CHANNEL(p, 8), (int)(sqrtf(.5f) * 255.f), 3) && near_(CHANNEL(p, 0), (int)(sqrtf(.25f) * 255.f), 3));
        /* and what is not metal, and black, reflects 4% of it */
        d.baseColor = artfVec3(0, 0, 0);
        d.metallic = 0;
        artfMaterialSet(s, metal, &d);
        p = middleOf(r, s, a, traced);
        CHECK(near_(CHANNEL(p, 8), (int)(sqrtf(.04f) * 255.f), 3));
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
        CHECK(near_(CHANNEL(p, 8), (int)(sqrtf(.25f) * 255.f), 4));
        artfSceneFree(s);
    }
    artfFree(a);
    artfRendererFree(r);
}

/* ---- shadows ---- */

/* A floor, a ball over it and a wall of bars, as a test of what maps can get wrong. */
static ArtfScene* shadowScene(void) {
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

static void testShadows(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);

    /* the maps come close to what rays say; with none, nothing is dark */
    ArtfScene* s = shadowScene();
    drawInto(r, s, a);
    const ArtfShadowDesc desc = {1024, 40};
    artfSceneSetShadows(s, &desc);
    drawInto(r, s, b);
    int darker = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) darker += CHANNEL(b[i], 8) + 8 < CHANNEL(a[i], 8);
    CHECK(darker > 600);
    ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 32, 0};
    artfSceneSetTrace(s, &rays);
    drawSamples(r, s, 32, a);
    artfSceneSetTrace(s, NULL);
    double far = differentShare(a, b, 40);
    printf("the sun's shadows against rays, pixels that differ by more than 40 of 255: %.2f%%\n", far * 100);
    CHECK(far < .015);
    artfSceneFree(s);

    /* a spot, and a point light: what stands next to them throws the same shadow as a ray says */
    for (int kind = 0; kind < 2; ++kind) {
        s = shadowScene();
        artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
        ArtfLight l = artfSceneAddLight(s, artfVec3(2.5f, 4, 1), artfVec3(2, 2, 2), 20.f);
        if (kind) artfLightSetSpot(s, l, artfVec3(-.45f, -1, -.2f), .5f, .8f);
        artfLightSetShadow(s, l, true);
        artfSceneSetShadows(s, &desc);
        drawInto(r, s, b);
        artfSceneSetTrace(s, &rays);
        drawSamples(r, s, 32, a);
        artfSceneSetTrace(s, NULL);
        far = differentShare(a, b, 40);
        printf("%s against rays, pixels that differ by more than 40 of 255: %.2f%%\n", kind ? "a spot" : "a point light", far * 100);
        CHECK(far < .02);
        artfSceneFree(s);
    }

    /* holes let light through: half a square throws half a shadow, as rays say */
    s = artfSceneNew();
    setCamera(s, 0, 8, .01f, 0, 0, 0);
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    artfSceneSetSun(s, artfVec3(-.8f, -1, 0), artfVec3(1, 1, 1));
    ArtfMaterialDesc matte = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 1), artfSceneAddMaterial(s, &matte), &t);
    static uint32_t half[64 * 64];
    for (int i = 0; i < 64 * 64; ++i) half[i] = i % 64 < 32 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    ArtfMaterialDesc cut = {artfVec3(1, 1, 1), artfSceneAddTexture(s, half, 64, 64, 0), 0, 1, artfVec3(0, 0, 0), ARTF_ALPHA_MASK, 0, 0, true};
    const ArtfTransform flat = artfTransformTrs(artfVec3(0, 1, 0), artfQuatAxisAngle(artfVec3(1, 0, 0), -1.5708f), artfVec3(1.5f, 1.5f, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &cut), &flat);
    artfSceneSetShadows(s, &desc);
    drawInto(r, s, b);
    artfSceneSetTrace(s, &rays);
    drawSamples(r, s, 32, a);
    artfSceneSetTrace(s, NULL);
    far = differentShare(a, b, 40);
    printf("a shadow with holes against rays, pixels that differ by more than 40 of 255: %.2f%%\n", far * 100);
    CHECK(far < .01);
    artfSceneFree(s);

    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

static void testShadowsBothBackends(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    const ArtfShadowDesc desc = {1024, 40};
    for (int kind = 0; kind < 3; ++kind) {
        ArtfScene* s = shadowScene();
        if (kind) {
            ArtfLight l = artfSceneAddLight(s, artfVec3(2.5f, 4, 1), artfVec3(1, 1, 1), 20.f);
            if (kind == 2) artfLightSetSpot(s, l, artfVec3(-.45f, -1, -.2f), .5f, .8f);
            artfLightSetShadow(s, l, true);
        }
        artfSceneSetShadows(s, &desc);
        drawInto(gpu, s, a);
        drawInto(cpu, s, b);
        const double far = differentShare(a, b, 24);
        printf("shadows, %d: pixels that differ by more than 24 of 255: %.2f%%\n", kind, far * 100);
        CHECK(far < .02);
        artfSceneFree(s);
    }
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}

static void testShadowBytes(void) {
    ArtfScene* s = shadowScene();
    const ArtfShadowDesc desc = {512, 40};
    artfSceneSetShadows(s, &desc);
    ArtfLight l = artfSceneAddLight(s, artfVec3(2.5f, 4, 1), artfVec3(1, 1, 1), 20.f);
    artfLightSetShadow(s, l, true);
    /* more than fits gets none, and says so */
    artfSetLog(countLog, &logged);
    logged = 0;
    for (int i = 0; i < 3; ++i) artfLightSetShadow(s, artfSceneAddLight(s, artfVec3((float)i, 3, 0), artfVec3(.2f, .2f, .2f), 9.f), true);
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {3, ARTF_SIMD_128}};
    uint64_t reference = 0;
    for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
        o.simd = runs[i].simd;
        ArtfRenderer* r = artfRendererNew(&o);
        drawInto(r, s, frame);
        const uint64_t h = hashOf(frame, (size_t)W * H);
        if (!i) reference = h;
        CHECK(h == reference);
        artfRendererFree(r);
    }
    CHECK(logged > 0);
    artfSetLog(NULL, NULL);
    artfFree(frame);
    artfSceneFree(s);
}

/* ---- materials of your own ---- */

/* The program's own C function, as a material is meant to be written: stripes across u. */
static ArtfExpr stripes(ArtfGraph* g, float count) {
    ArtfExpr x = artfExprFract(g, artfExprMul(g, artfExprX(g, artfExprUv(g)), artfExprNumber(g, count)));
    return artfExprIf(g, artfExprSub(g, x, artfExprNumber(g, .5f)), artfExprConst(g, 1, 1, 1, 1), artfExprConst(g, 0, 0, 0, 1));
}

static ArtfMaterial graphMaterial(ArtfScene* s, ArtfGraph* g, ArtfAlphaMode mode) {
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    d.alphaMode = mode;
    d.graph = artfSceneAddGraph(s, g);
    artfGraphFree(g);
    return artfSceneAddMaterial(s, &d);
}

static void testGraphs(ArtfBackend backend) {
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
        CHECK(near_(CHANNEL(p, 16), 128, 3) && CHANNEL(p, 8) == 0);
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
        CHECK(near_(CHANNEL(p, 16), (int)(sqrtf(sinf(2.f)) * 255.f), 3) && near_(CHANNEL(p, 8), (int)(fabsf(cosf(2.f)) * 255.f), 3));
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
static void testGraphsBothBackends(void) {
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

/* ---- blocks ---- */

/* A KTX2 file of one level, of that format, holding these blocks. */
static size_t makeKtx2(uint8_t* out, uint32_t format, uint32_t width, uint32_t height, const uint8_t* blocks, size_t bytes) {
    static const uint8_t mark[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 13, 10, 26, 10};
    memset(out, 0, 104);
    memcpy(out, mark, 12);
    put32le(out + 12, format);
    put32le(out + 16, 1);
    put32le(out + 20, width);
    put32le(out + 24, height);
    put32le(out + 36, 1);
    put32le(out + 40, 1);
    put32le(out + 80, 104);
    put32le(out + 88, (uint32_t)bytes);
    put32le(out + 96, (uint32_t)bytes);
    memcpy(out + 104, blocks, bytes);
    return 104 + bytes;
}

static void testBlocks(void) {
    /* what is made and turned back stays close to what it was */
    enum { SIDE = 32 };
    static uint32_t px[SIDE * SIDE];
    for (int y = 0; y < SIDE; ++y)
        for (int x = 0; x < SIDE; ++x) px[y * SIDE + x] = (uint32_t)(255 - x * 4) << 24 | (uint32_t)(x * 8) << 16 | (uint32_t)(y * 8) << 8 | (uint32_t)((x + y) * 4);
    for (int kind = 0; kind < 4; ++kind) {
        ArtfScene* s = artfSceneNew();
        static uint32_t opaque[SIDE * SIDE];
        for (int i = 0; i < SIDE * SIDE; ++i) opaque[i] = px[i] | 0xFF000000u;
        const ArtfTexture id = artfSceneAddTexture(s, kind & 1 ? px : opaque, SIDE, SIDE, 0);
        const ArtfTextureData* t = &s->textures.items[id];
        uint32_t format, offsets[16];
        size_t size;
        uint8_t* blocks = artfTextureEncode(t, kind >= 2, &format, &size, offsets);
        CHECK(size < (size_t)SIDE * SIDE * 4 * 2 / 3 * 2);
        ArtfTextureData back;
        memset(&back, 0, sizeof back);
        back.width = t->width;
        back.height = t->height;
        back.mipCount = t->mipCount;
        back.format = format;
        back.blocks = blocks;
        memcpy(back.blockOffsets, offsets, sizeof offsets);
        artfTextureDecode(&back);
        int worst = 0;
        for (int i = 0; i < SIDE * SIDE; ++i)
            for (int shift = 0; shift < 32; shift += 8) worst = artfMaxi(worst, abs(CHANNEL(back.pixels[i], shift) - CHANNEL(t->pixels[i], shift)));
        printf("blocks %d: the worst a channel strayed: %d\n", kind, worst);
        CHECK(worst < 40);
        artfFree(back.pixels);
        artfFree(blocks);
        artfSceneFree(s);
    }

    /* a file: read, and what is wrong in it refused */
    static uint8_t ktx[104 + 4096];
    static uint8_t blocks[4096];
    uint32_t state = 7;
    for (int i = 0; i < 4096; ++i) {
        state = state * 1664525u + 1013904223u;
        blocks[i] = (uint8_t)(state >> 24);
    }
    artfSetLog(countLog, &logged);
    logged = 0;
    ArtfScene* s = artfSceneNew();
    const size_t size = makeKtx2(ktx, 137, 64, 64, blocks, 4096);
    CHECK(artfSceneAddTextureKtx2(s, ktx, size) != 0);
    CHECK(artfSceneAddTextureKtx2(s, ktx, 100) == 0);
    CHECK(artfSceneAddTextureKtx2(s, "not this", 8) == 0);
    for (size_t cut = 0; cut < 110; ++cut) artfSceneAddTextureKtx2(s, ktx, cut);
    for (size_t i = 12; i < 104; ++i) {
        static uint8_t bad[104 + 4096];
        memcpy(bad, ktx, size);
        bad[i] ^= 0xFF;
        artfSceneAddTextureKtx2(s, bad, size);
    }
    CHECK(logged > 2);
    artfSetLog(NULL, NULL);
    artfSceneFree(s);

    /* The GPU knows these formats better than this code: random blocks, each format it takes, drawn by both.
     * Random blocks reach every mode, so what is wrong in a decoder shows as noise that does not match. */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    static const struct { uint32_t format, bytes; } formats[] = {{131, 8}, {137, 16}, {147, 8}, {151, 16}};
    for (size_t f = 0; f < ARTF_COUNT(formats); ++f) {
        s = plainScene();
        const size_t bytes = (size_t)(64 / 4) * (64 / 4) * formats[f].bytes;
        const size_t fileSize = makeKtx2(ktx, formats[f].format, 64, 64, blocks, bytes);
        ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTextureKtx2(s, ktx, fileSize), 0, 1, artfVec3(0, 0, 0)};
        addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, 0, 2);
        drawInto(gpu, s, a);
        drawInto(cpu, s, b);
        const double far = differentShare(a, b, 12);
        printf("format %u: pixels that differ by more than 12 of 255: %.2f%%\n", formats[f].format, far * 100);
        CHECK(far < .01);
        artfSceneFree(s);
    }
    /* plain textures compressed as they go up look like themselves */
    artfRendererFree(gpu);
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.compressTextures = true;
    gpu = artfRendererNew(&o);
    s = richScene();
    drawInto(gpu, s, a);
    drawInto(cpu, s, b);
    CHECK(differentShare(a, b, 24) < .02);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}

/* ---- noise ---- */

/* How far a picture is from another, on average, in steps of 255. */
static double errorOf(const uint32_t* a, const uint32_t* b) {
    double sum = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i)
        for (int shift = 0; shift < 24; shift += 8) sum += abs(CHANNEL(a[i], shift) - CHANNEL(b[i], shift));
    return sum / ((double)W * H * 3);
}

static void testDenoise(ArtfBackend backend) {
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
static void testDenoiseChange(ArtfBackend backend) {
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
static void testDenoiseMotion(ArtfBackend backend) {
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

/* Occlusion tried both ways: the frames do not change, and it goes back and forth only in its rounds. */
static void testOcclusionAuto(ArtfBackend backend) {
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

/* A map that did not change stays as it is; one that did comes out as a new renderer draws it. */
static void testShadowCache(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    ArtfMaterialDesc m = {artfVec3(.8f, .8f, .8f), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfTransform at = artfTransformAt(2, 0, 2, 1);
    const ArtfInstance moving = artfSceneAddInstance(s, artfSceneAddCube(s, 1), artfSceneAddMaterial(s, &m), &at);
    for (int i = 0; i < 40; ++i) addAt(s, artfSceneAddCube(s, .3f), 0, (float)(i % 8) * 12.f - 40.f, 0, (float)(i / 8) * 12.f - 30.f, 1);
    const ArtfShadowDesc maps = {256, 0};
    artfSceneSetShadows(s, &maps);
    ArtfFrameStats stats;
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn >= 1);
    drawInto(r, s, a);
    drawInto(r, s, b);
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn == 0);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    at = artfTransformAt(-1, 0, 3, 1);
    artfInstanceSetTransform(s, moving, &at);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn >= 1);
    drawInto(r, s, a);
    artfRendererFree(r);
    r = artfRendererNew(&o);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* Between two levels both are drawn, each in its pixels: nothing is left open, and the picture is between the two. */
static void testLodFade(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says) */
    const ArtfMesh mesh = artfSceneAddSphere(s, 1, 64, 96);
    const ArtfMeshData* m = &s->meshes.items[mesh - 1];
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
static void testGpuMeshlets(void) {
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

/* Two layers that cut through each other: each is in front on its own side. */
static void testLayersSorted(ArtfBackend backend) {
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

/* Two lamps apart, and something that moves by one of them: the other's maps stay, and what comes out is what a new
 * renderer draws. */
static void testShadowRegions(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    setCamera(s, 0, 6, 14, 0, 0, 0);
    ArtfMaterial white = whiteMaterial(s);
    ArtfTransform at = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 40, 1), white, &at);
    const ArtfMesh cube = artfSceneAddCube(s, 1);
    addAt(s, cube, white, 8, 0, 0, 1);
    at = artfTransformAt(-8, 0, 0, 1);
    const ArtfInstance moving = artfSceneAddInstance(s, cube, white, &at);
    const ArtfShadowDesc maps = {256, 0};
    artfSceneSetShadows(s, &maps);
    artfLightSetShadow(s, artfSceneAddLight(s, artfVec3(8, 3, 0), artfVec3(9, 9, 9), 6), true);
    artfLightSetShadow(s, artfSceneAddLight(s, artfVec3(-8, 3, 0), artfVec3(9, 9, 9), 6), true);
    ArtfFrameStats stats;
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn == 12);
    CHECK(artfRendererDraw(r, s));
    at = artfTransformAt(-7, 0, 1, 1);
    artfInstanceSetTransform(s, moving, &at);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn >= 1 && stats.shadowMapsDrawn <= 6);
    drawInto(r, s, a);
    artfRendererFree(r);
    r = artfRendererNew(&o);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* A layer behind what is solid, with four samples a pixel: at the edge it stays behind, on both backends. */
static void testLayersBehindSolid(void) {
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

/* What bones bend keeps what it gathered, as what moves whole does. */
static void testDenoiseBones(ArtfBackend backend) {
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

static void testDenoiseBytes(void) {
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

/* ---- what comes and goes ---- */

static void testRemoving(ArtfBackend backend) {
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

static void testPrepared(void) {
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

static void testResidency(ArtfBackend backend) {
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

/* Blocks of noise, turned into pixels, hashed. The hashes are of what was checked, block by block, against a decoder of
 * someone else's: 65,536 blocks of BC7 alike, and 46,000 of ASTC within one of 255 (it rounds a step sooner). */
static void testBlockDecoders(void) {
    uint32_t state = 12345;
    uint64_t bc7 = 1469598103934665603ull, astc = bc7;
    uint32_t out[144];
    int drawn = 0;
    for (int i = 0; i < 200000; ++i) {
        uint8_t b[16];
        for (int k = 0; k < 16; ++k) {
            state = state * 1664525u + 1013904223u;
            b[k] = (uint8_t)(state >> 24);
        }
        artfDecodeBc7(b, out);
        for (int k = 0; k < 16; ++k) bc7 = (bc7 ^ out[k]) * 1099511628211ull;
        artfDecodeAstc(b, 6, 6, out);
        if (out[0] != 0xFFFF00FFu) ++drawn;
        for (int k = 0; k < 36; ++k) astc = (astc ^ out[k]) * 1099511628211ull;
    }
    printf("blocks: bc7 %016llx, astc %016llx, %d of ASTC's are legal\n", (unsigned long long)bc7, (unsigned long long)astc, drawn);
    CHECK(bc7 == 0x173e39b598bebd9dull);
    CHECK(astc == 0x6bdc1b46124e7596ull);
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

static void testStreamedTexture(ArtfBackend backend) {
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
static void testResidencyGraph(ArtfBackend backend) {
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

/* A device lost in the middle of it all: false, and false after that; the renderer can be freed; and the scene
 * goes to a new one, which draws what the first did. */
static void testLostDevice(void) {
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
static void testRefusedSwapchain(void) {
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
static void testRefusedSwapchain(void) {}
#endif

/* A place that was left is taken again, and what is drawn from it is what is there now. */
static void testMeshPlaces(ArtfBackend backend) {
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

static void testGltfSparse(void) {
    static const char text[] = "{\"asset\":{\"version\":\"2.0\"},%s\"buffers\":[{\"byteLength\":44,\"uri\":\"data:application/octet-stream;base64,AAABAAIAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAACAPwAAAAA=\"}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":6},{\"buffer\":0,\"byteOffset\":8,\"byteLength\":36}],\"accessors\":[{\"componentType\":5126,\"type\":\"VEC3\",\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0],\"sparse\":{\"count\":3,\"indices\":{\"bufferView\":0,\"componentType\":5123},\"values\":{\"bufferView\":1}}}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    char file[1200];
    int size = snprintf(file, sizeof file, text, "");
    ArtfScene* s = artfSceneNew();
    CHECK(artfSceneLoadGltfMemory(s, file, (size_t)size, NULL, NULL) == 1);
    /* the three points came from the changes alone: a triangle with an area */
    CHECK(s->meshes.len >= 1 && s->meshes.items[0].vertexCount == 3 && s->meshes.items[0].radius > .4f);
    artfSceneFree(s);
    s = artfSceneNew();
    size = snprintf(file, sizeof file, text, "\"extensionsRequired\":[\"KHR_nothing_here\"],");
    CHECK(artfSceneLoadGltfMemory(s, file, (size_t)size, NULL, NULL) == 0);
    size = snprintf(file, sizeof file, text, "\"extensionsRequired\":[\"KHR_mesh_quantization\"],");
    CHECK(artfSceneLoadGltfMemory(s, file, (size_t)size, NULL, NULL) == 1);
    artfSceneFree(s);
}

/* KHR_lights_punctual, cameras, KHR_materials_emissive_strength and KHR_texture_transform. */
static void testGltfExtensions(void) {
    static const char file[] =
        "{\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"KHR_texture_transform\"],\"extensions\":{\"KHR_ligh"
        "ts_punctual\":{\"lights\":[{\"type\":\"point\",\"intensity\":20,\"range\":10,\"color\":[1,0.5,0.25]},{\"type\""
        ":\"directional\",\"intensity\":3},{\"type\":\"spot\",\"intensity\":5,\"spot\":{\"innerConeAngle\":0.2,\"outerC"
        "oneAngle\":0.5}}]}},\"buffers\":[{\"byteLength\":84,\"uri\":\"data:application/octet-stream;base64,AAAAAAAAAAA"
        "AAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAAA/AAAAAAAAAAAAAAA/\"}],\"bu"
        "fferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":"
        "24},{\"buffer\":0,\"byteOffset\":60,\"byteLength\":24}],\"accessors\":[{\"bufferView\":0,\"componentType\":512"
        "6,\"type\":\"VEC3\",\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]},{\"bufferView\":1,\"componentType\":5126,\"ty"
        "pe\":\"VEC2\",\"count\":3},{\"bufferView\":2,\"componentType\":5126,\"type\":\"VEC2\",\"count\":3}],\"material"
        "s\":[{\"emissiveFactor\":[1,1,1],\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":4}}"
        ",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"extensions\":{\"KHR_texture_transform\":{\"offs"
        "et\":[0.5,0],\"scale\":[2,2],\"texCoord\":1}}}}}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0"
        ",\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"material\":0}]}],\"cameras\":[{\"type\":\"perspective\",\"perspective\":"
        "{\"yfov\":0.7,\"znear\":0.05}}],\"nodes\":[{\"mesh\":0},{\"translation\":[0,2,0],\"extensions\":{\"KHR_lights_"
        "punctual\":{\"light\":0}}},{\"extensions\":{\"KHR_lights_punctual\":{\"light\":1}}},{\"translation\":[1,1,1],\""
        "extensions\":{\"KHR_lights_punctual\":{\"light\":2}}},{\"translation\":[0,0,5],\"camera\":0}],\"scenes\":[{\"n"
        "odes\":[0,1,2,3,4]}],\"scene\":0}";
    ArtfScene* s = artfSceneNew();
    const ArtfModel m = artfSceneLoadModelMemory(s, file, sizeof file - 1, NULL, NULL);
    CHECK(m && artfModelInstanceCount(s, m) == 1);
    /* the second set of coordinates, moved by half and doubled: (0.5, 0) and (0, 0.5) become (1.5, 0) and (0.5, 1) */
    float most = 0, least = 9;
    const ArtfMeshData* mesh = &s->meshes.items[0];
    for (uint32_t i = 0; i < mesh->vertexCount; ++i) {
        most = artfMaxf(most, artfHalfToFloat(mesh->vertices[i].u));
        least = artfMinf(least, artfHalfToFloat(mesh->vertices[i].u));
    }
    CHECK(most == 1.5f && least == .5f);
    CHECK(s->materials.items[s->materials.len - 1].emissive[0] == 4.f);
    /* the point light, its range kept; the sun, down -z; the spot, a cone */
    CHECK(s->lightOn[0] && s->lights[0].position[1] == 2.f && s->lights[0].radius == 10.f);
    CHECK(s->lights[0].color[0] > s->lights[0].color[1] && s->lights[0].color[1] > s->lights[0].color[2]);
    CHECK(s->sunColor.x == 3.f && s->toSun.z > .99f);
    CHECK(s->lightOn[1] && s->lights[1].spotCut > .87f && s->lights[1].spotCut < .88f);
    ArtfCamera eye;
    CHECK(artfModelCameraCount(s, m) == 1 && artfModelCamera(s, m, 0, &eye));
    CHECK(eye.position.z == 5.f && eye.target.z == 4.f && eye.up.y == 1.f && eye.fovY == .7f);
    CHECK(!artfModelCamera(s, m, 1, &eye));
    /* lights and cameras go with the model */
    const ArtfTransform moved = artfTransformAt(3, 0, 0, 1);
    artfModelSetTransform(s, m, &moved);
    CHECK(s->lights[0].position[0] == 3.f && artfModelCamera(s, m, 0, &eye) && eye.position.x == 3.f);
    artfSceneFree(s);
}

/* Light from all around, from a picture: the maps, a Radiance file, and both backends drawing the same. */
static void testEnvironment(void) {
    /* a picture lit above and dark below, with a brighter patch where -z is */
    enum { PW = 64, PH = 32 };
    float* picture = artfMalloc(PW * PH * 12);
    for (int y = 0; y < PH; ++y)
        for (int x = 0; x < PW; ++x) {
            float* p = picture + 3 * (y * PW + x);
            const float v = y < PH / 2 ? 1.f : 0.f;
            p[0] = p[1] = p[2] = v;
            if (y >= PH / 2 - 4 && y < PH / 2 && x >= PW / 2 - 2 && x < PW / 2 + 2) p[0] = 50.f;
        }
    ArtfScene* s = artfSceneNew();
    CHECK(artfSceneSetEnvironment(s, picture, PW, PH, 1.f, true));
    float c[3];
    artfEnvSample(s->environment, ARTF_ENV_DIFFUSE, 0, 1, 0, c);
    CHECK(fabsf(c[1] - 1.f) < .02f);
    artfEnvSample(s->environment, ARTF_ENV_DIFFUSE, 0, -1, 0, c);
    CHECK(c[1] < .02f);
    artfEnvSample(s->environment, ARTF_ENV_DIFFUSE, 1, 0, 0, c);
    CHECK(fabsf(c[1] - .5f) < .05f);
    /* the patch is toward -z, a little above the horizon: sharp in the first map, spread in the rough ones */
    float sharp[3], away[3], rough[3];
    artfEnvSample(s->environment, 0, 0, .1f, -1, sharp);
    artfEnvSample(s->environment, 0, 0, .1f, 1, away);
    artfEnvSample(s->environment, 4, 0, .1f, -1, rough);
    CHECK(sharp[0] > 20.f && away[0] < 1.5f && rough[0] > 1.f && rough[0] < sharp[0]);
    /* the border repeats what is across the fold: no seam below */
    float left[3], right[3];
    artfEnvSample(s->environment, 2, -.01f, -1, .3f, left);
    artfEnvSample(s->environment, 2, .01f, -1, .3f, right);
    CHECK(fabsf(left[1] - right[1]) < .02f);

    /* the same scene, on both backends */
    const ArtfMesh ball = artfSceneAddSphere(s, 1, 32, 48);
    ArtfMaterialDesc metal = artfMaterialDefaults();
    metal.baseColor = artfVec3(.9f, .9f, .9f);
    metal.metallic = 1;
    metal.roughness = .3f;
    artfSceneAddInstance(s, ball, artfSceneAddMaterial(s, &metal), NULL);
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    const ArtfCamera eye = {{0, .5f, 3.5f}, {0, 0, 0}, {0, 1, 0}, .9f, .1f};
    artfSceneSetCamera(s, &eye);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    drawInto(cpu, s, a);
    /* the background is the picture: dark below, lit above */
    CHECK((a[(H - 1) * W] & 255) < 20 && (a[0] & 255) > 200);
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("light from all around, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .01);
        artfRendererFree(gpu);
    }
    /* without it, the ball shows sky and ground instead */
    artfSceneSetEnvironment(s, NULL, 0, 0, 0, false);
    drawInto(cpu, s, b);
    CHECK(differentShare(a, b, 24) > .2);
    artfRendererFree(cpu);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfFree(picture);

    /* a Radiance file, flat and in runs: 1, 2 and 0.5 */
    static const uint8_t flat[] = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n\x80\x80\x80\x81\x80\x80\x80\x82";
    int fw = 0, fh = 0;
    float* got = artfImageDecodeHdr(flat, sizeof flat - 1, &fw, &fh);
    CHECK(got && fw == 2 && fh == 1 && got[0] == 1.f && got[3] == 2.f);
    artfFree(got);
    uint8_t runs[64 + 8 * 4];
    const char head[] = "#?RADIANCE\n\n-Y 1 +X 8\n";
    size_t at = sizeof head - 1;
    memcpy(runs, head, at);
    runs[at++] = 2; runs[at++] = 2; runs[at++] = 0; runs[at++] = 8;
    for (int ch = 0; ch < 4; ++ch) { runs[at++] = 128 + 8; runs[at++] = ch == 3 ? 128 : 128; }
    got = artfImageDecodeHdr(runs, at, &fw, &fh);
    CHECK(got && fw == 8 && got[21] == .5f);
    artfFree(got);
    CHECK(!artfImageDecodeHdr(runs, at - 3, &fw, &fh));
    CHECK(!artfImageDecodeHdr("not one", 7, &fw, &fh));
}

/* A frame in floats: the same picture as before with nothing after it; each effect the same on both backends; lights
 * past the frame's own; light that bounced. */
static ArtfScene* bleedScene(void) {
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

static void testFloats(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* plain = artfRendererNew(&o);
        o.hdr = true;
        ArtfRenderer* floats = artfRendererNew(&o);
        if (!plain || !floats) {
            artfRendererFree(plain);
            artfRendererFree(floats);
            continue;
        }
        ArtfScene* s = bleedScene();
        drawInto(plain, s, a);
        drawInto(floats, s, b);
        /* nothing after it: the picture as it was, to a level */
        int worst = 0;
        for (int i = 0; i < W * H; ++i)
            for (int k = 0; k < 24; k += 8) worst = artfMaxi(worst, abs((int)(a[i] >> k & 255) - (int)(b[i] >> k & 255)));
        printf("%s, a frame in floats against one in bytes: %d at most\n", backend ? "GPU" : "CPU", worst);
        CHECK(worst <= 1);
        /* each effect changes the picture, and the same way every time */
        const ArtfPostDesc each[6] = {{.bloom = .5f, .bloomThreshold = .1f}, {.occlusion = 1, .occlusionRadius = .8f}, {.reflections = 1}, {.focus = 3, .aperture = 10},
                                      {.volumetric = 1, .scattering = .5f}, {.temporal = true}};
        for (int e = 0; e < 6; ++e) {
            artfSceneSetPost(s, &each[e]);
            if (e == 4) artfSceneSetFog(s, artfVec3(.5f, .5f, .55f), .05f);
            drawInto(floats, s, c);
            const double changed = differentShare(b, c, 2);
            if (changed <= (e == 5 || e == 2 ? .001 : .01)) printf("effect %d changed %.4f of the pixels\n", e, changed);
            CHECK(changed > (e == 5 || e == 2 ? .001 : .01));
            if (e < 5) {
                drawInto(floats, s, a);
                CHECK(differentShare(a, c, 0) == 0);
            }
            if (e == 4) artfSceneSetFog(s, artfVec3(0, 0, 0), 0);
        }
        artfSceneSetPost(s, NULL);
        /* light that bounced: the floor by the red wall turns red */
        const int fx = W / 2 + 28, fy = H / 2 + 34;
        drawInto(plain, s, a);
        const ArtfIndirectDesc probes = {{8, 4, 8}, 64, .5f, 1, {-4, -1.5f, -3}, {4, 3, 3}};
        artfSceneSetIndirect(s, &probes);
        for (int k = 0; k < 6; ++k) drawInto(plain, s, b);
        const double before = meanOf(a, fx, fy, 16) - meanOf(a, fx, fy, 8), after = meanOf(b, fx, fy, 16) - meanOf(b, fx, fy, 8);
        printf("%s, light that bounced: red over green by the wall, %.1f without, %.1f with\n", backend ? "GPU" : "CPU", before, after);
        CHECK(after > before + 3);
        artfSceneFree(s);
        artfRendererFree(plain);
        artfRendererFree(floats);
    }
    /* the backends agree, on each effect and on light that bounced */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        o.backend = ARTF_BACKEND_SOFTWARE;
        ArtfRenderer* cpu = artfRendererNew(&o);
        const ArtfPostDesc all = {.bloom = .2f, .occlusion = 1, .reflections = 1, .focus = 6, .aperture = 6};
        ArtfScene* s = bleedScene();
        artfSceneSetPost(s, &all);
        const ArtfIndirectDesc probes = {{8, 4, 8}, 64, .5f, 1, {-4, -1.5f, -3}, {4, 3, 3}};
        artfSceneSetIndirect(s, &probes);
        drawInto(cpu, s, a);
        artfSceneSetIndirect(s, &probes); /* (the probes start over, so both see the same steps) */
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 16);
        printf("all effects and probes, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .03);
        /* lights past the frame's own */
        artfSceneSetPost(s, NULL);
        artfSceneSetIndirect(s, NULL);
        artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
        uint32_t st = 5;
        for (int i = 0; i < 100; ++i) {
            st = st * 747796405u + 2891336453u;
            const float x = (float)(st >> 8 & 255) / 255.f * 8.f - 4.f, z = (float)(st >> 16 & 255) / 255.f * 6.f - 3.f;
            CHECK(artfSceneAddLight(s, artfVec3(x, -.6f, z), artfVec3((float)(i % 3 == 0), (float)(i % 3 == 1), (float)(i % 3 == 2)), 1.5f) != 0);
        }
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        CHECK(meanOf(a, W / 2, H * 3 / 4, 16) > 30);
        const double lightsApart = differentShare(a, b, 16);
        printf("100 lights past the frame's own, the GPU against the CPU: %.2f%% of pixels apart\n", lightsApart * 100);
        CHECK(lightsApart < .02);
        artfSceneFree(s);
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* Particles and terrain: made of the scene's own parts, so the backends draw them alike. */
static void testExtras(void) {
    ArtfScene* s = artfSceneNew();
    setCamera(s, 0, 1, 6, 0, 0, 0);
    static float heights[33 * 33];
    for (int z = 0; z < 33; ++z)
        for (int x = 0; x < 33; ++x) heights[z * 33 + x] = (float)((x - 16) * (x - 16) + (z - 16) * (z - 16)) / 512.f;
    const ArtfMesh hill = artfSceneAddTerrain(s, heights, 33, 33, .25f, 2.f, 1.f);
    CHECK(hill != 0);
    CHECK(s->meshes.items[hill - 1].vertexCount == 33 * 33 && s->meshes.items[hill - 1].indexCount == 32 * 32 * 6);
    const ArtfTransform below = artfTransformAt(0, -1.5f, 0, 1);
    artfSceneAddInstance(s, hill, whiteMaterial(s), &below);
    ArtfEmitterDesc d;
    memset(&d, 0, sizeof d);
    d.velocity = artfVec3(0, 1, 0);
    d.spread = .3f;
    d.rate = 200;
    d.life = 1;
    d.size = .3f;
    d.sizeEnd = .1f;
    d.color = artfVec3(1, .6f, .2f);
    d.colorEnd = artfVec3(1, .2f, .1f);
    d.opacity = 1;
    d.glow = 4;
    d.most = 100;
    const ArtfEmitter e = artfSceneAddEmitter(s, &d);
    for (int i = 0; i < 30; ++i) artfEmitterStep(s, e, 1.f / 30.f);
    /* as many as the rate and the life allow, and no more than most */
    const uint32_t alive = artfEmitterAlive(s, e);
    CHECK(alive >= 90 && alive <= 100);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    drawInto(cpu, s, a);
    /* the fire glows over the hill: somewhere above the middle, bright */
    int brightest = 0;
    for (int y = H / 4; y < H / 2; ++y)
        for (int x = W / 2 - 20; x < W / 2 + 20; ++x) brightest = artfMaxi(brightest, (int)(a[y * W + x] >> 16 & 255));
    CHECK(brightest > 240);
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("particles and terrain, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .02);
        artfRendererFree(gpu);
    }
    /* gone, they leave nothing */
    artfSceneRemoveEmitter(s, e);
    drawInto(cpu, s, b);
    CHECK(differentShare(a, b, 24) > .002);
    artfRendererFree(cpu);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
}

static void testGltfTargets(void) {
    static const char file[] = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":80,\"uri\":\"data:application/octet-stream;base64,AAABAAIAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAQAAAAAA=\"}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":6},{\"buffer\":0,\"byteOffset\":8,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":44,\"byteLength\":36}],\"accessors\":[{\"bufferView\":0,\"componentType\":5123,\"type\":\"SCALAR\",\"count\":3},{\"bufferView\":1,\"componentType\":5126,\"type\":\"VEC3\",\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]},{\"bufferView\":2,\"componentType\":5126,\"type\":\"VEC3\",\"count\":3}],\"meshes\":[{\"weights\":[1],\"primitives\":[{\"attributes\":{\"POSITION\":1},\"indices\":0,\"targets\":[{\"POSITION\":2}]}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    ArtfScene* s = artfSceneNew();
    const ArtfModel m = artfSceneLoadModelMemory(s, file, sizeof file - 1, NULL, NULL);
    CHECK(m && artfModelInstanceCount(s, m) == 1);
    artfScenePosesApply(s);
    /* the top went from 1 to 4, as the mesh's own weight says */
    ArtfVertex v[3];
    artfMeshUnpack(&s->meshes.items[s->instances.items[0].mesh], v);
    float top = 0;
    for (int i = 0; i < 3; ++i) top = artfMaxf(top, v[i].py);
    CHECK(fabsf(top - 4.f) < .02f);
    artfSceneFree(s);
}

static void testSlerp(void) {
    /* around y, at a steady speed, even where the two are far apart */
    const float half = 1.3089969f; /* 150 degrees, halved */
    const float a[4] = {0, 0, 0, 1}, b[4] = {0, sinf(half), 0, cosf(half)};
    for (int i = 1; i < 8; ++i) {
        float q[4];
        artfQuatSlerp(a, b, (float)i / 8.f, q);
        CHECK(fabsf(q[1] - sinf(half * (float)i / 8.f)) < 1e-5f && fabsf(q[3] - cosf(half * (float)i / 8.f)) < 1e-5f);
    }
}

/* ---- what bends ---- */

/* A strip standing up, of two bones: what is below y = 1 stays with the first, what is above goes with the second. */
static ArtfMesh addStrip(ArtfScene* s) {
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
static void bendStrip(ArtfScene* s, ArtfInstance i, float angle) {
    const ArtfTransform turn = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 0, 1), angle), artfVec3(1, 1, 1));
    ArtfTransform joints[2] = {artfTransformIdentity(), turn};
    /* about the joint: move it to the origin, turn, and back */
    const float* m = turn.m;
    joints[1].m[3] = -m[1];
    joints[1].m[7] = 1.f - m[5];
    artfInstanceSetJoints(s, i, joints, 2);
}

static void testSkin(ArtfBackend backend) {
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
static void testMorph(ArtfBackend backend) {
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

static void testSkinBothBackends(void) {
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

/* ---- glTF ---- */

static void put32le(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

/* A .glb made here: two triangles of a square, with colors, under a node that moves and scales it. */
static size_t makeGlb(uint8_t* out, const char* extra, bool breakIt) {
    static const float positions[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    static const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    char json[1400];
    int n = snprintf(json, sizeof json,
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"children\":[1],\"translation\":[1,0,0]},{\"mesh\":0,\"scale\":[2,2,2]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.25,1,1]%s}],"
        "\"buffers\":[{\"byteLength\":60}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":48},{\"buffer\":0,\"byteOffset\":48,\"byteLength\":%d}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}]}",
        extra, breakIt ? 4000 : 12);
    while (n & 3) json[n++] = ' ';
    memcpy(out, "glTF", 4);
    put32le(out + 4, 2);
    put32le(out + 8, (uint32_t)(12 + 8 + n + 8 + 60));
    put32le(out + 12, (uint32_t)n);
    put32le(out + 16, 0x4E4F534Au);
    memcpy(out + 20, json, (size_t)n);
    uint8_t* bin = out + 20 + n;
    put32le(bin, 60);
    put32le(bin + 4, 0x004E4942u);
    memcpy(bin + 8, positions, 48);
    memcpy(bin + 56, indices, 12);
    return (size_t)(20 + n + 8 + 60);
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
        "\"nodes\":[{\"mesh\":0,\"translation\":[0,0,0]},{\"children\":[2]},{\"translation\":[0,1,0]},{\"mesh\":0,\"skin\":0}],"
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

static void testClips(void) {
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
    /* a matching clip in a scene with no clip asked for stays as it is */
    artfSetLog(countLog, &logged);
    logged = 0;
    artfAnimationSet(s, m, 9, 0);
    artfAnimationSet(s, 99, 0, 0);
    CHECK(logged == 2);
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

static void testGltf(void) {
    static uint8_t glb[4096];
    size_t size = makeGlb(glb, "}", false);
    ArtfScene* s = plainScene();
    CHECK(artfSceneLoadGltfMemory(s, glb, size, NULL, NULL) == 1);
    {
        /* the same, made apart from the scene */
        ArtfPreparedModel* prepared = artfPrepareModelMemory(glb, size, NULL);
        CHECK(prepared != NULL);
        ArtfScene* other = artfSceneNew();
        const ArtfModel model = artfSceneAddPreparedModel(other, prepared, NULL);
        CHECK(model && artfModelInstanceCount(other, model) == 1);
        CHECK(other->meshes.len == s->meshes.len);
        artfSceneFree(other);
        artfPreparedModelFree(artfPrepareModelMemory(glb, size, NULL)); /* (or left behind) */
    }
    ArtfVec3 center;
    float radius;
    CHECK(artfSceneBounds(s, &center, &radius));
    CHECK(near_((int)(center.x * 100.f), 100, 2) && near_((int)(center.y * 100.f), 0, 2));
    /* the node's scale came through, and its parent's move: what is at the middle is the material's color */
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    drawInto(r, s, a);
    const size_t at = (size_t)(H / 2) * W + W / 2 + 36;
    CHECK(near_(CHANNEL(a[at], 16), (int)(sqrtf(.5f) * 255.f), 2) && near_(CHANNEL(a[at], 8), (int)(sqrtf(.25f) * 255.f), 2));
    artfSceneFree(s);

    /* what is wrong is left out, and nothing goes past what is there */
    artfSetLog(countLog, &logged);
    logged = 0;
    s = plainScene();
    size = makeGlb(glb, "}", true);
    CHECK(artfSceneLoadGltfMemory(s, glb, size, NULL, NULL) == 1); /* (only its indices are gone: all vertices, no triangles) */
    artfSceneFree(s);
    s = plainScene();
    CHECK(artfSceneLoadGltfMemory(s, "{\"asset\":{\"version\":\"1.0\"}}", 28, NULL, NULL) == 0);
    CHECK(artfSceneLoadGltfMemory(s, "{not json", 9, NULL, NULL) == 0);
    CHECK(artfSceneLoadGltf(s, "no such file.glb", NULL) == 0);
    CHECK(logged >= 3);
    /* every cut of a good file, and every byte broken in turn: no crash, whatever comes out */
    size = makeGlb(glb, "}", false);
    for (size_t cut = 0; cut < size; cut += 7) artfSceneLoadGltfMemory(s, glb, cut, NULL, NULL);
    static uint8_t bad[4096];
    for (size_t i = 0; i < size; i += 3) {
        memcpy(bad, glb, size);
        bad[i] ^= 0x5A;
        artfSceneLoadGltfMemory(s, bad, size, NULL, NULL);
    }
    artfSceneFree(s);
    artfSetLog(NULL, NULL);

    /* an image: a PNG of 2 x 1 pixels, stored, one red and one half blue */
    static const uint8_t png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 20, 'I', 'D', 'A', 'T', 0x78, 1, 1, 9, 0, 0xF6, 0xFF, 0, 255, 0, 0, 255, 0, 0, 255, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0};
    int w = 0, h = 0;
    uint32_t* pixels = artfImageDecode(png, sizeof png, &w, &h);
    CHECK(pixels && w == 2 && h == 1);
    if (pixels) CHECK(pixels[0] == 0xFFFF0000u && pixels[1] == 0x800000FFu);
    artfFree(pixels);
    CHECK(!artfImageDecode("not a picture", 13, &w, &h));

    artfFree(a);
    artfRendererFree(r);
}

/* ---- rays ---- */

static void traceWith(ArtfScene* s, ArtfTraceMode mode, int samples, int bounces) {
    ArtfTraceDesc d = {mode, 1, samples, bounces};
    artfSceneSetTrace(s, &d);
}

static void drawSamples(ArtfRenderer* r, ArtfScene* s, int samples, uint32_t* frame) {
    for (int i = 0; i < samples; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
}

/* A mean of a little square of pixels around (x, y), for what is noisy. */
static double meanOf(const uint32_t* frame, int x, int y, int shift) {
    double sum = 0;
    for (int j = -3; j <= 3; ++j)
        for (int i = -3; i <= 3; ++i) sum += CHANNEL(frame[(size_t)(y + j) * W + x + i], shift);
    return sum / 49;
}

static void testTrace(ArtfBackend backend) {
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
static void testTraceData(void) {
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
static void testTraceBothBackends(void) {
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

static void testFluid(void) {
    const size_t cells = (size_t)ARTF_FLUID_WIDTH * ARTF_FLUID_HEIGHT;
    const float inner = 3.83f, outer = 34.f, spin = .6f;
    ArtfFluid* a = artfFluidNew(inner, outer, spin, 0);
    ArtfFluid* b = artfFluidNew(inner, outer, spin, 0);
    float* before = artfMalloc(cells * 4);
    memcpy(before, artfFluidField(a), cells * 4);

    /* it moves, the same way however the time is cut up */
    CHECK(artfFluidAdvance(a, 2.0));
    CHECK(artfFluidAdvance(b, 1.0));
    CHECK(artfFluidAdvance(b, 2.0));
    CHECK(memcmp(before, artfFluidField(a), cells * 4) != 0);
    CHECK(memcmp(artfFluidField(a), artfFluidField(b), cells * 4) == 0);
    CHECK(!artfFluidAdvance(a, artfFluidTime(a) + .1));

    /* Each ring goes around at Kerr's orbital speed: the shift that best lays a ring on itself as it
     * was is the one its orbit gives. (Over a little time: turbulence soon makes it another ring.) */
    static const int rows[2] = {90, 140};
    const float* now = artfFluidField(a);
    memcpy(before, now, cells * 4);
    const double start = artfFluidTime(a);
    CHECK(artfFluidAdvance(a, start + 2.05));
    const double passed = artfFluidTime(a) - start;
    now = artfFluidField(a);
    for (int k = 0; k < 2; ++k) {
        const int j = rows[k];
        const float rho = (float)j / (float)(ARTF_FLUID_HEIGHT - 1), r = inner / (1.f - rho * (1.f - inner / outer));
        const double expected = passed / ((double)r * sqrt((double)r) + spin) / 6.2831853 * ARTF_FLUID_WIDTH;
        double best = 1e30;
        int found = 0;
        for (int d = 0; d < ARTF_FLUID_WIDTH; ++d) {
            double sum = 0;
            for (int i = 0; i < ARTF_FLUID_WIDTH; ++i) {
                const double gap = (double)before[(size_t)j * ARTF_FLUID_WIDTH + i] - now[(size_t)j * ARTF_FLUID_WIDTH + ((i + d) & (ARTF_FLUID_WIDTH - 1))];
                sum += gap * gap;
            }
            if (sum < best) { best = sum; found = d; }
        }
        printf("the gas at r = %.1f: moved %d cells, an orbit says %.1f\n", (double)r, found, expected);
        CHECK(fabs((double)found - expected) < 3.0);
    }

    /* a long run: it neither blows up nor dies away */
    double first = 0;
    for (size_t i = 0; i < cells; ++i) first += now[i];
    for (double t = start + 3.0; t < 300.0; t += 3.0) artfFluidAdvance(a, t);
    now = artfFluidField(a);
    double sum = 0;
    bool finite = true;
    for (size_t i = 0; i < cells; ++i) {
        finite = finite && now[i] >= 0.f && now[i] < 100.f;
        sum += now[i];
    }
    printf("what shows of the gas, after a long run: %.2f of what it was\n", sum / first);
    CHECK(finite);
    CHECK(sum > first * .5 && sum < first * 2.);
    artfFree(before);
    artfFluidFree(a);
    artfFluidFree(b);
}

static void testBlackHole(void) {
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* a_ = a;
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);

    /* The shadow: what general relativity says, and nothing here assumes. Light passing closer than
     * sqrt(27) masses does not come back; one who stays still at r sees that as the angle whose sine
     * is sqrt(27) sqrt(1 - 2 / r) / r. The disc is kept far out, seen along its axis: near in it would
     * stand in front of the shadow, while from there only the images of it that went around the hole
     * come close to that edge, and they all but touch it. */
    static const float distances[3] = {14.f, 30.f, 80.f};
    for (int i = 0; i < 3; ++i) {
        const float r = distances[i];
        ArtfScene* s = holeScene(r, 1.57079633f, 0, 0);
        ArtfBlackHoleDesc far = s->hole.desc;
        far.discInner = 10.f;
        far.discOuter = 60.f;
        far.discGain = 200.f;
        far.samples = 4;
        far.stars = 0;
        artfSceneSetBlackHole(s, &far);
        /* closing in on the hole so that the shadow fills the frame: a pixel is then a small angle */
        const float sine = sqrtf(27.f) * sqrtf(1.f - 2.f / r) / r, angle = asinf(sine);
        ArtfCamera c = s->camera;
        c.fovY = angle * 2.6f;
        artfSceneSetCamera(s, &c);
        const float expected = tanf(angle) / tanf(c.fovY * .5f) * (float)H * .5f;
        const float seen = shadowRadius(cpu, s, a);
        printf("the shadow at %.0f masses: %.1f pixels, %.1f expected\n", (double)r, (double)seen, (double)expected);
        CHECK(fabsf(seen - expected) < 1.5f);
        artfSceneFree(s);
    }

    /* A spinning hole's shadow is not round and not centered. Seen from its equator, its edges are
     * the two circular orbits of light, one with the spin and one against it, at r = 2 (1 + cos(2/3
     * acos(-+a))), and each one's xi says where that lands. Space turns at the eye too. This is held
     * against no more than those formulas, with nothing but the sky to light the edge. */
    static const float spins[3] = {.3f, .6f, .95f};
    for (int i = 0; i < 3; ++i) {
        const double a = spins[i], r = 60.0;
        ArtfScene* s = holeScene((float)r, 0, 0, 0);
        ArtfBlackHoleDesc lit = s->hole.desc;
        lit.spin = spins[i];
        lit.discGain = 0;
        lit.discThickness = 0;
        lit.stars = 1e4f;
        artfSceneSetBlackHole(s, &lit);
        ArtfCamera c = s->camera;
        c.fovY = .36f;
        artfSceneSetCamera(s, &c);
        CHECK(artfRendererDraw(cpu, s));
        CHECK(artfRendererReadPixels(cpu, a_));
        const uint32_t* row = a_ + (size_t)(H / 2) * W;
        int left = W / 2, right = W / 2;
        while (left > 0 && (row[left] & 255) < 40) --left;
        while (right < W - 1 && (row[right] & 255) < 40) ++right;
        /* where each edge should be: a photon of that xi, seen by one at rest in the turning space */
        const double big = (r * r + a * a) * (r * r + a * a) - a * a * (r * r - 2 * r + a * a);
        const double varpi = sqrt(big) / r, alpha = r * sqrt(r * r - 2 * r + a * a) / sqrt(big), turning = 2 * a * r / big;
        double edge[2];
        for (int k = 0; k < 2; ++k) {
            /* with the spin, then against it */
            const double orbit = 2.0 * (1.0 + cos(2.0 / 3.0 * acos(k ? a : -a)));
            const double xi = -(orbit * orbit * orbit - 3 * orbit * orbit + a * a * orbit + a * a) / (a * (orbit - 1.0));
            const double along = xi * alpha / (varpi * (1.0 - turning * xi));
            edge[k] = along / sqrt(1.0 - along * along) / tan(c.fovY * .5) * H * .5;
        }
        printf("spin %.2f: the shadow from %d to %d pixels, %.1f to %.1f expected\n", a, left + 1 - W / 2, right - W / 2, edge[1], edge[0]);
        CHECK(fabs((double)(right - W / 2) - edge[0]) < 2.0);
        CHECK(fabs((double)(left + 1 - W / 2) - edge[1]) < 2.0);
        artfSceneFree(s);
    }

    /* the same pixels on any instruction set, with any number of threads */
    ArtfScene* s = holeScene(26.f, .06f, .5f, 1.f);
    ArtfBlackHoleDesc all = s->hole.desc;
    all.spin = .9f;
    all.bloom = 1.f;
    all.samples = 2;
    all.shutter = 3.f;
    all.velocity = artfVec3(.3f, 0, 0);
    artfSceneSetBlackHole(s, &all);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    const uint64_t expected = hashOf(a, (size_t)W * H);
    const ArtfSimd simds[3] = {ARTF_SIMD_PORTABLE, ARTF_SIMD_128, ARTF_SIMD_256};
    for (int i = 0; i < 3; ++i)
        for (int threads = 1; threads <= 3; threads += 2) {
            ArtfRendererOptions other = options(ARTF_BACKEND_SOFTWARE, threads, false);
            other.simd = simds[i];
            ArtfRenderer* r = artfRendererNew(&other);
            CHECK(artfRendererDraw(r, s));
            CHECK(artfRendererReadPixels(r, b));
            CHECK(hashOf(b, (size_t)W * H) == expected);
            artfRendererFree(r);
        }
    artfSceneFree(s);

    /* The gas goes from +x toward +z, so on the right it comes at an eye on the +z side: that side
     * is brighter, by as much as relativity says. With Doppler off the two sides are alike but for
     * the turbulence, which has no reason to be even. */
    s = holeScene(26.f, .06f, 0, 0);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    const double left = brightness(a, 0, W / 2), right = brightness(a, W / 2, W);
    CHECK(right > 0 && fabs(left - right) / (left + right) < .2);
    artfSceneFree(s);
    s = holeScene(26.f, .06f, 1.f, 0);
    {
        /* (exposed low: a side that has gone white tells nothing of how much brighter it is) */
        ArtfBlackHoleDesc d = s->hole.desc;
        d.exposure = .1f;
        artfSceneSetBlackHole(s, &d);
    }
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    printf("Doppler: the side that comes at the eye is %.2f times as bright\n", brightness(a, W / 2, W) / brightness(a, 0, W / 2));
    CHECK(brightness(a, W / 2, W) > brightness(a, 0, W / 2) * 1.5);
    artfSceneFree(s);

    /* time turns the gas, and costs no new map */
    s = holeScene(26.f, .06f, 0, 0);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    const uint64_t version = s->hole.mapVersion;
    ArtfBlackHoleDesc d = s->hole.desc;
    d.time = 40.f;
    artfSceneSetBlackHole(s, &d);
    CHECK(s->hole.mapVersion == version);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, b));
    CHECK(hashOf(a, (size_t)W * H) != hashOf(b, (size_t)W * H));

    /* what the scene holds goes in front; with the hole gone, the frame is as it would have been */
    ArtfMaterialDesc red = {artfVec3(0, 0, 0), 0, 0, 1, artfVec3(1, 0, 0)};
    const ArtfVec3 eye = s->camera.position;
    ArtfTransform t = artfTransformAt(0, eye.y * .75f, eye.z * .75f, 1); /* on the way from the eye to the hole */
    artfSceneAddInstance(s, artfSceneAddSphere(s, 1.f, 16, 24), artfSceneAddMaterial(s, &red), &t);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    CHECK((a[(H / 2) * W + W / 2] & 0xFFFFFF) == 0xFF0000);
    CHECK((a[(H / 8) * W + W / 2] & 0xFFFFFF) != 0xFF0000);
    artfSceneSetBlackHole(s, NULL);
    artfSceneSetBackground(s, artfVec3(0, 0, 1));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    CHECK((a[0] & 0xFFFFFF) == 0x0000FF);
    artfSceneSetBlackHole(s, &d);

    /* new sizes, down to nothing to speak of, and none of them a multiple of a tile or of eight lanes */
    static const int sizes[4][2] = {{W / 2, H / 2}, {101, 67}, {3, 2}, {1, 1}};
    for (int i = 0; i < 4; ++i) {
        artfRendererResize(cpu, sizes[i][0], sizes[i][1]);
        CHECK(artfRendererDraw(cpu, s));
    }
    artfRendererResize(cpu, W, H);
    CHECK(artfRendererDraw(cpu, s));

    /* the GPU draws the same hole, with what is in front of it, with and without occlusion */
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        d.doppler = .6f;
        d.bloom = 1.f;
        d.samples = 2;
        d.spin = .8f;
        d.shutter = 2.f;
        d.velocity = artfVec3(.2f, 0, -.1f);
        artfSceneSetBlackHole(s, &d);
        CHECK(artfRendererDraw(cpu, s));
        CHECK(artfRendererReadPixels(cpu, b));
        for (int occlusion = 0; occlusion < 2; ++occlusion) {
            artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, occlusion != 0);
            CHECK(artfRendererDraw(gpu, s));
            CHECK(artfRendererDraw(gpu, s));
            CHECK(artfRendererDraw(gpu, s));
            CHECK(artfRendererReadPixels(gpu, a));
            const double far = differentShare(a, b, 24);
            printf("black hole, pixels that differ by more than 24 of 255: %.2f%%\n", far * 100);
            CHECK(far < .01);
            CHECK(differentShare(a, b, 96) < .001);
        }
        /* new sizes, and a hole that goes and comes back */
        for (int i = 0; i < 4; ++i) {
            artfRendererResize(gpu, sizes[i][0], sizes[i][1]);
            CHECK(artfRendererDraw(gpu, s));
        }
        artfRendererResize(gpu, W, H);
        artfSceneSetBlackHole(s, NULL);
        CHECK(artfRendererDraw(gpu, s));
        artfSceneSetBlackHole(s, &d);
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererReadPixels(gpu, a));
        CHECK(differentShare(a, b, 24) < .01);
        /* another disc goes up */
        d.discOuter = 12.f;
        artfSceneSetBlackHole(s, &d);
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererDraw(cpu, s));
        CHECK(artfRendererReadPixels(gpu, a));
        CHECK(artfRendererReadPixels(cpu, b));
        CHECK(differentShare(a, b, 24) < .01);
        artfRendererFree(gpu);
    }
    artfSceneFree(s);
    artfRendererFree(cpu);
    artfFree(a);
    artfFree(b);
}

int main(void) {
    testPacking();
    testMeshlets();
    testInstances();
    testSamePixels();
    testNoGaps();
    testClipping();
    testCulling();
    testOverlay();
    testResize();
    testSimplify();
    testLod();
    testOcclusion(ARTF_BACKEND_SOFTWARE);
    testOcclusionUnderOverlay();
    testOcclusion(ARTF_BACKEND_VULKAN);
    testVulkan();
    testColorData();
    testLayers(ARTF_BACKEND_SOFTWARE);
    testLayers(ARTF_BACKEND_VULKAN);
    testMaterials(ARTF_BACKEND_SOFTWARE, false);
    testMaterials(ARTF_BACKEND_SOFTWARE, true);
    testMaterials(ARTF_BACKEND_VULKAN, false);
    testMaterials(ARTF_BACKEND_VULKAN, true);
    testSamePixelsOfLayers();
    testAntialias(ARTF_BACKEND_SOFTWARE);
    testAntialias(ARTF_BACKEND_VULKAN);
    testLayersBothBackends();
    testPictures();
    testTraceData();
    testTrace(ARTF_BACKEND_SOFTWARE);
    testTrace(ARTF_BACKEND_VULKAN);
    testTraceBothBackends();
    testHandles();
    testLook(ARTF_BACKEND_SOFTWARE);
    testLook(ARTF_BACKEND_VULKAN);
    testLifetimes();
    testPhysical(ARTF_BACKEND_SOFTWARE);
    testPhysical(ARTF_BACKEND_VULKAN);
    testShadowBytes();
    testShadows(ARTF_BACKEND_SOFTWARE);
    testShadows(ARTF_BACKEND_VULKAN);
    testShadowsBothBackends();
    testGltf();
    testClips();
    testSkin(ARTF_BACKEND_SOFTWARE);
    testSkin(ARTF_BACKEND_VULKAN);
    testSkinBothBackends();
    testMorph(ARTF_BACKEND_SOFTWARE);
    testMorph(ARTF_BACKEND_VULKAN);
    testDenoiseBytes();
    testDenoise(ARTF_BACKEND_SOFTWARE);
    testDenoise(ARTF_BACKEND_VULKAN);
    testDenoiseChange(ARTF_BACKEND_SOFTWARE);
    testDenoiseChange(ARTF_BACKEND_VULKAN);
    testDenoiseMotion(ARTF_BACKEND_SOFTWARE);
    testDenoiseMotion(ARTF_BACKEND_VULKAN);
    testDenoiseBones(ARTF_BACKEND_SOFTWARE);
    testDenoiseBones(ARTF_BACKEND_VULKAN);
    testOcclusionAuto(ARTF_BACKEND_SOFTWARE);
    testOcclusionAuto(ARTF_BACKEND_VULKAN);
    testShadowCache(ARTF_BACKEND_SOFTWARE);
    testShadowCache(ARTF_BACKEND_VULKAN);
    testShadowRegions(ARTF_BACKEND_SOFTWARE);
    testShadowRegions(ARTF_BACKEND_VULKAN);
    testLodFade(ARTF_BACKEND_SOFTWARE);
    testLodFade(ARTF_BACKEND_VULKAN);
    testGpuMeshlets();
    testLayersBehindSolid();
    testLayersSorted(ARTF_BACKEND_SOFTWARE);
    testLayersSorted(ARTF_BACKEND_VULKAN);
    testBlockDecoders();
    testStreamedTexture(ARTF_BACKEND_SOFTWARE);
    testStreamedTexture(ARTF_BACKEND_VULKAN);
    testResidencyGraph(ARTF_BACKEND_SOFTWARE);
    testResidencyGraph(ARTF_BACKEND_VULKAN);
    testMeshPlaces(ARTF_BACKEND_SOFTWARE);
    testMeshPlaces(ARTF_BACKEND_VULKAN);
    testSlerp();
    testGltfSparse();
    testGltfTargets();
    testGltfExtensions();
    testEnvironment();
    testFloats();
    testExtras();
    testLostDevice();
    testRefusedSwapchain();
    testBlocks();
    testPrepared();
    testRemoving(ARTF_BACKEND_SOFTWARE);
    testRemoving(ARTF_BACKEND_VULKAN);
    testResidency(ARTF_BACKEND_SOFTWARE);
    testResidency(ARTF_BACKEND_VULKAN);
    testGraphs(ARTF_BACKEND_SOFTWARE);
    testGraphs(ARTF_BACKEND_VULKAN);
    testGraphsBothBackends();
    testFluid();
    testBlackHole();
#ifdef ARTFUL_COUNT_ALLOCATIONS
    printf("allocations still alive: %ld\n", artfAllocationsAlive());
    CHECK(artfAllocationsAlive() == 0);
#endif
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
