/* artful — a sky from the sun: light scattered once by air (Rayleigh) and haze (Mie), and taken by ozone, along each
 * direction from a point above the ground, as Nishita et al. (1993) have it, with the constants Bruneton's and
 * Hillaire's skies use. It is made into a picture of all around, which the scene then has as its light from all
 * around: so both backends light by it, reflect it, show it and trace it, with nothing of their own.
 *
 * Asked for more (light scattered more than once, the sun drawn, clouds), it goes as Hillaire's (2020) does: what the
 * air leaves of the sun is read from a table by height and angle (Bruneton's mapping, which keeps the horizon fine);
 * light scattered more than once from another, small, of what a point at each height takes from all around when the sun
 * is at each angle, as if every order scattered alike: the second order over 1 - the share that goes on scattering.
 * Clouds are a layer followed through in steps, of tiled noise from a hash, each step lit by the sun through the
 * cloud above it (Beer's law, the powder of its edges, and three octaves of what scatters more than once: Wrenninge's),
 * and by the sky; seen through the air between.
 *
 * Light is in Artful's units: the sun's color is its light with pi taken out, so the sky's is pi times what the
 * air scatters of it toward the eye. Lengths are kilometers. No exp, sin or cos from the C library: the same bytes on
 * every processor. */
#include "scene/sceneInternal.h"
#include "core/threads.h"

#define EARTH 6360.f
#define TOP 6460.f
#define VIEW_STEPS 24
#define SUN_STEPS 8

#define sineOf artfSinFast
#define cosineOf artfCosFast

static inline float expOf(float x) { return artfExp2Fast(x * 1.44269504f); }

/* What the air holds at a height above the ground: air, haze and ozone, each as a share of what it is at the ground
 * (ozone: of its peak, at 25 km). */
static inline void densities(float height, float* out) {
    out[0] = expOf(-height / 8.f);
    out[1] = expOf(-height / 1.2f);
    out[2] = artfMaxf(1.f - fabsf(height - 25.f) / 15.f, 0);
}

/* How far from p along d (of length 1) to a sphere of radius r around the earth's middle; -1 if it is not met ahead. */
static float toSphere(const float* p, const float* d, float r) {
    const float b = p[0] * d[0] + p[1] * d[1] + p[2] * d[2], c = p[0] * p[0] + p[1] * p[1] + p[2] * p[2] - r * r;
    const float disc = b * b - c;
    if (disc < 0) return -1.f;
    const float root = sqrtf(disc), near_ = -b - root, far = -b + root;
    return near_ > 0 ? near_ : far > 0 ? far : -1.f;
}

/* Both: where a line from p along d meets the sphere, *near_ <= *far; false when it does not. */
static bool sphereSpan(const float* p, const float* d, float r, float* near_, float* far) {
    const float b = p[0] * d[0] + p[1] * d[1] + p[2] * d[2], c = p[0] * p[0] + p[1] * p[1] + p[2] * p[2] - r * r;
    const float disc = b * b - c;
    if (disc < 0) return false;
    const float root = sqrtf(disc);
    *near_ = -b - root;
    *far = -b + root;
    return true;
}

/* The tables: what the air leaves (optical depth, by Bruneton's mapping of height and angle), and light scattered
 * more than once (by the sun's cosine across and height down). */
#define TRANS_W 256
#define TRANS_H 64
#define TRANS_STEPS 40
#define MULTI_SIDE 32
#define MULTI_STEPS 20
#define MULTI_DIRS 8 /* a side: 64 directions */

/* Clouds: tiled noise of shapes (a side of NOISE, over SHAPE_KM), and where they are (a side of WEATHER, over
 * WEATHER_KM), followed in CLOUD_STEPS; each lit through LIGHT_STEPS toward the sun. */
#define NOISE 64
#define SHAPE_KM 12.f
#define DETAIL_KM 2.3f
#define WEATHER 128
#define WEATHER_KM 60.f
#define CLOUD_STEPS 64
#define LIGHT_STEPS 6
#define CLOUD_FAR 90.f /* km: past this, what is left of them fades into the air */

typedef struct Sky {
    float rayleigh[3], mie, mieTaken, ozone[3], g;
    float eye[3], toSun[3], sun[3], ground[3];
    int width, height;
    float* rgb;
    /* Hillaire's, when asked */
    bool tables;
    float multiple, disc, eyeSun[3];
    float* trans;  /* TRANS_W x TRANS_H x 3: optical depth to the top of the air */
    float* multi;  /* MULTI_SIDE x MULTI_SIDE x 3 */
    /* clouds */
    float coverage, base, top, density, wind[3], ambient[3];
    float* noise;  /* NOISE^3 */
    float* weather;/* WEATHER^2 */
} Sky;

/* What the air leaves of the sun's light at p: 0 behind the earth. */
static void sunThrough(const Sky* k, const float* p, float* out) {
    if (toSphere(p, k->toSun, EARTH) > 0) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    const float length = toSphere(p, k->toSun, TOP), step = artfMaxf(length, 0) / SUN_STEPS;
    float depth[3] = {0, 0, 0};
    for (int i = 0; i < SUN_STEPS; ++i) {
        const float t = ((float)i + .5f) * step;
        const float q[3] = {p[0] + k->toSun[0] * t, p[1] + k->toSun[1] * t, p[2] + k->toSun[2] * t};
        float d[3];
        densities(sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) - EARTH, d);
        for (int c = 0; c < 3; ++c) depth[c] += (d[0] * k->rayleigh[c] + d[1] * k->mieTaken + d[2] * k->ozone[c]) * step;
    }
    for (int c = 0; c < 3; ++c) out[c] = expOf(-depth[c]);
}

