/* artful — what comes after a frame in floats, on the CPU (ArtfRendererOptions.hdr): the effects of ArtfPostDesc,
 * then exposure and tone mapping, then the overlay. The GPU's shaders (artfShaderPost*) do the same arithmetic.
 *
 * The tiles leave the frame as planes: light (premultiplied, four planes), depth (1 / w, 0 where nothing is) and a
 * surface for each pixel (packSurface in kernels.inc). In order:
 *
 *   occlusion     how much of the light from all around reaches each pixel, from eight points around it in the
 *                 world, against the depth; blurred over 4 x 4, and taken from the share of light that came from all
 *                 around, which the tiles kept.
 *   reflections   a ray along the mirror direction, through the depth, in 24 steps; where it meets something, what
 *                 the frame shows there in place of the light from all around that the surface reflected, fading
 *                 toward the frame's edges.
 *   temporal      where each pixel was in the frame before, by its depth, and that frame's light there, kept within
 *                 what this frame shows around the pixel.
 *   focus         a disc of 24 samples, as wide as each pixel's circle of confusion.
 *   bloom         light past the threshold, down a pyramid of halves and back up: a wide, soft glow.
 *
 * Then auto exposure (by the mean, or a histogram), and the frame: exposure, the lens flare, the tone curve, and in what
 * a screen shows, grading, the look-up table and grain.
 *
 * Fog that light goes through comes before them all, and before the tiles: a grid of froxels (artfSoftFroxels) that
 * each surface reads as it is lit, layers too, and the background.
 *
 * No exp, pow or sin from the C library: the same bytes on every processor. */
#include "software/softwareInternal.h"
#include "core/threads.h"

#include <string.h>

/* (what runs for every pixel, inlined into its row, whatever the compiler would choose) */
#ifdef _MSC_VER
#define POST_INLINE static __forceinline
#else
#define POST_INLINE static inline __attribute__((always_inline))
#endif

typedef void (*RowFn)(ArtfSoftPost* p, int y);

typedef struct RowJob { ArtfSoftPost* p; RowFn fn; } RowJob;

static void rowJob(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    const RowJob* j = user;
    j->fn(j->p, (int)y);
}

static void rows(ArtfSoftPost* p, int count, RowFn fn) {
    RowJob j = {p, fn};
    artfPoolRun(p->pool, rowJob, &j, (uint32_t)count);
}

/* ---- where a pixel is ---- */

/* Distance along the view, from what the tiles keep (1 / w, or an orthographic view's depth); 0 where nothing is. */
static inline float distanceAt(const ArtfSoftPost* p, int x, int y) {
    const float d = p->f->postDepth[(size_t)y * p->width + x];
    return d > 0 ? artfDepthAlong(&p->f->data, d) : 0.f;
}

/* The world at a pixel's center, d along the view: on the ray from the eye, or in an orthographic view, from its
 * place on the view's plane. */
static inline void worldAt(const ArtfFrameData* f, int width, int height, float x, float y, float d, float* out) {
    const float px = (x + .5f) / (float)width * 2.f - 1.f, py = 1.f - (y + .5f) / (float)height * 2.f;
    if (artfOrtho(f))
        for (int k = 0; k < 3; ++k) out[k] = f->cameraPos[k] + px * f->eye[0][k] + py * f->eye[1][k] + f->eye[2][k] * d;
    else
        for (int k = 0; k < 3; ++k) out[k] = f->cameraPos[k] + (f->eye[2][k] + px * f->eye[0][k] + py * f->eye[1][k]) * d;
}

/* Where a point of the world falls, in pixels, through viewProj (by columns), and w, how far it is along the view;
 * false behind the eye. (Orthographic: w is 1, and how far comes from the depth.) */
static inline bool project(const ArtfFrameData* f, const float* m, const float* p, int width, int height, float* sx, float* sy, float* w) {
    const float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    const float cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    const float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    if (cw <= 1e-5f) return false;
    *sx = (cx / cw * .5f + .5f) * (float)width - .5f;
    *sy = (.5f - cy / cw * .5f) * (float)height - .5f;
    *w = artfOrtho(f) ? f->ortho[1] - (m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]) * f->ortho[0] : cw;
    return true;
}

static inline void normalOf(uint32_t s, float* n) {
    float a = (float)(s & 511) / 511.f * 2.f - 1.f, b = (float)((s >> 9) & 511) / 511.f * 2.f - 1.f;
    float z = 1.f - fabsf(a) - fabsf(b);
    if (z < 0) {
        const float fa = (1.f - fabsf(b)) * (a >= 0 ? 1.f : -1.f);
        b = (1.f - fabsf(a)) * (b >= 0 ? 1.f : -1.f);
        a = fa;
    }
    const float l = sqrtf(a * a + b * b + z * z);
    n[0] = a / l;
    n[1] = b / l;
    n[2] = z / l;
}
static inline float roughOf(uint32_t s) { return (float)((s >> 18) & 31) / 31.f; }
static inline float metalOf(uint32_t s) { return (float)((s >> 23) & 15) / 15.f; }
static inline float aroundOf(uint32_t s) { return (float)(s >> 27) / 31.f; }

static inline uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}

/* Bilinear, in a plane of w x h, at pixel coordinates (centers at whole numbers). */
static inline float tap(const float* plane, int w, int h, float x, float y) {
    x = artfClampf(x, 0, (float)(w - 1));
    y = artfClampf(y, 0, (float)(h - 1));
    const int x0 = (int)x, y0 = (int)y, x1 = artfMini(x0 + 1, w - 1), y1 = artfMini(y0 + 1, h - 1);
    const float fx = x - (float)x0, fy = y - (float)y0;
    const float a = plane[(size_t)y0 * w + x0], b = plane[(size_t)y0 * w + x1], c = plane[(size_t)y1 * w + x0], d = plane[(size_t)y1 * w + x1];
    const float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
    return top + (bottom - top) * fy;
}

/* ---- ambient occlusion ---- */

/* Eight directions in a hemisphere around +z, spread out and closer toward the center: a fixed kernel, turned at each
 * pixel by a pattern of 4 x 4. */
static const float kernel[8][3] = {{.30f, .10f, .25f}, {-.20f, .45f, .35f}, {-.55f, -.15f, .20f}, {.10f, -.60f, .40f},
                                   {.70f, .35f, .45f}, {-.35f, .75f, .30f}, {-.80f, -.40f, .35f}, {.25f, -.85f, .40f}};

