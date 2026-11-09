/* artful — a black hole: what it is, for both backends. The maps both read are made here, once; and
 * what a frame needs of the geometry around the eye, which is the same for all its rays. The light
 * itself is traced by each backend (software/lens.inc, vulkan/shaders.c).
 *
 * Nothing here calls pow, exp or the like: a C library's differ from one system to the next, and so
 * would every picture. Square roots are exact. */
#include "scene/sceneInternal.h"
#include "core/threads.h"

/* Where a row of the map is: rows are even in 1 / r, which spends them near the hole, where the disc
 * is hot and fine. */
static float rowRadius(float rho, float inner, float outer) { return inner / (1.f - rho * (1.f - inner / outer)); }

static uint32_t hashOf(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ seed * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    return h ^ h >> 15;
}

static float smooth(float a, float b, float x) {
    const float t = artfClampf((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

/* Noise that repeats every period in x: smooth steps between values on a lattice. */
static float noise(float x, float y, uint32_t period, uint32_t seed) {
    const float fx = floorf(x), fy = floorf(y);
    const float tx = x - fx, ty = y - fy;
    /* (through a signed integer: a negative float made unsigned is undefined in C) */
    const uint32_t x0 = (uint32_t)(int32_t)fx % period, x1 = (x0 + 1) % period, y0 = (uint32_t)(int32_t)fy, y1 = y0 + 1;
    const float a = (float)(hashOf(x0, y0, seed) >> 8), b = (float)(hashOf(x1, y0, seed) >> 8);
    const float c = (float)(hashOf(x0, y1, seed) >> 8), d = (float)(hashOf(x1, y1, seed) >> 8);
    const float sx = tx * tx * (3.f - 2.f * tx), sy = ty * ty * (3.f - 2.f * ty);
    const float top = a + (b - a) * sx, bottom = c + (d - c) * sx;
    return (top + (bottom - top) * sy) * (1.f / 16777216.f);
}

/* Octaves of noise, 0 to 1. */
static float layers(float x, float y, uint32_t period, float scaleY, float falloff, float stretch, int octaves, uint32_t seed) {
    float sum = 0, amplitude = .5f, total = 0;
    for (int o = 0; o < octaves; ++o) {
        sum += amplitude * noise(x * (float)period, y * scaleY, period, seed + (uint32_t)o);
        total += amplitude;
        amplitude *= falloff;
        period *= 2;
        scaleY *= stretch;
    }
    return sum / total;
}

/* e^x, by (1 + x / 2^20)^(2^20): only multiplications, so the same everywhere. */
static double exactExp(double x) {
    double v = 1.0 + x / 1048576.0;
    for (int i = 0; i < 20; ++i) v *= v;
    return v;
}

/* Planck's law at the three wavelengths a screen's colors stand for. */
static void planck(double kelvin, double* v) {
    static const double metres[3] = {611e-9, 549e-9, 464e-9};
    for (int c = 0; c < 3; ++c) {
        const double l = metres[c], l2 = l * l;
        v[c] = 1.0 / (l2 * l2 * l * (exactExp(.014388 / (l * kelvin)) - 1.0)); /* h c / (k l T) */
    }
}

/* What a black body at this temperature looks like on film balanced so that one at 6500 K is white, at
 * most 1. How bright it is is another matter: the gas's density says that. */
static void blackBody(double kelvin, float* rgb) {
    double v[3], white[3], most = 0;
    planck(kelvin, v);
    planck(6500.0, white);
    for (int c = 0; c < 3; ++c) {
        v[c] /= white[c];
        if (v[c] > most) most = v[c];
    }
    /* (film's dyes are not three wavelengths: they take the green down, and the blue up, toward salmon. Set
     * against the colors of James et al., 2015, figure 15a: dim gas orange, bright gas salmon going to pink) */
    v[1] *= .6;
    v[2] *= .72;
    for (int c = 0; c < 3; ++c) rgb[c] = (float)(v[c] / most);
}

/* A ridge: 1 on the lattice's middle values, 0 at its ends, sharpened: what makes strands of hair. */
static float ridge(float v) {
    const float r = 1.f - fabsf(2.f * v - 1.f);
    return r * r;
}

/* The gas as an artist would paint it, which is what the film's was (James et al., 2015, appendix A.6:
 * built in Houdini, with procedural detail for the close shots). Strands like hair along each orbit,
 * wandering in and out as they go around; clouds of it, whole near the hole and torn further out; and
 * its outer edge fraying away, strand by strand. Rows are even in 1 / r, as the map's. */
static float strands(float turn, float rho) {
    /* where a strand is wanders: slowly in and out, then a little faster */
    const float wander = .045f * (layers(turn, rho, 3, 5.f, .5f, 2.f, 4, 101) - .5f) + .012f * (layers(turn, rho, 11, 24.f, .5f, 2.f, 3, 107) - .5f);
    float along = turn + .03f * (layers(turn, rho, 5, 9.f, .5f, 2.f, 3, 113) - .5f);
    along -= floorf(along);
    const float across = rho + wander;
    float hair = 0, amplitude = .5f, total = 0, scale = 600.f;
    uint32_t period = 6;
    for (int o = 0; o < 6; ++o) {
        hair += amplitude * ridge(noise(along * (float)period, across * scale, period, 120u + (uint32_t)o));
        total += amplitude;
        amplitude *= .62f;
        period *= 2;
        scale *= 1.75f;
    }
    hair /= total;
    const float cloud = layers(along, across, 7, 34.f, .55f, 2.f, 5, 131);
    const float out = smooth(.85f, 1.f, rho);
    const float torn = smooth(.3f + .08f * out, .7f + .08f * out, cloud);
    const float gaps = 1.f + (torn - 1.f) * smooth(.1f, .6f, rho);
    const float edge = smooth(0.f, .02f, rho) * (1.f - smooth(.9f, 1.f, across));
    return edge * (.25f + 2.2f * hair) * (.04f + 1.5f * gaps);
}

/* A row of the painted gas. Each texel is its own: any thread draws the same bits. */
static void strandsRow(void* user, uint32_t y, uint32_t worker) {
    (void)worker;
    float* row = (float*)user + (size_t)y * ARTF_HOLE_MAP_WIDTH;
    const float rho = (float)y / (float)(ARTF_HOLE_MAP_HEIGHT - 1);
    /* thinner further out, as the square of 1 / r for the disc this is made for (one from the last
     * stable orbit of spin 0.6 out to 34): the outer gas lets the light behind it through */
    const float thin = 1.f - .887f * rho;
    for (int x = 0; x < ARTF_HOLE_MAP_WIDTH; ++x) {
        float turn = (float)x / (float)ARTF_HOLE_MAP_WIDTH + rho * .09f; /* (a slight winding) */
        turn -= floorf(turn);
        row[x] = strands(turn, rho) * thin * thin;
    }
}

static void buildDisc(float* map) {
    /* (two million texels, each some thirty lattice lookups: on all cores) */
    ArtfPool* pool = artfPoolNew(0);
    artfPoolRun(pool, strandsRow, map, ARTF_HOLE_MAP_HEIGHT);
    artfPoolFree(pool);
    /* scaled to a mean of 1 over the inner half, where most of the light is */
    double sum = 0;
    for (size_t i = 0; i < (size_t)ARTF_HOLE_MAP_WIDTH * (ARTF_HOLE_MAP_HEIGHT / 2); ++i) sum += map[i];
    const float scale = (float)((double)ARTF_HOLE_MAP_WIDTH * (ARTF_HOLE_MAP_HEIGHT / 2) / sum);
    for (size_t i = 0; i < (size_t)ARTF_HOLE_MAP_WIDTH * ARTF_HOLE_MAP_HEIGHT; ++i) map[i] *= scale;

    /* dust: lanes of it, along the orbits, from a little way out; none where the gas is hottest */
    float* dust = map + ARTF_HOLE_DUST_AT;
    for (int y = 0; y < ARTF_HOLE_DUST_HEIGHT; ++y) {
        const float rho = (float)y / (float)(ARTF_HOLE_DUST_HEIGHT - 1);
        const float outward = smooth(.25f, .8f, rho);
        for (int x = 0; x < ARTF_HOLE_DUST_WIDTH; ++x) {
            const float turn = (float)x / (float)ARTF_HOLE_DUST_WIDTH;
            const float lanes = layers(turn, rho, 4, 90.f, .55f, 2.f, 5, 151);
            dust[y * ARTF_HOLE_DUST_WIDTH + x] = outward * smooth(.45f, .7f, lanes);
        }
    }

    float* colors = map + ARTF_HOLE_COLORS_AT;
    for (int i = 0; i < ARTF_HOLE_COLORS; ++i) {
        /* (linear from 0, so that a shift of the light's frequency is a product of the heat) */
        const double kelvin = artfMaxf(ARTF_HOLE_KELVIN * (double)i / (ARTF_HOLE_COLORS / 2), 400.0);
        blackBody(kelvin, colors + i * 3);
    }

    /* the sky behind the stars: faint dust, heaped along a band, as in a galaxy seen from inside */
    float* sky = map + ARTF_HOLE_SKY_AT;
    for (int y = 0; y < ARTF_HOLE_SKY_HEIGHT; ++y) {
        const float height = (float)y / (float)(ARTF_HOLE_SKY_HEIGHT - 1);
        for (int x = 0; x < ARTF_HOLE_SKY_WIDTH; ++x) {
            const float turn = (float)x / (float)ARTF_HOLE_SKY_WIDTH;
            const float bent = height - .5f + .22f * (noise(turn * 2.f, 3.f, 2, 41) - .5f) + .1f * (noise(turn * 3.f, 5.f, 3, 42) - .5f);
            const float band = 1.f - smooth(0.f, .3f, fabsf(bent));
            const float dust = layers(turn, height, 8, 8.f, .58f, 2.f, 6, 51);
            sky[y * ARTF_HOLE_SKY_WIDTH + x] = .0006f + band * band * smooth(.3f, .75f, dust) * .006f;
        }
    }
}

/* The disc's one temperature, in kelvin, as the film's: James et al., 2015, section 4.1.1. */
#define HOLE_KELVIN 4500.f

/* Of each ring: how fast it turns, in turns per unit of time, around a hole of this spin; how hot it is,
 * all of it at the one temperature; and how much light its gas gives. The film's disc was brightest
 * near the hole and fell off slowly: most of it shows white, and only its thinning edges salmon. */
static void setRings(float* map, float inner, float outer, float spin) {
    float* rows = map + ARTF_HOLE_ROWS_AT;
    float* glow = map + ARTF_HOLE_GLOW_AT;
    for (int y = 0; y < ARTF_HOLE_MAP_HEIGHT; ++y) {
        const float r = rowRadius((float)y / (float)(ARTF_HOLE_MAP_HEIGHT - 1), inner, outer);
        rows[y * 2] = 1.f / (6.2831853f * (r * sqrtf(r) + spin));
        rows[y * 2 + 1] = HOLE_KELVIN / (float)ARTF_HOLE_KELVIN;
        /* (the map thins the gas as (inner / r)^2: what it gives goes down as (inner / r)^0.5 all told) */
        const float out = r / inner;
        glow[y] = 6.f * out * sqrtf(out);
    }
}

/* The cube root, by Newton. */
static double cubeRoot(double x) {
    if (x <= 0) return 0;
    double v = x > 1 ? x : 1;
    for (int i = 0; i < 60; ++i) v = (2.0 * v + x / (v * v)) / 3.0;
    return v;
}

float artfHoleIsco(float spin) {
    const double a = spin, a2 = a * a;
    const double z1 = 1.0 + cubeRoot(1.0 - a2) * (cubeRoot(1.0 + a) + cubeRoot(1.0 - a));
    const double z2 = sqrt(3.0 * a2 + z1 * z1);
    const double root = sqrt((3.0 - z1) * (3.0 + z1 + 2.0 * z2));
    return (float)(3.0 + z2 - (a < 0 ? -root : root));
}

float artfBlackHoleOrbitSpeed(float spin, float r) {
    /* an orbit's angular speed, against that of one who stands still in the turning space there */
    const double a = spin, r2 = (double)r * r, delta = r2 - 2.0 * r + a * a;
    const double big = (r2 + a * a) * (r2 + a * a) - a * a * delta;
    const double omega = 1.0 / ((double)r * sqrt((double)r) + a), drag = 2.0 * a * r / big;
    const double varpi = sqrt(big) / r, alpha = r * sqrt(delta) / sqrt(big);
    const double v = (omega - drag) * varpi / alpha;
    return (float)(v > .99 ? .99 : v);
}

ArtfBlackHoleDesc artfBlackHoleDefaults(void) {
    ArtfBlackHoleDesc d;
    memset(&d, 0, sizeof d);
    d.axis = artfVec3(0, -1, 0); /* (seen from above: the gas comes toward the eye on the left) */
    d.mass = 1.f;
    d.spin = .6f; /* (as the film's; and see James et al., 2015, for all of what follows) */
    d.discOuter = 34.f;
    d.discHeight = .03f;
    d.discThickness = 1.f;
    d.discGain = 2.1f;
    d.stars = .4f;
    d.bloom = 4.5f; /* (the veil of the film's lenses: James et al., 2015, figure 16) */
    d.exposure = 1.f;
    d.samples = 1;
    return d;
}

void artfSceneSetBlackHole(ArtfScene* s, const ArtfBlackHoleDesc* desc) {
    ArtfHole* h = &s->hole;
    if (!desc) { h->on = false; return; }
    const float spin = artfClampf(desc->spin, -.998f, .998f);
    const float horizon = 1.f + sqrtf(1.f - spin * spin);
    const float inner = desc->discInner > 0 ? artfMaxf(desc->discInner, horizon + .2f) : artfHoleIsco(spin);
    const float outer = artfMaxf(desc->discOuter, inner + 1.f);
    const bool sameDisc = h->map && h->desc.discInner == inner && h->desc.discOuter == outer;
    const bool sameSpin = sameDisc && h->desc.spin == spin;
    h->on = true;
    h->desc = *desc;
    h->desc.spin = spin;
    h->desc.mass = artfMaxf(desc->mass, 1e-6f);
    h->desc.discInner = inner;
    h->desc.discOuter = outer;
    h->desc.discHeight = artfMaxf(desc->discHeight, 1e-3f);
    h->desc.samples = artfClampi(desc->samples, 1, 8);
    if (!sameSpin) {
        const bool first = !h->map;
        if (first) h->map = artfMalloc(ARTF_HOLE_MAP_FLOATS * sizeof(float));
        if (first) buildDisc(h->map);
        setRings(h->map, inner, outer, spin);
        ++h->mapVersion;
        artfFluidFree(h->fluid);
        h->fluid = artfFluidNew(inner, outer, spin, desc->time);
        ++h->fluidVersion;
    } else if (artfFluidAdvance(h->fluid, desc->time)) {
        ++h->fluidVersion;
    }
}

void artfHoleFrame(const ArtfScene* s, int width, int height, ArtfHoleFrame* out, uint32_t* bloomPixels) {
    memset(out, 0, sizeof *out);
    const ArtfBlackHoleDesc* d = &s->hole.desc;
    const ArtfCamera* c = &s->camera;
    const float a = d->spin, horizon = 1.f + sqrtf(1.f - a * a);

    /* the hole's frame: the disc in xz */
    const ArtfVec3 given = artfVec3Length(d->axis) > 0 ? artfVec3Normalize(d->axis) : artfVec3(0, 1, 0);
    const ArtfVec3 other = fabsf(given.z) < .9f ? artfVec3(0, 0, 1) : artfVec3(1, 0, 0);
    const ArtfVec3 ex = artfVec3Normalize(artfVec3Cross(given, other)), ey = given, ez = artfVec3Cross(ex, ey);

    ArtfVec3 eye = artfVec3Scale(artfVec3Sub(c->position, d->position), 1.f / d->mass);
    float r = artfVec3Length(eye);
    const float least = horizon + .3f;
    if (r < least) { /* (no one looks out from inside: the eye stays just above the horizon) */
        eye = r > 0 ? artfVec3Scale(eye, least / r) : artfVec3(0, 0, least);
        r = least;
    }
    const float px = artfVec3Dot(eye, ex) / r, py = artfVec3Dot(eye, ey) / r, pz = artfVec3Dot(eye, ez) / r;

    /* Where the eye is, as a point on the unit sphere with its two directions along it, in the order
     * the tracing keeps: x, z, and the axis. What is on the axis itself has no azimuth: any will do. */
    const float sine = sqrtf(px * px + pz * pz);
    const float cx = sine > 1e-5f ? px / sine : 1.f, cz = sine > 1e-5f ? pz / sine : 0.f;
    const ArtfVec3 er = artfVec3(px, pz, py), etheta = artfVec3(py * cx, py * cz, -sine), ephi = artfVec3(-cz, cx, 0);
    out->er[0] = er.x, out->er[1] = er.y, out->er[2] = er.z, out->er[3] = r;
    out->etheta[0] = etheta.x, out->etheta[1] = etheta.y, out->etheta[2] = etheta.z;
    out->ephi[0] = ephi.x, out->ephi[1] = ephi.y, out->ephi[2] = ephi.z;

    /* Kerr's metric at the eye. The one who looks is at rest in the space that turns there: the frame
     * that sees nothing odd locally. */
    const float mu = py, r2 = r * r, a2 = a * a;
    const float sigma = r2 + a2 * mu * mu, delta = r2 - 2.f * r + a2;
    const float big = (r2 + a2) * (r2 + a2) - a2 * delta * sine * sine;
    const float rho = sqrtf(sigma), rootDelta = sqrtf(delta), rootBig = sqrtf(big);
    const float varpiAxis = rootBig / rho; /* the circumference's radius, per sine of the angle to the axis */
    out->metric[0] = rho * rootDelta / rootBig;            /* the lapse */
    out->metric[1] = 2.f * a * r / big * varpiAxis * sine; /* how fast space turns there, times the radius */
    out->metric[2] = varpiAxis * sine;
    out->metric[3] = varpiAxis;
    out->place[0] = rho;
    out->place[1] = rootDelta;
    out->place[2] = 1.f / r;
    out->place[3] = mu;
    out->spin[0] = a;
    out->spin[1] = .995f / horizon;
    out->spin[2] = d->discHeight;
    out->spin[3] = d->shutter;
    out->flow[0] = (float)artfFluidTime(s->hole.fluid);

    /* the camera's three axes and its speed, along the directions there */
    const ArtfVec3 forward = artfVec3Normalize(artfVec3Sub(c->target, c->position));
    const ArtfVec3 right = artfVec3Normalize(artfVec3Cross(forward, c->up));
    const ArtfVec3 up = artfVec3Cross(right, forward);
    const float tanY = tanf(c->fovY * .5f), tanX = tanY * (float)width / (float)artfMaxi(height, 1);
#define LOCAL(v, o, w) do { \
        const ArtfVec3 h_ = artfVec3(artfVec3Dot(v, ex), artfVec3Dot(v, ez), artfVec3Dot(v, ey)); \
        (o)[0] = artfVec3Dot(h_, er), (o)[1] = artfVec3Dot(h_, etheta), (o)[2] = artfVec3Dot(h_, ephi), (o)[3] = (w); \
    } while (0)
    const ArtfVec3 rightScaled = artfVec3Scale(right, tanX), upScaled = artfVec3Scale(up, tanY);
    LOCAL(rightScaled, out->right, 0.f);
    LOCAL(upScaled, out->up, 0.f);
    LOCAL(forward, out->forward, 0.f);
    float speed = artfVec3Length(d->velocity);
    const ArtfVec3 velocity = speed > .99f ? artfVec3Scale(d->velocity, .99f / speed) : d->velocity;
    speed = artfMinf(speed, .99f);
    LOCAL(velocity, out->boost, 1.f / sqrtf(1.f - speed * speed));
#undef LOCAL

    out->disc[0] = d->discInner;
    out->disc[1] = d->discOuter;
    out->disc[2] = d->time;
    out->disc[3] = d->discThickness;
    out->look[0] = d->discGain;
    out->look[1] = d->doppler;
    out->look[2] = d->stars;
    out->look[3] = d->bloom;
    out->image[0] = d->exposure;
    out->image[1] = 2.f / (float)width;
    out->image[2] = 2.f / (float)height;
    out->image[3] = 1.f / (float)(d->samples * d->samples);
    out->size[0] = (uint32_t)width;
    out->size[1] = (uint32_t)height;
    out->size[2] = (uint32_t)d->samples;

    uint32_t w = (uint32_t)width, h = (uint32_t)height, at = 0, levels = 0;
    /* (there is always one, however small the frame: tone mapping reads it) */
    while (levels < ARTF_BLOOM_LEVELS && (!levels || (w > 3 && h > 3))) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        out->bloom[levels][0] = at;
        out->bloom[levels][1] = w;
        out->bloom[levels][2] = h;
        at += w * h;
        ++levels;
    }
    out->size[3] = levels;
    *bloomPixels = at;
}