/* The light that comes toward the eye from direction d. */
static void skyToward(const Sky* k, const float* d, float* out) {
    const float ground = toSphere(k->eye, d, EARTH);
    const float length = ground > 0 ? ground : artfMaxf(toSphere(k->eye, d, TOP), 0);
    const float step = length / VIEW_STEPS;
    const float mu = d[0] * k->toSun[0] + d[1] * k->toSun[1] + d[2] * k->toSun[2];
    /* the phases: Rayleigh's, and Cornette and Shanks's for haze */
    const float rayleighPhase = .059683104f * (1.f + mu * mu);
    const float g = k->g, hg = 1.f + g * g - 2.f * g * mu;
    const float miePhase = .11936621f * (1.f - g * g) * (1.f + mu * mu) / ((2.f + g * g) * hg * sqrtf(hg));
    float light[3] = {0, 0, 0}, depth[3] = {0, 0, 0};
    for (int i = 0; i < VIEW_STEPS; ++i) {
        const float t = ((float)i + .5f) * step;
        const float p[3] = {k->eye[0] + d[0] * t, k->eye[1] + d[1] * t, k->eye[2] + d[2] * t};
        float dens[3], sun[3];
        densities(sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - EARTH, dens);
        sunThrough(k, p, sun);
        for (int c = 0; c < 3; ++c) {
            const float taken = (dens[0] * k->rayleigh[c] + dens[1] * k->mieTaken + dens[2] * k->ozone[c]) * step;
            const float half = depth[c] + taken * .5f;
            const float scattered = dens[0] * k->rayleigh[c] * rayleighPhase + dens[1] * k->mie * miePhase;
            light[c] += expOf(-half) * sun[c] * scattered * step;
            depth[c] += taken;
        }
    }
    for (int c = 0; c < 3; ++c) out[c] = light[c] * k->sun[c] * 3.14159265f;
    if (ground > 0) {
        /* the ground, lit by the sun through the air, seen through the air */
        const float p[3] = {k->eye[0] + d[0] * ground, k->eye[1] + d[1] * ground, k->eye[2] + d[2] * ground};
        float sun[3];
        sunThrough(k, p, sun);
        const float r = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        const float facing = artfMaxf((p[0] * k->toSun[0] + p[1] * k->toSun[1] + p[2] * k->toSun[2]) / r, 0);
        for (int c = 0; c < 3; ++c) out[c] += k->ground[c] * sun[c] * k->sun[c] * facing * expOf(-depth[c]);
    }
}

/* ---- the tables ---- */

/* Bilinear in a table of three floats a texel, w x h, at texel coordinates (centers at whole numbers). */
static void table3(const float* t, int w, int h, float x, float y, float* out) {
    x = artfClampf(x, 0, (float)(w - 1));
    y = artfClampf(y, 0, (float)(h - 1));
    const int x0 = artfMini((int)x, w - 2), y0 = artfMini((int)y, h - 2);
    const float fx = x - (float)x0, fy = y - (float)y0;
    const float* a = t + 3 * ((size_t)y0 * w + x0);
    const float* b = a + 3 * (size_t)w;
    for (int c = 0; c < 3; ++c) {
        const float top = a[c] + (a[3 + c] - a[c]) * fx, bottom = b[c] + (b[3 + c] - b[c]) * fx;
        out[c] = top + (bottom - top) * fy;
    }
}

/* Bruneton's mapping: from a radius and the cosine of a direction to how far the top of the air is (across) and the
 * height (down), each 0 to 1; and back. */
#define HORIZON 1132.2544f /* sqrt(TOP^2 - EARTH^2): to the top of the air, along the ground */
static void transPlace(float r, float mu, float* x, float* y) {
    const float rho = sqrtf(artfMaxf(r * r - EARTH * EARTH, 0));
    const float d = artfMaxf(-r * mu + sqrtf(artfMaxf(r * r * (mu * mu - 1.f) + TOP * TOP, 0)), 0);
    const float least = TOP - r, most = rho + HORIZON;
    *x = (d - least) / artfMaxf(most - least, 1e-6f);
    *y = rho / HORIZON;
}

/* What the air leaves of the sun's light at p, from the table: 0 behind the earth. */
static void sunThroughTable(const Sky* k, const float* p, float* out) {
    if (toSphere(p, k->toSun, EARTH) > 0) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    const float r = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const float mu = (p[0] * k->toSun[0] + p[1] * k->toSun[1] + p[2] * k->toSun[2]) / r;
    float x, y, depth[3];
    transPlace(r, mu, &x, &y);
    table3(k->trans, TRANS_W, TRANS_H, x * (TRANS_W - 1), y * (TRANS_H - 1), depth);
    for (int c = 0; c < 3; ++c) out[c] = expOf(-depth[c]);
}

static void transRow(void* user, uint32_t row, uint32_t worker) {
    (void)worker;
    Sky* k = user;
    const float rho = HORIZON * (float)row / (TRANS_H - 1), r = sqrtf(rho * rho + EARTH * EARTH);
    for (int i = 0; i < TRANS_W; ++i) {
        const float least = TOP - r, most = rho + HORIZON;
        const float d = least + (most - least) * (float)i / (TRANS_W - 1);
        const float mu = d > 0 ? artfClampf((HORIZON * HORIZON - rho * rho - d * d) / (2.f * r * d), -1, 1) : 1.f;
        const float step = d / TRANS_STEPS;
        float depth[3] = {0, 0, 0};
        for (int s = 0; s < TRANS_STEPS; ++s) {
            const float t = ((float)s + .5f) * step;
            const float height = sqrtf(artfMaxf(r * r + t * t + 2.f * r * mu * t, 0)) - EARTH;
            float dens[3];
            densities(artfMaxf(height, 0), dens);
            for (int c = 0; c < 3; ++c) depth[c] += (dens[0] * k->rayleigh[c] + dens[1] * k->mieTaken + dens[2] * k->ozone[c]) * step;
        }
        memcpy(k->trans + 3 * ((size_t)row * TRANS_W + i), depth, 12);
    }
}

/* Light scattered more than once at a height, with the sun at a cosine: Hillaire's Psi, for a sun of light 1. Each
 * texel looks 64 ways, and along each adds light scattered once (with a phase the same all ways) and the ground lit; and
 * how much of what comes goes on scattering. */
