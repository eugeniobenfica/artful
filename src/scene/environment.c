/* artful — light from all around, from a picture of it.
 *
 * A picture of all that is around (equirectangular: longitude across, latitude down) becomes maps of an
 * octahedron around +y: a direction falls on one by additions and a division, with no angle to work out, so
 * the CPU and the GPU find the same texels. The first map is the picture itself; the next four are what
 * surfaces of roughness .25 to 1 reflect, each texel the sum of what GGX's lobe around it gathers (as the eye
 * looked along the normal: the usual approximation); the last is what a surface facing each way takes from all
 * around, over pi. Each has a border of one texel, from across the fold, so that sampling needs no care at
 * the edges.
 *
 * Making them reads every texel of a smaller map for every texel of a larger: all cores share the work. No
 * sine, power or exponent from the C library goes in, so that the maps are the same bytes everywhere. */
#include "scene/sceneInternal.h"
#include "core/threads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- angles, by polynomials ---- */

/* atan2(y, x) in radians, within 1e-5. */
static float angleOf(float y, float x) {
    const float ax = fabsf(x), ay = fabsf(y);
    const float most = artfMaxf(artfMaxf(ax, ay), 1e-20f), least = artfMinf(ax, ay);
    const float a = least / most, s = a * a;
    float r = ((-.0464964749f * s + .15931422f) * s - .327622764f) * s * a + a;
    if (ay > ax) r = 1.57079633f - r;
    if (x < 0) r = 3.14159265f - r;
    return y < 0 ? -r : r;
}

/* ---- the picture ---- */

typedef struct Picture { const float* rgb; int width, height; } Picture;

/* What the picture shows toward a direction: +y is its top row, -z the middle of its columns. */
static void pictureAt(const Picture* p, float x, float y, float z, float* out) {
    const float u = .5f + angleOf(x, -z) * .15915494f;
    const float v = angleOf(sqrtf(artfMaxf(1.f - y * y, 0)), y) * .31830989f;
    float tx = u * (float)p->width - .5f, ty = artfClampf(v * (float)p->height - .5f, 0, (float)(p->height - 1));
    const float cx = floorf(tx), cy = floorf(ty), fx = tx - cx, fy = ty - cy;
    const int x0 = ((int)cx % p->width + p->width) % p->width, x1 = (x0 + 1) % p->width;
    const int y0 = (int)cy, y1 = artfMini(y0 + 1, p->height - 1);
    const float* a = p->rgb + 3 * ((size_t)y0 * p->width + x0);
    const float* b = p->rgb + 3 * ((size_t)y0 * p->width + x1);
    const float* c = p->rgb + 3 * ((size_t)y1 * p->width + x0);
    const float* d = p->rgb + 3 * ((size_t)y1 * p->width + x1);
    for (int k = 0; k < 3; ++k) {
        const float top = a[k] + (b[k] - a[k]) * fx, bottom = c[k] + (d[k] - c[k]) * fx;
        out[k] = artfMaxf(top + (bottom - top) * fy, 0);
    }
}

/* ---- the octahedron ---- */

/* The direction at (u, v), which may be past an edge by a texel: across the fold, as the map's border is. */
static void directionAt(float u, float v, float* d) {
    if (u < 0 || u > 1) {
        u = u < 0 ? -u : 2.f - u;
        v = 1.f - v;
    }
    if (v < 0 || v > 1) {
        v = v < 0 ? -v : 2.f - v;
        u = 1.f - u;
    }
    float a = u * 2.f - 1.f, b = v * 2.f - 1.f;
    const float y = 1.f - fabsf(a) - fabsf(b);
    if (y < 0) {
        const float folded = (1.f - fabsf(b)) * (a >= 0 ? 1.f : -1.f);
        b = (1.f - fabsf(a)) * (b >= 0 ? 1.f : -1.f);
        a = folded;
    }
    const float length = sqrtf(a * a + y * y + b * b);
    d[0] = a / length;
    d[1] = y / length;
    d[2] = b / length;
}

/* A map of n texels a side, of what lies toward each, with its solid angle: a texel of the octahedron covers
 * more of the sphere toward its faces' middles. */
typedef struct Source { float* rgb; float* dir; float* weight; uint32_t count; } Source;

typedef struct Job {
    const Picture* picture;
    const Source* source;
    float* out;       /* (n + 2)^2 texels, three floats each */
    uint32_t n;
    float a2;         /* GGX's alpha squared; 0: the light on a surface, cosine weighted */
    int samples;      /* of the picture, on a side, for each texel of a map made from it */
} Job;

