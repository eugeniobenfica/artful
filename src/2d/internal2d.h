/* artful-2d — what its files share: the scene as it is kept, and a frame as it is made. */
#ifndef ARTFUL_2D_INTERNAL_H
#define ARTFUL_2D_INTERNAL_H

#include "artful/2d.h"
#include "core/threads.h"

#define TILE2D 64          /* a tile of the frame, a side: what a thread draws in its cache */
#define MIPS2D 16
#define PI2D 3.14159265f

/* ---- the scene ---- */

/* A slot of what handles name: the handle is (generation << 24) | (index + 1). */
typedef struct Slot2D {
    uint8_t generation;
    bool alive;
} Slot2D;

typedef struct Texture2D {
    Slot2D slot;
    bool normalMap;
    uint64_t version; /* the scene's textureVersion when its pixels were last given */
    int levels;
    int width[MIPS2D], height[MIPS2D];
    uint32_t* texels[MIPS2D]; /* straight BGRA, as bytes hold light; a normal map's as it is */
} Texture2D;

typedef struct Sprite2D {
    Slot2D slot;
    ArtfSprite2DDesc desc; /* points: its own copy */
    ArtfVec2* points;
    uint64_t order;        /* when it was added: the order within a layer */
} Sprite2D;

typedef struct Light2D {
    Slot2D slot;
    ArtfLight2DDesc desc;
} Light2D;

typedef struct Occluder2D {
    Slot2D slot;
    ArtfVec2* points;
    int count;
    bool closed;
} Occluder2D;

struct ArtfScene2D {
    ARTF_VEC(Texture2D) textures;
    ARTF_VEC(Sprite2D) sprites;
    ARTF_VEC(Light2D) lights;
    ARTF_VEC(Occluder2D) occluders;
    uint64_t nextOrder;
    uint64_t textureVersion; /* each change of a texture's pixels, or of which are there */
    ArtfVec3 ambient;
    ArtfGI2DDesc gi;
    ArtfPost2DDesc post;
    ArtfVec3 background;
    float backgroundAlpha;
    ArtfCamera2D camera;
};

/* What a handle names, or NULL (and a word through artfSetLog). */
Texture2D* artf2DTexture(const ArtfScene2D* s, ArtfTexture2D h);
/* A 2 x 3 affine map: x' = m[0] x + m[1] y + m[2], y' = m[3] x + m[4] y + m[5]. */
typedef struct Affine2D { float m[6]; } Affine2D;
Affine2D artf2DCameraMap(const ArtfCamera2D* c, int width, int height);
Affine2D artf2DMul(Affine2D a, Affine2D b); /* b first */
Affine2D artf2DInverse(Affine2D a);
static inline ArtfVec2 artf2DApply(const Affine2D* a, float x, float y) {
    return artfVec2(a->m[0] * x + a->m[1] * y + a->m[2], a->m[3] * x + a->m[4] * y + a->m[5]);
}
/* A sprite's own space to the world. */
Affine2D artf2DSpriteMap(const Sprite2D* sp);

/* Its shape, centred: half its width and height (rect, ellipse), in its own space. */
static inline void artf2DSpriteHalf(const Sprite2D* sp, float* hw, float* hh) {
    *hw = sp->desc.size.x * .5f;
    *hh = sp->desc.size.y * .5f;
}

/* ---- what covers a point: inside, as a share (0..1), with edges as wide as `soft` of its own units ---- */

static inline float artf2DSegmentDistance(float px, float py, ArtfVec2 a, ArtfVec2 b) {
    const float ex = b.x - a.x, ey = b.y - a.y, len2 = ex * ex + ey * ey;
    float t = len2 > 0 ? ((px - a.x) * ex + (py - a.y) * ey) / len2 : 0;
    t = artfClampf(t, 0, 1);
    const float dx = px - a.x - ex * t, dy = py - a.y - ey * t;
    return sqrtf(dx * dx + dy * dy);
}