static void multiRow(void* user, uint32_t row, uint32_t worker) {
    (void)worker;
    Sky* k = user;
    const float height = artfMaxf(((float)row + .5f) / MULTI_SIDE * (TOP - EARTH), .01f);
    for (int i = 0; i < MULTI_SIDE; ++i) {
        const float muSun = ((float)i + .5f) / MULTI_SIDE * 2.f - 1.f;
        Sky local = *k;
        local.toSun[0] = sqrtf(artfMaxf(1.f - muSun * muSun, 0));
        local.toSun[1] = muSun;
        local.toSun[2] = 0;
        const float at[3] = {0, EARTH + height, 0};
        float second[3] = {0, 0, 0}, onward[3] = {0, 0, 0};
        for (int a = 0; a < MULTI_DIRS; ++a)
            for (int b = 0; b < MULTI_DIRS; ++b) {
                const float z = 1.f - 2.f * ((float)a + .5f) / MULTI_DIRS, phi = 6.28318531f * ((float)b + .5f) / MULTI_DIRS;
                const float across = sqrtf(artfMaxf(1.f - z * z, 0));
                const float d[3] = {across * cosineOf(phi), z, across * sineOf(phi)};
                const float ground = toSphere(at, d, EARTH);
                const float length = ground > 0 ? ground : artfMaxf(toSphere(at, d, TOP), 0);
                const float step = length / MULTI_STEPS;
                float depth[3] = {0, 0, 0};
                for (int s = 0; s < MULTI_STEPS; ++s) {
                    const float t = ((float)s + .5f) * step;
                    const float p[3] = {at[0] + d[0] * t, at[1] + d[1] * t, at[2] + d[2] * t};
                    float dens[3], sun[3];
                    densities(sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - EARTH, dens);
                    sunThroughTable(&local, p, sun);
                    for (int c = 0; c < 3; ++c) {
                        const float taken = (dens[0] * k->rayleigh[c] + dens[1] * k->mieTaken + dens[2] * k->ozone[c]) * step;
                        const float through = expOf(-(depth[c] + taken * .5f));
                        const float scatter = (dens[0] * k->rayleigh[c] + dens[1] * k->mie) * step;
                        second[c] += through * scatter * sun[c] * .079577472f; /* (1 / 4 pi) */
                        onward[c] += through * scatter;
                        depth[c] += taken;
                    }
                }
                if (ground > 0) {
                    const float p[3] = {at[0] + d[0] * ground, at[1] + d[1] * ground, at[2] + d[2] * ground};
                    float sun[3];
                    sunThroughTable(&local, p, sun);
                    const float r = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                    const float facing = artfMaxf((p[0] * local.toSun[0] + p[1] * local.toSun[1] + p[2] * local.toSun[2]) / r, 0);
                    for (int c = 0; c < 3; ++c) second[c] += expOf(-depth[c]) * sun[c] * facing * k->ground[c] * .31830989f;
                }
            }
        float* out = k->multi + 3 * ((size_t)row * MULTI_SIDE + i);
        for (int c = 0; c < 3; ++c) {
            const float all = second[c] / (MULTI_DIRS * MULTI_DIRS), share = onward[c] / (MULTI_DIRS * MULTI_DIRS);
            out[c] = all / artfMaxf(1.f - share, .01f);
        }
    }
}

/* The light scattered more than once at p, for a sun of light 1. */
static void multipleAt(const Sky* k, const float* p, float r, float* out) {
    const float mu = (p[0] * k->toSun[0] + p[1] * k->toSun[1] + p[2] * k->toSun[2]) / r;
    table3(k->multi, MULTI_SIDE, MULTI_SIDE, (mu * .5f + .5f) * MULTI_SIDE - .5f, (r - EARTH) / (TOP - EARTH) * MULTI_SIDE - .5f, out);
}

/* As skyToward, from the tables, with light scattered more than once and the sun drawn. */
static void skyTowardTables(const Sky* k, const float* d, float* out) {
    const float ground = toSphere(k->eye, d, EARTH);
    const float length = ground > 0 ? ground : artfMaxf(toSphere(k->eye, d, TOP), 0);
    const float step = length / VIEW_STEPS;
    const float mu = d[0] * k->toSun[0] + d[1] * k->toSun[1] + d[2] * k->toSun[2];
    const float rayleighPhase = .059683104f * (1.f + mu * mu);
    const float g = k->g, hg = 1.f + g * g - 2.f * g * mu;
    const float miePhase = .11936621f * (1.f - g * g) * (1.f + mu * mu) / ((2.f + g * g) * hg * sqrtf(hg));
    float light[3] = {0, 0, 0}, depth[3] = {0, 0, 0};
    for (int i = 0; i < VIEW_STEPS; ++i) {
        const float t = ((float)i + .5f) * step;
        const float p[3] = {k->eye[0] + d[0] * t, k->eye[1] + d[1] * t, k->eye[2] + d[2] * t};
        const float r = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        float dens[3], sun[3], many[3] = {0, 0, 0};
        densities(r - EARTH, dens);
        sunThroughTable(k, p, sun);
        if (k->multiple > 0) multipleAt(k, p, r, many);
        for (int c = 0; c < 3; ++c) {
            const float taken = (dens[0] * k->rayleigh[c] + dens[1] * k->mieTaken + dens[2] * k->ozone[c]) * step;
            const float half = depth[c] + taken * .5f;
            const float scattered = dens[0] * k->rayleigh[c] * rayleighPhase + dens[1] * k->mie * miePhase;
            const float more = (dens[0] * k->rayleigh[c] + dens[1] * k->mie) * many[c] * k->multiple;
            light[c] += expOf(-half) * (sun[c] * scattered + more) * step;
            depth[c] += taken;
        }
    }
    for (int c = 0; c < 3; ++c) out[c] = light[c] * k->sun[c] * 3.14159265f;
    if (ground > 0) {
        const float p[3] = {k->eye[0] + d[0] * ground, k->eye[1] + d[1] * ground, k->eye[2] + d[2] * ground};
        float sun[3];
        sunThroughTable(k, p, sun);
        const float r = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        const float facing = artfMaxf((p[0] * k->toSun[0] + p[1] * k->toSun[1] + p[2] * k->toSun[2]) / r, 0);
        for (int c = 0; c < 3; ++c) out[c] += k->ground[c] * sun[c] * k->sun[c] * facing * expOf(-depth[c]);
        return;
    }
    if (k->disc > 0 && mu > 0) {
        /* the sun: how far from its middle, by the sine (exact enough at this size), covering a texel's share at its
         * edge, darker toward it (Neckel's and Labs's limb, by channel) */
        const float cx = d[1] * k->toSun[2] - d[2] * k->toSun[1], cy = d[2] * k->toSun[0] - d[0] * k->toSun[2], cz = d[0] * k->toSun[1] - d[1] * k->toSun[0];
        const float off = sqrtf(cx * cx + cy * cy + cz * cz), texel = 6.28318531f / (float)k->width;
        const float covered = artfClampf((k->disc - off) / texel + .5f, 0, 1);
        if (covered > 0) {
            const float edge = artfMinf(off / k->disc, 1.f), cosine = sqrtf(artfMaxf(1.f - edge * edge, 0));
            const float limb[3] = {.397f, .503f, .652f};
            for (int c = 0; c < 3; ++c) {
                const float darker = cosine > 0 ? artfExp2Fast(artfLog2Fast(cosine) * limb[c]) : 0.f;
                out[c] += 10.f * k->eyeSun[c] * darker * covered;
            }
        }
    }
}

