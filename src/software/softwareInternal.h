/* artful — what the software backend's driver (software.c) and its kernels (kernels.inc) share. */
#ifndef ARTFUL_SOFTWARE_INTERNAL_H
#define ARTFUL_SOFTWARE_INTERNAL_H

#include "scene/sceneInternal.h"
#include "software/shadows.h"

#define ARTF_TILE 64
/* Geometry is cut into this many pieces whatever the number of threads, and tiles read the pieces in
 * order: so triangles reach a tile in the same order every time, and so the pixels are the same. */
#define ARTF_CHUNKS 64
#define ARTF_BIN_CAP 14
#define ARTF_HIZ_CELL 16
#define ARTF_TILE_PIXELS (ARTF_TILE * ARTF_TILE)
#define ARTF_TRI_BACK 0x80000000u
#define ARTF_SAMPLE_REACH 6 /* how far a pixel's samples are from its center, in 1/16 pixels */

/* A triangle as the tiles see it: vertices in 1/16 pixels, area positive, and depth (1 / w) as a plane
 * over pixels. Its pixels' colors are worked out later, and only if it shows, from the triangle it
 * came from (a piece of a clipped triangle has the same one). */
typedef struct ArtfRasterTri {
    double z0;
    float zdx, zdy;
    int32_t x[3], y[3];
    uint32_t instance;  /* index in the frame's visible instances */
    uint32_t triangle;  /* meshlet << 7 | triangle in the meshlet; ARTF_TRI_BACK: its back shows */
    int16_t minX, minY, maxX, maxY; /* the pixels it may cover, inclusive, inside the frame */
} ArtfRasterTri;

typedef struct ArtfBinBlock {
    uint32_t next;  /* block index + 1 */
    uint32_t count;
    uint32_t tris[ARTF_BIN_CAP];
} ArtfBinBlock;

/* What one piece of the geometry made. */
typedef struct ArtfChunk {
    ARTF_VEC(ArtfRasterTri) tris;
    ARTF_VEC(ArtfBinBlock) blocks;
    uint32_t (*heads)[2]; /* for each tile: its first and last block + 1 */
} ArtfChunk;

typedef struct ArtfVisible {
    const ArtfInstanceData* instance;
    const ArtfMeshData* mesh;
    float mvp[12];       /* object to clip space, rows x, y and w */
    float normal[9];     /* its cofactors, by columns: normals stay right on stretched objects */
    float camera[3];     /* in object space, for the cones */
    float center[3], radius; /* in the world */
    uint32_t index;      /* in the scene's instances */
    bool useCone;
    bool inside;         /* all within the frustum: its meshlets need no test */
    bool masked;         /* its material has holes: rasterizing asks the texture */
    bool twoSided;       /* its backs are drawn */
    uint32_t fade;       /* between two levels: how far, times 2, and 1 for the lighter one; 0: it is not */
    const ArtfMeshData* fadeFrom; /* the other level, before it goes in as an entry of its own */
} ArtfVisible;

/* Scratch of one thread: a tile's memory. Depth and ids stay here: what leaves a tile is its color.
 * Each is a plane for each sample, one after the other. */
typedef struct ArtfWorker {
    float* depth;
    uint32_t* ids;      /* index in tris + 1, or in a layer's triangles; 0: nothing */
    uint32_t* color;
    uint32_t* layer;    /* the colors of the layer being drawn */
    float* solidDepth;  /* when layers are sorted: what they all go by; then each pixel's fragments */
    float* fragmentDepth;
    uint32_t* fragmentColor;
    uint8_t* fragmentCount;
    ARTF_VEC(const ArtfRasterTri*) tris;
    ARTF_VEC(const ArtfRasterTri*) layers; /* what shows what is behind it: by instance, the farthest first */
    ARTF_VEC(uint8_t) seen;
} ArtfWorker;

typedef enum ArtfTileClass { ARTF_TILE_EMPTY, ARTF_TILE_MIXED, ARTF_TILE_OPAQUE } ArtfTileClass;

typedef struct ArtfSoftFrame {
    const ArtfScene* scene;
    ArtfFrameData data;
    int width, height, tilesX, tilesY;
    int samples;        /* of a pixel: 1 or 4 */
    double pixelW, pixelH; /* 1 / width, 1 / height */
    uint32_t* pixels;
    float nearW;        /* what w is clipped to */
    float guardX, guardY; /* how far beyond the frame a triangle may go unclipped, in clip space */
    uint32_t background;
    ArtfVisible* visible;
    uint32_t visibleCount;
    uint32_t solidCount;     /* those from here on are layers */
    uint32_t firstVisible;   /* the first the next geometry pass takes */
    bool geometryAppend;     /* it adds to what the pass before left */
    uint32_t* meshletStarts; /* of each visible instance, then the total */
    ArtfChunk* chunks;
    ArtfWorker* workers;
    /* the overlay */
    const uint32_t* overlay;
    int overlayWidth, overlayHeight, overlayStride;
    uint8_t* tileClass;
    uint8_t* tileDirty; /* its class must be worked out again */
    /* the farthest depth of each 16 x 16 cell, and of each tile, for occlusion */
    float* hizCells;
    float* hizTiles;
    int cellsX, cellsY;
    /* a black hole: its light before tone mapping, by planes, and its bloom's levels */
    bool hole;
    ArtfHoleFrame holeData;
    const float* holeMap;
    const float* holeFluid;
    const ArtfShadowSource* shadows; /* NULL: this frame has none */
    /* tracing: what each pixel gathered, four floats (light, and how often something was there) */
    float* traceSum;
    uint32_t traceDone;
    bool tracing;
    float* hdr[3];
    int hdrStride;      /* a multiple of 8 */
    float* bloom[3];
} ArtfSoftFrame;

typedef struct ArtfKernels {
    const char* name;
    void (*geometry)(ArtfSoftFrame* f, uint32_t chunk);
    uint32_t (*tile)(ArtfSoftFrame* f, uint32_t tile, uint32_t worker); /* 1 when skipped */
    void (*depth)(ArtfSoftFrame* f, uint32_t tile, uint32_t worker);    /* a tile's depth alone, for occlusion */
    void (*lens)(ArtfSoftFrame* f, uint32_t tile);                      /* a tile's light from a black hole */
} ArtfKernels;

extern const ArtfKernels artfKernelsPortable;
#ifdef ARTFUL_HAVE_AVX2
extern const ArtfKernels artfKernelsAvx2;
#endif
#ifdef ARTFUL_HAVE_SSE41
extern const ArtfKernels artfKernelsSse41;
#endif
#ifdef ARTFUL_HAVE_NEON
extern const ArtfKernels artfKernelsNeon;
#endif

#endif
