/* artful-render — draws a test scene and writes it to a PNG, or measures it.
 *
 *   artful-render [--backend vulkan|software] [--simd portable|128|256] [--threads n] [--size w h]
 *                 [--scene spheres|field|heavy|city|layers|lights|outdoors|blackhole] [--frames n] [--out file.png]
 *                 [--antialias]: four samples a pixel  [--clear]: no background, and a PNG with alpha
 *                 [--ortho height]: the camera is orthographic, its view this tall in the world
 *                 [--shadows side]: the sun casts shadows, in maps of that side; [--soft-shadows]: as wide as the
 *                 sun is, [--sun-size radians]; [--traced-shadows]: shadows from rays, through the trees or the GPU's ray
 *                 tracing hardware, for the sun and the sixteen lights (with --shadows, the rest keep maps)
 *                 [--model file.glb]: a glTF file, framed by the camera, on a plain floor if there is room
 *                 [--trace rays|paths] [--trace-samples n]: light followed along rays, for n samples a
 *                 pixel, gathered over frames (1 to start with); [--bounces n]; [--denoise]; [--hardware]: through the
 *                 GPU's ray tracing hardware
 *                 [--environment file.hdr]: light from all around, from a picture of it; [--environment-light x];
 *                 [--show-environment]: it is the background too; [--scene-exposure x]
 *                 [--hdr]: the frame in floats, and then: [--glow x] [--glow-threshold t] [--occlusion x]
 *                 [--occlusion-radius r] [--reflections x] [--traced-reflections]: from rays, not marched across the frame
 *                 [--volumetric x] [--scattering g] [--temporal]
 *                 [--focus distance --aperture pixels] [--fog density r g b]
 *                 [--tone aces|neutral|reinhard|none] [--auto-exposure] [--temperature x] [--tint x]
 *                 [--saturation x] [--contrast x] [--vignette x] [--chromatic pixels] [--motion-blur shutter]
 *                 [--sun x y z]: where the sun shines, as a direction; [--sky [haze]]: a sky from the sun, and
 *                 [--sky-multiple x]: light scattered more than once; [--sun-disc degrees]; [--sky-width n];
 *                 [--clouds coverage] [--cloud-base m] [--cloud-thickness m] [--cloud-density x] [--wind x y z]
 *                 [--cloud-shadow]
 *                 [--exposure-low share] [--exposure-high share]: auto exposure by a histogram;
 *                 [--exposure-speed x] [--exposure-speed-down x]
 *                 [--lift r g b] [--gamma r g b] [--gain r g b] [--grade-shadows r g b] [--grade-midtones r g b]
 *                 [--grade-highlights r g b] [--lut file.cube] [--lut-amount x] [--grain x] [--flare x]
 *                 [--fog-height base falloff]: fog that light goes through thins with height
 *                 [--indirect]: light that bounces, from probes, every frame; [--through-walls]: lighter, and it leaks
 *                 [--view lit|base-color|normals|roughness|metallic|ambient|emissive|lod|meshlets|instances|overdraw|depth|
 *                 motion|lights]: what the frame shows (ArtfView); [--passes]: with --frames, what each pass of the
 *                 last frame took
 *                 [--scene forest] [--hlod [cell]]: far trees merged by cubes of that side (Unreal's HLOD)
 *                 [--scene crowd]: figures that bend, each frame, with levels of detail as the lod feature says
 *                 [--particles n]: an emitter of many, n of them at once, stepped each frame
 *                 [--stream-meshes [MB]]: meshes' levels held by distance, within that budget
 *                 [--render-scale s]: with --hdr, the scene drawn at s of the size and upscaled (with --temporal, as
 *                 TSR does); [--sharpen x]: after temporal antialiasing or upscaling
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
#include "artful/blackhole.h"

#include <math.h>
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
static float orthoHeight; /* --ortho: the scene's camera is orthographic, this tall in the world */

/* The scene's camera, made orthographic when asked. */
static void setCamera(ArtfScene* s, ArtfCamera* c) {
    c->orthoHeight = orthoHeight;
    artfSceneSetCamera(s, c);
}

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
    setCamera(s, &c);
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