/* ---- clouds ---- */

static inline uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}

/* Value noise that tiles every period cells, at a point in cells: a hash at each corner, between them smoothly. */
static float valueNoise(float x, float y, float z, int period, uint32_t seed) {
    const int ix = (int)(x + 1024.f) - 1024, iy = (int)(y + 1024.f) - 1024, iz = (int)(z + 1024.f) - 1024;
    float fx = x - (float)ix, fy = y - (float)iy, fz = z - (float)iz;
    fx = fx * fx * (3.f - 2.f * fx);
    fy = fy * fy * (3.f - 2.f * fy);
    fz = fz * fz * (3.f - 2.f * fz);
    float corner[8];
    for (int c = 0; c < 8; ++c) {
        const uint32_t cx = (uint32_t)(((ix + (c & 1)) % period + period) % period), cy = (uint32_t)(((iy + (c >> 1 & 1)) % period + period) % period);
        const uint32_t cz = (uint32_t)(((iz + (c >> 2)) % period + period) % period);
        corner[c] = (float)(mix32(cx + mix32(cy + mix32(cz + seed))) >> 8) * (1.f / 16777216.f);
    }
    const float a = corner[0] + (corner[1] - corner[0]) * fx, b = corner[2] + (corner[3] - corner[2]) * fx;
    const float c = corner[4] + (corner[5] - corner[4]) * fx, d = corner[6] + (corner[7] - corner[6]) * fx;
    const float near_ = a + (b - a) * fy, far = c + (d - c) * fy;
    return near_ + (far - near_) * fz;
}

/* A slice of the shapes' noise: four octaves, tiling over NOISE texels; billowed (1 - |2n - 1| of the finer octaves),
 * as cumulus are. */
static void noiseSlice(void* user, uint32_t z, uint32_t worker) {
    (void)worker;
    Sky* k = user;
    for (int y = 0; y < NOISE; ++y)
        for (int x = 0; x < NOISE; ++x) {
            float sum = 0, weight = 0, amp = 1;
            for (int o = 0; o < 4; ++o) {
                const int period = 4 << o;
                const float scale = (float)period / NOISE;
                float n = valueNoise((float)x * scale, (float)y * scale, (float)z * scale, period, 17u + (uint32_t)o);
                if (o) n = 1.f - fabsf(2.f * n - 1.f);
                sum += n * amp;
                weight += amp;
                amp *= .5f;
            }
            k->noise[((size_t)z * NOISE + y) * NOISE + x] = sum / weight;
        }
}

static void weatherRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    Sky* k = user;
    for (int x = 0; x < WEATHER; ++x) {
        float sum = 0, weight = 0, amp = 1;
        for (int o = 0; o < 3; ++o) {
            const int period = 3 << o;
            const float scale = (float)period / WEATHER;
            sum += valueNoise((float)x * scale, (float)y * scale, .5f, period, 91u + (uint32_t)o) * amp;
            weight += amp;
            amp *= .5f;
        }
        k->weather[(size_t)y * WEATHER + x] = sum / weight;
    }
}

/* Trilinear in the shapes' noise, tiled, at a point in texels. */
static float noiseAt(const float* noise, float x, float y, float z) {
    const int ix = (int)(x + 65536.f) - 65536, iy = (int)(y + 65536.f) - 65536, iz = (int)(z + 65536.f) - 65536;
    const float fx = x - (float)ix, fy = y - (float)iy, fz = z - (float)iz;
    const int x0 = ix & (NOISE - 1), y0 = iy & (NOISE - 1), z0 = iz & (NOISE - 1);
    const int x1 = (x0 + 1) & (NOISE - 1), y1 = (y0 + 1) & (NOISE - 1), z1 = (z0 + 1) & (NOISE - 1);
#define N(a, b, c) noise[((size_t)(c) * NOISE + (b)) * NOISE + (a)]
    const float a = N(x0, y0, z0) + (N(x1, y0, z0) - N(x0, y0, z0)) * fx, b = N(x0, y1, z0) + (N(x1, y1, z0) - N(x0, y1, z0)) * fx;
    const float c = N(x0, y0, z1) + (N(x1, y0, z1) - N(x0, y0, z1)) * fx, d = N(x0, y1, z1) + (N(x1, y1, z1) - N(x0, y1, z1)) * fx;
#undef N
    const float near_ = a + (b - a) * fy, far = c + (d - c) * fy;
    return near_ + (far - near_) * fz;
}

static float weatherAt(const float* weather, float x, float y) {
    const int ix = (int)(x + 65536.f) - 65536, iy = (int)(y + 65536.f) - 65536;
    const float fx = x - (float)ix, fy = y - (float)iy;
    const int x0 = ix & (WEATHER - 1), y0 = iy & (WEATHER - 1), x1 = (x0 + 1) & (WEATHER - 1), y1 = (y0 + 1) & (WEATHER - 1);
    const float a = weather[y0 * WEATHER + x0], b = weather[y0 * WEATHER + x1], c = weather[y1 * WEATHER + x0], d = weather[y1 * WEATHER + x1];
    const float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
    return top + (bottom - top) * fy;
}

