/* artful — a sky from the sun: light scattered once by air (Rayleigh) and haze (Mie), and taken by ozone, along each
 * direction from a point above the ground, as Nishita et al. (1993) have it, with the constants Bruneton's and
 * Hillaire's skies use. It is made into a picture of all around, which the scene then has as its light from all
 * around: so both backends light by it, reflect it, show it and trace it, with nothing of their own.
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

typedef struct Sky {
    float rayleigh[3], mie, mieTaken, ozone[3], g;
    float eye[3], toSun[3], sun[3], ground[3];
    int width, height;
    float* rgb;
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

static void skyRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    const Sky* k = user;
    /* +y the top row, -z the middle column: as the environment's pictureAt reads it */
    const float theta = ((float)y + .5f) / (float)k->height * 3.14159265f;
    const float up = cosineOf(theta), across = sineOf(theta);
    for (int x = 0; x < k->width; ++x) {
        const float phi = (((float)x + .5f) / (float)k->width - .5f) * 6.28318531f;
        const float d[3] = {across * sineOf(phi), up, -across * cosineOf(phi)};
        skyToward(k, d, k->rgb + 3 * ((size_t)y * k->width + x));
    }
}

bool artfSceneSetSky(ArtfScene* s, const ArtfSkyDesc* desc) {
    if (!desc) return artfSceneSetEnvironment(s, NULL, 0, 0, 0, false);
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
    ArtfPool* pool = artfPoolNew(0);
    artfPoolRun(pool, skyRow, &k, (uint32_t)k.height);
    artfPoolFree(pool);
    /* the sun as it reaches the eye: reddened through the air, and gone below the horizon */
    if (!desc->keepSun) {
        float through[3];
        sunThrough(&k, k.eye, through);
        s->sunColor = artfVec3(sun.x * through[0], sun.y * through[1], sun.z * through[2]);
    }
    const bool ok = artfSceneSetEnvironment(s, k.rgb, k.width, k.height, 1.f, !desc->hidden);
    artfFree(k.rgb);
    return ok;
}
