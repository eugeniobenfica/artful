/* artful — the gas of a black hole's disc, as a fluid.
 *
 * Compressible, isothermal hydrodynamics in the disc's plane, on rings: density, and the velocity that
 * departs from a circular orbit (vr along the radius, w along the orbit). With Omega a ring's angular
 * speed and kappa the frequency at which what is pushed off it swings back,
 *
 *     dvr/dt = 2 Omega w + w^2 / r - c^2 d(ln S)/dr
 *     dw/dt  = -(kappa^2 / 2 Omega) vr - vr w / r - (c^2 / r) d(ln S)/dphi
 *     dS/dt  = -S div(v)
 *
 * Omega and kappa are those of Kerr's metric, so kappa falls to 0 at the last stable orbit: what comes
 * near it is no longer held, and falls. That is the one way general relativity enters here: the rest is
 * Newton's fluid. There is no magnetic field. What stirs a real disc is magnetic (the magnetorotational
 * instability), and needs three dimensions; here eddies are stirred in at random, as is usual in
 * two-dimensional studies of such turbulence, and the flow does what it will with them.
 *
 * Most of a ring's motion is its orbit, and the rings slide past each other far faster than anything
 * else happens. Moved by plain interpolation, all detail would soon be smeared away. So, as in the
 * FARGO scheme, a step moves what departs from an orbit first, a little at a time, and then shifts each
 * ring along itself by its own orbit, which keeps the shear sharp.
 *
 * It runs on the CPU, for both backends: what they are given is the same, and so is the picture. Only
 * + - * / and comparisons are used, in an order that does not depend on threads: the flow is chaotic, and
 * any difference would grow. So the same steps give the same bytes on any processor. */
#include "scene/sceneInternal.h"
#include "core/threads.h"

#define W ARTF_FLUID_WIDTH
#define H ARTF_FLUID_HEIGHT
#define STEP .2f           /* of time, in masses: within what sound allows on the finest ring */
#define DPHI (6.2831853f / (float)W)
#define MOST_STEPS 48      /* in one call: past that the gas does not try to catch up */
#define WARM_UP 60
#define SHEARED 900.f      /* how long the gas seems to have been turning when it starts */

struct ArtfFluid {
    float inner, outer;
    float* s[2];     /* density, around 1 */
    float* vr[2];
    float* w[2];
    float* field;    /* what the backends read */
    int now;         /* which of the two is current */
    float r[H], rowStep[H], omega[H], swing[H], sound[H], shift[H], envelope[H], fall[H];
    uint32_t seed;
    double time;
    ArtfPool* pool;
};

static uint32_t nextRandom(ArtfFluid* f) {
    uint32_t x = f->seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return f->seed = x;
}
static float random01(ArtfFluid* f) { return (float)(nextRandom(f) >> 8) * (1.f / 16777216.f); }

