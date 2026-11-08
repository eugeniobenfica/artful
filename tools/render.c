/* artful-render — draws a test scene and writes it to a PNG, or measures it.
 *
 *   artful-render [--backend vulkan|software] [--simd portable|128|256] [--threads n] [--size w h]
 *                 [--scene spheres|field|heavy|city|layers|blackhole] [--frames n] [--out file.png]
 *                 [--antialias]: four samples a pixel  [--clear]: no background, and a PNG with alpha
 *                 [--shadows side]: the sun casts shadows, in maps of that side
 *                 [--model file.glb]: a glTF file, framed by the camera, on a plain floor if there is room
 *                 [--trace rays|paths] [--trace-samples n]: light followed along rays, for n samples a
 *                 pixel, gathered over frames (1 to start with); [--bounces n]; [--denoise]
 *
 * With --frames it prints what a frame took. ARTFUL_FEATURES sets the scene's features.
 *
 * blackhole also takes: [--time t] [--samples n] [--doppler 0..1] [--distance masses] [--spin a]
 *                       [--tilt degrees] [--roll degrees] [--fov degrees] [--exposure x]
 *                       [--bloom x] [--around degrees] [--shutter t] [--orbit]: the eye moves, at an orbit's speed
 *                       [--sequence n --step t --out frames/%04d.png] [--to-distance d] [--to-tilt t]
 *                       [--to-around a]: n pictures, with the gas flowing from one to the next, and
 *                       the eye going smoothly to those places
 *
 * With --frames a black hole's time goes on, so that what a frame took includes the fluid's step. */
#include "artful/artful.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a PNG with no compression: stored deflate blocks ---- */

static uint32_t crcTable[256];
static uint32_t crc32Of(const uint8_t* p, size_t n, uint32_t c) {
    for (size_t i = 0; i < n; ++i) c = crcTable[(c ^ p[i]) & 255] ^ c >> 8;
    return c;
}
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE* f, const char* type, const uint8_t* data, uint32_t size) {
    uint8_t head[8];
    put32(head, size);
    memcpy(head + 4, type, 4);
    fwrite(head, 1, 8, f);
    if (size) fwrite(data, 1, size, f);
    uint32_t c = crc32Of(head + 4, 4, 0xFFFFFFFFu);
    c = crc32Of(data, size, c) ^ 0xFFFFFFFFu;
    put32(head, c);
    fwrite(head, 1, 4, f);
}
static bool pngAlpha; /* with its alpha, straight, as PNG wants it; the frame's is premultiplied */

static bool savePng(const char* path, const uint32_t* pixels, int w, int h) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ c >> 1 : c >> 1;
        crcTable[i] = c;
    }
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {0};
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;
    ihdr[9] = pngAlpha ? 6 : 2;
    chunk(f, "IHDR", ihdr, 13);
    const size_t row = (size_t)w * (pngAlpha ? 4 : 3) + 1, raw = row * h;
    uint8_t* data = malloc(raw);
    for (int y = 0; y < h; ++y) {
        uint8_t* o = data + row * y;
        *o++ = 0;
        for (int x = 0; x < w; ++x) {
            uint32_t p = pixels[(size_t)y * w + x];
            if (pngAlpha) {
                const uint32_t a = p >> 24, half = a / 2;
                for (int shift = 16; shift >= 0; shift -= 8) {
                    const uint32_t c = a ? ((p >> shift & 255) * 255 + half) / a : 0;
                    *o++ = (uint8_t)(c > 255 ? 255 : c);
                }
                *o++ = (uint8_t)a;
                continue;
            }
            *o++ = (uint8_t)(p >> 16);
            *o++ = (uint8_t)(p >> 8);
            *o++ = (uint8_t)p;
        }
    }
    const size_t blocks = (raw + 65534) / 65535;
    uint8_t* z = malloc(raw + blocks * 5 + 6);
    size_t n = 0;
    z[n++] = 0x78;
    z[n++] = 1;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; ++i) { a = (a + data[i]) % 65521; b = (b + a) % 65521; }
    for (size_t at = 0; at < raw; at += 65535) {
        size_t len = raw - at < 65535 ? raw - at : 65535;
        z[n++] = at + len == raw;
        z[n++] = (uint8_t)len;
        z[n++] = (uint8_t)(len >> 8);
        z[n++] = (uint8_t)~len;
        z[n++] = (uint8_t)(~len >> 8);
        memcpy(z + n, data + at, len);
        n += len;
    }
    put32(z + n, b << 16 | a);
    n += 4;
    chunk(f, "IDAT", z, (uint32_t)n);
    chunk(f, "IEND", (const uint8_t*)"", 0);
    fclose(f);
    free(data);
    free(z);
    return true;
}

