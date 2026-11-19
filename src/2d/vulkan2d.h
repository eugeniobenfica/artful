/* artful-2d — the GPU: what the compute shaders read and write, as vulkan2d.c lays it out and shaders2d.c reads it.
 *
 * Bindings of the one descriptor set, each a storage buffer:
 *   0 globals: vec4s (G_...)          1 items: ITEM_ROWS vec4s each (I_...)      2 edges: a vec4 each, x0 y0 x1 y1
 *   3 tiles: four uints a tile (where its items start, how many, where its lights start, how many), then the indices
 *   4 lamps: LAMP_ROWS vec4s each      5 shadow maps: of each, bins floats near, bins owners, bins floats past (uints)
 *   6 texels: straight BGRA            7 texture levels: four uints each: first texel, width, height, a normal map
 *   8 gi's light: three floats a texel
 *   9 the chain: the frame in floats (premultiplied rgba, width x height), then bloom's levels (rgb)
 *  10 the frame: BGRA                 11 what walls send on, a pixel each: rgb and how many gave it
 *  12 the same, a texel of gi each, for the next frame's gi
 * And gi's, when it runs on the GPU (as gi2d.c):
 *  13 outlines: two vec4s each (where its points start, how many, closed; its box)   14 their points: x y in a vec4
 *  15 what glows: items' indices                                                      16 what is solid (uints)
 *  17 what each solid texel sends (vec4)    18 the distance to the nearest solid texel    19 the distances' scratch
 *  20, 21 the cascades' rays, rgb in a vec4, even and odd cascades                    22 gi's light, between passes
 *  23 binding 12 as it was after the frame before */
#ifndef ARTFUL_2D_VULKAN_H
#define ARTFUL_2D_VULKAN_H

#include "2d/internal2d.h"

enum {
    B2_GLOBALS, B2_ITEMS, B2_EDGES, B2_TILES, B2_LAMPS, B2_SHADOWS, B2_TEXELS, B2_LEVELS, B2_GI, B2_CHAIN, B2_PIXELS, B2_REFLECT, B2_CELLS,
    B2_OUTLINES, B2_POINTS, B2_EMITTERS, B2_SOLID, B2_SOURCE, B2_DIST, B2_SCRATCH, B2_RAYS_EVEN, B2_RAYS_ODD, B2_GI_TMP, B2_CELLS_PREV,
    B2_COUNT
};

/* globals */
enum {
    G_SIZE,    /* width, height, tiles across, giScale (uints) */
    G_AMBIENT, /* rgb, exposure */
    G_BACK,    /* the background, premultiplied */
    G_MODE,    /* the tone curve, gi's width and height, whether there is gi (uints) */
    G_FLAGS,   /* the frame in floats (bloom), what walls send on (uints); bloom: how much of the frame stays, of each level */
    G_GI,      /* gi's intensity, its bounce; how many outlines, how many items glow (uints) */
    G_OUT,     /* the frame's bytes with red and blue swapped, for an RGBA window (uint) */
    G_ROWS
};

/* an item */
enum {
    I_MAP0,   /* the frame's pixels to its own space: m0 m1 m2 m3 */
    I_MAP1,   /* m4 m5, zoom, shape (uint) */
    I_SHAPE,  /* half its width and height, its corners' radius, half a line's thickness */
    I_LINE,   /* a line's ends */
    I_UV,     /* u = x + y s, v = z + w t */
    I_COLOR,  /* rgb, opacity */
    I_EMIT,   /* rgb, specular */
    I_TURN,   /* roughness, turn cos and sin, light mask (uint) */
    I_FLAGS,  /* flags (I2_...), owner, first edge, edges (uints) */
    I_TEX,    /* the levels of its texture, normal map, emissive map (uints; ~0: none) */
    I_BOX,    /* the pixels it covers: x0 y0 x1 y1 (uints) */
    ITEM_ROWS
};
enum { I2_LIT = 1, I2_NEAREST = 2, I2_POLYGON = 4, I2_EMITS = 8 };

/* a lamp */
enum {
    L_PLACE,  /* kind, x, y, mask (kind and mask uints) */
    L_COLOR,  /* rgb, range squared */
    L_REACH,  /* 1 / range squared, radius, height, a shadow map (uint) */
    L_DIR,    /* dirX, dirY, dirZ, cos inner */
    L_MAP,    /* cos outer, uMin, bins a pixel, bins (uint) */
    L_OFFSET, /* where its map starts in binding 5 (uint) */
    LAMP_ROWS
};

/* push constants of bloom's passes and of what walls send on: eight uints */
typedef struct Push2D { uint32_t from, fromWidth, fromHeight, to, toWidth, toHeight, scale, spare; } Push2D;

/* shaders2d.c: each returns SPIR-V, the caller's (artfFree), and its size in bytes */
uint32_t* artf2DShaderTiles(size_t* size);
uint32_t* artf2DShaderBloomDown(size_t* size);
uint32_t* artf2DShaderBloomUp(size_t* size);
uint32_t* artf2DShaderFinish(size_t* size); /* the frame and bloom's level 1, out to bytes */
uint32_t* artf2DShaderReflect(size_t* size);
/* gi, as gi2d.c: what is solid and what it sends, a texel each; the distances (exact, Felzenszwalb and Huttenlocher's),
 * a row (or column) an invocation; a cascade's rays, one an invocation (rays of an even cascade into binding 20, odd
 * into 21); cascade 0 to light; that light into what is solid (pass 0: from what is open, binding 8 into 22; 1: 22 into
 * 8). Push of a cascade: its spacing, rays a probe, probes across and down, the top one, where its rays start and end
 * (floats). */
uint32_t* artf2DShaderGiScene(size_t* size);
uint32_t* artf2DShaderGiDistance(size_t* size, bool columns);
uint32_t* artf2DShaderGiCascade(size_t* size, bool odd);
uint32_t* artf2DShaderGiGather(size_t* size);
uint32_t* artf2DShaderGiSpread(size_t* size, int pass);
#define GI_ROW 65536 /* a cascade's rays: this many an invocation row (1024 groups of 64) */

/* vulkan2d.c */
typedef struct Gpu2D Gpu2D;
/* NULL: Vulkan cannot run here (or cannot show in that surface). surface: NULL or none, frames stay on the GPU. */
Gpu2D* artf2DVulkanNew(const ArtfSurface* surface, bool noVsync, int width, int height);
bool artf2DVulkanShows(const Gpu2D* g);   /* it has a window */
double artf2DVulkanGpuMs(const Gpu2D* g); /* what the last frame done took on the GPU */
void artf2DVulkanResize(Gpu2D* g, int width, int height);
/* The last frame drawn, into dst (waiting for it). false: none of that size. */
bool artf2DVulkanPixels(Gpu2D* g, uint32_t* dst, int width, int height);
void artf2DVulkanFree(Gpu2D* g);
const char* artf2DVulkanName(const Gpu2D* g);
/* The frame f (prepared, its shadow maps made), with gi (NULL: none) on the GPU: shown in the window, if there is one,
 * and read back if asked (artf2DVulkanPixels then waits for no more than it). It does not wait for the GPU but for the
 * frame before the last. false: the device is lost. */
bool artf2DVulkanDraw(Gpu2D* g, const Frame2D* f, const ArtfGI2DDesc* gi, bool readBack);
/* For tests: the texels sent up since the program began. */
extern uint64_t artf2DVulkanTexelsSent;

#endif