static float smooth(float a, float b, float x) {
    const float t = artfClampf((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

static uint32_t latticeHash(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ seed * 0xC2B2AE3Du;
    h = (h ^ h >> 15) * 0x2C1B3C6Du;
    h = (h ^ h >> 12) * 0x297A2D39u;
    return h ^ h >> 15;
}

/* Noise on a lattice, wrapping every period across x, 0 to 1. */
static float lattice(float x, float y, uint32_t period, uint32_t seed) {
    const float fx = floorf(x), fy = floorf(y), tx = x - fx, ty = y - fy;
    const uint32_t x0 = (uint32_t)fx & (period - 1), x1 = (x0 + 1) & (period - 1), y0 = (uint32_t)fy, y1 = y0 + 1;
    const float a = (float)(latticeHash(x0, y0, seed) >> 8), b = (float)(latticeHash(x1, y0, seed) >> 8);
    const float c = (float)(latticeHash(x0, y1, seed) >> 8), d = (float)(latticeHash(x1, y1, seed) >> 8);
    const float sx = tx * tx * (3.f - 2.f * tx), sy = ty * ty * (3.f - 2.f * ty);
    const float top = a + (b - a) * sx, bottom = c + (d - c) * sx;
    return (top + (bottom - top) * sy) * (1.f / 16777216.f);
}

/* A field at (x, y) in cells: around the ring it wraps, across rings it stops. */
static inline float sample(const float* a, float x, float y) {
    const float fy = artfClampf(y, 0.f, (float)(H - 1) - .001f);
    const int y0 = (int)fy;
    const float py = fy - (float)y0;
    const float fx = floorf(x), px = x - fx;
    const int x0 = (int)fx & (W - 1), x1 = (x0 + 1) & (W - 1);
    const float* r0 = a + (size_t)y0 * W;
    const float* r1 = r0 + W;
    const float top = r0[x0] + (r0[x1] - r0[x0]) * px, bottom = r1[x0] + (r1[x1] - r1[x0]) * px;
    return top + (bottom - top) * py;
}

/* Velocities: moved by themselves, then pushed. Each new value is used at once, which is what keeps
 * orbits and sound stable at this step. */
static void velocityRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    ArtfFluid* f = user;
    const int j = (int)y, n = f->now;
    const float* s = f->s[n];
    const float* vr = f->vr[n];
    const float* w = f->w[n];
    float* outR = f->vr[n ^ 1] + (size_t)j * W;
    float* outW = f->w[n ^ 1] + (size_t)j * W;
    const float r = f->r[j], invR = 1.f / r, c2 = f->sound[j], omega = f->omega[j], swing = f->swing[j];
    const float toCells = STEP / (r * DPHI), toRows = STEP / f->rowStep[j];
    const int up = artfMini(j + 1, H - 1), down = artfMaxi(j - 1, 0);
    const float invSpan = 1.f / (f->r[up] - f->r[down]), invArc = 1.f / (2.f * r * DPHI);
    /* (what the flow drags on: the turbulence's viscosity, in short) */
    const float keep = 1.f - STEP * .04f * omega;
    for (int i = 0; i < W; ++i) {
        const size_t at = (size_t)j * W + i;
        const float x = (float)i - w[at] * toCells, yy = (float)j - vr[at] * toRows;
        const float v0 = sample(vr, x, yy), w0 = sample(w, x, yy);
        const float here = s[at], inv = 1.f / here;
        const float alongR = (s[(size_t)up * W + i] - s[(size_t)down * W + i]) * invSpan * inv;
        const float alongPhi = (s[(size_t)j * W + ((i + 1) & (W - 1))] - s[(size_t)j * W + ((i - 1) & (W - 1))]) * invArc * inv;
        const float w1 = w0 + STEP * (-swing * v0 - v0 * w0 * invR - c2 * alongPhi);
        const float v1 = v0 + STEP * (2.f * omega * w1 + w1 * w1 * invR - c2 * alongR);
        outW[i] = artfClampf(w1 * keep, -.4f, .4f);
        outR[i] = artfClampf(v1 * keep, -.4f, .4f);
    }
}

/* Density: moved, and thinned or heaped by how the new velocities spread. */
static void densityRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    ArtfFluid* f = user;
    const int j = (int)y, n = f->now;
    const float* s = f->s[n];
    const float* vr = f->vr[n ^ 1];
    const float* w = f->w[n ^ 1];
    float* out = f->s[n ^ 1] + (size_t)j * W;
    const float r = f->r[j], invR = 1.f / r;
    const float toCells = STEP / (r * DPHI), toRows = STEP / f->rowStep[j];
    const int up = artfMini(j + 1, H - 1), down = artfMaxi(j - 1, 0);
    const float rUp = f->r[up], rDown = f->r[down], invSpan = 1.f / (rUp - rDown), invArc = 1.f / (2.f * r * DPHI);
    /* (the disc is fed from outside and drains in: gas drifts inward, and rings fill back slowly) */
    const float drift = -.004f, refill = STEP * .002f * f->omega[j];
    for (int i = 0; i < W; ++i) {
        const size_t at = (size_t)j * W + i;
        const float x = (float)i - w[at] * toCells, yy = (float)j - (vr[at] + drift) * toRows;
        const float moved = sample(s, x, yy);
        const float spread = (rUp * vr[(size_t)up * W + i] - rDown * vr[(size_t)down * W + i]) * invSpan * invR +
                             (w[(size_t)j * W + ((i + 1) & (W - 1))] - w[(size_t)j * W + ((i - 1) & (W - 1))]) * invArc;
        const float next = moved * (1.f - STEP * artfClampf(spread, -2.f, 2.f));
        out[i] = artfClampf(next + (1.f - next) * refill, .03f, 8.f);
    }
}

/* A row along itself by a fraction of cells, by a Catmull-Rom cubic that does not overshoot its
 * neighbours: it reads at i - cells, which falls between b and b + 1. */
static void shiftOne(const float* in, float* out, float cells) {
    const float whole = floorf(cells), t = 1.f - (cells - whole), t2 = t * t, t3 = t2 * t;
    const int b = -(int)whole - 1;
    const float w0 = -.5f * t + t2 - .5f * t3, w1 = 1.f - 2.5f * t2 + 1.5f * t3, w2 = .5f * t + 2.f * t2 - 1.5f * t3, w3 = -.5f * t2 + .5f * t3;
    for (int i = 0; i < W; ++i) {
        const float p0 = in[(i + b - 1) & (W - 1)], p1 = in[(i + b) & (W - 1)], p2 = in[(i + b + 1) & (W - 1)], p3 = in[(i + b + 2) & (W - 1)];
        out[i] = artfClampf(w0 * p0 + w1 * p1 + w2 * p2 + w3 * p3, artfMinf(p1, p2), artfMaxf(p1, p2));
    }
}

static void shiftRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    ArtfFluid* f = user;
    const int n = f->now ^ 1;
    const size_t at = (size_t)y * W;
    const float cells = f->shift[y];
    shiftOne(f->s[n] + at, f->s[n ^ 1] + at, cells);
    shiftOne(f->vr[n] + at, f->vr[n ^ 1] + at, cells);
    shiftOne(f->w[n] + at, f->w[n ^ 1] + at, cells);
}