/* How much cloud takes at p, a height h above the ground: per kilometer. fine: with the finer noise that wears their
 * edges away. */
static float cloudAt(const Sky* k, const float* p, float h, bool fine) {
    const float frac = (h - k->base) / (k->top - k->base);
    if (!(frac > 0 && frac < 1)) return 0;
    const float x = p[0] + k->wind[0], y = h + k->wind[1], z = p[2] + k->wind[2];
    /* where they are: the coverage, more or less from place to place */
    const float local = artfClampf(k->coverage + (weatherAt(k->weather, x * (WEATHER / WEATHER_KM), z * (WEATHER / WEATHER_KM)) - .5f) * 1.2f, 0, 1);
    if (!(local > 0)) return 0;
    /* rounded below, thinner toward the top */
    const float rise = artfClampf(frac * 6.f, 0, 1), fall = artfClampf((1.f - frac) * 1.8f, 0, 1);
    const float shape = noiseAt(k->noise, x * (NOISE / SHAPE_KM), y * (NOISE / SHAPE_KM), z * (NOISE / SHAPE_KM)) * rise * (fall * (2.f - fall));
    float d = (shape - (1.f - local)) / artfMaxf(local, .05f);
    if (!(d > 0)) return 0;
    if (fine) {
        const float wear = noiseAt(k->noise, x * (NOISE / DETAIL_KM) + 17.f, y * (NOISE / DETAIL_KM), z * (NOISE / DETAIL_KM) + 31.f);
        d -= (1.f - wear) * .35f * (1.f - d);
        if (!(d > 0)) return 0;
    }
    return artfMinf(d, 1.f) * k->density * 45.f;
}

static inline float henyey(float g, float mu) {
    const float q = 1.f + g * g - 2.f * g * mu;
    return .079577472f * (1.f - g * g) / (q * sqrtf(q));
}

/* What clouds along direction d make of the sky behind them (out): followed from where the layer begins, at most
 * CLOUD_FAR on, in steps that start a little apart from texel to texel. */
static void cloudsToward(const Sky* k, const float* d, float jitter, float* out) {
    float a0, a1, b0, b1;
    const float r = sqrtf(k->eye[0] * k->eye[0] + k->eye[1] * k->eye[1] + k->eye[2] * k->eye[2]);
    const float inner = EARTH + k->base, outer = EARTH + k->top;
    if (!sphereSpan(k->eye, d, outer, &b0, &b1) || b1 <= 0) return;
    float from, to;
    if (r < inner) {
        /* from below: from the bottom up to the top; not past the ground */
        if (!sphereSpan(k->eye, d, inner, &a0, &a1)) return;
        const float ground = toSphere(k->eye, d, EARTH);
        if (ground > 0 && ground < a1) return;
        from = a1;
        to = b1;
    } else if (r < outer) {
        from = 0;
        to = sphereSpan(k->eye, d, inner, &a0, &a1) && a0 > 0 ? a0 : b1;
    } else {
        from = artfMaxf(b0, 0);
        to = sphereSpan(k->eye, d, inner, &a0, &a1) && a0 > 0 ? a0 : b1;
    }
    if (from > CLOUD_FAR) return;
    to = artfMinf(to, from + 30.f);
    const float step = (to - from) / CLOUD_STEPS;
    const float mu = d[0] * k->toSun[0] + d[1] * k->toSun[1] + d[2] * k->toSun[2];
    /* forward and back, and their octaves for light scattered many times (Wrenninge's a, b, c of .5) */
    float phase[3];
    for (int o = 0; o < 3; ++o) {
        const float g = o == 0 ? 1.f : o == 1 ? .5f : .25f;
        phase[o] = henyey(.8f * g, mu) * .7f + henyey(-.3f * g, mu) * .3f;
    }
    float light[3] = {0, 0, 0}, left = 1.f, depthSum = 0, depthWeight = 0;
    for (int i = 0; i < CLOUD_STEPS && left > .005f; ++i) {
        const float t = from + ((float)i + jitter) * step;
        const float p[3] = {k->eye[0] + d[0] * t, k->eye[1] + d[1] * t, k->eye[2] + d[2] * t};
        const float h = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - EARTH;
        const float sigma = cloudAt(k, p, h, true);
        if (!(sigma > 0)) continue;
        /* toward the sun, in steps that grow */
        float tau = 0, along = 0, sl = .06f;
        for (int s = 0; s < LIGHT_STEPS; ++s) {
            const float at = along + sl * .5f;
            const float q[3] = {p[0] + k->toSun[0] * at, p[1] + k->toSun[1] * at, p[2] + k->toSun[2] * at};
            tau += cloudAt(k, q, sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) - EARTH, s < 3) * sl;
            along += sl;
            sl *= 1.8f;
        }
        float sun[3];
        sunThroughTable(k, p, sun);
        const float powder = 1.f - expOf(-sigma * .2f), edge = powder + (1.f - powder) * (mu * .5f + .5f);
        float beer = 0;
        for (int o = 0; o < 3; ++o) {
            const float share = o == 0 ? 1.f : o == 1 ? .5f : .25f;
            beer += share * expOf(-tau * share) * phase[o];
        }
        const float frac = (h - k->base) / (k->top - k->base), sky = .25f + .75f * frac;
        const float through = expOf(-sigma * step), taken = 1.f - through;
        for (int c = 0; c < 3; ++c) {
            /* (all of what is taken is scattered: so what each step adds is its light times what it takes) */
            const float lit = k->sun[c] * 3.14159265f * sun[c] * beer * edge + k->ambient[c] * sky;
            light[c] += left * lit * taken;
        }
        depthSum += t * left * taken;
        depthWeight += left * taken;
        left *= through;
    }
    if (!(depthWeight > 0)) return;
    /* seen through the air up to where they mostly are; and fading into it far off */
    const float far = depthSum / depthWeight;
    float air[3] = {0, 0, 0};
    const float airStep = far / 8.f;
    for (int i = 0; i < 8; ++i) {
        const float t = ((float)i + .5f) * airStep;
        const float p[3] = {k->eye[0] + d[0] * t, k->eye[1] + d[1] * t, k->eye[2] + d[2] * t};
        float dens[3];
        densities(sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - EARTH, dens);
        for (int c = 0; c < 3; ++c) air[c] += (dens[0] * k->rayleigh[c] + dens[1] * k->mieTaken + dens[2] * k->ozone[c]) * airStep;
    }
    const float near_ = artfClampf(1.f - far / CLOUD_FAR, 0, 1), fade = near_ * (2.f - near_);
    const float cover = (1.f - left) * fade;
    for (int c = 0; c < 3; ++c) {
        const float clear = expOf(-air[c]);
        out[c] = out[c] * (1.f - cover) + fade * (light[c] * clear + (1.f - left) * (1.f - clear) * out[c]);
    }
}