/* ---- the scenes ---- */

static ArtfTexture checker(ArtfScene* s) {
    static uint32_t px[256 * 256];
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) px[y * 256 + x] = ((x / 32 + y / 32) & 1) ? 0xFFE8E4DCu : 0xFF3A4252u;
    return artfSceneAddTexture(s, px, 256, 256, 0);
}

static void camera(ArtfScene* s, float x, float y, float z) {
    ArtfCamera c = {artfVec3(x, y, z), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.0f, .1f};
    artfSceneSetCamera(s, &c);
}

/* A panel of colors, fading shapes in front of it and a floor: all that vertices and opacity can do. */
static void sceneLayers(ArtfScene* s) {
    camera(s, 0, 1.6f, 7.5f);
    ArtfMaterialDesc floorDesc = {artfVec3(.8f, .8f, .8f), checker(s), .1f, .33f, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, -1.4f, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 8), artfSceneAddMaterial(s, &floorDesc), &t);
    ArtfMaterialDesc plainDesc = {artfVec3(1, 1, 1), 0, .4f, .2f, artfVec3(0, 0, 0)};
    const ArtfMaterial plain = artfSceneAddMaterial(s, &plainDesc);

    enum { SIDE = 24 };
    static ArtfVertex v[(SIDE + 1) * (SIDE + 1)];
    static uint32_t idx[SIDE * SIDE * 6];
    uint32_t n = 0;
    for (int y = 0; y <= SIDE; ++y)
        for (int x = 0; x <= SIDE; ++x) {
            const float u = (float)x / SIDE, w = (float)y / SIDE;
            const uint32_t r = (uint32_t)(255.f * u), g = (uint32_t)(255.f * w), b = (uint32_t)(255.f * (1.f - u));
            const ArtfVertex q = {u * 6.f - 3.f, w * 4.f - 1.4f, 0, 0, 0, 1, u, w, 0xFF000000u | r << 16 | g << 8 | b};
            v[y * (SIDE + 1) + x] = q;
        }
    for (uint32_t y = 0; y < SIDE; ++y)
        for (uint32_t x = 0; x < SIDE; ++x) {
            const uint32_t a = y * (SIDE + 1) + x, b = a + 1, d = a + SIDE + 1, e = d + 1;
            idx[n++] = a; idx[n++] = b; idx[n++] = e;
            idx[n++] = a; idx[n++] = e; idx[n++] = d;
        }
    t = artfTransformAt(0, 0, -1.5f, 1);
    artfSceneAddInstance(s, artfSceneAddMesh(s, v, (SIDE + 1) * (SIDE + 1), idx, n), plain, &t);

    ArtfMaterialDesc blueDesc = {artfVec3(.2f, .45f, 1.f), 0, .8f, .14f, artfVec3(0, 0, 0)}, redDesc = {artfVec3(1.f, .25f, .15f), 0, .6f, .17f, artfVec3(0, 0, 0)};
    t = artfTransformAt(-1.5f, 0, .5f, 1);
    artfInstanceSetOpacity(s, artfSceneAddInstance(s, artfSceneAddSphere(s, 1.1f, 48, 64), artfSceneAddMaterial(s, &blueDesc), &t), .45f);
    t = artfTransformTrs(artfVec3(1.5f, .1f, .5f), artfQuatAxisAngle(artfVec3(1, .6f, .2f), .8f), artfVec3(1, 1, 1));
    artfInstanceSetOpacity(s, artfSceneAddInstance(s, artfSceneAddTorus(s, 1.f, .35f, 64, 32), artfSceneAddMaterial(s, &redDesc), &t), .65f);
}

static float frandom(uint32_t* st) {
    *st = *st * 1664525u + 1013904223u;
    return (float)(*st >> 8) / 16777216.f;
}