static void occlusionRow(ArtfSoftPost* p, int y) {
    const ArtfFrameData* f = &p->f->data;
    const float radius = f->post[0][3];
    for (int x = 0; x < p->width; ++x) {
        const size_t i = (size_t)y * p->width + x;
        const float d = distanceAt(p, x, y);
        const uint32_t s = p->f->postSurface[i];
        if (d <= 0 || !s) { p->occlusion[i] = 1.f; continue; }
        float n[3], at[3];
        normalOf(s, n);
        worldAt(f, p->width, p->height, (float)x, (float)y, d, at);
        /* a frame around the normal, turned by the pattern */
        const float turn = (float)((x & 3) * 4 + (y & 3)) * (6.2831853f / 16.f);
        float t[3] = {fabsf(n[0]) < .9f ? 1.f : 0.f, fabsf(n[0]) < .9f ? 0.f : 1.f, 0};
        const float tn = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
        for (int k = 0; k < 3; ++k) t[k] -= n[k] * tn;
        const float tl = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        for (int k = 0; k < 3; ++k) t[k] /= tl;
        const float b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
        /* (the turn's cosine and sine from a table of sixteen) */
        static const float cosines[16] = {1, .9238795f, .7071068f, .3826834f, 0, -.3826834f, -.7071068f, -.9238795f, -1, -.9238795f, -.7071068f, -.3826834f, 0, .3826834f, .7071068f, .9238795f};
        const int step = (x & 3) * 4 + (y & 3);
        const float c = cosines[step], sn = cosines[(step + 12) & 15];
        (void)turn;
        float hidden = 0;
        for (int k = 0; k < 8; ++k) {
            const float kx = kernel[k][0] * c - kernel[k][1] * sn, ky = kernel[k][0] * sn + kernel[k][1] * c, kz = kernel[k][2];
            float q[3];
            for (int j = 0; j < 3; ++j) q[j] = at[j] + (t[j] * kx + b[j] * ky + n[j] * kz) * radius;
            float sx, sy, w;
            if (!project(f, f->viewProj, q, p->width, p->height, &sx, &sy, &w)) continue;
            const int qx = (int)(sx + .5f), qy = (int)(sy + .5f);
            if (qx < 0 || qy < 0 || qx >= p->width || qy >= p->height) continue;
            const float there = distanceAt(p, qx, qy);
            if (there <= 0) continue;
            /* something is in front of the point, and near enough to shade it */
            const float gap = w - there;
            if (gap > .02f * radius && gap < radius) hidden += 1.f;
        }
        p->occlusion[i] = 1.f - hidden / 8.f;
    }
}

static void occlusionBlurRow(ArtfSoftPost* p, int y) {
    const ArtfFrameData* f = &p->f->data;
    const float strength = f->post[0][2];
    for (int x = 0; x < p->width; ++x) {
        const size_t i = (size_t)y * p->width + x;
        const float d = distanceAt(p, x, y);
        if (d <= 0) continue;
        float sum = 0, weight = 0;
        for (int dy = -2; dy < 2; ++dy)
            for (int dx = -2; dx < 2; ++dx) {
                const int qx = artfClampi(x + dx, 0, p->width - 1), qy = artfClampi(y + dy, 0, p->height - 1);
                const float there = distanceAt(p, qx, qy);
                /* (what is at another depth is another surface) */
                const float w = fabsf(there - d) < .05f * d ? 1.f : 0.f;
                sum += p->occlusion[(size_t)qy * p->width + qx] * w;
                weight += w;
            }
        const float open = weight > 0 ? sum / weight : 1.f;
        const float keep = 1.f - aroundOf(p->f->postSurface[i]) * (1.f - open) * strength;
        for (int c = 0; c < 3; ++c) p->f->postLight[c][i] *= keep;
    }
}

/* ---- reflections ---- */

/* What a reflection found (seen) takes the place of what the surface was lit with from all around in the mirror
 * direction r, as sure as fade says: into bloom's first level, at half. */
static void reflectionUse(ArtfSoftPost* p, const ArtfFrameData* f, size_t i, size_t half, uint32_t s, float rough, float vn, const float* r, const float* seen, float fade, float strength) {
    const float smooth = 1.f - rough / .6f, metal = metalOf(s);
    /* as the surface was lit from all around in the mirror direction (kernels.inc's highlight) */
    const float f0 = .04f + .96f * metal, c5 = 1.f - artfMaxf(-vn, 1e-4f), fresnel = c5 * c5 * c5 * c5 * c5;
    const float mirror = f0 + (artfMaxf(1.f - rough, f0) - f0) * fresnel, sure = smooth * smooth * fade * strength;
    float around[3];
    const float* environment = f->sky[3] > 0 ? p->f->scene->environment : NULL;
    if (environment) {
        artfEnvReflected(environment, rough, r[0], r[1], r[2], around);
        for (int c = 0; c < 3; ++c) around[c] *= f->sky[3];
    } else {
        const float up = r[1] * .5f + .5f;
        for (int c = 0; c < 3; ++c) around[c] = f->ground[c] + (f->sky[c] - f->ground[c]) * up;
    }
    /* a metal's reflection is tinted by its color: that of the light it shows, as near as is known */
    float tint[3] = {1, 1, 1};
    if (metal > 0) {
        const float sum = p->f->postLight[0][i] + p->f->postLight[1][i] + p->f->postLight[2][i];
        for (int c = 0; c < 3; ++c) tint[c] = 1.f + metal * (sum > 1e-6f ? p->f->postLight[c][i] * 3.f / sum - 1.f : 0.f);
    }
    for (int c = 0; c < 3; ++c) p->bloom[c][half] = (seen[c] - around[c]) * tint[c] * mirror * sure;
}

/* At half the frame's resolution, a pixel for each two by two: what the march found takes the place of what the surface
 * was lit with from all around in the mirror direction (the picture, or the sky's two colors), which hid it. What that
 * changes goes into bloom's first level, before bloom needs it; reflectionApplyRow takes it up. Where the march finds
 * nothing, the light from all around stays as the surface was lit. */