/* Hills from value noise, a fire and its smoke: terrain and particles, which need nothing of a backend. */
static float noiseAt(float x, float z) {
    const int ix = (int)floorf(x), iz = (int)floorf(z);
    const float fx = x - (float)ix, fz = z - (float)iz;
    float corner[4];
    for (int k = 0; k < 4; ++k) {
        uint32_t h = (uint32_t)(ix + (k & 1)) * 73856093u ^ (uint32_t)(iz + (k >> 1)) * 19349663u;
        h ^= h >> 13;
        h *= 0x5BD1E995u;
        h ^= h >> 15;
        corner[k] = (float)(h & 0xFFFF) / 65535.f;
    }
    const float sx = fx * fx * (3.f - 2.f * fx), sz = fz * fz * (3.f - 2.f * fz);
    const float top = corner[0] + (corner[1] - corner[0]) * sx, bottom = corner[2] + (corner[3] - corner[2]) * sx;
    return top + (bottom - top) * sz;
}

static ArtfEmitter fire, smoke;

static void sceneOutdoors(ArtfScene* s) {
    enum { SIDE = 129 };
    float* heights = malloc(SIDE * SIDE * 4);
    for (int z = 0; z < SIDE; ++z)
        for (int x = 0; x < SIDE; ++x) {
            const float fx = (float)x / 16.f, fz = (float)z / 16.f;
            const float edge = fabsf((float)x / (SIDE - 1) - .5f) + fabsf((float)z / (SIDE - 1) - .5f);
            heights[z * SIDE + x] = noiseAt(fx, fz) * .7f + noiseAt(fx * 2.f, fz * 2.f) * .3f + edge * edge * 3.f;
        }
    ArtfMaterialDesc grass = {artfVec3(.25f, .4f, .15f), 0, .2f, .8f, artfVec3(0, 0, 0)};
    const ArtfTransform ground = artfTransformAt(0, -2.f, 0, 1);
    artfSceneAddInstance(s, artfSceneAddTerrain(s, heights, SIDE, SIDE, .5f, 3.f, 8.f), artfSceneAddMaterial(s, &grass), &ground);
    free(heights);
    ArtfMaterialDesc stone = {artfVec3(.5f, .48f, .45f), 0, .4f, .5f, artfVec3(0, 0, 0)};
    const ArtfTransform rock = artfTransformAt(3.f, -.2f, -2.f, 1.4f);
    artfSceneAddInstance(s, artfSceneAddSphere(s, 1.f, 24, 32), artfSceneAddMaterial(s, &stone), &rock);
    ArtfEmitterDesc flames;
    memset(&flames, 0, sizeof flames);
    flames.position = artfVec3(0, -.2f, 0);
    flames.area = .3f;
    flames.velocity = artfVec3(0, 1.6f, 0);
    flames.spread = .25f;
    flames.rate = 120;
    flames.life = .9f;
    flames.size = .45f;
    flames.sizeEnd = .1f;
    flames.color = artfVec3(1, .55f, .15f);
    flames.colorEnd = artfVec3(.8f, .15f, .05f);
    flames.opacity = .9f;
    flames.glow = 6.f;
    flames.most = 200;
    fire = artfSceneAddEmitter(s, &flames);
    ArtfEmitterDesc fumes = flames;
    fumes.position = artfVec3(0, .6f, 0);
    fumes.velocity = artfVec3(.3f, .9f, 0);
    fumes.rate = 30;
    fumes.life = 4.f;
    fumes.size = .5f;
    fumes.sizeEnd = 2.f;
    fumes.color = artfVec3(.3f, .3f, .3f);
    fumes.colorEnd = artfVec3(.5f, .5f, .52f);
    fumes.opacity = .35f;
    fumes.opacityEnd = 0;
    fumes.glow = 0;
    fumes.most = 160;
    smoke = artfSceneAddEmitter(s, &fumes);
    camera(s, 0, 2.f, 9.f); /* (before the steps: particles turn to the camera as it is when they move) */
    for (int i = 0; i < 120; ++i) {
        artfEmitterStep(s, fire, 1.f / 30.f);
        artfEmitterStep(s, smoke, 1.f / 30.f);
    }
    artfSceneAddLight(s, artfVec3(0, .4f, 0), artfVec3(3.f, 1.4f, .4f), 9.f);
    artfSceneSetSun(s, artfVec3(-.4f, -.35f, .6f), artfVec3(1.4f, 1.2f, 1.f));
    artfSceneSetAmbient(s, artfVec3(.25f, .3f, .4f), artfVec3(.1f, .1f, .08f));
    artfSceneSetBackground(s, artfVec3(.55f, .65f, .8f));
}