/* What shows: where the density falls below what the disc's thinning asks for, it tears. */
static void fieldRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    ArtfFluid* f = user;
    const float* s = f->s[f->now] + (size_t)y * W;
    float* out = f->field + (size_t)y * W;
    const float envelope = f->envelope[y], fall = f->fall[y];
    for (int i = 0; i < W; ++i) {
        const float v = s[i] * envelope;
        out[i] = fall * smooth(.1f, 1.1f, v) * artfMinf(v, 2.2f);
    }
}

/* An eddy: a little whirl of velocity, with a clump of gas at its heart. The disc's shear does the rest. */
static void stir(ArtfFluid* f, bool small) {
    float* s = f->s[f->now];
    float* vr = f->vr[f->now];
    float* w = f->w[f->now];
    const float rho = random01(f), r0 = f->inner + (f->outer - f->inner) * rho * rho;
    int j0 = 0;
    while (j0 < H - 1 && f->r[j0] < r0) ++j0;
    const int i0 = (int)(nextRandom(f) & (W - 1));
    const float size = r0 * (small ? .006f + .02f * random01(f) : .02f + .1f * random01(f));
    const float c = sqrtf(f->sound[j0]);
    const float strength = small ? 0.f : (random01(f) < .5f ? -1.f : 1.f) * c * (.3f + .7f * random01(f));
    const float clump = (random01(f) - .42f) * (small ? 1.5f : .5f);
    const float invSize2 = 1.f / (size * size);
    const int reachX = artfMini((int)(2.5f * size / (r0 * DPHI)) + 1, W / 2 - 1);
    for (int j = 0; j < H; ++j) {
        const float dr = f->r[j] - r0;
        if (dr * dr > 6.25f * size * size) continue;
        for (int di = -reachX; di <= reachX; ++di) {
            const float dx = (float)di * f->r[j] * DPHI;
            const float d2 = (dr * dr + dx * dx) * invSize2;
            if (d2 > 6.25f) continue;
            const float g = 1.f - d2 * .16f, shape = g * g * g;
            const size_t at = (size_t)j * W + (size_t)((i0 + di) & (W - 1));
            vr[at] -= strength * shape * dx / size;
            w[at] += strength * shape * dr / size;
            s[at] = artfClampf(s[at] * (1.f + clump * shape), .03f, 8.f);
        }
    }
}

/* The shear wipes out all that varies around a ring, and so what is left of any gas, in time, is rings.
 * Real discs are kept from that by turbulence that three dimensions and a magnetic field would give; here
 * each ring's mean is drawn back to 1, which leaves its threads alone. */
static void balanceRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    ArtfFluid* f = user;
    float* s = f->s[f->now] + (size_t)y * W;
    float sum = 0;
    for (int i = 0; i < W; ++i) sum += s[i];
    const float mean = sum * (1.f / (float)W), scale = 1.f + .03f * (1.f / mean - 1.f);
    for (int i = 0; i < W; ++i) s[i] = artfClampf(s[i] * scale, .03f, 8.f);
}

static void step(ArtfFluid* f) {
    for (int i = 0; i < 2; ++i) stir(f, false);
    /* (and new clumps, small, for the shear to draw into new threads) */
    for (int i = 0; i < 14; ++i) stir(f, true);
    artfPoolRun(f->pool, velocityRow, f, H);
    artfPoolRun(f->pool, densityRow, f, H);
    /* (the shift goes from the new values back into the current ones: now does not change) */
    artfPoolRun(f->pool, shiftRow, f, H);
    artfPoolRun(f->pool, balanceRow, f, H);
    f->time += STEP;
}

