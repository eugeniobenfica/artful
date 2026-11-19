/* artful — drawing in two dimensions: pictures and shapes, lit. A library of its own (the target artful-2d, in
 * src/2d), that takes only Artful's core (memory, threads): a program that draws only in 2D links nothing of the 3D
 * renderer, and one that draws both links both.
 *
 * What is drawn: sprites (a picture, or a rectangle, a circle, a polygon or a line, of a color), in layers, under a
 * camera that moves, turns and zooms. What lights them: points, spots and a sun, with normal maps and highlights,
 * soft shadows from what is marked to cast them (as wide as their light), light that the scene's own glowing things
 * give off and that bounces off walls (radiance cascades), then high dynamic range, bloom and a tone curve.
 *
 * On the CPU, on every core: tiles of the frame drawn apart, each pixel blended in its layer's order, so that any
 * number of threads gives the same bytes, on x86 and on ARM. Or on the GPU, through Vulkan, by compute shaders built by
 * the library itself (no SDK), with the same arithmetic: the same picture, within a step or two of a byte.
 *
 * With 3D: a 2D frame is premultiplied BGRA, as a 3D one is, so it goes over a 3D scene as its overlay
 * (artfRendererSetOverlay), with a background of alpha 0; and a 3D frame goes into a 2D scene as a texture
 * (artfScene2DSetTexture with premultiplied), redrawn each frame.
 *
 * The world's y goes down, as the frame's does. Angles are in radians, clockwise on the screen. Colors are linear
 * light; bytes hold the square root of light, as everywhere in Artful. */
#ifndef ARTFUL_2D_H
#define ARTFUL_2D_H

#include "artful/core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ArtfVec2 { float x, y; } ArtfVec2;
ARTF_INLINE ArtfVec2 artfVec2(float x, float y) { ArtfVec2 v = {x, y}; return v; }

/* Handles are numbers; 0 is none. One that was removed stops working: calls with it do nothing, and say so through
 * artfSetLog. */
typedef uint32_t ArtfTexture2D;
typedef uint32_t ArtfSprite2D;
typedef uint32_t ArtfLight2D;
typedef uint32_t ArtfOccluder2D;

typedef struct ArtfScene2D ArtfScene2D;

ArtfScene2D* artfScene2DNew(void);
void artfScene2DFree(ArtfScene2D* s);

/* ---- textures ---------------------------------------------------------------------------- */

typedef struct ArtfTexture2DDesc {
    const uint32_t* pixels; /* BGRA; copied */
    int width, height;
    int stride;             /* in pixels; 0: width */
    bool premultiplied;     /* as a frame is (a 3D renderer's, or this one's); else straight, as a file's */
    bool normalMap;         /* x, y, z in red, green and blue, as 0..255 for -1..1, y up (as most tools make them),
                             * z towards the eye: read as they are, not as light */
} ArtfTexture2DDesc;

ArtfTexture2D artfScene2DAddTexture(ArtfScene2D* s, const ArtfTexture2DDesc* desc);
/* New pixels, of any size, for a texture: a 3D frame each frame, say. What uses it sees them from the next draw. */
void artfScene2DSetTexture(ArtfScene2D* s, ArtfTexture2D t, const ArtfTexture2DDesc* desc);
void artfScene2DRemoveTexture(ArtfScene2D* s, ArtfTexture2D t);

/* ---- sprites: what is drawn -------------------------------------------------------------- */

typedef enum ArtfShape2D {
    ARTF_SHAPE_RECT,    /* size wide and tall; corners rounded by cornerRadius */
    ARTF_SHAPE_ELLIPSE, /* size across: a circle when both are the same */
    ARTF_SHAPE_POLYGON, /* points, around position (concave too, with holes by winding: non-zero) */
    ARTF_SHAPE_LINE     /* from points[0] to points[1], thickness wide, with round ends */
} ArtfShape2D;

typedef struct ArtfSprite2DDesc {
    ArtfShape2D shape;
    ArtfVec2 position;    /* of the pivot, in the world */
    ArtfVec2 size;        /* rect and ellipse; a polygon's and a line's scale (0: 1) */
    ArtfVec2 pivot;       /* where position is on the shape, from -.5 (left, top) to .5; 0: its middle */
    float rotation;
    float cornerRadius;   /* rect */
    const ArtfVec2* points; /* polygon and line, in the shape's own space; copied */
    int pointCount;
    float thickness;      /* line */

    ArtfTexture2D texture;     /* 0: the color alone */
    ArtfVec2 uvMin, uvMax;     /* the part of the texture over the shape's box (a frame of a sheet); both 0: all */
    bool nearest;              /* texels as squares, for pixel art; else blended, from mipmaps when made smaller */
    ArtfTexture2D normalMap;   /* 0: the shape is flat, and faces the eye */
    ArtfTexture2D emissiveMap; /* 0: emissive all over */

    ArtfVec3 color;      /* times the texture; artfSprite2DDefaults: white */
    float opacity;       /* artfSprite2DDefaults: 1 */
    ArtfVec3 emissive;   /* its own light, times emissiveMap: shown as it is, and given off when gi is on */
    float specular;      /* 0..1: how much it reflects lights as a highlight (with a normal map) */
    float roughness;     /* of the highlight; artfSprite2DDefaults: .5 */

    int layer;           /* drawn from the lowest layer up; in a layer, in the order added */
    uint32_t lightMask;  /* the lights whose mask shares a bit with it light it; artfSprite2DDefaults: all */
    bool unlit;          /* its color as it is: an interface, a background */
    bool castsShadow;    /* its outline stops light, and gi's rays; its own pixels are lit as if it did not */
    bool hidden;
} ArtfSprite2DDesc;