static void reflectionRow(ArtfSoftPost* p, int hy) {
    const ArtfFrameData* f = &p->f->data;
    const float strength = f->post[1][0];
    const int hw = p->bloomW[0];
    for (int hx = 0; hx < hw; ++hx) {
        const int x = artfMini(hx * 2, p->width - 1), y = artfMini(hy * 2, p->height - 1);
        const size_t i = (size_t)y * p->width + x, half = (size_t)hy * hw + hx;
        for (int c = 0; c < 3; ++c) p->bloom[c][half] = 0;
        const uint32_t s = p->f->postSurface[i];
        const float d = distanceAt(p, x, y), rough = roughOf(s);
        if (d <= 0 || !s || rough > .6f) continue;
        float n[3], at[3], v[3];
        normalOf(s, n);
        worldAt(f, p->width, p->height, (float)x, (float)y, d, at);
        float eye[3];
        artfEyeOf(f, at, eye);
        for (int k = 0; k < 3; ++k) v[k] = at[k] - eye[k];
        const float vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int k = 0; k < 3; ++k) v[k] /= vl;
        const float vn = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
        float r[3];
        for (int k = 0; k < 3; ++k) r[k] = v[k] - 2.f * vn * n[k];
        /* toward the eye there is nothing in the frame to see */
        const float away = r[0] * f->eye[2][0] + r[1] * f->eye[2][1] + r[2] * f->eye[2][2];
        if (f->traced & ARTF_TRACED_REFLECTIONS) {
            /* from rays: what the scene shows that way, off the frame too, toward the eye as well */
            float seen[3];
            const uint32_t point = ((uint32_t)hx & 3u) + ((uint32_t)hy & 3u) * 4u + f->tracedFrame * 5u;
            if (!artfTracedReflection(p->f->scene, f, at, n, v, rough, point, seen)) continue;
            reflectionUse(p, f, i, half, s, rough, vn, r, seen, 1.f, strength);
            continue;
        }
        if (away < -.2f) continue;
        const float stride = d * .04f + .02f, thickness = d * .03f + .05f;
        /* the ray from here to as far as 24 steps reach, across the frame: a line, along which 1 / w goes evenly */
        float sx0, sy0, w0, sx1, sy1, w1, reach = stride * 70.f, end[3];
        if (!project(f, f->viewProj, at, p->width, p->height, &sx0, &sy0, &w0)) continue;
        const float wDrop = -(r[0] * f->eye[2][0] + r[1] * f->eye[2][1] + r[2] * f->eye[2][2]);
        /* (not past the eye; orthographic: not past near) */
        const bool ortho = artfOrtho(f);
        if (wDrop > 0) reach = artfMinf(reach, (ortho ? w0 - (f->ortho[1] - f->ortho[0]) : w0) * .9f / wDrop);
        for (int k = 0; k < 3; ++k) end[k] = at[k] + r[k] * reach;
        if (!project(f, f->viewProj, end, p->width, p->height, &sx1, &sy1, &w1)) continue;
        float hx2 = 0, hy2 = 0, found = 0;
        bool before = true; /* the last step was in front of what the frame shows */
        const float jitter = (float)(hash((uint32_t)(y * p->width + x)) & 255) / 256.f;
        for (int step = 1; step <= 24 && !found; ++step) {
            /* (closer together near the start) */
            const float share = ((float)step + jitter) / 25.f, t = share * share;
            const float sx = sx0 + (sx1 - sx0) * t, sy = sy0 + (sy1 - sy0) * t;
            /* (orthographic: w itself goes evenly) */
            const float w = ortho ? w0 + (w1 - w0) * t : 1.f / (1.f / w0 + (1.f / w1 - 1.f / w0) * t);
            const int qx = (int)(sx + .5f), qy = (int)(sy + .5f);
            if (qx < 0 || qy < 0 || qx >= p->width || qy >= p->height) break;
            const float there = distanceAt(p, qx, qy);
            /* a hit: from in front to behind, near enough to be the same surface, and not where it started */
            const bool behind = there > 0 && w > there;
            const float moved = fabsf(sx - (float)x) + fabsf(sy - (float)y);
            if (behind && before && w - there < thickness && moved > 2.f) {
                hx2 = sx;
                hy2 = sy;
                found = 1.f;
            }
            before = !behind;
        }
        if (!found) continue;
        /* fading where the ray left near the frame's edges, and as it grows rough */
        const float edge = artfMinf(artfMinf(hx2, (float)p->width - 1 - hx2), artfMinf(hy2, (float)p->height - 1 - hy2));
        const float fade = artfClampf(edge / ((float)p->width * .08f), 0, 1);
        float seen[3];
        for (int c = 0; c < 3; ++c) seen[c] = tap(p->f->postLight[c], p->width, p->height, hx2, hy2);
        reflectionUse(p, f, i, half, s, rough, vn, r, seen, fade, strength);
    }
}

static void reflectionApplyRow(ArtfSoftPost* p, int y) {
    const int hw = p->bloomW[0], hh = p->bloomH[0];
    for (int x = 0; x < p->width; ++x) {
        const size_t i = (size_t)y * p->width + x;
        const float fx = ((float)x + .5f) * .5f - .5f, fy = ((float)y + .5f) * .5f - .5f;
        for (int c = 0; c < 3; ++c) p->f->postLight[c][i] = artfMaxf(p->f->postLight[c][i] + tap(p->bloom[c], hw, hh, fx, fy), 0.f);
    }
}

/* ---- fog that light goes through: froxels ---- */

/* (1 + x + x^2 / 2)^-1, for e^-x: as the fog drawn with the frame */
static inline float through(float x) { return 1.f / (1.f + x + x * x * .5f); }

/* A row of the grid's columns: from the eye out through each slice, the sun through its shadows and what the sky gives,
 * less what the air takes. */
