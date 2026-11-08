/* artful — core types: memory, growable arrays, vectors, matrices and quaternions. Plain C11, no
 * dependencies beyond the C standard library.
 *
 * Conventions: types are ArtfPascalCase, functions artfCamelCase, macros and enum values ARTF_UPPER.
 * A function that returns a pointer says whether the caller owns it; everything else is borrowed. */
#ifndef ARTFUL_CORE_H
#define ARTFUL_CORE_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of these headers (CMakeLists.txt's project()). */
#define ARTF_VERSION_MAJOR 0
#define ARTF_VERSION_MINOR 3
#define ARTF_VERSION_PATCH 0

#define ARTF_INLINE static inline

/* ---- memory ------------------------------------------------------------------------------ */

/* The allocator every module uses. Running out of memory aborts: no caller checks for NULL. */
void* artfMalloc(size_t size);
void* artfCalloc(size_t count, size_t size);
void* artfRealloc(void* p, size_t size);
void artfFree(void* p);
/* Aligned to align bytes (a power of two), for what SIMD reads whole; freed with artfAlignedFree. */
void* artfAlignedMalloc(size_t size, size_t align);
void artfAlignedFree(void* p);

#define ARTF_NEW(T) ((T*)artfCalloc(1, sizeof(T)))
#define ARTF_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* A growable array: zeroed, it is empty. */
#define ARTF_VEC(T) struct { T* items; size_t len, cap; }
#define artfVecReserve(v, n) do { \
        if ((size_t)(n) > (v)->cap) { \
            size_t cap_ = (v)->cap ? (v)->cap * 2 : 8; \
            if (cap_ < (size_t)(n)) cap_ = (size_t)(n); \
            (v)->items = artfRealloc((v)->items, cap_ * sizeof *(v)->items); \
            (v)->cap = cap_; \
        } \
    } while (0)
#define artfVecPush(v, x) do { artfVecReserve((v), (v)->len + 1); (v)->items[(v)->len++] = (x); } while (0)
#define artfVecFree(v) do { artfFree((v)->items); (v)->items = NULL; (v)->len = (v)->cap = 0; } while (0)

/* What Artful has to say about what went wrong: why a backend is not there, what a wrong handle was. Nothing
 * is said until a function is given. Any thread may call it. */
typedef void (*ArtfLogFn)(void* user, const char* message);
void artfSetLog(ArtfLogFn fn, void* user);
void artfLog(const char* format, ...);

/* A monotonic clock, in milliseconds: for animating, and for measuring. */
double artfNowMs(void);

/* ---- numbers ----------------------------------------------------------------------------- */

ARTF_INLINE float artfMinf(float a, float b) { return b < a ? b : a; }
ARTF_INLINE float artfMaxf(float a, float b) { return a < b ? b : a; }
ARTF_INLINE float artfClampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
ARTF_INLINE int artfMini(int a, int b) { return a < b ? a : b; }
ARTF_INLINE int artfMaxi(int a, int b) { return a > b ? a : b; }
ARTF_INLINE int artfClampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- vectors ----------------------------------------------------------------------------- */

typedef struct ArtfVec3 { float x, y, z; } ArtfVec3;
typedef struct ArtfQuat { float x, y, z, w; } ArtfQuat;

