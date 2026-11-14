/* artful — a black hole: what its parts share, and what the tests look at. Not public. */
#ifndef ARTFUL_HOLE_INTERNAL_H
#define ARTFUL_HOLE_INTERNAL_H

#include "artful/artful.h"
#include "artful/blackhole.h"

/* The disc's gas: rings of it, each a row, so that each can turn at the speed of its own orbit. */
#define ARTF_HOLE_MAP_WIDTH 2048
#define ARTF_HOLE_MAP_HEIGHT 1024
#define ARTF_HOLE_DUST_WIDTH 1024 /* where the gas gives less light than it stops: dark lanes */
#define ARTF_HOLE_DUST_HEIGHT 256
#define ARTF_FLUID_WIDTH 1024  /* the disc's gas, as a fluid: around each ring (a power of two) */
#define ARTF_FLUID_HEIGHT 256  /* and rings */
#define ARTF_HOLE_COLORS 256
#define ARTF_HOLE_SKY_WIDTH 1024
#define ARTF_HOLE_SKY_HEIGHT 512

/* What a frame needs to know of a black hole: the background pass's params, which the GPU's shader reads from
 * the 17th vec4 of binding 11 on. In units of its mass. */
typedef struct ArtfHoleFrame {
    /* where the eye is on the unit sphere, and the two directions along it: x, z, and the axis */
    float er[4];      /* w: its distance */
    float etheta[4], ephi[4];
    /* a pixel's ray, forward + x right + y up with x and y in -1..1: along r, theta and phi at the eye */
    float right[4], up[4], forward[4];
    float metric[4];  /* at the eye: the lapse, space's turning times the radius, that radius, and per sine */
    float place[4];   /* sqrt(Sigma), sqrt(Delta), 1 / r, cos(theta) */
    float spin[4];    /* spin, the 1 / r of the horizon, the disc's height, the shutter */
    float flow[4];    /* the moment the fluid's field is of */
    float boost[4];   /* the eye's speed along r, theta and phi; w: gamma */
    float disc[4];    /* inner, outer, time, thickness */
    float look[4];    /* gain, doppler, stars, 0 */
    float image[4];   /* 0, 2 / width, 2 / height, 1 / rays a pixel */
    uint32_t size[4]; /* width, height, rays a side, 0 */
} ArtfHoleFrame;
#define ARTF_HOLE_PARAMS_AT 16 /* where the params start in binding 11, in vec4s: at byte 256 */

/* What both backends read, in one block of floats: the gas's finest detail, below what the fluid
 * resolves; of each row its turns per unit of time and its heat; a black body's colors, by temperature;
 * the dust of the sky; of each row how much light its gas gives. */
#define ARTF_HOLE_ROWS_AT (ARTF_HOLE_MAP_WIDTH * ARTF_HOLE_MAP_HEIGHT)
#define ARTF_HOLE_COLORS_AT (ARTF_HOLE_ROWS_AT + ARTF_HOLE_MAP_HEIGHT * 2)
#define ARTF_HOLE_SKY_AT (ARTF_HOLE_COLORS_AT + ARTF_HOLE_COLORS * 3)
#define ARTF_HOLE_GLOW_AT (ARTF_HOLE_SKY_AT + ARTF_HOLE_SKY_WIDTH * ARTF_HOLE_SKY_HEIGHT)
#define ARTF_HOLE_DUST_AT (ARTF_HOLE_GLOW_AT + ARTF_HOLE_MAP_HEIGHT)
#define ARTF_HOLE_MAP_FLOATS (ARTF_HOLE_DUST_AT + ARTF_HOLE_DUST_WIDTH * ARTF_HOLE_DUST_HEIGHT)
/* How much the fluid says of the gas's density (the painted map says the rest: the fluid is coarser than
 * what shows, and it moves the gas); and dust, which gives this much less light and stops this much more:
 * dark lanes, against the gas behind. */
#define ARTF_HOLE_FLUID .1f
#define ARTF_HOLE_DUST_DIM .97f
#define ARTF_HOLE_DUST_DENSE 25.f
/* A heat of 1 is this, in kelvin: the middle of the table of colors, which goes up linearly from 0. */
#define ARTF_HOLE_KELVIN 6500.0

typedef struct ArtfFluid ArtfFluid;

/* A scene's hole: the user of its background pass. */
typedef struct ArtfHole {
    ArtfBlackHoleDesc desc;
    float* map;
    uint64_t mapVersion;
    ArtfFluid* fluid;
    uint64_t fluidVersion; /* what the field holds: each step changes it */
    uint32_t* shader;      /* the GPU's (holeShader.c) */
    size_t shaderBytes;
} ArtfHole;

/* The scene's hole; NULL: it has none (its background pass is another, or there is none). */
ArtfHole* artfBlackHoleOf(const ArtfScene* s);

/* The disc's gas as a fluid (fluid.c). New: warmed up, at that moment. Advance: to that moment, in fixed
 * steps; true when the field changed. The field: ARTF_FLUID_WIDTH * ARTF_FLUID_HEIGHT floats, rows even in
 * 1 / r as the map's are, of what shows of the gas; what moment it is of. */
ArtfFluid* artfFluidNew(float inner, float outer, float spin, double time);
void artfFluidFree(ArtfFluid* f);
bool artfFluidAdvance(ArtfFluid* f, double time);
const float* artfFluidField(const ArtfFluid* f);
double artfFluidTime(const ArtfFluid* f);

/* Where the last stable orbit is around a hole of this spin, in masses. */
float artfHoleIsco(float spin);
/* What a frame of that size needs of the black hole, from the camera. */
void artfHoleFrame(const ArtfHole* hole, const ArtfCamera* camera, int width, int height, ArtfHoleFrame* out);

/* The light of a rectangle (lens.inc), built once for each instruction set, as the library's kernels are. */
typedef void (*ArtfHoleRows)(const ArtfHoleFrame* h, const float* map, const float* fluid, const ArtfBackgroundRows* rows);
void artfHoleRowsPortable(const ArtfHoleFrame* h, const float* map, const float* fluid, const ArtfBackgroundRows* rows);
void artfHoleRowsSse41(const ArtfHoleFrame* h, const float* map, const float* fluid, const ArtfBackgroundRows* rows);
void artfHoleRowsAvx2(const ArtfHoleFrame* h, const float* map, const float* fluid, const ArtfBackgroundRows* rows);
void artfHoleRowsNeon(const ArtfHoleFrame* h, const float* map, const float* fluid, const ArtfBackgroundRows* rows);

/* The same light on the GPU (holeShader.c): SPIR-V, the caller's (artfFree), and its size in bytes. NULL where the
 * library has no Vulkan. */
uint32_t* artfBlackHoleShader(size_t* size);

#endif