static void froxelRow(ArtfSoftPost* p, int j) {
    const ArtfFrameData* f = &p->f->data;
    const float density = f->look[1], amount = f->post[1][1], g = f->post[1][2], far = 4.f / artfMaxf(density, 1e-6f);
    for (int i = 0; i < p->froxelsX; ++i) {
        float dir[3], eye[3];
        const float cx = (float)(i * ARTF_FROXEL_TILE) + ARTF_FROXEL_TILE * .5f - .5f, cy = (float)(j * ARTF_FROXEL_TILE) + ARTF_FROXEL_TILE * .5f - .5f;
        /* (orthographic: each column starts from its place on the view's plane, and goes the way the view looks) */
        worldAt(f, p->width, p->height, cx, cy, 0.f, eye);
        if (!artfOrtho(f)) memcpy(eye, f->cameraPos, 12);
        worldAt(f, p->width, p->height, cx, cy, 1.f, dir);
        for (int k = 0; k < 3; ++k) dir[k] -= eye[k];
        const float along = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        /* Henyey-Greenstein, toward the sun */
        const float mu = (dir[0] * f->toSun[0] + dir[1] * f->toSun[1] + dir[2] * f->toSun[2]) / along;
        const float hg = 1.f + g * g - 2.f * g * mu, phase = (1.f - g * g) / (12.566371f * hg * sqrtf(hg));
        float light[3] = {0, 0, 0}, left = 1.f;
        float* out = p->froxels + ((size_t)j * p->froxelsX + i) * ARTF_FROXEL_SLICES * 4;
        for (int k = 0; k < ARTF_FROXEL_SLICES; ++k) {
            const float a = (float)k / ARTF_FROXEL_SLICES, b = (float)(k + 1) / ARTF_FROXEL_SLICES;
            const float d0 = far * a * a, d1 = far * b * b, t = (d0 + d1) * .5f;
            float q[3];
            for (int c = 0; c < 3; ++c) q[c] = eye[c] + dir[c] * t;
            const float sun = p->f->shadows ? artfShadowOfSun(p->f->shadows, eye, q, f->toSun, 0) : 1.f;
            const float stepThrough = through(density * artfFogHeight(f, q[1]) * (d1 - d0) * along), taken = left * (1.f - stepThrough);
            for (int c = 0; c < 3; ++c) light[c] += taken * (f->sunColor[c] * sun * phase * 12.566371f + f->fog[c]);
            left *= stepThrough;
            float* cell = out + (size_t)k * 4;
            for (int c = 0; c < 3; ++c) cell[c] = light[c] * amount;
            cell[3] = 1.f - (1.f - left) * amount;
        }
    }
}

void artfSoftFroxels(ArtfSoftPost* p, ArtfSoftFrame* f) {
    p->f = f;
    f->froxels = p->froxels;
    f->froxelsX = p->froxelsX;
    f->froxelsY = p->froxelsY;
    rows(p, p->froxelsY, froxelRow);
}

void artfFroxelAt(const ArtfSoftFrame* f, float sx, float sy, float w, float* out) {
    const ArtfFrameData* d = &f->data;
    const int gx = f->froxelsX, gy = f->froxelsY;
    const float far = 4.f / artfMaxf(d->look[1], 1e-6f);
    const float fx = artfClampf((sx - (ARTF_FROXEL_TILE * .5f - .5f)) / ARTF_FROXEL_TILE, 0, (float)(gx - 1));
    const float fy = artfClampf((sy - (ARTF_FROXEL_TILE * .5f - .5f)) / ARTF_FROXEL_TILE, 0, (float)(gy - 1));
    const float fz = artfClampf(sqrtf(artfMaxf(w, 0) / far) * ARTF_FROXEL_SLICES - 1.f, -1.f, ARTF_FROXEL_SLICES - 1);
    const int x0 = (int)fx, y0 = (int)fy, x1 = artfMini(x0 + 1, gx - 1), y1 = artfMini(y0 + 1, gy - 1);
    const int z0 = fz < 0 ? -1 : (int)fz, z1 = artfMini(z0 + 1, ARTF_FROXEL_SLICES - 1);
    const float tx = fx - (float)x0, ty = fy - (float)y0, tz = fz - (float)z0;
    /* (the four columns around it: each holds its slices one after the other) */
    const size_t column = ARTF_FROXEL_SLICES * 4;
    const size_t a = ((size_t)y0 * gx + x0) * column, b = ((size_t)y0 * gx + x1) * column, c = ((size_t)y1 * gx + x0) * column, e = ((size_t)y1 * gx + x1) * column;
    float near_[4] = {0, 0, 0, 1}, beyond[4];
    const float* s1 = f->froxels + (size_t)z1 * 4;
    for (int k = 0; k < 4; ++k) {
        const float top = s1[a + k] + (s1[b + k] - s1[a + k]) * tx, bottom = s1[c + k] + (s1[e + k] - s1[c + k]) * tx;
        beyond[k] = top + (bottom - top) * ty;
    }
    if (z0 >= 0) {
        const float* s0 = f->froxels + (size_t)z0 * 4;
        for (int k = 0; k < 4; ++k) {
            const float top = s0[a + k] + (s0[b + k] - s0[a + k]) * tx, bottom = s0[c + k] + (s0[e + k] - s0[c + k]) * tx;
            near_[k] = top + (bottom - top) * ty;
        }
    } /* (else at the eye: nothing added, all left) */
    for (int k = 0; k < 4; ++k) out[k] = near_[k] + (beyond[k] - near_[k]) * tz;
}

/* ---- temporal antialiasing ---- */

/* Where the surface at a pixel was in the frame before, in pixels: by its depth and how its instance moved, or for
 * what is far, by the view alone. false: it was behind the eye. */
POST_INLINE bool wasAt(const ArtfSoftPost* p, int x, int y, float* sx, float* sy) {
    const ArtfFrameData* f = &p->f->data;
    const float d = distanceAt(p, x, y);
    float at[3], w;
    worldAt(f, p->width, p->height, (float)x - f->post[2][2], (float)y + f->post[2][3], d > 0 ? d : 1e4f, at);
    const uint32_t who = d > 0 && p->f->motion ? p->f->postInstance[(size_t)y * p->width + x] : 0;
    if (who) {
        const float* m = p->f->motion + (size_t)(who - 1) * 12;
        float was[3];
        for (int r = 0; r < 3; ++r) was[r] = m[r * 4] * at[0] + m[r * 4 + 1] * at[1] + m[r * 4 + 2] * at[2] + m[r * 4 + 3];
        memcpy(at, was, sizeof was);
    }
    return project(f, f->previous, at, p->width, p->height, sx, sy, &w);
}

static void temporalRow(ArtfSoftPost* p, int y) {
    for (int x = 0; x < p->width; ++x) {
        const size_t i = (size_t)y * p->width + x;
        float lo[3] = {3e38f, 3e38f, 3e38f}, hi[3] = {-3e38f, -3e38f, -3e38f};
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const size_t q = (size_t)artfClampi(y + dy, 0, p->height - 1) * p->width + artfClampi(x + dx, 0, p->width - 1);
                for (int c = 0; c < 3; ++c) {
                    lo[c] = artfMinf(lo[c], p->f->postLight[c][q]);
                    hi[c] = artfMaxf(hi[c], p->f->postLight[c][q]);
                }
            }
        /* where this pixel was */
        float sx, sy;
        float share = .1f;
        if (!p->historyValid || !wasAt(p, x, y, &sx, &sy) || sx < 0 || sy < 0 || sx > p->width - 1 || sy > p->height - 1) share = 1.f;
        for (int c = 0; c < 3; ++c) {
            const float now = p->f->postLight[c][i];
            const float was = share < 1.f ? artfClampf(tap(p->history[c], p->width, p->height, sx, sy), lo[c], hi[c]) : now;
            p->scratch[c][i] = was + (now - was) * share;
        }
    }
}