/* a few lit objects: what shading looks like */
static void sceneSpheres(ArtfScene* s) {
    ArtfMaterialDesc floorDesc = {artfVec3(.9f, .9f, .9f), checker(s), .1f, .33f, artfVec3(0, 0, 0)};
    ArtfMesh plane = artfSceneAddPlane(s, 40.f, 10.f);
    ArtfTransform t = artfTransformAt(0, -1.2f, 0, 1);
    artfSceneAddInstance(s, plane, artfSceneAddMaterial(s, &floorDesc), &t);
    ArtfMesh sphere = artfSceneAddSphere(s, 1.f, 48, 64), torus = artfSceneAddTorus(s, 1.f, .35f, 64, 32), cube = artfSceneAddCube(s, 1.6f);
    ArtfMaterialDesc red = {artfVec3(.8f, .12f, .1f), 0, .8f, .14f, artfVec3(0, 0, 0)};
    ArtfMaterialDesc gold = {artfVec3(.85f, .6f, .15f), 0, .6f, .24f, artfVec3(0, 0, 0)};
    ArtfMaterialDesc box = {artfVec3(1, 1, 1), checker(s), .3f, .24f, artfVec3(0, 0, 0)};
    t = artfTransformAt(-2.6f, 0, 0, 1);
    artfSceneAddInstance(s, sphere, artfSceneAddMaterial(s, &red), &t);
    t = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(1, .2f, 0), 1.1f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, torus, artfSceneAddMaterial(s, &gold), &t);
    t = artfTransformTrs(artfVec3(2.6f, -.2f, 0), artfQuatAxisAngle(artfVec3(0, 1, 0), .6f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, cube, artfSceneAddMaterial(s, &box), &t);
    artfSceneAddLight(s, artfVec3(-1.5f, 1.5f, 2.5f), artfVec3(.2f, .5f, 1.2f), 7.f);
    camera(s, 0, 2.4f, 6.5f);
}

/* many small things: what a frame's fixed costs look like */
static void sceneField(ArtfScene* s, int side) {
    ArtfMesh cube = artfSceneAddCube(s, .7f), ball = artfSceneAddSphere(s, .45f, 8, 12);
    ArtfMaterial materials[6];
    uint32_t st = 7;
    for (int i = 0; i < 6; ++i) {
        ArtfMaterialDesc d = {artfVec3(frandom(&st), frandom(&st), frandom(&st)), 0, .4f, .24f, artfVec3(0, 0, 0)};
        materials[i] = artfSceneAddMaterial(s, &d);
    }
    for (int z = 0; z < side; ++z)
        for (int x = 0; x < side; ++x) {
            ArtfTransform t = artfTransformTrs(artfVec3((float)x - side * .5f, frandom(&st) * 2.f, (float)z - side * .5f), artfQuatAxisAngle(artfVec3(0, 1, .3f), frandom(&st) * 6.f), artfVec3(1, 1, 1));
            artfSceneAddInstance(s, (x + z) & 1 ? cube : ball, materials[(x * 7 + z) % 6], &t);
        }
    camera(s, 0, side * .3f + 3.f, side * .55f + 3.f);
}

/* a million triangles and more */
/* walls in front of a crowd: what occlusion is for */
static void sceneCity(ArtfScene* s) {
    ArtfMesh wall = artfSceneAddCube(s, 1.f), ball = artfSceneAddSphere(s, .45f, 24, 32);
    ArtfMaterialDesc grey = {artfVec3(.5f, .52f, .56f), 0, .1f, .33f, artfVec3(0, 0, 0)};
    ArtfMaterial stone = artfSceneAddMaterial(s, &grey);
    uint32_t st = 11;
    ArtfMaterialDesc red = {artfVec3(.8f, .3f, .2f), 0, .4f, .24f, artfVec3(0, 0, 0)};
    ArtfMaterial paint = artfSceneAddMaterial(s, &red);
    ArtfTransform t = artfTransformTrs(artfVec3(0, 0, 4), artfQuatIdentity(), artfVec3(120, 40, 1));
    artfSceneAddInstance(s, wall, stone, &t);
    for (int i = 0; i < 40000; ++i) {
        t = artfTransformAt((frandom(&st) - .5f) * 110.f, (frandom(&st) - .5f) * 30.f, -frandom(&st) * 100.f, 1.f);
        artfSceneAddInstance(s, ball, paint, &t);
    }
    for (int i = 0; i < 200; ++i) {
        t = artfTransformAt((frandom(&st) - .5f) * 16.f, (frandom(&st) - .5f) * 10.f, 8.f + frandom(&st) * 4.f, 1.f);
        artfSceneAddInstance(s, ball, paint, &t);
    }
    ArtfCamera c = {artfVec3(0, 0, 16), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.0f, .1f};
    artfSceneSetCamera(s, &c);
}

