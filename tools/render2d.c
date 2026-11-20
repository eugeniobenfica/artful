/* artful-render2d — a 2D scene, lit, to a PNG; and what its frames take. It links artful-2d alone: nothing of 3D.
 *
 *   artful-render2d [--width w] [--height h] [--threads n] [--frames n] [--no-gi] [--no-shadows] [--gi-scale s]
 *                   [--bloom b] [--time t] [--backend auto|software|vulkan] [--window] [--no-vsync] [--out file.png]
 *                   [--json file]
 *
 * --json: what each frame took, as artful-render --json writes it (tools/bench.py reads both): the time from one frame to
 * the next, the CPU's and the GPU's p50, p95, p99 and most, the first frame, and the mean of each part of a frame.
 *
 * --window (Windows): the frames shown in a window, through Vulkan's swapchain, as fast as they come with --no-vsync;
 * else each frame is read back, as a program that shows them itself would. Either way, what a frame takes is the time
 * from one to the next, with the GPU and the CPU each on a frame at once.
 *
 * The scene: a room of stone, its floor of bricks with their relief in a normal map, walls that cast shadows, a torch
 * as wide as a flame, a cold spot through a door, crystals that glow and light the room by gi, and dust in the air.
 * Every texture is made here, from numbers. */
#include "artful/2d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
    ihdr[9] = 2;
    chunk(f, "IHDR", ihdr, 13);
    const size_t row = (size_t)w * 3 + 1, raw = row * h;
    uint8_t* data = malloc(raw);
    for (int y = 0; y < h; ++y) {
        uint8_t* o = data + row * y;
        *o++ = 0;
        for (int x = 0; x < w; ++x) {
            const uint32_t p = pixels[(size_t)y * w + x];
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
        const size_t len = raw - at < 65535 ? raw - at : 65535;
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

/* ---- textures, from numbers ---- */

static uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    return x ^ x >> 16;
}
static float noise(int x, int y, uint32_t seed) { return (float)(hash((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ seed) & 0xFFFF) / 65535.f; }

/* Smooth noise, a cell of `cell` texels, wrapping at size. */
static float smooth(int x, int y, int cell, int size, uint32_t seed) {
    const int cx = x / cell, cy = y / cell, n = size / cell;
    const float fx = (float)(x % cell) / (float)cell, fy = (float)(y % cell) / (float)cell;
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float a = noise(cx % n, cy % n, seed), b = noise((cx + 1) % n, cy % n, seed);
    const float c = noise(cx % n, (cy + 1) % n, seed), d = noise((cx + 1) % n, (cy + 1) % n, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

static uint32_t bgra(float r, float g, float b) {
    const int R = (int)(r < 0 ? 0 : r > 1 ? 255 : r * 255.f + .5f), G = (int)(g < 0 ? 0 : g > 1 ? 255 : g * 255.f + .5f);
    const int B = (int)(b < 0 ? 0 : b > 1 ? 255 : b * 255.f + .5f);
    return 0xFF000000u | (uint32_t)R << 16 | (uint32_t)G << 8 | (uint32_t)B;
}

/* A normal map from heights: y up in the map, as tools make them. */
static void normalsOf(const float* height, int size, float strength, uint32_t* out) {
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float l = height[y * size + (x + size - 1) % size], r = height[y * size + (x + 1) % size];
            const float u = height[((y + size - 1) % size) * size + x], d = height[((y + 1) % size) * size + x];
            float nx = (l - r) * strength, ny = (d - u) * strength, nz = 1;
            const float len = sqrtf(nx * nx + ny * ny + nz * nz);
            nx /= len, ny /= len, nz /= len;
            out[y * size + x] = bgra(nx * .5f + .5f, ny * .5f + .5f, nz * .5f + .5f);
        }
}

enum { TEX = 128 };

/* Bricks: their color, and their relief. */
static void bricks(uint32_t* color, uint32_t* normals, bool stone) {
    float* height = malloc(sizeof(float) * TEX * TEX);
    for (int y = 0; y < TEX; ++y)
        for (int x = 0; x < TEX; ++x) {
            const int bh = stone ? 32 : 16, bw = stone ? 32 : 32;
            const int row = y / bh, ox = (row & 1) * bw / 2;
            const int bx = ((x + ox) % TEX) / bw, by = row;
            const int ix = (x + ox) % bw, iy = y % bh;
            const float edge = (float)(ix < iy ? (ix < bw - ix ? ix : bw - ix) : 0);
            const int dx = ix < bw - 1 - ix ? ix : bw - 1 - ix, dy = iy < bh - 1 - iy ? iy : bh - 1 - iy;
            const int dmin = dx < dy ? dx : dy;
            (void)edge;
            const float grain = smooth(x, y, 8, TEX, 7) * .5f + noise(x, y, 3) * .2f;
            const float mortar = dmin < 1 ? 1.f : 0.f;
            const float bevel = dmin < 3 ? (float)dmin / 3.f : 1.f;
            height[y * TEX + x] = mortar ? 0 : bevel * (.8f + grain * .3f);
            const float tint = noise(bx, by, stone ? 11u : 5u) * .25f;
            float r, g, b;
            if (stone) r = .34f + tint + grain * .15f, g = .32f + tint + grain * .14f, b = .33f + tint * .8f + grain * .12f;
            else r = .36f + tint * .8f + grain * .14f, g = .29f + tint * .6f + grain * .11f, b = .24f + tint * .5f + grain * .09f;
            if (mortar) r = g = b = .12f;
            color[y * TEX + x] = bgra(r, g, b);
        }
    normalsOf(height, TEX, stone ? 3.f : 4.f, normals);
    free(height);
}

/* A crystal's glow: bright in the middle, a facet's lines. */
static void crystal(uint32_t* color) {
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float dx = ((float)x - 15.5f) / 16.f, dy = ((float)y - 15.5f) / 16.f;
            const float d = 1.f - (dx < 0 ? -dx : dx) - (dy < 0 ? -dy : dy) * .6f;
            const float a = d > 0 ? 1.f : 0.f;
            const float v = d > 0 ? .55f + d * .45f : 0;
            color[y * 32 + x] = (uint32_t)(a * 255.f) << 24 | (bgra(v, v, v) & 0xFFFFFFu);
        }
}

/* ---- the room ---- */

typedef struct Room {
    bool shadows;
    ArtfLight2D torch;
    ArtfSprite2D flame;
    ArtfSprite2D dust[24];
} Room;

static void addWall(ArtfScene2D* s, float x, float y, float w, float h, ArtfTexture2D color, ArtfTexture2D normals) {
    ArtfSprite2DDesc d = artfSprite2DDefaults();
    d.position = artfVec2(x, y);
    d.size = artfVec2(w, h);
    d.texture = color;
    d.normalMap = normals;
    d.uvMax = artfVec2(w / 64.f, h / 64.f);
    d.castsShadow = true;
    d.layer = 2;
    d.specular = .2f;
    d.roughness = .6f;
    artfScene2DAddSprite(s, &d);
}

static void build(ArtfScene2D* s, Room* room, bool shadows) {
    room->shadows = shadows;
    static uint32_t floorColor[TEX * TEX], floorNormals[TEX * TEX], stoneColor[TEX * TEX], stoneNormals[TEX * TEX], glow[32 * 32];
    bricks(floorColor, floorNormals, false);
    bricks(stoneColor, stoneNormals, true);
    crystal(glow);
    ArtfTexture2DDesc td = {.pixels = floorColor, .width = TEX, .height = TEX};
    const ArtfTexture2D fc = artfScene2DAddTexture(s, &td);
    td.pixels = stoneColor;
    const ArtfTexture2D sc = artfScene2DAddTexture(s, &td);
    td.pixels = glow;
    td.width = td.height = 32;
    const ArtfTexture2D gc = artfScene2DAddTexture(s, &td);
    ArtfTexture2DDesc nd = {.pixels = floorNormals, .width = TEX, .height = TEX, .normalMap = true};
    const ArtfTexture2D fn = artfScene2DAddTexture(s, &nd);
    nd.pixels = stoneNormals;
    const ArtfTexture2D sn = artfScene2DAddTexture(s, &nd);

    /* (a world of 640 by 360, its middle at 0) */
    ArtfSprite2DDesc floor_ = artfSprite2DDefaults();
    floor_.size = artfVec2(640, 360);
    floor_.texture = fc;
    floor_.normalMap = fn;
    floor_.uvMax = artfVec2(640.f / 96.f, 360.f / 96.f);
    floor_.specular = .35f;
    floor_.roughness = .45f;
    artfScene2DAddSprite(s, &floor_);

    /* walls: the room's sides, a pillar, a wall with a door */
    addWall(s, 0, -170, 640, 20, sc, sn);
    addWall(s, 0, 170, 640, 20, sc, sn);
    addWall(s, -310, 0, 20, 360, sc, sn);
    addWall(s, 310, 0, 20, 360, sc, sn);
    addWall(s, 90, -95, 20, 130, sc, sn);
    addWall(s, 90, 115, 20, 90, sc, sn);
    addWall(s, -110, 40, 44, 44, sc, sn);
    /* a round column */
    ArtfSprite2DDesc column = artfSprite2DDefaults();
    column.shape = ARTF_SHAPE_ELLIPSE;
    column.position = artfVec2(-60, -80);
    column.size = artfVec2(34, 34);
    column.texture = sc;
    column.normalMap = sn;
    column.castsShadow = true;
    column.layer = 2;
    artfScene2DAddSprite(s, &column);

    /* crystals, that glow */
    const float crystals[4][5] = {{200, -60, .2f, 1.f, .9f}, {230, -40, .3f, .9f, 1.f}, {215, 110, 1.f, .25f, .8f}, {-250, 120, .3f, 1.f, .4f}};
    for (int i = 0; i < 4; ++i) {
        ArtfSprite2DDesc c = artfSprite2DDefaults();
        c.position = artfVec2(crystals[i][0], crystals[i][1]);
        c.size = artfVec2(18, 28);
        c.rotation = (float)i * .5f - .6f;
        c.texture = gc;
        c.color = artfVec3(crystals[i][2] * .3f, crystals[i][3] * .3f, crystals[i][4] * .3f);
        c.emissive = artfVec3(crystals[i][2] * 6, crystals[i][3] * 6, crystals[i][4] * 6);
        c.emissiveMap = gc;
        c.layer = 3;
        artfScene2DAddSprite(s, &c);
    }

    /* the torch: a flame (unlit, glowing) and its light, as wide as it */
    ArtfSprite2DDesc flame = artfSprite2DDefaults();
    flame.shape = ARTF_SHAPE_ELLIPSE;
    flame.position = artfVec2(-180, -40);
    flame.size = artfVec2(10, 14);
    flame.color = artfVec3(1, .7f, .3f);
    flame.emissive = artfVec3(8, 4.5f, 1.5f);
    flame.unlit = true;
    flame.layer = 4;
    room->flame = artfScene2DAddSprite(s, &flame);
    ArtfLight2DDesc torch = artfLight2DDefaults();
    torch.position = flame.position;
    torch.color = artfVec3(1, .62f, .3f);
    torch.intensity = 1.1f;
    torch.range = 330;
    torch.radius = 7;
    torch.height = 50;
    torch.shadows = shadows;
    room->torch = artfScene2DAddLight(s, &torch);

    /* moonlight through the door, from the right */
    ArtfLight2DDesc spot = artfLight2DDefaults();
    spot.kind = ARTF_LIGHT2D_SPOT;
    spot.position = artfVec2(290, 30);
    spot.direction = 3.3f;
    spot.coneInner = .25f;
    spot.coneOuter = .45f;
    spot.color = artfVec3(.45f, .6f, 1);
    spot.intensity = 1.2f;
    spot.range = 420;
    spot.radius = 4;
    spot.height = 70;
    spot.shadows = shadows;
    artfScene2DAddLight(s, &spot);

    /* dust, lit, over everything */
    for (int i = 0; i < 24; ++i) {
        ArtfSprite2DDesc d = artfSprite2DDefaults();
        d.shape = ARTF_SHAPE_ELLIPSE;
        d.position = artfVec2((noise(i, 1, 9) - .5f) * 560.f, (noise(i, 2, 9) - .5f) * 300.f);
        d.size = artfVec2(2.5f, 2.5f);
        d.color = artfVec3(1, 1, 1);
        d.opacity = .5f;
        d.layer = 5;
        room->dust[i] = artfScene2DAddSprite(s, &d);
    }

    artfScene2DSetAmbient(s, artfVec3(.025f, .025f, .035f));
    artfScene2DSetBackground(s, artfVec3(0, 0, 0), 1);
}

/* The torch flickers, and the dust drifts. */
static void animate(ArtfScene2D* s, Room* room, float t) {
    ArtfLight2DDesc torch = artfLight2DDefaults();
    const float flicker = 1.f + .08f * sinf(t * 13.f) + .05f * sinf(t * 31.f + 1.f);
    torch.position = artfVec2(-180 + sinf(t * 7.f) * 1.5f, -40 + cosf(t * 9.f) * 1.f);
    torch.color = artfVec3(1, .62f, .3f);
    torch.intensity = 1.1f * flicker;
    torch.range = 330;
    torch.radius = 7;
    torch.height = 50;
    torch.shadows = room->shadows;
    artfLight2DSet(s, room->torch, &torch);
    artfSprite2DPlace(s, room->flame, torch.position, sinf(t * 5.f) * .1f);
    for (int i = 0; i < 24; ++i) {
        const float x = (noise(i, 1, 9) - .5f) * 560.f + sinf(t * .7f + (float)i) * 20.f;
        const float y = (noise(i, 2, 9) - .5f) * 300.f - t * 6.f;
        artfSprite2DPlace(s, room->dust[i], artfVec2(x, y), 0);
    }
}

/* ---- what frames took, as JSON (--json), as artful-render's ---- */

static int byDouble(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static void percentiles(FILE* f, const double* times, int n) {
    double* sorted = malloc(sizeof(double) * (size_t)n);
    memcpy(sorted, times, sizeof(double) * (size_t)n);
    qsort(sorted, (size_t)n, sizeof(double), byDouble);
    const double p50 = sorted[(n - 1) / 2], p95 = sorted[(int)((n - 1) * .95)], p99 = sorted[(int)((n - 1) * .99)];
    fprintf(f, "{\"p50\": %.4f, \"p95\": %.4f, \"p99\": %.4f, \"max\": %.4f}", p50, p95, p99, sorted[n - 1]);
    free(sorted);
}

static void jsonString(FILE* f, const char* text) {
    fputc('"', f);
    for (const char* p = text; *p; ++p) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        if ((unsigned char)*p >= 32) fputc(*p, f);
    }
    fputc('"', f);
}

static bool writeBench(const char* path, int argc, char** argv, const ArtfRenderer2D* r, int width, int height, int frames, double perFrame, double firstMs,
                       double sinceStartMs, const double* frameTimes, const double* cpuTimes, const double* gpuTimes, const ArtfFrameStats2D* sum) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "{\n  \"tool\": \"artful-render2d\", \"version\": \"%d.%d.%d\",\n  \"arguments\": [", ARTF_VERSION_MAJOR, ARTF_VERSION_MINOR, ARTF_VERSION_PATCH);
    for (int i = 1; i < argc; ++i) {
        if (i > 1) fputs(", ", f);
        jsonString(f, argv[i]);
    }
    fputs("],\n  \"scene\": \"room2d\", \"renderer\": ", f);
    jsonString(f, artfRenderer2DDescription(r));
    const bool vulkan = artfRenderer2DBackend(r) == ARTF_BACKEND2D_VULKAN;
    fprintf(f, ", \"backend\": \"%s\", \"width\": %d, \"height\": %d,\n", vulkan ? "vulkan" : "software", width, height);
    fprintf(f, "  \"frames\": %d, \"msPerFrame\": %.4f, \"firstFrameMs\": %.3f, \"untilLastFrameMs\": %.3f,\n", frames, perFrame, firstMs, sinceStartMs);
    fputs("  \"frameMs\": ", f);
    percentiles(f, frameTimes, frames);
    fputs(",\n  \"cpuMs\": ", f);
    percentiles(f, cpuTimes, frames);
    if (vulkan) {
        fputs(",\n  \"gpuMs\": ", f);
        percentiles(f, gpuTimes, frames);
    }
    const double n = frames;
    fprintf(f, ",\n  \"passes\": {\"shadows\": %.4f, \"gi\": %.4f, \"tiles\": %.4f, \"after\": %.4f},\n", sum->shadowsMs / n, sum->giMs / n, sum->drawMs / n, sum->postMs / n);
    fprintf(f, "  \"sprites\": %u, \"lights\": %u, \"shadowMaps\": %u\n}\n", sum->spritesDrawn, sum->lightsDrawn, sum->shadowMaps);
    return fclose(f) == 0;
}

