/* artful — a black hole, behind the scene: an extension, built apart from the library (the target artful-blackhole,
 * in extensions/blackhole), on its background pass (artfSceneSetBackgroundPass). A program that has no black hole
 * links none of this. */
#ifndef ARTFUL_BLACKHOLE_H
#define ARTFUL_BLACKHOLE_H

#include "artful/scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Behind everything else in the scene: its light is traced along the paths Kerr's metric gives it,
 * pixel by pixel, through a glowing disc of gas and out to the stars, then goes through high dynamic
 * range, bloom and tone mapping. What the scene holds is drawn over it, and is not bent. Lengths given
 * in masses are multiples of mass: with no spin the horizon is at 2 and the photons' orbit at 3. */
typedef struct ArtfBlackHoleDesc {
    ArtfVec3 position;
    ArtfVec3 axis;       /* what the disc turns around; zero: +y */
    float mass;          /* in the world's lengths */
    float spin;          /* -1 to 1, of the most a hole can have; the same way round as the gas when above 0 */
    float discInner;     /* where the disc starts, in masses; 0: where orbits end, for this spin */
    float discOuter;     /* where it has faded out */
    float discHeight;    /* how deep it is, each way, per unit of radius */
    float discThickness; /* how much of the light behind it the disc stops */
    float discGain;      /* how bright it is */
    float doppler;       /* 0: the disc looks the same all around; 1: the side coming at the eye is as
                          * much brighter and bluer as relativity says */
    float stars;         /* how bright they are; 0: none */
    float bloom;         /* the veil of a lens: how much light spills around what is far too bright */
    float exposure;
    float time;          /* in masses: going forward, the gas flows; it cannot be run back */
    float shutter;       /* how much time a frame's exposure lasts: with several rays a pixel, motion blur */
    ArtfVec3 velocity;   /* the eye's, in the world, as a fraction of light's: what it sees moves toward
                          * where it is going, and with doppler, turns bluer there */
    int samples;         /* rays a pixel, on a side: 1 is one ray, 3 is nine */
} ArtfBlackHoleDesc;

/* A hole that looks like what cinema has shown of one: seen from outside, a thin bright disc, its far
 * side thrown up over the shadow and under it. */
ArtfBlackHoleDesc artfBlackHoleDefaults(void);
/* How fast a circular orbit goes at r masses, as a fraction of light's: for ArtfBlackHoleDesc.velocity. */
float artfBlackHoleOrbitSpeed(float spin, float r);
/* NULL: none. Call it every frame with a new time: the disc's gas is a fluid, and this is where it moves
 * on, in fixed steps, on the calling thread (a few milliseconds each). A change of the disc's size or of
 * the spin starts it over, which takes some time. It is the scene's background pass: it takes the place of
 * another, and another takes its place. */
void artfSceneSetBlackHole(ArtfScene* s, const ArtfBlackHoleDesc* desc);

#ifdef __cplusplus
}
#endif

#endif