/* The spheres at night, under 256 small colored lights: what lights past the frame's own cost. */
static void sceneLights(ArtfScene* s) {
    sceneSpheres(s);
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(.02f, .02f, .03f), artfVec3(.01f, .01f, .01f));
    uint32_t st = 11;
    for (int i = 0; i < 256; ++i) {
        const ArtfVec3 at = artfVec3((frandom(&st) - .5f) * 30.f, -1.f + frandom(&st) * .6f, (frandom(&st) - .5f) * 30.f - 4.f);
        const ArtfVec3 color = artfVec3(frandom(&st) * 2.f, frandom(&st) * 2.f, frandom(&st) * 2.f);
        artfSceneAddLight(s, at, color, 1.5f + frandom(&st) * 1.5f);
    }
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
    setCamera(s, &c);
}

/* trees as far as the eye goes: a trunk and a crown each, 40,000 of them over 800 m */
static void sceneForest(ArtfScene* s) {
    ArtfMesh crown = artfSceneAddSphere(s, 1.6f, 16, 24), trunk = artfSceneAddCube(s, 1.f), ground = artfSceneAddPlane(s, 1000.f, 200.f);
    ArtfMaterialDesc green = {artfVec3(.18f, .42f, .16f), 0, .1f, .6f, artfVec3(0, 0, 0)};
    ArtfMaterialDesc bark = {artfVec3(.35f, .25f, .16f), 0, .1f, .7f, artfVec3(0, 0, 0)};
    ArtfMaterialDesc soil = {artfVec3(.3f, .32f, .22f), 0, .1f, .8f, artfVec3(0, 0, 0)};
    ArtfMaterial leaves = artfSceneAddMaterial(s, &green), wood = artfSceneAddMaterial(s, &bark);
    artfSceneAddInstance(s, ground, artfSceneAddMaterial(s, &soil), NULL);
    uint32_t st = 5;
    for (int i = 0; i < 40000; ++i) {
        const float x = (frandom(&st) - .5f) * 800.f, z = -frandom(&st) * 800.f + 20.f, h = 3.f + frandom(&st) * 3.f;
        ArtfTransform t = artfTransformTrs(artfVec3(x, h * .5f, z), artfQuatIdentity(), artfVec3(.4f, h, .4f));
        artfSceneAddInstance(s, trunk, wood, &t);
        t = artfTransformAt(x, h + 1.f, z, .8f + frandom(&st) * .5f);
        artfSceneAddInstance(s, crown, leaves, &t);
    }
    ArtfCamera c = {artfVec3(0, 12, 40), artfVec3(0, 0, -100), artfVec3(0, 1, 0), 1.0f, .1f};
    artfSceneSetCamera(s, &c);
}