/* ---- clouds in the scene ---- */

void artfCloudsColumn(const float* field, const ArtfFrameData* f, const float* dir, float jitter, float farthest, float* out) {
    Sky k;
    memset(&k, 0, sizeof k);
    k.base = field[0];
    k.top = field[1];
    k.density = field[2];
    k.coverage = field[3];
    memcpy(k.wind, field + 4, 12);
    const float unit = field[7];
    memcpy(k.toSun, field + 8, 12);
    k.eye[1] = field[11];
    memcpy(k.sun, field + 12, 12);
    memcpy(k.ambient, field + 16, 12);
    k.noise = (float*)(field + ARTF_CLOUD_NOISE);
    k.weather = (float*)(field + ARTF_CLOUD_WEATHER);
    k.trans = (float*)(field + ARTF_CLOUD_TRANS);
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int s = 0; s < ARTF_CLOUD_SLICES; ++s) {
        float* cell = out + 4 * (1 + s);
        cell[0] = cell[1] = cell[2] = 0;
        cell[3] = 1.f;
    }
    /* where the layer is along the ray: as cloudsToward */
    float a0, a1, b0, b1;
    const float r = k.eye[1], inner = EARTH + k.base, outer = EARTH + k.top;
    if (!sphereSpan(k.eye, dir, outer, &b0, &b1) || b1 <= 0) return;
    float from, to;
    if (r < inner) {
        if (!sphereSpan(k.eye, dir, inner, &a0, &a1)) return;
        const float ground = toSphere(k.eye, dir, EARTH);
        if (ground > 0 && ground < a1) return;
        from = a1;
        to = b1;
    } else if (r < outer) {
        from = 0;
        to = sphereSpan(k.eye, dir, inner, &a0, &a1) && a0 > 0 ? a0 : b1;
    } else {
        from = artfMaxf(b0, 0);
        to = sphereSpan(k.eye, dir, inner, &a0, &a1) && a0 > 0 ? a0 : b1;
    }
    if (from > CLOUD_FAR || !(farthest * unit > from)) return;
    to = artfMinf(to, farthest * unit);
    to = artfMinf(to, from + 30.f);
    out[0] = from / unit;
    out[1] = to / unit;
    const int steps = ARTF_CLOUD_SLICES * ARTF_CLOUD_STEPS;
    const float step = (to - from) / (float)steps;
    const float mu = dir[0] * k.toSun[0] + dir[1] * k.toSun[1] + dir[2] * k.toSun[2];
    float phase[3];
    for (int o = 0; o < 3; ++o) {
        const float g = o == 0 ? 1.f : o == 1 ? .5f : .25f;
        phase[o] = henyey(.8f * g, mu) * .7f + henyey(-.3f * g, mu) * .3f;
    }
    float light[3] = {0, 0, 0}, left = 1.f;
    for (int i = 0; i < steps; ++i) {
        const float t = from + ((float)i + jitter) * step;
        const float p[3] = {k.eye[0] + dir[0] * t, k.eye[1] + dir[1] * t, k.eye[2] + dir[2] * t};
        const float h = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - EARTH;
        const float sigma = left > .005f ? cloudAt(&k, p, h, true) : 0.f;
        if (sigma > 0) {
            float tau = 0, along = 0, sl = .06f;
            for (int s = 0; s < LIGHT_STEPS; ++s) {
                const float at = along + sl * .5f;
                const float q[3] = {p[0] + k.toSun[0] * at, p[1] + k.toSun[1] * at, p[2] + k.toSun[2] * at};
                tau += cloudAt(&k, q, sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) - EARTH, s < 3) * sl;
                along += sl;
                sl *= 1.8f;
            }
            float sun[3];
            sunThroughTable(&k, p, sun);
            const float powder = 1.f - expOf(-sigma * .2f), edge = powder + (1.f - powder) * (mu * .5f + .5f);
            float beer = 0;
            for (int o = 0; o < 3; ++o) {
                const float share = o == 0 ? 1.f : o == 1 ? .5f : .25f;
                beer += share * expOf(-tau * share) * phase[o];
            }
            const float frac = (h - k.base) / (k.top - k.base), sky = .25f + .75f * frac;
            const float through = expOf(-sigma * step), taken = 1.f - through;
            /* (seen through the air between: what aerial perspective keeps of it) */
            float keep[3] = {1, 1, 1}, add[3];
            if (f->aerial[3][3] > 0) artfAerialOf(f, t / unit, mu, keep, add);
            for (int c = 0; c < 3; ++c) light[c] += left * (k.sun[c] * 3.14159265f * sun[c] * beer * edge + k.ambient[c] * sky) * taken * keep[c];
            left *= through;
        }
        if ((i + 1) % ARTF_CLOUD_STEPS == 0) {
            float* cell = out + 4 * (1 + (i + 1) / ARTF_CLOUD_STEPS - 1);
            for (int c = 0; c < 3; ++c) cell[c] = light[c];
            cell[3] = left;
        }
    }
}

