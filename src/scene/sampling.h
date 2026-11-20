/* artful — where a path's samples fall: Sobol's points, scrambled by Owen's nested scrambling, a hash at a time
 * (Burley, "Practical Hash-based Owen Scrambling", JCGT 9(4), 2020). Each pixel scrambles the same points its own way,
 * so a pixel's samples are spread as Sobol's are, and pixels do not repeat each other. Integers only: the same numbers on
 * every processor, and in the tracing shader (shaders.c's sobol4Of), which does the same in the same order. */
#ifndef ARTFUL_SAMPLING_H
#define ARTFUL_SAMPLING_H

#include <stdint.h>

/* The first 24 direction numbers of Sobol's first four dimensions (Joe and Kuo's): 16 million samples a pixel before
 * they repeat. */
#define ARTF_SOBOL_BITS 24
static const uint32_t artfSobolMatrix[4][ARTF_SOBOL_BITS] = {
    {0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u, 0x08000000u, 0x04000000u, 0x02000000u, 0x01000000u, 0x00800000u, 0x00400000u, 0x00200000u, 0x00100000u,
     0x00080000u, 0x00040000u, 0x00020000u, 0x00010000u, 0x00008000u, 0x00004000u, 0x00002000u, 0x00001000u, 0x00000800u, 0x00000400u, 0x00000200u, 0x00000100u},
    {0x80000000u, 0xC0000000u, 0xA0000000u, 0xF0000000u, 0x88000000u, 0xCC000000u, 0xAA000000u, 0xFF000000u, 0x80800000u, 0xC0C00000u, 0xA0A00000u, 0xF0F00000u,
     0x88880000u, 0xCCCC0000u, 0xAAAA0000u, 0xFFFF0000u, 0x80008000u, 0xC000C000u, 0xA000A000u, 0xF000F000u, 0x88008800u, 0xCC00CC00u, 0xAA00AA00u, 0xFF00FF00u},
    {0x80000000u, 0xC0000000u, 0x60000000u, 0x90000000u, 0xE8000000u, 0x5C000000u, 0x8E000000u, 0xC5000000u, 0x68800000u, 0x9CC00000u, 0xEE600000u, 0x55900000u,
     0x80680000u, 0xC09C0000u, 0x60EE0000u, 0x90550000u, 0xE8808000u, 0x5CC0C000u, 0x8E606000u, 0xC5909000u, 0x6868E800u, 0x9C9C5C00u, 0xEEEE8E00u, 0x5555C500u},
    {0x80000000u, 0xC0000000u, 0x20000000u, 0x50000000u, 0xF8000000u, 0x74000000u, 0xA2000000u, 0x93000000u, 0xD8800000u, 0x25400000u, 0x59E00000u, 0xE6D00000u,
     0x78080000u, 0xB40C0000u, 0x82020000u, 0xC3050000u, 0x208F8000u, 0x51474000u, 0xFBEA2000u, 0x75D93000u, 0xA0858800u, 0x914E5400u, 0xDBE79E00u, 0x25DB6D00u},
};

static inline uint32_t artfHash32(uint32_t x) {
    x = (x ^ x >> 16) * 0x7FEB352Du;
    x = (x ^ x >> 15) * 0x846CA68Bu;
    return x ^ x >> 16;
}

static inline uint32_t artfBitReverse(uint32_t x) {
    x = x << 16 | x >> 16;
    x = (x & 0x00FF00FFu) << 8 | (x & 0xFF00FF00u) >> 8;
    x = (x & 0x0F0F0F0Fu) << 4 | (x & 0xF0F0F0F0u) >> 4;
    x = (x & 0x33333333u) << 2 | (x & 0xCCCCCCCCu) >> 2;
    return (x & 0x55555555u) << 1 | (x & 0xAAAAAAAAu) >> 1;
}

/* Owen's scrambling of x's bits, each by all those above it, by seed (Burley's Laine-Karras permutation, on x's bits
 * reversed). */