/* figures that bend: a tube of 960 triangles, its upper half on a second bone, 2,500 of them */
static ArtfMesh crowdMesh(ArtfScene* s) {
    enum { RINGS = 40, AROUND = 12 };
    static ArtfVertex v[(RINGS + 1) * (AROUND + 1)];
    static ArtfVertexSkin k[(RINGS + 1) * (AROUND + 1)];
    static uint32_t idx[RINGS * AROUND * 6];
    for (int r = 0; r <= RINGS; ++r)
        for (int a = 0; a <= AROUND; ++a) {
            const float y = (float)r / RINGS * 2.f, turn = (float)a / AROUND * 6.2831853f;
            const ArtfVertex q = {cosf(turn) * .25f, y, sinf(turn) * .25f, cosf(turn), 0, sinf(turn), (float)a / AROUND, (float)r / RINGS, 0};
            const float top = y < .8f ? 0.f : y > 1.2f ? 1.f : (y - .8f) / .4f;
            const ArtfVertexSkin w = {{0, 1, 0, 0}, {1.f - top, top, 0, 0}};
            v[r * (AROUND + 1) + a] = q;
            k[r * (AROUND + 1) + a] = w;
        }
    uint32_t n = 0;
    for (uint32_t r = 0; r < RINGS; ++r)
        for (uint32_t a = 0; a < AROUND; ++a) {
            const uint32_t p = r * (AROUND + 1) + a, q = p + AROUND + 1;
            idx[n++] = p; idx[n++] = q + 1; idx[n++] = p + 1;
            idx[n++] = p; idx[n++] = q; idx[n++] = q + 1;
        }
    return artfSceneAddSkinnedMesh(s, v, (RINGS + 1) * (AROUND + 1), idx, n, k);
}

static ArtfInstance crowd[2500];

static void crowdBend(ArtfScene* s, float time) {
    for (int i = 0; i < 2500; ++i) {
        const float angle = sinf(time * 2.f + (float)i * .37f) * .8f;
        const ArtfTransform turn = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 0, 1), angle), artfVec3(1, 1, 1));
        ArtfTransform joints[2] = {artfTransformIdentity(), turn};
        joints[1].m[3] = -turn.m[1];
        joints[1].m[7] = 1.f - turn.m[5];
        artfInstanceSetJoints(s, crowd[i], joints, 2);
    }
}

static void sceneCrowd(ArtfScene* s) {
    ArtfMesh figure = crowdMesh(s), ground = artfSceneAddPlane(s, 400.f, 40.f);
    ArtfMaterialDesc cloth = {artfVec3(.6f, .3f, .2f), 0, .3f, .5f, artfVec3(0, 0, 0)};
    ArtfMaterialDesc soil = {artfVec3(.4f, .4f, .38f), 0, .1f, .8f, artfVec3(0, 0, 0)};
    ArtfMaterial m = artfSceneAddMaterial(s, &cloth);
    artfSceneAddInstance(s, ground, artfSceneAddMaterial(s, &soil), NULL);
    for (int i = 0; i < 2500; ++i) {
        const ArtfTransform t = artfTransformAt((float)(i % 50) * 2.f - 49.f, 0, -(float)(i / 50) * 4.f, 1.f);
        crowd[i] = artfSceneAddInstance(s, figure, m, &t);
    }
    crowdBend(s, 0);
    ArtfCamera c = {artfVec3(0, 6, 12), artfVec3(0, 0, -40), artfVec3(0, 1, 0), 1.0f, .1f};
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
    setCamera(s, &c);
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
    setCamera(s, &c);
}