static void sceneHeavy(ArtfScene* s) {
    ArtfMesh ball = artfSceneAddSphere(s, 1.f, 192, 256);
    ArtfMaterialDesc d = {artfVec3(.5f, .6f, .75f), 0, .5f, .2f, artfVec3(0, 0, 0)};
    ArtfMaterial m = artfSceneAddMaterial(s, &d);
    for (int i = 0; i < 16; ++i) {
        ArtfTransform t = artfTransformAt((float)(i % 4) * 2.4f - 3.6f, (float)(i / 4) * 2.4f - 3.6f, 0, 1);
        artfSceneAddInstance(s, ball, m, &t);
    }
    camera(s, 0, 0, 9.f);
}

/* a black hole, and nothing else: all the light there is in this picture is traced */
typedef struct HoleOptions { float time, doppler, distance, tilt, roll, fov, exposure, spin, around, shutter, bloom; int samples; bool orbit; int sequence; float step, toDistance, toTilt, toAround; } HoleOptions;

static void sceneBlackHole(ArtfScene* s, const HoleOptions* h) {
    ArtfBlackHoleDesc d = artfBlackHoleDefaults();
    d.time = h->time;
    d.doppler = h->doppler;
    d.samples = h->samples;
    d.exposure = h->exposure;
    d.spin = h->spin;
    d.shutter = h->shutter;
    if (h->bloom >= 0) d.bloom = h->bloom;
    const float tilt = h->tilt * 3.14159265f / 180.f, roll = h->roll * 3.14159265f / 180.f, around = h->around * 3.14159265f / 180.f;
    const float flat = cosf(tilt) * h->distance;
    if (h->orbit) { /* the way the gas goes */
        const float speed = artfBlackHoleOrbitSpeed(h->spin, h->distance);
        d.velocity = artfVec3(cosf(around) * speed, 0, -sinf(around) * speed);
    }
    artfSceneSetBlackHole(s, &d);
    const ArtfVec3 eye = artfVec3(sinf(around) * flat, sinf(tilt) * h->distance, cosf(around) * flat);
    const ArtfVec3 side = artfVec3(cosf(around), 0, -sinf(around));
    const ArtfVec3 up = artfVec3Add(artfVec3Scale(side, sinf(roll)), artfVec3(0, cosf(roll), 0));
    /* (one who moves sees the hole displaced toward where they are going: aimed there, it stays in frame) */
    const float beta = artfVec3Length(d.velocity);
    const ArtfVec3 aim = artfVec3Scale(d.velocity, h->distance / sqrtf(1.f - beta * beta));
    ArtfCamera c = {eye, aim, up, h->fov * 3.14159265f / 180.f, .1f};
    artfSceneSetCamera(s, &c);
}

static void printLog(void* user, const char* message) {
    (void)user;
    fprintf(stderr, "%s\n", message);
}

/* The eye goes back until all of what a file holds is in view. */
static void sceneModel(ArtfScene* s, const char* path) {
    const uint32_t count = artfSceneLoadGltf(s, path, NULL);
    printf("%u instances from %s\n", count, path);
    float radius = 1.f;
    ArtfVec3 middle = artfVec3(0, 0, 0);
    artfSceneBounds(s, &middle, &radius);
    ArtfCamera c = {artfVec3Add(middle, artfVec3(radius * 1.2f, radius * .9f, radius * 2.2f)), middle, artfVec3(0, 1, 0), .8f, radius * .01f};
    artfSceneSetCamera(s, &c);
}