static inline uint32_t artfOwenScramble(uint32_t x, uint32_t seed) {
    x = artfBitReverse(x);
    x += seed;
    x ^= x * 0x6C50B47Cu;
    x ^= x * 0xB82F1E52u;
    x ^= x * 0xC7AFE638u;
    x ^= x * 0x8D22F6E6u;
    return artfBitReverse(x);
}

/* (each dimension's own seed) */
static inline uint32_t artfSobolSeed(uint32_t seed, uint32_t dimension) { return artfHash32(seed + dimension * 0x9E3779B9u); }

/* Sample `index` of the sequence that `seed` scrambles: four numbers in [0, 1), 24 bits each. */
static inline void artfSobol4(uint32_t index, uint32_t seed, float out[4]) {
    index &= (1u << ARTF_SOBOL_BITS) - 1u;
    for (uint32_t d = 0; d < 4; ++d) {
        uint32_t x = 0;
        /* (to the index's last bit: a pixel's index is small, and the shader's all 24 give the same) */
        for (uint32_t i = index, k = 0; i; i >>= 1, ++k)
            if (i & 1u) x ^= artfSobolMatrix[d][k];
        out[d] = (float)(artfOwenScramble(x, artfSobolSeed(seed, d)) >> 8) * (1.f / 16777216.f);
    }
}

/* The seed of a pixel's sequence for a group of dimensions: the camera's (group 0), then each bounce's. */
static inline uint32_t artfPathSeed(uint32_t pixel, uint32_t group) { return artfHash32(artfHash32(pixel) ^ (group + 1u) * 0x85EBCA6Bu); }

/* ---- blue noise between pixels (Georgiev and Fajardo, 2016) ----
 *
 * With a sequence of its own, a pixel's error is unlike its neighbours' at random: white noise. So the pixels of a square
 * of 64 x 64 share one sequence (artfTileSeed), and each turns it (Cranley and Patterson) by its place in a tile of blue
 * noise (tools/blueNoise.py), a dimension and a group at a time from another place of the tile: with a sample or a few,
 * neighbours are told apart as blue noise is, and a pixel's samples are still Sobol's, turned. On the GPU, the tile is
 * in the tracing frame (ArtfTraceFrame.blue), and the same integers make the same turns. */
#include "scene/blueNoise.inc"

static inline uint32_t artfTileSeed(uint32_t x, uint32_t y, uint32_t group) { return artfPathSeed((y >> 6) * 65536u + (x >> 6), group); }

/* A pixel's turn of a dimension (salt: group x 4 + dimension): the tile's rank at a place the salt moves it to, as the
 * top 12 bits, and 20 of a hash below. */
static inline uint32_t artfBlueTurn(uint32_t x, uint32_t y, uint32_t salt) {
    const uint32_t h = artfHash32(salt * 0x9E3779B9u + 0x632BE5ABu);
    const uint32_t at = ((y + (h >> 6)) & 63u) * 64u + ((x + h) & 63u);
    return (uint32_t)artfBlueNoise[at] << 20 | artfHash32(h ^ x * 0x27D4EB2Du ^ y * 0x165667B1u) >> 12;
}

/* artfSobol4, of the square's sequence, turned for the pixel at (x, y). */
static inline void artfSobol4Turned(uint32_t index, uint32_t x, uint32_t y, uint32_t group, float out[4]) {
    index &= (1u << ARTF_SOBOL_BITS) - 1u;
    const uint32_t seed = artfTileSeed(x, y, group);
    for (uint32_t d = 0; d < 4; ++d) {
        uint32_t v = 0;
        for (uint32_t i = index, k = 0; i; i >>= 1, ++k)
            if (i & 1u) v ^= artfSobolMatrix[d][k];
        v = artfOwenScramble(v, artfSobolSeed(seed, d)) + artfBlueTurn(x, y, group * 4u + d);
        out[d] = (float)(v >> 8) * (1.f / 16777216.f);
    }
}

#endif