int main(int argc, char** argv) {
    artfSetLog(printLog, NULL);
    const char* model = NULL;
    int shadowSide = 0;
    const char* environment = NULL;
    bool softShadows = false, tracedShadows = false;
    float sunSize = -1;
    float environmentLight = 1.f, sceneExposure = 0, skyHaze = -1, fogHeight[2] = {0, 0}, sunAt[3] = {0, 0, 0};
    bool showEnvironment = false;
    ArtfSkyDesc skyMore; /* (what the sky has past haze) */
    memset(&skyMore, 0, sizeof skyMore);
    const char* lutFile = NULL;
    ArtfPostDesc post;
    memset(&post, 0, sizeof post);
    float fog[4] = {0, 0, 0, 0};
    bool indirect = false, throughWalls = false;
    ArtfTraceDesc trace = {ARTF_TRACE_NONE, 1, 0, 0, false};
    int traceSamples = 1;
    ArtfRendererOptions o;
    memset(&o, 0, sizeof o);
    o.width = 1280;
    o.height = 720;
    o.noVsync = true;
    const char *out = NULL, *sceneName = "spheres";
    int frames = 0, fieldSide = 100;
    ArtfView view = ARTF_VIEW_LIT;
    bool passes = false;
    float hlodCell = 0, streamBudget = -1;
    uint32_t particles = 0;
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
        else if (!strcmp(a, "--ortho") && i + 1 < argc) orthoHeight = (float)atof(argv[++i]);
        else if (!strcmp(a, "--denoise")) trace.denoise = true;
        else if (!strcmp(a, "--hardware")) trace.hardware = true;
        else if (!strcmp(a, "--shadows") && i + 1 < argc) shadowSide = atoi(argv[++i]);
        else if (!strcmp(a, "--soft-shadows")) softShadows = true;
        else if (!strcmp(a, "--traced-shadows")) tracedShadows = true;
        else if (!strcmp(a, "--traced-reflections")) post.tracedReflections = true;
        else if (!strcmp(a, "--sun-size") && i + 1 < argc) sunSize = (float)atof(argv[++i]);
        else if (!strcmp(a, "--environment") && i + 1 < argc) environment = argv[++i];
        else if (!strcmp(a, "--environment-light") && i + 1 < argc) environmentLight = (float)atof(argv[++i]);
        else if (!strcmp(a, "--show-environment")) showEnvironment = true;
        else if (!strcmp(a, "--hdr")) o.hdr = true;
        else if (!strcmp(a, "--glow") && i + 1 < argc) post.bloom = (float)atof(argv[++i]);
        else if (!strcmp(a, "--glow-threshold") && i + 1 < argc) post.bloomThreshold = (float)atof(argv[++i]);
        else if (!strcmp(a, "--occlusion") && i + 1 < argc) post.occlusion = (float)atof(argv[++i]);
        else if (!strcmp(a, "--occlusion-radius") && i + 1 < argc) post.occlusionRadius = (float)atof(argv[++i]);
        else if (!strcmp(a, "--reflections") && i + 1 < argc) post.reflections = (float)atof(argv[++i]);
        else if (!strcmp(a, "--volumetric") && i + 1 < argc) post.volumetric = (float)atof(argv[++i]);
        else if (!strcmp(a, "--scattering") && i + 1 < argc) post.scattering = (float)atof(argv[++i]);
        else if (!strcmp(a, "--temporal")) post.temporal = true;
        else if (!strcmp(a, "--render-scale") && i + 1 < argc) o.renderScale = (float)atof(argv[++i]);
        else if (!strcmp(a, "--sharpen") && i + 1 < argc) post.sharpen = (float)atof(argv[++i]);
        else if (!strcmp(a, "--tone") && i + 1 < argc) {
            const char* curve = argv[++i];
            post.toneMap = !strcmp(curve, "neutral") ? ARTF_TONE_NEUTRAL : !strcmp(curve, "reinhard") ? ARTF_TONE_REINHARD : !strcmp(curve, "none") ? ARTF_TONE_NONE : ARTF_TONE_ACES;
        }
        else if (!strcmp(a, "--auto-exposure")) post.autoExposure = true;
        else if (!strcmp(a, "--temperature") && i + 1 < argc) post.temperature = (float)atof(argv[++i]);
        else if (!strcmp(a, "--tint") && i + 1 < argc) post.tint = (float)atof(argv[++i]);
        else if (!strcmp(a, "--saturation") && i + 1 < argc) post.saturation = (float)atof(argv[++i]);
        else if (!strcmp(a, "--contrast") && i + 1 < argc) post.contrast = (float)atof(argv[++i]);
        else if (!strcmp(a, "--vignette") && i + 1 < argc) post.vignette = (float)atof(argv[++i]);
        else if (!strcmp(a, "--chromatic") && i + 1 < argc) post.chromatic = (float)atof(argv[++i]);
        else if (!strcmp(a, "--motion-blur") && i + 1 < argc) post.motionBlur = (float)atof(argv[++i]);
        else if (!strcmp(a, "--indirect")) indirect = true;
        else if (!strcmp(a, "--through-walls")) throughWalls = true;
        else if (!strcmp(a, "--hlod")) hlodCell = i + 1 < argc && argv[i + 1][0] != '-' ? (float)atof(argv[++i]) : 64.f;
        else if (!strcmp(a, "--particles") && i + 1 < argc) particles = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "--stream-meshes")) streamBudget = i + 1 < argc && argv[i + 1][0] != '-' ? (float)atof(argv[++i]) : 0.f;
        else if (!strcmp(a, "--focus") && i + 1 < argc) post.focus = (float)atof(argv[++i]);
        else if (!strcmp(a, "--aperture") && i + 1 < argc) post.aperture = (float)atof(argv[++i]);
        else if (!strcmp(a, "--fog") && i + 4 < argc) { for (int k = 0; k < 4; ++k) fog[k] = (float)atof(argv[++i]); }
        else if (!strcmp(a, "--scene-exposure") && i + 1 < argc) sceneExposure = (float)atof(argv[++i]);
        else if (!strcmp(a, "--sky")) skyHaze = i + 1 < argc && argv[i + 1][0] != '-' ? (float)atof(argv[++i]) : 0.f;
        else if (!strcmp(a, "--sky-multiple") && i + 1 < argc) skyMore.multiple = (float)atof(argv[++i]);
        else if (!strcmp(a, "--sun-disc") && i + 1 < argc) skyMore.sunDisc = (float)atof(argv[++i]);
        else if (!strcmp(a, "--sky-width") && i + 1 < argc) skyMore.width = atoi(argv[++i]);
        else if (!strcmp(a, "--clouds") && i + 1 < argc) skyMore.clouds.coverage = (float)atof(argv[++i]);
        else if (!strcmp(a, "--cloud-base") && i + 1 < argc) skyMore.clouds.base = (float)atof(argv[++i]);
        else if (!strcmp(a, "--cloud-thickness") && i + 1 < argc) skyMore.clouds.thickness = (float)atof(argv[++i]);
        else if (!strcmp(a, "--cloud-shadow")) skyMore.clouds.shadow = true;
        else if (!strcmp(a, "--cloud-density") && i + 1 < argc) skyMore.clouds.density = (float)atof(argv[++i]);
        else if (!strcmp(a, "--wind") && i + 3 < argc) { skyMore.clouds.wind.x = (float)atof(argv[++i]); skyMore.clouds.wind.y = (float)atof(argv[++i]); skyMore.clouds.wind.z = (float)atof(argv[++i]); }
        else if (!strcmp(a, "--exposure-low") && i + 1 < argc) post.exposureLow = (float)atof(argv[++i]);
        else if (!strcmp(a, "--exposure-high") && i + 1 < argc) post.exposureHigh = (float)atof(argv[++i]);
        else if (!strcmp(a, "--exposure-speed") && i + 1 < argc) post.exposureSpeed = (float)atof(argv[++i]);
        else if (!strcmp(a, "--exposure-speed-down") && i + 1 < argc) post.exposureSpeedDown = (float)atof(argv[++i]);
        else if ((!strcmp(a, "--lift") || !strcmp(a, "--gamma") || !strcmp(a, "--gain") || !strcmp(a, "--grade-shadows") || !strcmp(a, "--grade-midtones") ||
                  !strcmp(a, "--grade-highlights")) && i + 3 < argc) {
            ArtfVec3* to = !strcmp(a, "--lift") ? &post.lift : !strcmp(a, "--gamma") ? &post.gamma : !strcmp(a, "--gain") ? &post.gain
                         : !strcmp(a, "--grade-shadows") ? &post.shadows : !strcmp(a, "--grade-midtones") ? &post.midtones : &post.highlights;
            to->x = (float)atof(argv[++i]);
            to->y = (float)atof(argv[++i]);
            to->z = (float)atof(argv[++i]);
        }
        else if (!strcmp(a, "--lut") && i + 1 < argc) lutFile = argv[++i];
        else if (!strcmp(a, "--lut-amount") && i + 1 < argc) post.lutAmount = (float)atof(argv[++i]);
        else if (!strcmp(a, "--grain") && i + 1 < argc) post.grain = (float)atof(argv[++i]);
        else if (!strcmp(a, "--flare") && i + 1 < argc) post.flare = (float)atof(argv[++i]);
        else if (!strcmp(a, "--sun") && i + 3 < argc) { for (int k = 0; k < 3; ++k) sunAt[k] = (float)atof(argv[++i]); }
        else if (!strcmp(a, "--fog-height") && i + 2 < argc) { fogHeight[0] = (float)atof(argv[++i]); fogHeight[1] = (float)atof(argv[++i]); }
        else if (!strcmp(a, "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(a, "--view") && i + 1 < argc) {
            if (!artfViewFromName(argv[++i], &view)) { fprintf(stderr, "no view called %s\n", argv[i]); return 2; }
        }
        else if (!strcmp(a, "--passes")) passes = true;
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
    artfRendererSetView(r, view);

    const double t0 = artfNowMs();
    const bool isHole = !strcmp(sceneName, "blackhole");
    ArtfScene* s = artfSceneNew();
    if (model && strcmp(sceneName, "spheres")) sceneName = "spheres";
    if (model) sceneModel(s, model);
    else if (!strcmp(sceneName, "field")) sceneField(s, fieldSide);
    else if (!strcmp(sceneName, "heavy")) sceneHeavy(s);
    else if (!strcmp(sceneName, "city")) sceneCity(s);
    else if (!strcmp(sceneName, "layers")) sceneLayers(s);
    else if (!strcmp(sceneName, "lights")) sceneLights(s);
    else if (!strcmp(sceneName, "outdoors")) sceneOutdoors(s);
    else if (!strcmp(sceneName, "forest")) sceneForest(s);
    else if (!strcmp(sceneName, "crowd")) sceneCrowd(s);
    else if (isHole) sceneBlackHole(s, &hole);
    else sceneSpheres(s);
    if (pngAlpha) artfSceneSetBackgroundAlpha(s, 0);
    if (sceneExposure > 0) artfSceneSetExposure(s, sceneExposure);
    float* lut = NULL;
    if (lutFile) {
        FILE* file = fopen(lutFile, "rb");
        if (!file) { fprintf(stderr, "cannot read %s\n", lutFile); return 1; }
        fseek(file, 0, SEEK_END);
        const long length = ftell(file);
        fseek(file, 0, SEEK_SET);
        char* bytes = malloc((size_t)length + 1);
        const size_t got = fread(bytes, 1, (size_t)length, file);
        fclose(file);
        lut = artfLutDecodeCube(bytes, got, &post.lutSize);
        free(bytes);
        if (!lut) { fprintf(stderr, "%s is not a .cube file\n", lutFile); return 1; }
        post.lut = lut;
    }
    artfSceneSetPost(s, &post);
    artfFree(lut);
    if (fog[0] > 0) artfSceneSetFog(s, artfVec3(fog[1], fog[2], fog[3]), fog[0]);
    if (fogHeight[1] > 0) artfSceneSetFogHeight(s, fogHeight[0], fogHeight[1]);
    if (sunSize >= 0) artfSceneSetSunSize(s, sunSize);
    if (sunAt[0] != 0 || sunAt[1] != 0 || sunAt[2] != 0) artfSceneSetSun(s, artfVec3(sunAt[0], sunAt[1], sunAt[2]), artfVec3(3, 3, 3));
    if (skyHaze >= 0) {
        ArtfSkyDesc sky = skyMore;
        sky.haze = skyHaze;
        const double skyStart = artfNowMs();
        artfSceneSetSky(s, &sky);
        printf("sky: %.1f ms\n", artfNowMs() - skyStart);
    }
    if (indirect) {
        ArtfIndirectDesc probes;
        memset(&probes, 0, sizeof probes);
        probes.throughWalls = throughWalls;
        artfSceneSetIndirect(s, &probes);
    }
    if (environment) {
        FILE* file = fopen(environment, "rb");
        if (!file) { fprintf(stderr, "cannot read %s\n", environment); return 1; }
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        void* data = malloc((size_t)size);
        const size_t got = fread(data, 1, (size_t)size, file);
        fclose(file);
        int w = 0, h = 0;
        float* rgb = got == (size_t)size ? artfImageDecodeHdr(data, (size_t)size, &w, &h) : NULL;
        free(data);
        if (!rgb) { fprintf(stderr, "%s is not a Radiance picture\n", environment); return 1; }
        const double e0 = artfNowMs();
        artfSceneSetEnvironment(s, rgb, w, h, environmentLight, showEnvironment);
        printf("environment of %d x %d made into maps in %.1f ms\n", w, h, artfNowMs() - e0);
        artfFree(rgb);
    }
    if (shadowSide || tracedShadows) {
        const ArtfShadowDesc shadows = {shadowSide, 0, softShadows, tracedShadows};
        artfSceneSetShadows(s, &shadows);
    }
    if (trace.mode) {
        trace.samples = 1;
        trace.maxSamples = traceSamples;
        artfSceneSetTrace(s, &trace);
    }
    if (hlodCell > 0) {
        const double h0 = artfNowMs();
        ArtfHlodDesc hlod;
        memset(&hlod, 0, sizeof hlod);
        hlod.cell = hlodCell;
        const uint32_t made = artfSceneBuildHlod(s, &hlod);
        printf("%u merged meshes in %.1f ms\n", made, artfNowMs() - h0);
    }
    ArtfEmitter emitter = 0;
    if (particles) {
        ArtfEmitterDesc e;
        memset(&e, 0, sizeof e);
        e.position = artfVec3(0, 0, 0);
        e.area = 1.f;
        e.velocity = artfVec3(0, 6, 0);
        e.spread = .6f;
        e.gravity = artfVec3(0, -6, 0);
        e.drag = .2f;
        e.life = 3;
        e.rate = (float)particles / 3.f;
        e.size = .08f;
        e.sizeEnd = .03f;
        e.color = artfVec3(1, .7f, .3f);
        e.colorEnd = artfVec3(.4f, .4f, 1);
        e.opacity = 1;
        e.opacityEnd = .1f;
        e.glow = 1;
        e.most = particles;
        e.turbulence = 2;
        e.ground = true;
        e.groundHeight = -1;
        e.bounce = .4f;
        e.spin = 1;
        e.parallel = true;
        emitter = artfSceneAddEmitter(s, &e);
        artfEmitterBurst(s, emitter, particles / 2);
        for (int k = 0; k < 60; ++k) artfEmitterStep(s, emitter, 1.f / 60.f);
    }
    if (streamBudget >= 0) {
        ArtfResidencyDesc residency;
        memset(&residency, 0, sizeof residency);
        residency.mode = ARTF_RESIDENCY_AUTOMATIC;
        residency.meshes = true;
        residency.meshBudget = (size_t)(streamBudget * 1048576.f);
        artfSceneSetResidency(s, &residency);
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
    /* (temporal antialiasing gathers over frames: a still picture gets sixteen) */
    for (int k = 1; k < 16 && (((post.temporal || post.autoExposure) && o.hdr) || indirect); ++k) artfRendererDraw(r, s);
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
            if (emitter) artfEmitterStep(s, emitter, 1.f / 60.f);
            if (!strcmp(sceneName, "crowd")) crowdBend(s, (float)i / 60.f);
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
        if (passes) {
            /* (on the GPU, the last frame it finished) */
            double sum = 0;
            for (uint32_t k = 0; k < st.passCount; ++k) sum += st.passes[k].ms;
            printf("  %s, by pass:\n", gpu ? "the GPU" : "the CPU");
            for (uint32_t k = 0; k < st.passCount; ++k) printf("    %-24s %8.3f ms  %5.1f%%\n", st.passes[k].name, st.passes[k].ms, sum > 0 ? st.passes[k].ms * 100.0 / sum : 0.0);
            printf("    %-24s %8.3f ms\n", "all", sum);
        }
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