int main(int argc, char** argv) {
    artfSetLog(printLog, NULL);
    const char* model = NULL;
    int shadowSide = 0;
    ArtfTraceDesc trace = {ARTF_TRACE_NONE, 1, 0, 0, false};
    int traceSamples = 1;
    ArtfRendererOptions o;
    memset(&o, 0, sizeof o);
    o.width = 1280;
    o.height = 720;
    o.noVsync = true;
    const char *out = NULL, *sceneName = "spheres";
    int frames = 0, fieldSide = 100;
    HoleOptions hole = {0, 0, 74.1f, 3.44f, 0, 19.f, 1.f, .6f, 0, 0, -1.f, 1, false, 0, .9f, -1.f, -1000.f, -1000.f}; /* (the eye of the film's own pictures) */
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--backend") && i + 1 < argc) { ++i; o.backend = !strcmp(argv[i], "vulkan") ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE; }
        else if (!strcmp(a, "--simd") && i + 1 < argc) { ++i; o.simd = !strcmp(argv[i], "portable") ? ARTF_SIMD_PORTABLE : !strcmp(argv[i], "128") ? ARTF_SIMD_128 : ARTF_SIMD_256; }
        else if (!strcmp(a, "--threads") && i + 1 < argc) o.threads = atoi(argv[++i]);
        else if (!strcmp(a, "--size") && i + 2 < argc) { o.width = atoi(argv[++i]); o.height = atoi(argv[++i]); }
        else if (!strcmp(a, "--scene") && i + 1 < argc) sceneName = argv[++i];
        else if (!strcmp(a, "--side") && i + 1 < argc) fieldSide = atoi(argv[++i]);
        else if (!strcmp(a, "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(a, "--antialias")) o.antialias = true;
        else if (!strcmp(a, "--denoise")) trace.denoise = true;
        else if (!strcmp(a, "--shadows") && i + 1 < argc) shadowSide = atoi(argv[++i]);
        else if (!strcmp(a, "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(a, "--trace") && i + 1 < argc) trace.mode = !strcmp(argv[++i], "paths") ? ARTF_TRACE_PATHS : ARTF_TRACE_RAYS;
        else if (!strcmp(a, "--trace-samples") && i + 1 < argc) traceSamples = atoi(argv[++i]);
        else if (!strcmp(a, "--bounces") && i + 1 < argc) trace.bounces = atoi(argv[++i]);
        else if (!strcmp(a, "--clear")) pngAlpha = true;
        else if (!strcmp(a, "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(a, "--time") && i + 1 < argc) hole.time = (float)atof(argv[++i]);
        else if (!strcmp(a, "--samples") && i + 1 < argc) hole.samples = atoi(argv[++i]);
        else if (!strcmp(a, "--doppler") && i + 1 < argc) hole.doppler = (float)atof(argv[++i]);
        else if (!strcmp(a, "--distance") && i + 1 < argc) hole.distance = (float)atof(argv[++i]);
        else if (!strcmp(a, "--tilt") && i + 1 < argc) hole.tilt = (float)atof(argv[++i]);
        else if (!strcmp(a, "--roll") && i + 1 < argc) hole.roll = (float)atof(argv[++i]);
        else if (!strcmp(a, "--fov") && i + 1 < argc) hole.fov = (float)atof(argv[++i]);
        else if (!strcmp(a, "--exposure") && i + 1 < argc) hole.exposure = (float)atof(argv[++i]);
        else if (!strcmp(a, "--spin") && i + 1 < argc) hole.spin = (float)atof(argv[++i]);
        else if (!strcmp(a, "--around") && i + 1 < argc) hole.around = (float)atof(argv[++i]);
        else if (!strcmp(a, "--shutter") && i + 1 < argc) hole.shutter = (float)atof(argv[++i]);
        else if (!strcmp(a, "--orbit")) hole.orbit = true;
        else if (!strcmp(a, "--bloom") && i + 1 < argc) hole.bloom = (float)atof(argv[++i]);
        else if (!strcmp(a, "--sequence") && i + 1 < argc) hole.sequence = atoi(argv[++i]);
        else if (!strcmp(a, "--step") && i + 1 < argc) hole.step = (float)atof(argv[++i]);
        else if (!strcmp(a, "--to-distance") && i + 1 < argc) hole.toDistance = (float)atof(argv[++i]);
        else if (!strcmp(a, "--to-tilt") && i + 1 < argc) hole.toTilt = (float)atof(argv[++i]);
        else if (!strcmp(a, "--to-around") && i + 1 < argc) hole.toAround = (float)atof(argv[++i]);
        else { fprintf(stderr, "unknown: %s\n", a); return 2; }
    }

    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) { fputs("that backend cannot run here\n", stderr); return 1; }
    printf("%s, %d x %d\n", artfRendererDescription(r), o.width, o.height);

    const double t0 = artfNowMs();
    const bool isHole = !strcmp(sceneName, "blackhole");
    ArtfScene* s = artfSceneNew();
    if (model && strcmp(sceneName, "spheres")) sceneName = "spheres";
    if (model) sceneModel(s, model);
    else if (!strcmp(sceneName, "field")) sceneField(s, fieldSide);
    else if (!strcmp(sceneName, "heavy")) sceneHeavy(s);
    else if (!strcmp(sceneName, "city")) sceneCity(s);
    else if (!strcmp(sceneName, "layers")) sceneLayers(s);
    else if (isHole) sceneBlackHole(s, &hole);
    else sceneSpheres(s);
    if (pngAlpha) artfSceneSetBackgroundAlpha(s, 0);
    if (shadowSide) {
        const ArtfShadowDesc shadows = {shadowSide, 0};
        artfSceneSetShadows(s, &shadows);
    }
    if (trace.mode) {
        trace.samples = 1;
        trace.maxSamples = traceSamples;
        artfSceneSetTrace(s, &trace);
    }
    printf("scene built in %.1f ms, %u instances\n", artfNowMs() - t0, artfSceneInstanceCount(s));

    if (hole.sequence > 0 && out) {
        uint32_t* pixels = malloc((size_t)o.width * o.height * 4);
        const HoleOptions from = hole;
        const float toDistance = from.toDistance > 0 ? from.toDistance : from.distance;
        const float toTilt = from.toTilt > -999.f ? from.toTilt : from.tilt, toAround = from.toAround > -999.f ? from.toAround : from.around;
        for (int i = 0; i < hole.sequence; ++i) {
            const float x = hole.sequence > 1 ? (float)i / (float)(hole.sequence - 1) : 0.f, e = x * x * (3.f - 2.f * x);
            hole.distance = from.distance + (toDistance - from.distance) * e;
            hole.tilt = from.tilt + (toTilt - from.tilt) * e;
            hole.around = from.around + (toAround - from.around) * e;
            hole.time = from.time + from.step * (float)i;
            sceneBlackHole(s, &hole);
            char path[1024];
            snprintf(path, sizeof path, out, i);
            if (!artfRendererDraw(r, s) || !artfRendererReadPixels(r, pixels) || !savePng(path, pixels, o.width, o.height)) { fputs("could not write the picture\n", stderr); return 1; }
        }
        free(pixels);
        artfRendererFree(r);
        artfSceneFree(s);
        return 0;
    }
    if (!artfRendererDraw(r, s)) { fputs("the device was lost\n", stderr); return 1; }
    if (trace.mode && !frames) {
        const double start = artfNowMs();
        for (int i = 1; i < traceSamples; ++i)
            if (!artfRendererDraw(r, s)) { fputs("the device was lost\n", stderr); return 1; }
        printf("%d samples a pixel: %.1f ms each\n", traceSamples, (artfNowMs() - start) / artfMaxi(traceSamples - 1, 1));
    }
    if (frames > 0) {
        double cull = 0, geometry = 0, raster = 0, lens = 0, best = 1e9;
        ArtfFrameStats st;
        uint32_t* sink = malloc((size_t)o.width * o.height * 4);
        const bool gpu = artfRendererBackend(r) == ARTF_BACKEND_VULKAN;
        /* A GPU's frame ends when its pixels can be read, not when the call returns: the frames are
         * timed as a run, to the last pixel, and reading itself is taken out. */
        if (gpu) artfRendererReadPixels(r, sink);
        const double start = artfNowMs();
        for (int i = 0; i < frames; ++i) {
            if (isHole) {
                hole.time += .2f;
                sceneBlackHole(s, &hole);
            }
            const double a = artfNowMs();
            artfRendererDraw(r, s);
            const double b = artfNowMs() - a;
            best = b < best ? b : best;
            artfRendererStats(r, &st);
            cull += st.cullMs;
            geometry += st.geometryMs;
            raster += st.rasterMs;
            lens += st.lensMs;
        }
        double total = 0;
        if (gpu) {
            artfRendererReadPixels(r, sink);
            total = artfNowMs() - start;
            const double a = artfNowMs();
            artfRendererReadPixels(r, sink);
            total -= artfNowMs() - a;
        } else {
            total = artfNowMs() - start;
        }
        free(sink);
        printf("%d frames: %.3f ms a frame (%.0f fps)", frames, total / frames, 1000.0 * frames / total);
        if (gpu) printf(", %.3f ms of it on the CPU at best, %.3f ms on the GPU the last frame\n", best, st.gpuMs);
        else printf(", best %.3f ms\n  cull %.3f, geometry %.3f, lens %.3f, tiles %.3f ms; %u instances drawn, %u hidden, %u triangles binned\n", best, cull / frames, geometry / frames, lens / frames, raster / frames, st.instancesDrawn, st.instancesHidden, st.trianglesBinned);
    }
    if (out) {
        uint32_t* pixels = malloc((size_t)o.width * o.height * 4);
        if (!artfRendererReadPixels(r, pixels) || !savePng(out, pixels, o.width, o.height)) { fputs("could not write the picture\n", stderr); return 1; }
        free(pixels);
        printf("wrote %s\n", out);
    }
    artfRendererFree(r);
    artfSceneFree(s);
    return 0;
}