/* A white rectangle of 1 by 1, opaque, lit, in layer 0. */
ArtfSprite2DDesc artfSprite2DDefaults(void);
ArtfSprite2D artfScene2DAddSprite(ArtfScene2D* s, const ArtfSprite2DDesc* desc);
void artfSprite2DSet(ArtfScene2D* s, ArtfSprite2D sprite, const ArtfSprite2DDesc* desc);
/* What moves each frame, without the rest. */
void artfSprite2DPlace(ArtfScene2D* s, ArtfSprite2D sprite, ArtfVec2 position, float rotation);
void artfScene2DRemoveSprite(ArtfScene2D* s, ArtfSprite2D sprite);

/* ---- occluders: what stops light without being drawn -------------------------------------- */

/* An outline in the world: closed, it is solid; open, a wall as thin as a line. Copied. */
ArtfOccluder2D artfScene2DAddOccluder(ArtfScene2D* s, const ArtfVec2* points, int count, bool closed);
void artfScene2DRemoveOccluder(ArtfScene2D* s, ArtfOccluder2D o);

/* ---- lights ------------------------------------------------------------------------------ */

typedef enum ArtfLight2DKind { ARTF_LIGHT2D_POINT, ARTF_LIGHT2D_SPOT, ARTF_LIGHT2D_SUN } ArtfLight2DKind;

typedef struct ArtfLight2DDesc {
    ArtfLight2DKind kind;
    ArtfVec2 position;
    ArtfVec3 color;      /* linear; a white surface right under a light of 1 shows 1 */
    float intensity;     /* times color; artfLight2DDefaults: 1 */
    float range;         /* point and spot: where it has faded to nothing */
    float radius;        /* how wide the light itself is: shadows as soft as it is wide. Sun: its angle, in radians */
    float height;        /* how far above the picture it is, for normal maps: low lights rake across them. 0: a
                          * quarter of its range. Sun: how steeply it comes down, as z of its direction (0: 1) */
    float direction;     /* spot: where it points; sun: where its light goes. 0: to +x */
    float coneInner;     /* spot: half angles; full light inside the inner, none past the outer */
    float coneOuter;
    bool shadows;
    uint32_t mask;       /* what it lights: sprites whose lightMask shares a bit; artfLight2DDefaults: all */
} ArtfLight2DDesc;

/* A white point light of 1, with a range of 256 and shadows. */
ArtfLight2DDesc artfLight2DDefaults(void);
ArtfLight2D artfScene2DAddLight(ArtfScene2D* s, const ArtfLight2DDesc* desc);
void artfLight2DSet(ArtfScene2D* s, ArtfLight2D light, const ArtfLight2DDesc* desc);
void artfScene2DRemoveLight(ArtfScene2D* s, ArtfLight2D light);

/* ---- the scene's light, its view, and what comes after ------------------------------------ */

/* What lights everything evenly. */
void artfScene2DSetAmbient(ArtfScene2D* s, ArtfVec3 color);

/* Light that glowing sprites give off and that bounces off what casts shadows, traced by radiance cascades
 * (Sannikov, 2023) through what is on the screen: soft, with the shadows walls throw from it. */
typedef struct ArtfGI2DDesc {
    bool enabled;
    int scale;        /* 1, 2, 4 or 8: one of its texels for this many pixels a side; artfGI2DDefaults: 4 */
    float intensity;  /* artfGI2DDefaults: 1 */
    float bounce;     /* 0..1: how much of the light on a wall it sends on, from one frame to the next */
} ArtfGI2DDesc;
ArtfGI2DDesc artfGI2DDefaults(void);
void artfScene2DSetGI(ArtfScene2D* s, const ArtfGI2DDesc* desc);

typedef enum ArtfTone2D { ARTF_TONE2D_CLAMP, ARTF_TONE2D_REINHARD, ARTF_TONE2D_ACES } ArtfTone2D;