/* ---- motion blur: eight points along the way each pixel went, as long as the shutter was open (at most 32 pixels) ---- */

static void blurRow(ArtfSoftPost* p, int y) {
    const float shutter = p->f->data.motionBlur;
    for (int x = 0; x < p->width; ++x) {
        const size_t i = (size_t)y * p->width + x;
        float sx, sy, vx = 0, vy = 0;
        if (wasAt(p, x, y, &sx, &sy)) {
            vx = ((float)x - sx) * shutter;
            vy = ((float)y - sy) * shutter;
            const float length = vx * vx + vy * vy;
            if (length > 32.f * 32.f) {
                const float scale = 32.f / sqrtf(length);
                vx *= scale;
                vy *= scale;
            }
        }
        if (vx * vx + vy * vy < .25f) {
            for (int c = 0; c < 3; ++c) p->scratch[c][i] = p->f->postLight[c][i];
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            float sum = 0;
            for (int k = 0; k < 8; ++k) {
                const float along = ((float)k + .5f) * .125f - .5f;
                sum += tap(p->f->postLight[c], p->width, p->height, (float)x + vx * along, (float)y + vy * along);
            }
            p->scratch[c][i] = sum * .125f;
        }
    }
}

/* ---- depth of field ---- */

/* 24 points of a disc, in rings. */
static const float disc[24][2] = {{.33f, 0}, {-.17f, .29f}, {-.17f, -.29f}, {.62f, .17f}, {.17f, .62f}, {-.45f, .45f}, {-.62f, -.17f}, {-.17f, -.62f}, {.45f, -.45f},
                                  {.95f, 0}, {.82f, .48f}, {.48f, .82f}, {0, .95f}, {-.48f, .82f}, {-.82f, .48f}, {-.95f, 0}, {-.82f, -.48f}, {-.48f, -.82f},
                                  {0, -.95f}, {.48f, -.82f}, {.82f, -.48f}, {.3f, .3f}, {-.3f, .1f}, {.1f, -.3f}};

static inline float confusion(const ArtfFrameData* f, float d) {
    if (d <= 0) d = 1e6f;
    return artfMinf(f->post[2][1] * fabsf(1.f - f->post[2][0] / d), 24.f);
}

static void focusRow(ArtfSoftPost* p, int y) {
    const ArtfFrameData* f = &p->f->data;
    for (int x = 0; x < p->width; ++x) {
        const size_t i = (size_t)y * p->width + x;
        const float mine = confusion(f, distanceAt(p, x, y));
        if (mine < .5f) {
            for (int c = 0; c < 3; ++c) p->scratch[c][i] = p->f->postLight[c][i];
            continue;
        }
        float sum[3] = {p->f->postLight[0][i], p->f->postLight[1][i], p->f->postLight[2][i]}, weight = 1.f;
        for (int k = 0; k < 24; ++k) {
            const float sx = (float)x + disc[k][0] * mine, sy = (float)y + disc[k][1] * mine;
            const int qx = artfClampi((int)(sx + .5f), 0, p->width - 1), qy = artfClampi((int)(sy + .5f), 0, p->height - 1);
            /* what is sharper and behind does not spread over this pixel */
            const float theirs = confusion(f, distanceAt(p, qx, qy));
            const float distance = sqrtf(disc[k][0] * disc[k][0] + disc[k][1] * disc[k][1]) * mine;
            const float w = theirs >= distance ? 1.f : artfClampf(theirs / artfMaxf(distance, 1e-3f), 0, 1);
            const size_t q = (size_t)qy * p->width + qx;
            for (int c = 0; c < 3; ++c) sum[c] += p->f->postLight[c][q] * w;
            weight += w;
        }
        for (int c = 0; c < 3; ++c) p->scratch[c][i] = sum[c] / weight;
    }
}

/* ---- bloom ---- */

/* ---- auto exposure ---- */

/* The exposure the frame is shown at: the scene's (with auto exposure, or 1), times what auto exposure got to. */
static inline float exposureOf(const ArtfSoftPost* p) {
    const ArtfFrameData* f = &p->f->data;
    if (f->grade[0][1] > 0) return (f->look[0] > 0 ? f->look[0] : 1.f) * p->exposureScale;
    return f->look[0];
}

/* What the meter reads at a point of the frame (u, v from 0 to 1): its luminance in stops, and how much it counts, the
 * middle weighed twice the corners and each by what covers it (0: nothing does). */
static inline float meterPoint(const ArtfSoftPost* p, float u, float v, float* stops) {
    const size_t i = (size_t)(int)(v * (float)p->height) * p->width + (size_t)(int)(u * (float)p->width);
    const float alpha = artfClampf(p->f->postLight[3][i], 0, 1);
    if (!(alpha > 0)) return 0.f;
    const float luminance = (.2126f * p->f->postLight[0][i] + .7152f * p->f->postLight[1][i] + .0722f * p->f->postLight[2][i]) / alpha;
    const float du = u * 2.f - 1.f, dv = v * 2.f - 1.f;
    *stops = artfLog2Fast(artfClampf(luminance, 1.f / 4096.f, 4096.f));
    return alpha * (1.f - .25f * (du * du + dv * dv));
}

/* The frame's mean in stops, on a grid of points; the exposure goes a share of the way to bringing it to the key (one
 * share up, another down). With a histogram, the grid is denser, and its darkest and brightest shares are left out of
 * the mean, as Unreal's is. One thread: 576 points, or 2304. */