static inline float artf2DCoverAt(const Sprite2D* sp, float x, float y, float soft) {
    const ArtfSprite2DDesc* d = &sp->desc;
    float dist = 0;
    switch (d->shape) {
    case ARTF_SHAPE_RECT: {
        float hw, hh;
        artf2DSpriteHalf(sp, &hw, &hh);
        const float r = artfClampf(d->cornerRadius, 0, artfMinf(hw, hh));
        const float qx = (x < 0 ? -x : x) - hw + r, qy = (y < 0 ? -y : y) - hh + r;
        const float ox = artfMaxf(qx, 0), oy = artfMaxf(qy, 0);
        dist = sqrtf(ox * ox + oy * oy) + artfMinf(artfMaxf(qx, qy), 0) - r;
        break;
    }
    case ARTF_SHAPE_ELLIPSE: {
        float hw, hh;
        artf2DSpriteHalf(sp, &hw, &hh);
        if (hw <= 0 || hh <= 0) return 0;
        /* (Quilez's estimate: exact for a circle) */
        const float ux = x / hw, uy = y / hh, vx = x / (hw * hw), vy = y / (hh * hh);
        const float k1 = sqrtf(ux * ux + uy * uy), k2 = sqrtf(vx * vx + vy * vy);
        dist = k2 > 0 ? k1 * (k1 - 1.f) / k2 : -artfMinf(hw, hh);
        break;
    }
    case ARTF_SHAPE_LINE:
        if (d->pointCount < 2) return 0;
        dist = artf2DSegmentDistance(x, y, sp->points[0], sp->points[1]) - d->thickness * .5f;
        break;
    case ARTF_SHAPE_POLYGON: {
        int winding = 0;
        const int n = d->pointCount;
        for (int i = 0; i < n; ++i) {
            const ArtfVec2 a = sp->points[i], b = sp->points[(i + 1) % n];
            if ((a.y <= y) != (b.y <= y)) {
                const float cx = a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x);
                if (cx > x) winding += b.y > a.y ? 1 : -1;
            }
        }
        return winding ? 1.f : 0.f;
    }
    }
    return artfClampf(.5f - dist / soft, 0, 1);
}

/* ---- numbers that give the same bits everywhere ---- */

extern float artf2DSquare[256]; /* a byte to the light it holds: (i / 255)^2 */
void artf2DTables(void);
/* atan2, within 1e-5, of additions, multiplications and a division */
static inline float artf2DAtan2(float y, float x) {
    const float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    const float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx == 0) return 0;
    const float a = mn / mx, s = a * a;
    float r = (((((-.0117212f * s + .05265332f) * s - .11643287f) * s + .19354346f) * s - .33262347f) * s + .99997726f) * a;
    if (ay > ax) r = 1.57079633f - r;
    if (x < 0) r = 3.14159265f - r;
    return y < 0 ? -r : r;
}

/* ---- a frame ---- */

/* A sprite as this frame draws it: in the frame's pixels. */
typedef struct Item2D {
    const Sprite2D* sp;
    uint32_t handle;
    uint32_t owner;    /* 1 + its index among what casts shadows; 0: it casts none */
    Affine2D toLocal;  /* the frame's pixels to its own space */
    float zoom;        /* pixels for a unit of its own space */
    float turnCos, turnSin; /* its own space to the frame's: for normals */
    float hw, hh;      /* rect and ellipse */
    float uv[4];       /* u = uv[0] + x uv[1], v = uv[2] + y uv[3], of its own space */
    int x0, y0, x1, y1; /* the pixels it covers, the ends past them */
    int edgeStart, edgeCount; /* polygon: its edges in the frame */
    const Texture2D* tex[3]; /* texture, normal map, emissive map; NULL: none */
    int mip[3];
    bool emits, lit, polygon;
} Item2D;

typedef struct Edge2D { float x0, y0, x1, y1; } Edge2D;

/* An outline that stops light, in the frame's pixels. */
typedef struct Outline2D {
    int start, count;
    bool closed;
    uint32_t owner;
    float x0, y0, x1, y1; /* its box */
} Outline2D;

/* A light's shadow map: for each direction from it (or each line across the sun's), how far light goes; with whose
 * outline stopped it, and how far it goes past that one, for that one's own pixels. */
typedef struct ShadowMap2D {
    int bins;
    float* near_;    /* bins each */
    uint32_t* owner;
    float* past;
    float uMin, binsPerUnit; /* sun: where the bins start across, and how many a pixel */
} ShadowMap2D;

/* A light as this frame draws it, in pixels. */
typedef struct Lamp2D {
    ArtfLight2DKind kind;
    float x, y;            /* point and spot */
    float color[3];
    float range, range2, inverseRange2;
    float radius;          /* pixels; sun: radians */
    float height;
    float dirX, dirY;      /* spot: where it points; sun: towards it, in the plane, with dirZ */
    float dirZ;
    float cosInner, cosOuter;
    uint32_t mask;
    int x0, y0, x1, y1;    /* the pixels it reaches */
    ShadowMap2D* shadow;   /* NULL: none */
} Lamp2D;