void artfFluidFree(ArtfFluid* f) {
    if (!f) return;
    artfPoolFree(f->pool);
    for (int k = 0; k < 2; ++k) {
        artfFree(f->s[k]);
        artfFree(f->vr[k]);
        artfFree(f->w[k]);
    }
    artfFree(f->field);
    artfFree(f);
}

ArtfFluid* artfFluidNew(float inner, float outer, float spin, double time) {
    ArtfFluid* f = ARTF_NEW(ArtfFluid);
    f->inner = inner;
    f->outer = outer;
    f->seed = 0x9E3779B9u;
    f->time = time;
    f->pool = artfPoolNew(0);
    const size_t cells = (size_t)W * H;
    for (int k = 0; k < 2; ++k) {
        f->s[k] = artfMalloc(cells * 4);
        f->vr[k] = artfCalloc(cells, 4);
        f->w[k] = artfCalloc(cells, 4);
    }
    f->field = artfMalloc(cells * 4);

    const float k = 1.f - inner / outer;
    for (int j = 0; j < H; ++j) {
        const float rho = (float)j / (float)(H - 1), r = inner / (1.f - rho * k);
        f->r[j] = r;
        const float root = sqrtf(r), omega = 1.f / (r * root + spin);
        f->omega[j] = omega;
        /* Kerr's epicyclic frequency: squared, it goes through 0 at the last stable orbit */
        const float kappa2 = omega * omega * (1.f - 6.f / r + 8.f * spin / (r * root) - 3.f * spin * spin / (r * r));
        f->swing[j] = artfMaxf(kappa2, -.5f * omega * omega) / (2.f * omega);
        /* sound: the disc's thickness times the orbit's speed */
        const float c = .03f * r * omega;
        f->sound[j] = c * c;
        f->shift[j] = omega * STEP / DPHI;
        f->envelope[j] = smooth(inner + .5f, inner + 2.2f, r) * (1.f - .86f * smooth(outer * .12f, outer * .8f, r));
        const float ratio = inner / r;
        f->fall[j] = sqrtf(ratio);
    }
    for (int j = 0; j < H; ++j) f->rowStep[j] = (f->r[artfMini(j + 1, H - 1)] - f->r[artfMaxi(j - 1, 0)]) * (j == 0 || j == H - 1 ? 1.f : .5f);

    /* It starts as clumps of gas that the shear has already been drawing out for a long time: that is
     * a few thousand steps' work, and what it does to a clump is known in advance, each ring having
     * simply turned by its own orbit. Then it works up its own turbulence from there. */
    float* s = f->s[0];
    for (int j = 0; j < H; ++j) {
        const float rho = (float)j / (float)(H - 1), wound = f->omega[j] * SHEARED / 6.2831853f;
        for (int i = 0; i < W; ++i) {
            float turn = (float)i / (float)W - wound;
            turn -= floorf(turn);
            float sum = 0, amplitude = .5f, total = 0;
            uint32_t period = 16;
            float across = 14.f;
            for (uint32_t o = 0; o < 5; ++o) {
                sum += amplitude * lattice(turn * (float)period, rho * across, period, 7 + o);
                total += amplitude;
                amplitude *= .72f;
                period *= 2;
                across *= 2.f;
            }
            const float v = sum / total;
            s[(size_t)j * W + i] = .06f + 3.4f * smooth(.34f, .68f, v) * smooth(.34f, .68f, v);
        }
    }
    for (int i = 0; i < 200; ++i) stir(f, false);
    for (int i = 0; i < WARM_UP; ++i) step(f);
    f->time = time;
    artfPoolRun(f->pool, fieldRow, f, H);
    return f;
}

bool artfFluidAdvance(ArtfFluid* f, double time) {
    if (time - f->time < STEP) {
        /* behind: gas does not run back; the rings' turning alone covers what is near */
        if (time < f->time - 8.0) f->time = time;
        return false;
    }
    int steps = (int)((time - f->time) / STEP);
    if (steps > MOST_STEPS) {
        f->time = time - (double)MOST_STEPS * STEP;
        steps = MOST_STEPS;
    }
    for (int i = 0; i < steps; ++i) step(f);
    artfPoolRun(f->pool, fieldRow, f, H);
    return true;
}

const float* artfFluidField(const ArtfFluid* f) { return f->field; }
double artfFluidTime(const ArtfFluid* f) { return f->time; }