void artfCloudsAt(const float* grid, int columns, int rows, float sx, float sy, float distance, float* out) {
    const float fx = artfClampf((sx - (ARTF_CLOUD_TILE * .5f - .5f)) / ARTF_CLOUD_TILE, 0, (float)(columns - 1));
    const float fy = artfClampf((sy - (ARTF_CLOUD_TILE * .5f - .5f)) / ARTF_CLOUD_TILE, 0, (float)(rows - 1));
    const int x0 = (int)fx, y0 = (int)fy, x1 = artfMini(x0 + 1, columns - 1), y1 = artfMini(y0 + 1, rows - 1);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    const int cx[4] = {x0, x1, x0, x1}, cy[4] = {y0, y0, y1, y1};
    const float weight[4] = {(1.f - tx) * (1.f - ty), tx * (1.f - ty), (1.f - tx) * ty, tx * ty};
    out[0] = out[1] = out[2] = 0;
    out[3] = 0;
    for (int n = 0; n < 4; ++n) {
        /* (each column by its own span: where this distance falls in its slices, between the two around it) */
        const float* cell = grid + ((size_t)cy[n] * columns + cx[n]) * ARTF_CLOUD_CELL;
        float here[4] = {0, 0, 0, 1};
        if (cell[1] > cell[0] && distance > cell[0]) {
            const float z = artfMinf((distance - cell[0]) / (cell[1] - cell[0]), 1.f) * ARTF_CLOUD_SLICES;
            const int k1 = artfMini((int)z, ARTF_CLOUD_SLICES - 1), k0 = k1 - 1;
            const float tz = z - (float)k1;
            const float* b = cell + 4 * (1 + k1);
            const float a[4] = {k0 >= 0 ? cell[4 * (1 + k0)] : 0.f, k0 >= 0 ? cell[4 * (1 + k0) + 1] : 0.f, k0 >= 0 ? cell[4 * (1 + k0) + 2] : 0.f, k0 >= 0 ? cell[4 * (1 + k0) + 3] : 1.f};
            for (int c = 0; c < 4; ++c) here[c] = a[c] + (b[c] - a[c]) * tz;
        }
        for (int c = 0; c < 4; ++c) out[c] += here[c] * weight[n];
    }
}

/* What the clouds leave of the sun at the eye, from a few places around it: their shadow where the eye is, soft. */
static float cloudShadow(const Sky* k) {
    if (k->toSun[1] <= 0) return 1.f;
    const float offsets[5][2] = {{0, 0}, {.25f, 0}, {-.25f, 0}, {0, .25f}, {0, -.25f}};
    float sum = 0;
    for (int o = 0; o < 5; ++o) {
        const float at[3] = {k->eye[0] + offsets[o][0], k->eye[1], k->eye[2] + offsets[o][1]};
        float a0, a1, b0, b1;
        if (!sphereSpan(at, k->toSun, EARTH + k->top, &b0, &b1)) {
            sum += 1.f;
            continue;
        }
        const float from = sphereSpan(at, k->toSun, EARTH + k->base, &a0, &a1) ? artfMaxf(a1, 0) : 0, to = artfMinf(b1, from + 30.f);
        const float step = artfMaxf(to - from, 0) / 32.f;
        float tau = 0;
        for (int i = 0; i < 32; ++i) {
            const float t = from + ((float)i + .5f) * step;
            const float p[3] = {at[0] + k->toSun[0] * t, at[1] + k->toSun[1] * t, at[2] + k->toSun[2] * t};
            tau += cloudAt(k, p, sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - EARTH, false) * step;
        }
        sum += expOf(-tau);
    }
    return sum / 5.f;
}

static void skyRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    const Sky* k = user;
    /* +y the top row, -z the middle column: as the environment's pictureAt reads it */
    const float theta = ((float)y + .5f) / (float)k->height * 3.14159265f;
    const float up = cosineOf(theta), across = sineOf(theta);
    for (int x = 0; x < k->width; ++x) {
        const float phi = (((float)x + .5f) / (float)k->width - .5f) * 6.28318531f;
        const float d[3] = {across * sineOf(phi), up, -across * cosineOf(phi)};
        float* out = k->rgb + 3 * ((size_t)y * k->width + x);
        if (!k->tables) {
            skyToward(k, d, out);
            continue;
        }
        skyTowardTables(k, d, out);
        if (k->coverage > 0) cloudsToward(k, d, (float)(mix32((uint32_t)y * (uint32_t)k->width + (uint32_t)x) >> 8) * (1.f / 16777216.f), out);
    }
}