static void texelOf(const Job* j, uint32_t row, uint32_t column, float* out) {
    const uint32_t n = j->n;
    float d[3];
    if (j->picture) {
        /* from the picture: the average of samples x samples within the texel */
        out[0] = out[1] = out[2] = 0;
        for (int sy = 0; sy < j->samples; ++sy)
            for (int sx = 0; sx < j->samples; ++sx) {
                const float u = ((float)column - 1.f + ((float)sx + .5f) / (float)j->samples) / (float)n;
                const float v = ((float)row - 1.f + ((float)sy + .5f) / (float)j->samples) / (float)n;
                float c[3];
                directionAt(u, v, d);
                pictureAt(j->picture, d[0], d[1], d[2], c);
                for (int k = 0; k < 3; ++k) out[k] += c[k];
            }
        const float share = 1.f / (float)(j->samples * j->samples);
        for (int k = 0; k < 3; ++k) out[k] *= share;
        return;
    }
    directionAt(((float)column - .5f) / (float)n, ((float)row - .5f) / (float)n, d);
    double sum[3] = {0, 0, 0}, total = 0;
    const Source* s = j->source;
    for (uint32_t i = 0; i < s->count; ++i) {
        const float* l = s->dir + 3 * i;
        const float nl = d[0] * l[0] + d[1] * l[1] + d[2] * l[2];
        if (nl <= 0) continue;
        float w = nl * s->weight[i];
        if (j->a2 > 0) {
            /* GGX's D at the half vector: n.h^2 is (1 + n.l) / 2 */
            const float nh2 = (1.f + nl) * .5f, q = nh2 * (j->a2 - 1.f) + 1.f;
            w *= j->a2 / (q * q);
        }
        for (int k = 0; k < 3; ++k) sum[k] += (double)(s->rgb[3 * i + k] * w);
        total += (double)w;
    }
    for (int k = 0; k < 3; ++k) out[k] = total > 0 ? (float)(sum[k] / total) : 0.f;
}

static void rowJob(void* user, uint32_t row, uint32_t worker) {
    (void)worker;
    const Job* j = user;
    const uint32_t w = j->n + 2;
    for (uint32_t column = 0; column < w; ++column) texelOf(j, row, column, j->out + 3 * ((size_t)row * w + column));
}

static void runRows(ArtfPool* pool, Job* job) {
    if (pool) artfPoolRun(pool, rowJob, job, job->n + 2);
    else
        for (uint32_t row = 0; row < job->n + 2; ++row) rowJob(job, row, 0);
}

/* A map of the picture, n a side, made a source for the others: its texels' inside only, with their directions. */
static Source sourceOf(ArtfPool* pool, const Picture* p, uint32_t n, int samples) {
    float* map = artfMalloc((size_t)(n + 2) * (n + 2) * 12);
    Job job = {p, NULL, map, n, 0, samples};
    runRows(pool, &job);
    Source s = {artfMalloc((size_t)n * n * 12), artfMalloc((size_t)n * n * 12), artfMalloc((size_t)n * n * 4), n * n};
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            const uint32_t i = y * n + x;
            memcpy(s.rgb + 3 * i, map + 3 * ((size_t)(y + 1) * (n + 2) + x + 1), 12);
            float* d = s.dir + 3 * i;
            directionAt(((float)x + .5f) / (float)n, ((float)y + .5f) / (float)n, d);
            /* a texel's solid angle goes as the cube of the direction's sum of sizes, on the octahedron's face */
            const float l1 = fabsf(d[0]) + fabsf(d[1]) + fabsf(d[2]);
            s.weight[i] = l1 * l1 * l1;
        }
    artfFree(map);
    return s;
}

static void sourceFree(Source* s) {
    artfFree(s->rgb);
    artfFree(s->dir);
    artfFree(s->weight);
}