static void meter(ArtfSoftPost* p) {
    const ArtfFrameData* f = &p->f->data;
    float sum = 0, weights = 0;
    if (!(f->grade2[6][3] > 0)) {
        for (int gy = 0; gy < ARTF_METER_Y; ++gy)
            for (int gx = 0; gx < ARTF_METER_X; ++gx) {
                const float u = ((float)gx + .5f) / (float)ARTF_METER_X, v = ((float)gy + .5f) / (float)ARTF_METER_Y;
                float stops = 0;
                const float weight = meterPoint(p, u, v, &stops);
                if (!(weight > 0)) continue;
                sum += weight * stops;
                weights += weight;
            }
    } else {
        /* (each bin keeps its weight, and its weight times stops, as whole numbers of 1 / 65536 and 1 / 4096: sums that
         * do not hang on their order, as the GPU's 64 invocations add them at once; what is left in a bin is then its
         * own mean) */
        uint32_t binWeight[ARTF_HISTOGRAM_BINS], binStops[ARTF_HISTOGRAM_BINS], total = 0;
        for (int b = 0; b < ARTF_HISTOGRAM_BINS; ++b) binWeight[b] = binStops[b] = 0;
        for (int gy = 0; gy < ARTF_HISTOGRAM_Y; ++gy)
            for (int gx = 0; gx < ARTF_HISTOGRAM_X; ++gx) {
                const float u = ((float)gx + .5f) / (float)ARTF_HISTOGRAM_X, v = ((float)gy + .5f) / (float)ARTF_HISTOGRAM_Y;
                float stops = 0;
                const float weight = meterPoint(p, u, v, &stops);
                const int b = artfMini((int)(artfMaxf(stops + 12.f, 0) * ((float)ARTF_HISTOGRAM_BINS / 24.f)), ARTF_HISTOGRAM_BINS - 1);
                binWeight[b] += (uint32_t)(weight * 65536.f + .5f);
                binStops[b] += (uint32_t)(int32_t)floorf(weight * stops * 4096.f + .5f);
            }
        for (int b = 0; b < ARTF_HISTOGRAM_BINS; ++b) total += binWeight[b];
        const float all = (float)total * (1.f / 65536.f);
        const float from = all * f->grade2[6][0], to = all * (1.f - f->grade2[6][1]);
        float before = 0;
        for (int b = 0; b < ARTF_HISTOGRAM_BINS; ++b) {
            const float w = (float)binWeight[b] * (1.f / 65536.f), st = (float)(int32_t)binStops[b] * (1.f / 4096.f);
            const float kept = artfMinf(before + w, to) - artfMaxf(before, from);
            if (kept > 0 && w > 0) {
                sum += st * (kept / w);
                weights += kept;
            }
            before += w;
        }
    }
    float target = weights > 0 ? f->grade[0][2] - sum / weights : p->exposureValid ? p->exposureLog : 0.f;
    target = artfClampf(target, f->grade[1][0], f->grade[1][1]);
    const float speed = target < p->exposureLog ? f->grade2[6][2] : f->grade[0][3];
    p->exposureLog = p->exposureValid ? p->exposureLog + (target - p->exposureLog) * speed : target;
    p->exposureScale = artfExp2Fast(p->exposureLog);
    p->exposureValid = true;
}

static void bloomDownRow(ArtfSoftPost* p, int y) {
    const int level = p->bloomLevel;
    const int w = p->bloomW[level], h = p->bloomH[level];
    (void)h;
    const float shown = exposureOf(p), threshold = p->f->data.post[0][1], exposure = shown > 0 ? shown : 1.f;
    for (int x = 0; x < w; ++x) {
        float sum[3] = {0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            /* (four bilinear taps around the center of the 2 x 2 above: a 4 x 4 tent) */
            const float sx = (float)x * 2.f + .5f + (k & 1 ? .5f : -.5f), sy = (float)y * 2.f + .5f + (k & 2 ? .5f : -.5f);
            if (!level) {
                float c[3];
                for (int ch = 0; ch < 3; ++ch) c[ch] = tap(p->f->postLight[ch], p->width, p->height, sx, sy) * exposure;
                const float most = artfMaxf(c[0], artfMaxf(c[1], c[2]));
                const float past = most > threshold ? (most - threshold) / most : 0.f;
                for (int ch = 0; ch < 3; ++ch) sum[ch] += c[ch] * past;
            } else {
                for (int ch = 0; ch < 3; ++ch) sum[ch] += tap(p->bloom[ch] + p->bloomAt[level - 1], p->bloomW[level - 1], p->bloomH[level - 1], sx, sy);
            }
        }
        for (int ch = 0; ch < 3; ++ch) p->bloom[ch][p->bloomAt[level] + (size_t)y * w + x] = sum[ch] * .25f;
    }
}

static void bloomUpRow(ArtfSoftPost* p, int y) {
    const int level = p->bloomLevel;
    const int w = p->bloomW[level];
    for (int x = 0; x < w; ++x) {
        const float sx = ((float)x + .5f) * .5f - .5f, sy = ((float)y + .5f) * .5f - .5f;
        for (int ch = 0; ch < 3; ++ch) p->bloom[ch][p->bloomAt[level] + (size_t)y * w + x] += tap(p->bloom[ch] + p->bloomAt[level + 1], p->bloomW[level + 1], p->bloomH[level + 1], sx, sy);
    }
}

/* ---- the frame: exposure, tone mapping, the overlay ---- */

/* Khronos's PBR Neutral: what is under its start stays as it is, past it the peak rolls off to 1, toward white. */
static inline void toneNeutral(float* c) {
    const float start = .76f, desaturation = .15f;
    const float least = artfMinf(c[0], artfMinf(c[1], c[2]));
    const float offset = least < .08f ? least - 6.25f * least * least : .04f;
    for (int k = 0; k < 3; ++k) c[k] -= offset;
    const float peak = artfMaxf(c[0], artfMaxf(c[1], c[2]));
    if (peak < start) return;
    const float d = 1.f - start, top = 1.f - d * d / (peak + d - start);
    const float g = 1.f - 1.f / (desaturation * (peak - top) + 1.f);
    for (int k = 0; k < 3; ++k) {
        c[k] *= top / peak;
        c[k] += (top - c[k]) * g;
    }
}

/* Lens flare: what blooms, again at three places mirrored across the middle, each its own color, fading as it goes from
 * the middle to the frame's edges (and gone past them). dx, dy: from the middle to the pixel's center. */
static const float flareScale[3] = {1.f, .5f, 1.8f};
static const float flareTint[3][3] = {{1.f, .7f, .4f}, {.4f, .7f, 1.f}, {.7f, 1.f, .6f}};