bool artfSceneSetSky(ArtfScene* s, const ArtfSkyDesc* desc) {
    if (!desc) {
        artfFree(s->clouds);
        s->clouds = NULL;
        ++s->cloudsGeneration;
        return artfSceneSetEnvironment(s, NULL, 0, 0, 0, false);
    }
    Sky k;
    memset(&k, 0, sizeof k);
    const float haze = desc->haze > 0 ? desc->haze : 1.f;
    const float rayleigh[3] = {5.802e-3f, 13.558e-3f, 33.1e-3f}, ozone[3] = {.650e-3f, 1.881e-3f, .085e-3f};
    for (int c = 0; c < 3; ++c) {
        k.rayleigh[c] = rayleigh[c];
        k.ozone[c] = ozone[c];
    }
    k.mie = 3.996e-3f * haze;
    k.mieTaken = 4.4e-3f * haze;
    k.g = .8f;
    k.eye[1] = EARTH + (desc->altitude > 0 ? desc->altitude : 200.f) * .001f;
    memcpy(k.toSun, &s->toSun, 12);
    const ArtfVec3 sun = artfVec3Dot(desc->sun, desc->sun) > 0 ? desc->sun : artfVec3(3, 3, 3);
    const ArtfVec3 ground = artfVec3Dot(desc->ground, desc->ground) > 0 ? desc->ground : artfVec3(.1f, .1f, .1f);
    memcpy(k.sun, &sun, 12);
    memcpy(k.ground, &ground, 12);
    k.width = desc->width >= 16 ? artfMini(desc->width, 4096) : 256;
    k.height = artfMaxi(k.width / 2, 8);
    k.rgb = artfMalloc((size_t)k.width * k.height * 3 * sizeof(float));
    k.multiple = artfMaxf(desc->multiple, 0);
    k.disc = artfClampf(desc->sunDisc, 0, 20.f) * .5f * .017453293f;
    k.coverage = artfClampf(desc->clouds.coverage, 0, 1);
    k.tables = k.multiple > 0 || k.disc > 0 || k.coverage > 0;
    ArtfPool* pool = artfPoolNew(0);
    float through[3];
    float shadow = 1.f;
    if (k.tables) {
        k.trans = artfMalloc(TRANS_W * TRANS_H * 12);
        artfPoolRun(pool, transRow, &k, TRANS_H);
        if (k.multiple > 0) {
            k.multi = artfMalloc(MULTI_SIDE * MULTI_SIDE * 12);
            artfPoolRun(pool, multiRow, &k, MULTI_SIDE);
        }
        sunThroughTable(&k, k.eye, through);
        for (int c = 0; c < 3; ++c) k.eyeSun[c] = k.sun[c] * through[c];
        if (k.coverage > 0) {
            k.base = (desc->clouds.base > 0 ? desc->clouds.base : 1500.f) * .001f;
            k.top = k.base + (desc->clouds.thickness > 0 ? desc->clouds.thickness : 2500.f) * .001f;
            k.density = desc->clouds.density > 0 ? desc->clouds.density : 1.f;
            k.wind[0] = desc->clouds.wind.x * .001f;
            k.wind[1] = desc->clouds.wind.y * .001f;
            k.wind[2] = desc->clouds.wind.z * .001f;
            k.noise = artfMalloc((size_t)NOISE * NOISE * NOISE * 4);
            k.weather = artfMalloc((size_t)WEATHER * WEATHER * 4);
            artfPoolRun(pool, noiseSlice, &k, NOISE);
            artfPoolRun(pool, weatherRow, &k, WEATHER);
            /* the sky's light on them: the mean of overhead and four ways along the horizon, a little up */
            const float ways[5][3] = {{0, 1, 0}, {.98f, .2f, 0}, {-.98f, .2f, 0}, {0, .2f, .98f}, {0, .2f, -.98f}};
            for (int w = 0; w < 5; ++w) {
                float l[3];
                skyTowardTables(&k, ways[w], l);
                for (int c = 0; c < 3; ++c) k.ambient[c] += l[c] * .2f;
            }
            if (desc->clouds.shadow) shadow = cloudShadow(&k);
        }
    }
    artfPoolRun(pool, skyRow, &k, (uint32_t)k.height);
    artfPoolFree(pool);
    /* the clouds, kept for the scene's own (with aerial perspective: its units are what places them) */
    artfFree(s->clouds);
    s->clouds = NULL;
    ++s->cloudsGeneration;
    if (k.coverage > 0 && desc->aerial > 0) {
        float* c = artfCalloc(ARTF_CLOUD_FLOATS, 4);
        const float rows[ARTF_CLOUD_ROWS][4] = {{k.base, k.top, k.density, k.coverage}, {k.wind[0], k.wind[1], k.wind[2], desc->aerial * .001f},
                                                {k.toSun[0], k.toSun[1], k.toSun[2], k.eye[1]}, {k.sun[0], k.sun[1], k.sun[2], 0},
                                                {k.ambient[0], k.ambient[1], k.ambient[2], 0}};
        memcpy(c, rows, sizeof rows);
        memcpy(c + ARTF_CLOUD_NOISE, k.noise, (size_t)NOISE * NOISE * NOISE * 4);
        memcpy(c + ARTF_CLOUD_WEATHER, k.weather, (size_t)WEATHER * WEATHER * 4);
        memcpy(c + ARTF_CLOUD_TRANS, k.trans, (size_t)TRANS_W * TRANS_H * 12);
        s->clouds = c;
    }
    /* aerial perspective: the air at the eye's altitude, the sun as it reaches the eye (before clouds), and the mean
     * of the picture all around (the sky's light, scattered the same all ways) */
    memset(s->aerial, 0, sizeof s->aerial);
    if (desc->aerial > 0) {
        float dens[3], eyeSun[3], mean[3] = {0, 0, 0}, weight = 0;
        densities(k.eye[1] - EARTH, dens);
        if (k.tables) memcpy(eyeSun, k.eyeSun, 12);
        else {
            sunThrough(&k, k.eye, eyeSun);
            for (int c = 0; c < 3; ++c) eyeSun[c] *= k.sun[c];
        }
        for (int y = 0; y < k.height; ++y) {
            const float w = sineOf(((float)y + .5f) / (float)k.height * 3.14159265f);
            float row[3] = {0, 0, 0};
            for (int x = 0; x < k.width; ++x)
                for (int c = 0; c < 3; ++c) row[c] += k.rgb[((size_t)y * k.width + x) * 3 + c];
            for (int c = 0; c < 3; ++c) mean[c] += row[c] * w;
            weight += w * (float)k.width;
        }
        const float g = k.g, mieScale = .11936621f * (1.f - g * g) / (2.f + g * g);
        const float mie = k.mie * dens[1], unit = desc->aerial * .001f; /* (the tables are in kilometres) */
        for (int c = 0; c < 3; ++c) {
            const float rayleigh = k.rayleigh[c] * dens[0], taken = rayleigh + k.mieTaken * dens[1] + k.ozone[c] * dens[2];
            s->aerial[0][c] = taken * unit;
            s->aerial[1][c] = eyeSun[c] * 3.14159265f * rayleigh * .059683104f / taken;
            s->aerial[2][c] = eyeSun[c] * 3.14159265f * mie * mieScale / taken;
            s->aerial[3][c] = mean[c] / weight * (rayleigh + mie) / taken;
        }
        s->aerial[1][3] = g;
        s->aerial[3][3] = 1.f;
    }
    /* the sun as it reaches the eye: reddened through the air, and gone below the horizon; under clouds, dimmer */
    if (!desc->keepSun) {
        if (!k.tables) sunThrough(&k, k.eye, through);
        s->sunColor = artfVec3(sun.x * through[0] * shadow, sun.y * through[1] * shadow, sun.z * through[2] * shadow);
    }
    const bool ok = artfSceneSetEnvironment(s, k.rgb, k.width, k.height, 1.f, !desc->hidden);
    artfFree(k.rgb);
    artfFree(k.trans);
    artfFree(k.multi);
    artfFree(k.noise);
    artfFree(k.weather);
    return ok;
}