ARTF_INLINE ArtfVec3 artfVec3(float x, float y, float z) { ArtfVec3 v = {x, y, z}; return v; }
ARTF_INLINE ArtfVec3 artfVec3Add(ArtfVec3 a, ArtfVec3 b) { return artfVec3(a.x + b.x, a.y + b.y, a.z + b.z); }
ARTF_INLINE ArtfVec3 artfVec3Sub(ArtfVec3 a, ArtfVec3 b) { return artfVec3(a.x - b.x, a.y - b.y, a.z - b.z); }
ARTF_INLINE ArtfVec3 artfVec3Scale(ArtfVec3 a, float s) { return artfVec3(a.x * s, a.y * s, a.z * s); }
ARTF_INLINE float artfVec3Dot(ArtfVec3 a, ArtfVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
ARTF_INLINE ArtfVec3 artfVec3Cross(ArtfVec3 a, ArtfVec3 b) { return artfVec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
ARTF_INLINE float artfVec3Length(ArtfVec3 a) { return sqrtf(artfVec3Dot(a, a)); }
/* The zero vector stays zero. */
ARTF_INLINE ArtfVec3 artfVec3Normalize(ArtfVec3 a) { float l = artfVec3Length(a); return l > 0 ? artfVec3Scale(a, 1.f / l) : a; }

/* ---- matrices ---------------------------------------------------------------------------- */

/* 4 x 4, by columns: m[column * 4 + row]. */
typedef struct ArtfMat4 { float m[16]; } ArtfMat4;

/* An object's place in the world: the top three rows of a matrix, by rows. This is what an instance
 * keeps, and what the GPU reads. Negative scales are not supported: they turn faces inside out. */
typedef struct ArtfTransform { float m[12]; } ArtfTransform;

ARTF_INLINE ArtfMat4 artfMat4Identity(void) { ArtfMat4 r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}; return r; }
/* a * b (b applied first) */
ARTF_INLINE ArtfMat4 artfMat4Mul(ArtfMat4 a, ArtfMat4 b) {
    ArtfMat4 r;
    for (int c = 0; c < 4; ++c)
        for (int w = 0; w < 4; ++w)
            r.m[c * 4 + w] = a.m[w] * b.m[c * 4] + a.m[4 + w] * b.m[c * 4 + 1] + a.m[8 + w] * b.m[c * 4 + 2] + a.m[12 + w] * b.m[c * 4 + 3];
    return r;
}
/* From the eye, looking at target; right-handed, -z forward. */
ARTF_INLINE ArtfMat4 artfMat4LookAt(ArtfVec3 eye, ArtfVec3 target, ArtfVec3 up) {
    ArtfVec3 f = artfVec3Normalize(artfVec3Sub(target, eye));
    ArtfVec3 s = artfVec3Normalize(artfVec3Cross(f, up));
    ArtfVec3 u = artfVec3Cross(s, f);
    ArtfMat4 r = {{s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0, -artfVec3Dot(s, eye), -artfVec3Dot(u, eye), artfVec3Dot(f, eye), 1}};
    return r;
}
/* Perspective with reversed depth and no far plane: depth is near / distance, 1 at the near plane
 * and 0 at infinity, which spends a float's precision evenly along the view. fovY in radians. */
ARTF_INLINE ArtfMat4 artfMat4Perspective(float fovY, float aspect, float near_) {
    float f = 1.f / tanf(fovY * .5f);
    ArtfMat4 r = {{f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, 0, -1, 0, 0, near_, 0}};
    return r;
}

ARTF_INLINE ArtfQuat artfQuatIdentity(void) { ArtfQuat q = {0, 0, 0, 1}; return q; }
ARTF_INLINE ArtfQuat artfQuatAxisAngle(ArtfVec3 axis, float radians) {
    ArtfVec3 a = artfVec3Normalize(axis);
    float s = sinf(radians * .5f);
    ArtfQuat q = {a.x * s, a.y * s, a.z * s, cosf(radians * .5f)};
    return q;
}
/* a * b (b applied first) */
ARTF_INLINE ArtfQuat artfQuatMul(ArtfQuat a, ArtfQuat b) {
    ArtfQuat q = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                  a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    return q;
}

ARTF_INLINE ArtfTransform artfTransformIdentity(void) { ArtfTransform t = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}; return t; }
/* Scale first, then the rotation, then the position. */
ARTF_INLINE ArtfTransform artfTransformTrs(ArtfVec3 position, ArtfQuat q, ArtfVec3 scale) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    ArtfTransform t = {{(1 - 2 * (y * y + z * z)) * scale.x, 2 * (x * y - w * z) * scale.y, 2 * (x * z + w * y) * scale.z, position.x,
                        2 * (x * y + w * z) * scale.x, (1 - 2 * (x * x + z * z)) * scale.y, 2 * (y * z - w * x) * scale.z, position.y,
                        2 * (x * z - w * y) * scale.x, 2 * (y * z + w * x) * scale.y, (1 - 2 * (x * x + y * y)) * scale.z, position.z}};
    return t;
}
ARTF_INLINE ArtfTransform artfTransformAt(float x, float y, float z, float scale) {
    ArtfTransform t = {{scale, 0, 0, x, 0, scale, 0, y, 0, 0, scale, z}};
    return t;
}

#ifdef __cplusplus
}
#endif

#endif