int main(int argc, char** argv) {
    const double started = artfNowMs();
    const char* json = NULL;
    int width = 1280, height = 720, threads = 0, frames = 1, giScale = 4;
    bool gi = true, shadows = true;
    float bloom = .06f, time = 0;
    const char* out = "room2d.png";
    ArtfBackend2D backend = ARTF_BACKEND2D_AUTO;
    bool window = false, noVsync = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool more = i + 1 < argc;
        if (!strcmp(a, "--width") && more) width = atoi(argv[++i]);
        else if (!strcmp(a, "--height") && more) height = atoi(argv[++i]);
        else if (!strcmp(a, "--threads") && more) threads = atoi(argv[++i]);
        else if (!strcmp(a, "--frames") && more) frames = atoi(argv[++i]);
        else if (!strcmp(a, "--gi-scale") && more) giScale = atoi(argv[++i]);
        else if (!strcmp(a, "--bloom") && more) bloom = (float)atof(argv[++i]);
        else if (!strcmp(a, "--time") && more) time = (float)atof(argv[++i]);
        else if (!strcmp(a, "--out") && more) out = argv[++i];
        else if (!strcmp(a, "--json") && more) json = argv[++i];
        else if (!strcmp(a, "--backend") && more) {
            const char* b = argv[++i];
            backend = !strcmp(b, "vulkan") ? ARTF_BACKEND2D_VULKAN : !strcmp(b, "software") ? ARTF_BACKEND2D_SOFTWARE : ARTF_BACKEND2D_AUTO;
        } else if (!strcmp(a, "--window")) window = true;
        else if (!strcmp(a, "--no-vsync")) noVsync = true;
        else if (!strcmp(a, "--no-gi")) gi = false;
        else if (!strcmp(a, "--no-shadows")) shadows = false;
        else {
            fprintf(stderr, "usage: artful-render2d [--width w] [--height h] [--threads n] [--frames n] [--no-gi] [--no-shadows]\n"
                            "                       [--gi-scale s] [--bloom b] [--time t] [--backend auto|software|vulkan] [--window]\n"
                            "                       [--no-vsync] [--out file.png] [--json file]\n");
            return 1;
        }
    }
    ArtfScene2D* s = artfScene2DNew();
    Room room;
    build(s, &room, shadows);
    ArtfCamera2D camera = {.center = {0, 0}, .zoom = (float)height / 360.f};
    artfScene2DSetCamera(s, &camera);
    ArtfGI2DDesc g = artfGI2DDefaults();
    g.enabled = gi;
    g.scale = giScale;
    g.intensity = .7f;
    g.bounce = .6f;
    artfScene2DSetGI(s, &g);
    ArtfPost2DDesc post = {.exposure = 1.f, .tone = ARTF_TONE2D_ACES, .bloom = bloom, .bloomRadius = .04f};
    artfScene2DSetPost(s, &post);

    ArtfRenderer2DOptions o = {.width = width, .height = height, .threads = threads, .backend = backend, .noVsync = noVsync};