static inline void flareAt(const ArtfSoftPost* p, float dx, float dy, float* out) {
    const float cx = (float)p->width * .5f, cy = (float)p->height * .5f;
    for (int c = 0; c < 3; ++c) out[c] = 0;
    for (int g = 0; g < 3; ++g) {
        const float k = flareScale[g];
        const float ax = dx * k / cx, ay = dy * k / cy;
        const float fade = artfMaxf(1.f - ax * ax, 0) * artfMaxf(1.f - ay * ay, 0);
        const float gx = (cx - dx * k) * .5f - .5f, gy = (cy - dy * k) * .5f - .5f;
        /* (tap's arithmetic, for the three planes at once) */
        const int w = p->bloomW[0], h = p->bloomH[0];
        const float x = artfClampf(gx, 0, (float)(w - 1)), y = artfClampf(gy, 0, (float)(h - 1));
        const int x0 = (int)x, y0 = (int)y, x1 = artfMini(x0 + 1, w - 1), y1 = artfMini(y0 + 1, h - 1);
        const float fx = x - (float)x0, fy = y - (float)y0;
        const size_t i00 = (size_t)y0 * w + x0, i01 = (size_t)y0 * w + x1, i10 = (size_t)y1 * w + x0, i11 = (size_t)y1 * w + x1;
        for (int c = 0; c < 3; ++c) {
            const float* plane = p->bloom[c];
            const float top = plane[i00] + (plane[i01] - plane[i00]) * fx, bottom = plane[i10] + (plane[i11] - plane[i10]) * fx;
            const float tinted = flareTint[g][c] * fade;
            out[c] += tinted * (top + (bottom - top) * fy);
        }
    }
}

/* What is done in what a screen shows, last: lift, gamma and gain, the color wheels, the look-up table, the grain. */
static inline void gradeShown(const ArtfSoftPost* p, const ArtfFrameData* f, float* c, int x, int y) {
    if (f->grade2[0][3] > 0) {
        for (int k = 0; k < 3; ++k) {
            const float o = f->grade2[2][k] * (c[k] + f->grade2[0][k] * (1.f - c[k]));
            c[k] = f->grade2[1][k] == 1 ? o : o > 0 ? artfExp2Fast(artfLog2Fast(o) * f->grade2[1][k]) : 0.f;
        }
        const float l = artfClampf(.2126f * c[0] + .7152f * c[1] + .0722f * c[2], 0, 1);
        const float dark = (1.f - l) * (1.f - l), middle = 2.f * l * (1.f - l), bright = l * l;
        for (int k = 0; k < 3; ++k) c[k] = artfClampf(c[k] * (dark * f->grade2[3][k] + middle * f->grade2[4][k] + bright * f->grade2[5][k]), 0, 1);
    }
    const int n = (int)f->grade2[3][3];
    if (n >= 2 && p->f->scene->lut) {
        const float* lut = p->f->scene->lut;
        int at[3];
        float t[3];
        for (int k = 0; k < 3; ++k) {
            const float q = artfClampf(c[k], 0, 1) * (float)(n - 1);
            at[k] = artfMini((int)q, n - 2);
            t[k] = q - (float)at[k];
        }
        const size_t i000 = (size_t)3 * (((size_t)at[2] * n + at[1]) * n + at[0]), dy = (size_t)3 * n, dz = (size_t)3 * n * n;
        const float amount = f->grade2[4][3];
        for (int k = 0; k < 3; ++k) {
            const float* q = lut + i000 + k;
            const float a = q[0] + (q[3] - q[0]) * t[0], b = q[dy] + (q[dy + 3] - q[dy]) * t[0];
            const float d = q[dz] + (q[dz + 3] - q[dz]) * t[0], e = q[dz + dy] + (q[dz + dy + 3] - q[dz + dy]) * t[0];
            const float near_ = a + (b - a) * t[1], far = d + (e - d) * t[1];
            const float looked = near_ + (far - near_) * t[2];
            c[k] = artfClampf(c[k] + (looked - c[k]) * amount, 0, 1);
        }
    }
    const float grain = f->grade2[1][3];
    if (grain > 0) {
        const uint32_t h = hash((uint32_t)y * (uint32_t)p->width + (uint32_t)x + hash((uint32_t)f->grade2[2][3]));
        const float noise = (float)(h >> 8) * (1.f / 16777216.f) - .5f;
        for (int k = 0; k < 3; ++k) c[k] = artfClampf(c[k] + noise * grain * (.0625f + .75f * c[k] * (1.f - c[k])), 0, 1);
    }
}

static void finalRow(ArtfSoftPost* p, int y) {
    const ArtfSoftFrame* sf = p->f;
    const ArtfFrameData* f = &sf->data;
    const float exposure = exposureOf(p), bloom = f->post[0][0];
    const int bw = p->bloomW[0], bh = p->bloomH[0];
    const int curve = (int)f->grade[0][0];
    const float saturation = f->grade[1][2], contrast = f->grade[1][3];
    const float temperature = f->grade[2][0], tint = f->grade[2][1], vignette = f->grade[2][2], chromatic = f->grade[2][3];
    const float balance[3] = {1.f + .2f * temperature, 1.f - .2f * tint, 1.f - .2f * temperature};
    const bool balanced = temperature != 0 || tint != 0;
    const bool graded = f->grade2[0][3] > 0 || f->grade2[3][3] > 0 || f->grade2[1][3] > 0;
    const float flare = f->grade2[5][3];
    /* (from the middle, as a share of the way to a corner) */
    const float cx = (float)p->width * .5f, cy = (float)p->height * .5f, corner = 1.f / (cx * cx + cy * cy);
    const float apart = chromatic * sqrtf(corner), dy = (float)y + .5f - cy;
    for (int x = 0; x < p->width; ++x) {
        const size_t i = (size_t)y * p->width + x;
        const float alpha = artfClampf(sf->postLight[3][i], 0, 1);
        const float dx = (float)x + .5f - cx;
        uint32_t out = (uint32_t)(alpha * 255.f + .5f) << 24;
        float v[3];
        for (int c = 0; c < 3; ++c) v[c] = sf->postLight[c][i];
        if (chromatic > 0) {
            v[0] = tap(sf->postLight[0], p->width, p->height, (float)x + dx * apart, (float)y + dy * apart);
            v[2] = tap(sf->postLight[2], p->width, p->height, (float)x - dx * apart, (float)y - dy * apart);
        }
        for (int c = 0; c < 3; ++c) {
            if (alpha > 0) v[c] /= alpha;
            if (exposure > 0) v[c] *= exposure;
            if (bloom > 0) v[c] += tap(p->bloom[c], bw, bh, ((float)x + .5f) * .5f - .5f, ((float)y + .5f) * .5f - .5f) * bloom / artfMaxf(alpha, 1e-3f);
        }
        if (flare > 0) {
            float ghosts[3];
            flareAt(p, dx, dy, ghosts);
            for (int c = 0; c < 3; ++c) v[c] += ghosts[c] * flare / artfMaxf(alpha, 1e-3f);
        }
        if (balanced)
            for (int c = 0; c < 3; ++c) v[c] *= balance[c];
        if (saturation != 0) {
            const float grey = .2126f * v[0] + .7152f * v[1] + .0722f * v[2];
            for (int c = 0; c < 3; ++c) v[c] = grey + (v[c] - grey) * (1.f + saturation);
        }
        if (vignette > 0) {
            const float q = 1.f + vignette * (dx * dx + dy * dy) * corner, keep = 1.f / (q * q);
            for (int c = 0; c < 3; ++c) v[c] *= keep;
        }
        for (int c = 0; c < 3; ++c) v[c] = v[c] > 0 ? artfMinf(v[c], 65504.f) : 0.f; /* (and not a number: nothing) */
        if (exposure > 0) {
            if (curve == ARTF_TONE_NEUTRAL) toneNeutral(v);
            for (int c = 0; c < 3; ++c) {
                if (curve == ARTF_TONE_ACES) v[c] = v[c] * (v[c] * 2.51f + .03f) / (v[c] * (v[c] * 2.43f + .59f) + .14f);
                else if (curve == ARTF_TONE_REINHARD) v[c] = v[c] / (1.f + v[c]);
            }
        }
        float shown[3];
        for (int c = 0; c < 3; ++c) {
            shown[c] = sqrtf(artfClampf(v[c], 0, 1));
            if (contrast != 0) shown[c] = artfClampf(.5f + (shown[c] - .5f) * (1.f + contrast), 0, 1);
        }
        if (graded) gradeShown(p, f, shown, x, y);
        for (int c = 0; c < 3; ++c) out |= (uint32_t)(shown[c] * alpha * 255.f + .5f) << (16 - 8 * c);
        if (sf->overlay && x < sf->overlayWidth && y < sf->overlayHeight) {
            const uint32_t s = sf->overlay[(size_t)y * sf->overlayStride + x];
            if (s) {
                const uint32_t rest = 255 - (s >> 24);
                uint32_t blended = 0;
                for (int c = 0; c < 32; c += 8) {
                    uint32_t t = ((out >> c) & 255) * rest + 128;
                    blended |= ((t + (t >> 8)) >> 8) << c;
                }
                out = s + blended;
            }
        }
        p->pixels[i] = out;
    }
}