bool artfSceneSetEnvironment(ArtfScene* s, const float* rgb, int width, int height, float light, bool shown) {
    if (!rgb) {
        artfFree(s->environment);
        s->environment = NULL;
        ++s->environmentVersion;
        return true;
    }
    if (width < 2 || height < 1) {
        artfLog("artful: an environment of %d x %d pixels", width, height);
        return false;
    }
    float* env = s->environment ? s->environment : artfMalloc(ARTF_ENV_FLOATS * sizeof(float));
    const Picture picture = {rgb, width, height};
    ArtfPool* pool = artfPoolNew(0);
    /* the first map, of the picture itself, then the sources the others are made from */
    Job first = {&picture, NULL, env, artfEnvSize(0), 0, 2};
    runRows(pool, &first);
    Source fine = sourceOf(pool, &picture, 64, 4), coarse = sourceOf(pool, &picture, 32, 6);
    for (int level = 1; level < ARTF_ENV_LEVELS; ++level) {
        const float rough = level == ARTF_ENV_DIFFUSE ? 0.f : (float)level * .25f, a = rough * rough;
        Job job = {NULL, level == 1 ? &fine : &coarse, env + artfEnvStart(level), artfEnvSize(level), a * a, 0};
        runRows(pool, &job);
    }
    sourceFree(&fine);
    sourceFree(&coarse);
    artfPoolFree(pool);
    s->environment = env;
    s->environmentLight = light;
    s->environmentShown = shown;
    ++s->environmentVersion;
    return true;
}

void artfSceneSetEnvironmentLight(ArtfScene* s, float light, bool shown) {
    s->environmentLight = light;
    s->environmentShown = shown;
}

/* ---- Radiance's .hdr ---- */

static bool hdrRow(const uint8_t** at, const uint8_t* end, uint8_t* out, int width) {
    const uint8_t* p = *at;
    if (width < 8 || width > 32767 || end - p < 4 || p[0] != 2 || p[1] != 2 || (p[2] << 8 | p[3]) != width || p[2] & 0x80) {
        /* flat: four bytes a pixel */
        if (end - p < (ptrdiff_t)width * 4) return false;
        memcpy(out, p, (size_t)width * 4);
        *at = p + (size_t)width * 4;
        return true;
    }
    p += 4;
    /* by channel, each in runs */
    for (int c = 0; c < 4; ++c)
        for (int x = 0; x < width;) {
            if (p >= end) return false;
            int count = *p++;
            if (count > 128) {
                count -= 128;
                if (p >= end || x + count > width) return false;
                for (int i = 0; i < count; ++i) out[(x + i) * 4 + c] = *p;
                ++p;
            } else {
                if (!count || x + count > width || end - p < count) return false;
                for (int i = 0; i < count; ++i) out[(x + i) * 4 + c] = p[i];
                p += count;
            }
            x += count;
        }
    *at = p;
    return true;
}

float* artfImageDecodeHdr(const void* data, size_t size, int* width, int* height) {
    const uint8_t* p = data;
    const uint8_t* end = p + size;
    if (size < 11 || (memcmp(p, "#?RADIANCE", 10) && memcmp(p, "#?RGBE", 6))) return NULL;
    /* the header, to an empty line */
    bool rgbe = true;
    while (p < end) {
        const uint8_t* line = p;
        while (p < end && *p != '\n') ++p;
        if (p >= end) return NULL;
        const size_t length = (size_t)(p - line);
        ++p;
        if (!length) break;
        if (length > 7 && !memcmp(line, "FORMAT=", 7)) rgbe = length >= 22 && !memcmp(line + 7, "32-bit_rle_rgbe", 15);
    }
    if (!rgbe) return NULL;
    /* "-Y height +X width": rows from the top, as all of them are written */
    char line[64];
    size_t n = 0;
    while (p < end && *p != '\n' && n + 1 < sizeof line) line[n++] = (char)*p++;
    line[n] = 0;
    if (p >= end) return NULL;
    ++p;
    int w = 0, h = 0;
    char ys = 0, xs = 0, ya = 0, xa = 0;
    if (sscanf(line, "%c%c %d %c%c %d", &ys, &ya, &h, &xs, &xa, &w) != 6 || ys != '-' || ya != 'Y' || xs != '+' || xa != 'X') return NULL;
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768) return NULL;
    float* out = artfMalloc((size_t)w * h * 12);
    uint8_t* row = artfMalloc((size_t)w * 4);
    for (int y = 0; y < h; ++y) {
        if (!hdrRow(&p, end, row, w)) {
            artfFree(row);
            artfFree(out);
            return NULL;
        }
        for (int x = 0; x < w; ++x) {
            const uint8_t* e = row + x * 4;
            float* o = out + 3 * ((size_t)y * w + x);
            /* (2^(e - 136): exact, by the exponent's bits) */
            float scale = 0;
            if (e[3]) {
                const uint32_t bits = (uint32_t)artfClampi(e[3] - 136 + 127, 1, 254) << 23;
                memcpy(&scale, &bits, 4);
            }
            o[0] = (float)e[0] * scale;
            o[1] = (float)e[1] * scale;
            o[2] = (float)e[2] * scale;
        }
    }
    artfFree(row);
    *width = w;
    *height = h;
    return out;
}