#ifdef _WIN32
    HWND shown = NULL;
    if (window) {
        RECT area = {0, 0, width, height};
        AdjustWindowRect(&area, WS_OVERLAPPEDWINDOW, FALSE);
        shown = CreateWindowExA(0, "STATIC", "artful-render2d", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left,
                                area.bottom - area.top, NULL, NULL, GetModuleHandleA(NULL), NULL);
        o.surface.kind = ARTF_SURFACE_WIN32;
        o.surface.a = shown;
    }
#else
    if (window) fputs("--window: Windows only; frames stay in memory\n", stderr);
#endif
    ArtfRenderer2D* r = artfRenderer2DNew(&o);
    if (!r) {
        fputs("that backend cannot run here\n", stderr);
        return 1;
    }
    ArtfFrameStats2D sum = {0};
    double began = 0, firstMs = 0, last = 0;
    const int counted = frames > 0 ? frames : 1;
    double* frameTimes = calloc((size_t)counted, sizeof(double));
    double* cpuTimes = calloc((size_t)counted, sizeof(double));
    double* gpuTimes = calloc((size_t)counted, sizeof(double));
    /* (two frames before the ones measured: what bounces needs a frame before it) */
    for (int i = -2; i < frames; ++i) {
        if (!i) began = last = artfNowMs();
#ifdef _WIN32
        MSG message;
        while (shown && PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&message);
#endif
        animate(s, &room, time + (float)artfMaxi(i, 0) / 60.f);
        artfRenderer2DDraw(r, s);
        if (!window) artfRenderer2DPixels(r); /* (read back, each frame) */
        if (i == -2) firstMs = artfNowMs() - started;
        if (i < 0) continue;
        ArtfFrameStats2D st;
        artfRenderer2DStats(r, &st);
        const double now = artfNowMs();
        frameTimes[i] = now - last;
        last = now;
        cpuTimes[i] = st.cpuMs;
        gpuTimes[i] = st.gpuMs;
        sum.gpuMs += st.gpuMs, sum.cpuMs += st.cpuMs, sum.shadowsMs += st.shadowsMs, sum.giMs += st.giMs, sum.drawMs += st.drawMs, sum.postMs += st.postMs;
        sum.spritesDrawn = st.spritesDrawn, sum.lightsDrawn = st.lightsDrawn, sum.shadowMaps = st.shadowMaps;
    }
    const double n = frames > 0 ? frames : 1, each = (artfNowMs() - began) / n;
    printf("%s%s, %dx%d, %d sprites, %u lights, %u shadow maps: %.2f ms a frame, %.0f a second (the CPU %.2f: shadows %.2f, gi %.2f, tiles %.2f, "
           "after %.2f; the GPU %.2f)\n",
           artfRenderer2DDescription(r), window ? ", in a window" : "", width, height, (int)sum.spritesDrawn, sum.lightsDrawn, sum.shadowMaps, each,
           1000. / each, sum.cpuMs / n, sum.shadowsMs / n, sum.giMs / n, sum.drawMs / n, sum.postMs / n, sum.gpuMs / n);
    if (json && frames > 0 && !writeBench(json, argc, argv, r, width, height, frames, each, firstMs, artfNowMs() - started, frameTimes, cpuTimes, gpuTimes, &sum)) {
        fprintf(stderr, "could not write %s\n", json);
        return 1;
    }
    free(frameTimes);
    free(cpuTimes);
    free(gpuTimes);
    uint32_t* pixels = malloc((size_t)width * height * 4);
    const bool saved = artfRenderer2DReadPixels(r, pixels) && savePng(out, pixels, width, height);
    free(pixels);
#ifdef _WIN32
    if (shown) DestroyWindow(shown);
#endif
    artfRenderer2DFree(r);
    artfScene2DFree(s);
    if (!saved) {
        fprintf(stderr, "could not write %s\n", out);
        return 1;
    }
    return 0;
}