static void copyScratch(ArtfSoftPost* p) {
    for (int c = 0; c < 3; ++c) {
        float* t = p->f->postLight[c];
        p->f->postLight[c] = p->scratch[c];
        p->scratch[c] = t;
    }
}

void artfSoftPostSize(ArtfSoftPost* p, int width, int height) {
    if (p->width == width && p->height == height) return;
    artfSoftPostFree(p);
    p->width = width;
    p->height = height;
    const size_t pixels = (size_t)width * height;
    for (int c = 0; c < 3; ++c) {
        p->scratch[c] = artfMalloc(pixels * 4);
        p->history[c] = artfMalloc(pixels * 4);
    }
    p->occlusion = artfMalloc(pixels * 4);
    p->froxelsX = (width + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE;
    p->froxelsY = (height + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE;
    p->froxels = artfMalloc((size_t)p->froxelsX * p->froxelsY * ARTF_FROXEL_SLICES * 16);
    size_t at = 0;
    int w = width, h = height;
    for (int l = 0; l < ARTF_POST_BLOOM_LEVELS; ++l) {
        w = artfMaxi((w + 1) / 2, 1);
        h = artfMaxi((h + 1) / 2, 1);
        p->bloomW[l] = w;
        p->bloomH[l] = h;
        p->bloomAt[l] = at;
        at += (size_t)w * h;
    }
    for (int c = 0; c < 3; ++c) p->bloom[c] = artfCalloc(at + 1, 4);
    p->historyValid = false;
}

void artfSoftPostFree(ArtfSoftPost* p) {
    for (int c = 0; c < 3; ++c) {
        artfFree(p->scratch[c]);
        artfFree(p->history[c]);
        artfFree(p->bloom[c]);
        p->scratch[c] = p->history[c] = p->bloom[c] = NULL;
    }
    artfFree(p->froxels);
    p->froxels = NULL;
    artfFree(p->occlusion);
    p->occlusion = NULL;
    p->width = p->height = 0;
}

void artfSoftPostRun(ArtfSoftPost* p, ArtfSoftFrame* f, uint32_t* pixels) {
    p->f = f;
    p->pixels = pixels;
    const ArtfFrameData* d = &f->data;
    ArtfPassLog* log = p->log;
    if (d->post[0][2] > 0) {
        rows(p, p->height, occlusionRow);
        rows(p, p->height, occlusionBlurRow);
        if (log) artfPassMark(log, "ambient occlusion");
    }
    if (d->post[1][0] > 0) {
        rows(p, p->bloomH[0], reflectionRow);
        rows(p, p->height, reflectionApplyRow);
        if (log) artfPassMark(log, "reflections");
    }
    if (d->post[1][3] > 0) {
        rows(p, p->height, temporalRow);
        copyScratch(p);
        for (int c = 0; c < 3; ++c) memcpy(p->history[c], f->postLight[c], (size_t)p->width * p->height * 4);
        p->historyValid = true;
        if (log) artfPassMark(log, "temporal antialiasing");
    } else {
        p->historyValid = false;
    }
    if (d->post[2][1] > 0 && d->post[2][0] > 0) {
        rows(p, p->height, focusRow);
        copyScratch(p);
        if (log) artfPassMark(log, "depth of field");
    }
    if (d->motionBlur > 0) {
        rows(p, p->height, blurRow);
        copyScratch(p);
        if (log) artfPassMark(log, "motion blur");
    }
    if (d->grade[0][1] > 0) {
        meter(p);
        if (log) artfPassMark(log, "auto exposure");
    } else {
        p->exposureValid = false;
    }
    if (d->post[0][0] > 0) {
        for (p->bloomLevel = 0; p->bloomLevel < ARTF_POST_BLOOM_LEVELS; ++p->bloomLevel) rows(p, p->bloomH[p->bloomLevel], bloomDownRow);
        for (p->bloomLevel = ARTF_POST_BLOOM_LEVELS - 2; p->bloomLevel >= 0; --p->bloomLevel) rows(p, p->bloomH[p->bloomLevel], bloomUpRow);
        if (log) artfPassMark(log, "bloom");
    }
    rows(p, p->height, finalRow);
    if (log) artfPassMark(log, "tone map and out");
}
