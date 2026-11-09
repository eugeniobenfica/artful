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
 * Fog that light goes through comes before them all, and before the tiles: a grid of froxels (artfSoftFroxels) that
 * each surface reads as it is lit, layers too, and the background.
 *
 * No exp, pow or sin from the C library: the same bytes on every processor. */
#include "software/softwareInternal.h"
#include "core/threads.h"

#include <string.h>

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

/* Distance along the view, from what the tiles keep (1 / w); 0 where nothing is. */
static inline float distanceAt(const ArtfSoftPost* p, int x, int y) {
    const float d = p->f->postDepth[(size_t)y * p->width + x];
    return d > 0 ? 1.f / d : 0.f;
}

/* The world at a pixel's center, d along the view. */
static inline void worldAt(const ArtfFrameData* f, int width, int height, float x, float y, float d, float* out) {
    const float px = (x + .5f) / (float)width * 2.f - 1.f, py = 1.f - (y + .5f) / (float)height * 2.f;
    for (int k = 0; k < 3; ++k) out[k] = f->cameraPos[k] + (f->eye[2][k] + px * f->eye[0][k] + py * f->eye[1][k]) * d;
}

/* Where a point of the world falls, in pixels, through viewProj (by columns); false behind the eye. */
static inline bool project(const float* m, const float* p, int width, int height, float* sx, float* sy, float* w) {
    const float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    const float cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    const float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    if (cw <= 1e-5f) return false;
    *sx = (cx / cw * .5f + .5f) * (float)width - .5f;
    *sy = (.5f - cy / cw * .5f) * (float)height - .5f;
    *w = cw;
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
            if (!project(f->viewProj, q, p->width, p->height, &sx, &sy, &w)) continue;
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
        for (int k = 0; k < 3; ++k) v[k] = at[k] - f->cameraPos[k];
        const float vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int k = 0; k < 3; ++k) v[k] /= vl;
        const float vn = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
        float r[3];
        for (int k = 0; k < 3; ++k) r[k] = v[k] - 2.f * vn * n[k];
        /* toward the eye there is nothing in the frame to see */
        const float away = r[0] * f->eye[2][0] + r[1] * f->eye[2][1] + r[2] * f->eye[2][2];
        if (away < -.2f) continue;
        const float stride = d * .04f + .02f, thickness = d * .03f + .05f;
        /* the ray from here to as far as 24 steps reach, across the frame: a line, along which 1 / w goes evenly */
        float sx0, sy0, w0, sx1, sy1, w1, reach = stride * 70.f, end[3];
        if (!project(f->viewProj, at, p->width, p->height, &sx0, &sy0, &w0)) continue;
        const float wDrop = -(r[0] * f->eye[2][0] + r[1] * f->eye[2][1] + r[2] * f->eye[2][2]);
        if (wDrop > 0) reach = artfMinf(reach, w0 * .9f / wDrop); /* (not past the eye) */
        for (int k = 0; k < 3; ++k) end[k] = at[k] + r[k] * reach;
        if (!project(f->viewProj, end, p->width, p->height, &sx1, &sy1, &w1)) continue;
        float hx2 = 0, hy2 = 0, found = 0;
        bool before = true; /* the last step was in front of what the frame shows */
        const float jitter = (float)(hash((uint32_t)(y * p->width + x)) & 255) / 256.f;
        for (int step = 1; step <= 24 && !found; ++step) {
            /* (closer together near the start) */
            const float share = ((float)step + jitter) / 25.f, t = share * share;
            const float sx = sx0 + (sx1 - sx0) * t, sy = sy0 + (sy1 - sy0) * t;
            const float w = 1.f / (1.f / w0 + (1.f / w1 - 1.f / w0) * t);
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
        for (int c = 0; c < 3; ++c) p->bloom[c][half] = (tap(p->f->postLight[c], p->width, p->height, hx2, hy2) - around[c]) * tint[c] * mirror * sure;
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
        float dir[3];
        const float cx = (float)(i * ARTF_FROXEL_TILE) + ARTF_FROXEL_TILE * .5f - .5f, cy = (float)(j * ARTF_FROXEL_TILE) + ARTF_FROXEL_TILE * .5f - .5f;
        worldAt(f, p->width, p->height, cx, cy, 1.f, dir);
        for (int k = 0; k < 3; ++k) dir[k] -= f->cameraPos[k];
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
            for (int c = 0; c < 3; ++c) q[c] = f->cameraPos[c] + dir[c] * t;
            const float sun = p->f->shadows ? artfShadowOfSun(p->f->shadows, f->cameraPos, q, f->toSun) : 1.f;
            const float stepThrough = through(density * (d1 - d0) * along), taken = left * (1.f - stepThrough);
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

static void temporalRow(ArtfSoftPost* p, int y) {
    const ArtfFrameData* f = &p->f->data;
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
        /* where this pixel was: by its depth, or for what is far, by the view alone */
        const float d = distanceAt(p, x, y);
        float at[3], sx, sy, w;
        worldAt(f, p->width, p->height, (float)x - f->post[2][2], (float)y + f->post[2][3], d > 0 ? d : 1e4f, at);
        float share = .1f;
        if (!p->historyValid || !project(f->previous, at, p->width, p->height, &sx, &sy, &w) || sx < 0 || sy < 0 || sx > p->width - 1 || sy > p->height - 1) share = 1.f;
        for (int c = 0; c < 3; ++c) {
            const float now = p->f->postLight[c][i];
            const float was = share < 1.f ? artfClampf(tap(p->history[c], p->width, p->height, sx, sy), lo[c], hi[c]) : now;
            p->scratch[c][i] = was + (now - was) * share;
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

static void bloomDownRow(ArtfSoftPost* p, int y) {
    const int level = p->bloomLevel;
    const int w = p->bloomW[level], h = p->bloomH[level];
    (void)h;
    const float threshold = p->f->data.post[0][1], exposure = p->f->data.look[0] > 0 ? p->f->data.look[0] : 1.f;
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

static void finalRow(ArtfSoftPost* p, int y) {
    const ArtfSoftFrame* sf = p->f;
    const ArtfFrameData* f = &sf->data;
    const float exposure = f->look[0], bloom = f->post[0][0];
    const int bw = p->bloomW[0], bh = p->bloomH[0];
    for (int x = 0; x < p->width; ++x) {
        const size_t i = (size_t)y * p->width + x;
        const float alpha = artfClampf(sf->postLight[3][i], 0, 1);
        uint32_t out = (uint32_t)(alpha * 255.f + .5f) << 24;
        for (int c = 0; c < 3; ++c) {
            float v = sf->postLight[c][i];
            if (alpha > 0) v /= alpha;
            if (exposure > 0) v *= exposure;
            if (bloom > 0) v += tap(p->bloom[c], bw, bh, ((float)x + .5f) * .5f - .5f, ((float)y + .5f) * .5f - .5f) * bloom / artfMaxf(alpha, 1e-3f);
            v = v > 0 ? artfMinf(v, 65504.f) : 0.f; /* (and not a number: nothing) */
            if (exposure > 0) v = v * (v * 2.51f + .03f) / (v * (v * 2.43f + .59f) + .14f);
            out |= (uint32_t)(sqrtf(artfClampf(v, 0, 1)) * alpha * 255.f + .5f) << (16 - 8 * c);
        }
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
    if (d->post[0][2] > 0) {
        rows(p, p->height, occlusionRow);
        rows(p, p->height, occlusionBlurRow);
    }
    if (d->post[1][0] > 0) {
        rows(p, p->bloomH[0], reflectionRow);
        rows(p, p->height, reflectionApplyRow);
    }
    if (d->post[1][3] > 0) {
        rows(p, p->height, temporalRow);
        copyScratch(p);
        for (int c = 0; c < 3; ++c) memcpy(p->history[c], f->postLight[c], (size_t)p->width * p->height * 4);
        p->historyValid = true;
    } else {
        p->historyValid = false;
    }
    if (d->post[2][1] > 0 && d->post[2][0] > 0) {
        rows(p, p->height, focusRow);
        copyScratch(p);
    }
    if (d->post[0][0] > 0) {
        for (p->bloomLevel = 0; p->bloomLevel < ARTF_POST_BLOOM_LEVELS; ++p->bloomLevel) rows(p, p->bloomH[p->bloomLevel], bloomDownRow);
        for (p->bloomLevel = ARTF_POST_BLOOM_LEVELS - 2; p->bloomLevel >= 0; --p->bloomLevel) rows(p, p->bloomH[p->bloomLevel], bloomUpRow);
    }
    rows(p, p->height, finalRow);
}