typedef struct ArtfPost2DDesc {
    float exposure;    /* times the light; 0: 1 */
    ArtfTone2D tone;   /* how light past 1 rolls off */
    float bloom;       /* how much of what is bright spills around it; 0: none */
    float bloomRadius; /* how far, as a share of the frame's height; 0: .05 */
} ArtfPost2DDesc;
void artfScene2DSetPost(ArtfScene2D* s, const ArtfPost2DDesc* desc);

/* Where nothing is drawn: a color, as it is, and how opaque (0: the frame is clear there, for over a 3D one). */
void artfScene2DSetBackground(ArtfScene2D* s, ArtfVec3 color, float alpha);

typedef struct ArtfCamera2D {
    ArtfVec2 center;  /* what is at the middle of the frame */
    float zoom;       /* pixels for a unit of the world; 0: 1 */
    float rotation;
} ArtfCamera2D;
void artfScene2DSetCamera(ArtfScene2D* s, const ArtfCamera2D* camera);
/* From the frame's pixels to the world, as the last camera given places them; and back. */
ArtfVec2 artfScene2DToWorld(const ArtfScene2D* s, int frameWidth, int frameHeight, ArtfVec2 pixel);
ArtfVec2 artfScene2DToFrame(const ArtfScene2D* s, int frameWidth, int frameHeight, ArtfVec2 world);
/* The sprite on top at a point of the world, of those not hidden (0: none). */
ArtfSprite2D artfScene2DPick(const ArtfScene2D* s, ArtfVec2 world);

/* ---- drawing ----------------------------------------------------------------------------- */

typedef struct ArtfRenderer2D ArtfRenderer2D;

typedef enum ArtfBackend2D {
    ARTF_BACKEND2D_AUTO,     /* Vulkan where it runs, else the CPU */
    ARTF_BACKEND2D_SOFTWARE, /* the CPU, on every core */
    /* the GPU, by compute shaders: gi's cascades, the tiles, bloom and the tone curve. What is light work stays on the
     * CPU: the scene brought into the frame, and the lights' shadow maps. The frame comes back to memory, as the CPU's,
     * for a window, a file, or a 3D renderer's overlay. */
    ARTF_BACKEND2D_VULKAN
} ArtfBackend2D;

typedef struct ArtfRenderer2DOptions {
    int width, height;
    int threads; /* 0: as many as the process may use, at most 16; 1: none */
    ArtfBackend2D backend;
    /* Vulkan: a window to show frames in, through a swapchain of its own; they are then not read back unless asked
     * (artfRenderer2DReadPixels). The CPU does not use it: its frames are shown by the program. */
    ArtfSurface surface;
    bool noVsync; /* show frames at once, not at the screen's pace (Vulkan, with a surface) */
} ArtfRenderer2DOptions;

/* NULL only when a backend was asked for by name and cannot run here. */
ArtfRenderer2D* artfRenderer2DNew(const ArtfRenderer2DOptions* options);
ArtfBackend2D artfRenderer2DBackend(const ArtfRenderer2D* r);
/* What draws: "software, 16 threads", "AMD Radeon RX 9060 XT". Borrowed. */
const char* artfRenderer2DDescription(const ArtfRenderer2D* r);
void artfRenderer2DFree(ArtfRenderer2D* r);
void artfRenderer2DResize(ArtfRenderer2D* r, int width, int height);
/* A frame of the scene, shown in the surface if there is one. A renderer keeps what gi gathered from the frames before:
 * one renderer for each view. On Vulkan it does not wait for the GPU: two frames can be on their way, so that the CPU
 * makes the next while the GPU draws this one. false: the GPU was lost, and this frame was not shown; the CPU draws
 * from then on, into memory. */
bool artfRenderer2DDraw(ArtfRenderer2D* r, ArtfScene2D* s);
/* The last frame: width * height premultiplied BGRA, valid until the next draw or resize (on Vulkan, after waiting for
 * it). Borrowed. NULL on Vulkan with a surface: artfRenderer2DReadPixels. */
const uint32_t* artfRenderer2DPixels(ArtfRenderer2D* r);
/* The last frame into dst, width * height pixels, whatever the backend (on Vulkan with a surface, it is copied back
 * from the GPU for this). false: there is none. */
bool artfRenderer2DReadPixels(ArtfRenderer2D* r, uint32_t* dst);

typedef struct ArtfFrameStats2D {
    double cpuMs;     /* all of artfRenderer2DDraw */
    double gpuMs;     /* Vulkan: what the GPU took, by its timestamps, for the last frame it has finished */
    double shadowsMs; /* the lights' maps */
    double giMs;
    double drawMs;    /* the tiles: shapes, lit */
    double postMs;    /* bloom, the tone curve, bytes */
    uint32_t spritesDrawn, lightsDrawn, shadowMaps;
} ArtfFrameStats2D;
void artfRenderer2DStats(const ArtfRenderer2D* r, ArtfFrameStats2D* out);

#ifdef __cplusplus
}
#endif

#endif