typedef struct Gi2D Gi2D;

typedef struct Frame2D {
    const ArtfScene2D* scene;
    int width, height;
    int tilesX, tilesY;
    ARTF_VEC(Item2D) items;
    ARTF_VEC(Edge2D) edges;
    ARTF_VEC(ArtfVec2) outlinePoints;
    ARTF_VEC(Outline2D) outlines;
    ARTF_VEC(Lamp2D) lamps;
    uint32_t** tileItems;  /* for each tile, its items' indices in order; tileItemCount each */
    uint32_t* tileItemCount;
    uint16_t** tileLamps;
    uint32_t* tileLampCount;
    float ambient[3];
    float background[4];   /* premultiplied */
    /* gi: what the tiles read, and what they leave for the next frame */
    int giScale, giWidth, giHeight;
    const float* giLight;  /* 3 floats a texel; NULL: no gi */
    float* reflected;      /* 4 floats a texel: light that walls send on, and how many pixels gave it */
    /* out */
    float* hdr;            /* premultiplied rgba floats, when bloom needs them; else NULL */
    uint32_t* pixels;
    float exposure;
    ArtfTone2D tone;
} Frame2D;

/* shadow2d.c */
void artf2DShadowBuild(Frame2D* f, Lamp2D* lamp, const float* binCos, const float* binSin);
float artf2DShadowSee(const Lamp2D* lamp, float px, float py, uint32_t owner);

/* gi2d.c */
Gi2D* artf2DGiNew(void);
void artf2DGiFree(Gi2D* g);
/* Light from glowing sprites and walls, for the frame f, into f->giLight; f->reflected is where the tiles leave
 * what the walls send on, for the next. */
void artf2DGiRun(Gi2D* g, Frame2D* f, const ArtfGI2DDesc* desc, ArtfPool* pool);

/* draw2d.c: one sprite's sample at a point of its own space: premultiplied linear color and alpha, its emission
 * (both times cover), and which way it faces in the frame (NULL: not wanted) */
void artf2DSample(const Item2D* it, float lx, float ly, float cover, float rgba[4], float emit[3], float normal[3]);
/* What a thread needs to draw a tile. */
typedef struct TileScratch2D {
    float* color;  /* TILE2D * TILE2D * 4 */
    float* cover;  /* TILE2D */
    float* crossX; /* as many as the most edges a polygon has */
    int* crossDir;
} TileScratch2D;
void artf2DDrawTile(const Frame2D* f, uint32_t tile, TileScratch2D* scratch);
/* A polygon's coverage along the row y, for pixels x0..x1-1 (four rows of samples, each exact across). */
void artf2DPolygonRow(const Frame2D* f, const Item2D* it, int y, int x0, int x1, float* cover, float* crossX, int* crossDir);

/* post2d.c */
void artf2DBloom(Frame2D* f, float amount, float radius, ArtfPool* pool);

/* A pixel out to bytes: exposure, the tone curve, the square root of light, premultiplied. */
static inline void artf2DTone(const Frame2D* f, const float rgba[4], uint32_t* out) {
    float v[3];
    /* (light where nothing covers, as bloom leaves: as opaque as it is bright) */
    const float most = artfMaxf(rgba[0], artfMaxf(rgba[1], rgba[2]));
    const float alpha = artfClampf(artfMaxf(rgba[3], artfMinf(most * f->exposure, 1)), 0, 1);
    if (alpha <= 0) {
        *out = 0;
        return;
    }
    const float k = f->exposure / alpha;
    for (int c = 0; c < 3; ++c) {
        float x = rgba[c] * k;
        x = x > 0 ? artfMinf(x, 65504.f) : 0.f; /* (and not a number: nothing) */
        if (f->tone == ARTF_TONE2D_ACES) x = x * (x * 2.51f + .03f) / (x * (x * 2.43f + .59f) + .14f);
        else if (f->tone == ARTF_TONE2D_REINHARD) x = x / (1.f + x);
        v[c] = sqrtf(artfClampf(x, 0, 1));
    }
    uint32_t p = (uint32_t)(alpha * 255.f + .5f) << 24;
    for (int c = 0; c < 3; ++c) p |= (uint32_t)(v[c] * alpha * 255.f + .5f) << (16 - 8 * c);
    *out = p;
}

#endif
