/* artful — the GPU's shaders, built at start.
 *
 * Bindings of the one descriptor set:
 *   0 frame: ArtfFrameData, as vec4s
 *   1 instances: ArtfInstanceData, as vec4s
 *   2 visible: of each copy drawn, its instance and its mesh
 *   3 meshes: five vec4s each: center and radius, scale, offset, the lighter meshes, their errors
 *   4 materials: ArtfMaterialData, as vec4s
 *   5 indirect: the draw commands, five words each
 *   6 textures: an array of samplers        7 overlay: a sampler
 *   8 depth: a sampler                      9 hiz: the depth pyramid
 *   10 flags: of each instance, whether it showed last frame
 *   16 colors: of the vertices of meshes that have them, BGRA; the first is white
 *   24 ArtfShadowFrame   25 the shadow atlas: a sampler   26 what casts shadows: its instance and its mesh
 *   65 the meshlets view: of each mesh where its triangles start, then each triangle's meshlet
 *
 * Lighting here is the same arithmetic as the CPU's (kernels.inc), so that both draw the same scene. */
#include "vulkan/shaders.h"
#include "scene/sampling.h"
#include "artful/renderer.h"
#include "scene/sceneInternal.h"
#include "vulkan/spirv.h"

#include <stdlib.h>

/* where things are in the frame's vec4s */
enum { F_VIEWPROJ = 0, F_PLANES = 4, F_CAMERA = 9, F_TOSUN = 10, F_SUNCOLOR = 11, F_SKY = 12, F_GROUND = 13, F_COUNTS = 15, F_LIGHTS = 16, F_LIGHT_ROWS = 3, F_PROJECTION = 64, F_HIZ = 65, F_LOOK = 80, F_FOG = 81, F_EYE = 82, F_POST = 85, F_PREVIOUS = 88, F_INDIRECT = 92, F_GRADE = 95, F_MATERIALS = 98, F_LIGHT_SIZES = 99, F_CULL = 103, F_GRADE2 = 104, F_ORTHO = 111, F_TRACED = 112, F_SHAPES = 113, F_OUTPUT = 177, F_SIZE = 178 };

typedef struct Resources {
    SpvId frame, instances, visible, meshes, materials, indirect, hiz, flags, environment;
    SpvId ptrUniformVec4, ptrStorageVec4, ptrStorageUint, ptrStorageFloat, uintArray, vec4Array, floatArray;
    SpvId zero;
} Resources;

static SpvId blockOf(Spv* s, SpvId array, bool writable) {
    SpvId block = spvTypeStruct(s, &array, 1);
    spvDecorate(s, block, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, block, 0, SPV_DECORATE_Offset, 0);
    if (!writable) spvMemberDecorate(s, block, 0, SPV_DECORATE_NonWritable, -1);
    return block;
}

/* Declares what a shader reads. A buffer the shader does not write is marked so, as the vertex and
 * fragment stages require. */
static void declare(Spv* s, Resources* r, bool writable) {
    r->zero = spvConstInt(s, 0);
    r->ptrUniformVec4 = spvTypePointer(s, SPV_STORAGE_Uniform, s->vec4);
    r->ptrStorageVec4 = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->vec4);
    r->ptrStorageUint = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->uintType);
    SpvId frameArray = spvTypeArray(s, s->vec4, F_SIZE, 16);
    r->frame = spvResource(s, blockOf(s, frameArray, true), SPV_STORAGE_Uniform, 0);
    SpvId vec4Array = spvTypeArray(s, s->vec4, 0, 16), uintArray = spvTypeArray(s, s->uintType, 0, 4);
    r->uintArray = uintArray;
    r->vec4Array = vec4Array;
    r->instances = spvResource(s, blockOf(s, vec4Array, false), SPV_STORAGE_StorageBuffer, 1);
    r->visible = spvResource(s, blockOf(s, uintArray, writable), SPV_STORAGE_StorageBuffer, 2);
    r->meshes = spvResource(s, blockOf(s, vec4Array, false), SPV_STORAGE_StorageBuffer, 3);
    r->materials = spvResource(s, blockOf(s, vec4Array, false), SPV_STORAGE_StorageBuffer, 4);
    r->ptrStorageFloat = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->floatType);
    r->floatArray = spvTypeArray(s, s->floatType, 0, 4);
    r->environment = spvResource(s, blockOf(s, r->floatArray, false), SPV_STORAGE_StorageBuffer, 49);
    if (writable) {
        r->indirect = spvResource(s, blockOf(s, uintArray, true), SPV_STORAGE_StorageBuffer, 5);
        r->hiz = spvResource(s, blockOf(s, uintArray, true), SPV_STORAGE_StorageBuffer, 9);
        r->flags = spvResource(s, blockOf(s, uintArray, true), SPV_STORAGE_StorageBuffer, 10);
    }
}

static SpvId frameVec(Spv* s, const Resources* r, uint32_t i) {
    const SpvId idx[2] = {r->zero, spvConstUint(s, i)};
    return spvLoad(s, s->vec4, spvAccess(s, r->ptrUniformVec4, r->frame, idx, 2));
}
static SpvId frameVecAt(Spv* s, const Resources* r, SpvId i) {
    const SpvId idx[2] = {r->zero, i};
    return spvLoad(s, s->vec4, spvAccess(s, r->ptrUniformVec4, r->frame, idx, 2));
}
/* One float of the frame's block: row i, component c. */
static SpvId frameFloatAt(Spv* s, const Resources* r, SpvId i, SpvId c) {
    const SpvId idx[3] = {r->zero, i, c};
    return spvLoad(s, s->floatType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_Uniform, s->floatType), r->frame, idx, 3));
}
static SpvId bufferVec(Spv* s, const Resources* r, SpvId buffer, SpvId i) {
    const SpvId idx[2] = {r->zero, i};
    return spvLoad(s, s->vec4, spvAccess(s, r->ptrStorageVec4, buffer, idx, 2));
}
static SpvId bufferUintPtr(Spv* s, const Resources* r, SpvId buffer, SpvId i) {
    const SpvId idx[2] = {r->zero, i};
    return spvAccess(s, r->ptrStorageUint, buffer, idx, 2);
}

#define UINT(v) spvConstUint(s, v)
#define FLOAT(v) spvConstFloat(s, v)
#define ADD_U(a, b) spvOp2(s, SPV_OP_IAdd, s->uintType, a, b)
#define MUL_U(a, b) spvOp2(s, SPV_OP_IMul, s->uintType, a, b)
#define ADD_F(a, b) spvOp2(s, SPV_OP_FAdd, s->floatType, a, b)
#define SUB_F(a, b) spvOp2(s, SPV_OP_FSub, s->floatType, a, b)
#define MUL_F(a, b) spvOp2(s, SPV_OP_FMul, s->floatType, a, b)
#define DIV_F(a, b) spvOp2(s, SPV_OP_FDiv, s->floatType, a, b)
#define ADD_V3(a, b) spvOp2(s, SPV_OP_FAdd, s->vec3, a, b)
#define SUB_V3(a, b) spvOp2(s, SPV_OP_FSub, s->vec3, a, b)
#define MUL_V3(a, b) spvOp2(s, SPV_OP_FMul, s->vec3, a, b)
#define SCALE_V3(a, f) spvOp2(s, SPV_OP_VectorTimesScalar, s->vec3, a, f)
#define SCALE_V4(a, f) spvOp2(s, SPV_OP_VectorTimesScalar, s->vec4, a, f)
#define ADD_V4(a, b) spvOp2(s, SPV_OP_FAdd, s->vec4, a, b)
#define DOT3(a, b) spvOp2(s, SPV_OP_Dot, s->floatType, a, b)
#define DOT4(a, b) spvOp2(s, SPV_OP_Dot, s->floatType, a, b)
#define MAX_F(a, b) spvGlsl2(s, SPV_GLSL_FMax, s->floatType, a, b)
#define MIN_F(a, b) spvGlsl2(s, SPV_GLSL_FMin, s->floatType, a, b)
#define NORMALIZE(a) spvGlsl1(s, SPV_GLSL_Normalize, s->vec3, a)
#define X(v) spvExtract(s, s->floatType, v, 0)
#define Y(v) spvExtract(s, s->floatType, v, 1)
#define Z(v) spvExtract(s, s->floatType, v, 2)
#define W(v) spvExtract(s, s->floatType, v, 3)
#define AS_UINT(f) spvOp1(s, SPV_OP_Bitcast, s->uintType, f)

static SpvId vec3Of(Spv* s, SpvId x, SpvId y, SpvId z) { const SpvId p[3] = {x, y, z}; return spvCompose(s, s->vec3, p, 3); }
static SpvId vec4Of(Spv* s, SpvId v3, SpvId w) { const SpvId p[2] = {v3, w}; return spvCompose(s, s->vec4, p, 2); }
/* artfSrgbDecode and artfSrgbEncode (core/color.h), three at a time: what a picture's bytes mean as light, and back. */
static SpvId srgbDecode3(Spv* s, SpvId v) { return vec3Of(s, spvSrgbDecode(s, X(v)), spvSrgbDecode(s, Y(v)), spvSrgbDecode(s, Z(v))); }
static SpvId srgbEncode3(Spv* s, SpvId v) { return vec3Of(s, spvSrgbEncode(s, X(v)), spvSrgbEncode(s, Y(v)), spvSrgbEncode(s, Z(v))); }
static SpvId same3(Spv* s, float v) { SpvId c = spvConstFloat(s, v); return vec3Of(s, c, c, c); }
/* condition ? a : b */
static SpvId pick3(Spv* s, SpvId condition, SpvId a, SpvId b) {
    SpvId x = spvSelect(s, s->floatType, condition, spvExtract(s, s->floatType, a, 0), spvExtract(s, s->floatType, b, 0));
    SpvId y = spvSelect(s, s->floatType, condition, spvExtract(s, s->floatType, a, 1), spvExtract(s, s->floatType, b, 1));
    SpvId z = spvSelect(s, s->floatType, condition, spvExtract(s, s->floatType, a, 2), spvExtract(s, s->floatType, b, 2));
    return vec3Of(s, x, y, z);
}
/* An orthographic view: ArtfFrameData.ortho, how deep its depth goes, is above 0. */
static SpvId orthoOn(Spv* s, const Resources* r) { return spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, Z(frameVec(s, r, F_ORTHO)), spvConstFloat(s, 0.f)); }
/* Where the eye is for a point of the world: as artfEyeOf, the camera, or in an orthographic view the place on its
 * plane behind the point. */
static SpvId eyeOf(Spv* s, const Resources* r, SpvId world) {
    SpvId camera = spvShuffle3(s, frameVec(s, r, F_CAMERA)), ahead = spvShuffle3(s, frameVec(s, r, F_EYE + 2));
    SpvId onPlane = SUB_V3(world, SCALE_V3(ahead, DOT3(SUB_V3(world, camera), ahead)));
    return pick3(s, orthoOn(s, r), onPlane, camera);
}
/* As toneMap in kernels.inc, where exposure is above 0. */
static SpvId exposed(Spv* s, SpvId light, SpvId exposure) {
    SpvId v = spvOp2(s, SPV_OP_VectorTimesScalar, s->vec3, light, exposure);
    SpvId top = spvOp2(s, SPV_OP_FMul, s->vec3, v, spvOp2(s, SPV_OP_FAdd, s->vec3, spvOp2(s, SPV_OP_FMul, s->vec3, v, same3(s, 2.51f)), same3(s, .03f)));
    SpvId bottom = spvOp2(s, SPV_OP_FAdd, s->vec3, spvOp2(s, SPV_OP_FMul, s->vec3, v, spvOp2(s, SPV_OP_FAdd, s->vec3, spvOp2(s, SPV_OP_FMul, s->vec3, v, same3(s, 2.43f)), same3(s, .59f))), same3(s, .14f));
    SpvId mapped = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvOp2(s, SPV_OP_FDiv, s->vec3, top, bottom), same3(s, 1.f));
    return pick3(s, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, exposure, spvConstFloat(s, 0.f)), mapped, light);
}

/* As probeReach in trace.c: how far the probe whose map starts at the float map sees toward dir (of any length). */
static void probeReachSpv(Spv* s, const Resources* r, SpvId probes, SpvId map, SpvId dir, SpvId* mean, SpvId* square) {
#define PR_ABS(a) spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, a)
#define PR_SIGN(a) spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, a, FLOAT(0.f)), FLOAT(1.f), FLOAT(-1.f))
    const float side = (float)ARTF_PROBE_SIDE;
    SpvId sum = ADD_F(ADD_F(ADD_F(PR_ABS(X(dir)), PR_ABS(Y(dir))), PR_ABS(Z(dir))), FLOAT(1e-20f));
    SpvId over = DIV_F(FLOAT(1.f), sum);
    SpvId a = MUL_F(X(dir), over), b = MUL_F(Y(dir), over);
    SpvId below = spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, Z(dir), FLOAT(0.f));
    SpvId fa = MUL_F(SUB_F(FLOAT(1.f), PR_ABS(b)), PR_SIGN(a)), fb = MUL_F(SUB_F(FLOAT(1.f), PR_ABS(a)), PR_SIGN(b));
    a = spvSelect(s, s->floatType, below, fa, a);
    b = spvSelect(s, s->floatType, below, fb, b);
    SpvId u = ADD_F(MUL_F(a, FLOAT(.5f)), FLOAT(.5f)), v = ADD_F(MUL_F(b, FLOAT(.5f)), FLOAT(.5f));
    /* the nearest texel; two floats a texel, so a vec4 holds two side by side */
    SpvId x = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, MIN_F(spvGlsl1(s, SPV_GLSL_Floor, s->floatType, MUL_F(u, FLOAT(side))), FLOAT(side - 1.f)));
    SpvId y = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, MIN_F(spvGlsl1(s, SPV_GLSL_Floor, s->floatType, MUL_F(v, FLOAT(side))), FLOAT(side - 1.f)));
    SpvId first = ADD_U(map, MUL_U(ADD_U(MUL_U(y, UINT(ARTF_PROBE_SIDE)), x), UINT(2)));
    SpvId vec = bufferVec(s, r, probes, spvOp2(s, SPV_OP_UDiv, s->uintType, first, UINT(4)));
    SpvId odd = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, first, UINT(2)), UINT(0));
    *mean = spvSelect(s, s->floatType, odd, Z(vec), X(vec));
    *square = spvSelect(s, s->floatType, odd, W(vec), Y(vec));
#undef PR_ABS
#undef PR_SIGN
}

/* ---- fog that light goes through: the grid of froxels, as artfFroxelAt ---- */

/* What the grid (binding 56) says at a pixel (sx, sy: its center at whole numbers) and w along the view: light added,
 * and what is left of what is behind. */
static SpvId froxelAt(Spv* s, const Resources* r, SpvId grid, SpvId sx, SpvId sy, SpvId w) {
#define FX_FLOOR(a) spvGlsl1(s, SPV_GLSL_Floor, s->floatType, a)
#define FX_CLAMP(v, lo, hi) MIN_F(MAX_F(v, lo), hi)
#define FX_U(f) spvOp1(s, SPV_OP_ConvertFToU, s->uintType, f)
    const float tile = (float)ARTF_FROXEL_TILE, slices = (float)ARTF_FROXEL_SLICES;
    SpvId width = W(frameVec(s, r, F_FOG)), height = W(frameVec(s, r, F_EYE + 1));
    SpvId gxf = FX_FLOOR(DIV_F(ADD_F(width, FLOAT(tile - 1.f)), FLOAT(tile))), gyf = FX_FLOOR(DIV_F(ADD_F(height, FLOAT(tile - 1.f)), FLOAT(tile)));
    SpvId far = DIV_F(FLOAT(4.f), MAX_F(Y(frameVec(s, r, F_LOOK)), FLOAT(1e-6f)));
    SpvId fx = FX_CLAMP(DIV_F(SUB_F(sx, FLOAT(tile * .5f - .5f)), FLOAT(tile)), FLOAT(0.f), SUB_F(gxf, FLOAT(1.f)));
    SpvId fy = FX_CLAMP(DIV_F(SUB_F(sy, FLOAT(tile * .5f - .5f)), FLOAT(tile)), FLOAT(0.f), SUB_F(gyf, FLOAT(1.f)));
    SpvId fz = FX_CLAMP(SUB_F(MUL_F(spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, DIV_F(MAX_F(w, FLOAT(0.f)), far)), FLOAT(slices)), FLOAT(1.f)), FLOAT(-1.f), FLOAT(slices - 1.f));
    SpvId bx = FX_FLOOR(fx), by = FX_FLOOR(fy), bz = FX_FLOOR(fz);
    SpvId tx = SUB_F(fx, bx), ty = SUB_F(fy, by), tz = SUB_F(fz, bz);
    SpvId gx = FX_U(gxf);
    SpvId x0 = FX_U(bx), y0 = FX_U(by), x1 = FX_U(MIN_F(ADD_F(bx, FLOAT(1.f)), SUB_F(gxf, FLOAT(1.f)))), y1 = FX_U(MIN_F(ADD_F(by, FLOAT(1.f)), SUB_F(gyf, FLOAT(1.f))));
    SpvId nearEye = spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, bz, FLOAT(0.f));
    SpvId z0 = FX_U(MAX_F(bz, FLOAT(0.f))), z1 = FX_U(MIN_F(ADD_F(bz, FLOAT(1.f)), FLOAT(slices - 1.f)));
    SpvId slice[2];
    for (int k = 0; k < 2; ++k) {
        /* (a column's slices one after another) */
        SpvId z = k ? z1 : z0;
#define FX_CELL(yy, xx) bufferVec(s, r, grid, ADD_U(MUL_U(ADD_U(MUL_U(yy, gx), xx), UINT(ARTF_FROXEL_SLICES)), z))
        SpvId a = FX_CELL(y0, x0), b = FX_CELL(y0, x1), c = FX_CELL(y1, x0), d = FX_CELL(y1, x1);
#undef FX_CELL
        SpvId top = ADD_V4(a, SCALE_V4(spvOp2(s, SPV_OP_FSub, s->vec4, b, a), tx)), bottom = ADD_V4(c, SCALE_V4(spvOp2(s, SPV_OP_FSub, s->vec4, d, c), tx));
        slice[k] = ADD_V4(top, SCALE_V4(spvOp2(s, SPV_OP_FSub, s->vec4, bottom, top), ty));
    }
    /* (before the first slice: at the eye, nothing added and all left) */
    const SpvId eyeParts[4] = {FLOAT(0.f), FLOAT(0.f), FLOAT(0.f), FLOAT(1.f)};
    SpvId atEye = spvCompose(s, s->vec4, eyeParts, 4);
    SpvId first = vec4Of(s, pick3(s, nearEye, spvShuffle3(s, atEye), spvShuffle3(s, slice[0])), spvSelect(s, s->floatType, nearEye, FLOAT(1.f), W(slice[0])));
    return ADD_V4(first, SCALE_V4(spvOp2(s, SPV_OP_FSub, s->vec4, slice[1], first), tz));
#undef FX_FLOOR
#undef FX_CLAMP
#undef FX_U
}

/* ---- light from all around: as artfEnvSample and artfEnvReflected, on the same floats ---- */

static SpvId envFloat(Spv* s, const Resources* r, SpvId i) {
    const SpvId idx[2] = {r->zero, i};
    return spvLoad(s, s->floatType, spvAccess(s, r->ptrStorageFloat, r->environment, idx, 2));
}

/* What map level (a uint) holds toward d. */
static SpvId envSample(Spv* s, const Resources* r, SpvId level, SpvId d) {
#define E_IS(n) spvOp2(s, 170 /* OpIEqual */, s->boolType, level, spvConstUint(s, n))
#define E_PICKU(c, a, b) spvSelect(s, s->uintType, c, a, b)
#define E_F(v) spvConstFloat(s, v)
#define E_ADD(a, b) spvOp2(s, SPV_OP_FAdd, s->floatType, a, b)
#define E_SUB(a, b) spvOp2(s, SPV_OP_FSub, s->floatType, a, b)
#define E_MUL(a, b) spvOp2(s, SPV_OP_FMul, s->floatType, a, b)
#define E_ABS(a) spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, a)
#define E_SIGN(a) spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, a, E_F(0.f)), E_F(1.f), E_F(-1.f))
    SpvId n = E_PICKU(E_IS(0), spvConstUint(s, artfEnvSize(0)), E_PICKU(E_IS(1), spvConstUint(s, artfEnvSize(1)), E_PICKU(E_IS(2), spvConstUint(s, artfEnvSize(2)), spvConstUint(s, artfEnvSize(3)))));
    SpvId start = spvConstUint(s, artfEnvStart(ARTF_ENV_LEVELS - 1));
    for (int i = ARTF_ENV_LEVELS - 2; i >= 0; --i) start = E_PICKU(E_IS((uint32_t)i), spvConstUint(s, artfEnvStart(i)), start);
    SpvId x = X(d), y = Y(d), z = Z(d);
    SpvId sum = spvGlsl2(s, SPV_GLSL_FMax, s->floatType, E_ADD(E_ADD(E_ABS(x), E_ABS(y)), E_ABS(z)), E_F(1e-20f));
    SpvId a = spvOp2(s, SPV_OP_FDiv, s->floatType, x, sum), b = spvOp2(s, SPV_OP_FDiv, s->floatType, z, sum);
    SpvId below = spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, y, E_F(0.f));
    SpvId foldedA = E_MUL(E_SUB(E_F(1.f), E_ABS(b)), E_SIGN(a)), foldedB = E_MUL(E_SUB(E_F(1.f), E_ABS(a)), E_SIGN(b));
    a = spvSelect(s, s->floatType, below, foldedA, a);
    b = spvSelect(s, s->floatType, below, foldedB, b);
    SpvId nf = spvOp1(s, SPV_OP_ConvertUToF, s->floatType, n);
    SpvId tx = spvGlsl2(s, SPV_GLSL_FMin, s->floatType, spvGlsl2(s, SPV_GLSL_FMax, s->floatType, E_ADD(E_MUL(E_ADD(E_MUL(a, E_F(.5f)), E_F(.5f)), nf), E_F(.5f)), E_F(0.f)), nf);
    SpvId ty = spvGlsl2(s, SPV_GLSL_FMin, s->floatType, spvGlsl2(s, SPV_GLSL_FMax, s->floatType, E_ADD(E_MUL(E_ADD(E_MUL(b, E_F(.5f)), E_F(.5f)), nf), E_F(.5f)), E_F(0.f)), nf);
    SpvId cx = spvGlsl1(s, SPV_GLSL_Floor, s->floatType, tx), cy = spvGlsl1(s, SPV_GLSL_Floor, s->floatType, ty);
    SpvId fx = E_SUB(tx, cx), fy = E_SUB(ty, cy);
    SpvId ix = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, cx), iy = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, cy);
    SpvId w = spvOp2(s, SPV_OP_IAdd, s->uintType, n, spvConstUint(s, 2));
    SpvId at = spvOp2(s, SPV_OP_IAdd, s->uintType, start, spvOp2(s, SPV_OP_IMul, s->uintType, spvConstUint(s, 3), spvOp2(s, SPV_OP_IAdd, s->uintType, spvOp2(s, SPV_OP_IMul, s->uintType, iy, w), ix)));
    SpvId down = spvOp2(s, SPV_OP_IMul, s->uintType, w, spvConstUint(s, 3));
    SpvId out[3];
    for (uint32_t k = 0; k < 3; ++k) {
        SpvId i00 = spvOp2(s, SPV_OP_IAdd, s->uintType, at, spvConstUint(s, k));
        SpvId i10 = spvOp2(s, SPV_OP_IAdd, s->uintType, i00, spvConstUint(s, 3));
        SpvId i01 = spvOp2(s, SPV_OP_IAdd, s->uintType, i00, down), i11 = spvOp2(s, SPV_OP_IAdd, s->uintType, i01, spvConstUint(s, 3));
        SpvId p00 = envFloat(s, r, i00), p10 = envFloat(s, r, i10), p01 = envFloat(s, r, i01), p11 = envFloat(s, r, i11);
        SpvId top = E_ADD(p00, E_MUL(E_SUB(p10, p00), fx)), bottom = E_ADD(p01, E_MUL(E_SUB(p11, p01), fx));
        out[k] = E_ADD(top, E_MUL(E_SUB(bottom, top), fy));
    }
    return vec3Of(s, out[0], out[1], out[2]);
#undef E_IS
#undef E_PICKU
#undef E_ABS
#undef E_SIGN
}

/* Light reflected toward d by a surface this rough. */
static SpvId envReflected(Spv* s, const Resources* r, SpvId rough, SpvId d) {
    SpvId at = E_MUL(spvGlsl2(s, SPV_GLSL_FMin, s->floatType, spvGlsl2(s, SPV_GLSL_FMax, s->floatType, rough, E_F(0.f)), E_F(1.f)), E_F(4.f));
    SpvId first = spvGlsl2(s, SPV_GLSL_FMin, s->floatType, spvGlsl1(s, SPV_GLSL_Floor, s->floatType, at), E_F(3.f)), share = E_SUB(at, first);
    SpvId level = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, first);
    SpvId a = envSample(s, r, level, d), b = envSample(s, r, spvOp2(s, SPV_OP_IAdd, s->uintType, level, spvConstUint(s, 1)), d);
    return spvOp2(s, SPV_OP_FAdd, s->vec3, a, spvOp2(s, SPV_OP_VectorTimesScalar, s->vec3, spvOp2(s, SPV_OP_FSub, s->vec3, b, a), share));
#undef E_F
#undef E_ADD
#undef E_SUB
#undef E_MUL
}

static uint32_t* finish(Spv* s, size_t* size) {
    size_t words;
    uint32_t* code = spvFinish(s, &words);
    *size = words * 4;
    spvFini(s);
    return code;
}

/* ---- culling: one invocation an instance. What is in view goes to its mesh's draw command. ---- */

/* Adds a copy of instance i to mesh chosen's command, in the commands from base on. */
/* fade goes above the mesh's number: see the vertex shader. */
static void addCopy(Spv* s, const Resources* r, SpvId base, SpvId chosen, SpvId i, SpvId fade) {
    SpvId command = MUL_U(ADD_U(base, chosen), UINT(5));
    SpvId slot = spvAtomicAdd(s, bufferUintPtr(s, r, r->indirect, ADD_U(command, UINT(1))), UINT(1));
    SpvId first = spvLoad(s, s->uintType, bufferUintPtr(s, r, r->indirect, ADD_U(command, UINT(4))));
    SpvId at = MUL_U(ADD_U(first, slot), UINT(2));
    spvStore(s, bufferUintPtr(s, r, r->visible, at), i);
    spvStore(s, bufferUintPtr(s, r, r->visible, ADD_U(at, UINT(1))), spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, chosen, spvOp2(s, 196 /* OpShiftLeftLogical */, s->uintType, fade, UINT(23))));
}

/* What fades goes in twice, with commands that can leave pixels out. */
static void addDrawn(Spv* s, const Resources* r, SpvId base, SpvId cutBase, SpvId fading, SpvId chosen, SpvId other, SpvId fade, SpvId i) {
    SpvId once, twice;
    spvIfBegin(s, spvOp1(s, SPV_OP_LogicalNot, s->boolType, fading), &once);
    addCopy(s, r, base, chosen, i, UINT(0));
    spvIfEnd(s, once);
    spvIfBegin(s, fading, &twice);
    addCopy(s, r, cutBase, chosen, i, spvOp2(s, 197, s->uintType, spvOp2(s, 196, s->uintType, fade, UINT(1)), UINT(1)));
    addCopy(s, r, cutBase, other, i, spvOp2(s, 196, s->uintType, fade, UINT(1)));
    spvIfEnd(s, twice);
}

static SpvId hizTexel(Spv* s, const Resources* r, SpvId start, SpvId width, SpvId x, SpvId y) {
    SpvId bits = spvLoad(s, s->uintType, bufferUintPtr(s, r, r->hiz, ADD_U(start, ADD_U(MUL_U(y, width), x))));
    return spvOp1(s, SPV_OP_Bitcast, s->floatType, bits);
}

/* Is all of this sphere behind the depth pyramid? As sphereHidden in software.c: the sphere as a cube
 * facing the eye, whose corners give the rectangle it can reach; then the level where that rectangle
 * is at most two texels wide, and its corners there. Depth here is near / w. */
static SpvId sphereHidden(Spv* s, const Resources* r, SpvId world, SpvId radius) {
    SpvId wx = X(world), wy = Y(world), wz = Z(world);
    SpvId clip = ADD_V4(ADD_V4(SCALE_V4(frameVec(s, r, F_VIEWPROJ), wx), SCALE_V4(frameVec(s, r, F_VIEWPROJ + 1), wy)),
                        ADD_V4(SCALE_V4(frameVec(s, r, F_VIEWPROJ + 2), wz), frameVec(s, r, F_VIEWPROJ + 3)));
    SpvId cx = X(clip), cy = Y(clip), cw = W(clip);
    SpvId projection = frameVec(s, r, F_PROJECTION);
    SpvId nearW = Z(projection);
    /* (orthographic: w is 1, and the sphere's nearest point is its radius nearer along the view than its center) */
    SpvId ortho = orthoOn(s, r), depthRow = frameVec(s, r, F_ORTHO);
    SpvId along = DOT3(SUB_V3(spvShuffle3(s, world), spvShuffle3(s, frameVec(s, r, F_CAMERA))), spvShuffle3(s, frameVec(s, r, F_EYE + 2)));
    SpvId closest = SUB_F(spvSelect(s, s->floatType, ortho, along, cw), radius);
    SpvId safe = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, closest, spvSelect(s, s->floatType, ortho, SUB_F(W(depthRow), Z(depthRow)), nearW));
    SpvId nearest = spvSelect(s, s->floatType, ortho, FLOAT(1.f), MAX_F(closest, nearW)), farthest = spvSelect(s, s->floatType, ortho, FLOAT(1.f), ADD_F(cw, radius));
    SpvId px = MUL_F(radius, X(projection)), py = MUL_F(radius, Y(projection));
    SpvId left = SUB_F(cx, px), right = ADD_F(cx, px), down = SUB_F(cy, py), up = ADD_F(cy, py);
    SpvId xlo = MIN_F(DIV_F(left, nearest), DIV_F(left, farthest)), xhi = MAX_F(DIV_F(right, nearest), DIV_F(right, farthest));
    SpvId ylo = MIN_F(DIV_F(down, nearest), DIV_F(down, farthest)), yhi = MAX_F(DIV_F(up, nearest), DIV_F(up, farthest));

    SpvId base = frameVec(s, r, F_HIZ);
    SpvId w0 = AS_UINT(Y(base)), h0 = AS_UINT(Z(base));
    SpvId fw = spvOp1(s, SPV_OP_ConvertUToF, s->floatType, w0), fh = spvOp1(s, SPV_OP_ConvertUToF, s->floatType, h0);
#define UNIT(v) MIN_F(MAX_F(v, FLOAT(0.f)), FLOAT(1.f))
    SpvId x0 = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, MUL_F(UNIT(ADD_F(MUL_F(xlo, FLOAT(.5f)), FLOAT(.5f))), fw));
    SpvId x1 = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, MUL_F(UNIT(ADD_F(MUL_F(xhi, FLOAT(.5f)), FLOAT(.5f))), fw));
    SpvId y0 = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, MUL_F(UNIT(SUB_F(FLOAT(.5f), MUL_F(yhi, FLOAT(.5f)))), fh));
    SpvId y1 = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, MUL_F(UNIT(SUB_F(FLOAT(.5f), MUL_F(ylo, FLOAT(.5f)))), fh));
#undef UNIT
#define UMIN(a, b) spvGlsl2(s, SPV_GLSL_UMin, s->uintType, a, b)
#define UMAX(a, b) spvGlsl2(s, SPV_GLSL_UMax, s->uintType, a, b)
#define SUB_U(a, b) spvOp2(s, SPV_OP_ISub, s->uintType, a, b)
#define SHIFT(a, b) spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, a, b)
    SpvId size = UMAX(SUB_U(x1, x0), SUB_U(y1, y0));
    SpvId msb = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvGlsl1(s, SPV_GLSL_FindUMsb, s->intType, UMAX(size, UINT(1))));
    SpvId levels = AS_UINT(W(frameVec(s, r, F_COUNTS)));
    /* (at msb + 1 shifts the rectangle spans two texels at most, so its corners see all of it) */
    SpvId level = UMIN(ADD_U(msb, UINT(1)), SUB_U(levels, UINT(1)));
    SpvId row = frameVecAt(s, r, ADD_U(UINT(F_HIZ), level));
    SpvId start = AS_UINT(X(row)), width = AS_UINT(Y(row)), height = AS_UINT(Z(row));
    SpvId lastX = SUB_U(width, UINT(1)), lastY = SUB_U(height, UINT(1));
    SpvId tx0 = UMIN(SHIFT(x0, level), lastX), tx1 = UMIN(SHIFT(x1, level), lastX), ty0 = UMIN(SHIFT(y0, level), lastY), ty1 = UMIN(SHIFT(y1, level), lastY);
    SpvId far = MIN_F(MIN_F(hizTexel(s, r, start, width, tx0, ty0), hizTexel(s, r, start, width, tx1, ty0)),
                      MIN_F(hizTexel(s, r, start, width, tx0, ty1), hizTexel(s, r, start, width, tx1, ty1)));
    SpvId depth = spvSelect(s, s->floatType, ortho, DIV_F(SUB_F(W(depthRow), closest), MAX_F(Z(depthRow), FLOAT(1e-20f))), DIV_F(nearW, nearest));
    return spvOp2(s, SPV_OP_LogicalAnd, s->boolType, safe, spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, depth, far));
}

/* Meshlets on the GPU. Bindings: 40 of each, three rows: its sphere; its cone; where its triangles start and how many
 * indices; 41 of each mesh, four numbers: its first meshlet, how many, its first index and first vertex (0 meshlets: it is
 * drawn whole); 42 commands, one a meshlet of what is drawn this way; 43 first what a dispatch needs (three numbers), how
 * many commands, where the list has room, how many commands fit, where the second pass's half of this starts, where its
 * commands start; then four numbers a job: instance, mesh, first command. Occlusion's second pass has a half of its own. */
enum { M_GROUPS = 0, M_JOBS = 1, M_COMMANDS = 3, M_LIST = 4, M_CAPACITY = 5, M_LATE = 6, M_FIRST = 7, M_DRAWN = 8, M_HEADER = 12 };

/* What goes to a job rather than to a command of its own: what is not fitted waits for addDrawn. flags: where the
 * instance's meshlets say whether they showed. */
static void addMeshlets(Spv* s, const Resources* r, SpvId state, SpvId half, SpvId count, SpvId chosen, SpvId i, SpvId base, SpvId cutBase, SpvId fading, SpvId other, SpvId fade, SpvId flags) {
    SpvId capacity = spvLoad(s, s->uintType, bufferUintPtr(s, r, state, ADD_U(half, UINT(M_CAPACITY))));
    SpvId slot = spvAtomicAdd(s, bufferUintPtr(s, r, state, ADD_U(half, UINT(M_COMMANDS))), count);
    SpvId fits = spvOp2(s, 178 /* OpULessThanEqual */, s->boolType, ADD_U(slot, count), capacity);
    SpvId taken, left;
    spvIfBegin(s, fits, &taken);
    SpvId job = spvAtomicAdd(s, bufferUintPtr(s, r, state, ADD_U(half, UINT(M_JOBS))), UINT(1));
    SpvId at = ADD_U(ADD_U(half, UINT(M_HEADER)), MUL_U(job, UINT(4)));
    spvStore(s, bufferUintPtr(s, r, state, at), i);
    spvStore(s, bufferUintPtr(s, r, state, ADD_U(at, UINT(1))), chosen);
    spvStore(s, bufferUintPtr(s, r, state, ADD_U(at, UINT(2))), slot);
    spvStore(s, bufferUintPtr(s, r, state, ADD_U(at, UINT(3))), flags);
    SpvId list = MUL_U(ADD_U(spvLoad(s, s->uintType, bufferUintPtr(s, r, state, ADD_U(half, UINT(M_LIST)))), job), UINT(2));
    spvStore(s, bufferUintPtr(s, r, r->visible, list), i);
    spvStore(s, bufferUintPtr(s, r, r->visible, ADD_U(list, UINT(1))), chosen);
    spvIfEnd(s, taken);
    spvIfBegin(s, spvOp1(s, SPV_OP_LogicalNot, s->boolType, fits), &left);
    addDrawn(s, r, base, cutBase, fading, chosen, other, fade, i);
    spvIfEnd(s, left);
}

uint32_t* artfShaderCull(size_t* size, ArtfCullPhase phase) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    declare(s, &r, true);
    SpvId meshletMeshes = spvResource(s, blockOf(s, r.uintArray, false), SPV_STORAGE_StorageBuffer, 41);
    SpvId meshletState = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 43);
    SpvId flagStarts = phase == ARTF_CULL_ALL ? 0 : spvResource(s, blockOf(s, r.uintArray, false), SPV_STORAGE_StorageBuffer, 47);
    SpvId uvec3 = spvNewId(s);
    spvEmit(&s->types, 23 /* OpTypeVector */, SPV_WORDS(uvec3, s->uintType, 3));
    SpvId global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, uvec3), SPV_STORAGE_Input);
    spvDecorate(s, global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);

    SpvId main = spvBeginMain(s);
    SpvId i = spvExtract(s, s->uintType, spvLoad(s, uvec3, global), 0);
    SpvId counts = frameVec(s, &r, F_COUNTS);
    SpvId merge;
    spvIfBegin(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, i, AS_UINT(Y(counts))), &merge);

    SpvId base = MUL_U(i, UINT(4));
    SpvId r0 = bufferVec(s, &r, r.instances, base), r1 = bufferVec(s, &r, r.instances, ADD_U(base, UINT(1)));
    SpvId r2 = bufferVec(s, &r, r.instances, ADD_U(base, UINT(2))), meta = bufferVec(s, &r, r.instances, ADD_U(base, UINT(3)));
    SpvId mesh = AS_UINT(X(meta));
    SpvId meshBase = MUL_U(mesh, UINT(5));
    SpvId sphere = bufferVec(s, &r, r.meshes, meshBase);
    SpvId center = vec4Of(s, spvShuffle3(s, sphere), FLOAT(1.f));
    SpvId world3 = vec3Of(s, DOT4(r0, center), DOT4(r1, center), DOT4(r2, center));
    SpvId world = vec4Of(s, world3, FLOAT(1.f));
    SpvId radius = MUL_F(W(sphere), Z(meta));
    /* the nearest plane's distance, which is the one that counts */
    SpvId least = DOT4(frameVec(s, &r, F_PLANES), world);
    for (uint32_t p = 1; p < 5; ++p) least = MIN_F(least, DOT4(frameVec(s, &r, F_PLANES + p), world));
    /* (what shows what is behind it is not for this: the CPU puts layers in order, and they come after) */
    SpvId solid = spvOp2(s, SPV_OP_ULessThan, s->boolType, AS_UINT(W(meta)), UINT(1u << 24));
    /* (and what has holes or backs goes to commands of its own, two parts on) */
    SpvId meshCount = AS_UINT(W(frameVec(s, &r, F_PROJECTION)));
    SpvId cut = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, AS_UINT(W(meta)), UINT((uint32_t)ARTF_INSTANCE_CUTOUT << 16)), UINT(0));
    SpvId part = spvSelect(s, s->uintType, cut, MUL_U(meshCount, UINT(2)), UINT(0));
    SpvId inView = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, solid, spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, ADD_F(least, radius), FLOAT(0.f)));

    /* the mesh this size and distance call for: as artfMeshLevel */
    SpvId eye = spvShuffle3(s, frameVec(s, &r, F_CAMERA));
    SpvId nearW = Z(frameVec(s, &r, F_PROJECTION));
    SpvId distance = MAX_F(SUB_F(spvGlsl1(s, SPV_GLSL_Length, s->floatType, SUB_V3(world3, eye)), radius), nearW);
    /* (orthographic: all are as at 1, as artfSeenDistance) */
    distance = spvSelect(s, s->floatType, orthoOn(s, &r), FLOAT(1.f), distance);
    /* what is too small to see: as artfTooSmall */
    SpvId cullScale = X(frameVec(s, &r, F_CULL));
    SpvId tooSmall = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, cullScale, FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, MUL_F(radius, cullScale), distance));
    inView = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, inView, spvOp1(s, SPV_OP_LogicalNot, s->boolType, tooSmall));
    SpvId lodIds = bufferVec(s, &r, r.meshes, ADD_U(meshBase, UINT(3))), lodErrors = bufferVec(s, &r, r.meshes, ADD_U(meshBase, UINT(4)));
    SpvId lodScale = Z(counts);
    SpvId lodOn = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, lodScale, FLOAT(0.f));
    SpvId scaled = MUL_F(Z(meta), lodScale);
    SpvId chosen = mesh, other = mesh, limit = FLOAT(0.f);
    for (uint32_t level = 0; level < 3; ++level) {
        SpvId error = spvExtract(s, s->floatType, lodErrors, level); /* (below 0: no such level) */
        SpvId fits = spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, MUL_F(error, scaled), distance);
        SpvId exists = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, error, FLOAT(0.f));
        SpvId take = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, lodOn, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, fits, exists));
        other = spvSelect(s, s->uintType, take, chosen, other);
        limit = spvSelect(s, s->floatType, take, MUL_F(error, scaled), limit);
        chosen = spvSelect(s, s->uintType, take, AS_UINT(spvExtract(s, s->floatType, lodIds, level)), chosen);
    }
    /* as artfMeshFade */
    SpvId band = W(frameVec(s, &r, F_LOOK));
    SpvId positive = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, band, FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, limit, FLOAT(0.f)));
    SpvId fading = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, positive, spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, distance, MUL_F(limit, ADD_F(FLOAT(1.f), band))));
    SpvId along = DIV_F(SUB_F(distance, limit), MAX_F(MUL_F(limit, band), FLOAT(1e-20f)));
    SpvId fade = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvOp1(s, SPV_OP_ConvertFToS, s->intType, MIN_F(MAX_F(MUL_F(along, FLOAT(255.f)), FLOAT(1.f)), FLOAT(254.f))));
    SpvId cutPart = MUL_U(meshCount, UINT(2));

    /* a large mesh goes by its meshlets, if there is a place for it: not what has holes, or fades */
    SpvId half = phase == ARTF_CULL_LATE ? spvLoad(s, s->uintType, bufferUintPtr(s, &r, meshletState, UINT(M_LATE))) : UINT(0);
    SpvId meshlets = spvLoad(s, s->uintType, bufferUintPtr(s, &r, meshletMeshes, ADD_U(MUL_U(chosen, UINT(4)), UINT(1))));
    SpvId on = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvLoad(s, s->uintType, bufferUintPtr(s, &r, meshletState, ADD_U(half, UINT(M_CAPACITY)))), UINT(0));
    SpvId plain = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp1(s, SPV_OP_LogicalNot, s->boolType, cut), spvOp1(s, SPV_OP_LogicalNot, s->boolType, fading));
    SpvId byMeshlets = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, on, plain), spvOp2(s, SPV_OP_INotEqual, s->boolType, meshlets, UINT(0)));
    /* (with occlusion each meshlet remembers: one with no room for that goes whole) */
    SpvId flags = phase == ARTF_CULL_ALL ? UINT(0) : spvLoad(s, s->uintType, bufferUintPtr(s, &r, flagStarts, i));
    if (phase != ARTF_CULL_ALL) byMeshlets = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, byMeshlets, spvOp2(s, SPV_OP_INotEqual, s->boolType, flags, UINT(0xFFFFFFFFu)));
    SpvId notByMeshlets = spvOp1(s, SPV_OP_LogicalNot, s->boolType, byMeshlets);
    SpvId drawHere, whole;
    if (phase == ARTF_CULL_ALL) {
        spvIfBegin(s, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, inView, byMeshlets), &drawHere);
        addMeshlets(s, &r, meshletState, half, meshlets, chosen, i, part, cutPart, fading, other, fade, flags);
        spvIfEnd(s, drawHere);
        spvIfBegin(s, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, inView, notByMeshlets), &whole);
        addDrawn(s, &r, part, cutPart, fading, chosen, other, fade, i);
        spvIfEnd(s, whole);
    } else if (phase == ARTF_CULL_EARLY) {
        /* what showed last frame */
        SpvId shown = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvLoad(s, s->uintType, bufferUintPtr(s, &r, r.flags, i)), UINT(0));
        SpvId here = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, inView, shown);
        spvIfBegin(s, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, here, byMeshlets), &drawHere);
        addMeshlets(s, &r, meshletState, half, meshlets, chosen, i, part, cutPart, fading, other, fade, flags);
        spvIfEnd(s, drawHere);
        spvIfBegin(s, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, here, notByMeshlets), &whole);
        addDrawn(s, &r, part, cutPart, fading, chosen, other, fade, i);
        spvIfEnd(s, whole);
    } else {
        /* everything in view is tested: what shows now is what goes first next frame */
        SpvId flag = bufferUintPtr(s, &r, r.flags, i);
        SpvId shown = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvLoad(s, s->uintType, flag), UINT(0));
        SpvId hidden = sphereHidden(s, &r, world, radius);
        SpvId visible = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, inView, spvOp1(s, SPV_OP_LogicalNot, s->boolType, hidden));
        spvStore(s, flag, spvSelect(s, s->uintType, visible, UINT(1), UINT(0)));
        SpvId here = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, visible, spvOp1(s, SPV_OP_LogicalNot, s->boolType, shown));
        /* (what showed last frame still has meshlets to come; what did not has all of them, whatever they remember:
         * its top bit says so. What is hidden whole goes no further) */
        spvIfBegin(s, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, visible, byMeshlets), &drawHere);
        SpvId fresh = spvSelect(s, s->uintType, shown, UINT(0), UINT(1u << 31));
        addMeshlets(s, &r, meshletState, half, meshlets, chosen, i, ADD_U(meshCount, part), ADD_U(meshCount, cutPart), fading, other, fade, spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, flags, fresh));
        spvIfEnd(s, drawHere);
        spvIfBegin(s, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, here, notByMeshlets), &whole);
        addDrawn(s, &r, ADD_U(meshCount, part), ADD_U(meshCount, cutPart), fading, chosen, other, fade, i);
        spvIfEnd(s, whole);
    }
    spvIfEnd(s, merge);
    spvEndMain(s);

    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &global, 1);
    spvExecutionMode(s, main, 17 /* LocalSize */, 64, 1, 1, 3);
    return finish(s, size);
}

/* A meshlet of a job: an invocation each. As meshletHidden on the CPU, with what it has of the world. */
uint32_t* artfShaderMeshletCull(size_t* size, ArtfCullPhase phase) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    declare(s, &r, true);
    SpvId table = spvResource(s, blockOf(s, r.vec4Array, false), SPV_STORAGE_StorageBuffer, 40);
    SpvId meshes = spvResource(s, blockOf(s, r.uintArray, false), SPV_STORAGE_StorageBuffer, 41);
    SpvId commands = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 42);
    SpvId state = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 43);
    SpvId flags = phase == ARTF_CULL_ALL ? 0 : spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 46);
    SpvId uvec3 = spvNewId(s);
    spvEmit(&s->types, 23 /* OpTypeVector */, SPV_WORDS(uvec3, s->uintType, 3));
    SpvId global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, uvec3), SPV_STORAGE_Input);
    spvDecorate(s, global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);
    SpvId main = spvBeginMain(s);
    SpvId id = spvLoad(s, uvec3, global);
    SpvId i = spvExtract(s, s->uintType, id, 0), job = spvExtract(s, s->uintType, id, 1);
    SpvId half = phase == ARTF_CULL_LATE ? spvLoad(s, s->uintType, bufferUintPtr(s, &r, state, UINT(M_LATE))) : UINT(0);
    SpvId at = ADD_U(ADD_U(half, UINT(M_HEADER)), MUL_U(job, UINT(4)));
    SpvId instance = spvLoad(s, s->uintType, bufferUintPtr(s, &r, state, at));
    SpvId mesh = spvLoad(s, s->uintType, bufferUintPtr(s, &r, state, ADD_U(at, UINT(1))));
    SpvId info = MUL_U(mesh, UINT(4));
    SpvId first = spvLoad(s, s->uintType, bufferUintPtr(s, &r, meshes, info)), count = spvLoad(s, s->uintType, bufferUintPtr(s, &r, meshes, ADD_U(info, UINT(1))));
    SpvId firstIndex = spvLoad(s, s->uintType, bufferUintPtr(s, &r, meshes, ADD_U(info, UINT(2)))), firstVertex = spvLoad(s, s->uintType, bufferUintPtr(s, &r, meshes, ADD_U(info, UINT(3))));
    SpvId merge;
    spvIfBegin(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, i, count), &merge);
    SpvId row = MUL_U(ADD_U(first, i), UINT(3));
    SpvId sphere = bufferVec(s, &r, table, row), cone = bufferVec(s, &r, table, ADD_U(row, UINT(1))), range = bufferVec(s, &r, table, ADD_U(row, UINT(2)));
    SpvId base = MUL_U(instance, UINT(4));
    SpvId r0 = bufferVec(s, &r, r.instances, base), r1 = bufferVec(s, &r, r.instances, ADD_U(base, UINT(1)));
    SpvId r2 = bufferVec(s, &r, r.instances, ADD_U(base, UINT(2))), meta = bufferVec(s, &r, r.instances, ADD_U(base, UINT(3)));
    SpvId center = vec4Of(s, spvShuffle3(s, sphere), FLOAT(1.f));
    SpvId world3 = vec3Of(s, DOT4(r0, center), DOT4(r1, center), DOT4(r2, center));
    SpvId world = vec4Of(s, world3, FLOAT(1.f));
    SpvId radius = MUL_F(W(sphere), Z(meta));
    SpvId least = DOT4(frameVec(s, &r, F_PLANES), world);
    for (uint32_t p = 1; p < 5; ++p) least = MIN_F(least, DOT4(frameVec(s, &r, F_PLANES + p), world));
    SpvId seen = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, ADD_F(least, radius), FLOAT(0.f));
    /* its cone, in the world, where the instance keeps its shape */
    SpvId uniform = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, AS_UINT(W(meta)), UINT((uint32_t)ARTF_INSTANCE_UNIFORM << 16)), UINT(0));
    SpvId axis4 = vec4Of(s, spvShuffle3(s, cone), FLOAT(0.f));
    SpvId inverse = DIV_F(FLOAT(1.f), MAX_F(Z(meta), FLOAT(1e-20f)));
    SpvId axis = SCALE_V3(vec3Of(s, DOT4(r0, axis4), DOT4(r1, axis4), DOT4(r2, axis4)), inverse);
    SpvId toIt = SUB_V3(world3, spvShuffle3(s, frameVec(s, &r, F_CAMERA)));
    SpvId along = DOT3(toIt, axis);
    SpvId reach = ADD_F(MUL_F(W(cone), spvGlsl1(s, SPV_GLSL_Length, s->floatType, toIt)), radius);
    /* (orthographic: all rays go the way the view looks, as meshletCulled in kernels.inc) */
    SpvId ortho = orthoOn(s, &r);
    along = spvSelect(s, s->floatType, ortho, DOT3(spvShuffle3(s, frameVec(s, &r, F_EYE + 2)), axis), along);
    reach = spvSelect(s, s->floatType, ortho, W(cone), reach);
    SpvId hasCone = spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, W(cone), FLOAT(2.f));
    SpvId away = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, uniform, hasCone), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, along, reach));
    SpvId shown = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, seen, spvOp1(s, SPV_OP_LogicalNot, s->boolType, away));
    /* occlusion, as for whole instances: what showed last frame goes first; the rest by what that left in the pyramid */
    if (phase != ARTF_CULL_ALL) {
        SpvId word = spvLoad(s, s->uintType, bufferUintPtr(s, &r, state, ADD_U(at, UINT(3))));
        SpvId mine = bufferUintPtr(s, &r, flags, ADD_U(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, word, UINT(0x7FFFFFFFu)), i));
        SpvId wasDrawn = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, word, UINT(1u << 31)), UINT(1u << 31));
        SpvId showedBefore = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, wasDrawn, spvOp2(s, SPV_OP_INotEqual, s->boolType, spvLoad(s, s->uintType, mine), UINT(0)));
        if (phase == ARTF_CULL_EARLY) {
            shown = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, shown, showedBefore);
        } else {
            SpvId visible = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, shown, spvOp1(s, SPV_OP_LogicalNot, s->boolType, sphereHidden(s, &r, world, radius)));
            spvStore(s, mine, spvSelect(s, s->uintType, visible, UINT(1), UINT(0)));
            shown = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, visible, spvOp1(s, SPV_OP_LogicalNot, s->boolType, showedBefore));
        }
    }
    /* (a command only for what is drawn: an empty one still takes the GPU's time) */
    SpvId drawHere;
    spvIfBegin(s, shown, &drawHere);
    SpvId slot = spvAtomicAdd(s, bufferUintPtr(s, &r, state, ADD_U(half, UINT(M_DRAWN))), UINT(1));
    SpvId command = MUL_U(ADD_U(spvLoad(s, s->uintType, bufferUintPtr(s, &r, state, ADD_U(half, UINT(M_FIRST)))), slot), UINT(5));
    SpvId listStart = spvLoad(s, s->uintType, bufferUintPtr(s, &r, state, ADD_U(half, UINT(M_LIST))));
    spvStore(s, bufferUintPtr(s, &r, commands, command), AS_UINT(Y(range)));
    spvStore(s, bufferUintPtr(s, &r, commands, ADD_U(command, UINT(1))), UINT(1));
    spvStore(s, bufferUintPtr(s, &r, commands, ADD_U(command, UINT(2))), ADD_U(firstIndex, AS_UINT(X(range))));
    spvStore(s, bufferUintPtr(s, &r, commands, ADD_U(command, UINT(3))), firstVertex);
    spvStore(s, bufferUintPtr(s, &r, commands, ADD_U(command, UINT(4))), ADD_U(listStart, job));
    spvIfEnd(s, drawHere);
    spvIfEnd(s, merge);
    spvEndMain(s);
    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &global, 1);
    spvExecutionMode(s, main, 17 /* LocalSize */, 64, 1, 1, 3);
    return finish(s, size);
}

/* ---- the depth pyramid: each texel, the farthest of the four below it ---- */

uint32_t* artfShaderHiz(size_t* size, bool fromDepth, int samples) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    declare(s, &r, true);
    SpvId uvec3 = spvNewId(s), ivec2 = spvNewId(s);
    spvEmit(&s->types, 23, SPV_WORDS(uvec3, s->uintType, 3));
    spvEmit(&s->types, 23, SPV_WORDS(ivec2, s->intType, 2));
    SpvId global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, uvec3), SPV_STORAGE_Input);
    spvDecorate(s, global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);
    SpvId pushBlock = spvTypeStruct(s, &s->uintType, 1);
    spvDecorate(s, pushBlock, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, pushBlock, 0, SPV_DECORATE_Offset, 0);
    SpvId push = spvVariable(s, spvTypePointer(s, SPV_STORAGE_PushConstant, pushBlock), SPV_STORAGE_PushConstant);
    SpvId imageType = 0, sampled = samples > 1 ? spvTypeSampledImage2DMS(s, &imageType) : spvTypeSampledImage2D(s, &imageType), depth = 0;
    if (fromDepth) depth = spvResource(s, sampled, SPV_STORAGE_UniformConstant, 8);

    SpvId main = spvBeginMain(s);
    SpvId i = spvExtract(s, s->uintType, spvLoad(s, uvec3, global), 0);
    SpvId level = spvLoad(s, s->uintType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_PushConstant, s->uintType), push, &r.zero, 1));
    SpvId row = frameVecAt(s, &r, ADD_U(UINT(F_HIZ), level));
    SpvId start = AS_UINT(X(row)), width = AS_UINT(Y(row)), height = AS_UINT(Z(row));
    SpvId merge;
    spvIfBegin(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, i, MUL_U(width, height)), &merge);
    SpvId x = spvOp2(s, SPV_OP_UMod, s->uintType, i, width), y = spvOp2(s, SPV_OP_UDiv, s->uintType, i, width);
    SpvId x0 = MUL_U(x, UINT(2)), y0 = MUL_U(y, UINT(2));
    SpvId from = frameVecAt(s, &r, fromDepth ? UINT(F_HIZ + 14) : ADD_U(UINT(F_HIZ - 1), level));
    SpvId lastX = SUB_U(AS_UINT(Y(from)), UINT(1)), lastY = SUB_U(AS_UINT(Z(from)), UINT(1));
    SpvId x1 = UMIN(ADD_U(x0, UINT(1)), lastX), y1 = UMIN(ADD_U(y0, UINT(1)), lastY);
    x0 = UMIN(x0, lastX);
    y0 = UMIN(y0, lastY);
    SpvId values[4];
    const SpvId xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
    for (int k = 0; k < 4; ++k) {
        if (fromDepth) {
            const SpvId xy[2] = {spvOp1(s, SPV_OP_Bitcast, s->intType, xs[k]), spvOp1(s, SPV_OP_Bitcast, s->intType, ys[k])};
            SpvId coord = spvCompose(s, ivec2, xy, 2);
            values[k] = samples > 1 ? X(spvImageFetchSample(s, imageType, spvLoad(s, sampled, depth), coord, spvConstInt(s, 0))) : X(spvImageFetch(s, imageType, spvLoad(s, sampled, depth), coord));
            for (int n = 1; n < samples; ++n) values[k] = MIN_F(values[k], X(spvImageFetchSample(s, imageType, spvLoad(s, sampled, depth), coord, spvConstInt(s, n))));
        } else {
            values[k] = hizTexel(s, &r, AS_UINT(X(from)), AS_UINT(Y(from)), xs[k], ys[k]);
        }
    }
    SpvId far = MIN_F(MIN_F(values[0], values[1]), MIN_F(values[2], values[3]));
    spvStore(s, bufferUintPtr(s, &r, r.hiz, ADD_U(start, i)), AS_UINT(far));
    spvIfEnd(s, merge);
    spvEndMain(s);

    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &global, 1);
    spvExecutionMode(s, main, 17, 64, 1, 1, 3);
    return finish(s, size);
}

/* ---- the scene: vertices ---- */

uint32_t* artfShaderSceneVertex(size_t* size, bool motion) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    declare(s, &r, false);
    SpvId colors = spvResource(s, blockOf(s, r.uintArray, false), SPV_STORAGE_StorageBuffer, 16);
    /* (with motion: what the vertices were a frame ago, as they are in the vertex buffer, for what a pose bent) */
    SpvId pastVertices = motion ? spvResource(s, blockOf(s, r.uintArray, false), SPV_STORAGE_StorageBuffer, 68) : 0;

    SpvId ptrIn4 = spvTypePointer(s, SPV_STORAGE_Input, s->vec4), ptrIn2 = spvTypePointer(s, SPV_STORAGE_Input, s->vec2), ptrInInt = spvTypePointer(s, SPV_STORAGE_Input, s->intType);
    SpvId aPosition = spvVariable(s, ptrIn4, SPV_STORAGE_Input), aNormal = spvVariable(s, ptrIn2, SPV_STORAGE_Input), aUv = spvVariable(s, ptrIn2, SPV_STORAGE_Input);
    spvDecorate(s, aPosition, SPV_DECORATE_Location, 0);
    spvDecorate(s, aNormal, SPV_DECORATE_Location, 1);
    spvDecorate(s, aUv, SPV_DECORATE_Location, 2);
    SpvId instanceIndex = spvVariable(s, ptrInInt, SPV_STORAGE_Input);
    spvDecorate(s, instanceIndex, SPV_DECORATE_BuiltIn, SPV_BUILTIN_InstanceIndex);
    SpvId vertexIndex = spvVariable(s, ptrInInt, SPV_STORAGE_Input);
    spvDecorate(s, vertexIndex, SPV_DECORATE_BuiltIn, SPV_BUILTIN_VertexIndex);

    SpvId perVertex = spvTypeStruct(s, &s->vec4, 1);
    spvDecorate(s, perVertex, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, perVertex, 0, SPV_DECORATE_BuiltIn, SPV_BUILTIN_Position);
    /* (a layer's depth is drawn first, then its colors where depth is equal: both must get the same bits) */
    spvMemberDecorate(s, perVertex, 0, SPV_DECORATE_Invariant, -1);
    SpvId outVertex = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, perVertex), SPV_STORAGE_Output);
    SpvId ptrOut3 = spvTypePointer(s, SPV_STORAGE_Output, s->vec3), ptrOut2 = spvTypePointer(s, SPV_STORAGE_Output, s->vec2), ptrOutUint = spvTypePointer(s, SPV_STORAGE_Output, s->uintType);
    SpvId vWorld = spvVariable(s, ptrOut3, SPV_STORAGE_Output), vNormal = spvVariable(s, ptrOut3, SPV_STORAGE_Output), vUv = spvVariable(s, ptrOut2, SPV_STORAGE_Output), vMaterial = spvVariable(s, ptrOutUint, SPV_STORAGE_Output);
    spvDecorate(s, vWorld, SPV_DECORATE_Location, 0);
    spvDecorate(s, vNormal, SPV_DECORATE_Location, 1);
    spvDecorate(s, vUv, SPV_DECORATE_Location, 2);
    spvDecorate(s, vMaterial, SPV_DECORATE_Location, 3);
    spvDecorate(s, vMaterial, SPV_DECORATE_Flat, -1);
    SpvId vColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, vColor, SPV_DECORATE_Location, 4);
    SpvId vOpacity = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->floatType), SPV_STORAGE_Output);
    spvDecorate(s, vOpacity, SPV_DECORATE_Location, 5);
    spvDecorate(s, vOpacity, SPV_DECORATE_Flat, -1);
    SpvId vFade = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->uintType), SPV_STORAGE_Output);
    spvDecorate(s, vFade, SPV_DECORATE_Location, 6);
    spvDecorate(s, vFade, SPV_DECORATE_Flat, -1);
    SpvId vInstance = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->uintType), SPV_STORAGE_Output);
    spvDecorate(s, vInstance, SPV_DECORATE_Location, 7);
    spvDecorate(s, vInstance, SPV_DECORATE_Flat, -1);
    SpvId vPast = 0;
    if (motion) {
        vPast = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
        spvDecorate(s, vPast, SPV_DECORATE_Location, 8);
    }

    SpvId main = spvBeginMain(s);
    /* the copy drawn is this instance */
    SpvId copy = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvLoad(s, s->intType, instanceIndex));
    SpvId at = MUL_U(copy, UINT(2));
    SpvId id = spvLoad(s, s->uintType, bufferUintPtr(s, &r, r.visible, at));
    /* (a mesh's number, and above it how far it is in a fade) */
    SpvId meshWord = spvLoad(s, s->uintType, bufferUintPtr(s, &r, r.visible, ADD_U(at, UINT(1))));
    SpvId mesh = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, meshWord, UINT((1u << 23) - 1));
    spvStore(s, vFade, meshWord); /* (the fragment shader takes the fade from above bit 23) */
    spvStore(s, vInstance, id);
    SpvId base = MUL_U(id, UINT(4));
    SpvId r0 = bufferVec(s, &r, r.instances, base), r1 = bufferVec(s, &r, r.instances, ADD_U(base, UINT(1)));
    SpvId r2 = bufferVec(s, &r, r.instances, ADD_U(base, UINT(2))), meta = bufferVec(s, &r, r.instances, ADD_U(base, UINT(3)));
    SpvId meshBase = MUL_U(mesh, UINT(5));
    SpvId scaleRow = bufferVec(s, &r, r.meshes, ADD_U(meshBase, UINT(1))), offsetRow = bufferVec(s, &r, r.meshes, ADD_U(meshBase, UINT(2)));
    SpvId scale = spvShuffle3(s, scaleRow), offset = spvShuffle3(s, offsetRow);

    SpvId p = ADD_V3(MUL_V3(spvShuffle3(s, spvLoad(s, s->vec4, aPosition)), scale), offset);
    SpvId p4 = vec4Of(s, p, FLOAT(1.f));
    SpvId wx = DOT4(r0, p4), wy = DOT4(r1, p4), wz = DOT4(r2, p4);
    SpvId clip = ADD_V4(ADD_V4(SCALE_V4(frameVec(s, &r, F_VIEWPROJ), wx), SCALE_V4(frameVec(s, &r, F_VIEWPROJ + 1), wy)),
                        ADD_V4(SCALE_V4(frameVec(s, &r, F_VIEWPROJ + 2), wz), frameVec(s, &r, F_VIEWPROJ + 3)));
    const SpvId member = r.zero;
    spvStore(s, spvAccess(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), outVertex, &member, 1), clip);
    spvStore(s, vWorld, vec3Of(s, wx, wy, wz));

    /* the normal unfolded from the octahedron, through the matrix's cofactors */
    SpvId oct = spvLoad(s, s->vec2, aNormal);
    SpvId ox = spvExtract(s, s->floatType, oct, 0), oy = spvExtract(s, s->floatType, oct, 1);
    SpvId oz = SUB_F(SUB_F(FLOAT(1.f), spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, ox)), spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, oy));
    SpvId fold = MAX_F(spvOp1(s, SPV_OP_FNegate, s->floatType, oz), FLOAT(0.f));
    SpvId positiveX = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, ox, FLOAT(0.f)), positiveY = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, oy, FLOAT(0.f));
    SpvId nx = ADD_F(ox, spvSelect(s, s->floatType, positiveX, spvOp1(s, SPV_OP_FNegate, s->floatType, fold), fold));
    SpvId ny = ADD_F(oy, spvSelect(s, s->floatType, positiveY, spvOp1(s, SPV_OP_FNegate, s->floatType, fold), fold));
    SpvId c0 = vec3Of(s, X(r0), X(r1), X(r2)), c1 = vec3Of(s, Y(r0), Y(r1), Y(r2)), c2 = vec3Of(s, Z(r0), Z(r1), Z(r2));
    SpvId k0 = spvGlsl2(s, SPV_GLSL_Cross, s->vec3, c1, c2), k1 = spvGlsl2(s, SPV_GLSL_Cross, s->vec3, c2, c0), k2 = spvGlsl2(s, SPV_GLSL_Cross, s->vec3, c0, c1);
    SpvId normal = ADD_V3(ADD_V3(SCALE_V3(k0, nx), SCALE_V3(k1, ny)), SCALE_V3(k2, oz));
    spvStore(s, vNormal, NORMALIZE(normal));
    spvStore(s, vUv, spvLoad(s, s->vec2, aUv));
    spvStore(s, vMaterial, AS_UINT(Y(meta)));

    /* its color, where the mesh has colors; as light, as a texture's texels are; and how much of it shows */
    SpvId colorBase = AS_UINT(W(scaleRow)), firstVertex = AS_UINT(W(offsetRow));
    SpvId vertex = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvLoad(s, s->intType, vertexIndex));
    SpvId colored = spvOp2(s, SPV_OP_INotEqual, s->boolType, colorBase, UINT(0));
    SpvId colorAt = spvSelect(s, s->uintType, colored, ADD_U(colorBase, spvOp2(s, SPV_OP_ISub, s->uintType, vertex, firstVertex)), UINT(0));
    SpvId bgra = spvGlsl1(s, SPV_GLSL_UnpackUnorm4x8, s->vec4, spvLoad(s, s->uintType, bufferUintPtr(s, &r, colors, colorAt)));
    SpvId rgb = vec3Of(s, Z(bgra), Y(bgra), X(bgra));
    SpvId opacityBits = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, AS_UINT(W(meta)), UINT(65535));
    SpvId opacity = DIV_F(spvOp1(s, SPV_OP_ConvertUToF, s->floatType, opacityBits), FLOAT(65535.f));
    spvStore(s, vColor, vec4Of(s, srgbDecode3(s, rgb), W(bgra)));
    spvStore(s, vOpacity, opacity);
    if (motion) {
        /* where a vertex that a pose moved was: its place a frame ago, through its instance as it was (the motion's rows,
         * after the instances, four each: the fourth says it was bent) and the view before. The instance's top bit says
         * so to the pixels, which read this only then. */
        /* (only the scene's instances have motion: particles of emitters of many come after it) */
        SpvId instanceCount = AS_UINT(Y(frameVec(s, &r, F_COUNTS)));
        SpvId moving = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, Y(frameVec(s, &r, F_CULL)), FLOAT(0.f)),
                              spvOp2(s, SPV_OP_ULessThan, s->boolType, id, instanceCount)), movingMerge;
        spvIfBegin(s, moving, &movingMerge);
        SpvId row = ADD_U(MUL_U(instanceCount, UINT(4)), MUL_U(id, UINT(4)));
        SpvId bentMerge;
        spvIfBegin(s, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, X(bufferVec(s, &r, r.instances, ADD_U(row, UINT(3)))), FLOAT(0.f)), &bentMerge);
        SpvId word0 = spvLoad(s, s->uintType, bufferUintPtr(s, &r, pastVertices, MUL_U(vertex, UINT(4))));
        SpvId word1 = spvLoad(s, s->uintType, bufferUintPtr(s, &r, pastVertices, ADD_U(MUL_U(vertex, UINT(4)), UINT(1))));
        SpvId unit = vec3Of(s, spvOp1(s, SPV_OP_ConvertUToF, s->floatType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, word0, UINT(65535))),
                            spvOp1(s, SPV_OP_ConvertUToF, s->floatType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, word0, UINT(16))),
                            spvOp1(s, SPV_OP_ConvertUToF, s->floatType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, word1, UINT(65535))));
        SpvId was = ADD_V3(MUL_V3(SCALE_V3(unit, FLOAT(1.f / 65535.f)), scale), offset);
        SpvId was4 = vec4Of(s, was, FLOAT(1.f));
        SpvId w4 = vec4Of(s, vec3Of(s, DOT4(r0, was4), DOT4(r1, was4), DOT4(r2, was4)), FLOAT(1.f));
        SpvId m0 = bufferVec(s, &r, r.instances, row), m1 = bufferVec(s, &r, r.instances, ADD_U(row, UINT(1))), m2 = bufferVec(s, &r, r.instances, ADD_U(row, UINT(2)));
        SpvId before = vec3Of(s, DOT4(m0, w4), DOT4(m1, w4), DOT4(m2, w4));
        SpvId pastClip = ADD_V4(ADD_V4(SCALE_V4(frameVec(s, &r, F_PREVIOUS), X(before)), SCALE_V4(frameVec(s, &r, F_PREVIOUS + 1), Y(before))),
                                ADD_V4(SCALE_V4(frameVec(s, &r, F_PREVIOUS + 2), Z(before)), frameVec(s, &r, F_PREVIOUS + 3)));
        spvStore(s, vPast, vec4Of(s, vec3Of(s, X(pastClip), Y(pastClip), W(pastClip)), FLOAT(1.f)));
        spvStore(s, vInstance, spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, id, UINT(ARTF_MOTION_VECTOR)));
        spvIfEnd(s, bentMerge);
        spvIfEnd(s, movingMerge);
    }
    spvEndMain(s);

    const SpvId interface[15] = {aPosition, aNormal, aUv, instanceIndex, vertexIndex, outVertex, vWorld, vNormal, vUv, vMaterial, vColor, vOpacity, vFade, vInstance, vPast};
    spvEntryPoint(s, SPV_MODEL_Vertex, main, interface, motion ? 15 : 14);
    return finish(s, size);
}

/* ---- the scene: pixels ---- */

typedef struct Accumulators { SpvId diffuse, highlight; } Accumulators;

/* ---- materials of the program's own ---- */

/* The variables a graph writes: what the material said goes in, and what it makes of that comes out. */
typedef struct GraphVars { SpvId color, emissive, normal, rough, metal, alpha; } GraphVars;

typedef struct GraphInputs { SpvId uv, world, normal, toEye, base, row; bool fragment; } GraphInputs; /* row: where the material's rows start */

static SpvId same4(Spv* s, SpvId x) { const SpvId p[4] = {x, x, x, x}; return spvCompose(s, s->vec4, p, 4); }

/* As sineOf in graph.c, one number. */
static SpvId graphSine(Spv* s, SpvId x) {
    const SpvId zero = spvConstFloat(s, 0.f), half = spvConstFloat(s, .5f);
    SpvId t = spvOp2(s, SPV_OP_FMul, s->floatType, x, spvConstFloat(s, 1.f / 6.28318530718f));
    t = spvOp2(s, SPV_OP_FSub, s->floatType, t, spvGlsl1(s, SPV_GLSL_Floor, s->floatType, spvOp2(s, SPV_OP_FAdd, s->floatType, t, half)));
    SpvId a = spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, t);
    SpvId folded = spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, a, spvConstFloat(s, .25f)), spvOp2(s, SPV_OP_FSub, s->floatType, half, a), a);
    SpvId u = spvOp2(s, SPV_OP_FMul, s->floatType, folded, spvConstFloat(s, 6.28318530718f)), u2 = spvOp2(s, SPV_OP_FMul, s->floatType, u, u);
    SpvId p = spvOp2(s, SPV_OP_FAdd, s->floatType, spvConstFloat(s, 1.f / 120.f), spvOp2(s, SPV_OP_FMul, s->floatType, u2, spvConstFloat(s, -1.f / 5040.f)));
    p = spvOp2(s, SPV_OP_FAdd, s->floatType, spvConstFloat(s, -1.f / 6.f), spvOp2(s, SPV_OP_FMul, s->floatType, u2, p));
    p = spvOp2(s, SPV_OP_FAdd, s->floatType, spvConstFloat(s, 1.f), spvOp2(s, SPV_OP_FMul, s->floatType, u2, p));
    p = spvOp2(s, SPV_OP_FMul, s->floatType, u, p);
    return spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, t, zero), spvOp1(s, SPV_OP_FNegate, s->floatType, p), p);
}

/* What a graph makes of a pixel, written out: the one definition, as SPIR-V. */
static void emitGraph(Spv* s, const Resources* r, const ArtfGraphData* g, const GraphInputs* in, const GraphVars* out, SpvId textures, SpvId sampled) {
    SpvId regs[ARTF_GRAPH_NODES + 1];
    regs[0] = same4(s, spvConstFloat(s, 0.f));
    for (uint32_t i = 0; i < g->count; ++i) {
        const ArtfGraphNode* n = &g->nodes[i];
        SpvId a = regs[n->a], b = n->op == ARTF_OP_TEXTURE ? 0 : regs[n->b], c = regs[n->c], v = 0;
        const SpvId one = spvConstFloat(s, 1.f), none = spvConstFloat(s, 0.f);
        switch (n->op) {
        case ARTF_OP_CONST: { const SpvId p[4] = {spvConstFloat(s, n->v[0]), spvConstFloat(s, n->v[1]), spvConstFloat(s, n->v[2]), spvConstFloat(s, n->v[3])}; v = spvCompose(s, s->vec4, p, 4); break; }
        case ARTF_OP_UV: { const SpvId p[4] = {spvExtract(s, s->floatType, in->uv, 0), spvExtract(s, s->floatType, in->uv, 1), none, none}; v = spvCompose(s, s->vec4, p, 4); break; }
        case ARTF_OP_POSITION: v = vec4Of(s, in->world, one); break;
        case ARTF_OP_NORMAL: v = vec4Of(s, in->normal, none); break;
        case ARTF_OP_TO_EYE: v = vec4Of(s, in->toEye, none); break;
        case ARTF_OP_BASE: v = vec4Of(s, in->base, one); break;
        case ARTF_OP_PARAM: v = bufferVec(s, r, r->materials, spvOp2(s, SPV_OP_IAdd, s->uintType, in->row, spvConstUint(s, 5 + n->a))); break;
        case ARTF_OP_TEXTURE: {
            SpvId index = spvConstUint(s, n->b);
            SpvId slot = spvAccess(s, spvTypePointer(s, SPV_STORAGE_UniformConstant, sampled), textures, &index, 1);
            const SpvId uvParts[2] = {spvExtract(s, s->floatType, a, 0), spvExtract(s, s->floatType, a, 1)};
            /* (a material is the same across a triangle, so neighbouring pixels are here together for the mipmap) */
            SpvId coords = spvCompose(s, s->vec2, uvParts, 2);
            SpvId t = in->fragment ? spvImageSample(s, spvLoad(s, sampled, slot), coords) : spvImageSampleLod(s, spvLoad(s, sampled, slot), coords, none);
            SpvId rgb = spvShuffle3(s, t);
            v = vec4Of(s, srgbDecode3(s, rgb), spvExtract(s, s->floatType, t, 3));
            break;
        }
        case ARTF_OP_ADD: v = spvOp2(s, SPV_OP_FAdd, s->vec4, a, b); break;
        case ARTF_OP_SUB: v = spvOp2(s, SPV_OP_FSub, s->vec4, a, b); break;
        case ARTF_OP_MUL: v = spvOp2(s, SPV_OP_FMul, s->vec4, a, b); break;
        case ARTF_OP_DIV: v = spvOp2(s, SPV_OP_FDiv, s->vec4, a, b); break;
        case ARTF_OP_MIN: v = spvGlsl2(s, SPV_GLSL_FMin, s->vec4, a, b); break;
        case ARTF_OP_MAX: v = spvGlsl2(s, SPV_GLSL_FMax, s->vec4, a, b); break;
        case ARTF_OP_DOT: v = same4(s, spvOp2(s, SPV_OP_Dot, s->floatType, spvShuffle3(s, a), spvShuffle3(s, b))); break;
        case ARTF_OP_FLOOR: v = spvGlsl1(s, SPV_GLSL_Floor, s->vec4, a); break;
        case ARTF_OP_FRACT: v = spvOp2(s, SPV_OP_FSub, s->vec4, a, spvGlsl1(s, SPV_GLSL_Floor, s->vec4, a)); break;
        case ARTF_OP_ABS: v = spvGlsl1(s, SPV_GLSL_FAbs, s->vec4, a); break;
        case ARTF_OP_SQRT: v = spvGlsl1(s, SPV_GLSL_Sqrt, s->vec4, spvGlsl2(s, SPV_GLSL_FMax, s->vec4, a, same4(s, none))); break;
        case ARTF_OP_SIN: { SpvId q[4]; for (uint32_t k = 0; k < 4; ++k) q[k] = graphSine(s, spvExtract(s, s->floatType, a, k)); v = spvCompose(s, s->vec4, q, 4); break; }
        case ARTF_OP_COS: { SpvId q[4]; for (uint32_t k = 0; k < 4; ++k) q[k] = graphSine(s, spvOp2(s, SPV_OP_FAdd, s->floatType, spvExtract(s, s->floatType, a, k), spvConstFloat(s, 1.57079632679f))); v = spvCompose(s, s->vec4, q, 4); break; }
        case ARTF_OP_MIX: v = spvOp2(s, SPV_OP_FAdd, s->vec4, a, spvOp2(s, SPV_OP_FMul, s->vec4, spvOp2(s, SPV_OP_FSub, s->vec4, b, a), c)); break;
        case ARTF_OP_IF: {
            SpvId q[4];
            for (uint32_t k = 0; k < 4; ++k) q[k] = spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, spvExtract(s, s->floatType, a, k), none), spvExtract(s, s->floatType, b, k), spvExtract(s, s->floatType, c, k));
            v = spvCompose(s, s->vec4, q, 4);
            break;
        }
        case ARTF_OP_X: v = same4(s, spvExtract(s, s->floatType, a, 0)); break;
        case ARTF_OP_Y: v = same4(s, spvExtract(s, s->floatType, a, 1)); break;
        case ARTF_OP_Z: v = same4(s, spvExtract(s, s->floatType, a, 2)); break;
        case ARTF_OP_W: v = same4(s, spvExtract(s, s->floatType, a, 3)); break;
        case ARTF_OP_VEC: { const SpvId p[4] = {spvExtract(s, s->floatType, a, 0), spvExtract(s, s->floatType, b, 0), spvExtract(s, s->floatType, c, 0), one}; v = spvCompose(s, s->vec4, p, 4); break; }
        default: v = regs[0]; break;
        }
        regs[i + 1] = v;
    }
    const uint32_t* o = g->outputs;
    if (o[ARTF_OUT_COLOR]) spvStore(s, out->color, spvShuffle3(s, regs[o[ARTF_OUT_COLOR]]));
    if (o[ARTF_OUT_EMISSIVE]) spvStore(s, out->emissive, spvShuffle3(s, regs[o[ARTF_OUT_EMISSIVE]]));
    if (o[ARTF_OUT_ROUGHNESS]) spvStore(s, out->rough, spvGlsl2(s, SPV_GLSL_FMin, s->floatType, spvGlsl2(s, SPV_GLSL_FMax, s->floatType, spvExtract(s, s->floatType, regs[o[ARTF_OUT_ROUGHNESS]], 0), spvConstFloat(s, .045f)), spvConstFloat(s, 1.f)));
    if (o[ARTF_OUT_METALLIC]) spvStore(s, out->metal, spvGlsl2(s, SPV_GLSL_FMin, s->floatType, spvGlsl2(s, SPV_GLSL_FMax, s->floatType, spvExtract(s, s->floatType, regs[o[ARTF_OUT_METALLIC]], 0), spvConstFloat(s, 0.f)), spvConstFloat(s, 1.f)));
    if (o[ARTF_OUT_ALPHA]) spvStore(s, out->alpha, spvExtract(s, s->floatType, regs[o[ARTF_OUT_ALPHA]], 0));
    if (o[ARTF_OUT_NORMAL]) spvStore(s, out->normal, spvGlsl1(s, SPV_GLSL_Normalize, s->vec3, spvShuffle3(s, regs[o[ARTF_OUT_NORMAL]])));
}

/* The graph of a material: one branch for each in the scene. */
static void emitGraphs(Spv* s, const Resources* r, const ArtfGraphData* graphs, uint32_t count, SpvId graphId, const GraphInputs* in, const GraphVars* out, SpvId textures, SpvId sampled) {
    for (uint32_t i = 0; i < count; ++i) {
        SpvId merge;
        spvIfBegin(s, spvOp2(s, 170 /* OpIEqual */, s->boolType, graphId, spvConstUint(s, i + 1)), &merge);
        emitGraph(s, r, &graphs[i], in, out, textures, sampled);
        spvIfEnd(s, merge);
    }
}

/* Rows of ArtfShadowFrame. */
enum { S_SPLITS = 0, S_SUN = 1, S_LIGHTS = 2, S_VIEWS = 18, S_VIEW_ROWS = 7, S_INFO = 4, S_PLACE = 5, S_SIZE = 6, S_EXTRA = S_VIEWS + ARTF_SHADOW_VIEWS * S_VIEW_ROWS,
       S_ROWS = S_EXTRA + ARTF_SHADOW_EXTRA };

/* turning: soft shadows take shadows.c's fewer reads, turned by turn (artfShadowTurn), where it holds; 0: never. */
typedef struct Shadows { SpvId block, atlas, imageType, sampled, ivec2; const Resources* r; SpvId turning, turn; } Shadows;

static SpvId shadowRow(Spv* s, const Shadows* h, SpvId row) {
    const SpvId idx[2] = {h->r->zero, row};
    return spvLoad(s, s->vec4, spvAccess(s, h->r->ptrUniformVec4, h->block, idx, 2));
}

/* As lookUp in shadows.c, for a place already lifted off its surface. */
static SpvId shadowLookup(Spv* s, const Shadows* h, SpvId view, SpvId place) {
    SpvId base = ADD_U(UINT(S_VIEWS), MUL_U(view, UINT(S_VIEW_ROWS)));
    SpvId p = vec4Of(s, place, FLOAT(1.f));
    SpvId info = shadowRow(s, h, ADD_U(base, UINT(S_INFO)));
    SpvId cx = DOT4(shadowRow(s, h, base), p), cy = DOT4(shadowRow(s, h, ADD_U(base, UINT(1))), p), cz = DOT4(shadowRow(s, h, ADD_U(base, UINT(2))), p), cw = DOT4(shadowRow(s, h, ADD_U(base, UINT(3))), p);
    SpvId size = X(shadowRow(s, h, ADD_U(base, UINT(S_SIZE))));
    SpvId iw = DIV_F(FLOAT(1.f), cw);
    SpvId u = MUL_F(ADD_F(MUL_F(MUL_F(cx, iw), FLOAT(.5f)), FLOAT(.5f)), size), v = MUL_F(ADD_F(MUL_F(MUL_F(cy, iw), FLOAT(.5f)), FLOAT(.5f)), size);
    SpvId depth = SUB_F(MUL_F(cz, iw), W(info));
    SpvId cu = spvGlsl1(s, SPV_GLSL_Floor, s->floatType, u), cv = spvGlsl1(s, SPV_GLSL_Floor, s->floatType, v), last = SUB_F(size, FLOAT(1.f));
    SpvId image = spvLoad(s, h->sampled, h->atlas);
    SpvId lit = FLOAT(0.f);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            SpvId tu = MIN_F(MAX_F(ADD_F(cu, FLOAT((float)dx)), FLOAT(0.f)), last), tv = MIN_F(MAX_F(ADD_F(cv, FLOAT((float)dy)), FLOAT(0.f)), last);
            const SpvId xy[2] = {spvOp1(s, SPV_OP_ConvertFToS, s->intType, ADD_F(tu, X(info))), spvOp1(s, SPV_OP_ConvertFToS, s->intType, ADD_F(tv, Y(info)))};
            SpvId there = X(spvImageFetch(s, h->imageType, image, spvCompose(s, h->ivec2, xy, 2)));
            lit = ADD_F(lit, spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, depth, there), FLOAT(1.f), FLOAT(0.f)));
        }
    SpvId zero = FLOAT(0.f);
    SpvId inU = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, u, zero), spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, u, size));
    SpvId inV = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, v, zero), spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, v, size));
    SpvId inside = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, inU, inV), spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, cw, zero));
    return spvSelect(s, s->floatType, inside, MUL_F(lit, FLOAT(1.f / 9.f)), FLOAT(1.f));
}

#define P_NOT_TURNING(b) spvOp1(s, SPV_OP_LogicalNot, s->boolType, b)
/* As lookUpSoft in shadows.c: soft by the light's size (0: shadowLookup's nine texels). */
static SpvId shadowLookupSoft(Spv* s, const Shadows* h, SpvId view, SpvId place, SpvId lightSize, bool sun) {
    SpvId base = ADD_U(UINT(S_VIEWS), MUL_U(view, UINT(S_VIEW_ROWS)));
    SpvId p = vec4Of(s, place, FLOAT(1.f));
    SpvId info = shadowRow(s, h, ADD_U(base, UINT(S_INFO)));
    SpvId r2 = shadowRow(s, h, ADD_U(base, UINT(2))), r3 = shadowRow(s, h, ADD_U(base, UINT(3)));
    SpvId cx = DOT4(shadowRow(s, h, base), p), cy = DOT4(shadowRow(s, h, ADD_U(base, UINT(1))), p), cz = DOT4(r2, p), cw = DOT4(r3, p);
    SpvId size = X(shadowRow(s, h, ADD_U(base, UINT(S_SIZE))));
    SpvId iw = DIV_F(FLOAT(1.f), cw);
    SpvId u = MUL_F(ADD_F(MUL_F(MUL_F(cx, iw), FLOAT(.5f)), FLOAT(.5f)), size), v = MUL_F(ADD_F(MUL_F(MUL_F(cy, iw), FLOAT(.5f)), FLOAT(.5f)), size);
    SpvId depth = SUB_F(MUL_F(cz, iw), W(info));
    SpvId last = SUB_F(size, FLOAT(1.f));
    SpvId image = spvLoad(s, h->sampled, h->atlas);
#define SOFT_TAP(fu, fv) X(spvImageFetch(s, h->imageType, image, spvCompose(s, h->ivec2, (const SpvId[2]){spvOp1(s, SPV_OP_ConvertFToS, s->intType, ADD_F(MIN_F(MAX_F(spvGlsl1(s, SPV_GLSL_Floor, s->floatType, fu), FLOAT(0.f)), last), X(info))), \
                                                                                                 spvOp1(s, SPV_OP_ConvertFToS, s->intType, ADD_F(MIN_F(MAX_F(spvGlsl1(s, SPV_GLSL_Floor, s->floatType, fv), FLOAT(0.f)), last), Y(info)))}, 2)))
#define SOFT_SPREAD(blocker)                                                                                                                                       \
    (sun ? DIV_F(DIV_F(MUL_F(lightSize, SUB_F(depth, blocker)), spvGlsl1(s, SPV_GLSL_Length, s->floatType, spvShuffle3(s, r2))), Z(info))                          \
         : DIV_F(MUL_F(lightSize, SUB_F(cw, DIV_F(b, MIN_F(SUB_F(blocker, a), FLOAT(-1e-12f))))), MUL_F(MUL_F(MAX_F(DIV_F(b, MIN_F(SUB_F(blocker, a), FLOAT(-1e-12f))), FLOAT(1e-6f)), Z(info)), cw)))
    SpvId row3 = spvShuffle3(s, r3);
    SpvId a = sun ? 0 : DIV_F(DOT3(spvShuffle3(s, r2), row3), DOT3(row3, row3)), b = sun ? 0 : SUB_F(W(r2), MUL_F(a, W(r3)));
    /* with temporal antialiasing: 2 x 2 reads to find what is in the way and 3 x 3 to filter, moved within their squares
     * (as lookUpSoft) */
    SpvId fewer = FLOAT(1.f), fewerCount = FLOAT(0.f);
    if (h->turning) {
        SpvId before = s->block, merge;
        spvIfBegin(s, h->turning, &merge);
        SpvId jx = SUB_F(MUL_F(ADD_F(spvOp1(s, SPV_OP_ConvertUToF, s->floatType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, h->turn, UINT(3))), FLOAT(.5f)), FLOAT(.25f)), FLOAT(.5f));
        SpvId jy = SUB_F(MUL_F(ADD_F(spvOp1(s, SPV_OP_ConvertUToF, s->floatType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, h->turn, UINT(2))), FLOAT(.5f)), FLOAT(.25f)), FLOAT(.5f));
        SpvId blockers = FLOAT(0.f), count = FLOAT(0.f);
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                SpvId there = SOFT_TAP(ADD_F(u, MUL_F(ADD_F(FLOAT((float)dx - .5f), jx), FLOAT(6.f))), ADD_F(v, MUL_F(ADD_F(FLOAT((float)dy - .5f), jy), FLOAT(6.f))));
                SpvId in = spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, there, depth);
                blockers = ADD_F(blockers, spvSelect(s, s->floatType, in, there, FLOAT(0.f)));
                count = ADD_F(count, spvSelect(s, s->floatType, in, FLOAT(1.f), FLOAT(0.f)));
            }
        SpvId blocker = DIV_F(blockers, MAX_F(count, FLOAT(1.f)));
        SpvId spread = MUL_F(MIN_F(MAX_F(SOFT_SPREAD(blocker), FLOAT(1.f)), FLOAT(12.f)), FLOAT(.5f));
        SpvId cell = MUL_F(spread, FLOAT(4.f / 3.f));
        SpvId lit = FLOAT(0.f);
        for (int dy = 0; dy < 3; ++dy)
            for (int dx = 0; dx < 3; ++dx) {
                SpvId there = SOFT_TAP(ADD_F(u, MUL_F(ADD_F(FLOAT((float)dx - 1.f), jx), cell)), ADD_F(v, MUL_F(ADD_F(FLOAT((float)dy - 1.f), jy), cell)));
                lit = ADD_F(lit, spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, depth, there), FLOAT(1.f), FLOAT(0.f)));
            }
        SpvId got = MUL_F(lit, FLOAT(1.f / 9.f));
        SpvId from = s->block;
        spvIfEnd(s, merge);
        fewer = spvPhi(s, s->floatType, got, from, FLOAT(1.f), before);
        fewerCount = spvPhi(s, s->floatType, count, from, FLOAT(0.f), before);
    }
    /* without: all 50, the same every frame */
    SpvId steady = 0, steadyCount = 0;
    {
        SpvId before = s->block, merge = 0;
        if (h->turning) spvIfBegin(s, P_NOT_TURNING(h->turning), &merge);
        SpvId blockers = FLOAT(0.f), count = FLOAT(0.f);
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                SpvId there = SOFT_TAP(ADD_F(u, FLOAT((float)dx * 3.f)), ADD_F(v, FLOAT((float)dy * 3.f)));
                SpvId in = spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, there, depth);
                blockers = ADD_F(blockers, spvSelect(s, s->floatType, in, there, FLOAT(0.f)));
                count = ADD_F(count, spvSelect(s, s->floatType, in, FLOAT(1.f), FLOAT(0.f)));
            }
        SpvId blocker = DIV_F(blockers, MAX_F(count, FLOAT(1.f)));
        SpvId spread = MUL_F(MIN_F(MAX_F(SOFT_SPREAD(blocker), FLOAT(1.f)), FLOAT(12.f)), FLOAT(.5f));
        SpvId lit = FLOAT(0.f);
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                SpvId there = SOFT_TAP(ADD_F(u, MUL_F(FLOAT((float)dx), spread)), ADD_F(v, MUL_F(FLOAT((float)dy), spread)));
                lit = ADD_F(lit, spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, depth, there), FLOAT(1.f), FLOAT(0.f)));
            }
        steady = MUL_F(lit, FLOAT(1.f / 25.f));
        steadyCount = count;
        if (h->turning) {
            SpvId from = s->block;
            spvIfEnd(s, merge);
            steady = spvPhi(s, s->floatType, steady, from, fewer, before);
            steadyCount = spvPhi(s, s->floatType, steadyCount, from, fewerCount, before);
        }
    }
#undef SOFT_SPREAD
#undef SOFT_TAP
    SpvId zero = FLOAT(0.f);
    SpvId inU = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, u, zero), spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, u, size));
    SpvId inV = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, v, zero), spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, v, size));
    SpvId inside = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, inU, inV), spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, cw, zero));
    SpvId soft = spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, steadyCount, FLOAT(0.f)), steady, FLOAT(1.f));
    SpvId got = spvSelect(s, s->floatType, inside, soft, FLOAT(1.f));
    /* (no size: the nine texels, as drawn without) */
    return spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, lightSize, FLOAT(0.f)), got, shadowLookup(s, h, view, place));
}

/* ---- what lights and materials past glTF's core need: as sceneInternal.h's, step for step ---- */

#define G_LT(a, b) spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, a, b)
#define G_GT(a, b) spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, a, b)
#define G_PICK(c, a, b) spvSelect(s, s->floatType, c, a, b)
#define G_ABS(a) spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, a)
#define G_SQRT(a) spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, a)
#define G_FLOOR(a) spvGlsl1(s, SPV_GLSL_Floor, s->floatType, a)
#define G_CLAMP(v, lo, hi) MIN_F(MAX_F(v, lo), hi)
#define G_AND(a, b) spvOp2(s, SPV_OP_LogicalAnd, s->boolType, a, b)
#define G_CROSS(a, b) spvGlsl2(s, SPV_GLSL_Cross, s->vec3, a, b)
#define G_U(f) spvOp1(s, SPV_OP_ConvertFToU, s->uintType, f)

/* artfLog2Fast and artfExp2Fast (spirv.c). */
#define log2Of(s, x) spvLog2Fast(s, x)
#define exp2Of(s, x) spvExp2Fast(s, x)

/* artfCosFast. */
static SpvId cosOf(Spv* s, SpvId x) {
    x = ADD_F(x, FLOAT(1.57079633f));
    SpvId turns = spvOp1(s, 111 /* OpConvertSToF */, s->floatType, spvOp1(s, SPV_OP_ConvertFToS, s->intType, ADD_F(MUL_F(x, FLOAT(.159154943f)), G_PICK(G_LT(x, FLOAT(0.f)), FLOAT(-.5f), FLOAT(.5f)))));
    x = SUB_F(x, MUL_F(FLOAT(6.28318531f), turns));
    x = G_PICK(G_GT(x, FLOAT(1.57079633f)), SUB_F(FLOAT(3.14159265f), x), G_PICK(G_LT(x, FLOAT(-1.57079633f)), SUB_F(FLOAT(-3.14159265f), x), x));
    SpvId x2 = MUL_F(x, x);
    SpvId p = SUB_F(FLOAT(.00019841270f), MUL_F(x2, FLOAT(.0000027557319f)));
    p = SUB_F(FLOAT(.0083333333f), MUL_F(x2, p));
    p = SUB_F(FLOAT(.16666667f), MUL_F(x2, p));
    return MUL_F(x, SUB_F(FLOAT(1.f), MUL_F(x2, p)));
}
/* artfAcosFast and artfAtan2Fast. */
static SpvId acosOf(Spv* s, SpvId x) {
    SpvId a = MIN_F(G_ABS(x), FLOAT(1.f));
    SpvId r = MUL_F(G_SQRT(SUB_F(FLOAT(1.f), a)), ADD_F(FLOAT(1.5707288f), MUL_F(a, ADD_F(FLOAT(-.2121144f), MUL_F(a, SUB_F(FLOAT(.0742610f), MUL_F(a, FLOAT(.0187293f))))))));
    return G_PICK(G_LT(x, FLOAT(0.f)), SUB_F(FLOAT(3.14159265f), r), r);
}
static SpvId atan2Of(Spv* s, SpvId y, SpvId x) {
    SpvId ax = G_ABS(x), ay = G_ABS(y), big = MAX_F(ax, ay), small = MIN_F(ax, ay);
    SpvId z = DIV_F(small, MAX_F(big, FLOAT(1e-30f))), z2 = MUL_F(z, z);
    SpvId p = SUB_F(FLOAT(.05265332f), MUL_F(z2, FLOAT(.01172120f)));
    p = ADD_F(FLOAT(-.11643287f), MUL_F(z2, p));
    p = ADD_F(FLOAT(.19354346f), MUL_F(z2, p));
    p = ADD_F(FLOAT(-.33262347f), MUL_F(z2, p));
    SpvId r = MUL_F(z, ADD_F(FLOAT(.99997726f), MUL_F(z2, p)));
    r = G_PICK(G_GT(ay, ax), SUB_F(FLOAT(1.57079633f), r), r);
    r = G_PICK(G_LT(x, FLOAT(0.f)), SUB_F(FLOAT(3.14159265f), r), r);
    return G_PICK(G_LT(y, FLOAT(0.f)), spvOp1(s, SPV_OP_FNegate, s->floatType, r), r);
}

/* The shading model (shading/shading.inc), as instructions: shader... */
#define S_FN(name) shader##name
#define S_DEF static
#define S_SELF Spv* s,
#define S_PASS s,
#define S_F SpvId
#define S_V3 SpvId
#define S_FLAG SpvId
#define S_WIDE 0
#define S_C(x) FLOAT(x)
#define S_ADD(a, b) ADD_F(a, b)
#define S_SUB(a, b) SUB_F(a, b)
#define S_MUL(a, b) MUL_F(a, b)
#define S_DIV(a, b) DIV_F(a, b)
#define S_MIN(a, b) MIN_F(a, b)
#define S_MAX(a, b) MAX_F(a, b)
#define S_SQRT(a) G_SQRT(a)
#define S_GT(a, b) spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, a, b)
#define S_SELECT(c, a, b) spvSelect(s, s->floatType, c, a, b)
#define S_PICK(c, a, b) ((c) ? spvSelect(s, s->floatType, (c), (a), (b)) : (b))
#define S_V3_OF(x, y, z) vec3Of(s, x, y, z)
#define S_X(v) X(v)
#define S_Y(v) Y(v)
#define S_Z(v) Z(v)
#define S_DOT3(a, b) DOT3(a, b)
#define S_NORMALIZE(a) NORMALIZE(a)
#define S_LOG2(a) log2Of(s, a)
#define S_EXP2(a) exp2Of(s, a)
#define S_COS(a) cosOf(s, a)
#define S_CLAMP(a, lo, hi) G_CLAMP(a, lo, hi)
#define S_ADD3(a, b) ADD_V3(a, b)
#define S_SUB3(a, b) SUB_V3(a, b)
#define S_MUL3(a, b) MUL_V3(a, b)
#define S_MAX3(a, b) spvGlsl2(s, SPV_GLSL_FMax, s->vec3, a, b)
#define S_SCALE3(a, f) SCALE_V3(a, f)
#define S_SPLAT3(f) vec3Of(s, f, f, f)
#include "shading/shading.inc"

/* Profiles are in the lights' buffer (binding 60), past those past the frame's own: one for each light it holds. */
#define G_PROFILES (ARTF_MAX_LIGHTS * 12)
static SpvId profileFloat(Spv* s, const Resources* r, SpvId buffer, SpvId i) {
    const SpvId idx[3] = {r->zero, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, i, UINT(2)), spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, i, UINT(3))};
    return spvLoad(s, s->floatType, spvAccess(s, r->ptrStorageFloat, buffer, idx, 3));
}
/* artfProfileAt, for the frame's light at slot. */
static SpvId profileAt(Spv* s, const Resources* r, SpvId buffer, SpvId slot, SpvId axis, SpvId ref, SpvId dir) {
    SpvId side = G_CROSS(axis, ref);
    SpvId x = MUL_F(acosOf(s, DOT3(dir, axis)), FLOAT((float)(ARTF_PROFILE_V - 1) / 3.14159265f));
    SpvId y = MUL_F(atan2Of(s, DOT3(dir, side), DOT3(dir, ref)), FLOAT((float)ARTF_PROFILE_H / 6.28318531f));
    y = G_PICK(G_LT(y, FLOAT(0.f)), ADD_F(y, FLOAT((float)ARTF_PROFILE_H)), y);
    SpvId fx = MIN_F(G_FLOOR(x), FLOAT((float)(ARTF_PROFILE_V - 2))), fy = G_FLOOR(y), tx = SUB_F(x, fx), ty = SUB_F(y, fy);
    SpvId j0 = spvOp2(s, SPV_OP_UMod, s->uintType, G_U(fy), UINT(ARTF_PROFILE_H)), j1 = spvOp2(s, SPV_OP_UMod, s->uintType, ADD_U(j0, UINT(1)), UINT(ARTF_PROFILE_H));
    SpvId base = ADD_U(ADD_U(UINT(G_PROFILES), MUL_U(slot, UINT(ARTF_PROFILE_FLOATS))), G_U(fx));
    SpvId at0 = ADD_U(base, MUL_U(j0, UINT(ARTF_PROFILE_V))), at1 = ADD_U(base, MUL_U(j1, UINT(ARTF_PROFILE_V)));
    SpvId a0 = profileFloat(s, r, buffer, at0), a1 = profileFloat(s, r, buffer, ADD_U(at0, UINT(1)));
    SpvId b0 = profileFloat(s, r, buffer, at1), b1 = profileFloat(s, r, buffer, ADD_U(at1, UINT(1)));
    SpvId a = ADD_F(a0, MUL_F(SUB_F(a1, a0), tx)), b = ADD_F(b0, MUL_F(SUB_F(b1, b0), tx));
    return ADD_F(a, MUL_F(SUB_F(b, a), ty));
}

/* The highlight's linearly transformed cosines, past the profiles in the lights' buffer: artfLtcAt. */
#define G_LTC (G_PROFILES + ARTF_FRAME_LIGHTS * ARTF_PROFILE_FLOATS)
static void ltcAt(Spv* s, const Resources* r, SpvId buffer, SpvId rough, SpvId ndv, SpvId out[5]) {
    SpvId fx = MUL_F(SUB_F(G_CLAMP(rough, FLOAT(ARTF_LTC_MIN_ROUGH), FLOAT(1.f)), FLOAT(ARTF_LTC_MIN_ROUGH)), FLOAT((float)(ARTF_LTC_SIZE - 1) / (1.f - ARTF_LTC_MIN_ROUGH)));
    SpvId fy = MUL_F(G_SQRT(G_CLAMP(SUB_F(FLOAT(1.f), ndv), FLOAT(0.f), FLOAT(1.f))), FLOAT((float)(ARTF_LTC_SIZE - 1)));
    SpvId x = MIN_F(G_FLOOR(fx), FLOAT((float)(ARTF_LTC_SIZE - 2))), y = MIN_F(G_FLOOR(fy), FLOAT((float)(ARTF_LTC_SIZE - 2)));
    SpvId tx = SUB_F(fx, x), ty = SUB_F(fy, y);
    SpvId at = ADD_U(UINT(G_LTC), MUL_U(ADD_U(MUL_U(G_U(x), UINT(ARTF_LTC_SIZE)), G_U(y)), UINT(5)));
    for (uint32_t k = 0; k < 5; ++k) {
        SpvId c00 = profileFloat(s, r, buffer, ADD_U(at, UINT(k))), c10 = profileFloat(s, r, buffer, ADD_U(at, UINT(ARTF_LTC_SIZE * 5 + k)));
        SpvId c01 = profileFloat(s, r, buffer, ADD_U(at, UINT(5 + k))), c11 = profileFloat(s, r, buffer, ADD_U(at, UINT(ARTF_LTC_SIZE * 5 + 5 + k)));
        SpvId a = ADD_F(c00, MUL_F(SUB_F(c10, c00), tx)), b = ADD_F(c01, MUL_F(SUB_F(c11, c01), tx));
        out[k] = ADD_F(a, MUL_F(SUB_F(b, a), ty));
    }
}

/* artfLightPlace: a light's shape (its rows A and B), ball or point, seen from p; and a shape's light from all of it
 * (shading.inc's AreaLight: lambert, glossy, from way), reach2 being 1 / its radius^2 and ltc artfLtcAt's. */
typedef struct ShapeLit { SpvId toDiffuse, diffuse, toHighlight, highlight, distance2, lambert, glossy, way; } ShapeLit;
static ShapeLit lightPlace(Spv* s, SpvId rowA, SpvId rowB, SpvId center, SpvId ball, SpvId p, SpvId n, SpvId view, SpvId rough, SpvId reach2, const SpvId* ltc) {
    SpvId u = spvShuffle3(s, rowA), hl = W(rowA), f = spvShuffle3(s, rowB), hw = W(rowB);
    SpvId zero = FLOAT(0.f), one = FLOAT(1.f);
    SpvId d = SUB_V3(center, p);
    SpvId side = G_CROSS(f, u), rect = G_GT(hw, zero), shaped = G_GT(hl, zero);
    SpvId along = G_CLAMP(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(d, u)), spvOp1(s, SPV_OP_FNegate, s->floatType, hl), hl);
    SpvId across = G_PICK(rect, G_CLAMP(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(d, side)), spvOp1(s, SPV_OP_FNegate, s->floatType, hw), hw), zero);
    SpvId before = spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(d, f));
    SpvId q = ADD_V3(ADD_V3(d, SCALE_V3(u, along)), SCALE_V3(side, across));
    SpvId q2 = MAX_F(DOT3(q, q), FLOAT(1e-8f)), qi = DIV_F(one, G_SQRT(q2));
    SpvId twice = MUL_F(FLOAT(2.f), DOT3(n, view));
    SpvId r = SUB_V3(SCALE_V3(n, twice), view);
    /* a rectangle: where the mirror ray meets its plane, kept on it */
    SpvId toward = DOT3(r, f), hits = G_LT(toward, FLOAT(-1e-4f));
    SpvId t = G_PICK(hits, DIV_F(before, spvOp1(s, SPV_OP_FNegate, s->floatType, toward)), zero);
    SpvId e = SUB_V3(SCALE_V3(r, t), d);
    SpvId a = G_PICK(hits, G_CLAMP(DOT3(e, u), spvOp1(s, SPV_OP_FNegate, s->floatType, hl), hl), along);
    SpvId b = G_PICK(hits, G_CLAMP(DOT3(e, side), spvOp1(s, SPV_OP_FNegate, s->floatType, hw), hw), across);
    SpvId onRect = ADD_V3(ADD_V3(d, SCALE_V3(u, a)), SCALE_V3(side, b));
    /* a tube: the point of its line nearest the mirror ray, then of the ball around it */
    SpvId l0 = SUB_V3(d, SCALE_V3(u, hl)), ld = SCALE_V3(u, MUL_F(FLOAT(2.f), hl));
    SpvId rl = DOT3(r, ld), r0 = DOT3(r, l0);
    SpvId tt = G_CLAMP(DIV_F(SUB_F(MUL_F(r0, rl), DOT3(l0, ld)), MAX_F(SUB_F(DOT3(ld, ld), MUL_F(rl, rl)), FLOAT(1e-8f))), zero, one);
    SpvId h = ADD_V3(l0, SCALE_V3(ld, tt));
    SpvId c = SUB_V3(SCALE_V3(r, DOT3(h, r)), h);
    SpvId onTube = ADD_V3(h, SCALE_V3(c, MIN_F(DIV_F(ball, MAX_F(spvGlsl1(s, SPV_GLSL_Length, s->floatType, c), FLOAT(1e-6f))), one)));
    h = pick3(s, rect, onRect, onTube);
    SpvId hi = DIV_F(one, G_SQRT(MAX_F(DOT3(h, h), FLOAT(1e-8f))));
    SpvId alpha = MUL_F(rough, rough), longer = MIN_F(ADD_F(alpha, MUL_F(MUL_F(hl, FLOAT(.5f)), hi)), one);
    SpvId wider = MIN_F(ADD_F(alpha, MUL_F(MUL_F(G_PICK(rect, hw, ball), FLOAT(.5f)), hi)), one);
    SpvId kept = DIV_F(alpha, wider);
    SpvId energy = MUL_F(DIV_F(alpha, longer), G_PICK(rect, kept, MUL_F(kept, kept)));
    energy = G_PICK(G_AND(rect, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, before, zero)), zero, energy);
    /* no shape: a ball's or a point's */
    SpvId p2 = MAX_F(DOT3(d, d), FLOAT(1e-8f)), pi = DIV_F(one, G_SQRT(p2));
    SpvId toPoint = SCALE_V3(d, pi);
    SpvId cb = SUB_V3(SCALE_V3(r, DOT3(d, r)), d);
    SpvId hb = ADD_V3(d, SCALE_V3(cb, MIN_F(DIV_F(ball, MAX_F(spvGlsl1(s, SPV_GLSL_Length, s->floatType, cb), FLOAT(1e-6f))), one)));
    SpvId sized = G_GT(ball, zero);
    SpvId toBall = pick3(s, sized, SCALE_V3(hb, DIV_F(one, G_SQRT(MAX_F(DOT3(hb, hb), FLOAT(1e-12f))))), toPoint);
    SpvId keptBall = DIV_F(alpha, MIN_F(ADD_F(alpha, MUL_F(MUL_F(ball, FLOAT(.5f)), pi)), one));
    ShapeLit out = {pick3(s, shaped, SCALE_V3(q, qi), toPoint), G_PICK(shaped, G_PICK(rect, MUL_F(MAX_F(before, zero), qi), one), one),
                    pick3(s, shaped, SCALE_V3(h, hi), toBall), G_PICK(shaped, energy, G_PICK(sized, MUL_F(keptBall, keptBall), one)), G_PICK(shaped, q2, p2), 0, 0, 0};
    shaderAreaLight(s, d, u, hl, f, hw, ball, reach2, n, view, ltc[0], ltc[1], ltc[2], ltc[3], ltc[4], &out.lambert, &out.glossy, &out.way);
    return out;
}

/* As depthThrough in shadows.c: from a view's map, how far a place is past what the light meets first; less than 0
 * where the map does not say. */
static SpvId depthThroughOf(Spv* s, const Shadows* h, SpvId view, SpvId place, bool sun) {
    SpvId base = ADD_U(UINT(S_VIEWS), MUL_U(view, UINT(S_VIEW_ROWS)));
    SpvId p = vec4Of(s, place, FLOAT(1.f));
    SpvId info = shadowRow(s, h, ADD_U(base, UINT(S_INFO)));
    SpvId r2 = shadowRow(s, h, ADD_U(base, UINT(2))), r3 = shadowRow(s, h, ADD_U(base, UINT(3)));
    SpvId cx = DOT4(shadowRow(s, h, base), p), cy = DOT4(shadowRow(s, h, ADD_U(base, UINT(1))), p), cz = DOT4(r2, p), w = DOT4(r3, p);
    SpvId size = X(shadowRow(s, h, ADD_U(base, UINT(S_SIZE))));
    SpvId iw = DIV_F(FLOAT(1.f), w);
    SpvId u = MUL_F(ADD_F(MUL_F(MUL_F(cx, iw), FLOAT(.5f)), FLOAT(.5f)), size), v = MUL_F(ADD_F(MUL_F(MUL_F(cy, iw), FLOAT(.5f)), FLOAT(.5f)), size);
    SpvId depth = MUL_F(cz, iw), last = SUB_F(size, FLOAT(1.f));
    const SpvId xy[2] = {spvOp1(s, SPV_OP_ConvertFToS, s->intType, ADD_F(G_FLOOR(G_CLAMP(u, FLOAT(0.f), last)), X(info))),
                         spvOp1(s, SPV_OP_ConvertFToS, s->intType, ADD_F(G_FLOOR(G_CLAMP(v, FLOAT(0.f), last)), Y(info)))};
    SpvId there = X(spvImageFetch(s, h->imageType, spvLoad(s, h->sampled, h->atlas), spvCompose(s, h->ivec2, xy, 2)));
    SpvId got;
    if (sun) {
        got = DIV_F(MAX_F(SUB_F(depth, there), FLOAT(0.f)), spvGlsl1(s, SPV_GLSL_Length, s->floatType, spvShuffle3(s, r2)));
    } else {
        SpvId row3 = spvShuffle3(s, r3);
        SpvId a = DIV_F(DOT3(spvShuffle3(s, r2), row3), DOT3(row3, row3)), b = SUB_F(W(r2), MUL_F(a, W(r3)));
        got = MAX_F(SUB_F(w, DIV_F(b, MIN_F(SUB_F(there, a), FLOAT(-1e-12f)))), FLOAT(0.f));
    }
    SpvId inU = G_AND(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, u, FLOAT(0.f)), G_LT(u, size));
    SpvId inV = G_AND(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, v, FLOAT(0.f)), G_LT(v, size));
    return G_PICK(G_AND(G_AND(inU, inV), G_GT(w, FLOAT(0.f))), got, FLOAT(-1.f));
}

/* What a surface reflects, as Reflecting in kernels.inc: its lobe (shading.inc's: drawn out where its flag says), how
 * much (specular), and what head on; and what the lobe is multiplied by, for the light it loses (shading.inc's Gain3;
 * 0: nothing, as a coat's). */
typedef struct Reflecting { shaderLobe lobe; SpvId strength, f0, gain; } Reflecting;

/* The same as addLight in kernels.inc, but for the light's color: shading.inc's highlight, times Schlick's Fresnel. */
static SpvId lightHighlight(Spv* s, const Reflecting* f, SpvId n, SpvId view, SpvId toLight, SpvId ndl, SpvId strength) {
    SpvId vdh;
    SpvId lit = shaderHighlight(s, &f->lobe, f->lobe.drawn, n, view, shaderHalfway(s, view, toLight), ndl, MUL_F(strength, f->strength), &vdh);
    SpvId fresnel = shaderSchlick3(s, f->f0, shaderFifth(s, vdh));
    if (f->gain) fresnel = MUL_V3(fresnel, f->gain);
    return SCALE_V3(fresnel, lit);
}

/* A shape's highlight (shading.inc's AreaLight's glossy, its strength): Schlick's Fresnel from where it comes on the
 * whole, as addShaped. */
static SpvId areaHighlight(Spv* s, const Reflecting* f, SpvId view, SpvId way, SpvId strength) {
    SpvId vdh = MAX_F(DOT3(view, shaderHalfway(s, view, way)), FLOAT(0.f));
    SpvId fresnel = shaderSchlick3(s, f->f0, shaderFifth(s, vdh));
    if (f->gain) fresnel = MUL_V3(fresnel, f->gain);
    return SCALE_V3(fresnel, MUL_F(strength, f->strength));
}

/* Past glTF's core (ArtfMaterialMore), as Extras in kernels.inc: where the coat, sheen and light under the surface add
 * up; the coat's lobe and normal; n . v and 1 / sheen's alpha. */
typedef struct Coating { SpvId coat, sheen, under; Reflecting lobe; SpvId coatN, ndv, sheenInverse; } Coating;

static void addLight(Spv* s, const Accumulators* a, const Reflecting* f, SpvId n, SpvId view, SpvId toLight, SpvId strength, SpvId color) {
    SpvId ndl = MAX_F(DOT3(n, toLight), FLOAT(0.f));
    SpvId amount = MUL_F(ndl, strength);
    spvStore(s, a->diffuse, ADD_V3(spvLoad(s, s->vec3, a->diffuse), SCALE_V3(color, amount)));
    spvStore(s, a->highlight, ADD_V3(spvLoad(s, s->vec3, a->highlight), MUL_V3(color, lightHighlight(s, f, n, view, toLight, ndl, strength))));
}

/* As addExtras: the coat's lobe (lightHighlight with F0 .04, by its normal), Charlie's sheen, and light under the surface
 * that wraps past where it turns from the light and comes through from behind. strength has the shadow, open not;
 * through: 2^-(thickness x reach), 0 when it is not known. */
static void addExtras(Spv* s, const Coating* c, SpvId n, SpvId view, SpvId toLight, SpvId strength, SpvId open, SpvId through, SpvId color) {
    SpvId h = shaderHalfway(s, view, toLight);
    SpvId towardLight = DOT3(n, toLight), ndl = MAX_F(towardLight, FLOAT(0.f));
    SpvId cndl = MAX_F(DOT3(c->coatN, toLight), FLOAT(0.f));
    spvStore(s, c->coat, ADD_V3(spvLoad(s, s->vec3, c->coat), MUL_V3(color, lightHighlight(s, &c->lobe, c->coatN, view, toLight, cndl, strength))));
    SpvId grazing = MUL_F(MUL_F(shaderCharlie(s, MAX_F(DOT3(n, h), FLOAT(0.f)), ndl, c->ndv, c->sheenInverse), ndl), strength);
    spvStore(s, c->sheen, ADD_V3(spvLoad(s, s->vec3, c->sheen), SCALE_V3(color, grazing)));
    SpvId lit = ADD_F(MUL_F(shaderWrap(s, towardLight, ndl), strength), MUL_F(MUL_F(MAX_F(spvOp1(s, SPV_OP_FNegate, s->floatType, towardLight), FLOAT(0.f)), through), open));
    spvStore(s, c->under, ADD_V3(spvLoad(s, s->vec3, c->under), SCALE_V3(color, lit)));
}

/* A texture of the array, by an index that differs from pixel to pixel. */
/* As textureAt, from one mipmap. */
static SpvId textureLodAt(Spv* s, SpvId textures, SpvId sampled, SpvId index, SpvId uv, SpvId lod) {
    spvDecorate(s, index, SPV_DECORATE_NonUniform, -1);
    SpvId slot = spvAccess(s, spvTypePointer(s, SPV_STORAGE_UniformConstant, sampled), textures, &index, 1);
    spvDecorate(s, slot, SPV_DECORATE_NonUniform, -1);
    SpvId image = spvLoad(s, sampled, slot);
    spvDecorate(s, image, SPV_DECORATE_NonUniform, -1);
    return spvImageSampleLod(s, image, uv, lod);
}

static SpvId textureAt(Spv* s, SpvId textures, SpvId sampled, SpvId index, SpvId uv) {
    spvDecorate(s, index, SPV_DECORATE_NonUniform, -1);
    SpvId slot = spvAccess(s, spvTypePointer(s, SPV_STORAGE_UniformConstant, sampled), textures, &index, 1);
    spvDecorate(s, slot, SPV_DECORATE_NonUniform, -1);
    SpvId image = spvLoad(s, sampled, slot);
    spvDecorate(s, image, SPV_DECORATE_NonUniform, -1);
    return spvImageSample(s, image, uv);
}

/* As packSurface in kernels.inc: a normal folded onto an octahedron (9 + 9 bits), roughness (5), metal (4) and the
 * share of light from all around (5). */
static SpvId packSurfaceSpv(Spv* s, SpvId n, SpvId rough, SpvId metal, SpvId ambient) {
#define P_F(v) spvConstFloat(s, v)
#define P_SIGN(a) spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, a, P_F(0.f)), P_F(-1.f), P_F(1.f))
#define P_ABS(a) spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, a)
#define P_Q(x, top) spvOp1(s, SPV_OP_ConvertFToU, s->uintType, ADD_F(MUL_F(MIN_F(MAX_F(x, P_F(0.f)), P_F(1.f)), P_F(top)), P_F(.5f)))
    SpvId sum = MAX_F(ADD_F(ADD_F(P_ABS(X(n)), P_ABS(Y(n))), P_ABS(Z(n))), P_F(1e-20f));
    SpvId a = DIV_F(X(n), sum), b = DIV_F(Y(n), sum);
    SpvId below = spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, Z(n), P_F(0.f));
    SpvId fa = MUL_F(SUB_F(P_F(1.f), P_ABS(b)), P_SIGN(a)), fb = MUL_F(SUB_F(P_F(1.f), P_ABS(a)), P_SIGN(b));
    a = spvSelect(s, s->floatType, below, fa, a);
    b = spvSelect(s, s->floatType, below, fb, b);
    SpvId qa = P_Q(ADD_F(MUL_F(a, P_F(.5f)), P_F(.5f)), 511.f), qb = P_Q(ADD_F(MUL_F(b, P_F(.5f)), P_F(.5f)), 511.f);
#define P_OR(x, y) spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, x, y)
#define P_UP(x, k) spvOp2(s, 196 /* OpShiftLeftLogical */, s->uintType, x, spvConstUint(s, k))
    SpvId out = P_OR(P_OR(qa, P_UP(qb, 9)), P_OR(P_OR(P_UP(P_Q(rough, 31.f), 18), P_UP(P_Q(metal, 15.f), 23)), P_UP(P_Q(ambient, 31.f), 27)));
#undef P_F
#undef P_SIGN
#undef P_ABS
#undef P_Q
#undef P_OR
#undef P_UP
    return out;
}

/* As lampShadow in shadows.c: how much of a lamp's light reaches world (d: from there to the lamp), from its word:
 * first view + 1, and views << 8. */
static SpvId lampShadowSpv(Spv* s, const Shadows* shadows, SpvId lamp, SpvId d, SpvId d2, SpvId n, SpvId toLight, SpvId world, SpvId softSize) {
    SpvId fromLamp = spvOp1(s, SPV_OP_FNegate, s->vec3, d);
    SpvId ax = spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, X(fromLamp)), ay = spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, Y(fromLamp)), az = spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, Z(fromLamp));
    SpvId geX = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, ax, ay), geXZ = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, ax, az), geYZ = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, ay, az);
    SpvId isX = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, geX, geXZ);
    SpvId positive = FLOAT(0.f);
    SpvId faceX = spvSelect(s, s->uintType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, X(fromLamp), positive), UINT(0), UINT(1));
    SpvId faceY = spvSelect(s, s->uintType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, Y(fromLamp), positive), UINT(2), UINT(3));
    SpvId faceZ = spvSelect(s, s->uintType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, Z(fromLamp), positive), UINT(4), UINT(5));
    SpvId face = spvSelect(s, s->uintType, isX, faceX, spvSelect(s, s->uintType, geYZ, faceY, faceZ));
    SpvId cube = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, lamp, UINT(8)), UINT(1));
    SpvId lampView = ADD_U(ADD_U(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, lamp, UINT(255)), UINT(0xFFFFFFFFu)), spvSelect(s, s->uintType, cube, face, UINT(0)));
    SpvId distance = spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, d2);
    SpvId texel = Z(shadowRow(s, shadows, ADD_U(ADD_U(UINT(S_VIEWS), MUL_U(lampView, UINT(S_VIEW_ROWS))), UINT(S_INFO))));
    SpvId lift = DIV_F(MUL_F(MUL_F(texel, distance), FLOAT(2.f)), MAX_F(DOT3(n, toLight), FLOAT(.15f)));
    SpvId place = ADD_V3(world, SCALE_V3(n, lift));
    return softSize ? shadowLookupSoft(s, shadows, lampView, place, softSize, false) : shadowLookup(s, shadows, lampView, place);
}

/* ---- views (ArtfView): what kernels.inc's viewLanes does, in the same order ---- */

/* As artfViewColor: a color for a number, by a hash. */
static SpvId viewColorSpv(Spv* s, SpvId k) {
    SpvId h = MUL_U(ADD_U(k, UINT(1)), UINT(0x9E3779B1u));
    h = spvOp2(s, SPV_OP_BitwiseXor, s->uintType, h, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, h, UINT(15)));
    h = MUL_U(h, UINT(0x85EBCA77u));
    h = spvOp2(s, SPV_OP_BitwiseXor, s->uintType, h, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, h, UINT(13)));
    SpvId c[3];
    for (int i = 0; i < 3; ++i) {
        SpvId byte = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, h, UINT(8u * (uint32_t)i)), UINT(255));
        c[i] = MUL_F(spvOp1(s, SPV_OP_ConvertUToF, s->floatType, ADD_U(UINT(96), spvOp2(s, SPV_OP_UMod, s->uintType, byte, UINT(160)))), FLOAT(1.f / 255.f));
    }
    return vec3Of(s, c[0], c[1], c[2]);
}

/* As artfViewHeat: a count, red to yellow to white. */
static SpvId viewHeatSpv(Spv* s, SpvId count) {
    static const uint32_t steps[3] = {32, 8, 2};
    SpvId c[3];
    for (int i = 0; i < 3; ++i) c[i] = MUL_F(spvOp1(s, SPV_OP_ConvertUToF, s->floatType, spvGlsl2(s, SPV_GLSL_UMin, s->uintType, MUL_U(count, UINT(steps[i])), UINT(255))), FLOAT(1.f / 255.f));
    return vec3Of(s, c[0], c[1], c[2]);
}

typedef struct ViewInputs {
    SpvId albedo, n, rough, metal, around, emission, world; /* what the surface is, and where */
    SpvId instance, meshWord, meshlet;                     /* what it is of: meshlet 0 where it is not known */
    SpvId lights;                                          /* how many reach it, as a float */
} ViewInputs;

/* The light a view shows, by the frame's view, in place of lit. */
static SpvId viewLightSpv(Spv* s, const Resources* r, const ViewInputs* in, SpvId lit) {
    SpvId view = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Z(frameVec(s, r, F_CULL)));
#define IS(v) spvOp2(s, 170 /* OpIEqual */, s->boolType, view, UINT(v))
#define SQUARE(d) srgbDecode3(s, d) /* (light that shows as d) */
    /* its level of detail: which of its instance's mesh's lighter versions is drawn */
    SpvId mesh = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, in->meshWord, UINT((1u << 23) - 1));
    SpvId base = AS_UINT(X(bufferVec(s, r, r->instances, ADD_U(MUL_U(in->instance, UINT(4)), UINT(3)))));
    SpvId lods = bufferVec(s, r, r->meshes, ADD_U(MUL_U(base, UINT(5)), UINT(3)));
#define SAME(a, b) spvOp2(s, 170, s->boolType, a, b)
    SpvId level = spvSelect(s, s->uintType, SAME(mesh, AS_UINT(Z(lods))), UINT(3), UINT(0));
    level = spvSelect(s, s->uintType, SAME(mesh, AS_UINT(Y(lods))), UINT(2), level);
    level = spvSelect(s, s->uintType, SAME(mesh, AS_UINT(X(lods))), UINT(1), level);
    level = spvSelect(s, s->uintType, SAME(mesh, base), UINT(0), level);
#undef SAME
    SpvId levelColor[3];
    for (uint32_t c = 0; c < 3; ++c)
        levelColor[c] = spvSelect(s, s->floatType, spvOp2(s, 166 /* OpLogicalOr */, s->boolType, spvOp2(s, 170, s->boolType, level, UINT(0)), spvOp2(s, 170, s->boolType, level, UINT(c + 1))), FLOAT(1.f), FLOAT(.25f));
    /* how far along the view, and where it was in the frame before */
    SpvId p4 = vec4Of(s, in->world, FLOAT(1.f));
    SpvId clip = ADD_V4(ADD_V4(SCALE_V4(frameVec(s, r, F_VIEWPROJ), X(p4)), SCALE_V4(frameVec(s, r, F_VIEWPROJ + 1), Y(p4))), ADD_V4(SCALE_V4(frameVec(s, r, F_VIEWPROJ + 2), Z(p4)), frameVec(s, r, F_VIEWPROJ + 3)));
    SpvId far = DIV_F(FLOAT(ARTF_VIEW_DEPTH_HALF), ADD_F(FLOAT(ARTF_VIEW_DEPTH_HALF), MAX_F(W(clip), FLOAT(0.f))));
    SpvId row = ADD_U(MUL_U(AS_UINT(Y(frameVec(s, r, F_COUNTS))), UINT(4)), MUL_U(in->instance, UINT(4)));
    SpvId k0 = bufferVec(s, r, r->instances, row), k1 = bufferVec(s, r, r->instances, ADD_U(row, UINT(1))), k2 = bufferVec(s, r, r->instances, ADD_U(row, UINT(2)));
    SpvId moved = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, Y(frameVec(s, r, F_CULL)), FLOAT(0.f));
    SpvId was = pick3(s, moved, vec3Of(s, DOT4(k0, p4), DOT4(k1, p4), DOT4(k2, p4)), in->world);
    SpvId was4 = vec4Of(s, was, FLOAT(1.f));
    SpvId before = ADD_V4(ADD_V4(SCALE_V4(frameVec(s, r, F_PREVIOUS), X(was4)), SCALE_V4(frameVec(s, r, F_PREVIOUS + 1), Y(was4))), ADD_V4(SCALE_V4(frameVec(s, r, F_PREVIOUS + 2), Z(was4)), frameVec(s, r, F_PREVIOUS + 3)));
    SpvId cw = MAX_F(W(clip), FLOAT(1e-5f)), pw = MAX_F(W(before), FLOAT(1e-5f));
    SpvId vx = MUL_F(MUL_F(SUB_F(DIV_F(X(clip), cw), DIV_F(X(before), pw)), FLOAT(.5f)), W(frameVec(s, r, F_FOG)));
    SpvId vy = MUL_F(MUL_F(SUB_F(DIV_F(Y(before), pw), DIV_F(Y(clip), cw)), FLOAT(.5f)), W(frameVec(s, r, F_EYE + 1)));
#define UNIT(v) MIN_F(MAX_F(v, FLOAT(0.f)), FLOAT(1.f))
    SpvId motion = vec3Of(s, UNIT(ADD_F(FLOAT(ARTF_VIEW_MOTION_STILL), MUL_F(vx, FLOAT(ARTF_VIEW_MOTION_SCALE)))), UNIT(ADD_F(FLOAT(ARTF_VIEW_MOTION_STILL), MUL_F(vy, FLOAT(ARTF_VIEW_MOTION_SCALE)))), FLOAT(ARTF_VIEW_MOTION_STILL));
#undef UNIT
    SpvId half = same3(s, .5f);
    SpvId normals = ADD_V3(MUL_V3(in->n, half), half);
    SpvId out = lit;
    out = pick3(s, IS(ARTF_VIEW_BASE_COLOR), in->albedo, out);
    out = pick3(s, IS(ARTF_VIEW_NORMALS), SQUARE(normals), out);
    out = pick3(s, IS(ARTF_VIEW_ROUGHNESS), SQUARE(vec3Of(s, in->rough, in->rough, in->rough)), out);
    out = pick3(s, IS(ARTF_VIEW_METALLIC), SQUARE(vec3Of(s, in->metal, in->metal, in->metal)), out);
    out = pick3(s, IS(ARTF_VIEW_AMBIENT), in->around, out);
    out = pick3(s, IS(ARTF_VIEW_EMISSIVE), in->emission, out);
    out = pick3(s, IS(ARTF_VIEW_LOD), SQUARE(vec3Of(s, levelColor[0], levelColor[1], levelColor[2])), out);
    out = pick3(s, IS(ARTF_VIEW_MESHLETS), SQUARE(viewColorSpv(s, in->meshlet)), out);
    out = pick3(s, IS(ARTF_VIEW_INSTANCES), SQUARE(viewColorSpv(s, in->instance)), out);
    out = pick3(s, IS(ARTF_VIEW_DEPTH), SQUARE(vec3Of(s, far, far, far)), out);
    out = pick3(s, IS(ARTF_VIEW_MOTION), SQUARE(motion), out);
    out = pick3(s, IS(ARTF_VIEW_LIGHTS), SQUARE(viewHeatSpv(s, spvOp1(s, SPV_OP_ConvertFToU, s->uintType, in->lights))), out);
#undef IS
#undef SQUARE
    return out;
}
/* Rays while drawing, from the pixel shader: below, with the tracer's code. */
typedef struct Tracer Tracer;
static Tracer* tracerNew(void);
static void tracerDeclare(Spv* s, Tracer* t, const Resources* r, SpvId textures, SpvId sampled, const ArtfGraphData* graphs, uint32_t graphCount, bool hardware);
static void tracerVariables(Tracer* t);
static SpvId tracedShadowSpv(const Tracer* t, SpvId world, SpvId n, bool sun, SpvId index);

/* Which of a lamp's views a place is in, from its word, as lampShadowSpv. */
static SpvId lampViewSpv(Spv* s, SpvId lamp, SpvId d) {
    SpvId fromLamp = spvOp1(s, SPV_OP_FNegate, s->vec3, d);
    SpvId ax = G_ABS(X(fromLamp)), ay = G_ABS(Y(fromLamp)), az = G_ABS(Z(fromLamp));
    SpvId geX = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, ax, ay), geXZ = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, ax, az), geYZ = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, ay, az);
    SpvId faceX = spvSelect(s, s->uintType, G_GT(X(fromLamp), FLOAT(0.f)), UINT(0), UINT(1));
    SpvId faceY = spvSelect(s, s->uintType, G_GT(Y(fromLamp), FLOAT(0.f)), UINT(2), UINT(3));
    SpvId faceZ = spvSelect(s, s->uintType, G_GT(Z(fromLamp), FLOAT(0.f)), UINT(4), UINT(5));
    SpvId face = spvSelect(s, s->uintType, G_AND(geX, geXZ), faceX, spvSelect(s, s->uintType, geYZ, faceY, faceZ));
    SpvId cube = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, lamp, UINT(8)), UINT(1));
    return ADD_U(ADD_U(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, lamp, UINT(255)), UINT(0xFFFFFFFFu)), spvSelect(s, s->uintType, cube, face, UINT(0)));
}

uint32_t* artfShaderSceneFragment(size_t* size, uint32_t maxTextures, bool cutout, const ArtfGraphData* graphs, uint32_t graphCount, int lists, uint32_t variant) {
    const bool hdr = variant & ARTF_SHADER_FLOATS, withProbes = variant & ARTF_SHADER_PROBES, walls = variant & ARTF_SHADER_WALLS, froxelsOn = hdr && variant & ARTF_SHADER_FROXELS;
    const bool coats = variant & ARTF_SHADER_COAT, softShadows = variant & ARTF_SHADER_SOFT, shapes = variant & ARTF_SHADER_SHAPES;
    const bool viewed = variant & ARTF_SHADER_VIEW, meshletIds = viewed && variant & ARTF_SHADER_MESHLETS;
    const bool traced = variant & ARTF_SHADER_TRACED, rayQuery = traced && variant & ARTF_SHADER_RAY_QUERY;
    Spv b, *s = &b;
    spvInit(s);
    spvCapability(s, 5301); /* ShaderNonUniform */
    spvCapability(s, 5307); /* SampledImageArrayNonUniformIndexing */
    spvCapability(s, 50); /* ImageQuery: a texture's size, for the tracer's cones */
    if (meshletIds) spvCapability(s, 2); /* Geometry: for PrimitiveId */
    if (rayQuery) spvCapability(s, 4472 /* RayQueryKHR: capabilities come before extensions */);
    spvExtension(s, "SPV_EXT_descriptor_indexing");
    Resources r;
    declare(s, &r, false);

    SpvId imageType, sampled = spvTypeSampledImage2D(s, &imageType);
    SpvId textures = spvResource(s, spvTypeArray(s, sampled, maxTextures, 0), SPV_STORAGE_UniformConstant, 6);
    /* shadows from rays (ArtfShadowDesc.traced): the trees, or the hardware */
    Tracer* tracer = NULL;
    if (traced) {
        tracer = tracerNew();
        tracerDeclare(s, tracer, &r, textures, sampled, graphs, graphCount, rayQuery);
    }
    Shadows shadows = {spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, S_ROWS, 16), true), SPV_STORAGE_Uniform, 24), spvResource(s, sampled, SPV_STORAGE_UniformConstant, 25), imageType, sampled, spvNewId(s), &r};
    spvEmit(&s->types, 23 /* OpTypeVector */, SPV_WORDS(shadows.ivec2, s->intType, 2));
    SpvId ptrIn3 = spvTypePointer(s, SPV_STORAGE_Input, s->vec3);
    SpvId vWorld = spvVariable(s, ptrIn3, SPV_STORAGE_Input), vNormal = spvVariable(s, ptrIn3, SPV_STORAGE_Input);
    SpvId vUv = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec2), SPV_STORAGE_Input), vMaterial = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->uintType), SPV_STORAGE_Input);
    spvDecorate(s, vWorld, SPV_DECORATE_Location, 0);
    spvDecorate(s, vNormal, SPV_DECORATE_Location, 1);
    spvDecorate(s, vUv, SPV_DECORATE_Location, 2);
    spvDecorate(s, vMaterial, SPV_DECORATE_Location, 3);
    spvDecorate(s, vMaterial, SPV_DECORATE_Flat, -1);
    SpvId vColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
    spvDecorate(s, vColor, SPV_DECORATE_Location, 4);
    SpvId vOpacity = 0, frontFacing = 0, vFade = 0, fragCoord = 0, vInstance = 0, listCounts = 0, listFragments = 0, sampleMask = 0;
    if (lists > 1) {
        /* (which of a pixel's samples this covers) */
        sampleMask = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, spvTypeArray(s, s->intType, 1, 0)), SPV_STORAGE_Input);
        spvDecorate(s, sampleMask, SPV_DECORATE_BuiltIn, 20 /* SampleMask */);
    }
    if (lists || hdr || viewed) {
        vInstance = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->uintType), SPV_STORAGE_Input);
        spvDecorate(s, vInstance, SPV_DECORATE_Location, 7);
        spvDecorate(s, vInstance, SPV_DECORATE_Flat, -1);
    }
    if (lists) {
        listCounts = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 44);
        listFragments = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 45);
    }
    if (cutout || viewed) {
        /* (the mesh drawn, and above it how far it is in a fade) */
        vFade = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->uintType), SPV_STORAGE_Input);
        spvDecorate(s, vFade, SPV_DECORATE_Location, 6);
        spvDecorate(s, vFade, SPV_DECORATE_Flat, -1);
    }
    SpvId primitive = 0, meshletTable = 0;
    if (meshletIds) {
        primitive = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->intType), SPV_STORAGE_Input);
        spvDecorate(s, primitive, SPV_DECORATE_BuiltIn, 7 /* PrimitiveId */);
        spvDecorate(s, primitive, SPV_DECORATE_Flat, -1);
        meshletTable = spvResource(s, blockOf(s, r.uintArray, false), SPV_STORAGE_StorageBuffer, 65);
    }
    if (cutout) {
        fragCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
        spvDecorate(s, fragCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
        vOpacity = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->floatType), SPV_STORAGE_Input);
        spvDecorate(s, vOpacity, SPV_DECORATE_Location, 5);
        spvDecorate(s, vOpacity, SPV_DECORATE_Flat, -1);
        frontFacing = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->boolType), SPV_STORAGE_Input);
        spvDecorate(s, frontFacing, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FrontFacing);
    }
    /* lights past the frame's own: their list for each cluster of the frame (artfSceneLightTiles) */
    SpvId extraLights = spvResource(s, blockOf(s, r.vec4Array, false), SPV_STORAGE_StorageBuffer, 60);
    SpvId probes = spvResource(s, blockOf(s, r.vec4Array, false), SPV_STORAGE_StorageBuffer, 62);
    SpvId tileLights = spvResource(s, blockOf(s, r.uintArray, false), SPV_STORAGE_StorageBuffer, 61);
    SpvId decals = spvResource(s, blockOf(s, r.vec4Array, false), SPV_STORAGE_StorageBuffer, 63);
    SpvId pixelCoord = fragCoord;
    if (!pixelCoord) {
        pixelCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
        spvDecorate(s, pixelCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
    }
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);
    SpvId outSurface = 0, froxels = 0;
    if (hdr) {
        froxels = spvResource(s, blockOf(s, r.vec4Array, false), SPV_STORAGE_StorageBuffer, 56);
        /* (a frame in floats: the surface beside the light, as packSurface on the CPU) */
        /* (and the instance that shows + 1, for what follows motion) */
        outSurface = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, spvTypeVector(s, s->uintType, 2)), SPV_STORAGE_Output);
        spvDecorate(s, outSurface, SPV_DECORATE_Location, 1);
    }
    /* (what a pose bent: where it was, through the view before, from the vertex shader; w above 0 when it was bent) */
    SpvId vPast = 0;
    if (hdr && !lists) {
        vPast = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
        spvDecorate(s, vPast, SPV_DECORATE_Location, 8);
    }

    SpvId main = spvBeginMain(s);
    if (tracer) tracerVariables(tracer);
    SpvId ptrFn3 = spvTypePointer(s, SPV_STORAGE_Function, s->vec3), ptrFnUint = spvTypePointer(s, SPV_STORAGE_Function, s->uintType);
    Accumulators acc = {spvVariable(s, ptrFn3, SPV_STORAGE_Function), spvVariable(s, ptrFn3, SPV_STORAGE_Function)};
    SpvId ptrFnFloat = spvTypePointer(s, SPV_STORAGE_Function, s->floatType);
    SpvId shadowed = spvVariable(s, ptrFnFloat, SPV_STORAGE_Function);
    SpvId lightsSeen = viewed ? spvVariable(s, ptrFnFloat, SPV_STORAGE_Function) : 0; /* (in a view: how many lights reach it) */
    const GraphVars own = {spvVariable(s, ptrFn3, SPV_STORAGE_Function), spvVariable(s, ptrFn3, SPV_STORAGE_Function), spvVariable(s, ptrFn3, SPV_STORAGE_Function),
                           spvVariable(s, ptrFnFloat, SPV_STORAGE_Function), spvVariable(s, ptrFnFloat, SPV_STORAGE_Function), spvVariable(s, ptrFnFloat, SPV_STORAGE_Function)};
    SpvId counter = spvVariable(s, ptrFnUint, SPV_STORAGE_Function);
    SpvId decalAlbedo = spvVariable(s, ptrFn3, SPV_STORAGE_Function), decalRough = spvVariable(s, ptrFnFloat, SPV_STORAGE_Function);
    SpvId probeSum = spvVariable(s, ptrFn3, SPV_STORAGE_Function), probeWeights = spvVariable(s, ptrFnFloat, SPV_STORAGE_Function);
    SpvId probeMirror = spvVariable(s, ptrFn3, SPV_STORAGE_Function);
    Coating coating;
    memset(&coating, 0, sizeof coating);
    SpvId through = 0;
    if (coats) {
        coating.coat = spvVariable(s, ptrFn3, SPV_STORAGE_Function);
        coating.sheen = spvVariable(s, ptrFn3, SPV_STORAGE_Function);
        coating.under = spvVariable(s, ptrFn3, SPV_STORAGE_Function);
        through = spvVariable(s, ptrFnFloat, SPV_STORAGE_Function);
    }
    const Coating* layers = coats ? &coating : NULL;

    SpvId world = spvLoad(s, s->vec3, vWorld);
    SpvId n = NORMALIZE(spvLoad(s, s->vec3, vNormal));
    if (viewed) spvStore(s, lightsSeen, FLOAT(0.f));
    if (softShadows && hdr) {
        /* with temporal antialiasing, soft shadows turn by the pixel and the frame: as artfShadowTurn */
        SpvId coord = spvLoad(s, s->vec4, pixelCoord);
        SpvId px = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, X(coord)), py = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Y(coord));
        SpvId a0 = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, px, UINT(1)), b0 = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, py, UINT(1));
        SpvId a1 = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, px, UINT(1)), UINT(1));
        SpvId b1 = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, py, UINT(1)), UINT(1));
#define BAYER2(a, b) spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, spvOp2(s, SPV_OP_ShiftLeftLogical, s->uintType, spvOp2(s, SPV_OP_BitwiseXor, s->uintType, a, b), UINT(1)), b)
        SpvId pattern = ADD_U(MUL_U(BAYER2(a0, b0), UINT(4)), BAYER2(a1, b1));
#undef BAYER2
        SpvId number = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, W(frameVec(s, &r, F_OUTPUT)));
        shadows.turn = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, ADD_U(pattern, MUL_U(number, UINT(7))), UINT(15));
        shadows.turning = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, W(frameVec(s, &r, F_POST + 1)), FLOAT(0.f));
    }
    SpvId front = 0;
    if (cutout) {
        /* (a back is lit as a front) */
        front = spvLoad(s, s->boolType, frontFacing);
        n = SCALE_V3(n, spvSelect(s, s->floatType, front, FLOAT(1.f), FLOAT(-1.f)));
    }
    SpvId material = MUL_U(spvLoad(s, s->uintType, vMaterial), UINT(ARTF_MATERIAL_ROWS));
    SpvId m0 = bufferVec(s, &r, r.materials, material), m1 = bufferVec(s, &r, r.materials, ADD_U(material, UINT(1))), m2 = bufferVec(s, &r, r.materials, ADD_U(material, UINT(2)));
    SpvId m3 = bufferVec(s, &r, r.materials, ADD_U(material, UINT(3))), m4 = bufferVec(s, &r, r.materials, ADD_U(material, UINT(4)));

    /* what it is made of, here. Every material has every map (white when none was given), so there is no
     * branch; a normal map needs one choice, as white is not flat */
    SpvId uv = spvLoad(s, s->vec2, vUv);
    SpvId sample = textureAt(s, textures, sampled, AS_UINT(X(m2)), uv);
    SpvId texel = spvShuffle3(s, sample);
    SpvId bumps = spvShuffle3(s, textureAt(s, textures, sampled, AS_UINT(X(m4)), uv));
    SpvId surface = textureAt(s, textures, sampled, AS_UINT(Y(m4)), uv);
    SpvId occlusionTexel = textureAt(s, textures, sampled, AS_UINT(Z(m4)), uv);
    SpvId rough = MAX_F(MUL_F(W(m1), Y(surface)), FLOAT(.045f)), metal = MUL_F(Y(m3), Z(surface));
    SpvId open = ADD_F(FLOAT(1.f), MUL_F(SUB_F(X(occlusionTexel), FLOAT(1.f)), W(m3)));

    /* where u and v go on it, from how they change between pixels: so vertices need not hold it */
    SpvId dpx = spvOp1(s, 207 /* OpDPdx */, s->vec3, world), dpy = spvOp1(s, 208 /* OpDPdy */, s->vec3, world);
    SpvId dux = spvOp1(s, 207, s->vec2, uv), duy = spvOp1(s, 208, s->vec2, uv);
    /* how much the normal turns over the pixel, as turnAt on the CPU (shading.inc's Widened) */
    SpvId dnx = spvOp1(s, 207, s->vec3, n), dny = spvOp1(s, 208, s->vec3, n);
    SpvId turn = MUL_F(ADD_F(DOT3(dnx, dnx), DOT3(dny, dny)), FLOAT(.25f));
    SpvId tangentRaw = SUB_V3(SCALE_V3(dpx, spvExtract(s, s->floatType, duy, 1)), SCALE_V3(dpy, spvExtract(s, s->floatType, dux, 1)));
    SpvId bitangentRaw = SUB_V3(SCALE_V3(dpy, spvExtract(s, s->floatType, dux, 0)), SCALE_V3(dpx, spvExtract(s, s->floatType, duy, 0)));
    SpvId det = SUB_F(MUL_F(spvExtract(s, s->floatType, dux, 0), spvExtract(s, s->floatType, duy, 1)), MUL_F(spvExtract(s, s->floatType, duy, 0), spvExtract(s, s->floatType, dux, 1)));
    SpvId orient = spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, det, FLOAT(0.f)), FLOAT(-1.f), FLOAT(1.f));
    SpvId tangent = SCALE_V3(NORMALIZE(tangentRaw), orient), bitangent = SCALE_V3(NORMALIZE(bitangentRaw), orient);
    SpvId bx = MUL_F(SUB_F(MUL_F(X(bumps), FLOAT(2.f)), FLOAT(1.f)), Z(m3)), by = MUL_F(SUB_F(FLOAT(1.f), MUL_F(Y(bumps), FLOAT(2.f))), Z(m3)), bz = SUB_F(MUL_F(Z(bumps), FLOAT(2.f)), FLOAT(1.f));
    SpvId bent = NORMALIZE(ADD_V3(ADD_V3(SCALE_V3(tangent, bx), SCALE_V3(bitangent, by)), SCALE_V3(n, bz)));
    SpvId plainN = n;
    n = pick3(s, spvOp2(s, SPV_OP_INotEqual, s->boolType, AS_UINT(X(m4)), UINT(0)), bent, n);
    turn = ADD_F(turn, spvSelect(s, s->floatType, spvOp2(s, SPV_OP_INotEqual, s->boolType, AS_UINT(X(m4)), UINT(0)), shaderMapVariance(s, X(bumps), Y(bumps), Z(bumps)), FLOAT(0.f)));
    /* past glTF's core (ArtfMaterialMore), as on the CPU: its rows, and the coat's and sheen's textures (white when none) */
    SpvId e[ARTF_MATERIAL_MORE_ROWS], moreFlags = 0, coatAmount = 0, coatRough = 0, sheenRough = 0, sheenColor = 0, coatN = 0;
    if (coats) {
        SpvId at = ADD_U(MUL_U(spvLoad(s, s->uintType, vMaterial), UINT(ARTF_MATERIAL_MORE_ROWS)), MUL_U(AS_UINT(X(frameVec(s, &r, F_MATERIALS))), UINT(ARTF_MATERIAL_ROWS)));
        for (uint32_t k = 0; k < ARTF_MATERIAL_MORE_ROWS; ++k) e[k] = bufferVec(s, &r, r.materials, ADD_U(at, UINT(k)));
        moreFlags = AS_UINT(W(e[0]));
        coatAmount = MUL_F(X(e[0]), X(textureAt(s, textures, sampled, AS_UINT(X(e[2])), uv)));
        coatRough = MAX_F(MUL_F(Y(e[0]), Y(textureAt(s, textures, sampled, AS_UINT(Y(e[2])), uv))), FLOAT(.045f));
        SpvId sheenTexel = spvShuffle3(s, textureAt(s, textures, sampled, AS_UINT(W(e[2])), uv));
        sheenColor = MUL_V3(spvShuffle3(s, e[1]), srgbDecode3(s, sheenTexel));
        sheenRough = MAX_F(MUL_F(Z(e[0]), W(textureAt(s, textures, sampled, AS_UINT(X(e[3])), uv))), FLOAT(.07f));
        /* (the coat's normal map bends the surface as it is, not as its own normal map bends it) */
        SpvId coatBumps = spvShuffle3(s, textureAt(s, textures, sampled, AS_UINT(Z(e[2])), uv));
        SpvId cx = MUL_F(SUB_F(MUL_F(X(coatBumps), FLOAT(2.f)), FLOAT(1.f)), Y(e[3])), cy = MUL_F(SUB_F(FLOAT(1.f), MUL_F(Y(coatBumps), FLOAT(2.f))), Y(e[3])), cz = SUB_F(MUL_F(Z(coatBumps), FLOAT(2.f)), FLOAT(1.f));
        coatN = NORMALIZE(ADD_V3(ADD_V3(SCALE_V3(tangent, cx), SCALE_V3(bitangent, cy)), SCALE_V3(plainN, cz)));
    }

    SpvId tint = spvLoad(s, s->vec4, vColor);
    SpvId shown = W(tint);
    if (cutout) {
        /* holes, and backs that are not to show: after the textures, whose mipmaps need their neighbours */
        SpvId flags = AS_UINT(W(m2));
        SpvId isMask = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, flags, UINT(ARTF_MATERIAL_MASK)), UINT(0));
        SpvId isBlend = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, flags, UINT(ARTF_MATERIAL_BLEND)), UINT(0));
        SpvId isDouble = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, flags, UINT(ARTF_MATERIAL_DOUBLE)), UINT(0));
        SpvId coverage = MUL_F(W(sample), W(tint));
        SpvId hole = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, isMask, spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, coverage, Z(m2)));
        /* (what blends leaves out what is nearly not there: its depth would hide what is behind, as the CPU's) */
        hole = spvOp2(s, 166 /* OpLogicalOr */, s->boolType, hole, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, isBlend, spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, coverage, FLOAT(1.f / 255.f))));
        SpvId hiddenBack = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp1(s, SPV_OP_LogicalNot, s->boolType, front), spvOp1(s, SPV_OP_LogicalNot, s->boolType, isDouble));
        /* between two levels: as maskHoles */
        SpvId fade = spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, spvLoad(s, s->uintType, vFade), UINT(23)), coord = spvLoad(s, s->vec4, fragCoord);
#define BITS_AND(a, b) spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, a, spvConstUint(s, b))
#define BITS_UP(a, n) spvOp2(s, 196, s->uintType, a, spvConstUint(s, n))
#define BITS_DOWN(a, n) spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, a, spvConstUint(s, n))
#define BITS_OR(a, b) spvOp2(s, 197, s->uintType, a, b)
        SpvId px = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, X(coord)), py = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Y(coord));
        SpvId mixed = spvOp2(s, 198 /* OpBitwiseXor */, s->uintType, px, py);
        SpvId cell = BITS_OR(BITS_OR(BITS_OR(BITS_UP(BITS_AND(mixed, 1), 5), BITS_UP(BITS_AND(py, 1), 4)), BITS_OR(BITS_UP(BITS_AND(mixed, 2), 2), BITS_UP(BITS_AND(py, 2), 1))), BITS_OR(BITS_DOWN(BITS_AND(mixed, 4), 1), BITS_DOWN(BITS_AND(py, 4), 2)));
        cell = ADD_U(MUL_U(cell, UINT(4)), UINT(2));
        SpvId below = spvOp2(s, SPV_OP_ULessThan, s->boolType, cell, BITS_DOWN(fade, 1));
        SpvId lighter = spvOp2(s, SPV_OP_INotEqual, s->boolType, BITS_AND(fade, 1), UINT(0));
        SpvId other = spvOp2(s, 165 /* OpLogicalNotEqual */, s->boolType, below, lighter);
        SpvId faded = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_INotEqual, s->boolType, fade, UINT(0)), other);
#undef BITS_AND
#undef BITS_UP
#undef BITS_DOWN
#undef BITS_OR
        spvKillIf(s, spvOp2(s, 166 /* OpLogicalOr */, s->boolType, spvOp2(s, 166, s->boolType, hole, hiddenBack), faded));
        shown = MUL_F(MUL_F(W(tint), MUL_F(spvLoad(s, s->floatType, vOpacity), Y(m2))), spvSelect(s, s->floatType, isBlend, W(sample), FLOAT(1.f)));
    }
    SpvId albedo = MUL_V3(MUL_V3(spvShuffle3(s, m0), srgbDecode3(s, texel)), spvShuffle3(s, tint));

    SpvId specular = W(m0);
    SpvId view = NORMALIZE(SUB_V3(eyeOf(s, &r, world), world));
    SpvId emission = spvShuffle3(s, m1);
    if (cutout && graphCount) {
        /* a material of the program's own: only here, so that plain ones pay nothing */
        spvStore(s, own.color, albedo);
        spvStore(s, own.emissive, emission);
        spvStore(s, own.normal, n);
        spvStore(s, own.rough, rough);
        spvStore(s, own.metal, metal);
        spvStore(s, own.alpha, FLOAT(1.f));
        const GraphInputs inputs = {uv, world, n, view, albedo, material, true};
        emitGraphs(s, &r, graphs, graphCount, AS_UINT(W(m4)), &inputs, &own, textures, sampled);
        albedo = spvLoad(s, s->vec3, own.color);
        emission = spvLoad(s, s->vec3, own.emissive);
        n = spvLoad(s, s->vec3, own.normal);
        rough = spvLoad(s, s->floatType, own.rough);
        metal = spvLoad(s, s->floatType, own.metal);
        shown = MUL_F(shown, spvLoad(s, s->floatType, own.alpha));
    }
    /* decals over it, as on the CPU: in the order they were added, not on what blends */
    {
        SpvId flagsHere = AS_UINT(W(m2));
        SpvId blends = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, flagsHere, UINT(ARTF_MATERIAL_BLEND)), UINT(0));
        SpvId decalCount = spvSelect(s, s->uintType, blends, UINT(0), spvOp1(s, SPV_OP_ConvertFToU, s->uintType, W(frameVec(s, &r, F_INDIRECT + 1))));
        spvStore(s, decalAlbedo, albedo);
        spvStore(s, decalRough, rough);
        spvStore(s, counter, UINT(0));
        SpvId dHeader = spvNewId(s), dTest = spvNewId(s), dBody = spvNewId(s), dNext = spvNewId(s), dDone = spvNewId(s);
        spvBranch(s, dHeader);
        spvLabel(s, dHeader);
        spvLoopMerge(s, dDone, dNext);
        spvBranch(s, dTest);
        spvLabel(s, dTest);
        SpvId k = spvLoad(s, s->uintType, counter);
        spvBranchIf(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, k, decalCount), dBody, dDone);
        spvLabel(s, dBody);
        {
            SpvId at = MUL_U(k, UINT(5));
            SpvId r0 = bufferVec(s, &r, decals, at), r1 = bufferVec(s, &r, decals, ADD_U(at, UINT(1))), r2 = bufferVec(s, &r, decals, ADD_U(at, UINT(2)));
            SpvId lx = ADD_F(DOT3(spvShuffle3(s, r0), world), W(r0)), ly = ADD_F(DOT3(spvShuffle3(s, r1), world), W(r1)), lz = ADD_F(DOT3(spvShuffle3(s, r2), world), W(r2));
            SpvId lessX = spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, lx), FLOAT(.5f));
            SpvId lessY = spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, ly), FLOAT(.5f));
            SpvId lessZ = spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, lz), FLOAT(.5f));
            SpvId inside;
            spvIfBegin(s, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, lessX, lessY), lessZ), &inside);
            {
                SpvId r3 = bufferVec(s, &r, decals, ADD_U(at, UINT(3))), r4 = bufferVec(s, &r, decals, ADD_U(at, UINT(4)));
                SpvId upLength = spvGlsl1(s, SPV_GLSL_Length, s->floatType, spvShuffle3(s, r1));
                SpvId up = SCALE_V3(spvShuffle3(s, r1), spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, upLength, FLOAT(0.f)), DIV_F(FLOAT(1.f), MAX_F(upLength, FLOAT(1e-30f))), FLOAT(0.f)));
                SpvId facing = MIN_F(MAX_F(MUL_F(SUB_F(DOT3(n, up), FLOAT(.2f)), FLOAT(1.f / .3f)), FLOAT(0.f)), FLOAT(1.f));
                SpvId y2 = MUL_F(MUL_F(ly, ly), FLOAT(4.f)), fade = MAX_F(SUB_F(FLOAT(1.f), MUL_F(y2, y2)), FLOAT(0.f));
                const SpvId uvParts[2] = {ADD_F(lx, FLOAT(.5f)), ADD_F(lz, FLOAT(.5f))};
                SpvId got = textureLodAt(s, textures, sampled, AS_UINT(X(r4)), spvCompose(s, s->vec2, uvParts, 2), W(r4));
                SpvId cover = MUL_F(MUL_F(W(got), W(r3)), MUL_F(facing, fade));
                SpvId was = spvLoad(s, s->vec3, decalAlbedo), texel = spvShuffle3(s, got);
                SpvId want = MUL_V3(spvShuffle3(s, r3), srgbDecode3(s, texel));
                spvStore(s, decalAlbedo, ADD_V3(was, SCALE_V3(SUB_V3(want, was), cover)));
                SpvId roughWas = spvLoad(s, s->floatType, decalRough);
                spvStore(s, decalRough, spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, Y(r4), FLOAT(0.f)), ADD_F(roughWas, MUL_F(SUB_F(Y(r4), roughWas), cover)), roughWas));
            }
            spvIfEnd(s, inside);
        }
        spvBranch(s, dNext);
        spvLabel(s, dNext);
        spvStore(s, counter, ADD_U(spvLoad(s, s->uintType, counter), UINT(1)));
        spvBranch(s, dHeader);
        spvLabel(s, dDone);
        albedo = spvLoad(s, s->vec3, decalAlbedo);
        rough = spvLoad(s, s->floatType, decalRough);
    }
    /* a highlight as wide as the normal turns over the pixel, where it is shiny */
    rough = spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, ADD_F(specular, metal), FLOAT(0.f)), shaderWidened(s, rough, turn), rough);
    SpvId a = MUL_F(rough, rough), notMetal = SUB_F(FLOAT(1.f), metal);
    SpvId ndv = MAX_F(DOT3(n, view), FLOAT(1e-4f));
    SpvId dielectric = SCALE_V3(same3(s, .04f), specular);
    SpvId f0 = shaderReflects3(s, dielectric, notMetal, albedo, metal);
    Reflecting reflecting = {shaderLobeOf(s, rough, ndv), specular, f0, 0};
#define MORE_HAS(bit) spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, moreFlags, UINT(bit)), UINT(0))
    if (coats) {
        /* a dielectric's of a color, and a thin film's colors, as on the CPU */
        f0 = shaderReflects3(s, MUL_V3(dielectric, spvShuffle3(s, e[4])), notMetal, albedo, metal);
        f0 = pick3(s, MORE_HAS(ARTF_MORE_IRIDESCENCE), shaderIridescence(s, f0, ndv, W(e[3]), W(e[4]), Z(e[5])), f0);
        reflecting.f0 = f0;
        /* highlights drawn out: the way u grows, square to the normal, turned as asked */
        SpvId t = NORMALIZE(SUB_V3(tangent, SCALE_V3(n, DOT3(n, tangent)))), bt = G_CROSS(n, t);
        shaderDraw(s, &reflecting.lobe, MORE_HAS(ARTF_MORE_ANISOTROPY), t, bt, X(e[5]), Y(e[5]), a, shaderDrawnLong(s, a, MUL_F(Z(e[3]), Z(e[3]))));
    }
    reflecting.gain = shaderGain3(s, reflecting.f0, shaderUnkept(s, ndv, rough));
    SpvId shiny = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, ADD_F(specular, metal), FLOAT(0.f));

    SpvId up = ADD_F(MUL_F(spvExtract(s, s->floatType, n, 1), FLOAT(.5f)), FLOAT(.5f));
    SpvId ground = spvShuffle3(s, frameVec(s, &r, F_GROUND)), sky = spvShuffle3(s, frameVec(s, &r, F_SKY));
    spvStore(s, acc.diffuse, SCALE_V3(shaderSkyGround3(s, ground, sky, up), open));
    /* what comes from all around, in the mirror direction: all a metal shows in a plain sky */
    SpvId twice = MUL_F(DOT3(n, view), FLOAT(2.f));
    SpvId reflectedUp = ADD_F(MUL_F(SUB_F(MUL_F(Y(n), twice), Y(view)), FLOAT(.5f)), FLOAT(.5f));
    SpvId env = shaderSkyGround3(s, ground, sky, reflectedUp);
    SpvId mirror = shaderMirror3(s, f0, SUB_F(FLOAT(1.f), rough), MUL_F(shaderFifth(s, ndv), open), open);
    spvStore(s, acc.highlight, pick3(s, shiny, MUL_V3(env, mirror), same3(s, 0.f)));
    /* or from a picture of all around */
    SpvId environmentLight = W(frameVec(s, &r, F_SKY)), aroundMerge;
    spvIfBegin(s, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, environmentLight, FLOAT(0.f)), &aroundMerge);
    {
        spvStore(s, acc.diffuse, SCALE_V3(envSample(s, &r, UINT(ARTF_ENV_DIFFUSE), n), MUL_F(environmentLight, open)));
        SpvId reflected = SUB_V3(SCALE_V3(n, twice), view);
        SpvId around = SCALE_V3(envReflected(s, &r, rough, reflected), environmentLight);
        spvStore(s, acc.highlight, pick3(s, shiny, MUL_V3(around, mirror), same3(s, 0.f)));
    }
    spvIfEnd(s, aroundMerge);

    /* or from the probes near it: light that bounced, as artfIndirectAt */
    SpvId indirectRow = frameVec(s, &r, F_INDIRECT), indirectMerge;
    spvIfBegin(s, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, W(indirectRow), FLOAT(0.f)), &indirectMerge);
    if (withProbes) {
        SpvId origin = spvShuffle3(s, indirectRow), spacing = spvShuffle3(s, frameVec(s, &r, F_INDIRECT + 1)), dims = spvShuffle3(s, frameVec(s, &r, F_INDIRECT + 2));
        SpvId q = ADD_V3(world, MUL_V3(n, SCALE_V3(spacing, FLOAT(.25f))));
        SpvId place = spvOp2(s, SPV_OP_FDiv, s->vec3, SUB_V3(q, origin), spacing);
        SpvId last = SUB_V3(dims, same3(s, 1.f));
        place = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, place, same3(s, 0.f)), last);
        SpvId base = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl1(s, SPV_GLSL_Floor, s->vec3, place), SUB_V3(dims, same3(s, 2.f)));
        SpvId w = SUB_V3(place, base);
        SpvId dx = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, X(dims)), dy = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Y(dims));
        SpvId bx = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, X(base)), by = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Y(base)), bz = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Z(base));
        SpvId probeCount = MUL_U(MUL_U(dx, dy), spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Z(dims)));
        /* (the eight probes in a loop, not eight copies: what is drawn without probes pays for the registers of one) */
        spvStore(s, probeSum, same3(s, 0.f));
        spvStore(s, probeMirror, same3(s, 0.f));
        spvStore(s, probeWeights, FLOAT(0.f));
        SpvId mirrorDir = SUB_V3(SCALE_V3(n, twice), view);
        spvStore(s, counter, UINT(0));
        SpvId pHeader = spvNewId(s), pTest = spvNewId(s), pBody = spvNewId(s), pNext = spvNewId(s), pDone = spvNewId(s);
        spvBranch(s, pHeader);
        spvLabel(s, pHeader);
        spvLoopMerge(s, pDone, pNext);
        spvBranch(s, pTest);
        spvLabel(s, pTest);
        SpvId corner = spvLoad(s, s->uintType, counter);
        spvBranchIf(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, corner, UINT(8)), pBody, pDone);
        spvLabel(s, pBody);
        {
            SpvId cx = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, corner, UINT(1));
            SpvId cy = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, corner, UINT(1)), UINT(1));
            SpvId cz = spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, corner, UINT(2));
            SpvId x = ADD_U(bx, cx), y = ADD_U(by, cy), z = ADD_U(bz, cz);
#define SIDE_OF(bit, t) spvSelect(s, s->floatType, spvOp2(s, SPV_OP_INotEqual, s->boolType, bit, UINT(0)), t, SUB_F(FLOAT(1.f), t))
            SpvId weight = MUL_F(MUL_F(SIDE_OF(cx, X(w)), SIDE_OF(cy, Y(w))), SIDE_OF(cz, Z(w)));
#undef SIDE_OF
            SpvId index = ADD_U(MUL_U(ADD_U(MUL_U(z, dy), y), dx), x), at = MUL_U(index, UINT(4));
            /* as artfIndirectAt: from the probe to the point, what is behind counts less and what it cannot see hardly at all */
            SpvId cell = vec3Of(s, spvOp1(s, SPV_OP_ConvertUToF, s->floatType, x), spvOp1(s, SPV_OP_ConvertUToF, s->floatType, y), spvOp1(s, SPV_OP_ConvertUToF, s->floatType, z));
            SpvId there = ADD_V3(origin, MUL_V3(cell, spacing));
            SpvId toPoint = SUB_V3(q, there);
            if (walls) {
            /* (one inverse square root for both the distance and the direction) */
            SpvId d2 = MAX_F(DOT3(toPoint, toPoint), FLOAT(1e-12f));
            SpvId inv = spvGlsl1(s, 32 /* InverseSqrt */, s->floatType, d2), distance = MUL_F(d2, inv);
            SpvId facing = MUL_F(SUB_F(FLOAT(1.f), MUL_F(DOT3(toPoint, n), inv)), FLOAT(.5f));
            SpvId dir = toPoint;
            weight = MUL_F(weight, ADD_F(MUL_F(facing, facing), FLOAT(.2f)));
            /* how far it sees that way: bilinear in its map, as probeReach */
            SpvId mean, square;
            probeReachSpv(s, &r, probes, ADD_U(MUL_U(probeCount, UINT(16)), MUL_U(index, UINT(ARTF_PROBE_MAP))), dir, &mean, &square);
            SpvId variance = MAX_F(SUB_F(square, MUL_F(mean, mean)), FLOAT(1e-4f)), past = SUB_F(distance, mean);
            SpvId seen = DIV_F(variance, ADD_F(variance, MUL_F(past, past)));
            SpvId visible = spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, distance, mean), MAX_F(MUL_F(MUL_F(seen, seen), seen), FLOAT(.002f)), FLOAT(1.f));
            weight = MUL_F(weight, visible);
            }
            spvStore(s, probeWeights, ADD_F(spvLoad(s, s->floatType, probeWeights), weight));
            SpvId c0 = spvShuffle3(s, bufferVec(s, &r, probes, at));
            SpvId lean = vec3Of(s, DOT3(spvShuffle3(s, bufferVec(s, &r, probes, ADD_U(at, UINT(1)))), n), DOT3(spvShuffle3(s, bufferVec(s, &r, probes, ADD_U(at, UINT(2)))), n),
                                DOT3(spvShuffle3(s, bufferVec(s, &r, probes, ADD_U(at, UINT(3)))), n));
            spvStore(s, probeSum, ADD_V3(spvLoad(s, s->vec3, probeSum), SCALE_V3(spvGlsl2(s, SPV_GLSL_FMax, s->vec3, ADD_V3(c0, lean), same3(s, 0.f)), weight)));
            /* (and toward the mirror direction: what a shiny surface reflects of light that bounced) */
            SpvId toward = vec3Of(s, DOT3(spvShuffle3(s, bufferVec(s, &r, probes, ADD_U(at, UINT(1)))), mirrorDir), DOT3(spvShuffle3(s, bufferVec(s, &r, probes, ADD_U(at, UINT(2)))), mirrorDir),
                                  DOT3(spvShuffle3(s, bufferVec(s, &r, probes, ADD_U(at, UINT(3)))), mirrorDir));
            spvStore(s, probeMirror, ADD_V3(spvLoad(s, s->vec3, probeMirror), SCALE_V3(spvGlsl2(s, SPV_GLSL_FMax, s->vec3, ADD_V3(c0, toward), same3(s, 0.f)), weight)));
        }
        spvBranch(s, pNext);
        spvLabel(s, pNext);
        spvStore(s, counter, ADD_U(spvLoad(s, s->uintType, counter), UINT(1)));
        spvBranch(s, pHeader);
        spvLabel(s, pDone);
        SpvId sum = spvLoad(s, s->vec3, probeSum), weights = spvLoad(s, s->floatType, probeWeights);
        SpvId scale = spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, weights, FLOAT(1e-6f)), DIV_F(FLOAT(1.f), MAX_F(weights, FLOAT(1e-6f))), FLOAT(0.f));
        spvStore(s, acc.diffuse, SCALE_V3(sum, MUL_F(MUL_F(W(indirectRow), open), scale)));
        SpvId bounced = SCALE_V3(spvLoad(s, s->vec3, probeMirror), MUL_F(scale, W(indirectRow)));
        spvStore(s, acc.highlight, pick3(s, shiny, MUL_V3(bounced, mirror), same3(s, 0.f)));
    }
    spvIfEnd(s, indirectMerge);
    /* (what came from all around, for ambient occlusion after the frame) */
    SpvId aroundDiffuse = spvLoad(s, s->vec3, acc.diffuse), aroundHighlight = spvLoad(s, s->vec3, acc.highlight);
    /* a clear coat and sheen: what they take from all around, as on the CPU */
    SpvId coatSeen = 0, aroundCoat = 0, aroundSheen = 0, subsurface = 0, reach = 0;
    if (coats) {
        sheenColor = SCALE_V3(sheenColor, notMetal);
        subsurface = SCALE_V3(spvShuffle3(s, e[6]), notMetal);
        reach = DIV_F(FLOAT(1.44269504f), W(e[1]));
        coatN = pick3(s, spvOp2(s, SPV_OP_INotEqual, s->boolType, AS_UINT(Z(e[2])), UINT(0)), coatN, n);
        coating.coatN = coatN;
        coating.ndv = ndv;
        coating.sheenInverse = DIV_F(FLOAT(1.f), MUL_F(sheenRough, sheenRough));
        SpvId coatNv = MAX_F(DOT3(coatN, view), FLOAT(1e-4f));
        const Reflecting lobe = {shaderLobeOf(s, coatRough, coatNv), FLOAT(1.f), same3(s, .04f)};
        coating.lobe = lobe;
        SpvId far = shaderFifth(s, coatNv);
        coatSeen = shaderCoatFresnel(s, far);
        SpvId kept = shaderMirror(s, FLOAT(.04f), SUB_F(FLOAT(1.f), coatRough), MUL_F(far, open), open);
        SpvId coatMirror = SUB_V3(SCALE_V3(coatN, MUL_F(DOT3(coatN, view), FLOAT(2.f))), view);
        SpvId coatUp = ADD_F(MUL_F(Y(coatMirror), FLOAT(.5f)), FLOAT(.5f));
        spvStore(s, coating.coat, SCALE_V3(shaderSkyGround3(s, ground, sky, coatUp), kept));
        SpvId coatMerge;
        spvIfBegin(s, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, environmentLight, FLOAT(0.f)), &coatMerge);
        spvStore(s, coating.coat, SCALE_V3(envReflected(s, &r, coatRough, coatMirror), MUL_F(environmentLight, kept)));
        spvIfEnd(s, coatMerge);
        /* (sheen from all around, as artfSheenAround) */
        spvStore(s, coating.sheen, SCALE_V3(aroundDiffuse, shaderSheenAround(s, shaderFifth(s, ndv), sheenRough)));
        spvStore(s, coating.under, same3(s, 0.f));
        aroundCoat = spvLoad(s, s->vec3, coating.coat);
        aroundSheen = spvLoad(s, s->vec3, coating.sheen);
    }
    /* the sun's shadow: as artfShadowOfSun */
    SpvId eye = eyeOf(s, &r, world);
    SpvId sunRow = shadowRow(s, &shadows, UINT(S_SUN)), splits = shadowRow(s, &shadows, UINT(S_SPLITS));
    spvStore(s, shadowed, FLOAT(1.f));
    if (coats) spvStore(s, through, FLOAT(0.f));
    SpvId sunCasts;
    SpvId tracedCasts = traced ? AS_UINT(Y(frameVec(s, &r, F_TRACED))) : 0;
    if (traced) {
        /* from rays, where the sun reaches at all: as tracedLanes in kernels.inc */
        SpvId reaches = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, DOT3(n, spvShuffle3(s, frameVec(s, &r, F_TOSUN))), FLOAT(0.f));
        spvIfBegin(s, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, tracedCasts, UINT(ARTF_TRACED_SUN)), UINT(0)), reaches), &sunCasts);
        spvStore(s, shadowed, tracedShadowSpv(tracer, world, n, true, 0));
    } else {
        spvIfBegin(s, spvOp2(s, SPV_OP_INotEqual, s->boolType, AS_UINT(X(sunRow)), UINT(0)), &sunCasts);
    }
    if (!traced) {
        SpvId distance = spvGlsl1(s, SPV_GLSL_Length, s->floatType, SUB_V3(world, eye));
        SpvId past0 = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, distance, X(splits)), past1 = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, distance, Y(splits));
        SpvId past2 = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, distance, Z(splits));
        SpvId cascade = spvSelect(s, s->uintType, past1, UINT(2), spvSelect(s, s->uintType, past0, UINT(1), UINT(0)));
        SpvId sunView = ADD_U(ADD_U(AS_UINT(X(sunRow)), UINT(0xFFFFFFFFu)), cascade);
        SpvId viewBase = ADD_U(UINT(S_VIEWS), MUL_U(sunView, UINT(S_VIEW_ROWS)));
        SpvId facing = MAX_F(DOT3(n, spvShuffle3(s, shadowRow(s, &shadows, ADD_U(viewBase, UINT(S_PLACE))))), FLOAT(.15f));
        SpvId lift = DIV_F(MUL_F(Z(shadowRow(s, &shadows, ADD_U(viewBase, UINT(S_INFO)))), FLOAT(2.f)), facing);
        SpvId sunPlace = ADD_V3(world, SCALE_V3(n, lift));
        SpvId seen = softShadows ? shadowLookupSoft(s, &shadows, sunView, sunPlace, Z(frameVec(s, &r, F_LOOK)), true) : shadowLookup(s, &shadows, sunView, sunPlace);
        spvStore(s, shadowed, spvSelect(s, s->floatType, past2, FLOAT(1.f), seen));
        if (coats) {
            /* (how far the sun's light goes inside what it meets first, for light under the surface) */
            SpvId thick = depthThroughOf(s, &shadows, sunView, world, true);
            spvStore(s, through, G_PICK(G_AND(spvOp1(s, SPV_OP_LogicalNot, s->boolType, past2), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, thick, FLOAT(0.f))), exp2Of(s, MUL_F(spvOp1(s, SPV_OP_FNegate, s->floatType, thick), reach)), FLOAT(0.f)));
        }
    }
    spvIfEnd(s, sunCasts);
    addLight(s, &acc, &reflecting, n, view, spvShuffle3(s, frameVec(s, &r, F_TOSUN)), spvLoad(s, s->floatType, shadowed), spvShuffle3(s, frameVec(s, &r, F_SUNCOLOR)));
    if (coats) addExtras(s, layers, n, view, spvShuffle3(s, frameVec(s, &r, F_TOSUN)), spvLoad(s, s->floatType, shadowed), FLOAT(1.f), spvLoad(s, s->floatType, through), spvShuffle3(s, frameVec(s, &r, F_SUNCOLOR)));

    /* the point lights */
    SpvId lightCount = AS_UINT(X(frameVec(s, &r, F_COUNTS)));
    spvStore(s, counter, UINT(0));
    SpvId header = spvNewId(s), test = spvNewId(s), body = spvNewId(s), next = spvNewId(s), done = spvNewId(s);
    spvBranch(s, header);
    spvLabel(s, header);
    spvLoopMerge(s, done, next);
    spvBranch(s, test);
    spvLabel(s, test);
    SpvId i = spvLoad(s, s->uintType, counter);
    spvBranchIf(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, i, lightCount), body, done);
    spvLabel(s, body);
    {
        SpvId at = ADD_U(UINT(F_LIGHTS), MUL_U(i, UINT(F_LIGHT_ROWS)));
        SpvId lightA = frameVecAt(s, &r, at), lightB = frameVecAt(s, &r, ADD_U(at, UINT(1))), lightC = frameVecAt(s, &r, ADD_U(at, UINT(2)));
        SpvId d = SUB_V3(spvShuffle3(s, lightA), world);
        SpvId d2 = DOT3(d, d);
        SpvId invLength = DIV_F(FLOAT(1.f), spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, MAX_F(d2, FLOAT(1e-8f))));
        SpvId radius = W(lightA);
        SpvId fade = MAX_F(SUB_F(FLOAT(1.f), DIV_F(d2, MUL_F(radius, radius))), FLOAT(0.f));
        if (viewed) spvStore(s, lightsSeen, ADD_F(spvLoad(s, s->floatType, lightsSeen), spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, fade, FLOAT(0.f)), FLOAT(1.f), FLOAT(0.f))));
        SpvId toLight = SCALE_V3(d, invLength);
        SpvId spot = MUL_F(SUB_F(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(toLight, spvShuffle3(s, lightC))), W(lightC)), W(lightB));
        spot = MIN_F(MAX_F(spot, FLOAT(0.f)), FLOAT(1.f));
        /* its shadow: as artfShadowOfLight */
        spvStore(s, shadowed, FLOAT(1.f));
        if (coats) spvStore(s, through, FLOAT(0.f));
        SpvId shapeA = 0, shapeB = 0, shapeC = 0, shapeD = 0;
        if (shapes) {
            SpvId shapeAt = ADD_U(UINT(F_SHAPES), MUL_U(i, UINT(4)));
            shapeA = frameVecAt(s, &r, shapeAt);
            shapeB = frameVecAt(s, &r, ADD_U(shapeAt, UINT(1)));
            shapeC = frameVecAt(s, &r, ADD_U(shapeAt, UINT(2)));
            shapeD = frameVecAt(s, &r, ADD_U(shapeAt, UINT(3)));
        }
        SpvId lampCasts;
        if (traced) {
            /* from rays, where the light reaches at all */
            SpvId casts = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, tracedCasts, i), UINT(1)), UINT(0));
            spvIfBegin(s, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, casts, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, MUL_F(fade, spot), FLOAT(0.f))), &lampCasts);
            spvStore(s, shadowed, tracedShadowSpv(tracer, world, n, false, i));
            spvIfEnd(s, lampCasts);
        } else {
            SpvId lamp = AS_UINT(X(shadowRow(s, &shadows, ADD_U(UINT(S_LIGHTS), i))));
            spvIfBegin(s, spvOp2(s, SPV_OP_INotEqual, s->boolType, lamp, UINT(0)), &lampCasts);
            {
                SpvId lampSize = softShadows ? frameFloatAt(s, &r, ADD_U(UINT(F_LIGHT_SIZES), spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, i, UINT(2))), spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, i, UINT(3))) : 0;
                /* (as wide as its shape, too: artfLightSoftSize) */
                if (softShadows && shapes) lampSize = ADD_F(lampSize, MAX_F(W(shapeA), W(shapeB)));
                spvStore(s, shadowed, lampShadowSpv(s, &shadows, lamp, d, d2, n, toLight, world, lampSize));
                if (coats) {
                    SpvId thick = depthThroughOf(s, &shadows, lampViewSpv(s, lamp, d), world, false);
                    spvStore(s, through, G_PICK(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, thick, FLOAT(0.f)), exp2Of(s, MUL_F(spvOp1(s, SPV_OP_FNegate, s->floatType, thick), reach)), FLOAT(0.f)));
                }
            }
            spvIfEnd(s, lampCasts);
        }
        SpvId strength = MUL_F(MUL_F(MUL_F(fade, fade), spot), spvLoad(s, s->floatType, shadowed));
        SpvId shapedMerge = 0, plainMerge = 0, shapedHere = 0;
        if (shapes) {
            /* a shape or a profile: as addShaped on the CPU */
            shapedHere = spvOp2(s, 166 /* OpLogicalOr */, s->boolType, G_GT(W(shapeA), FLOAT(0.f)), G_GT(W(shapeC), FLOAT(0.f)));
            spvIfBegin(s, shapedHere, &shapedMerge);
            SpvId ball = frameFloatAt(s, &r, ADD_U(UINT(F_LIGHT_SIZES), spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, i, UINT(2))), spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, i, UINT(3)));
            SpvId ltc[5];
            ltcAt(s, &r, extraLights, rough, MAX_F(DOT3(n, view), FLOAT(0.f)), ltc);
            ShapeLit place = lightPlace(s, shapeA, shapeB, spvShuffle3(s, lightA), ball, world, n, view, rough, DIV_F(FLOAT(1.f), MUL_F(radius, radius)), ltc);
            SpvId near_ = MAX_F(SUB_F(FLOAT(1.f), DIV_F(place.distance2, MUL_F(radius, radius))), FLOAT(0.f));
            SpvId from = NORMALIZE(spvOp1(s, SPV_OP_FNegate, s->vec3, d));
            SpvId profile = G_PICK(G_GT(W(shapeC), FLOAT(0.f)), profileAt(s, &r, extraLights, i, spvShuffle3(s, shapeC), spvShuffle3(s, shapeD), from), FLOAT(1.f));
            SpvId lit = MUL_F(MUL_F(near_, near_), profile), shadow = spvLoad(s, s->floatType, shadowed);
            SpvId amount = MUL_F(MUL_F(MUL_F(lit, place.diffuse), spot), shadow), color = spvShuffle3(s, lightB);
            /* a shape's from all of it (as addShaped), a ball's or a point's from a point of it */
            SpvId area = G_GT(W(shapeA), FLOAT(0.f));
            SpvId shown = G_PICK(area, MUL_F(MUL_F(MUL_F(place.lambert, profile), spot), shadow), MUL_F(MAX_F(DOT3(n, place.toDiffuse), FLOAT(0.f)), amount));
            spvStore(s, acc.diffuse, ADD_V3(spvLoad(s, s->vec3, acc.diffuse), SCALE_V3(color, shown)));
            SpvId towardNdl = MAX_F(DOT3(n, place.toHighlight), FLOAT(0.f));
            SpvId shine = pick3(s, area, areaHighlight(s, &reflecting, view, place.way, MUL_F(MUL_F(MUL_F(place.glossy, profile), spot), shadow)),
                                lightHighlight(s, &reflecting, n, view, place.toHighlight, towardNdl, MUL_F(MUL_F(MUL_F(lit, place.highlight), spot), shadow)));
            spvStore(s, acc.highlight, ADD_V3(spvLoad(s, s->vec3, acc.highlight), MUL_V3(color, shine)));
            if (coats) addExtras(s, layers, n, view, place.toDiffuse, amount, MUL_F(MUL_F(lit, place.diffuse), spot), spvLoad(s, s->floatType, through), color);
            spvIfEnd(s, shapedMerge);
            spvIfBegin(s, spvOp1(s, SPV_OP_LogicalNot, s->boolType, shapedHere), &plainMerge);
        }
        /* a light with a size: its highlight from its point nearest the mirror ray (shading.inc's BallNearest) */
        SpvId size = frameFloatAt(s, &r, ADD_U(UINT(F_LIGHT_SIZES), spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, i, UINT(2))), spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, i, UINT(3)));
        SpvId energy;
        SpvId toward = shaderBallNearest(s, n, view, d, size, rough, &energy);
        SpvId sized = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, size, FLOAT(0.f));
        SpvId sizedMerge, pointMerge;
        spvIfBegin(s, sized, &sizedMerge);
        {
            /* (its diffuse light from the middle; its highlight from the nearest point, kept as bright as a point's) */
            SpvId ndl = MAX_F(DOT3(n, toLight), FLOAT(0.f));
            spvStore(s, acc.diffuse, ADD_V3(spvLoad(s, s->vec3, acc.diffuse), SCALE_V3(spvShuffle3(s, lightB), MUL_F(ndl, strength))));
            SpvId towardNdl = MAX_F(DOT3(n, toward), FLOAT(0.f));
            SpvId lit = lightHighlight(s, &reflecting, n, view, toward, towardNdl, MUL_F(strength, energy));
            spvStore(s, acc.highlight, ADD_V3(spvLoad(s, s->vec3, acc.highlight), MUL_V3(spvShuffle3(s, lightB), lit)));
        }
        spvIfEnd(s, sizedMerge);
        spvIfBegin(s, spvOp1(s, SPV_OP_LogicalNot, s->boolType, sized), &pointMerge);
        addLight(s, &acc, &reflecting, n, view, toLight, strength, spvShuffle3(s, lightB));
        spvIfEnd(s, pointMerge);
        if (coats) addExtras(s, layers, n, view, toLight, strength, MUL_F(MUL_F(fade, fade), spot), spvLoad(s, s->floatType, through), spvShuffle3(s, lightB));
        if (shapes) spvIfEnd(s, plainMerge);
    }
    spvBranch(s, next);
    spvLabel(s, next);
    spvStore(s, counter, ADD_U(spvLoad(s, s->uintType, counter), UINT(1)));
    spvBranch(s, header);
    spvLabel(s, done);

    /* those past them that reach this square: the same light, with no shadow */
    SpvId extraCount = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, W(frameVec(s, &r, F_EYE + 2))), anyExtra;
    spvIfBegin(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, UINT(0), extraCount), &anyExtra);
    {
        SpvId coord = spvLoad(s, s->vec4, pixelCoord);
        SpvId tx = spvOp2(s, SPV_OP_UDiv, s->uintType, spvOp1(s, SPV_OP_ConvertFToU, s->uintType, X(coord)), UINT(ARTF_LIGHT_TILE));
        SpvId ty = spvOp2(s, SPV_OP_UDiv, s->uintType, spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Y(coord)), UINT(ARTF_LIGHT_TILE));
        SpvId tilesX = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(spvOp1(s, SPV_OP_ConvertFToU, s->uintType, W(frameVec(s, &r, F_FOG))), UINT(ARTF_LIGHT_TILE - 1)), UINT(ARTF_LIGHT_TILE));
        /* (and the slice of depth: as artfLightSliceAt) */
        SpvId slicing = frameVec(s, &r, F_ORTHO);
        SpvId along = DOT3(SUB_V3(world, spvShuffle3(s, frameVec(s, &r, F_CAMERA))), spvShuffle3(s, frameVec(s, &r, F_EYE + 2)));
        SpvId deep = ADD_F(MUL_F(spvGlsl1(s, SPV_GLSL_Log2, s->floatType, MAX_F(along, FLOAT(1e-30f))), X(slicing)), Y(slicing));
        SpvId slice = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, MIN_F(MAX_F(spvGlsl1(s, SPV_GLSL_Floor, s->floatType, deep), FLOAT(0.f)), FLOAT((float)(ARTF_LIGHT_SLICES - 1))));
        SpvId head = MUL_U(ADD_U(MUL_U(ADD_U(MUL_U(ty, tilesX), tx), UINT(ARTF_LIGHT_SLICES)), slice), UINT(2));
        SpvId base = spvLoad(s, s->uintType, bufferUintPtr(s, &r, tileLights, head));
        SpvId listed = spvLoad(s, s->uintType, bufferUintPtr(s, &r, tileLights, ADD_U(head, UINT(1))));
        spvStore(s, counter, UINT(0));
        SpvId eHeader = spvNewId(s), eTest = spvNewId(s), eBody = spvNewId(s), eNext = spvNewId(s), eDone = spvNewId(s);
        spvBranch(s, eHeader);
        spvLabel(s, eHeader);
        spvLoopMerge(s, eDone, eNext);
        spvBranch(s, eTest);
        spvLabel(s, eTest);
        SpvId k = spvLoad(s, s->uintType, counter);
        spvBranchIf(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, k, listed), eBody, eDone);
        spvLabel(s, eBody);
        {
            SpvId li = spvLoad(s, s->uintType, bufferUintPtr(s, &r, tileLights, ADD_U(base, k)));
            SpvId at = MUL_U(li, UINT(3));
            SpvId lightA = bufferVec(s, &r, extraLights, at), lightB = bufferVec(s, &r, extraLights, ADD_U(at, UINT(1))), lightC = bufferVec(s, &r, extraLights, ADD_U(at, UINT(2)));
            SpvId d = SUB_V3(spvShuffle3(s, lightA), world);
            SpvId d2 = DOT3(d, d);
            SpvId invLength = DIV_F(FLOAT(1.f), spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, MAX_F(d2, FLOAT(1e-8f))));
            SpvId radius = W(lightA);
            SpvId fade = MAX_F(SUB_F(FLOAT(1.f), DIV_F(d2, MUL_F(radius, radius))), FLOAT(0.f));
            if (viewed) spvStore(s, lightsSeen, ADD_F(spvLoad(s, s->floatType, lightsSeen), spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, fade, FLOAT(0.f)), FLOAT(1.f), FLOAT(0.f))));
            SpvId toLight = SCALE_V3(d, invLength);
            SpvId spot = MUL_F(SUB_F(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(toLight, spvShuffle3(s, lightC))), W(lightC)), W(lightB));
            spot = MIN_F(MAX_F(spot, FLOAT(0.f)), FLOAT(1.f));
            /* the first few have shadows of their own: as artfShadowOfExtra */
            SpvId listed = spvOp2(s, SPV_OP_ULessThan, s->boolType, li, UINT(ARTF_SHADOW_EXTRA));
            SpvId lamp = AS_UINT(X(shadowRow(s, &shadows, ADD_U(UINT(S_EXTRA), spvGlsl2(s, 38 /* UMin */, s->uintType, li, UINT(ARTF_SHADOW_EXTRA - 1))))));
            lamp = spvSelect(s, s->uintType, listed, lamp, UINT(0));
            spvStore(s, shadowed, FLOAT(1.f));
            SpvId extraCasts;
            spvIfBegin(s, spvOp2(s, SPV_OP_INotEqual, s->boolType, lamp, UINT(0)), &extraCasts);
            spvStore(s, shadowed, lampShadowSpv(s, &shadows, lamp, d, d2, n, toLight, world, 0));
            spvIfEnd(s, extraCasts);
            addLight(s, &acc, &reflecting, n, view, toLight, MUL_F(MUL_F(MUL_F(fade, fade), spot), spvLoad(s, s->floatType, shadowed)), spvShuffle3(s, lightB));
            if (coats) addExtras(s, layers, n, view, toLight, MUL_F(MUL_F(MUL_F(fade, fade), spot), spvLoad(s, s->floatType, shadowed)), FLOAT(0.f), FLOAT(0.f), spvShuffle3(s, lightB));
        }
        spvBranch(s, eNext);
        spvLabel(s, eNext);
        spvStore(s, counter, ADD_U(spvLoad(s, s->uintType, counter), UINT(1)));
        spvBranch(s, eHeader);
        spvLabel(s, eDone);
    }
    spvIfEnd(s, anyExtra);

    SpvId lit = ADD_V3(ADD_V3(MUL_V3(SCALE_V3(albedo, notMetal), spvLoad(s, s->vec3, acc.diffuse)), spvLoad(s, s->vec3, acc.highlight)), emission);
    SpvId aroundLit = ADD_V3(MUL_V3(SCALE_V3(albedo, notMetal), aroundDiffuse), aroundHighlight);
    if (coats) {
        /* sheen on the surface, and what came from under it, under the coat; the coat takes from all of it what its
         * Fresnel reflects */
        SpvId keepUnder = SUB_F(FLOAT(1.f), MUL_F(coatAmount, coatSeen));
        SpvId under = ADD_V3(ADD_V3(SUB_V3(lit, emission), MUL_V3(subsurface, spvLoad(s, s->vec3, coating.under))), MUL_V3(sheenColor, spvLoad(s, s->vec3, coating.sheen)));
        lit = ADD_V3(ADD_V3(SCALE_V3(under, keepUnder), SCALE_V3(spvLoad(s, s->vec3, coating.coat), coatAmount)), emission);
        aroundLit = ADD_V3(SCALE_V3(ADD_V3(aroundLit, MUL_V3(sheenColor, aroundSheen)), keepUnder), SCALE_V3(aroundCoat, coatAmount));
    }
    /* an unlit material: its color as it is, and its own light */
    SpvId unlit = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, AS_UINT(W(m2)), UINT(ARTF_MATERIAL_UNLIT)), UINT(0));
    lit = pick3(s, unlit, ADD_V3(albedo, emission), lit);
    if (viewed) {
        /* what a view shows, in its place: its frame has no fog and no exposure */
        /* (without the bit that says a pose bent it: views follow instances alone, as on the CPU) */
        SpvId instance = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvLoad(s, s->uintType, vInstance), UINT(~ARTF_MOTION_VECTOR));
        SpvId meshWord = spvLoad(s, s->uintType, vFade), meshlet = instance;
        if (meshletIds) {
            SpvId first = spvLoad(s, s->uintType, bufferUintPtr(s, &r, meshletTable, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, meshWord, UINT((1u << 23) - 1))));
            meshlet = spvLoad(s, s->uintType, bufferUintPtr(s, &r, meshletTable, ADD_U(first, spvOp1(s, SPV_OP_Bitcast, s->uintType, spvLoad(s, s->intType, primitive)))));
        }
        const ViewInputs in = {albedo, n, rough, metal, aroundLit, emission, world, instance, meshWord, meshlet, spvLoad(s, s->floatType, lightsSeen)};
        lit = viewLightSpv(s, &r, &in, lit);
    }
    /* air, then film, as on the CPU */
    SpvId look = frameVec(s, &r, F_LOOK), fog = spvShuffle3(s, frameVec(s, &r, F_FOG));
    SpvId keep = shaderFogKeep(s, MUL_F(spvGlsl1(s, SPV_GLSL_Length, s->floatType, SUB_V3(world, eyeOf(s, &r, world))), Y(look)));
    SpvId fogged = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, Y(look), FLOAT(0.f));
    SpvId volumetric = 0;
    if (hdr) {
        /* (fog that light goes through comes from the grid instead) */
        volumetric = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, fogged, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, Y(frameVec(s, &r, F_POST + 1)), FLOAT(0.f)));
        fogged = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, fogged, spvOp1(s, SPV_OP_LogicalNot, s->boolType, volumetric));
    }
    lit = pick3(s, fogged, ADD_V3(fog, SCALE_V3(SUB_V3(lit, fog), keep)), lit);
    if (hdr) {
        /* fog that light goes through, where this pixel is, as on the CPU (only in the shaders made for it) */
        SpvId airLeft = FLOAT(1.f);
        if (froxelsOn) {
            SpvId coord = spvLoad(s, s->vec4, pixelCoord);
            /* (how far along the view: w, or in an orthographic view, from the depth) */
            SpvId depthRow = frameVec(s, &r, F_ORTHO);
            SpvId along = spvSelect(s, s->floatType, orthoOn(s, &r), SUB_F(W(depthRow), MUL_F(Z(coord), Z(depthRow))), DIV_F(FLOAT(1.f), W(coord)));
            SpvId air = froxelAt(s, &r, froxels, SUB_F(X(coord), FLOAT(.5f)), SUB_F(Y(coord), FLOAT(.5f)), along);
            airLeft = spvSelect(s, s->floatType, volumetric, W(air), FLOAT(1.f));
            lit = pick3(s, volumetric, ADD_V3(SCALE_V3(lit, W(air)), spvShuffle3(s, air)), lit);
        }
        /* light as it is, premultiplied; and the surface */
        SpvId alphaHdr = MIN_F(MAX_F(shown, FLOAT(0.f)), FLOAT(1.f));
        spvStore(s, outColor, vec4Of(s, SCALE_V3(spvGlsl2(s, SPV_GLSL_FMax, s->vec3, lit, same3(s, 0.f)), alphaHdr), alphaHdr));
        SpvId lum3 = vec3Of(s, FLOAT(.3f), FLOAT(.59f), FLOAT(.11f));
        SpvId around = DOT3(aroundLit, lum3);
        SpvId share = DIV_F(MUL_F(around, airLeft), MAX_F(DOT3(lit, lum3), FLOAT(1e-6f)));
        /* the second word: the instance + 1, or for what a pose bent, where it was (as kernels.inc's pixelMotion) */
        SpvId who = spvLoad(s, s->uintType, vInstance);
        SpvId instanceWord = ADD_U(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, who, UINT(~ARTF_MOTION_VECTOR)), UINT(1));
        SpvId bent = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, who, UINT(ARTF_MOTION_VECTOR)), UINT(0));
        SpvId before = s->block, bentMerge;
        spvIfBegin(s, bent, &bentMerge);
        SpvId past = spvLoad(s, s->vec4, vPast);
        SpvId pastAt = vec3Of(s, X(past), Y(past), Z(past));
        SpvId pixel = spvLoad(s, s->vec4, pixelCoord), jitter = frameVec(s, &r, F_POST + 2);
        SpvId sceneW = W(frameVec(s, &r, F_FOG)), sceneH = W(frameVec(s, &r, F_EYE + 1));
        SpvId pastW = MAX_F(Z(pastAt), FLOAT(1e-5f));
        SpvId pastX = SUB_F(MUL_F(ADD_F(MUL_F(DIV_F(X(pastAt), pastW), FLOAT(.5f)), FLOAT(.5f)), sceneW), FLOAT(.5f));
        SpvId pastY = SUB_F(MUL_F(SUB_F(FLOAT(.5f), MUL_F(DIV_F(Y(pastAt), pastW), FLOAT(.5f))), sceneH), FLOAT(.5f));
        SpvId movedX = SUB_F(pastX, SUB_F(SUB_F(X(pixel), FLOAT(.5f)), Z(jitter))), movedY = SUB_F(pastY, ADD_F(SUB_F(Y(pixel), FLOAT(.5f)), W(jitter)));
        SpvId behind = spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, Z(pastAt), FLOAT(1e-5f));
        movedX = spvSelect(s, s->floatType, behind, FLOAT(1e5f), movedX);
        movedY = spvSelect(s, s->floatType, behind, FLOAT(1e5f), movedY);
#define PACKED(d) spvOp1(s, SPV_OP_ConvertFToU, s->uintType, MIN_F(MAX_F(ADD_F(MUL_F(d, FLOAT(4.f)), FLOAT(16384.5f)), FLOAT(0.f)), FLOAT(32767.f)))
        SpvId vector = spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, UINT(ARTF_MOTION_VECTOR), spvOp2(s, 197, s->uintType, PACKED(movedX), spvOp2(s, SPV_OP_ShiftLeftLogical, s->uintType, PACKED(movedY), UINT(15))));
#undef PACKED
        SpvId vectorFrom = s->block;
        spvIfEnd(s, bentMerge);
        SpvId word = spvPhi(s, s->uintType, vector, vectorFrom, instanceWord, before);
        const SpvId surfaceParts[2] = {packSurfaceSpv(s, n, rough, metal, share), word};
        spvStore(s, outSurface, spvCompose(s, spvTypeVector(s, s->uintType, 2), surfaceParts, 2));
        spvEndMain(s);
        const SpvId all[14] = {vWorld, vNormal, vUv, vMaterial, vColor, outColor, outSurface, vOpacity, frontFacing, vFade, pixelCoord, vInstance, primitive, vPast};
        SpvId hdrInterface[14];
        uint32_t used = 0;
        for (int k = 0; k < 14; ++k)
            if (all[k]) hdrInterface[used++] = all[k];
        spvEntryPoint(s, SPV_MODEL_Fragment, main, hdrInterface, used);
        spvExecutionMode(s, main, 7 /* OriginUpperLeft */, 0, 0, 0, 0);
        artfFree(tracer);
        return finish(s, size);
    }
    lit = exposed(s, lit, X(look));
    /* to the frame's colors: sRGB, as on the CPU */
    SpvId lo = vec3Of(s, FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)), hi = vec3Of(s, FLOAT(1.f), FLOAT(1.f), FLOAT(1.f));
    SpvId clamped = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, lit, lo), hi);
    /* premultiplied, as the frame is: 1 for all that is solid */
    SpvId alpha = MIN_F(MAX_F(shown, FLOAT(0.f)), FLOAT(1.f));
    SpvId finished = vec4Of(s, SCALE_V3(srgbEncode3(s, clamped), alpha), alpha);
    spvStore(s, outColor, finished);
    if (lists) {
        /* in this pixel's list: where it is, its color as a frame holds it, and whose it is */
        SpvId coord = spvLoad(s, s->vec4, fragCoord);
        SpvId width = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, W(frameVec(s, &r, F_FOG)));
        SpvId pixel = ADD_U(MUL_U(spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Y(coord)), width), spvOp1(s, SPV_OP_ConvertFToU, s->uintType, X(coord)));
        SpvId slot = spvAtomicAdd(s, bufferUintPtr(s, &r, listCounts, pixel), UINT(1));
        SpvId fits, packed = UINT(0);
        const SpvId parts[4] = {X(finished), Y(finished), Z(finished), W(finished)};
        static const uint32_t shifts[4] = {16, 8, 0, 24};
        for (int k = 0; k < 4; ++k) {
            SpvId byte = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, ADD_F(MUL_F(parts[k], FLOAT(255.f)), FLOAT(.5f)));
            packed = spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, packed, spvOp2(s, 196 /* OpShiftLeftLogical */, s->uintType, byte, UINT(shifts[k])));
        }
        spvIfBegin(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, slot, UINT(ARTF_LAYER_FRAGMENTS)), &fits);
        SpvId at = MUL_U(ADD_U(MUL_U(pixel, UINT(ARTF_LAYER_FRAGMENTS)), slot), UINT(3));
        spvStore(s, bufferUintPtr(s, &r, listFragments, at), AS_UINT(Z(coord)));
        spvStore(s, bufferUintPtr(s, &r, listFragments, ADD_U(at, UINT(1))), packed);
        /* (without the bit that says a pose bent it: a layer's motion goes by its instance) */
        SpvId who = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvLoad(s, s->uintType, vInstance), UINT(~ARTF_MOTION_VECTOR));
        if (lists > 1) {
            SpvId mask = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvLoad(s, s->intType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_Input, s->intType), sampleMask, &r.zero, 1)));
            who = spvOp2(s, 197, s->uintType, who, spvOp2(s, 196, s->uintType, mask, UINT(28)));
        }
        spvStore(s, bufferUintPtr(s, &r, listFragments, ADD_U(at, UINT(2))), who);
        spvIfEnd(s, fits);
    }
    spvEndMain(s);

    const SpvId all[13] = {vWorld, vNormal, vUv, vMaterial, vColor, outColor, vOpacity, frontFacing, vFade, pixelCoord, vInstance, sampleMask, primitive};
    SpvId interface[13];
    uint32_t used = 0;
    for (int k = 0; k < 13; ++k)
        if (all[k]) interface[used++] = all[k];
    spvEntryPoint(s, SPV_MODEL_Fragment, main, interface, used);
    spvExecutionMode(s, main, 7 /* OriginUpperLeft */, 0, 0, 0, 0);
    if (lists) spvExecutionMode(s, main, 9 /* EarlyFragmentTests */, 0, 0, 0, 0);
    artfFree(tracer);
    return finish(s, size);
}

/* ---- shadow maps: what casts, seen from a light ---- */

uint32_t* artfShaderShadowVertex(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    declare(s, &r, false);
    SpvId list = spvResource(s, blockOf(s, r.uintArray, false), SPV_STORAGE_StorageBuffer, 26);
    Shadows shadows = {spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, S_ROWS, 16), true), SPV_STORAGE_Uniform, 24), 0, 0, 0, 0, &r};
    SpvId pushBlock = spvTypeStruct(s, &s->uintType, 1);
    spvDecorate(s, pushBlock, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, pushBlock, 0, SPV_DECORATE_Offset, 4);
    SpvId push = spvVariable(s, spvTypePointer(s, SPV_STORAGE_PushConstant, pushBlock), SPV_STORAGE_PushConstant);
    SpvId ptrIn4 = spvTypePointer(s, SPV_STORAGE_Input, s->vec4), ptrIn2 = spvTypePointer(s, SPV_STORAGE_Input, s->vec2), ptrInInt = spvTypePointer(s, SPV_STORAGE_Input, s->intType);
    SpvId aPosition = spvVariable(s, ptrIn4, SPV_STORAGE_Input), aUv = spvVariable(s, ptrIn2, SPV_STORAGE_Input);
    spvDecorate(s, aPosition, SPV_DECORATE_Location, 0);
    spvDecorate(s, aUv, SPV_DECORATE_Location, 2);
    SpvId instanceIndex = spvVariable(s, ptrInInt, SPV_STORAGE_Input);
    spvDecorate(s, instanceIndex, SPV_DECORATE_BuiltIn, SPV_BUILTIN_InstanceIndex);
    SpvId perVertex = spvTypeStruct(s, &s->vec4, 1);
    spvDecorate(s, perVertex, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, perVertex, 0, SPV_DECORATE_BuiltIn, SPV_BUILTIN_Position);
    SpvId outVertex = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, perVertex), SPV_STORAGE_Output);
    SpvId vUv = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec2), SPV_STORAGE_Output), vMaterial = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->uintType), SPV_STORAGE_Output);
    spvDecorate(s, vUv, SPV_DECORATE_Location, 0);
    spvDecorate(s, vMaterial, SPV_DECORATE_Location, 1);
    spvDecorate(s, vMaterial, SPV_DECORATE_Flat, -1);

    SpvId main = spvBeginMain(s);
    SpvId at = MUL_U(spvOp1(s, SPV_OP_Bitcast, s->uintType, spvLoad(s, s->intType, instanceIndex)), UINT(2));
    SpvId id = spvLoad(s, s->uintType, bufferUintPtr(s, &r, list, at)), mesh = spvLoad(s, s->uintType, bufferUintPtr(s, &r, list, ADD_U(at, UINT(1))));
    SpvId base = MUL_U(id, UINT(4));
    SpvId r0 = bufferVec(s, &r, r.instances, base), r1 = bufferVec(s, &r, r.instances, ADD_U(base, UINT(1))), r2 = bufferVec(s, &r, r.instances, ADD_U(base, UINT(2))), meta = bufferVec(s, &r, r.instances, ADD_U(base, UINT(3)));
    SpvId meshBase = MUL_U(mesh, UINT(5));
    SpvId scale = spvShuffle3(s, bufferVec(s, &r, r.meshes, ADD_U(meshBase, UINT(1)))), offset = spvShuffle3(s, bufferVec(s, &r, r.meshes, ADD_U(meshBase, UINT(2))));
    SpvId p4 = vec4Of(s, ADD_V3(MUL_V3(spvShuffle3(s, spvLoad(s, s->vec4, aPosition)), scale), offset), FLOAT(1.f));
    SpvId world = vec4Of(s, vec3Of(s, DOT4(r0, p4), DOT4(r1, p4), DOT4(r2, p4)), FLOAT(1.f));
    SpvId view = spvLoad(s, s->uintType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_PushConstant, s->uintType), push, &r.zero, 1));
    SpvId row = ADD_U(UINT(S_VIEWS), MUL_U(view, UINT(S_VIEW_ROWS)));
    const SpvId clip[4] = {DOT4(shadowRow(s, &shadows, row), world), DOT4(shadowRow(s, &shadows, ADD_U(row, UINT(1))), world), DOT4(shadowRow(s, &shadows, ADD_U(row, UINT(2))), world), DOT4(shadowRow(s, &shadows, ADD_U(row, UINT(3))), world)};
    const SpvId member = r.zero;
    spvStore(s, spvAccess(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), outVertex, &member, 1), spvCompose(s, s->vec4, clip, 4));
    spvStore(s, vUv, spvLoad(s, s->vec2, aUv));
    spvStore(s, vMaterial, AS_UINT(Y(meta)));
    spvEndMain(s);

    const SpvId interface[6] = {aPosition, aUv, instanceIndex, outVertex, vUv, vMaterial};
    spvEntryPoint(s, SPV_MODEL_Vertex, main, interface, 6);
    return finish(s, size);
}

uint32_t* artfShaderShadowFragment(size_t* size, uint32_t maxTextures) {
    Spv b, *s = &b;
    spvInit(s);
    spvCapability(s, 5301); /* ShaderNonUniform */
    spvCapability(s, 5307); /* SampledImageArrayNonUniformIndexing */
    spvCapability(s, 50); /* ImageQuery: a texture's size, for the tracer's cones */
    spvExtension(s, "SPV_EXT_descriptor_indexing");
    Resources r;
    declare(s, &r, false);
    SpvId imageType, sampled = spvTypeSampledImage2D(s, &imageType);
    SpvId textures = spvResource(s, spvTypeArray(s, sampled, maxTextures, 0), SPV_STORAGE_UniformConstant, 6);
    SpvId vUv = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec2), SPV_STORAGE_Input), vMaterial = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->uintType), SPV_STORAGE_Input);
    spvDecorate(s, vUv, SPV_DECORATE_Location, 0);
    spvDecorate(s, vMaterial, SPV_DECORATE_Location, 1);
    spvDecorate(s, vMaterial, SPV_DECORATE_Flat, -1);

    SpvId main = spvBeginMain(s);
    SpvId m2 = bufferVec(s, &r, r.materials, ADD_U(MUL_U(spvLoad(s, s->uintType, vMaterial), UINT(ARTF_MATERIAL_ROWS)), UINT(2)));
    SpvId sample = textureAt(s, textures, sampled, AS_UINT(X(m2)), spvLoad(s, s->vec2, vUv));
    spvKillIf(s, spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, W(sample), Z(m2)));
    spvEndMain(s);

    const SpvId interface[2] = {vUv, vMaterial};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, interface, 2);
    spvExecutionMode(s, main, 7, 0, 0, 0, 0);
    return finish(s, size);
}

/* ---- the overdraw view: each triangle that covers a pixel adds a step of artfViewHeat ---- */

uint32_t* artfShaderOverdraw(size_t* size, bool hdr) {
    (void)hdr; /* (light or the frame's colors, a step is the same: with floats, what shows is it by sRGB) */
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    declare(s, &r, false);
    SpvId vMaterial = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->uintType), SPV_STORAGE_Input);
    spvDecorate(s, vMaterial, SPV_DECORATE_Location, 3);
    spvDecorate(s, vMaterial, SPV_DECORATE_Flat, -1);
    SpvId frontFacing = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->boolType), SPV_STORAGE_Input);
    spvDecorate(s, frontFacing, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FrontFacing);
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);

    SpvId main = spvBeginMain(s);
    /* a back shows only where its material has two sides, as the CPU keeps it */
    SpvId m2 = bufferVec(s, &r, r.materials, ADD_U(MUL_U(spvLoad(s, s->uintType, vMaterial), UINT(ARTF_MATERIAL_ROWS)), UINT(2)));
    SpvId isDouble = spvOp2(s, SPV_OP_INotEqual, s->boolType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, AS_UINT(W(m2)), UINT(ARTF_MATERIAL_DOUBLE)), UINT(0));
    SpvId front = spvLoad(s, s->boolType, frontFacing);
    spvKillIf(s, spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp1(s, SPV_OP_LogicalNot, s->boolType, front), spvOp1(s, SPV_OP_LogicalNot, s->boolType, isDouble)));
    const SpvId step[4] = {FLOAT(32.f / 255.f), FLOAT(8.f / 255.f), FLOAT(2.f / 255.f), FLOAT(0.f)};
    spvStore(s, outColor, spvCompose(s, s->vec4, step, 4));
    spvEndMain(s);

    const SpvId interface[3] = {vMaterial, frontFacing, outColor};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, interface, 3);
    spvExecutionMode(s, main, 7, 0, 0, 0, 0);
    return finish(s, size);
}

/* ---- the overlay: one triangle over the frame, its pixels read as they are ---- */

uint32_t* artfShaderOverlayVertex(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    SpvId vertexIndex = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->intType), SPV_STORAGE_Input);
    spvDecorate(s, vertexIndex, SPV_DECORATE_BuiltIn, SPV_BUILTIN_VertexIndex);
    SpvId perVertex = spvTypeStruct(s, &s->vec4, 1);
    spvDecorate(s, perVertex, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, perVertex, 0, SPV_DECORATE_BuiltIn, SPV_BUILTIN_Position);
    SpvId outVertex = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, perVertex), SPV_STORAGE_Output);

    SpvId main = spvBeginMain(s);
    SpvId i = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvLoad(s, s->intType, vertexIndex));
    /* (-1, -1), (3, -1), (-1, 3) */
    SpvId u = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvOp2(s, SPV_OP_ShiftLeftLogical, s->uintType, i, UINT(1)), UINT(2));
    SpvId v = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, i, UINT(2));
    SpvId x = SUB_F(MUL_F(spvOp1(s, 112 /* OpConvertUToF */, s->floatType, u), FLOAT(2.f)), FLOAT(1.f));
    SpvId y = SUB_F(MUL_F(spvOp1(s, 112, s->floatType, v), FLOAT(2.f)), FLOAT(1.f));
    const SpvId member = spvConstInt(s, 0), parts[4] = {x, y, FLOAT(0.f), FLOAT(1.f)};
    spvStore(s, spvAccess(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), outVertex, &member, 1), spvCompose(s, s->vec4, parts, 4));
    spvEndMain(s);

    const SpvId interface[2] = {vertexIndex, outVertex};
    spvEntryPoint(s, SPV_MODEL_Vertex, main, interface, 2);
    return finish(s, size);
}

uint32_t* artfShaderOverlayFragment(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    SpvId imageType, sampled = spvTypeSampledImage2D(s, &imageType);
    SpvId overlay = spvResource(s, sampled, SPV_STORAGE_UniformConstant, 7);
    SpvId fragCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
    spvDecorate(s, fragCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);
    SpvId ivec2 = spvNewId(s);
    spvEmit(&s->types, 23 /* OpTypeVector */, SPV_WORDS(ivec2, s->intType, 2));

    SpvId main = spvBeginMain(s);
    SpvId coord = spvLoad(s, s->vec4, fragCoord);
    const SpvId xy[2] = {spvOp1(s, SPV_OP_ConvertFToS, s->intType, X(coord)), spvOp1(s, SPV_OP_ConvertFToS, s->intType, Y(coord))};
    SpvId texel = spvImageFetch(s, imageType, spvLoad(s, sampled, overlay), spvCompose(s, ivec2, xy, 2));
    spvStore(s, outColor, texel);
    spvEndMain(s);

    const SpvId interface[2] = {fragCoord, outColor};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, interface, 2);
    spvExecutionMode(s, main, 7, 0, 0, 0, 0);
    return finish(s, size);
}

/* ---- a background pass: its bloom, and the frame's background ----
 *
 * Bindings: 11 ArtfBackgroundBlock, as vec4s       12 hdr: light, a vec4 a pixel (the program's shader writes it)
 *           13 bloom: the levels, a vec4 a texel
 *
 * The arithmetic of software.c's bloom and kernels.inc's backgroundFill. */

enum { B_SIZE = 0, B_IMAGE = 1, B_BLOOM = 2, B_COUNT = ARTF_BACKGROUND_PARAMS_AT + ARTF_BACKGROUND_PARAMS / 16 };

typedef struct BackgroundResources {
    SpvId block, hdr, bloom;
    SpvId ptrUniformVec4, ptrStorageVec4;
    SpvId zero;
} BackgroundResources;

static void backgroundDeclare(Spv* s, BackgroundResources* r, bool writeBloom) {
    r->zero = spvConstInt(s, 0);
    r->ptrUniformVec4 = spvTypePointer(s, SPV_STORAGE_Uniform, s->vec4);
    r->ptrStorageVec4 = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->vec4);
    r->block = spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, B_COUNT, 16), true), SPV_STORAGE_Uniform, 11);
    SpvId vec4Array = spvTypeArray(s, s->vec4, 0, 16);
    r->hdr = spvResource(s, blockOf(s, vec4Array, false), SPV_STORAGE_StorageBuffer, 12);
    r->bloom = spvResource(s, blockOf(s, vec4Array, writeBloom), SPV_STORAGE_StorageBuffer, 13);
}

static SpvId backgroundVec(Spv* s, const BackgroundResources* r, uint32_t i) {
    const SpvId idx[2] = {r->zero, spvConstUint(s, i)};
    return spvLoad(s, s->vec4, spvAccess(s, r->ptrUniformVec4, r->block, idx, 2));
}
static SpvId backgroundVecAt(Spv* s, const BackgroundResources* r, SpvId i) {
    const SpvId idx[2] = {r->zero, i};
    return spvLoad(s, s->vec4, spvAccess(s, r->ptrUniformVec4, r->block, idx, 2));
}
static SpvId pixelPtr(Spv* s, const BackgroundResources* r, SpvId buffer, SpvId i) {
    const SpvId idx[2] = {r->zero, i};
    return spvAccess(s, r->ptrStorageVec4, buffer, idx, 2);
}

#define LESS_F(a, b) spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, a, b)
#define MORE_F(a, b) spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, a, b)
#define BOTH(a, b) spvOp2(s, SPV_OP_LogicalAnd, s->boolType, a, b)
#define FLOOR_F(a) spvGlsl1(s, SPV_GLSL_Floor, s->floatType, a)
#define SQRT_F(a) spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, a)
#define ABS_F(a) spvGlsl1(s, SPV_GLSL_FAbs, s->floatType, a)
#define PICK_F(c, a, b) spvSelect(s, s->floatType, c, a, b)
#define TO_F(u) spvOp1(s, SPV_OP_ConvertUToF, s->floatType, u)
#define TO_U(f) spvOp1(s, SPV_OP_Bitcast, s->uintType, spvOp1(s, SPV_OP_ConvertFToS, s->intType, f))
#define XOR_U(a, b) spvOp2(s, SPV_OP_BitwiseXor, s->uintType, a, b)
#define AND_U(a, b) spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, a, b)
#define SHR_U(a, n) spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, a, UINT(n))
#define BYTE_OF(h, shift) MUL_F(TO_F(AND_U(SHR_U(h, shift), UINT(255))), FLOAT(1.f / 255.f))

/* As backgroundHash in kernels.inc. */
static SpvId backgroundHash(Spv* s, SpvId x, SpvId y, SpvId z) {
    SpvId h = XOR_U(XOR_U(MUL_U(x, UINT(73856093)), MUL_U(y, UINT(19349663))), MUL_U(z, UINT(83492791)));
    h = MUL_U(XOR_U(h, SHR_U(h, 15)), UINT(0x2C1B3C6D));
    h = MUL_U(XOR_U(h, SHR_U(h, 12)), UINT(0x297A2D39));
    return XOR_U(h, SHR_U(h, 15));
}

/* Reads the push constant that says which level this pass makes. */
typedef struct BloomPass { SpvId global, push, uvec3; } BloomPass;

static void bloomDeclare(Spv* s, BloomPass* p) {
    p->uvec3 = spvNewId(s);
    spvEmit(&s->types, 23, SPV_WORDS(p->uvec3, s->uintType, 3));
    p->global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, p->uvec3), SPV_STORAGE_Input);
    spvDecorate(s, p->global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);
    SpvId pushBlock = spvTypeStruct(s, &s->uintType, 1);
    spvDecorate(s, pushBlock, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, pushBlock, 0, SPV_DECORATE_Offset, 0);
    p->push = spvVariable(s, spvTypePointer(s, SPV_STORAGE_PushConstant, pushBlock), SPV_STORAGE_PushConstant);
}

/* The bilinear sample of a level at a place given in its texels. */
static SpvId bloomTap(Spv* s, const BackgroundResources* r, SpvId start, SpvId width, SpvId height, SpvId sx, SpvId sy) {
    SpvId baseX = FLOOR_F(sx), baseY = FLOOR_F(sy), partX = SUB_F(sx, baseX), partY = SUB_F(sy, baseY);
    SpvId lastX = SUB_F(TO_F(width), FLOAT(1.f)), lastY = SUB_F(TO_F(height), FLOAT(1.f));
#define CLAMP_TO(v, last) TO_U(MIN_F(MAX_F(v, FLOAT(0.f)), last))
    SpvId x0 = CLAMP_TO(baseX, lastX), x1 = CLAMP_TO(ADD_F(baseX, FLOAT(1.f)), lastX);
    SpvId y0 = MUL_U(CLAMP_TO(baseY, lastY), width), y1 = MUL_U(CLAMP_TO(ADD_F(baseY, FLOAT(1.f)), lastY), width);
#undef CLAMP_TO
#define TEXEL(y, x) spvShuffle3(s, spvLoad(s, s->vec4, pixelPtr(s, r, r->bloom, ADD_U(start, ADD_U(y, x)))))
    SpvId a = TEXEL(y0, x0), b = TEXEL(y0, x1), c = TEXEL(y1, x0), d = TEXEL(y1, x1);
#undef TEXEL
    SpvId top = ADD_V3(a, SCALE_V3(SUB_V3(b, a), partX)), bottom = ADD_V3(c, SCALE_V3(SUB_V3(d, c), partX));
    return ADD_V3(top, SCALE_V3(SUB_V3(bottom, top), partY));
}

/* A level of the bloom, as bloomDownRow in software.c: from the frame's light, or the level before. */
uint32_t* artfShaderBloomDown(size_t* size, bool fromLight) {
    Spv b, *s = &b;
    spvInit(s);
    BackgroundResources r;
    backgroundDeclare(s, &r, true);
    BloomPass p;
    bloomDeclare(s, &p);

    SpvId main = spvBeginMain(s);
    SpvId i = spvExtract(s, s->uintType, spvLoad(s, p.uvec3, p.global), 0);
    SpvId level = spvLoad(s, s->uintType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_PushConstant, s->uintType), p.push, &r.zero, 1));
    SpvId row = backgroundVecAt(s, &r, ADD_U(UINT(B_BLOOM), level));
    SpvId start = AS_UINT(X(row)), width = AS_UINT(Y(row)), height = AS_UINT(Z(row));
    SpvId merge;
    spvIfBegin(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, i, MUL_U(width, height)), &merge);
    SpvId x = spvOp2(s, SPV_OP_UMod, s->uintType, i, width), y = spvOp2(s, SPV_OP_UDiv, s->uintType, i, width);
    SpvId from = fromLight ? backgroundVec(s, &r, B_SIZE) : backgroundVecAt(s, &r, ADD_U(UINT(B_BLOOM - 1), level));
    SpvId fromStart = fromLight ? UINT(0) : AS_UINT(X(from));
    SpvId fromWidth = fromLight ? AS_UINT(X(from)) : AS_UINT(Y(from)), fromHeight = fromLight ? AS_UINT(Y(from)) : AS_UINT(Z(from));
    SpvId sum = 0;
    if (fromLight) {
        SpvId lastX = SUB_U(fromWidth, UINT(1)), lastY = SUB_U(fromHeight, UINT(1));
        SpvId x0 = MUL_U(x, UINT(2)), y0 = MUL_U(y, UINT(2));
        SpvId x1 = UMIN(ADD_U(x0, UINT(1)), lastX), y1 = UMIN(ADD_U(y0, UINT(1)), lastY);
        x0 = UMIN(x0, lastX);
        y0 = UMIN(y0, lastY);
        const SpvId xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
        SpvId ceiling = vec3Of(s, FLOAT(ARTF_BLOOM_CEILING), FLOAT(ARTF_BLOOM_CEILING), FLOAT(ARTF_BLOOM_CEILING));
        for (int k = 0; k < 4; ++k) {
            SpvId texel = spvShuffle3(s, spvLoad(s, s->vec4, pixelPtr(s, &r, r.hdr, ADD_U(MUL_U(ys[k], fromWidth), xs[k]))));
            /* (as bloomDownRow: the light past the threshold) */
            texel = spvGlsl2(s, SPV_GLSL_FMax, s->vec3, SUB_V3(texel, vec3Of(s, FLOAT(ARTF_BLOOM_THRESHOLD), FLOAT(ARTF_BLOOM_THRESHOLD), FLOAT(ARTF_BLOOM_THRESHOLD))), vec3Of(s, FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)));
            texel = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, texel, ceiling);
            sum = k ? ADD_V3(sum, texel) : texel;
        }
    } else {
        SpvId cx = ADD_F(MUL_F(TO_F(x), FLOAT(2.f)), FLOAT(.5f)), cy = ADD_F(MUL_F(TO_F(y), FLOAT(2.f)), FLOAT(.5f));
        for (int k = 0; k < 4; ++k) {
            SpvId tap = bloomTap(s, &r, fromStart, fromWidth, fromHeight, ADD_F(cx, FLOAT(k & 1 ? 1.f : -1.f)), ADD_F(cy, FLOAT(k & 2 ? 1.f : -1.f)));
            sum = k ? ADD_V3(sum, tap) : tap;
        }
    }
    spvStore(s, pixelPtr(s, &r, r.bloom, ADD_U(start, i)), vec4Of(s, SCALE_V3(sum, FLOAT(.25f)), FLOAT(0.f)));
    spvIfEnd(s, merge);
    spvEndMain(s);

    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &p.global, 1);
    spvExecutionMode(s, main, 17, 64, 1, 1, 3);
    return finish(s, size);
}

/* Adds the level above to this one, spread by four taps. */
uint32_t* artfShaderBloomUp(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    BackgroundResources r;
    backgroundDeclare(s, &r, true);
    BloomPass p;
    bloomDeclare(s, &p);

    SpvId main = spvBeginMain(s);
    SpvId i = spvExtract(s, s->uintType, spvLoad(s, p.uvec3, p.global), 0);
    SpvId level = spvLoad(s, s->uintType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_PushConstant, s->uintType), p.push, &r.zero, 1));
    SpvId row = backgroundVecAt(s, &r, ADD_U(UINT(B_BLOOM), level));
    SpvId start = AS_UINT(X(row)), width = AS_UINT(Y(row)), height = AS_UINT(Z(row));
    SpvId merge;
    spvIfBegin(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, i, MUL_U(width, height)), &merge);
    SpvId x = spvOp2(s, SPV_OP_UMod, s->uintType, i, width), y = spvOp2(s, SPV_OP_UDiv, s->uintType, i, width);
    SpvId above = backgroundVecAt(s, &r, ADD_U(UINT(B_BLOOM + 1), level));
    SpvId aboveStart = AS_UINT(X(above)), aboveWidth = AS_UINT(Y(above)), aboveHeight = AS_UINT(Z(above));
    SpvId sx = SUB_F(MUL_F(ADD_F(TO_F(x), FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f)), sy = SUB_F(MUL_F(ADD_F(TO_F(y), FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f));
    SpvId sum = 0;
    for (int k = 0; k < 4; ++k) {
        SpvId tap = bloomTap(s, &r, aboveStart, aboveWidth, aboveHeight, ADD_F(sx, FLOAT(k & 1 ? .5f : -.5f)), ADD_F(sy, FLOAT(k & 2 ? .5f : -.5f)));
        sum = k ? ADD_V3(sum, tap) : tap;
    }
    SpvId here = pixelPtr(s, &r, r.bloom, ADD_U(start, i));
    SpvId mine = spvShuffle3(s, spvLoad(s, s->vec4, here));
    spvStore(s, here, vec4Of(s, ADD_V3(mine, SCALE_V3(sum, FLOAT(.25f * ARTF_BLOOM_FADE))), FLOAT(0.f)));
    spvIfEnd(s, merge);
    spvEndMain(s);

    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &p.global, 1);
    spvExecutionMode(s, main, 17, 64, 1, 1, 3);
    return finish(s, size);
}

/* The picture of all around, behind the scene: as environmentFill in kernels.inc. */
uint32_t* artfShaderEnvironmentBackdrop(size_t* size, bool hdr) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    declare(s, &r, false);
    SpvId fragCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
    spvDecorate(s, fragCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);

    SpvId main = spvBeginMain(s);
    SpvId coord = spvLoad(s, s->vec4, fragCoord);
    SpvId right = frameVec(s, &r, F_EYE), up = frameVec(s, &r, F_EYE + 1), forward = spvShuffle3(s, frameVec(s, &r, F_EYE + 2));
    /* (orthographic: every ray goes the way the view looks, and sees the same) */
    SpvId spread = spvSelect(s, s->floatType, orthoOn(s, &r), FLOAT(0.f), FLOAT(1.f));
    SpvId px = MUL_F(SUB_F(MUL_F(DIV_F(X(coord), W(right)), FLOAT(2.f)), FLOAT(1.f)), spread);
    SpvId py = MUL_F(SUB_F(FLOAT(1.f), MUL_F(DIV_F(Y(coord), W(up)), FLOAT(2.f))), spread);
    SpvId d = ADD_V3(ADD_V3(forward, SCALE_V3(spvShuffle3(s, right), px)), SCALE_V3(spvShuffle3(s, up), py));
    SpvId light = SCALE_V3(envSample(s, &r, UINT(0), d), W(frameVec(s, &r, F_SKY)));
    if (hdr) {
        SpvId a = W(frameVec(s, &r, 14));
        spvStore(s, outColor, vec4Of(s, SCALE_V3(light, a), a));
        spvEndMain(s);
        const SpvId io[2] = {fragCoord, outColor};
        spvEntryPoint(s, SPV_MODEL_Fragment, main, io, 2);
        spvExecutionMode(s, main, 7, 0, 0, 0, 0);
        return finish(s, size);
    }
    light = exposed(s, light, X(frameVec(s, &r, F_LOOK)));
    SpvId shown = srgbEncode3(s, spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, light, same3(s, 0.f)), same3(s, 1.f)));
    SpvId alpha = W(frameVec(s, &r, 14));
    spvStore(s, outColor, vec4Of(s, SCALE_V3(shown, alpha), alpha));
    spvEndMain(s);

    const SpvId interface[2] = {fragCoord, outColor};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, interface, 2);
    spvExecutionMode(s, main, 7, 0, 0, 0, 0);
    return finish(s, size);
}

/* Fog that light goes through, over what is behind the scene: the grid's farthest slice. It blends as
 * light * left + added * alpha, as fogBackdrop in kernels.inc. */
uint32_t* artfShaderFogBackdrop(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    declare(s, &r, false);
    SpvId froxels = spvResource(s, blockOf(s, r.vec4Array, false), SPV_STORAGE_StorageBuffer, 56);
    SpvId fragCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
    spvDecorate(s, fragCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);
    SpvId main = spvBeginMain(s);
    SpvId coord = spvLoad(s, s->vec4, fragCoord);
    spvStore(s, outColor, froxelAt(s, &r, froxels, SUB_F(X(coord), FLOAT(.5f)), SUB_F(Y(coord), FLOAT(.5f)), FLOAT(3e38f)));
    spvEndMain(s);
    const SpvId io[2] = {fragCoord, outColor};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, io, 2);
    spvExecutionMode(s, main, 7, 0, 0, 0, 0);
    return finish(s, size);
}

/* The frame's background: the light and its glow, tone mapped, under whatever the scene draws. */
uint32_t* artfShaderBackgroundBackdrop(size_t* size, bool hdr) {
    Spv b, *s = &b;
    spvInit(s);
    BackgroundResources r;
    backgroundDeclare(s, &r, false);
    SpvId fragCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
    spvDecorate(s, fragCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);

    SpvId main = spvBeginMain(s);
    SpvId coord = spvLoad(s, s->vec4, fragCoord);
    SpvId fx = X(coord), fy = Y(coord); /* (pixel centers: x + .5) */
    SpvId x = TO_U(FLOOR_F(fx)), y = TO_U(FLOOR_F(fy));
    SpvId sizes = backgroundVec(s, &r, B_SIZE), image = backgroundVec(s, &r, B_IMAGE), level = backgroundVec(s, &r, B_BLOOM);
    SpvId light = spvShuffle3(s, spvLoad(s, s->vec4, pixelPtr(s, &r, r.hdr, ADD_U(MUL_U(y, AS_UINT(X(sizes))), x))));
    SpvId sx = SUB_F(MUL_F(fx, FLOAT(.5f)), FLOAT(.5f)), sy = SUB_F(MUL_F(fy, FLOAT(.5f)), FLOAT(.5f));
    SpvId glow = bloomTap(s, &r, AS_UINT(X(level)), AS_UINT(Y(level)), AS_UINT(Z(level)), sx, sy);
    /* (as backgroundFill: a lens's glow is nearly white) */
    SpvId grey = ADD_F(ADD_F(MUL_F(X(glow), FLOAT(.3f)), MUL_F(Y(glow), FLOAT(.59f))), MUL_F(Z(glow), FLOAT(.11f)));
    glow = ADD_V3(glow, SCALE_V3(SUB_V3(vec3Of(s, grey, grey, grey), glow), FLOAT(ARTF_BLOOM_WHITE)));
    SpvId all = SCALE_V3(ADD_V3(light, SCALE_V3(glow, MUL_F(Y(image), FLOAT(1.f - ARTF_BLOOM_FADE)))), X(image));

    /* as backgroundToneMap */
    SpvId vec3Const[5];
    const float numbers[5] = {2.51f, .03f, 2.43f, .59f, .14f};
    for (int i = 0; i < 5; ++i) vec3Const[i] = vec3Of(s, FLOAT(numbers[i]), FLOAT(numbers[i]), FLOAT(numbers[i]));
    SpvId top = MUL_V3(all, ADD_V3(MUL_V3(all, vec3Const[0]), vec3Const[1]));
    SpvId bottom = ADD_V3(MUL_V3(all, ADD_V3(MUL_V3(all, vec3Const[2]), vec3Const[3])), vec3Const[4]);
    SpvId mapped = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvOp2(s, SPV_OP_FDiv, s->vec3, top, bottom), vec3Of(s, FLOAT(1.f), FLOAT(1.f), FLOAT(1.f)));
    SpvId shown = srgbEncode3(s, mapped);

    if (hdr) {
        /* (its own exposure and tone mapping, as the CPU's: back to light for what comes after) */
        spvStore(s, outColor, vec4Of(s, srgbDecode3(s, shown), FLOAT(1.f)));
        spvEndMain(s);
        const SpvId io[2] = {fragCoord, outColor};
        spvEntryPoint(s, SPV_MODEL_Fragment, main, io, 2);
        spvExecutionMode(s, main, 7, 0, 0, 0, 0);
        return finish(s, size);
    }
    SpvId noise = backgroundHash(s, ADD_U(x, MUL_U(y, UINT(8192))), UINT(7), UINT(13));
    SpvId dither = MUL_F(SUB_F(BYTE_OF(noise, 0), FLOAT(.5f)), FLOAT(1.f / 255.f));
    spvStore(s, outColor, vec4Of(s, ADD_V3(shown, vec3Of(s, dither, dither, dither)), FLOAT(1.f)));
    spvEndMain(s);

    const SpvId interface[2] = {fragCoord, outColor};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, interface, 2);
    spvExecutionMode(s, main, 7, 0, 0, 0, 0);
    return finish(s, size);
}

/* ---- tracing: the same arithmetic as trace.c ----
 *
 * Bindings: 17 the meshes' trees, 18 triangles, 19 vertices, 20 the tree over the instances, 21 instances,
 * 22 what each pixel gathered, 23 ArtfTraceFrame. A shader here has no functions: what is called twice in
 * trace.c is written out twice. */

enum { T_EYE = 0, T_RIGHT = 1, T_UP = 2, T_FORWARD = 3, T_SIZE = 4, T_PATH = 5, T_PREVIOUS = 6, T_ROWS = 10 };

typedef struct Loop { SpvId header, test, body, next, done; } Loop;

static void loopOpen(Spv* s, Loop* l) {
    l->header = spvNewId(s);
    l->test = spvNewId(s);
    l->body = spvNewId(s);
    l->next = spvNewId(s);
    l->done = spvNewId(s);
    spvBranch(s, l->header);
    spvLabel(s, l->header);
    spvLoopMerge(s, l->done, l->next);
    spvBranch(s, l->test);
    spvLabel(s, l->test);
}
static void loopWhile(Spv* s, Loop* l, SpvId condition) {
    spvBranchIf(s, condition, l->body, l->done);
    spvLabel(s, l->body);
}
static void loopClose(Spv* s, Loop* l) {
    spvBranch(s, l->next);
    spvLabel(s, l->next);
    spvBranch(s, l->header);
    spvLabel(s, l->done);
}
static void breakIf(Spv* s, Loop* l, SpvId condition) {
    SpvId merge;
    spvIfBegin(s, condition, &merge);
    spvBranch(s, l->done);
    spvLabel(s, merge);
}

typedef struct Tracer {
    Spv* s;
    Resources r;
    SpvId nodes, triangles, vertices, top, instances, sum, frame;
    SpvId textures, sampledType;
    SpvId ptrFnUint;
    /* variables: all at the top of main, as SPIR-V wants */
    SpvId stack, depth, at, pop, stack2, depth2, at2, pop2, leaf, leaf2;
    SpvId bestT, bestU, bestV, bestInstance, bestTriangle;
    SpvId state, origin, direction, path, light, covered, bounces, step, sampleIndex, choice, diffuseLikely, bounceDir, keptVar, shineVar, towardVar, likelyVar, coneWidth, coneSpread;
    SpvId shadowOrigin, through, limit, shadowStep, lightIndex, ballStep, ball, travelled, keep;
    GraphVars own;
    const ArtfGraphData* graphs;
    uint32_t graphCount;
    bool guides;
    SpvId guide, albedoGuide, shiftGuide;
    SpvId guideBits, guidePlace, guideColor, guideInstance, guideShift; /* what a pixel's first surface was */
    bool hardware;                 /* rays through the ray tracing hardware */
    bool more;                     /* materials past glTF's core, lights with a shape or a profile: what reads them */
    SpvId extraLights;             /* the lights' buffer, for their profiles */
    SpvId alongVar, acrossVar;     /* where on a shape this path looks */
    SpvId accel, accelType, query; /* the top level, its type; the ray query, a variable of main */
    /* rays while drawing: artfTracedBall, a table of the module's; what a pixel's rays to a light add up to, the ray */
    SpvId ballTable, tracedSum, tracedRay;
} Tracer;

#define IS_EQ(a, b) spvOp2(s, 170 /* OpIEqual */, s->boolType, a, b)
#define NOT_EQ(a, b) spvOp2(s, SPV_OP_INotEqual, s->boolType, a, b)
#define LESS_U(a, b) spvOp2(s, SPV_OP_ULessThan, s->boolType, a, b)
#define EITHER(a, b) spvOp2(s, 166 /* OpLogicalOr */, s->boolType, a, b)
#define NOT(a) spvOp1(s, SPV_OP_LogicalNot, s->boolType, a)
#define LOAD_U(v) spvLoad(s, s->uintType, v)
#define LOAD_F(v) spvLoad(s, s->floatType, v)
#define LOAD_V3(v) spvLoad(s, s->vec3, v)
#define VAR(pointer) spvVariable(s, pointer, SPV_STORAGE_Function)
#define FAR_AWAY 3e38f

static SpvId traceFrameVec(const Tracer* t, uint32_t i) {
    Spv* s = t->s;
    const SpvId idx[2] = {t->r.zero, UINT(i)};
    return spvLoad(s, s->vec4, spvAccess(s, t->r.ptrUniformVec4, t->frame, idx, 2));
}

static SpvId stackAt(const Tracer* t, SpvId stack, SpvId i) { return spvAccess(t->s, t->ptrFnUint, stack, &i, 1); }

static SpvId inverseOf(Spv* s, SpvId d) {
    SpvId c[3] = {X(d), Y(d), Z(d)};
    for (int k = 0; k < 3; ++k) c[k] = DIV_F(FLOAT(1.f), PICK_F(MORE_F(ABS_F(c[k]), FLOAT(1e-20f)), c[k], FLOAT(1e-20f)));
    return vec3Of(s, c[0], c[1], c[2]);
}

/* As boxEnter. */
static SpvId boxEnter(Spv* s, SpvId lo, SpvId hi, SpvId o, SpvId inv, SpvId limit) {
    SpvId a = MUL_V3(SUB_V3(spvShuffle3(s, lo), o), inv), b = MUL_V3(SUB_V3(spvShuffle3(s, hi), o), inv);
    SpvId near_ = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, a, b), far = spvGlsl2(s, SPV_GLSL_FMax, s->vec3, a, b);
    SpvId entry = MAX_F(FLOAT(0.f), MAX_F(X(near_), MAX_F(Y(near_), Z(near_))));
    SpvId exit_ = MIN_F(limit, MIN_F(X(far), MIN_F(Y(far), Z(far))));
    return PICK_F(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, entry, exit_), entry, FLOAT(FAR_AWAY));
}

/* An inner node: the nearer child next, the other for later. */
static void visitChildren(const Tracer* t, SpvId buffer, SpvId first, SpvId o, SpvId inv, SpvId stack, SpvId depth, SpvId at, SpvId pop) {
    Spv* s = t->s;
    SpvId limit = LOAD_F(t->bestT);
    SpvId left = MUL_U(first, UINT(2)), right = ADD_U(left, UINT(2));
    SpvId l = boxEnter(s, bufferVec(s, &t->r, buffer, left), bufferVec(s, &t->r, buffer, ADD_U(left, UINT(1))), o, inv, limit);
    SpvId r = boxEnter(s, bufferVec(s, &t->r, buffer, right), bufferVec(s, &t->r, buffer, ADD_U(right, UINT(1))), o, inv, limit);
    SpvId leftFirst = spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, l, r), second = ADD_U(first, UINT(1));
    SpvId near_ = spvSelect(s, s->uintType, leftFirst, first, second), far = spvSelect(s, s->uintType, leftFirst, second, first);
    SpvId merge;
    spvIfBegin(s, LESS_F(MAX_F(l, r), FLOAT(FAR_AWAY)), &merge);
    SpvId top = LOAD_U(depth);
    spvStore(s, stackAt(t, stack, top), far);
    spvStore(s, depth, ADD_U(top, UINT(1)));
    spvIfEnd(s, merge);
    SpvId going = LESS_F(MIN_F(l, r), FLOAT(FAR_AWAY));
    spvIfBegin(s, going, &merge);
    spvStore(s, at, near_);
    spvStore(s, pop, UINT(0));
    spvIfEnd(s, merge);
}

/* As the end of each turn of the loops: the next node, or out. */
static void popNode(const Tracer* t, Loop* l, SpvId stack, SpvId depth, SpvId at, SpvId pop) {
    Spv* s = t->s;
    SpvId popping = NOT_EQ(LOAD_U(pop), UINT(0));
    breakIf(s, l, BOTH(popping, IS_EQ(LOAD_U(depth), UINT(0))));
    SpvId merge;
    spvIfBegin(s, popping, &merge);
    SpvId top = SUB_U(LOAD_U(depth), UINT(1));
    spvStore(s, depth, top);
    spvStore(s, at, LOAD_U(stackAt(t, stack, top)));
    spvIfEnd(s, merge);
}

static SpvId vertexVec(const Tracer* t, SpvId index, uint32_t row) { Spv* s = t->s; return bufferVec(s, &t->r, t->vertices, ADD_U(MUL_U(index, UINT(3)), UINT(row))); }

/* As hitMesh. */
static void hitMesh(const Tracer* t, SpvId instance, SpvId root, SpvId twoSided, SpvId o, SpvId d) {
    Spv* s = t->s;
    SpvId inv = inverseOf(s, d);
    spvStore(s, t->depth2, UINT(0));
    spvStore(s, t->at2, root);
    Loop nodes;
    loopOpen(s, &nodes);
    loopWhile(s, &nodes, spvConstBool(s, true));
    SpvId base = MUL_U(LOAD_U(t->at2), UINT(2));
    SpvId first = AS_UINT(W(bufferVec(s, &t->r, t->nodes, base))), count = AS_UINT(W(bufferVec(s, &t->r, t->nodes, ADD_U(base, UINT(1)))));
    spvStore(s, t->pop2, UINT(1));
    SpvId isLeaf = NOT_EQ(count, UINT(0)), merge;
    spvIfBegin(s, isLeaf, &merge);
    {
        spvStore(s, t->leaf2, first);
        Loop tris;
        SpvId end = ADD_U(first, count);
        loopOpen(s, &tris);
        loopWhile(s, &tris, LESS_U(LOAD_U(t->leaf2), end));
        SpvId i = LOAD_U(t->leaf2);
        SpvId tri = bufferVec(s, &t->r, t->triangles, i);
        SpvId a = spvShuffle3(s, vertexVec(t, AS_UINT(X(tri)), 0)), b = spvShuffle3(s, vertexVec(t, AS_UINT(Y(tri)), 0)), c = spvShuffle3(s, vertexVec(t, AS_UINT(Z(tri)), 0));
        SpvId e1 = SUB_V3(b, a), e2 = SUB_V3(c, a);
        SpvId p = spvGlsl2(s, SPV_GLSL_Cross, s->vec3, d, e2);
        SpvId det = DOT3(e1, p), id = DIV_F(FLOAT(1.f), det);
        SpvId from = SUB_V3(o, a);
        SpvId u = MUL_F(DOT3(from, p), id);
        SpvId q = spvGlsl2(s, SPV_GLSL_Cross, s->vec3, from, e1);
        SpvId v = MUL_F(DOT3(d, q), id), dist = MUL_F(DOT3(e2, q), id);
        SpvId faces = EITHER(MORE_F(det, FLOAT(0.f)), BOTH(twoSided, LESS_F(det, FLOAT(0.f))));
        SpvId inside = BOTH(BOTH(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, u, FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, v, FLOAT(0.f))), spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, ADD_F(u, v), FLOAT(1.f)));
        SpvId nearer = BOTH(MORE_F(dist, FLOAT(0.f)), LESS_F(dist, LOAD_F(t->bestT)));
        SpvId took;
        spvIfBegin(s, BOTH(faces, BOTH(inside, nearer)), &took);
        spvStore(s, t->bestT, dist);
        spvStore(s, t->bestU, u);
        spvStore(s, t->bestV, v);
        spvStore(s, t->bestInstance, instance);
        spvStore(s, t->bestTriangle, i);
        spvIfEnd(s, took);
        spvStore(s, t->leaf2, ADD_U(i, UINT(1)));
        loopClose(s, &tris);
    }
    spvIfEnd(s, merge);
    spvIfBegin(s, NOT(isLeaf), &merge);
    visitChildren(t, t->nodes, first, o, inv, t->stack2, t->depth2, t->at2, t->pop2);
    spvIfEnd(s, merge);
    popNode(t, &nodes, t->stack2, t->depth2, t->at2, t->pop2);
    loopClose(s, &nodes);
}

/* As closest, by the ray tracing hardware: a ray query through the top level, all of it taken as opaque (what shows
 * through is the path's business, as with the trees). An instance's id is its place among the traced ones, which the
 * table of instances gives its mesh's first triangle by; fronts are counter-clockwise, as det > 0 has them, and backs
 * are culled for what has one side unless bothSides. */
static void closestHardware(const Tracer* t, SpvId o, SpvId d, SpvId limit, bool bothSides) {
    Spv* s = t->s;
    SpvId accel = spvLoad(s, t->accelType, t->accel);
    spvEmit(&s->code, 4473 /* OpRayQueryInitializeKHR */, SPV_WORDS(t->query, accel, UINT(bothSides ? 1u : 1u | 0x10u /* opaque, cull backs */), UINT(0xFF), o, FLOAT(0.f), d, limit));
    Loop proceed;
    loopOpen(s, &proceed);
    SpvId more = spvNewId(s);
    spvEmit(&s->code, 4477 /* OpRayQueryProceedKHR */, SPV_WORDS(s->boolType, more, t->query));
    loopWhile(s, &proceed, more);
    loopClose(s, &proceed);
    SpvId committed = spvConstInt(s, 1);
    SpvId kind = spvNewId(s);
    spvEmit(&s->code, 4479 /* OpRayQueryGetIntersectionTypeKHR */, SPV_WORDS(s->uintType, kind, t->query, committed));
    SpvId hitT = spvNewId(s), instance = spvNewId(s), primitive = spvNewId(s), bary = spvNewId(s);
    spvEmit(&s->code, 6018 /* OpRayQueryGetIntersectionTKHR */, SPV_WORDS(s->floatType, hitT, t->query, committed));
    spvEmit(&s->code, 6020 /* OpRayQueryGetIntersectionInstanceIdKHR */, SPV_WORDS(s->intType, instance, t->query, committed));
    spvEmit(&s->code, 6023 /* OpRayQueryGetIntersectionPrimitiveIndexKHR */, SPV_WORDS(s->intType, primitive, t->query, committed));
    spvEmit(&s->code, 6024 /* OpRayQueryGetIntersectionBarycentricsKHR */, SPV_WORDS(s->vec2, bary, t->query, committed));
    SpvId hit = NOT_EQ(kind, UINT(0));
    spvStore(s, t->bestT, PICK_F(hit, hitT, limit));
    spvStore(s, t->bestU, spvExtract(s, s->floatType, bary, 0));
    spvStore(s, t->bestV, spvExtract(s, s->floatType, bary, 1));
    spvStore(s, t->bestInstance, spvSelect(s, s->uintType, hit, spvOp1(s, SPV_OP_Bitcast, s->uintType, instance), UINT(0xFFFFFFFFu)));
    /* (the mesh's first triangle from the instance's table, past the custom index's 24 bits: bit 0 is its sides) */
    SpvId meta = bufferVec(s, &t->r, t->instances, ADD_U(MUL_U(spvSelect(s, s->uintType, hit, spvOp1(s, SPV_OP_Bitcast, s->uintType, instance), UINT(0)), UINT(4)), UINT(3)));
    spvStore(s, t->bestTriangle, ADD_U(SHR_U(AS_UINT(W(meta)), 1), spvOp1(s, SPV_OP_Bitcast, s->uintType, primitive)));
}

/* As closest: leaves what it found in bestT and the others. */
static void closest(const Tracer* t, SpvId o, SpvId d, SpvId limit, bool bothSides) {
    Spv* s = t->s;
    if (t->hardware) {
        closestHardware(t, o, d, limit, bothSides);
        return;
    }
    SpvId inv = inverseOf(s, d);
    spvStore(s, t->bestT, limit);
    spvStore(s, t->bestInstance, UINT(0xFFFFFFFFu));
    spvStore(s, t->depth, UINT(0));
    spvStore(s, t->at, UINT(0));
    SpvId rootEntry = boxEnter(s, bufferVec(s, &t->r, t->top, UINT(0)), bufferVec(s, &t->r, t->top, UINT(1)), o, inv, limit), reached;
    spvIfBegin(s, LESS_F(rootEntry, FLOAT(FAR_AWAY)), &reached);
    Loop nodes;
    loopOpen(s, &nodes);
    loopWhile(s, &nodes, spvConstBool(s, true));
    SpvId base = MUL_U(LOAD_U(t->at), UINT(2));
    SpvId first = AS_UINT(W(bufferVec(s, &t->r, t->top, base))), count = AS_UINT(W(bufferVec(s, &t->r, t->top, ADD_U(base, UINT(1)))));
    spvStore(s, t->pop, UINT(1));
    SpvId isLeaf = NOT_EQ(count, UINT(0)), merge;
    spvIfBegin(s, isLeaf, &merge);
    {
        spvStore(s, t->leaf, first);
        Loop each;
        SpvId end = ADD_U(first, count);
        loopOpen(s, &each);
        loopWhile(s, &each, LESS_U(LOAD_U(t->leaf), end));
        SpvId i = LOAD_U(t->leaf), row = MUL_U(i, UINT(4));
        SpvId k0 = bufferVec(s, &t->r, t->instances, row), k1 = bufferVec(s, &t->r, t->instances, ADD_U(row, UINT(1))), k2 = bufferVec(s, &t->r, t->instances, ADD_U(row, UINT(2))), meta = bufferVec(s, &t->r, t->instances, ADD_U(row, UINT(3)));
        SpvId o4 = vec4Of(s, o, FLOAT(1.f));
        SpvId oo = vec3Of(s, DOT4(k0, o4), DOT4(k1, o4), DOT4(k2, o4));
        SpvId od = vec3Of(s, DOT3(spvShuffle3(s, k0), d), DOT3(spvShuffle3(s, k1), d), DOT3(spvShuffle3(s, k2), d));
        hitMesh(t, i, AS_UINT(X(meta)), bothSides ? spvConstBool(s, true) : NOT_EQ(AND_U(AS_UINT(W(meta)), UINT(1)), UINT(0)), oo, od);
        spvStore(s, t->leaf, ADD_U(i, UINT(1)));
        loopClose(s, &each);
    }
    spvIfEnd(s, merge);
    spvIfBegin(s, NOT(isLeaf), &merge);
    visitChildren(t, t->top, first, o, inv, t->stack, t->depth, t->at, t->pop);
    spvIfEnd(s, merge);
    popNode(t, &nodes, t->stack, t->depth, t->at, t->pop);
    loopClose(s, &nodes);
    spvIfEnd(s, reached);
}

/* What a ray met, as trace.c's Surface: past glTF's core, what it has (ARTF_MORE_), the coat's normal, 1 / sheen's alpha, the
 * color of what comes from under the surface and 1 / how far it goes, by log2, and what its inside takes. */
typedef struct Met {
    SpvId normal, albedo, alpha, emissive, front, ior, shiny, rough, open, mirror, f0;
    Reflecting reflecting;
    SpvId coat, under, sheen;
    Reflecting coating;
    SpvId flags, coatNormal, sheenInverse, subsurface, reach, absorb;
} Met;

static SpvId maxOf3(Spv* s, SpvId v) { return MAX_F(X(v), MAX_F(Y(v), Z(v))); }

/* A texture of the array, in its largest mipmap. */
/* trace.c's texelCone: the level by the cone (lodBase), from the texture's size as the GPU has it (its first held
 * level is level 0 here), between two, by the sampler. */
static SpvId traceTexture(const Tracer* t, SpvId lodBase, SpvId index, SpvId uv) {
    Spv* s = t->s;
    spvDecorate(s, index, SPV_DECORATE_NonUniform, -1);
    SpvId slot = spvAccess(s, spvTypePointer(s, SPV_STORAGE_UniformConstant, t->sampledType), t->textures, &index, 1);
    spvDecorate(s, slot, SPV_DECORATE_NonUniform, -1);
    SpvId image = spvLoad(s, t->sampledType, slot);
    spvDecorate(s, image, SPV_DECORATE_NonUniform, -1);
    SpvId imageType;
    spvTypeSampledImage2D(s, &imageType);
    SpvId plain = spvOp1(s, 100 /* OpImage */, imageType, image);
    spvDecorate(s, plain, SPV_DECORATE_NonUniform, -1);
    SpvId size = spvOp2(s, 103 /* OpImageQuerySizeLod */, spvTypeVector(s, s->intType, 2), plain, spvConstInt(s, 0));
    SpvId levels = spvOp1(s, 106 /* OpImageQueryLevels */, s->intType, plain);
    /* (half the log2 of its texels, by their sides' highest bits: trace.c's) */
    SpvId wideBits = spvGlsl1(s, SPV_GLSL_FindUMsb, s->uintType, spvOp1(s, SPV_OP_Bitcast, s->uintType, spvExtract(s, s->intType, size, 0)));
    SpvId highBits = spvGlsl1(s, SPV_GLSL_FindUMsb, s->uintType, spvOp1(s, SPV_OP_Bitcast, s->uintType, spvExtract(s, s->intType, size, 1)));
    SpvId bits = spvOp1(s, SPV_OP_ConvertUToF, s->floatType, spvOp2(s, SPV_OP_IAdd, s->uintType, wideBits, highBits));
    SpvId most = SUB_F(spvOp1(s, 111 /* OpConvertSToF */, s->floatType, levels), FLOAT(1.f));
    SpvId at = MIN_F(MAX_F(ADD_F(lodBase, MUL_F(FLOAT(.5f), bits)), FLOAT(0.f)), most);
    return spvImageSampleLod(s, image, uv, at);
}

/* As surfaceAt, for what closest left. */
/* cone: how wide the ray's cone is where it met the surface (trace.c's surfaceAt); 0, a ray. */
static Met surfaceAt(const Tracer* t, SpvId origin, SpvId d, SpvId cone) {
    Spv* s = t->s;
    SpvId instance = LOAD_U(t->bestInstance), row = MUL_U(instance, UINT(4));
    SpvId k0 = spvShuffle3(s, bufferVec(s, &t->r, t->instances, row)), k1 = spvShuffle3(s, bufferVec(s, &t->r, t->instances, ADD_U(row, UINT(1)))), k2 = spvShuffle3(s, bufferVec(s, &t->r, t->instances, ADD_U(row, UINT(2))));
    SpvId meta = bufferVec(s, &t->r, t->instances, ADD_U(row, UINT(3)));
    SpvId tri = bufferVec(s, &t->r, t->triangles, LOAD_U(t->bestTriangle));
    SpvId wb = LOAD_F(t->bestU), wc = LOAD_F(t->bestV), wa = SUB_F(SUB_F(FLOAT(1.f), wb), wc);
    SpvId index[3] = {AS_UINT(X(tri)), AS_UINT(Y(tri)), AS_UINT(Z(tri))};
    const SpvId weight[3] = {wa, wb, wc};
    SpvId on = 0, tint = 0, alpha = 0, u = 0, v = 0, corner[3], cornerU[3], cornerV[3];
    for (int i = 0; i < 3; ++i) {
        SpvId a = vertexVec(t, index[i], 0), b = vertexVec(t, index[i], 1), c = vertexVec(t, index[i], 2);
        SpvId bgra = spvGlsl1(s, SPV_GLSL_UnpackUnorm4x8, s->vec4, AS_UINT(X(c)));
        SpvId rgb = vec3Of(s, Z(bgra), Y(bgra), X(bgra));
        SpvId n = SCALE_V3(spvShuffle3(s, b), weight[i]), color = SCALE_V3(srgbDecode3(s, rgb), weight[i]);
        SpvId share = MUL_F(W(bgra), weight[i]), su = MUL_F(W(a), weight[i]), sv = MUL_F(W(b), weight[i]);
        corner[i] = spvShuffle3(s, a);
        cornerU[i] = W(a);
        cornerV[i] = W(b);
        on = i ? ADD_V3(on, n) : n;
        tint = i ? ADD_V3(tint, color) : color;
        alpha = i ? ADD_F(alpha, share) : share;
        u = i ? ADD_F(u, su) : su;
        v = i ? ADD_F(v, sv) : sv;
    }
    SpvId material = MUL_U(AS_UINT(Y(meta)), UINT(ARTF_MATERIAL_ROWS));
    SpvId m0 = bufferVec(s, &t->r, t->r.materials, material), m1 = bufferVec(s, &t->r, t->r.materials, ADD_U(material, UINT(1))), m2 = bufferVec(s, &t->r, t->r.materials, ADD_U(material, UINT(2)));
    SpvId m3 = bufferVec(s, &t->r, t->r.materials, ADD_U(material, UINT(3))), m4 = bufferVec(s, &t->r, t->r.materials, ADD_U(material, UINT(4)));
    const SpvId uvParts[2] = {u, v};
    SpvId uv = spvCompose(s, s->vec2, uvParts, 2);
    /* how many of a texture's texels the cone covers here (trace.c's) */
    SpvId lodBase;
    {
        SpvId e1 = SUB_V3(corner[1], corner[0]), e2 = SUB_V3(corner[2], corner[0]), cr = G_CROSS(e1, e2);
        SpvId w = ADD_V3(ADD_V3(SCALE_V3(k0, X(cr)), SCALE_V3(k1, Y(cr))), SCALE_V3(k2, Z(cr)));
        SpvId det = DOT3(k0, G_CROSS(k1, k2));
        SpvId wl = spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, DOT3(w, w)), world = DIV_F(wl, MAX_F(G_ABS(det), FLOAT(1e-30f)));
        SpvId area = G_ABS(SUB_F(MUL_F(SUB_F(cornerU[1], cornerU[0]), SUB_F(cornerV[2], cornerV[0])), MUL_F(SUB_F(cornerU[2], cornerU[0]), SUB_F(cornerV[1], cornerV[0]))));
        SpvId facing = MAX_F(DIV_F(G_ABS(DOT3(w, d)), MAX_F(wl, FLOAT(1e-30f))), FLOAT(1e-3f));
        SpvId fine = ADD_F(MUL_F(FLOAT(.5f), log2Of(s, DIV_F(area, world))), log2Of(s, DIV_F(cone, facing)));
        SpvId usable = BOTH(BOTH(MORE_F(cone, FLOAT(0.f)), MORE_F(area, FLOAT(0.f))), MORE_F(world, FLOAT(0.f)));
        lodBase = PICK_F(usable, fine, FLOAT(-100.f));
    }
    SpvId texel = traceTexture(t, lodBase, AS_UINT(X(m2)), uv), bumps = traceTexture(t, lodBase, AS_UINT(X(m4)), uv), surface = traceTexture(t, lodBase, AS_UINT(Y(m4)), uv), occlusion = traceTexture(t, lodBase, AS_UINT(Z(m4)), uv);
    SpvId rgbTexel = spvShuffle3(s, texel);

    /* where u and v go on the triangle, in the object: as in trace.c */
    SpvId du1 = SUB_F(cornerU[1], cornerU[0]), dv1 = SUB_F(cornerV[1], cornerV[0]), du2 = SUB_F(cornerU[2], cornerU[0]), dv2 = SUB_F(cornerV[2], cornerV[0]);
    SpvId e1 = SUB_V3(corner[1], corner[0]), e2 = SUB_V3(corner[2], corner[0]);
    SpvId det = SUB_F(MUL_F(du1, dv2), MUL_F(du2, dv1));
    SpvId orient = PICK_F(LESS_F(det, FLOAT(0.f)), FLOAT(-1.f), FLOAT(1.f));
    SpvId tangent = SCALE_V3(NORMALIZE(SUB_V3(SCALE_V3(e1, dv2), SCALE_V3(e2, dv1))), orient), bitangent = SCALE_V3(NORMALIZE(SUB_V3(SCALE_V3(e2, du1), SCALE_V3(e1, du2))), orient);
    SpvId bx = MUL_F(SUB_F(MUL_F(X(bumps), FLOAT(2.f)), FLOAT(1.f)), Z(m3)), by = MUL_F(SUB_F(FLOAT(1.f), MUL_F(Y(bumps), FLOAT(2.f))), Z(m3)), bz = SUB_F(MUL_F(Z(bumps), FLOAT(2.f)), FLOAT(1.f));
    SpvId bent = ADD_V3(ADD_V3(SCALE_V3(tangent, bx), SCALE_V3(bitangent, by)), SCALE_V3(on, bz));
    SpvId plainOn = on;
    on = pick3(s, NOT_EQ(AS_UINT(X(m4)), UINT(0)), bent, on);

    SpvId n = ADD_V3(ADD_V3(SCALE_V3(k0, X(on)), SCALE_V3(k1, Y(on))), SCALE_V3(k2, Z(on)));
    n = NORMALIZE(n);
    SpvId side = PICK_F(MORE_F(DOT3(n, d), FLOAT(0.f)), FLOAT(-1.f), FLOAT(1.f));

    SpvId flags = AS_UINT(W(m2));
    SpvId isMask = NOT_EQ(AND_U(flags, UINT(ARTF_MATERIAL_MASK)), UINT(0)), isBlend = NOT_EQ(AND_U(flags, UINT(ARTF_MATERIAL_BLEND)), UINT(0));
    SpvId shown = MUL_F(MUL_F(Z(meta), Y(m2)), alpha);
    shown = MUL_F(shown, PICK_F(isBlend, W(texel), FLOAT(1.f)));
    SpvId hole = BOTH(isMask, LESS_F(MUL_F(W(texel), alpha), Z(m2)));
    SpvId normal = SCALE_V3(n, side);
    SpvId albedo = MUL_V3(MUL_V3(spvShuffle3(s, m0), srgbDecode3(s, rgbTexel)), tint);
    SpvId metal = MUL_F(Y(m3), Z(surface)), specular = W(m0);
    SpvId rough = MAX_F(MUL_F(W(m1), Y(surface)), FLOAT(.045f));
    SpvId emission = spvShuffle3(s, m1);
    if (t->graphCount) {
        spvStore(s, t->own.color, albedo);
        spvStore(s, t->own.emissive, emission);
        spvStore(s, t->own.normal, normal);
        spvStore(s, t->own.rough, rough);
        spvStore(s, t->own.metal, metal);
        spvStore(s, t->own.alpha, FLOAT(1.f));
        const GraphInputs inputs = {uv, ADD_V3(origin, SCALE_V3(d, LOAD_F(t->bestT))), normal, spvOp1(s, SPV_OP_FNegate, s->vec3, d), albedo, material};
        emitGraphs(s, &t->r, t->graphs, t->graphCount, AS_UINT(W(m4)), &inputs, &t->own, t->textures, t->sampledType);
        albedo = LOAD_V3(t->own.color);
        emission = LOAD_V3(t->own.emissive);
        normal = LOAD_V3(t->own.normal);
        rough = LOAD_F(t->own.rough);
        metal = LOAD_F(t->own.metal);
        shown = MUL_F(shown, LOAD_F(t->own.alpha));
    }
    SpvId notMetal = SUB_F(FLOAT(1.f), metal), a = MUL_F(rough, rough);
    SpvId ndv = MAX_F(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(normal, d)), FLOAT(1e-4f));
    SpvId dielectric = SCALE_V3(same3(s, .04f), specular);
    SpvId f0 = shaderReflects3(s, dielectric, notMetal, albedo, metal);
    SpvId shiny = EITHER(MORE_F(ADD_F(specular, metal), FLOAT(0.f)), NOT_EQ(AS_UINT(W(m4)), UINT(0)));
    SpvId unlit = NOT_EQ(AND_U(flags, UINT(ARTF_MATERIAL_UNLIT)), UINT(0));
    if (!t->more) {
        /* (no material past glTF's core in the scene: a mirror and a lobe, as trace.c's lightFrom) */
        SpvId gain = shaderGain3(s, f0, shaderUnkept(s, ndv, rough));
        SpvId mirror = MUL_V3(shaderMirror3(s, f0, SUB_F(FLOAT(1.f), rough), shaderFifth(s, ndv), FLOAT(1.f)), gain);
        const Met plain = {normal, SCALE_V3(albedo, notMetal), PICK_F(hole, FLOAT(0.f), shown), emission, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, DOT3(n, d), FLOAT(0.f)), X(m3),
                           shiny, rough, ADD_F(FLOAT(1.f), MUL_F(SUB_F(X(occlusion), FLOAT(1.f)), W(m3))), pick3(s, shiny, mirror, same3(s, 0.f)), f0,
                           {shaderLobeOf(s, rough, ndv), specular, f0, gain},
                           FLOAT(0.f), FLOAT(1.f), same3(s, 0.f), {{0}}, 0, 0, 0, 0, 0, 0};
        return plain;
    }
    /* past glTF's core (ArtfMaterialMore), as trace.c's surfaceAt: its rows, and the coat's and sheen's textures */
    SpvId at = ADD_U(MUL_U(AS_UINT(Y(meta)), UINT(ARTF_MATERIAL_MORE_ROWS)), MUL_U(AS_UINT(X(frameVec(s, &t->r, F_MATERIALS))), UINT(ARTF_MATERIAL_ROWS)));
    SpvId e[ARTF_MATERIAL_MORE_ROWS];
    for (uint32_t k = 0; k < ARTF_MATERIAL_MORE_ROWS; ++k) e[k] = bufferVec(s, &t->r, t->r.materials, ADD_U(at, UINT(k)));
    SpvId moreFlags = spvSelect(s, s->uintType, unlit, AND_U(AS_UINT(W(e[0])), UINT(ARTF_MORE_VOLUME)), AS_UINT(W(e[0])));
#define T_HAS(bit) NOT_EQ(AND_U(moreFlags, UINT(bit)), UINT(0))
    f0 = shaderReflects3(s, MUL_V3(dielectric, spvShuffle3(s, e[4])), notMetal, albedo, metal);
    f0 = pick3(s, T_HAS(ARTF_MORE_IRIDESCENCE), shaderIridescence(s, f0, ndv, W(e[3]), W(e[4]), Z(e[5])), f0);
    SpvId gain = shaderGain3(s, f0, shaderUnkept(s, ndv, rough));
    SpvId mirror = pick3(s, shiny, MUL_V3(shaderMirror3(s, f0, SUB_F(FLOAT(1.f), rough), shaderFifth(s, ndv), FLOAT(1.f)), gain), same3(s, 0.f));
    /* highlights drawn out: the way u grows, into the world by toObject's inverse, square to the normal, turned */
    SpvId c12 = G_CROSS(k1, k2), c20 = G_CROSS(k2, k0), c01 = G_CROSS(k0, k1);
    SpvId tw = SCALE_V3(ADD_V3(ADD_V3(SCALE_V3(c12, X(tangent)), SCALE_V3(c20, Y(tangent))), SCALE_V3(c01, Z(tangent))), PICK_F(LESS_F(DOT3(k0, c12), FLOAT(0.f)), FLOAT(-1.f), FLOAT(1.f)));
    SpvId ta = NORMALIZE(SUB_V3(tw, SCALE_V3(normal, DOT3(normal, tw)))), tb = G_CROSS(normal, ta);
    shaderLobe lobe = shaderLobeOf(s, rough, ndv);
    shaderDraw(s, &lobe, T_HAS(ARTF_MORE_ANISOTROPY), ta, tb, X(e[5]), Y(e[5]), a, shaderDrawnLong(s, a, MUL_F(Z(e[3]), Z(e[3]))));
    /* the coat and sheen, and their textures (white when none) */
    SpvId coat = PICK_F(unlit, FLOAT(0.f), MUL_F(X(e[0]), X(traceTexture(t, lodBase, AS_UINT(X(e[2])), uv))));
    SpvId coatRough = MAX_F(MUL_F(Y(e[0]), Y(traceTexture(t, lodBase, AS_UINT(Y(e[2])), uv))), FLOAT(.045f));
    SpvId sheenRough = MAX_F(MUL_F(Z(e[0]), W(traceTexture(t, lodBase, AS_UINT(X(e[3])), uv))), FLOAT(.07f));
    SpvId sheenTexel = spvShuffle3(s, traceTexture(t, lodBase, AS_UINT(W(e[2])), uv));
    SpvId sheenColor = pick3(s, unlit, same3(s, 0.f), MUL_V3(spvShuffle3(s, e[1]), srgbDecode3(s, sheenTexel)));
    SpvId coatBumps = traceTexture(t, lodBase, AS_UINT(Z(e[2])), uv);
    SpvId cx = MUL_F(SUB_F(MUL_F(X(coatBumps), FLOAT(2.f)), FLOAT(1.f)), Y(e[3])), cy = MUL_F(SUB_F(FLOAT(1.f), MUL_F(Y(coatBumps), FLOAT(2.f))), Y(e[3])), cz = SUB_F(MUL_F(Z(coatBumps), FLOAT(2.f)), FLOAT(1.f));
    SpvId co = ADD_V3(ADD_V3(SCALE_V3(tangent, cx), SCALE_V3(bitangent, cy)), SCALE_V3(plainOn, cz));
    SpvId coatWorld = SCALE_V3(NORMALIZE(ADD_V3(ADD_V3(SCALE_V3(k0, X(co)), SCALE_V3(k1, Y(co))), SCALE_V3(k2, Z(co)))), side);
    SpvId coatNormal = pick3(s, NOT_EQ(AS_UINT(Z(e[2])), UINT(0)), coatWorld, normal);
    SpvId coatNv = MAX_F(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(coatNormal, d)), FLOAT(1e-4f)), coatFar = shaderFifth(s, coatNv);
    SpvId under = SUB_F(FLOAT(1.f), MUL_F(coat, shaderCoatFresnel(s, coatFar)));
    SpvId coatMirror = shaderMirror(s, FLOAT(.04f), SUB_F(FLOAT(1.f), coatRough), coatFar, FLOAT(1.f));
    SpvId layered = NOT_EQ(AND_U(moreFlags, UINT(ARTF_MORE_DRAWN)), UINT(0));
    mirror = ADD_V3(SCALE_V3(mirror, under), vec3Of(s, MUL_F(coat, coatMirror), MUL_F(coat, coatMirror), MUL_F(coat, coatMirror)));
    const Met more = {normal, SCALE_V3(albedo, MUL_F(notMetal, under)), PICK_F(hole, FLOAT(0.f), shown), emission, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, DOT3(n, d), FLOAT(0.f)), X(m3),
                      EITHER(shiny, layered), rough, ADD_F(FLOAT(1.f), MUL_F(SUB_F(X(occlusion), FLOAT(1.f)), W(m3))), mirror, f0,
                      {lobe, specular, f0, gain},
                      coat, under, SCALE_V3(sheenColor, MUL_F(notMetal, under)),
                      {shaderLobeOf(s, coatRough, coatNv), FLOAT(1.f), same3(s, .04f)},
                      moreFlags, coatNormal, DIV_F(FLOAT(1.f), MUL_F(sheenRough, sheenRough)), pick3(s, unlit, same3(s, 0.f), SCALE_V3(spvShuffle3(s, e[6]), MUL_F(notMetal, under))),
                      DIV_F(FLOAT(1.44269504f), W(e[1])), spvShuffle3(s, e[7])};
#undef T_HAS
    return more;
}

static SpvId traceHash(Spv* s, SpvId x) {
    x = MUL_U(XOR_U(x, SHR_U(x, 16)), UINT(0x7FEB352Du));
    x = MUL_U(XOR_U(x, SHR_U(x, 15)), UINT(0x846CA68Bu));
    return XOR_U(x, SHR_U(x, 16));
}

/* ---- aiming at light from all around: as artfEnvAim and artfEnvAimDensity, on the same floats ---- */

/* artfEnvFind: of 513 sums from start, the one of 512 that u falls in, in nine halvings */
static SpvId envFindSpv(Spv* s, const Resources* r, SpvId start, SpvId u) {
    SpvId at = spvConstUint(s, 0);
    for (uint32_t step = 256; step; step >>= 1) {
        SpvId next = spvOp2(s, SPV_OP_IAdd, s->uintType, at, spvConstUint(s, step));
        SpvId sum = envFloat(s, r, spvOp2(s, SPV_OP_IAdd, s->uintType, start, next));
        at = spvSelect(s, s->uintType, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, sum, u), next, at);
    }
    return at;
}

/* artfEnvAimShare */
static SpvId envAimShareSpv(Spv* s, SpvId rowShare, SpvId columnShare, SpvId d) {
    SpvId l1 = ADD_F(ADD_F(G_ABS(X(d)), G_ABS(Y(d))), G_ABS(Z(d)));
    return DIV_F(MUL_F(MUL_F(rowShare, columnShare), FLOAT(65536.f)), MUL_F(MUL_F(l1, l1), l1));
}

/* artfEnvDirection */
static SpvId envDirectionSpv(Spv* s, SpvId u, SpvId v) {
    SpvId a = SUB_F(MUL_F(u, FLOAT(2.f)), FLOAT(1.f)), b = SUB_F(MUL_F(v, FLOAT(2.f)), FLOAT(1.f));
    SpvId sum = ADD_F(G_ABS(a), G_ABS(b));
    SpvId y = SUB_F(FLOAT(1.f), sum);
    SpvId folded = G_GT(sum, FLOAT(1.f));
    SpvId signA = G_PICK(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, a, FLOAT(0.f)), FLOAT(1.f), FLOAT(-1.f));
    SpvId signB = G_PICK(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, b, FLOAT(0.f)), FLOAT(1.f), FLOAT(-1.f));
    SpvId unfoldedA = MUL_F(SUB_F(FLOAT(1.f), G_ABS(b)), signA), unfoldedB = MUL_F(SUB_F(FLOAT(1.f), G_ABS(a)), signB);
    a = G_PICK(folded, unfoldedA, a);
    b = G_PICK(folded, unfoldedB, b);
    SpvId inverse = DIV_F(FLOAT(1.f), spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, ADD_F(ADD_F(MUL_F(a, a), MUL_F(y, y)), MUL_F(b, b))));
    return vec3Of(s, MUL_F(a, inverse), MUL_F(y, inverse), MUL_F(b, inverse));
}

/* artfEnvAim: a direction (into *d) and how likely it was; read only where there is a picture of all around */
static SpvId envAimSpv(Spv* s, const Resources* r, SpvId u0, SpvId u1, SpvId* d) {
    SpvId rows = spvConstUint(s, ARTF_ENV_AIM_START + 512 * 513);
    SpvId row = envFindSpv(s, r, rows, u0);
    SpvId columns = spvOp2(s, SPV_OP_IAdd, s->uintType, spvConstUint(s, ARTF_ENV_AIM_START), spvOp2(s, SPV_OP_IMul, s->uintType, row, spvConstUint(s, 513)));
    SpvId column = envFindSpv(s, r, columns, u1);
    SpvId rowLow = envFloat(s, r, spvOp2(s, SPV_OP_IAdd, s->uintType, rows, row)), rowHigh = envFloat(s, r, spvOp2(s, SPV_OP_IAdd, s->uintType, rows, spvOp2(s, SPV_OP_IAdd, s->uintType, row, spvConstUint(s, 1))));
    SpvId columnLow = envFloat(s, r, spvOp2(s, SPV_OP_IAdd, s->uintType, columns, column)), columnHigh = envFloat(s, r, spvOp2(s, SPV_OP_IAdd, s->uintType, columns, spvOp2(s, SPV_OP_IAdd, s->uintType, column, spvConstUint(s, 1))));
    SpvId rowShare = SUB_F(rowHigh, rowLow), columnShare = SUB_F(columnHigh, columnLow);
    SpvId across = DIV_F(SUB_F(u1, columnLow), columnShare), down = DIV_F(SUB_F(u0, rowLow), rowShare);
    SpvId cu = MUL_F(ADD_F(spvOp1(s, SPV_OP_ConvertUToF, s->floatType, column), across), FLOAT(1.f / 512.f));
    SpvId cv = MUL_F(ADD_F(spvOp1(s, SPV_OP_ConvertUToF, s->floatType, row), down), FLOAT(1.f / 512.f));
    *d = envDirectionSpv(s, cu, cv);
    SpvId total = envFloat(s, r, spvConstUint(s, ARTF_ENV_AIM_START + 512 * 513 + 512));
    return G_PICK(G_GT(total, FLOAT(0.f)), envAimShareSpv(s, rowShare, columnShare, *d), FLOAT(0.f));
}

/* artfEnvAimDensity */
static SpvId envAimDensitySpv(Spv* s, const Resources* r, SpvId d) {
    SpvId x = X(d), y = Y(d), z = Z(d);
    SpvId sum = MAX_F(ADD_F(ADD_F(G_ABS(x), G_ABS(y)), G_ABS(z)), FLOAT(1e-20f));
    SpvId a = DIV_F(x, sum), b = DIV_F(z, sum);
    SpvId below = G_LT(y, FLOAT(0.f));
    SpvId signA = G_PICK(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, a, FLOAT(0.f)), FLOAT(1.f), FLOAT(-1.f));
    SpvId signB = G_PICK(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, b, FLOAT(0.f)), FLOAT(1.f), FLOAT(-1.f));
    SpvId foldedA = MUL_F(SUB_F(FLOAT(1.f), G_ABS(b)), signA), foldedB = MUL_F(SUB_F(FLOAT(1.f), G_ABS(a)), signB);
    a = G_PICK(below, foldedA, a);
    b = G_PICK(below, foldedB, b);
    SpvId u = ADD_F(MUL_F(a, FLOAT(.5f)), FLOAT(.5f)), v = ADD_F(MUL_F(b, FLOAT(.5f)), FLOAT(.5f));
    SpvId column = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, MIN_F(MUL_F(u, FLOAT(512.f)), FLOAT(511.f)));
    SpvId row = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, MIN_F(MUL_F(v, FLOAT(512.f)), FLOAT(511.f)));
    SpvId rows = spvConstUint(s, ARTF_ENV_AIM_START + 512 * 513);
    SpvId columns = spvOp2(s, SPV_OP_IAdd, s->uintType, spvConstUint(s, ARTF_ENV_AIM_START), spvOp2(s, SPV_OP_IMul, s->uintType, row, spvConstUint(s, 513)));
    SpvId one = spvConstUint(s, 1);
    SpvId rowShare = SUB_F(envFloat(s, r, spvOp2(s, SPV_OP_IAdd, s->uintType, rows, spvOp2(s, SPV_OP_IAdd, s->uintType, row, one))), envFloat(s, r, spvOp2(s, SPV_OP_IAdd, s->uintType, rows, row)));
    SpvId columnShare = SUB_F(envFloat(s, r, spvOp2(s, SPV_OP_IAdd, s->uintType, columns, spvOp2(s, SPV_OP_IAdd, s->uintType, column, one))), envFloat(s, r, spvOp2(s, SPV_OP_IAdd, s->uintType, columns, column)));
    SpvId total = envFloat(s, r, spvConstUint(s, ARTF_ENV_AIM_START + 512 * 513 + 512));
    return G_PICK(G_GT(total, FLOAT(0.f)), envAimShareSpv(s, rowShare, columnShare, d), FLOAT(0.f));
}

/* artfSobol4 and artfPathSeed (scene/sampling.h): a path's numbers, Sobol's points scrambled by Owen's way, in the
 * same order. */
static SpvId owenOf(Spv* s, SpvId x, SpvId seed) {
    x = ADD_U(spvOp1(s, 204 /* OpBitReverse */, s->uintType, x), seed);
    const uint32_t m[4] = {0x6C50B47Cu, 0xB82F1E52u, 0xC7AFE638u, 0x8D22F6E6u};
    for (int k = 0; k < 4; ++k) x = XOR_U(x, MUL_U(x, UINT(m[k])));
    return spvOp1(s, 204, s->uintType, x);
}
static void sobol4Of(Spv* s, SpvId index, SpvId seed, SpvId* out) {
    for (uint32_t d = 0; d < 4; ++d) {
        SpvId x = UINT(0);
        for (int k = 0; k < ARTF_SOBOL_BITS; ++k) x = XOR_U(x, MUL_U(AND_U(SHR_U(index, k), UINT(1)), UINT(artfSobolMatrix[d][k])));
        SpvId seedHere = traceHash(s, ADD_U(seed, UINT(d * 0x9E3779B9u)));
        out[d] = MUL_F(TO_F(SHR_U(owenOf(s, x, seedHere), 8)), FLOAT(1.f / 16777216.f));
    }
}
static SpvId pathSeedOf(Spv* s, SpvId pixel, SpvId group) { return traceHash(s, XOR_U(traceHash(s, pixel), MUL_U(ADD_U(group, UINT(1)), UINT(0x85EBCA6Bu)))); }
/* trace.c's ballOf */
static SpvId directionOfSpv(Spv* s, const SpvId* u) {
    SpvId z = SUB_F(FLOAT(1.f), MUL_F(FLOAT(2.f), u[0]));
    SpvId across = spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, MAX_F(SUB_F(FLOAT(1.f), MUL_F(z, z)), FLOAT(0.f)));
    SpvId turn = MUL_F(FLOAT(6.28318531f), u[1]);
    return vec3Of(s, MUL_F(across, cosOf(s, turn)), MUL_F(across, cosOf(s, SUB_F(turn, FLOAT(1.57079633f)))), z);
}
/* trace.c's ggxD, ggxLambda and visibleNormalOf */
static SpvId ggxDSpv(Spv* s, SpvId alpha, SpvId nh) {
    SpvId a2 = MUL_F(alpha, alpha), k = ADD_F(MUL_F(MUL_F(nh, nh), SUB_F(a2, FLOAT(1.f))), FLOAT(1.f));
    return DIV_F(a2, MUL_F(MUL_F(FLOAT(3.14159265f), k), k));
}
static SpvId ggxLambdaSpv(Spv* s, SpvId alpha, SpvId c) {
    SpvId c2 = MAX_F(MUL_F(c, c), FLOAT(1e-8f));
    SpvId inside = ADD_F(FLOAT(1.f), DIV_F(MUL_F(MUL_F(alpha, alpha), SUB_F(FLOAT(1.f), c2)), c2));
    return MUL_F(SUB_F(spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, inside), FLOAT(1.f)), FLOAT(.5f));
}
static SpvId visibleNormalSpv(Spv* s, SpvId n, SpvId v, SpvId alpha, const SpvId* u) {
    SpvId nx = X(n), ny = Y(n), nz = Z(n);
    SpvId sign = G_PICK(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, nz, FLOAT(0.f)), FLOAT(1.f), FLOAT(-1.f));
    SpvId a = DIV_F(FLOAT(-1.f), ADD_F(sign, nz)), b = MUL_F(MUL_F(nx, ny), a);
    SpvId t = vec3Of(s, ADD_F(FLOAT(1.f), MUL_F(MUL_F(MUL_F(sign, nx), nx), a)), MUL_F(sign, b), MUL_F(spvOp1(s, SPV_OP_FNegate, s->floatType, sign), nx));
    SpvId bt = vec3Of(s, b, ADD_F(sign, MUL_F(MUL_F(ny, ny), a)), spvOp1(s, SPV_OP_FNegate, s->floatType, ny));
    SpvId sx = MUL_F(DOT3(v, t), alpha), sy = MUL_F(DOT3(v, bt), alpha), sz = MAX_F(DOT3(v, n), FLOAT(1e-4f));
    SpvId inverse = DIV_F(FLOAT(1.f), spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, ADD_F(ADD_F(MUL_F(sx, sx), MUL_F(sy, sy)), MUL_F(sz, sz))));
    sx = MUL_F(sx, inverse);
    sy = MUL_F(sy, inverse);
    sz = MUL_F(sz, inverse);
    SpvId turn = MUL_F(FLOAT(6.28318531f), u[0]);
    SpvId z = SUB_F(MUL_F(SUB_F(FLOAT(1.f), u[1]), ADD_F(FLOAT(1.f), sz)), sz);
    SpvId across = spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, MIN_F(MAX_F(SUB_F(FLOAT(1.f), MUL_F(z, z)), FLOAT(0.f)), FLOAT(1.f)));
    SpvId cx = MUL_F(ADD_F(MUL_F(across, cosOf(s, turn)), sx), alpha);
    SpvId cy = MUL_F(ADD_F(MUL_F(across, cosOf(s, SUB_F(turn, FLOAT(1.57079633f)))), sy), alpha);
    SpvId cz = ADD_F(z, sz);
    SpvId back = DIV_F(FLOAT(1.f), spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, ADD_F(ADD_F(MUL_F(cx, cx), MUL_F(cy, cy)), MUL_F(cz, cz))));
    cx = MUL_F(cx, back);
    cy = MUL_F(cy, back);
    cz = MUL_F(cz, back);
    return ADD_V3(ADD_V3(SCALE_V3(t, cx), SCALE_V3(bt, cy)), SCALE_V3(n, cz));
}

static SpvId ballOfSpv(Spv* s, const SpvId* u) {
    SpvId r = exp2Of(s, MUL_F(log2Of(s, u[2]), FLOAT(1.f / 3.f)));
    return SCALE_V3(directionOfSpv(s, u), r);
}

/* What a ray aimed at all around brought (trace.c's): the share given back diffusely and the highlight's, each weighed
 * against its own bounce; in the lights' loop, after its walk (t->through), where the walk is the lights' own. */
static void aimedLightSpv(const Tracer* t, const Met* m, SpvId isEnv, SpvId view, SpvId toward, SpvId alpha, SpvId nv, SpvId most, SpvId chance, SpvId environmentLight) {
    Spv* s = t->s;
    SpvId merge;
    spvIfBegin(s, isEnv, &merge);
    {
        SpvId likely = LOAD_F(t->likelyVar), cosine = DOT3(m->normal, toward);
        SpvId other = MUL_F(MUL_F(SUB_F(FLOAT(1.f), chance), cosine), FLOAT(1.f / 3.14159265f));
        SpvId through = LOAD_F(t->through);
        SpvId around = envSample(s, &t->r, UINT(0), toward);
        SpvId weight = DIV_F(MUL_F(likely, likely), ADD_F(MUL_F(likely, likely), MUL_F(other, other)));
        SpvId share = MUL_F(MUL_F(MUL_F(MUL_F(MUL_F(MUL_F(m->open, SUB_F(FLOAT(1.f), most)), FLOAT(1.f / 3.14159265f)), cosine), through), DIV_F(weight, likely)), environmentLight);
        SpvId h = NORMALIZE(ADD_V3(view, toward));
        SpvId facing = ggxDSpv(s, alpha, MAX_F(DOT3(m->normal, h), FLOAT(0.f)));
        SpvId lambdaView = ggxLambdaSpv(s, alpha, nv);
        SpvId otherShine = DIV_F(MUL_F(chance, facing), MUL_F(MUL_F(ADD_F(FLOAT(1.f), lambdaView), FLOAT(4.f)), nv));
        SpvId shineWeight = DIV_F(MUL_F(likely, likely), ADD_F(MUL_F(likely, likely), MUL_F(otherShine, otherShine)));
        SpvId highlight = MUL_F(MUL_F(MUL_F(DIV_F(facing, MUL_F(MUL_F(FLOAT(4.f), nv), ADD_F(ADD_F(FLOAT(1.f), lambdaView), ggxLambdaSpv(s, alpha, cosine)))), through), DIV_F(shineWeight, likely)), environmentLight);
        highlight = PICK_F(MORE_F(chance, FLOAT(0.f)), highlight, FLOAT(0.f));
        SpvId gained = MUL_V3(ADD_V3(SCALE_V3(m->albedo, share), SCALE_V3(m->mirror, highlight)), around);
        spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), MUL_V3(LOAD_V3(t->path), gained)));
    }
    spvIfEnd(s, merge);
}

static SpvId traceRandom(const Tracer* t) {
    Spv* s = t->s;
    SpvId next = ADD_U(MUL_U(LOAD_U(t->state), UINT(747796405u)), UINT(2891336453u));
    spvStore(s, t->state, next);
    return MUL_F(TO_F(SHR_U(traceHash(s, next), 8)), FLOAT(1.f / 16777216.f));
}

static SpvId reachOf(Spv* s, SpvId p) {
    SpvId a = spvGlsl1(s, SPV_GLSL_FAbs, s->vec3, p);
    return MUL_F(FLOAT(1e-4f), ADD_F(FLOAT(1.f), MAX_F(X(a), MAX_F(Y(a), Z(a)))));
}

/* As lightThrough: leaves it in `through`. */
static void lightThrough(const Tracer* t, SpvId from, SpvId to, SpvId limit) {
    Spv* s = t->s;
    spvStore(s, t->shadowOrigin, from);
    spvStore(s, t->through, FLOAT(1.f));
    spvStore(s, t->limit, limit);
    spvStore(s, t->shadowStep, UINT(0));
    Loop steps;
    loopOpen(s, &steps);
    loopWhile(s, &steps, LESS_U(LOAD_U(t->shadowStep), UINT(8)));
    SpvId o = LOAD_V3(t->shadowOrigin);
    closest(t, o, to, LOAD_F(t->limit), true);
    breakIf(s, &steps, IS_EQ(LOAD_U(t->bestInstance), UINT(0xFFFFFFFFu)));
    Met m = surfaceAt(t, o, to, FLOAT(0.f));
    SpvId through = MUL_F(LOAD_F(t->through), SUB_F(FLOAT(1.f), MIN_F(MAX_F(m.alpha, FLOAT(0.f)), FLOAT(1.f))));
    spvStore(s, t->through, PICK_F(LESS_F(through, FLOAT(.01f)), FLOAT(0.f), through));
    breakIf(s, &steps, LESS_F(through, FLOAT(.01f)));
    SpvId step = ADD_F(LOAD_F(t->bestT), reachOf(s, o));
    spvStore(s, t->shadowOrigin, ADD_V3(o, SCALE_V3(to, step)));
    SpvId left = SUB_F(LOAD_F(t->limit), step);
    spvStore(s, t->limit, left);
    breakIf(s, &steps, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, left, FLOAT(0.f)));
    spvStore(s, t->shadowStep, ADD_U(LOAD_U(t->shadowStep), UINT(1)));
    loopClose(s, &steps);
}

/* ---- rays while drawing (ArtfShadowDesc.traced, ArtfPostDesc.screen.tracedReflections): trace.c's artfTraced* ----
 *
 * A pixel shader or a pass after the frame follows rays through the same trees (or the hardware) as tracing, with
 * the same code: what it needs is declared here. */

/* artfTracedBall, as a table of the module's. */
static SpvId tracedBallTable(Spv* s) {
    uint32_t words[18];
    SpvId array = spvTypeArray(s, s->vec3, 16, 0), pointer = spvTypePointer(s, 6 /* Private */, array);
    for (int i = 0; i < 16; ++i) {
        const SpvId c[3] = {FLOAT(artfTracedBall[i][0]), FLOAT(artfTracedBall[i][1]), FLOAT(artfTracedBall[i][2])};
        words[2 + i] = spvNewId(s);
        spvEmit(&s->types, 44 /* OpConstantComposite */, SPV_WORDS(s->vec3, words[2 + i], c[0], c[1], c[2]));
    }
    words[0] = array;
    words[1] = spvNewId(s);
    spvEmit(&s->types, 44, words, 18);
    SpvId table = spvNewId(s);
    spvEmit(&s->types, 59 /* OpVariable */, SPV_WORDS(pointer, table, 6, words[1]));
    return table;
}

/* Before main: the trees' bindings, the top level with the hardware (its capability already asked for), and the
 * table. textures and sampled: the shader's array of binding 6. */
static void tracerDeclare(Spv* s, Tracer* t, const Resources* r, SpvId textures, SpvId sampled, const ArtfGraphData* graphs, uint32_t graphCount, bool hardware) {
    memset(t, 0, sizeof *t);
    t->s = s;
    t->r = *r;
    t->textures = textures;
    t->sampledType = sampled;
    t->nodes = spvResource(s, blockOf(s, r->vec4Array, false), SPV_STORAGE_StorageBuffer, 17);
    t->triangles = spvResource(s, blockOf(s, r->vec4Array, false), SPV_STORAGE_StorageBuffer, 18);
    t->vertices = spvResource(s, blockOf(s, r->vec4Array, false), SPV_STORAGE_StorageBuffer, 19);
    t->top = spvResource(s, blockOf(s, r->vec4Array, false), SPV_STORAGE_StorageBuffer, 20);
    t->instances = spvResource(s, blockOf(s, r->vec4Array, false), SPV_STORAGE_StorageBuffer, 21);
    t->hardware = hardware;
    if (hardware) {
        spvExtension(s, "SPV_KHR_ray_query");
        t->accelType = spvTypeOpaque(s, 5341 /* OpTypeAccelerationStructureKHR */);
        t->accel = spvResource(s, t->accelType, SPV_STORAGE_UniformConstant, 64);
    }
    t->graphs = graphs;
    t->graphCount = graphCount;
    t->ptrFnUint = spvTypePointer(s, SPV_STORAGE_Function, s->uintType);
    t->ballTable = tracedBallTable(s);
}

/* First thing in main: what the walks and lightThrough keep. */
static void tracerVariables(Tracer* t) {
    Spv* s = t->s;
    SpvId ptrStack = spvTypePointer(s, SPV_STORAGE_Function, spvTypeArray(s, s->uintType, ARTF_TRACE_STACK, 0)), ptrUint = t->ptrFnUint;
    SpvId ptrFloat = spvTypePointer(s, SPV_STORAGE_Function, s->floatType), ptrVec3 = spvTypePointer(s, SPV_STORAGE_Function, s->vec3);
    t->stack = VAR(ptrStack); t->stack2 = VAR(ptrStack);
    t->depth = VAR(ptrUint); t->at = VAR(ptrUint); t->pop = VAR(ptrUint); t->leaf = VAR(ptrUint);
    t->depth2 = VAR(ptrUint); t->at2 = VAR(ptrUint); t->pop2 = VAR(ptrUint); t->leaf2 = VAR(ptrUint);
    t->bestT = VAR(ptrFloat); t->bestU = VAR(ptrFloat); t->bestV = VAR(ptrFloat);
    t->bestInstance = VAR(ptrUint); t->bestTriangle = VAR(ptrUint);
    t->shadowOrigin = VAR(ptrVec3); t->origin = VAR(ptrVec3); t->direction = VAR(ptrVec3);
    t->through = VAR(ptrFloat); t->limit = VAR(ptrFloat); t->shadowStep = VAR(ptrUint); t->step = VAR(ptrUint);
    t->tracedSum = VAR(ptrFloat); t->tracedRay = VAR(ptrUint);
    t->own.color = VAR(ptrVec3); t->own.emissive = VAR(ptrVec3); t->own.normal = VAR(ptrVec3);
    t->own.rough = VAR(ptrFloat); t->own.metal = VAR(ptrFloat); t->own.alpha = VAR(ptrFloat);
    if (t->hardware) t->query = VAR(spvTypePointer(s, SPV_STORAGE_Function, spvTypeOpaque(s, 4472 /* OpTypeRayQueryKHR */)));
}

static Tracer* tracerNew(void) { return artfCalloc(1, sizeof(Tracer)); }

static SpvId tracedBallAt(const Tracer* t, SpvId i) {
    Spv* s = t->s;
    return spvLoad(s, s->vec3, spvAccess(s, spvTypePointer(s, 6 /* Private */, s->vec3), t->ballTable, &i, 1));
}

/* As artfTracedPlace. */
static SpvId tracedPlaceSpv(Spv* s, const Resources* r, SpvId p) {
    SpvId c0 = frameVec(s, r, F_VIEWPROJ), c1 = frameVec(s, r, F_VIEWPROJ + 1), c2 = frameVec(s, r, F_VIEWPROJ + 2), c3 = frameVec(s, r, F_VIEWPROJ + 3);
    SpvId px = X(p), py = Y(p), pz = Z(p);
    SpvId cx = ADD_F(ADD_F(ADD_F(MUL_F(X(c0), px), MUL_F(X(c1), py)), MUL_F(X(c2), pz)), X(c3));
    SpvId cy = ADD_F(ADD_F(ADD_F(MUL_F(Y(c0), px), MUL_F(Y(c1), py)), MUL_F(Y(c2), pz)), Y(c3));
    SpvId cw = ADD_F(ADD_F(ADD_F(MUL_F(W(c0), px), MUL_F(W(c1), py)), MUL_F(W(c2), pz)), W(c3));
    SpvId inv = DIV_F(FLOAT(1.f), MAX_F(cw, FLOAT(1e-6f)));
    SpvId x = MUL_F(ADD_F(MUL_F(MUL_F(cx, inv), FLOAT(.5f)), FLOAT(.5f)), W(frameVec(s, r, F_EYE)));
    SpvId y = MUL_F(SUB_F(FLOAT(.5f), MUL_F(MUL_F(cy, inv), FLOAT(.5f))), W(frameVec(s, r, F_EYE + 1)));
    x = MIN_F(MAX_F(x, FLOAT(-1e6f)), FLOAT(1e6f));
    y = MIN_F(MAX_F(y, FLOAT(-1e6f)), FLOAT(1e6f));
    SpvId ix = AND_U(TO_U(FLOOR_F(x)), UINT(1)), iy = AND_U(TO_U(FLOOR_F(y)), UINT(1));
    return AND_U(ADD_U(ADD_U(ix, MUL_U(iy, UINT(2))), AS_UINT(Z(frameVec(s, r, F_TRACED)))), UINT(3));
}

/* As tracedLift. */
static SpvId tracedLiftSpv(Spv* s, const Resources* r, SpvId p, SpvId n) {
    SpvId lift = ADD_F(reachOf(s, p), MUL_F(spvGlsl1(s, SPV_GLSL_Length, s->floatType, SUB_V3(p, spvShuffle3(s, frameVec(s, r, F_CAMERA)))), FLOAT(1e-3f)));
    return ADD_V3(p, SCALE_V3(n, lift));
}

/* As artfTracedShadow: the sun, or the frame's light `index`. */
static SpvId tracedShadowSpv(const Tracer* t, SpvId world, SpvId n, bool sun, SpvId index) {
    Spv* s = t->s;
    const Resources* r = &t->r;
    SpvId o = tracedLiftSpv(s, r, world, n);
    SpvId size = sun ? Z(frameVec(s, r, F_LOOK)) : frameFloatAt(s, r, ADD_U(UINT(F_LIGHT_SIZES), SHR_U(index, 2)), AND_U(index, UINT(3)));
    SpvId rays = spvSelect(s, s->uintType, MORE_F(size, FLOAT(0.f)), spvGlsl2(s, SPV_GLSL_UMax, s->uintType, AS_UINT(W(frameVec(s, r, F_TRACED))), UINT(1)), UINT(1));
    SpvId first = MUL_U(tracedPlaceSpv(s, r, world), UINT(4));
    SpvId position = sun ? 0 : spvShuffle3(s, frameVecAt(s, r, ADD_U(UINT(F_LIGHTS), MUL_U(index, UINT(F_LIGHT_ROWS)))));
    SpvId toSun = sun ? spvShuffle3(s, frameVec(s, r, F_TOSUN)) : 0;
    spvStore(s, t->tracedSum, FLOAT(0.f));
    spvStore(s, t->tracedRay, UINT(0));
    Loop each;
    loopOpen(s, &each);
    loopWhile(s, &each, LESS_U(LOAD_U(t->tracedRay), rays));
    {
        SpvId k = LOAD_U(t->tracedRay);
        SpvId b = SCALE_V3(tracedBallAt(t, AND_U(ADD_U(first, k), UINT(15))), size);
        SpvId to, limit;
        if (sun) {
            to = NORMALIZE(ADD_V3(toSun, b));
            limit = FLOAT(3e37f);
        } else {
            SpvId toward = SUB_V3(ADD_V3(position, b), o);
            limit = SQRT_F(MAX_F(DOT3(toward, toward), FLOAT(1e-8f)));
            to = SCALE_V3(toward, DIV_F(FLOAT(1.f), limit));
        }
        lightThrough(t, o, to, limit);
        spvStore(s, t->tracedSum, ADD_F(LOAD_F(t->tracedSum), LOAD_F(t->through)));
        spvStore(s, t->tracedRay, ADD_U(k, UINT(1)));
    }
    loopClose(s, &each);
    return DIV_F(LOAD_F(t->tracedSum), TO_F(rays));
}

uint32_t* artfShaderTrace(size_t* size, uint32_t maxTextures, const ArtfGraphData* graphs, uint32_t graphCount, bool guides, bool hardware, bool more) {
    Spv b, *s = &b;
    spvInit(s);
    spvCapability(s, 5301); /* ShaderNonUniform */
    spvCapability(s, 5307); /* SampledImageArrayNonUniformIndexing */
    spvCapability(s, 50); /* ImageQuery: a texture's size, for the tracer's cones */
    if (hardware) spvCapability(s, 4472 /* RayQueryKHR: capabilities come before extensions */);
    spvExtension(s, "SPV_EXT_descriptor_indexing");
    Tracer tracer, *t = &tracer;
    memset(t, 0, sizeof *t);
    t->s = s;
    declare(s, &t->r, true);
    SpvId imageType;
    t->sampledType = spvTypeSampledImage2D(s, &imageType);
    t->textures = spvResource(s, spvTypeArray(s, t->sampledType, maxTextures, 0), SPV_STORAGE_UniformConstant, 6);
    t->nodes = spvResource(s, blockOf(s, t->r.vec4Array, false), SPV_STORAGE_StorageBuffer, 17);
    t->triangles = spvResource(s, blockOf(s, t->r.vec4Array, false), SPV_STORAGE_StorageBuffer, 18);
    t->vertices = spvResource(s, blockOf(s, t->r.vec4Array, false), SPV_STORAGE_StorageBuffer, 19);
    t->top = spvResource(s, blockOf(s, t->r.vec4Array, false), SPV_STORAGE_StorageBuffer, 20);
    t->instances = spvResource(s, blockOf(s, t->r.vec4Array, false), SPV_STORAGE_StorageBuffer, 21);
    t->sum = spvResource(s, blockOf(s, t->r.vec4Array, true), SPV_STORAGE_StorageBuffer, 22);
    t->frame = spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, T_ROWS, 16), true), SPV_STORAGE_Uniform, 23);
    t->guides = guides;
    t->hardware = hardware;
    t->more = more;
    if (more) t->extraLights = spvResource(s, blockOf(s, t->r.vec4Array, false), SPV_STORAGE_StorageBuffer, 60);
    if (hardware) {
        spvExtension(s, "SPV_KHR_ray_query");
        t->accelType = spvTypeOpaque(s, 5341 /* OpTypeAccelerationStructureKHR */);
        t->accel = spvResource(s, t->accelType, SPV_STORAGE_UniformConstant, 64);
    }
    if (guides) {
        t->guide = spvResource(s, blockOf(s, t->r.vec4Array, true), SPV_STORAGE_StorageBuffer, 27);
        t->albedoGuide = spvResource(s, blockOf(s, t->r.vec4Array, true), SPV_STORAGE_StorageBuffer, 28);
        t->shiftGuide = spvResource(s, blockOf(s, t->r.vec4Array, true), SPV_STORAGE_StorageBuffer, 48);
    }
    SpvId uvec3 = spvNewId(s);
    spvEmit(&s->types, 23 /* OpTypeVector */, SPV_WORDS(uvec3, s->uintType, 3));
    SpvId global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, uvec3), SPV_STORAGE_Input);
    spvDecorate(s, global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);
    SpvId stackType = spvTypeArray(s, s->uintType, ARTF_TRACE_STACK, 0);
    SpvId ptrStack = spvTypePointer(s, SPV_STORAGE_Function, stackType), ptrUint = spvTypePointer(s, SPV_STORAGE_Function, s->uintType);
    SpvId ptrFloat = spvTypePointer(s, SPV_STORAGE_Function, s->floatType), ptrVec3 = spvTypePointer(s, SPV_STORAGE_Function, s->vec3);
    t->ptrFnUint = ptrUint;

    SpvId main = spvBeginMain(s);
    t->stack = VAR(ptrStack); t->stack2 = VAR(ptrStack);
    t->depth = VAR(ptrUint); t->at = VAR(ptrUint); t->pop = VAR(ptrUint); t->leaf = VAR(ptrUint);
    t->depth2 = VAR(ptrUint); t->at2 = VAR(ptrUint); t->pop2 = VAR(ptrUint); t->leaf2 = VAR(ptrUint);
    t->bestT = VAR(ptrFloat); t->bestU = VAR(ptrFloat); t->bestV = VAR(ptrFloat);
    t->bestInstance = VAR(ptrUint); t->bestTriangle = VAR(ptrUint);
    t->state = VAR(ptrUint); t->covered = VAR(ptrUint); t->bounces = VAR(ptrUint); t->step = VAR(ptrUint); t->sampleIndex = VAR(ptrUint);
    t->origin = VAR(ptrVec3); t->direction = VAR(ptrVec3); t->path = VAR(ptrVec3); t->light = VAR(ptrVec3);
    t->shadowOrigin = VAR(ptrVec3); t->ball = VAR(ptrVec3); t->choice = VAR(ptrFloat); t->diffuseLikely = VAR(ptrFloat); t->bounceDir = VAR(ptrVec3); t->keptVar = VAR(ptrFloat); t->shineVar = VAR(ptrFloat); t->towardVar = VAR(ptrVec3); t->likelyVar = VAR(ptrFloat); t->coneWidth = VAR(ptrFloat); t->coneSpread = VAR(ptrFloat);
    t->through = VAR(ptrFloat); t->limit = VAR(ptrFloat); t->travelled = VAR(ptrFloat); t->keep = VAR(ptrFloat);
    t->shadowStep = VAR(ptrUint); t->lightIndex = VAR(ptrUint); t->ballStep = VAR(ptrUint);
    t->graphs = graphs;
    t->graphCount = graphCount;
    t->guideBits = VAR(ptrUint); t->guidePlace = VAR(ptrVec3); t->guideColor = VAR(ptrVec3); t->guideInstance = VAR(ptrUint); t->guideShift = VAR(ptrVec3);
    t->own.color = VAR(ptrVec3); t->own.emissive = VAR(ptrVec3); t->own.normal = VAR(ptrVec3);
    t->own.rough = VAR(ptrFloat); t->own.metal = VAR(ptrFloat); t->own.alpha = VAR(ptrFloat);
    if (more) { t->alongVar = VAR(ptrFloat); t->acrossVar = VAR(ptrFloat); }
    if (hardware) t->query = VAR(spvTypePointer(s, SPV_STORAGE_Function, spvTypeOpaque(s, 4472 /* OpTypeRayQueryKHR */)));

    SpvId id = spvLoad(s, uvec3, global);
    SpvId x = spvExtract(s, s->uintType, id, 0), y = spvExtract(s, s->uintType, id, 1);
    SpvId sizes = traceFrameVec(t, T_SIZE), pathRow = traceFrameVec(t, T_PATH);
    SpvId width = AS_UINT(X(sizes)), height = AS_UINT(Y(sizes)), done = AS_UINT(Z(sizes)), samples = AS_UINT(W(sizes));
    SpvId maxBounces = AS_UINT(X(pathRow)), paths = NOT_EQ(AS_UINT(Y(pathRow)), UINT(0));
    SpvId inFrame;
    spvIfBegin(s, BOTH(LESS_U(x, width), LESS_U(y, height)), &inFrame);
    SpvId eye = spvShuffle3(s, traceFrameVec(t, T_EYE)), right = spvShuffle3(s, traceFrameVec(t, T_RIGHT)), up = spvShuffle3(s, traceFrameVec(t, T_UP)), forward = spvShuffle3(s, traceFrameVec(t, T_FORWARD));
    SpvId sky = spvShuffle3(s, frameVec(s, &t->r, F_SKY)), ground = spvShuffle3(s, frameVec(s, &t->r, F_GROUND));
    SpvId lightCount = AS_UINT(X(frameVec(s, &t->r, F_COUNTS)));
    SpvId look = frameVec(s, &t->r, F_LOOK), fog = spvShuffle3(s, frameVec(s, &t->r, F_FOG));
    SpvId pixel = ADD_U(x, MUL_U(y, width));
    SpvId zero3 = vec3Of(s, FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)), one3 = vec3Of(s, FLOAT(1.f), FLOAT(1.f), FLOAT(1.f));

    spvStore(s, t->sampleIndex, UINT(0));
    spvStore(s, t->guideBits, UINT(0));
    spvStore(s, t->guideInstance, UINT(0));
    spvStore(s, t->guideShift, zero3);
    spvStore(s, t->guidePlace, zero3);
    spvStore(s, t->guideColor, zero3);
    Loop sampleLoop;
    loopOpen(s, &sampleLoop);
    loopWhile(s, &sampleLoop, LESS_U(LOAD_U(t->sampleIndex), samples));
    {
        spvStore(s, t->state, traceHash(s, ADD_U(traceHash(s, pixel), ADD_U(done, LOAD_U(t->sampleIndex)))));
        spvStore(s, t->diffuseLikely, FLOAT(0.f));
        {
            /* the ray's cone (trace.c's): a pixel's angle, or in an orthographic view a pixel's width */
            SpvId pixelSize = DIV_F(MUL_F(FLOAT(2.f), spvGlsl1(s, SPV_GLSL_Length, s->floatType, up)), TO_F(height));
            SpvId flat = MORE_F(W(traceFrameVec(t, T_EYE)), FLOAT(0.f));
            spvStore(s, t->coneWidth, PICK_F(flat, pixelSize, FLOAT(0.f)));
            spvStore(s, t->coneSpread, PICK_F(flat, FLOAT(0.f), pixelSize));
        }
        SpvId jitter[4];
        sobol4Of(s, ADD_U(done, LOAD_U(t->sampleIndex)), pathSeedOf(s, pixel, UINT(0)), jitter);
        SpvId jx = jitter[0], jy = jitter[1];
        SpvId px = SUB_F(MUL_F(DIV_F(ADD_F(TO_F(x), jx), TO_F(width)), FLOAT(2.f)), FLOAT(1.f));
        SpvId py = SUB_F(FLOAT(1.f), MUL_F(DIV_F(ADD_F(TO_F(y), jy), TO_F(height)), FLOAT(2.f)));
        /* (orthographic, eye's w 1: each ray from its place on the view's plane, all forward. By a 0 or a 1, so that a
         * perspective's arithmetic is as it was, to the bit) */
        SpvId across = W(traceFrameVec(t, T_EYE)), spread = SUB_F(FLOAT(1.f), across);
        spvStore(s, t->origin, ADD_V3(ADD_V3(eye, SCALE_V3(right, MUL_F(px, across))), SCALE_V3(up, MUL_F(py, across))));
        spvStore(s, t->direction, NORMALIZE(ADD_V3(ADD_V3(forward, SCALE_V3(right, MUL_F(px, spread))), SCALE_V3(up, MUL_F(py, spread)))));
        spvStore(s, t->path, one3);
        spvStore(s, t->light, zero3);
        spvStore(s, t->covered, UINT(0));
        spvStore(s, t->bounces, UINT(0));
        spvStore(s, t->step, UINT(0));
        spvStore(s, t->travelled, FLOAT(0.f));
        spvStore(s, t->keep, FLOAT(1.f));

        Loop steps;
        loopOpen(s, &steps);
        loopWhile(s, &steps, LESS_U(LOAD_U(t->step), UINT(48)));
        spvStore(s, t->step, ADD_U(LOAD_U(t->step), UINT(1)));
        SpvId o = LOAD_V3(t->origin), d = LOAD_V3(t->direction);
        closest(t, o, d, FLOAT(3e37f), false);
        SpvId missed = IS_EQ(LOAD_U(t->bestInstance), UINT(0xFFFFFFFFu)), merge;
        SpvId environmentLight = W(frameVec(s, &t->r, F_SKY)), aroundOn = MORE_F(environmentLight, FLOAT(0.f)), aroundMerge;
        spvIfBegin(s, BOTH(BOTH(missed, NOT_EQ(LOAD_U(t->covered), UINT(0))), NOT(aroundOn)), &merge);
        {
            SpvId share = ADD_F(MUL_F(Y(d), FLOAT(.5f)), FLOAT(.5f));
            SpvId env = shaderSkyGround3(s, ground, sky, share);
            spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), MUL_V3(LOAD_V3(t->path), env)));
        }
        spvIfEnd(s, merge);
        spvIfBegin(s, BOTH(BOTH(missed, NOT_EQ(LOAD_U(t->covered), UINT(0))), aroundOn), &aroundMerge);
        {
            /* (weighed against aiming at it, after a diffuse bounce: as trace.c's) */
            SpvId before = LOAD_F(t->diffuseLikely), other = envAimDensitySpv(s, &t->r, d);
            SpvId weight = PICK_F(BOTH(paths, MORE_F(before, FLOAT(0.f))), DIV_F(MUL_F(before, before), ADD_F(MUL_F(before, before), MUL_F(other, other))), FLOAT(1.f));
            SpvId env = SCALE_V3(envSample(s, &t->r, UINT(0), d), MUL_F(environmentLight, weight));
            spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), MUL_V3(LOAD_V3(t->path), env)));
        }
        spvIfEnd(s, aroundMerge);
        breakIf(s, &steps, missed);

        spvStore(s, t->coneWidth, ADD_F(LOAD_F(t->coneWidth), MUL_F(LOAD_F(t->coneSpread), LOAD_F(t->bestT))));
        Met m = surfaceAt(t, o, d, LOAD_F(t->coneWidth));
        SpvId p = ADD_V3(o, SCALE_V3(d, LOAD_F(t->bestT)));
        SpvId reach = reachOf(s, p);
        if (more) {
            /* (what came from inside a thing that takes some light as it goes: KHR_materials_volume) */
            SpvId inside = BOTH(NOT(m.front), NOT_EQ(AND_U(m.flags, UINT(ARTF_MORE_VOLUME)), UINT(0)));
            SpvId gone = SCALE_V3(m.absorb, spvOp1(s, SPV_OP_FNegate, s->floatType, LOAD_F(t->bestT)));
            SpvId kept = vec3Of(s, exp2Of(s, X(gone)), exp2Of(s, Y(gone)), exp2Of(s, Z(gone)));
            spvStore(s, t->path, MUL_V3(LOAD_V3(t->path), pick3(s, inside, kept, same3(s, 1.f))));
        }
        SpvId travelled = ADD_F(LOAD_F(t->travelled), LOAD_F(t->bestT));
        spvStore(s, t->travelled, travelled);
        SpvId goesThrough = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, traceRandom(t), m.alpha);
        spvIfBegin(s, goesThrough, &merge);
        {
            /* straight, or bent, or turned back when it comes out too flat */
            SpvId bends = MORE_F(m.ior, FLOAT(0.f));
            SpvId eta = PICK_F(m.front, DIV_F(FLOAT(1.f), MAX_F(m.ior, FLOAT(1e-3f))), m.ior);
            SpvId cosine = spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(d, m.normal));
            SpvId k2 = SUB_F(FLOAT(1.f), MUL_F(MUL_F(eta, eta), SUB_F(FLOAT(1.f), MUL_F(cosine, cosine))));
            SpvId turned = LESS_F(k2, FLOAT(0.f));
            SpvId along = PICK_F(turned, MUL_F(FLOAT(2.f), cosine), SUB_F(MUL_F(eta, cosine), SQRT_F(MAX_F(k2, FLOAT(0.f)))));
            SpvId bent = NORMALIZE(ADD_V3(SCALE_V3(d, PICK_F(turned, FLOAT(1.f), eta)), SCALE_V3(m.normal, along)));
            SpvId onward = pick3(s, bends, bent, d);
            spvStore(s, t->direction, onward);
            spvStore(s, t->origin, ADD_V3(p, SCALE_V3(onward, reach)));
        }
        spvIfEnd(s, merge);
        SpvId stops;
        spvIfBegin(s, NOT(goesThrough), &stops);
        {
            SpvId first;
            spvIfBegin(s, IS_EQ(LOAD_U(t->covered), UINT(0)), &first);
            {
                if (guides) {
                    SpvId firstSample;
                    spvIfBegin(s, IS_EQ(LOAD_U(t->sampleIndex), UINT(0)), &firstSample);
                    /* (as artfNormalPack) */
                    SpvId packed = UINT(0);
                    for (int k = 0; k < 3; ++k) {
                        SpvId c = ADD_F(MUL_F(ADD_F(MUL_F(spvExtract(s, s->floatType, m.normal, (uint32_t)k), FLOAT(.5f)), FLOAT(.5f)), FLOAT(1023.f)), FLOAT(.5f));
                        packed = spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, packed, spvOp2(s, SPV_OP_ShiftLeftLogical, s->uintType, TO_U(c), UINT((uint32_t)k * 10)));
                    }
                    spvStore(s, t->guideBits, packed);
                    spvStore(s, t->guidePlace, p);
                    spvStore(s, t->guideColor, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, m.albedo, m.f0));
                    spvStore(s, t->guideInstance, ADD_U(LOAD_U(t->bestInstance), UINT(1)));
                    {
                        /* as shiftOf */
                        SpvId tri = bufferVec(s, &t->r, t->triangles, LOAD_U(t->bestTriangle));
                        SpvId wb = LOAD_F(t->bestU), wc = LOAD_F(t->bestV), wa = SUB_F(SUB_F(FLOAT(1.f), wb), wc);
                        const SpvId corners[3] = {AS_UINT(X(tri)), AS_UINT(Y(tri)), AS_UINT(Z(tri))}, shares[3] = {wa, wb, wc};
                        SpvId d = zero3;
                        for (int k = 0; k < 3; ++k) {
                            SpvId row = vertexVec(t, corners[k], 2);
                            d = ADD_V3(d, SCALE_V3(vec3Of(s, Y(row), Z(row), W(row)), shares[k]));
                        }
                        SpvId at = MUL_U(LOAD_U(t->bestInstance), UINT(4));
                        SpvId k0 = spvShuffle3(s, bufferVec(s, &t->r, t->instances, at)), k1 = spvShuffle3(s, bufferVec(s, &t->r, t->instances, ADD_U(at, UINT(1)))), k2 = spvShuffle3(s, bufferVec(s, &t->r, t->instances, ADD_U(at, UINT(2))));
                        SpvId c12 = spvGlsl2(s, SPV_GLSL_Cross, s->vec3, k1, k2), c20 = spvGlsl2(s, SPV_GLSL_Cross, s->vec3, k2, k0), c01 = spvGlsl2(s, SPV_GLSL_Cross, s->vec3, k0, k1);
                        SpvId det = DOT3(k0, c12);
                        SpvId inverse = PICK_F(MORE_F(ABS_F(det), FLOAT(0.f)), DIV_F(FLOAT(1.f), det), FLOAT(0.f));
                        SpvId shift = ADD_V3(ADD_V3(SCALE_V3(c12, spvExtract(s, s->floatType, d, 0)), SCALE_V3(c20, spvExtract(s, s->floatType, d, 1))), SCALE_V3(c01, spvExtract(s, s->floatType, d, 2)));
                        spvStore(s, t->guideShift, SCALE_V3(shift, inverse));
                    }
                    spvIfEnd(s, firstSample);
                }
                spvStore(s, t->keep, shaderFogKeep(s, MUL_F(travelled, Y(look))));
            }
            spvIfEnd(s, first);
            spvStore(s, t->covered, UINT(1));
            SpvId view = spvOp1(s, SPV_OP_FNegate, s->vec3, d);
            SpvId lifted = ADD_V3(p, SCALE_V3(m.normal, reach));
            spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), MUL_V3(LOAD_V3(t->path), m.emissive)));

            /* a point even in a ball, and which lobe: this bounce's numbers (trace.c's ballOf) */
            SpvId u[4];
            sobol4Of(s, ADD_U(done, LOAD_U(t->sampleIndex)), pathSeedOf(s, pixel, ADD_U(LOAD_U(t->bounces), UINT(1))), u);
            spvStore(s, t->ball, ballOfSpv(s, u));
            spvStore(s, t->choice, u[3]);
            SpvId ballDir = LOAD_V3(t->ball);
            SpvId toSun = NORMALIZE(ADD_V3(spvShuffle3(s, frameVec(s, &t->r, F_TOSUN)), SCALE_V3(ballDir, Z(look))));

            /* the lights, the sun, and last, on a path under a picture of all around, a ray aimed at it (trace.c's; in
             * the lights' loop, so that the trees are walked for it where they are walked for lights) */
            SpvId alpha = MAX_F(MUL_F(m.rough, m.rough), FLOAT(1e-3f)), nv = MAX_F(DOT3(m.normal, view), FLOAT(1e-4f));
            SpvId most = maxOf3(s, m.mirror);
            SpvId chance = PICK_F(m.shiny, MIN_F(MAX_F(most, FLOAT(.05f)), FLOAT(.95f)), FLOAT(0.f));
            SpvId aims = BOTH(BOTH(paths, aroundOn), spvOp2(s, SPV_OP_ULessThan, s->boolType, LOAD_U(t->bounces), maxBounces));
            SpvId lightEnd = ADD_U(lightCount, spvSelect(s, s->uintType, aims, UINT(1), UINT(0)));
            spvStore(s, t->lightIndex, UINT(0));
            Loop lights;
            loopOpen(s, &lights);
            loopWhile(s, &lights, spvOp2(s, 178 /* OpULessThanEqual */, s->boolType, LOAD_U(t->lightIndex), lightEnd));
            {
                SpvId li = LOAD_U(t->lightIndex);
                SpvId isSun = IS_EQ(li, lightCount), isEnv = IS_EQ(li, ADD_U(lightCount, UINT(1)));
                spvStore(s, t->towardVar, zero3);
                spvStore(s, t->likelyVar, FLOAT(0.f));
                SpvId aimedMerge;
                spvIfBegin(s, isEnv, &aimedMerge);
                {
                    SpvId e[4], toward;
                    sobol4Of(s, ADD_U(done, LOAD_U(t->sampleIndex)), pathSeedOf(s, pixel, ADD_U(LOAD_U(t->bounces), UINT(65))), e);
                    spvStore(s, t->likelyVar, envAimSpv(s, &t->r, e[0], e[1], &toward));
                    spvStore(s, t->towardVar, toward);
                }
                spvIfEnd(s, aimedMerge);
                SpvId row = ADD_U(UINT(F_LIGHTS), MUL_U(spvGlsl2(s, SPV_GLSL_UMin, s->uintType, li, UINT(ARTF_FRAME_LIGHTS - 1)), UINT(F_LIGHT_ROWS)));
                SpvId lightA = frameVecAt(s, &t->r, row), lightB = frameVecAt(s, &t->r, ADD_U(row, UINT(1))), lightC = frameVecAt(s, &t->r, ADD_U(row, UINT(2)));
                /* (a light with a size: each path sees another point of it) */
                SpvId lightSize = PICK_F(isSun, FLOAT(0.f), frameFloatAt(s, &t->r, ADD_U(UINT(F_LIGHT_SIZES), SHR_U(spvGlsl2(s, SPV_GLSL_UMin, s->uintType, li, UINT(ARTF_FRAME_LIGHTS - 1)), 2)),
                                                                      AND_U(spvGlsl2(s, SPV_GLSL_UMin, s->uintType, li, UINT(ARTF_FRAME_LIGHTS - 1)), UINT(3))));
                SpvId toPoint = SUB_V3(ADD_V3(spvShuffle3(s, lightA), SCALE_V3(ballDir, lightSize)), lifted);
                SpvId share = FLOAT(1.f), special = 0, shapeC = 0, shapeD = 0, ci = spvGlsl2(s, SPV_GLSL_UMin, s->uintType, li, UINT(ARTF_FRAME_LIGHTS - 1));
                if (more) {
                    /* a shape: each path sees another point of it, as trace.c's shapedFrom */
                    SpvId shapeAt = ADD_U(UINT(F_SHAPES), MUL_U(ci, UINT(4)));
                    SpvId shapeA = frameVecAt(s, &t->r, shapeAt), shapeB = frameVecAt(s, &t->r, ADD_U(shapeAt, UINT(1)));
                    shapeC = frameVecAt(s, &t->r, ADD_U(shapeAt, UINT(2)));
                    shapeD = frameVecAt(s, &t->r, ADD_U(shapeAt, UINT(3)));
                    SpvId shaped = BOTH(NOT(EITHER(isSun, isEnv)), MORE_F(W(shapeA), FLOAT(0.f))), rect = BOTH(shaped, MORE_F(W(shapeB), FLOAT(0.f)));
                    special = BOTH(NOT(EITHER(isSun, isEnv)), EITHER(MORE_F(W(shapeA), FLOAT(0.f)), MORE_F(W(shapeC), FLOAT(0.f))));
                    spvStore(s, t->alongVar, FLOAT(0.f));
                    spvStore(s, t->acrossVar, FLOAT(0.f));
                    SpvId shapedMerge, rectMerge;
                    spvIfBegin(s, shaped, &shapedMerge);
                    spvStore(s, t->alongVar, MUL_F(SUB_F(MUL_F(traceRandom(t), FLOAT(2.f)), FLOAT(1.f)), W(shapeA)));
                    spvIfBegin(s, rect, &rectMerge);
                    spvStore(s, t->acrossVar, MUL_F(SUB_F(MUL_F(traceRandom(t), FLOAT(2.f)), FLOAT(1.f)), W(shapeB)));
                    spvIfEnd(s, rectMerge);
                    spvIfEnd(s, shapedMerge);
                    SpvId u = spvShuffle3(s, shapeA), facing = spvShuffle3(s, shapeB), along = LOAD_F(t->alongVar);
                    SpvId onRect = SUB_V3(ADD_V3(ADD_V3(spvShuffle3(s, lightA), SCALE_V3(u, along)), SCALE_V3(G_CROSS(facing, u), LOAD_F(t->acrossVar))), lifted);
                    toPoint = pick3(s, rect, onRect, ADD_V3(toPoint, SCALE_V3(u, along)));
                    share = PICK_F(rect, spvOp1(s, SPV_OP_FNegate, s->floatType, DIV_F(DOT3(toPoint, facing), SQRT_F(MAX_F(DOT3(toPoint, toPoint), FLOAT(1e-8f))))), FLOAT(1.f));
                }
                SpvId d2 = DOT3(toPoint, toPoint), distance = SQRT_F(MAX_F(d2, FLOAT(1e-8f)));
                SpvId radius = W(lightA);
                SpvId fade = MAX_F(SUB_F(FLOAT(1.f), DIV_F(d2, MUL_F(radius, radius))), FLOAT(0.f));
                SpvId toLamp = SCALE_V3(toPoint, DIV_F(FLOAT(1.f), distance));
                SpvId to = pick3(s, isEnv, LOAD_V3(t->towardVar), pick3(s, isSun, toSun, toLamp));
                SpvId spot = MUL_F(SUB_F(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(toLamp, spvShuffle3(s, lightC))), W(lightC)), W(lightB));
                if (more) {
                    /* (a shape's cone and profile from its middle) */
                    SpvId middle = NORMALIZE(SUB_V3(lifted, spvShuffle3(s, lightA)));
                    SpvId profile = PICK_F(MORE_F(W(shapeC), FLOAT(0.f)), profileAt(s, &t->r, t->extraLights, ci, spvShuffle3(s, shapeC), spvShuffle3(s, shapeD), middle), FLOAT(1.f));
                    SpvId fromMiddle = MUL_F(MUL_F(MIN_F(MAX_F(MUL_F(SUB_F(DOT3(middle, spvShuffle3(s, lightC)), W(lightC)), W(lightB)), FLOAT(0.f)), FLOAT(1.f)), profile), PICK_F(MORE_F(share, FLOAT(0.f)), FLOAT(1.f), FLOAT(0.f)));
                    spot = PICK_F(special, fromMiddle, MIN_F(MAX_F(spot, FLOAT(0.f)), FLOAT(1.f)));
                } else {
                    spot = MIN_F(MAX_F(spot, FLOAT(0.f)), FLOAT(1.f));
                }
                SpvId strength = PICK_F(isEnv, PICK_F(MORE_F(LOAD_F(t->likelyVar), FLOAT(0.f)), FLOAT(1.f), FLOAT(0.f)), PICK_F(isSun, FLOAT(1.f), MUL_F(MUL_F(fade, fade), spot)));
                SpvId color = pick3(s, isEnv, one3, pick3(s, isSun, spvShuffle3(s, frameVec(s, &t->r, F_SUNCOLOR)), spvShuffle3(s, lightB)));
                SpvId limit = PICK_F(EITHER(isSun, isEnv), FLOAT(3e37f), distance);
                SpvId ndl = DOT3(m.normal, to);
                SpvId lit;
                if (!more) {
                    spvIfBegin(s, BOTH(BOTH(MORE_F(ndl, FLOAT(0.f)), MORE_F(strength, FLOAT(0.f))), MORE_F(DOT3(color, color), FLOAT(0.f))), &lit);
                    {
                        lightThrough(t, lifted, to, limit);
                        SpvId lamp;
                        spvIfBegin(s, NOT(isEnv), &lamp);
                        {
                            SpvId highlight = lightHighlight(s, &m.reflecting, m.normal, view, to, ndl, FLOAT(1.f));
                            SpvId amount = MUL_F(strength, LOAD_F(t->through));
                            SpvId reflected = ADD_V3(SCALE_V3(m.albedo, ndl), highlight);
                            spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), SCALE_V3(MUL_V3(MUL_V3(LOAD_V3(t->path), color), reflected), amount)));
                        }
                        spvIfEnd(s, lamp);
                        aimedLightSpv(t, &m, isEnv, view, to, alpha, nv, most, chance, environmentLight);
                    }
                    spvIfEnd(s, lit);
                } else {
                    /* as lightFromMore: the coat's lobe by its normal, Charlie's sheen, highlights drawn out, light under
                     * the surface that wraps past where it turns from the light */
                    SpvId under = NOT_EQ(AND_U(m.flags, UINT(ARTF_MORE_SUBSURFACE)), UINT(0)), shining = BOTH(MORE_F(strength, FLOAT(0.f)), MORE_F(DOT3(color, color), FLOAT(0.f)));
                    spvIfBegin(s, BOTH(shining, MORE_F(ndl, FLOAT(0.f))), &lit);
                    {
                        lightThrough(t, lifted, to, limit);
                        aimedLightSpv(t, &m, isEnv, view, to, alpha, nv, most, chance, environmentLight);
                        SpvId lamp;
                        spvIfBegin(s, NOT(isEnv), &lamp);
                        SpvId upon = MAX_F(ndl, FLOAT(0.f));
                        SpvId highlight = SCALE_V3(lightHighlight(s, &m.reflecting, m.normal, view, to, upon, FLOAT(1.f)), m.under);
                        SpvId coatNdl = MAX_F(DOT3(m.coatNormal, to), FLOAT(0.f));
                        highlight = ADD_V3(highlight, SCALE_V3(lightHighlight(s, &m.coating, m.coatNormal, view, to, coatNdl, FLOAT(1.f)), m.coat));
                        SpvId sheen = MUL_F(shaderCharlie(s, MAX_F(DOT3(m.normal, shaderHalfway(s, view, to)), FLOAT(0.f)), upon, m.reflecting.lobe.ndv, m.sheenInverse), upon);
                        highlight = ADD_V3(highlight, SCALE_V3(m.sheen, PICK_F(MORE_F(ndl, FLOAT(0.f)), sheen, FLOAT(0.f))));
                        SpvId wrap = shaderWrap(s, ndl, upon);
                        SpvId reflected = ADD_V3(SCALE_V3(ADD_V3(SCALE_V3(m.albedo, upon), SCALE_V3(m.subsurface, wrap)), share), highlight);
                        spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), SCALE_V3(MUL_V3(MUL_V3(LOAD_V3(t->path), color), reflected), MUL_F(strength, LOAD_F(t->through)))));
                        spvIfEnd(s, lamp);
                    }
                    spvIfEnd(s, lit);
                    SpvId behind;
                    spvIfBegin(s, BOTH(BOTH(BOTH(shining, under), NOT(isEnv)), LESS_F(ndl, FLOAT(0.f))), &behind);
                    {
                        /* from just under the surface, toward the light: what is inside, then what is past it */
                        SpvId close = reachOf(s, lifted);
                        SpvId from = SUB_V3(lifted, SCALE_V3(m.normal, MUL_F(close, FLOAT(3.f))));
                        closest(t, from, to, limit, true);
                        SpvId inside = PICK_F(IS_EQ(LOAD_U(t->bestInstance), UINT(0xFFFFFFFFu)), FLOAT(0.f), LOAD_F(t->bestT));
                        lightThrough(t, ADD_V3(from, SCALE_V3(to, ADD_F(inside, close))), to, SUB_F(limit, inside));
                        SpvId kept = MUL_F(MUL_F(LOAD_F(t->through), exp2Of(s, spvOp1(s, SPV_OP_FNegate, s->floatType, MUL_F(inside, m.reach)))), spvOp1(s, SPV_OP_FNegate, s->floatType, ndl));
                        spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), SCALE_V3(MUL_V3(MUL_V3(LOAD_V3(t->path), color), m.subsurface), MUL_F(MUL_F(kept, share), strength))));
                    }
                    spvIfEnd(s, behind);
                }
                spvStore(s, t->lightIndex, ADD_U(li, UINT(1)));
            }
            loopClose(s, &lights);

            SpvId mirror = SUB_V3(d, SCALE_V3(m.normal, MUL_F(FLOAT(2.f), DOT3(d, m.normal))));
            SpvId last = spvOp2(s, SPV_OP_ULessThan, s->boolType, LOAD_U(t->bounces), maxBounces);
            last = NOT(last);

            /* rays: light from all around, and a mirror for what reflects */
            SpvId share = ADD_F(MUL_F(Y(m.normal), FLOAT(.5f)), FLOAT(.5f));
            SpvId ambient = shaderSkyGround3(s, ground, sky, share);
            SpvId reflectedUp = ADD_F(MUL_F(Y(mirror), FLOAT(.5f)), FLOAT(.5f));
            SpvId reflectedSky = shaderSkyGround3(s, ground, sky, reflectedUp);
            SpvId rayMode, rayAround;
            spvIfBegin(s, BOTH(NOT(paths), NOT(aroundOn)), &rayMode);
            {
                SpvId here = SCALE_V3(MUL_V3(m.albedo, ambient), m.open);
                SpvId ending = SCALE_V3(MUL_V3(m.mirror, reflectedSky), m.open);
                SpvId both = pick3(s, BOTH(last, m.shiny), ADD_V3(here, ending), here);
                spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), MUL_V3(LOAD_V3(t->path), both)));
            }
            spvIfEnd(s, rayMode);
            spvIfBegin(s, BOTH(NOT(paths), aroundOn), &rayAround);
            {
                SpvId here = SCALE_V3(MUL_V3(m.albedo, SCALE_V3(envSample(s, &t->r, UINT(ARTF_ENV_DIFFUSE), m.normal), environmentLight)), m.open);
                SpvId ending = SCALE_V3(MUL_V3(m.mirror, SCALE_V3(envReflected(s, &t->r, m.rough, mirror), environmentLight)), m.open);
                SpvId both = pick3(s, BOTH(last, m.shiny), ADD_V3(here, ending), here);
                spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), MUL_V3(LOAD_V3(t->path), both)));
            }
            spvIfEnd(s, rayAround);
            breakIf(s, &steps, EITHER(last, BOTH(NOT(paths), NOT(m.shiny))));
            SpvId isMirror = EITHER(NOT(paths), LESS_F(LOAD_F(t->choice), chance));
            /* (a point on a sphere over the surface: as many rays as the cosine says, exactly) */
            SpvId diffuse = ADD_V3(SCALE_V3(m.normal, FLOAT(1.0001f)), directionOfSpv(s, u));
            spvStore(s, t->bounceDir, diffuse);
            spvStore(s, t->keptVar, FLOAT(0.f));
            spvStore(s, t->shineVar, FLOAT(0.f));
            SpvId glossyMerge;
            spvIfBegin(s, isMirror, &glossyMerge);
            {
                SpvId lambdaView = ggxLambdaSpv(s, alpha, nv);
                SpvId facet = visibleNormalSpv(s, m.normal, view, alpha, u);
                SpvId glossy = SUB_V3(d, SCALE_V3(facet, MUL_F(FLOAT(2.f), DOT3(d, facet))));
                SpvId glossyCos = DOT3(m.normal, glossy);
                spvStore(s, t->bounceDir, glossy);
                spvStore(s, t->keptVar, PICK_F(MORE_F(glossyCos, FLOAT(0.f)), DIV_F(ADD_F(FLOAT(1.f), lambdaView), ADD_F(ADD_F(FLOAT(1.f), lambdaView), ggxLambdaSpv(s, alpha, glossyCos))), FLOAT(0.f)));
                spvStore(s, t->shineVar, DIV_F(MUL_F(chance, ggxDSpv(s, alpha, MAX_F(DOT3(m.normal, facet), FLOAT(0.f)))), MUL_F(MUL_F(ADD_F(FLOAT(1.f), lambdaView), FLOAT(4.f)), nv)));
            }
            spvIfEnd(s, glossyMerge);
            SpvId next = NORMALIZE(LOAD_V3(t->bounceDir)), kept = LOAD_F(t->keptVar);
            spvStore(s, t->diffuseLikely, PICK_F(isMirror, LOAD_F(t->shineVar), MUL_F(MUL_F(SUB_F(FLOAT(1.f), chance), MAX_F(DOT3(next, m.normal), FLOAT(0.f))), FLOAT(1.f / 3.14159265f))));
            spvStore(s, t->coneSpread, ADD_F(LOAD_F(t->coneSpread), PICK_F(isMirror, MUL_F(FLOAT(2.f), alpha), FLOAT(1.f))));
            SpvId keep = DIV_F(MUL_F(m.open, SUB_F(FLOAT(1.f), most)), SUB_F(FLOAT(1.f), chance));
            SpvId tint = pick3(s, isMirror, pick3(s, paths, SCALE_V3(m.mirror, DIV_F(kept, MAX_F(chance, FLOAT(1e-6f)))), SCALE_V3(m.mirror, kept)), SCALE_V3(m.albedo, keep));
            spvStore(s, t->path, MUL_V3(LOAD_V3(t->path), tint));
            spvStore(s, t->direction, next);
            breakIf(s, &steps, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, DOT3(next, m.normal), FLOAT(0.f)));
            SpvId count = ADD_U(LOAD_U(t->bounces), UINT(1));
            spvStore(s, t->bounces, count);
            spvStore(s, t->origin, lifted);
            /* past a few bounces, what brings little ends now and then */
            SpvId roulette;
            spvIfBegin(s, LESS_U(UINT(3), count), &roulette);
            {
                SpvId now = LOAD_V3(t->path);
                SpvId keep = MIN_F(MAX_F(MAX_F(X(now), MAX_F(Y(now), Z(now))), FLOAT(.05f)), FLOAT(1.f));
                breakIf(s, &steps, spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, traceRandom(t), keep));
                spvStore(s, t->path, SCALE_V3(now, DIV_F(FLOAT(1.f), keep)));
            }
            spvIfEnd(s, roulette);
        }
        spvIfEnd(s, stops);
        loopClose(s, &steps);

        SpvId covered = NOT_EQ(LOAD_U(t->covered), UINT(0)), kept;
        spvIfBegin(s, covered, &kept);
        {
            SpvId limit = vec3Of(s, FLOAT(64.f), FLOAT(64.f), FLOAT(64.f));
            SpvId gathered = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, LOAD_V3(t->light), limit);
            gathered = ADD_V3(fog, SCALE_V3(SUB_V3(gathered, fog), LOAD_F(t->keep)));
            const SpvId idx[2] = {t->r.zero, pixel};
            SpvId slot = spvAccess(s, t->r.ptrStorageVec4, t->sum, idx, 2);
            spvStore(s, slot, ADD_V4(spvLoad(s, s->vec4, slot), vec4Of(s, gathered, FLOAT(1.f))));
        }
        spvIfEnd(s, kept);
        spvStore(s, t->sampleIndex, ADD_U(LOAD_U(t->sampleIndex), UINT(1)));
    }
    loopClose(s, &sampleLoop);
    if (guides) {
        const SpvId idx[2] = {t->r.zero, pixel};
        spvStore(s, spvAccess(s, t->r.ptrStorageVec4, t->guide, idx, 2), vec4Of(s, LOAD_V3(t->guidePlace), spvOp1(s, SPV_OP_Bitcast, s->floatType, LOAD_U(t->guideBits))));
        spvStore(s, spvAccess(s, t->r.ptrStorageVec4, t->albedoGuide, idx, 2), vec4Of(s, LOAD_V3(t->guideColor), TO_F(LOAD_U(t->guideInstance))));
        spvStore(s, spvAccess(s, t->r.ptrStorageVec4, t->shiftGuide, idx, 2), vec4Of(s, LOAD_V3(t->guideShift), FLOAT(0.f)));
    }
    spvIfEnd(s, inFrame);
    spvEndMain(s);

    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &global, 1);
    spvExecutionMode(s, main, 17 /* LocalSize */, 8, 8, 1, 3);
    return finish(s, size);
}

/* ---- light that bounced: the probes gathered on the GPU, as trace.c's probeJob ----
 *
 * A group of 64 for each probe. One of them draws the rays' directions in turn, from the same random numbers as the
 * CPU's, into what the group shares; each then follows a ray, 64 at a time, through the trees or the hardware, with
 * the tracer's code; then each adds up, for its texel of the map of how far the probe sees, the rays near it, and one
 * adds up their light, both in the CPU's order. 62: what the probes held, read; 70: what they hold now, written. */

_Static_assert(ARTF_PROBE_SIDE == 8, "a probe's map is 8 x 8: an invocation's texel is at (i & 7, i >> 3)");

static void groupBarrier(Spv* s) { spvEmit(&s->code, 224 /* OpControlBarrier */, SPV_WORDS(UINT(2), UINT(2), UINT(0x108))); /* the group's, acquire and release, its memory */ }

/* As artfIndirectAt, from the probes in `probes` (62): light from all around a surface at p facing n, over pi. */
static SpvId indirectAtSpv(const Tracer* t, SpvId probes, SpvId p, SpvId n, bool walls, SpvId sumVar, SpvId weightsVar, SpvId counter) {
    Spv* s = t->s;
    const Resources* r = &t->r;
    SpvId origin = spvShuffle3(s, frameVec(s, r, F_INDIRECT)), spacing = spvShuffle3(s, frameVec(s, r, F_INDIRECT + 1)), dims = spvShuffle3(s, frameVec(s, r, F_INDIRECT + 2));
    /* (a little out along the normal: a probe on the surface's own side counts more) */
    SpvId q = ADD_V3(p, SCALE_V3(MUL_V3(n, spacing), FLOAT(.25f)));
    SpvId at = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, spvOp2(s, SPV_OP_FDiv, s->vec3, SUB_V3(q, origin), spacing), same3(s, 0.f)), SUB_V3(dims, same3(s, 1.f)));
    SpvId base = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl1(s, SPV_GLSL_Floor, s->vec3, at), SUB_V3(dims, same3(s, 2.f)));
    SpvId w = SUB_V3(at, base);
    SpvId dx = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, X(dims)), dy = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Y(dims)), dz = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Z(dims));
    SpvId bx = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, X(base)), by = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Y(base)), bz = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Z(base));
    SpvId count = MUL_U(MUL_U(dx, dy), dz);
    spvStore(s, sumVar, same3(s, 0.f));
    spvStore(s, weightsVar, FLOAT(0.f));
    spvStore(s, counter, UINT(0));
    Loop corners;
    loopOpen(s, &corners);
    loopWhile(s, &corners, LESS_U(LOAD_U(counter), UINT(8)));
    {
        SpvId c = LOAD_U(counter);
        SpvId cx = AND_U(c, UINT(1)), cy = AND_U(SHR_U(c, 1), UINT(1)), cz = SHR_U(c, 2);
        SpvId x = ADD_U(bx, cx), y = ADD_U(by, cy), z = ADD_U(bz, cz);
#define SIDE_OF(bit, f) PICK_F(NOT_EQ(bit, UINT(0)), f, SUB_F(FLOAT(1.f), f))
        SpvId weight = MUL_F(MUL_F(SIDE_OF(cx, X(w)), SIDE_OF(cy, Y(w))), SIDE_OF(cz, Z(w)));
#undef SIDE_OF
        SpvId index = ADD_U(MUL_U(ADD_U(MUL_U(z, dy), y), dx), x), row = MUL_U(index, UINT(4));
        if (walls) {
            /* from the probe to the point: one behind the surface counts less, and one that cannot see it hardly at all */
            SpvId there = ADD_V3(origin, MUL_V3(vec3Of(s, TO_F(x), TO_F(y), TO_F(z)), spacing));
            SpvId dir = SUB_V3(q, there);
            SpvId distance = SQRT_F(DOT3(dir, dir));
            SpvId inv = PICK_F(MORE_F(distance, FLOAT(1e-6f)), DIV_F(FLOAT(1.f), MAX_F(distance, FLOAT(1e-6f))), FLOAT(0.f));
            SpvId facing = MUL_F(SUB_F(FLOAT(1.f), MUL_F(DOT3(dir, n), inv)), FLOAT(.5f));
            weight = MUL_F(weight, ADD_F(MUL_F(facing, facing), FLOAT(.2f)));
            SpvId mean, square;
            probeReachSpv(s, r, probes, ADD_U(MUL_U(count, UINT(16)), MUL_U(index, UINT(ARTF_PROBE_MAP))), dir, &mean, &square);
            SpvId variance = MAX_F(SUB_F(square, MUL_F(mean, mean)), FLOAT(1e-4f)), past = SUB_F(distance, mean);
            SpvId seen = DIV_F(variance, ADD_F(variance, MUL_F(past, past)));
            weight = PICK_F(MORE_F(distance, mean), MUL_F(weight, MAX_F(MUL_F(MUL_F(seen, seen), seen), FLOAT(.002f))), weight);
        }
        spvStore(s, weightsVar, ADD_F(LOAD_F(weightsVar), weight));
        SpvId c0 = spvShuffle3(s, bufferVec(s, r, probes, row));
        SpvId lean = vec3Of(s, DOT3(spvShuffle3(s, bufferVec(s, r, probes, ADD_U(row, UINT(1)))), n), DOT3(spvShuffle3(s, bufferVec(s, r, probes, ADD_U(row, UINT(2)))), n),
                            DOT3(spvShuffle3(s, bufferVec(s, r, probes, ADD_U(row, UINT(3)))), n));
        spvStore(s, sumVar, ADD_V3(LOAD_V3(sumVar), SCALE_V3(spvGlsl2(s, SPV_GLSL_FMax, s->vec3, ADD_V3(c0, lean), same3(s, 0.f)), weight)));
        spvStore(s, counter, ADD_U(c, UINT(1)));
    }
    loopClose(s, &corners);
    SpvId weights = LOAD_F(weightsVar);
    return SCALE_V3(LOAD_V3(sumVar), PICK_F(MORE_F(weights, FLOAT(1e-6f)), DIV_F(FLOAT(1.f), MAX_F(weights, FLOAT(1e-6f))), FLOAT(0.f)));
}

/* As lightFrom with a path of 1: a light toward `to` (of length 1), as far as limit, adds to `light` (a variable) what
 * the surface reflects of it toward view, as much as what is in the way lets through. */
static void probeLightFrom(const Tracer* t, const Met* m, SpvId lifted, SpvId view, SpvId to, SpvId limit, SpvId strength, SpvId color, SpvId on, SpvId light) {
    Spv* s = t->s;
    SpvId ndl = DOT3(m->normal, to), lit;
    spvIfBegin(s, BOTH(on, BOTH(MORE_F(ndl, FLOAT(0.f)), MORE_F(strength, FLOAT(0.f)))), &lit);
    {
        lightThrough(t, lifted, to, limit);
        /* (its highlight's lobe never drawn out: lightFrom's is not) */
        Reflecting plain = m->reflecting;
        plain.lobe.drawn = 0;
        SpvId highlight = pick3(s, m->shiny, lightHighlight(s, &plain, m->normal, view, to, ndl, FLOAT(1.f)), same3(s, 0.f));
        SpvId reflected = ADD_V3(SCALE_V3(m->albedo, ndl), highlight);
        spvStore(s, light, ADD_V3(LOAD_V3(light), SCALE_V3(MUL_V3(color, reflected), MUL_F(strength, LOAD_F(t->through)))));
    }
    spvIfEnd(s, lit);
}

uint32_t* artfShaderProbes(size_t* size, uint32_t maxTextures, const ArtfGraphData* graphs, uint32_t graphCount, bool hardware, bool more, bool walls, int rays, float keep) {
    enum { GROUP = ARTF_PROBE_SIDE * ARTF_PROBE_SIDE }; /* (one for each texel of a probe's map) */
    Spv b, *s = &b;
    spvInit(s);
    spvCapability(s, 5301); /* ShaderNonUniform */
    spvCapability(s, 5307); /* SampledImageArrayNonUniformIndexing */
    spvCapability(s, 50); /* ImageQuery: a texture's size, for the tracer's cones */
    if (hardware) spvCapability(s, 4472 /* RayQueryKHR: capabilities come before extensions */);
    spvExtension(s, "SPV_EXT_descriptor_indexing");
    Resources r;
    declare(s, &r, false);
    SpvId imageType, sampled = spvTypeSampledImage2D(s, &imageType);
    SpvId textures = spvResource(s, spvTypeArray(s, sampled, maxTextures, 0), SPV_STORAGE_UniformConstant, 6);
    Tracer tracer, *t = &tracer;
    tracerDeclare(s, t, &r, textures, sampled, graphs, graphCount, hardware);
    t->more = more;
    SpvId probes = spvResource(s, blockOf(s, r.vec4Array, false), SPV_STORAGE_StorageBuffer, 62);
    SpvId next = spvResource(s, blockOf(s, r.floatArray, true), SPV_STORAGE_StorageBuffer, 70);
    SpvId uvec3 = spvTypeVector(s, s->uintType, 3);
    SpvId groupVar = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, uvec3), SPV_STORAGE_Input), localVar = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, uvec3), SPV_STORAGE_Input);
    spvDecorate(s, groupVar, SPV_DECORATE_BuiltIn, 26 /* WorkgroupId */);
    spvDecorate(s, localVar, SPV_DECORATE_BuiltIn, 27 /* LocalInvocationId */);
    SpvId pushBlock = spvTypeStruct(s, &s->uintType, 1);
    spvDecorate(s, pushBlock, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, pushBlock, 0, SPV_DECORATE_Offset, 0);
    SpvId push = spvVariable(s, spvTypePointer(s, SPV_STORAGE_PushConstant, pushBlock), SPV_STORAGE_PushConstant);
    /* what the group shares: of each ray, its direction and how far it went, and its light */
    SpvId shared = spvTypeArray(s, s->vec4, GROUP, 0), ptrShared = spvTypePointer(s, 4 /* Workgroup */, s->vec4);
    SpvId rayDir = spvVariable(s, spvTypePointer(s, 4, shared), 4), rayLight = spvVariable(s, spvTypePointer(s, 4, shared), 4);

    SpvId main = spvBeginMain(s);
    tracerVariables(t);
    SpvId ptrU = spvTypePointer(s, SPV_STORAGE_Function, s->uintType), ptrF = spvTypePointer(s, SPV_STORAGE_Function, s->floatType), ptr3 = spvTypePointer(s, SPV_STORAGE_Function, s->vec3);
    t->state = VAR(ptrU);
    SpvId chunkVar = VAR(ptrU), jVar = VAR(ptrU), tryVar = VAR(ptrU), acceptedVar = VAR(ptrU), lightIndex = VAR(ptrU), counter = VAR(ptrU);
    SpvId ballVar = VAR(ptr3), lightVar = VAR(ptr3), sumVar = VAR(ptr3), meanVar = VAR(ptr3), leanVar[3];
    for (int k = 0; k < 3; ++k) leanVar[k] = VAR(ptr3);
    SpvId weightsVar = VAR(ptrF), reachVar = VAR(ptrF), reach2Var = VAR(ptrF), reachWeightVar = VAR(ptrF);

    SpvId group = spvLoad(s, uvec3, groupVar), lid = spvExtract(s, s->uintType, spvLoad(s, uvec3, localVar), 0);
    SpvId gx = spvExtract(s, s->uintType, group, 0), gy = spvExtract(s, s->uintType, group, 1);
    SpvId frames = spvLoad(s, s->uintType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_PushConstant, s->uintType), push, &r.zero, 1));
    SpvId origin = spvShuffle3(s, frameVec(s, &r, F_INDIRECT)), spacing = spvShuffle3(s, frameVec(s, &r, F_INDIRECT + 1)), dims = spvShuffle3(s, frameVec(s, &r, F_INDIRECT + 2));
    SpvId dx = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, X(dims)), dy = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Y(dims)), dz = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Z(dims));
    SpvId count = MUL_U(MUL_U(dx, dy), dz), index = ADD_U(MUL_U(gy, MUL_U(dx, dy)), gx);
    /* (the groups: dims[0] x dims[1] across, dims[2] down) */
    SpvId o = ADD_V3(origin, MUL_V3(vec3Of(s, TO_F(spvOp2(s, SPV_OP_UMod, s->uintType, gx, dx)), TO_F(spvOp2(s, SPV_OP_UDiv, s->uintType, gx, dx)), TO_F(gy)), spacing));
    SpvId most = MUL_F(SQRT_F(DOT3(spacing, spacing)), FLOAT(1.5f));
    /* this one's texel: the direction at its middle, unfolded */
    SpvId toward;
    {
        SpvId a = SUB_F(MUL_F(DIV_F(ADD_F(TO_F(AND_U(lid, UINT(ARTF_PROBE_SIDE - 1))), FLOAT(.5f)), FLOAT((float)ARTF_PROBE_SIDE)), FLOAT(2.f)), FLOAT(1.f));
        SpvId c = SUB_F(MUL_F(DIV_F(ADD_F(TO_F(SHR_U(lid, 3)), FLOAT(.5f)), FLOAT((float)ARTF_PROBE_SIDE)), FLOAT(2.f)), FLOAT(1.f));
        SpvId z = SUB_F(SUB_F(FLOAT(1.f), ABS_F(a)), ABS_F(c));
        SpvId below = LESS_F(z, FLOAT(0.f));
#define SIGN_OF(v) PICK_F(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, v, FLOAT(0.f)), FLOAT(1.f), FLOAT(-1.f))
        SpvId fa = MUL_F(SUB_F(FLOAT(1.f), ABS_F(c)), SIGN_OF(a)), fc = MUL_F(SUB_F(FLOAT(1.f), ABS_F(a)), SIGN_OF(c));
#undef SIGN_OF
        a = PICK_F(below, fa, a);
        c = PICK_F(below, fc, c);
        SpvId length = SQRT_F(ADD_F(ADD_F(MUL_F(a, a), MUL_F(c, c)), MUL_F(z, z)));
        toward = vec3Of(s, DIV_F(a, length), DIV_F(c, length), DIV_F(z, length));
    }
    SpvId first = IS_EQ(lid, UINT(0));
    spvStore(s, reachVar, FLOAT(0.f));
    spvStore(s, reach2Var, FLOAT(0.f));
    spvStore(s, reachWeightVar, FLOAT(0.f));
    spvStore(s, meanVar, same3(s, 0.f));
    for (int k = 0; k < 3; ++k) spvStore(s, leanVar[k], same3(s, 0.f));
    spvStore(s, t->state, traceHash(s, ADD_U(ADD_U(MUL_U(index, UINT(9781u)), MUL_U(frames, UINT(6271u))), UINT(1u))));
    SpvId env = MORE_F(W(frameVec(s, &r, F_SKY)), FLOAT(0.f));
    SpvId sunColor = spvShuffle3(s, frameVec(s, &r, F_SUNCOLOR)), toSun = spvShuffle3(s, frameVec(s, &r, F_TOSUN));
    SpvId sunOn = MORE_F(maxOf3(s, sunColor), FLOAT(0.f));
    SpvId lightCount = AS_UINT(X(frameVec(s, &r, F_COUNTS)));

    spvStore(s, chunkVar, UINT(0));
    Loop chunks;
    loopOpen(s, &chunks);
    loopWhile(s, &chunks, LESS_U(LOAD_U(chunkVar), UINT((uint32_t)rays)));
    {
        SpvId chunk = LOAD_U(chunkVar);
        SpvId here = UMIN(SUB_U(UINT((uint32_t)rays), chunk), UINT(GROUP)), merge;
        /* the directions, by one, as randomInBall and normalize3: the CPU's random numbers, in its order */
        spvIfBegin(s, first, &merge);
        {
            spvStore(s, jVar, UINT(0));
            Loop each;
            loopOpen(s, &each);
            loopWhile(s, &each, LESS_U(LOAD_U(jVar), here));
            {
                spvStore(s, acceptedVar, UINT(0));
                spvStore(s, tryVar, UINT(0));
                Loop tries;
                loopOpen(s, &tries);
                loopWhile(s, &tries, LESS_U(LOAD_U(tryVar), UINT(16)));
                {
                    SpvId bx = SUB_F(MUL_F(traceRandom(t), FLOAT(2.f)), FLOAT(1.f)), by = SUB_F(MUL_F(traceRandom(t), FLOAT(2.f)), FLOAT(1.f)), bz = SUB_F(MUL_F(traceRandom(t), FLOAT(2.f)), FLOAT(1.f));
                    SpvId v = vec3Of(s, bx, by, bz);
                    spvStore(s, ballVar, v);
                    SpvId inside = spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, DOT3(v, v), FLOAT(1.f));
                    spvStore(s, acceptedVar, spvSelect(s, s->uintType, inside, UINT(1), UINT(0)));
                    breakIf(s, &tries, inside);
                    spvStore(s, tryVar, ADD_U(LOAD_U(tryVar), UINT(1)));
                }
                loopClose(s, &tries);
                SpvId d = pick3(s, NOT_EQ(LOAD_U(acceptedVar), UINT(0)), LOAD_V3(ballVar), same3(s, 0.f));
                d = SCALE_V3(d, DIV_F(FLOAT(1.f), SQRT_F(MAX_F(DOT3(d, d), FLOAT(1e-30f)))));
                SpvId j = LOAD_U(jVar);
                spvStore(s, spvAccess(s, ptrShared, rayDir, &j, 1), vec4Of(s, d, FLOAT(0.f)));
                spvStore(s, jVar, ADD_U(j, UINT(1)));
            }
            loopClose(s, &each);
        }
        spvIfEnd(s, merge);
        groupBarrier(s);
        /* each its ray: what it meets, how far, and what light comes back along it */
        spvIfBegin(s, LESS_U(lid, here), &merge);
        {
            SpvId d = spvShuffle3(s, spvLoad(s, s->vec4, spvAccess(s, ptrShared, rayDir, &lid, 1)));
            closest(t, o, d, FLOAT(3e37f), true);
            SpvId hit = NOT_EQ(LOAD_U(t->bestInstance), UINT(0xFFFFFFFFu));
            SpvId bestT = LOAD_F(t->bestT);
            SpvId far = PICK_F(hit, MIN_F(bestT, most), most);
            SpvId plain = shaderSkyGround3(s, spvShuffle3(s, frameVec(s, &r, F_GROUND)), spvShuffle3(s, frameVec(s, &r, F_SKY)), ADD_F(MUL_F(Y(d), FLOAT(.5f)), FLOAT(.5f)));
            spvStore(s, lightVar, pick3(s, hit, same3(s, 0.f), plain));
            SpvId met;
            spvIfBegin(s, BOTH(NOT(hit), env), &met);
            spvStore(s, lightVar, SCALE_V3(envSample(s, &r, UINT(ARTF_ENV_DIFFUSE - 1), d), W(frameVec(s, &r, F_SKY))));
            spvIfEnd(s, met);
            spvIfBegin(s, hit, &met);
            {
                Met m = surfaceAt(t, o, d, FLOAT(0.f));
                /* (a probe inside something sees its backs: it takes nothing from them) */
                SpvId front;
                spvIfBegin(s, m.front, &front);
                {
                    SpvId p = ADD_V3(o, SCALE_V3(d, bestT));
                    SpvId lifted = ADD_V3(p, SCALE_V3(m.normal, reachOf(s, p)));
                    SpvId view = spvOp1(s, SPV_OP_FNegate, s->vec3, d);
                    spvStore(s, lightVar, m.emissive);
                    probeLightFrom(t, &m, lifted, view, toSun, FLOAT(3e37f), FLOAT(1.f), sunColor, sunOn, lightVar);
                    spvStore(s, lightIndex, UINT(0));
                    Loop lights;
                    loopOpen(s, &lights);
                    loopWhile(s, &lights, LESS_U(LOAD_U(lightIndex), lightCount));
                    {
                        SpvId li = LOAD_U(lightIndex);
                        SpvId row = ADD_U(UINT(F_LIGHTS), MUL_U(li, UINT(F_LIGHT_ROWS)));
                        SpvId lightA = frameVecAt(s, &r, row), lightB = frameVecAt(s, &r, ADD_U(row, UINT(1))), lightC = frameVecAt(s, &r, ADD_U(row, UINT(2)));
                        SpvId toLight = SUB_V3(spvShuffle3(s, lightA), lifted);
                        SpvId d2 = DOT3(toLight, toLight), distance = SQRT_F(MAX_F(d2, FLOAT(1e-8f)));
                        SpvId fade = MAX_F(SUB_F(FLOAT(1.f), DIV_F(d2, MUL_F(W(lightA), W(lightA)))), FLOAT(0.f));
                        SpvId to = spvOp2(s, SPV_OP_FDiv, s->vec3, toLight, vec3Of(s, distance, distance, distance));
                        SpvId spot = MIN_F(MAX_F(MUL_F(SUB_F(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(to, spvShuffle3(s, lightC))), W(lightC)), W(lightB)), FLOAT(0.f)), FLOAT(1.f));
                        probeLightFrom(t, &m, lifted, view, to, distance, MUL_F(MUL_F(fade, fade), spot), spvShuffle3(s, lightB), spvConstBool(s, true), lightVar);
                        spvStore(s, lightIndex, ADD_U(li, UINT(1)));
                    }
                    loopClose(s, &lights);
                    /* what bounced to it before: the probes, as the last step left them */
                    SpvId around = indirectAtSpv(t, probes, p, m.normal, walls, sumVar, weightsVar, counter);
                    spvStore(s, lightVar, ADD_V3(LOAD_V3(lightVar), MUL_V3(m.albedo, around)));
                }
                spvIfEnd(s, front);
            }
            spvIfEnd(s, met);
            SpvId light = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, LOAD_V3(lightVar), same3(s, 64.f)); /* (trace.c's LIGHT_LIMIT) */
            spvStore(s, spvAccess(s, ptrShared, rayDir, &lid, 1), vec4Of(s, d, far));
            spvStore(s, spvAccess(s, ptrShared, rayLight, &lid, 1), vec4Of(s, light, FLOAT(0.f)));
        }
        spvIfEnd(s, merge);
        groupBarrier(s);
        /* each its texel: how far the rays near its direction went (a narrow lobe each: the cosine to the 32nd) */
        spvStore(s, jVar, UINT(0));
        Loop texel;
        loopOpen(s, &texel);
        loopWhile(s, &texel, LESS_U(LOAD_U(jVar), here));
        {
            SpvId j = LOAD_U(jVar);
            SpvId ray = spvLoad(s, s->vec4, spvAccess(s, ptrShared, rayDir, &j, 1));
            SpvId far = W(ray), w = MAX_F(DOT3(spvShuffle3(s, ray), toward), FLOAT(0.f));
            for (int k = 0; k < 5; ++k) w = MUL_F(w, w);
            spvStore(s, reachVar, ADD_F(LOAD_F(reachVar), MUL_F(w, far)));
            spvStore(s, reach2Var, ADD_F(LOAD_F(reach2Var), MUL_F(MUL_F(w, far), far)));
            spvStore(s, reachWeightVar, ADD_F(LOAD_F(reachWeightVar), w));
            spvStore(s, jVar, ADD_U(j, UINT(1)));
        }
        loopClose(s, &texel);
        /* and one, the light: its mean, and how it leans */
        spvIfBegin(s, first, &merge);
        {
            spvStore(s, jVar, UINT(0));
            Loop each;
            loopOpen(s, &each);
            loopWhile(s, &each, LESS_U(LOAD_U(jVar), here));
            {
                SpvId j = LOAD_U(jVar);
                SpvId d = spvShuffle3(s, spvLoad(s, s->vec4, spvAccess(s, ptrShared, rayDir, &j, 1)));
                SpvId light = spvShuffle3(s, spvLoad(s, s->vec4, spvAccess(s, ptrShared, rayLight, &j, 1)));
                spvStore(s, meanVar, ADD_V3(LOAD_V3(meanVar), light));
                for (uint32_t k = 0; k < 3; ++k) spvStore(s, leanVar[k], ADD_V3(LOAD_V3(leanVar[k]), SCALE_V3(d, spvExtract(s, s->floatType, light, k))));
                spvStore(s, jVar, ADD_U(j, UINT(1)));
            }
            loopClose(s, &each);
        }
        spvIfEnd(s, merge);
        groupBarrier(s);
        spvStore(s, chunkVar, ADD_U(chunk, UINT(GROUP)));
    }
    loopClose(s, &chunks);

    /* mixed with what the probe had: none of it, the first step */
    SpvId kept = PICK_F(NOT_EQ(frames, UINT(0)), FLOAT(keep), FLOAT(0.f)), taken = SUB_F(FLOAT(1.f), kept);
    SpvId each = FLOAT(1.f / (float)rays);
#define NEXT_AT(i) spvAccess(s, r.ptrStorageFloat, next, (const SpvId[2]){r.zero, (i)}, 2)
    SpvId wrote;
    spvIfBegin(s, first, &wrote);
    {
        SpvId at = MUL_U(index, UINT(16)), was[4];
        for (uint32_t k = 0; k < 4; ++k) was[k] = bufferVec(s, &r, probes, ADD_U(MUL_U(index, UINT(4)), UINT(k)));
        SpvId mean = LOAD_V3(meanVar);
        for (uint32_t k = 0; k < 3; ++k) {
            spvStore(s, NEXT_AT(ADD_U(at, UINT(k))), ADD_F(MUL_F(spvExtract(s, s->floatType, was[0], k), kept), MUL_F(MUL_F(spvExtract(s, s->floatType, mean, k), each), taken)));
            SpvId lean = LOAD_V3(leanVar[k]);
            for (uint32_t a = 0; a < 3; ++a)
                spvStore(s, NEXT_AT(ADD_U(at, UINT(4 + 4 * k + a))), ADD_F(MUL_F(spvExtract(s, s->floatType, was[1 + k], a), kept), MUL_F(MUL_F(MUL_F(FLOAT(2.f), spvExtract(s, s->floatType, lean, a)), each), taken)));
            spvStore(s, NEXT_AT(ADD_U(at, UINT(7 + 4 * k))), FLOAT(0.f));
        }
        spvStore(s, NEXT_AT(ADD_U(at, UINT(3))), FLOAT(0.f));
    }
    spvIfEnd(s, wrote);
    {
        /* (a direction no ray went near: as far as a probe sees) */
        SpvId weight = LOAD_F(reachWeightVar), seen = MORE_F(weight, FLOAT(1e-6f)), over = DIV_F(FLOAT(1.f), MAX_F(weight, FLOAT(1e-6f)));
        SpvId m = PICK_F(seen, MUL_F(LOAD_F(reachVar), over), most), m2 = PICK_F(seen, MUL_F(LOAD_F(reach2Var), over), MUL_F(most, most));
        SpvId at = ADD_U(ADD_U(MUL_U(count, UINT(16)), MUL_U(index, UINT(ARTF_PROBE_MAP))), MUL_U(lid, UINT(2)));
        SpvId was = bufferVec(s, &r, probes, SHR_U(at, 2)), odd = NOT_EQ(AND_U(lid, UINT(1)), UINT(0));
        SpvId wasMean = PICK_F(odd, Z(was), X(was)), wasSquare = PICK_F(odd, W(was), Y(was));
        spvStore(s, NEXT_AT(at), ADD_F(MUL_F(wasMean, kept), MUL_F(m, taken)));
        spvStore(s, NEXT_AT(ADD_U(at, UINT(1))), ADD_F(MUL_F(wasSquare, kept), MUL_F(m2, taken)));
    }
#undef NEXT_AT
    spvEndMain(s);

    const SpvId inputs[2] = {groupVar, localVar};
    spvEntryPoint(s, SPV_MODEL_GLCompute, main, inputs, 2);
    spvExecutionMode(s, main, 17 /* LocalSize */, GROUP, 1, 1, 3);
    return finish(s, size);
}

/* What was gathered, over what is there: premultiplied, in the frame's colors. */
/* A pixel's list, in order: of what one instance left, its nearest; the rest, the farthest first, into one color that
 * goes over the frame. Depth is greater when nearer. */
uint32_t* artfShaderLayerResolve(size_t* size, int samples) {
    enum { MOST = ARTF_LAYER_FRAGMENTS };
    Spv b, *s = &b;
    spvInit(s);
    if (samples > 1) spvCapability(s, 35 /* SampleRateShading */);
    Resources r;
    declare(s, &r, false);
    SpvId counts = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 44);
    SpvId fragments = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 45);
    SpvId fragCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
    spvDecorate(s, fragCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);
    SpvId sampleId = 0, imageType = 0, sampled = 0, solidDepth = 0, ivec2 = 0;
    if (samples > 1) {
        sampleId = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->intType), SPV_STORAGE_Input);
        spvDecorate(s, sampleId, SPV_DECORATE_BuiltIn, 18 /* SampleId */);
        spvDecorate(s, sampleId, SPV_DECORATE_Flat, -1);
        sampled = spvTypeSampledImage2DMS(s, &imageType);
        solidDepth = spvResource(s, sampled, SPV_STORAGE_UniformConstant, 8);
        ivec2 = spvNewId(s);
        spvEmit(&s->types, 23 /* OpTypeVector */, SPV_WORDS(ivec2, s->intType, 2));
    }
    SpvId main = spvBeginMain(s);
    SpvId coord = spvLoad(s, s->vec4, fragCoord);
    SpvId width = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, W(frameVec(s, &r, F_FOG)));
    SpvId pixel = ADD_U(MUL_U(spvOp1(s, SPV_OP_ConvertFToU, s->uintType, Y(coord)), width), spvOp1(s, SPV_OP_ConvertFToU, s->uintType, X(coord)));
    SpvId count = spvLoad(s, s->uintType, bufferUintPtr(s, &r, counts, pixel));
    SpvId thisSample = samples > 1 ? spvOp2(s, 196 /* OpShiftLeftLogical */, s->uintType, UINT(1 << 28), spvOp1(s, SPV_OP_Bitcast, s->uintType, spvLoad(s, s->intType, sampleId))) : 0;
    /* what is solid at this sample: a fragment was tested at its pixel's center, and may be behind it here */
    SpvId solid = 0;
    if (samples > 1) {
        const SpvId xy[2] = {spvOp1(s, SPV_OP_ConvertFToS, s->intType, X(coord)), spvOp1(s, SPV_OP_ConvertFToS, s->intType, Y(coord))};
        solid = X(spvImageFetchSample(s, imageType, spvLoad(s, sampled, solidDepth), spvCompose(s, ivec2, xy, 2), spvLoad(s, s->intType, sampleId)));
    }
    SpvId depth[MOST], color[MOST], who[MOST], there[MOST];
    for (int k = 0; k < MOST; ++k) {
        SpvId at = MUL_U(ADD_U(MUL_U(pixel, UINT(MOST)), UINT((uint32_t)k)), UINT(3));
        there[k] = spvOp2(s, SPV_OP_ULessThan, s->boolType, UINT((uint32_t)k), count);
        depth[k] = spvOp1(s, SPV_OP_Bitcast, s->floatType, spvLoad(s, s->uintType, bufferUintPtr(s, &r, fragments, at)));
        color[k] = spvLoad(s, s->uintType, bufferUintPtr(s, &r, fragments, ADD_U(at, UINT(1))));
        who[k] = spvLoad(s, s->uintType, bufferUintPtr(s, &r, fragments, ADD_U(at, UINT(2))));
        if (samples > 1) {
            /* (only what covers this sample; whose it is, in the bits below) */
            there[k] = BOTH(there[k], NOT_EQ(AND_U(who[k], thisSample), UINT(0)));
            there[k] = BOTH(there[k], spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, depth[k], MUL_F(solid, FLOAT(.999f))));
            who[k] = AND_U(who[k], UINT((1u << 28) - 1));
        }
    }
    /* what lies behind another of the same instance goes */
    SpvId kept[MOST];
    for (int k = 0; k < MOST; ++k) {
        kept[k] = there[k];
        for (int j = 0; j < MOST; ++j) {
            if (j == k) continue;
            SpvId same = BOTH(there[j], spvOp2(s, 170 /* OpIEqual */, s->boolType, who[j], who[k]));
            SpvId nearer = j < k ? spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, depth[j], depth[k]) : MORE_F(depth[j], depth[k]);
            kept[k] = BOTH(kept[k], spvOp1(s, SPV_OP_LogicalNot, s->boolType, BOTH(same, nearer)));
        }
    }
    for (int k = 0; k < MOST; ++k) {
        depth[k] = PICK_F(kept[k], depth[k], FLOAT(-1.f));
        color[k] = spvSelect(s, s->uintType, kept[k], color[k], UINT(0));
    }
    /* in order, with a network of compare and swap */
    static const uint8_t network[19][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {1, 2}, {5, 6}, {0, 4}, {3, 7}, {1, 5}, {2, 6}, {1, 4}, {3, 6}, {2, 4}, {3, 5}, {3, 4}};
    for (int n = 0; n < 19; ++n) {
        const int a = network[n][0], c = network[n][1];
        SpvId swap = MORE_F(depth[a], depth[c]);
        SpvId da = PICK_F(swap, depth[c], depth[a]), dc = PICK_F(swap, depth[a], depth[c]);
        SpvId ca = spvSelect(s, s->uintType, swap, color[c], color[a]), cc = spvSelect(s, s->uintType, swap, color[a], color[c]);
        depth[a] = da; depth[c] = dc;
        color[a] = ca; color[c] = cc;
    }
    const SpvId zero[4] = {FLOAT(0.f), FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)};
    SpvId all = spvCompose(s, s->vec4, zero, 4);
    for (int k = 0; k < MOST; ++k) {
        const SpvId parts[4] = {BYTE_OF(color[k], 16), BYTE_OF(color[k], 8), BYTE_OF(color[k], 0), BYTE_OF(color[k], 24)};
        SpvId top = spvCompose(s, s->vec4, parts, 4);
        all = spvOp2(s, SPV_OP_FAdd, s->vec4, top, spvOp2(s, SPV_OP_VectorTimesScalar, s->vec4, all, SUB_F(FLOAT(1.f), W(top))));
    }
    spvStore(s, outColor, all);
    spvEndMain(s);
    const SpvId interface[3] = {fragCoord, outColor, sampleId};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, interface, samples > 1 ? 3 : 2);
    spvExecutionMode(s, main, 7, 0, 0, 0, 0);
    return finish(s, size);
}

uint32_t* artfShaderTraceShow(size_t* size, bool hdr) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    declare(s, &r, false);
    SpvId sum = spvResource(s, blockOf(s, r.vec4Array, false), SPV_STORAGE_StorageBuffer, 22);
    SpvId frame = spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, T_ROWS, 16), true), SPV_STORAGE_Uniform, 23);
    SpvId fragCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
    spvDecorate(s, fragCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);

    SpvId main = spvBeginMain(s);
    SpvId coord = spvLoad(s, s->vec4, fragCoord);
    SpvId x = TO_U(FLOOR_F(X(coord))), y = TO_U(FLOOR_F(Y(coord)));
    const SpvId rowSize[2] = {r.zero, UINT(T_SIZE)}, rowPath[2] = {r.zero, UINT(T_PATH)};
    SpvId sizes = spvLoad(s, s->vec4, spvAccess(s, r.ptrUniformVec4, frame, rowSize, 2)), pathRow = spvLoad(s, s->vec4, spvAccess(s, r.ptrUniformVec4, frame, rowPath, 2));
    SpvId total = TO_F(AS_UINT(Z(pathRow)));
    SpvId gathered = bufferVec(s, &r, sum, ADD_U(x, MUL_U(y, AS_UINT(X(sizes)))));
    SpvId covered = W(gathered);
    SpvId alpha = DIV_F(covered, total);
    SpvId raw = SCALE_V3(spvShuffle3(s, gathered), DIV_F(FLOAT(1.f), MAX_F(covered, FLOAT(1e-6f))));
    if (hdr) {
        spvStore(s, outColor, vec4Of(s, SCALE_V3(raw, alpha), alpha));
        spvEndMain(s);
        const SpvId io[2] = {fragCoord, outColor};
        spvEntryPoint(s, SPV_MODEL_Fragment, main, io, 2);
        spvExecutionMode(s, main, 7, 0, 0, 0, 0);
        return finish(s, size);
    }
    SpvId light = exposed(s, raw, X(frameVec(s, &r, F_LOOK)));
    SpvId lo = vec3Of(s, FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)), hi = vec3Of(s, FLOAT(1.f), FLOAT(1.f), FLOAT(1.f));
    SpvId clamped = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, light, lo), hi);
    spvStore(s, outColor, vec4Of(s, SCALE_V3(srgbEncode3(s, clamped), alpha), alpha));
    spvEndMain(s);

    const SpvId interface[2] = {fragCoord, outColor};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, interface, 2);
    spvExecutionMode(s, main, 7, 0, 0, 0, 0);
    return finish(s, size);
}

/* ---- what each shadow map needs: the test in geometryJob ----
 *
 * Bindings: 38 how many casters there are and how many commands a view has, then four numbers each (instance, mesh,
 * command); 39 each view's commands; 26 where those list what they draw. An invocation is a caster in a view. */
uint32_t* artfShaderShadowCull(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    memset(&r, 0, sizeof r);
    declare(s, &r, true);
    SpvId casters = spvResource(s, blockOf(s, r.uintArray, false), SPV_STORAGE_StorageBuffer, 38);
    SpvId commands = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 39);
    SpvId list = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 26);
    Shadows shadows = {spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, S_ROWS, 16), true), SPV_STORAGE_Uniform, 24), 0, 0, 0, 0, &r};
    SpvId uvec3 = spvNewId(s);
    spvEmit(&s->types, 23 /* OpTypeVector */, SPV_WORDS(uvec3, s->uintType, 3));
    SpvId global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, uvec3), SPV_STORAGE_Input);
    spvDecorate(s, global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);
    SpvId main = spvBeginMain(s);
    SpvId id = spvLoad(s, uvec3, global);
    SpvId i = spvExtract(s, s->uintType, id, 0), view = spvExtract(s, s->uintType, id, 1);
    SpvId count = spvLoad(s, s->uintType, bufferUintPtr(s, &r, casters, UINT(0)));
    SpvId perView = spvLoad(s, s->uintType, bufferUintPtr(s, &r, casters, UINT(1)));
    SpvId merge;
    spvIfBegin(s, LESS_U(i, count), &merge);
    SpvId at = MUL_U(ADD_U(i, UINT(1)), UINT(4));
    SpvId instance = spvLoad(s, s->uintType, bufferUintPtr(s, &r, casters, at));
    SpvId mesh = spvLoad(s, s->uintType, bufferUintPtr(s, &r, casters, ADD_U(at, UINT(1))));
    SpvId command = spvLoad(s, s->uintType, bufferUintPtr(s, &r, casters, ADD_U(at, UINT(2))));
    SpvId base = MUL_U(instance, UINT(4));
    SpvId r0 = bufferVec(s, &r, r.instances, base), r1 = bufferVec(s, &r, r.instances, ADD_U(base, UINT(1)));
    SpvId r2 = bufferVec(s, &r, r.instances, ADD_U(base, UINT(2))), meta = bufferVec(s, &r, r.instances, ADD_U(base, UINT(3)));
    SpvId sphere = bufferVec(s, &r, r.meshes, MUL_U(mesh, UINT(5)));
    SpvId center = vec4Of(s, spvShuffle3(s, sphere), FLOAT(1.f));
    SpvId world3 = vec3Of(s, DOT4(r0, center), DOT4(r1, center), DOT4(r2, center));
    SpvId world = vec4Of(s, world3, FLOAT(1.f));
    SpvId radius = MUL_F(W(sphere), Z(meta));
    SpvId row = ADD_U(UINT(S_VIEWS), MUL_U(view, UINT(S_VIEW_ROWS)));
    SpvId info = shadowRow(s, &shadows, ADD_U(row, UINT(S_INFO))), place = shadowRow(s, &shadows, ADD_U(row, UINT(S_PLACE)));
    SpvId side = X(shadowRow(s, &shadows, ADD_U(row, UINT(S_SIZE))));
    /* the sun: a box */
    SpvId half = MUL_F(MUL_F(Z(info), side), FLOAT(.5f));
    SpvId limit = ADD_F(FLOAT(1.f), DIV_F(radius, half));
    SpvId x = DOT4(shadowRow(s, &shadows, row), world), y = DOT4(shadowRow(s, &shadows, ADD_U(row, UINT(1))), world);
    SpvId inBox = BOTH(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, ABS_F(x), limit), spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, ABS_F(y), limit));
    /* a lamp: a ball */
    SpvId apart = SUB_V3(world3, spvShuffle3(s, place));
    SpvId reach = ADD_F(W(place), radius);
    SpvId inBall = spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, DOT3(apart, apart), MUL_F(reach, reach));
    SpvId sun = spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, W(place), FLOAT(0.f));
    SpvId inside = spvSelect(s, s->boolType, sun, inBox, inBall);
    SpvId here;
    spvIfBegin(s, inside, &here);
    SpvId entry = MUL_U(ADD_U(MUL_U(view, perView), command), UINT(5));
    SpvId slot = spvAtomicAdd(s, bufferUintPtr(s, &r, commands, ADD_U(entry, UINT(1))), UINT(1));
    SpvId first = spvLoad(s, s->uintType, bufferUintPtr(s, &r, commands, ADD_U(entry, UINT(4))));
    SpvId to = MUL_U(ADD_U(first, slot), UINT(2));
    spvStore(s, bufferUintPtr(s, &r, list, to), instance);
    spvStore(s, bufferUintPtr(s, &r, list, ADD_U(to, UINT(1))), mesh);
    spvIfEnd(s, here);
    spvIfEnd(s, merge);
    spvEndMain(s);
    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &global, 1);
    spvExecutionMode(s, main, 17 /* LocalSize */, 64, 1, 1, 3);
    return finish(s, size);
}

/* ---- bones: artfMeshDeform's arithmetic, for all that a frame moved ----
 *
 * Bindings: 34 what meshes were, five rows a vertex: position and normal, eight bones in four numbers, eight weights;
 * 35 first three rows a job (where it goes, how many, where it reads, where its bones are; the mesh's box; how many
 * bones), then the bones, three rows each; 36 the vertices, as numbers. Push constant: the job. */
uint32_t* artfShaderSkin(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    memset(&r, 0, sizeof r);
    declare(s, &r, true);
    SpvId data = spvResource(s, blockOf(s, r.vec4Array, false), SPV_STORAGE_StorageBuffer, 34);
    SpvId table = spvResource(s, blockOf(s, r.vec4Array, false), SPV_STORAGE_StorageBuffer, 35);
    SpvId vertices = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 36);
    SpvId pushBlock = spvTypeStruct(s, &s->uintType, 1);
    spvDecorate(s, pushBlock, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, pushBlock, 0, SPV_DECORATE_Offset, 0);
    SpvId push = spvVariable(s, spvTypePointer(s, SPV_STORAGE_PushConstant, pushBlock), SPV_STORAGE_PushConstant);
    SpvId uvec3 = spvNewId(s);
    spvEmit(&s->types, 23 /* OpTypeVector */, SPV_WORDS(uvec3, s->uintType, 3));
    SpvId global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, uvec3), SPV_STORAGE_Input);
    spvDecorate(s, global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);
    SpvId main = spvBeginMain(s);
    SpvId x = spvExtract(s, s->uintType, spvLoad(s, uvec3, global), 0);
    SpvId job = spvLoad(s, s->uintType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_PushConstant, s->uintType), push, &r.zero, 1));
    SpvId row = MUL_U(job, UINT(3));
    SpvId where = bufferVec(s, &r, table, row), scale = bufferVec(s, &r, table, ADD_U(row, UINT(1))), offset = bufferVec(s, &r, table, ADD_U(row, UINT(2)));
    SpvId first = AS_UINT(X(where)), count = AS_UINT(Y(where)), rest = AS_UINT(Z(where)), bones = AS_UINT(W(where));
    SpvId boneCount = AS_UINT(W(scale));
    SpvId merge;
    spvIfBegin(s, LESS_U(x, count), &merge);
    SpvId at = ADD_U(rest, MUL_U(x, UINT(5)));
    SpvId v0 = bufferVec(s, &r, data, at), v1 = bufferVec(s, &r, data, ADD_U(at, UINT(1))), v2 = bufferVec(s, &r, data, ADD_U(at, UINT(2)));
    SpvId v3 = bufferVec(s, &r, data, ADD_U(at, UINT(3))), v4 = bufferVec(s, &r, data, ADD_U(at, UINT(4)));
    const SpvId words[4] = {AS_UINT(Z(v1)), AS_UINT(W(v1)), AS_UINT(X(v2)), AS_UINT(Y(v2))};
    const SpvId weights[8] = {X(v3), Y(v3), Z(v3), W(v3), X(v4), Y(v4), Z(v4), W(v4)};
    const SpvId zero4[4] = {FLOAT(0.f), FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)};
    SpvId t[3];
    for (int i = 0; i < 3; ++i) t[i] = spvCompose(s, s->vec4, zero4, 4);
    for (int k = 0; k < 8; ++k) {
        SpvId bone = AND_U(SHR_U(words[k / 2], (k & 1) * 16), UINT(65535));
        SpvId weight = PICK_F(LESS_U(bone, boneCount), weights[k], FLOAT(0.f));
        SpvId safe = spvSelect(s, s->uintType, LESS_U(bone, boneCount), bone, UINT(0));
        SpvId boneRow = ADD_U(bones, MUL_U(safe, UINT(3)));
        for (int i = 0; i < 3; ++i) {
            SpvId J = bufferVec(s, &r, table, ADD_U(boneRow, UINT((uint32_t)i)));
            t[i] = spvOp2(s, SPV_OP_FAdd, s->vec4, t[i], spvOp2(s, SPV_OP_VectorTimesScalar, s->vec4, J, weight));
        }
    }
    const SpvId pos[3] = {X(v0), Y(v0), Z(v0)}, normal[3] = {W(v0), X(v1), Y(v1)};
    SpvId q[3], n[3];
    for (int i = 0; i < 3; ++i) {
        SpvId p = ADD_F(ADD_F(ADD_F(MUL_F(X(t[i]), pos[0]), MUL_F(Y(t[i]), pos[1])), MUL_F(Z(t[i]), pos[2])), W(t[i]));
        SpvId sc = i == 0 ? X(scale) : i == 1 ? Y(scale) : Z(scale), of = i == 0 ? X(offset) : i == 1 ? Y(offset) : Z(offset);
        SpvId unit = MIN_F(MAX_F(DIV_F(SUB_F(p, of), sc), FLOAT(0.f)), FLOAT(1.f));
        q[i] = TO_U(FLOOR_F(ADD_F(MUL_F(unit, FLOAT(65535.f)), FLOAT(.5f))));
        n[i] = ADD_F(ADD_F(MUL_F(X(t[i]), normal[0]), MUL_F(Y(t[i]), normal[1])), MUL_F(Z(t[i]), normal[2]));
    }
    /* as octEncode */
    SpvId length = ADD_F(ADD_F(ABS_F(n[0]), ABS_F(n[1])), ABS_F(n[2]));
    SpvId some = MORE_F(length, FLOAT(0.f));
    SpvId inverse = DIV_F(FLOAT(1.f), PICK_F(some, length, FLOAT(1.f)));
    SpvId ox = MUL_F(n[0], inverse), oy = MUL_F(n[1], inverse);
    SpvId sx = PICK_F(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, ox, FLOAT(0.f)), FLOAT(1.f), FLOAT(-1.f));
    SpvId sy = PICK_F(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, oy, FLOAT(0.f)), FLOAT(1.f), FLOAT(-1.f));
    SpvId under = LESS_F(n[2], FLOAT(0.f));
    SpvId fx = PICK_F(under, MUL_F(SUB_F(FLOAT(1.f), ABS_F(oy)), sx), ox), fy = PICK_F(under, MUL_F(SUB_F(FLOAT(1.f), ABS_F(ox)), sy), oy);
    SpvId cx = FLOOR_F(ADD_F(MUL_F(MIN_F(MAX_F(fx, FLOAT(-1.f)), FLOAT(1.f)), FLOAT(32767.f)), FLOAT(.5f)));
    SpvId cy = FLOOR_F(ADD_F(MUL_F(MIN_F(MAX_F(fy, FLOAT(-1.f)), FLOAT(1.f)), FLOAT(32767.f)), FLOAT(.5f)));
    SpvId nx = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvOp1(s, 110 /* OpConvertFToS */, s->intType, PICK_F(some, cx, FLOAT(0.f))));
    SpvId ny = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvOp1(s, 110, s->intType, PICK_F(some, cy, FLOAT(0.f))));
    SpvId to = MUL_U(ADD_U(first, x), UINT(4));
    spvStore(s, bufferUintPtr(s, &r, vertices, to), spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, q[0], spvOp2(s, 196 /* OpShiftLeftLogical */, s->uintType, q[1], UINT(16))));
    spvStore(s, bufferUintPtr(s, &r, vertices, ADD_U(to, UINT(1))), q[2]);
    spvStore(s, bufferUintPtr(s, &r, vertices, ADD_U(to, UINT(2))), spvOp2(s, 197, s->uintType, AND_U(nx, UINT(65535)), spvOp2(s, 196, s->uintType, ny, UINT(16))));
    spvIfEnd(s, merge);
    spvEndMain(s);
    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &global, 1);
    spvExecutionMode(s, main, 17 /* LocalSize */, 64, 1, 1, 3);
    return finish(s, size);
}

/* ---- taking out noise: the same arithmetic as denoise.c ----
 *
 * Bindings: 22 the frame's light, and in the end the clean frame; 23 ArtfTraceFrame; 27 where the first surface is
 * and its normal; 28 its color; 29 the frame before's 27; 30 what was gathered; 31 what is gathered now; 32 and 33
 * what the filter passes hand on. Push constant: the pass's step. */

enum { D_NOISY = 22, D_GUIDE = 27, D_ALBEDO = 28, D_PREVIOUS = 29, D_HISTORY = 30, D_NEXT = 31, D_PING = 32, D_PONG = 33, D_MOTION = 37, D_SHIFT = 48 };

typedef struct Denoise {
    Resources r;
    SpvId noisy, guide, albedo, previous, history, next, ping, pong, frame, motion, shift;
    SpvId x, y, width, height, pixel, eye;
    SpvId push;
} Denoise;

static SpvId denoiseStart(Spv* s, Denoise* d, SpvId* global, SpvId* merge) {
    declare(s, &d->r, true);
    d->noisy = spvResource(s, blockOf(s, d->r.vec4Array, true), SPV_STORAGE_StorageBuffer, D_NOISY);
    d->guide = spvResource(s, blockOf(s, d->r.vec4Array, true), SPV_STORAGE_StorageBuffer, D_GUIDE);
    d->albedo = spvResource(s, blockOf(s, d->r.vec4Array, true), SPV_STORAGE_StorageBuffer, D_ALBEDO);
    d->previous = spvResource(s, blockOf(s, d->r.vec4Array, true), SPV_STORAGE_StorageBuffer, D_PREVIOUS);
    d->history = spvResource(s, blockOf(s, d->r.vec4Array, true), SPV_STORAGE_StorageBuffer, D_HISTORY);
    d->next = spvResource(s, blockOf(s, d->r.vec4Array, true), SPV_STORAGE_StorageBuffer, D_NEXT);
    d->ping = spvResource(s, blockOf(s, d->r.vec4Array, true), SPV_STORAGE_StorageBuffer, D_PING);
    d->pong = spvResource(s, blockOf(s, d->r.vec4Array, true), SPV_STORAGE_StorageBuffer, D_PONG);
    d->motion = spvResource(s, blockOf(s, d->r.vec4Array, false), SPV_STORAGE_StorageBuffer, D_MOTION);
    d->shift = spvResource(s, blockOf(s, d->r.vec4Array, false), SPV_STORAGE_StorageBuffer, D_SHIFT);
    d->frame = spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, T_ROWS, 16), true), SPV_STORAGE_Uniform, 23);
    SpvId pushBlock = spvTypeStruct(s, &s->uintType, 1);
    spvDecorate(s, pushBlock, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, pushBlock, 0, SPV_DECORATE_Offset, 0);
    d->push = spvVariable(s, spvTypePointer(s, SPV_STORAGE_PushConstant, pushBlock), SPV_STORAGE_PushConstant);
    SpvId uvec3 = spvNewId(s);
    spvEmit(&s->types, 23 /* OpTypeVector */, SPV_WORDS(uvec3, s->uintType, 3));
    *global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, uvec3), SPV_STORAGE_Input);
    spvDecorate(s, *global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);
    SpvId main = spvBeginMain(s);
    SpvId id = spvLoad(s, uvec3, *global);
    d->x = spvExtract(s, s->uintType, id, 0);
    d->y = spvExtract(s, s->uintType, id, 1);
    const SpvId rowSize[2] = {d->r.zero, UINT(T_SIZE)}, rowEye[2] = {d->r.zero, UINT(T_EYE)};
    SpvId sizes = spvLoad(s, s->vec4, spvAccess(s, d->r.ptrUniformVec4, d->frame, rowSize, 2));
    d->eye = spvShuffle3(s, spvLoad(s, s->vec4, spvAccess(s, d->r.ptrUniformVec4, d->frame, rowEye, 2)));
    d->width = AS_UINT(X(sizes));
    d->height = AS_UINT(Y(sizes));
    d->pixel = ADD_U(d->x, MUL_U(d->y, d->width));
    spvIfBegin(s, BOTH(LESS_U(d->x, d->width), LESS_U(d->y, d->height)), merge);
    return main;
}

static uint32_t* denoiseEnd(Spv* s, SpvId main, SpvId global, SpvId merge, size_t* size) {
    spvIfEnd(s, merge);
    spvEndMain(s);
    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &global, 1);
    spvExecutionMode(s, main, 17 /* LocalSize */, 8, 8, 1, 3);
    return finish(s, size);
}

/* As normalUnpack: the normal, from the fourth number of a guide. */
static SpvId packedNormal(Spv* s, SpvId bits) {
    SpvId c[3];
    for (int k = 0; k < 3; ++k) c[k] = SUB_F(MUL_F(TO_F(AND_U(SHR_U(bits, k * 10), UINT(1023))), FLOAT(2.f / 1023.f)), FLOAT(1.f));
    return vec3Of(s, c[0], c[1], c[2]);
}

static SpvId vecAt(Spv* s, const Denoise* d, SpvId buffer, SpvId i) { return bufferVec(s, &d->r, buffer, i); }
static void vecPut(Spv* s, const Denoise* d, SpvId buffer, SpvId i, SpvId value) {
    const SpvId idx[2] = {d->r.zero, i};
    spvStore(s, spvAccess(s, d->r.ptrStorageVec4, buffer, idx, 2), value);
}

/* This frame's light over color, in ping: as lightRow. */
uint32_t* artfShaderDenoiseLight(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    Denoise d;
    memset(&d, 0, sizeof d);
    SpvId global, merge;
    SpvId main = denoiseStart(s, &d, &global, &merge);
    SpvId noisy = vecAt(s, &d, d.noisy, d.pixel), albedo = spvShuffle3(s, vecAt(s, &d, d.albedo, d.pixel));
    SpvId bits = AS_UINT(W(vecAt(s, &d, d.guide, d.pixel)));
    SpvId safe = spvGlsl2(s, SPV_GLSL_FMax, s->vec3, albedo, same3(s, .02f));
    SpvId light = spvOp2(s, SPV_OP_FDiv, s->vec3, SCALE_V3(spvShuffle3(s, noisy), DIV_F(FLOAT(1.f), MAX_F(W(noisy), FLOAT(1.f)))), safe);
    vecPut(s, &d, d.ping, d.pixel, vec4Of(s, light, PICK_F(NOT_EQ(bits, UINT(0)), FLOAT(1.f), FLOAT(0.f))));
    return denoiseEnd(s, main, global, merge, size);
}

uint32_t* artfShaderDenoiseTime(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    Denoise d;
    memset(&d, 0, sizeof d);
    SpvId global, merge;
    SpvId main = denoiseStart(s, &d, &global, &merge);
    /* variables go first in a function, and this has few: the first block is a selection, so they sit in it as well */
    const SpvId rows[6] = {UINT(T_PATH), UINT(T_PREVIOUS), UINT(T_PREVIOUS + 1), UINT(T_PREVIOUS + 2), UINT(T_PREVIOUS + 3), UINT(T_SIZE)};
    SpvId r[6];
    for (int i = 0; i < 6; ++i) {
        const SpvId idx[2] = {d.r.zero, rows[i]};
        r[i] = spvLoad(s, s->vec4, spvAccess(s, d.r.ptrUniformVec4, d.frame, idx, 2));
    }
    SpvId samples = TO_F(AS_UINT(W(r[5])));
    SpvId first = NOT_EQ(AS_UINT(W(r[0])), UINT(0));
    SpvId noisy = vecAt(s, &d, d.noisy, d.pixel), guide = vecAt(s, &d, d.guide, d.pixel);
    SpvId coverage = DIV_F(W(noisy), samples);
    SpvId bits = AS_UINT(W(guide)), hasSurface = NOT_EQ(bits, UINT(0));
    SpvId n = packedNormal(s, bits), world = spvShuffle3(s, guide);
    SpvId light = spvShuffle3(s, vecAt(s, &d, d.ping, d.pixel));
    /* where its object was, and the way it faced: as temporalRow */
    SpvId instance = TO_U(W(vecAt(s, &d, d.albedo, d.pixel)));
    SpvId isOne = NOT_EQ(instance, UINT(0));
    SpvId motionRow = MUL_U(spvSelect(s, s->uintType, isOne, SUB_U(instance, UINT(1)), UINT(0)), UINT(3));
    {
        const SpvId rowsOf[3] = {vecAt(s, &d, d.motion, motionRow), vecAt(s, &d, d.motion, ADD_U(motionRow, UINT(1))), vecAt(s, &d, d.motion, ADD_U(motionRow, UINT(2)))};
        SpvId wp[3], np[3];
        SpvId bent = ADD_V3(world, spvShuffle3(s, vecAt(s, &d, d.shift, d.pixel)));
        for (int i = 0; i < 3; ++i) {
            SpvId wx0 = spvExtract(s, s->floatType, bent, 0), wy0 = spvExtract(s, s->floatType, bent, 1), wz0 = spvExtract(s, s->floatType, bent, 2);
            SpvId nx0 = spvExtract(s, s->floatType, n, 0), ny0 = spvExtract(s, s->floatType, n, 1), nz0 = spvExtract(s, s->floatType, n, 2);
            wp[i] = ADD_F(ADD_F(ADD_F(MUL_F(X(rowsOf[i]), wx0), MUL_F(Y(rowsOf[i]), wy0)), MUL_F(Z(rowsOf[i]), wz0)), W(rowsOf[i]));
            np[i] = ADD_F(ADD_F(MUL_F(X(rowsOf[i]), nx0), MUL_F(Y(rowsOf[i]), ny0)), MUL_F(Z(rowsOf[i]), nz0));
        }
        world = pick3(s, isOne, vec3Of(s, wp[0], wp[1], wp[2]), world);
        n = pick3(s, isOne, vec3Of(s, np[0], np[1], np[2]), n);
    }
    /* where it was: the matrix's columns are rows here, as the frame before gave them */
    SpvId w4 = vec4Of(s, world, FLOAT(1.f));
    SpvId cx = ADD_F(ADD_F(ADD_F(MUL_F(X(r[1]), X(w4)), MUL_F(X(r[2]), Y(w4))), MUL_F(X(r[3]), Z(w4))), X(r[4]));
    SpvId cy = ADD_F(ADD_F(ADD_F(MUL_F(Y(r[1]), X(w4)), MUL_F(Y(r[2]), Y(w4))), MUL_F(Y(r[3]), Z(w4))), Y(r[4]));
    SpvId cw = ADD_F(ADD_F(ADD_F(MUL_F(W(r[1]), X(w4)), MUL_F(W(r[2]), Y(w4))), MUL_F(W(r[3]), Z(w4))), W(r[4]));
    SpvId fw = TO_F(d.width), fh = TO_F(d.height);
    SpvId px = MUL_F(ADD_F(MUL_F(DIV_F(cx, cw), FLOAT(.5f)), FLOAT(.5f)), fw), py = MUL_F(SUB_F(FLOAT(.5f), MUL_F(DIV_F(cy, cw), FLOAT(.5f))), fh);
    SpvId inFrame = BOTH(BOTH(MORE_F(cw, FLOAT(0.f)), NOT(first)), BOTH(BOTH(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, px, FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, py, FLOAT(0.f))), BOTH(LESS_F(px, fw), LESS_F(py, fh))));
    SpvId cpx = MIN_F(MAX_F(px, FLOAT(0.f)), SUB_F(fw, FLOAT(1.f))), cpy = MIN_F(MAX_F(py, FLOAT(0.f)), SUB_F(fh, FLOAT(1.f)));
    SpvId j = ADD_U(TO_U(cpx), MUL_U(TO_U(cpy), d.width));
    SpvId then = vecAt(s, &d, d.previous, j);
    SpvId pn = packedNormal(s, AS_UINT(W(then)));
    SpvId apart = SUB_V3(spvShuffle3(s, then), world);
    SpvId tolerance = ADD_F(MUL_F(FLOAT(.01f), spvGlsl1(s, SPV_GLSL_Length, s->floatType, SUB_V3(world, d.eye))), FLOAT(1e-3f));
    SpvId off = DOT3(apart, n);
    SpvId same = BOTH(BOTH(NOT_EQ(AS_UINT(W(then)), UINT(0)), LESS_F(MUL_F(off, off), MUL_F(tolerance, tolerance))), MORE_F(DOT3(n, pn), FLOAT(.9f)));
    SpvId taken = BOTH(inFrame, same);
    SpvId h = vecAt(s, &d, d.history, j);
    SpvId whole = FLOOR_F(W(h));
    /* what this frame's light says it can be: as lightRange */
    SpvId sum = same3(s, 0.f), squares = same3(s, 0.f), around = FLOAT(0.f);
    SpvId nowSum = same3(s, 0.f), thenSum = same3(s, 0.f), pairs = FLOAT(0.f);
    SpvId jx = spvOp1(s, SPV_OP_Bitcast, s->intType, TO_U(cpx)), jy = spvOp1(s, SPV_OP_Bitcast, s->intType, TO_U(cpy));
    SpvId ix = spvOp1(s, SPV_OP_Bitcast, s->intType, d.x), iy = spvOp1(s, SPV_OP_Bitcast, s->intType, d.y);
    SpvId iw = spvOp1(s, SPV_OP_Bitcast, s->intType, d.width), ih = spvOp1(s, SPV_OP_Bitcast, s->intType, d.height);
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            SpvId qx = spvOp2(s, SPV_OP_IAdd, s->intType, ix, spvConstInt(s, dx)), qy = spvOp2(s, SPV_OP_IAdd, s->intType, iy, spvConstInt(s, dy));
            SpvId zero = spvConstInt(s, 0);
            SpvId inside = BOTH(BOTH(spvOp2(s, 175 /* OpSGreaterThanEqual */, s->boolType, qx, zero), spvOp2(s, 177 /* OpSLessThan */, s->boolType, qx, iw)), BOTH(spvOp2(s, 175, s->boolType, qy, zero), spvOp2(s, 177, s->boolType, qy, ih)));
            SpvId kx = spvGlsl2(s, 42 /* SMax */, s->intType, spvGlsl2(s, 39 /* SMin */, s->intType, qx, spvOp2(s, SPV_OP_ISub, s->intType, iw, spvConstInt(s, 1))), zero);
            SpvId ky = spvGlsl2(s, 42, s->intType, spvGlsl2(s, 39, s->intType, qy, spvOp2(s, SPV_OP_ISub, s->intType, ih, spvConstInt(s, 1))), zero);
            SpvId q = vecAt(s, &d, d.ping, ADD_U(spvOp1(s, SPV_OP_Bitcast, s->uintType, kx), MUL_U(spvOp1(s, SPV_OP_Bitcast, s->uintType, ky), d.width)));
            SpvId weight = PICK_F(inside, W(q), FLOAT(0.f));
            SpvId value = spvShuffle3(s, q);
            sum = ADD_V3(sum, SCALE_V3(value, weight));
            squares = ADD_V3(squares, SCALE_V3(MUL_V3(value, value), weight));
            around = ADD_F(around, weight);
            SpvId hx = spvGlsl2(s, 42, s->intType, spvGlsl2(s, 39, s->intType, spvOp2(s, SPV_OP_IAdd, s->intType, jx, spvConstInt(s, dx)), spvOp2(s, SPV_OP_ISub, s->intType, iw, spvConstInt(s, 1))), zero);
            SpvId hy = spvGlsl2(s, 42, s->intType, spvGlsl2(s, 39, s->intType, spvOp2(s, SPV_OP_IAdd, s->intType, jy, spvConstInt(s, dy)), spvOp2(s, SPV_OP_ISub, s->intType, ih, spvConstInt(s, 1))), zero);
            SpvId at = ADD_U(spvOp1(s, SPV_OP_Bitcast, s->uintType, hx), MUL_U(spvOp1(s, SPV_OP_Bitcast, s->uintType, hy), d.width));
            SpvId pair = PICK_F(NOT_EQ(AS_UINT(W(vecAt(s, &d, d.previous, at))), UINT(0)), weight, FLOAT(0.f));
            nowSum = ADD_V3(nowSum, SCALE_V3(value, pair));
            thenSum = ADD_V3(thenSum, SCALE_V3(spvShuffle3(s, vecAt(s, &d, d.history, at)), pair));
            pairs = ADD_F(pairs, pair);
        }
    SpvId inverse = DIV_F(FLOAT(1.f), MAX_F(around, FLOAT(1.f)));
    SpvId mean = SCALE_V3(sum, inverse);
    SpvId deviation = spvGlsl1(s, SPV_GLSL_Sqrt, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, SUB_V3(SCALE_V3(squares, inverse), MUL_V3(mean, mean)), same3(s, 0.f)));
    SpvId range = ADD_V3(ADD_V3(SCALE_V3(deviation, FLOAT(1.25f)), same3(s, .01f)), SCALE_V3(mean, FLOAT(.05f)));
    SpvId was = spvShuffle3(s, h);
    SpvId kept = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, was, SUB_V3(mean, range)), ADD_V3(mean, range));
    SpvId change = SUB_V3(kept, was);
    /* the neighbourhood taken together: as lightRange */
    SpvId pairInverse = DIV_F(FLOAT(1.f), MAX_F(pairs, FLOAT(1.f)));
    SpvId allowed = ADD_V3(ADD_V3(SCALE_V3(deviation, MUL_F(FLOAT(4.f), SQRT_F(pairInverse))), same3(s, .002f)), SCALE_V3(mean, FLOAT(.01f)));
    SpvId drift = SCALE_V3(SUB_V3(nowSum, thenSum), pairInverse);
    SpvId over = SUB_V3(MUL_V3(drift, drift), MUL_V3(allowed, allowed));
    SpvId drifted = MORE_F(MAX_F(MAX_F(X(over), Y(over)), Z(over)), FLOAT(0.f));
    SpvId found = EITHER(MORE_F(DOT3(change, change), FLOAT(0.f)), drifted);
    SpvId count = PICK_F(taken, MIN_F(ADD_F(whole, FLOAT(1.f)), PICK_F(found, FLOAT(8.f), FLOAT(256.f))), FLOAT(1.f));
    SpvId alphaBefore = PICK_F(taken, SUB_F(W(h), whole), coverage);
    SpvId before = pick3(s, taken, kept, same3(s, 0.f));
    SpvId share = DIV_F(FLOAT(1.f), count);
    SpvId gathered = ADD_V3(before, SCALE_V3(SUB_V3(light, before), share));
    SpvId alpha = MIN_F(ADD_F(alphaBefore, MUL_F(SUB_F(coverage, alphaBefore), share)), FLOAT(.999f));
    SpvId nothing = ADD_F(FLOAT(1.f), MIN_F(coverage, FLOAT(.999f)));
    vecPut(s, &d, d.next, d.pixel, vec4Of(s, pick3(s, hasSurface, gathered, same3(s, 0.f)), PICK_F(hasSurface, ADD_F(count, alpha), nothing)));
    return denoiseEnd(s, main, global, merge, size);
}

/* One pass of the filter: from ping to pong, or the other way round, by what `flip` says; from what was
 * gathered, the first time. */
uint32_t* artfShaderDenoiseSpace(size_t* size, int pass) {
    Spv b, *s = &b;
    spvInit(s);
    Denoise d;
    memset(&d, 0, sizeof d);
    SpvId global, merge;
    SpvId main = denoiseStart(s, &d, &global, &merge);
    SpvId from = pass == 0 ? d.next : pass == 1 ? d.ping : d.pong, to = pass == 1 ? d.pong : d.ping;
    const int step = 1 << pass;
    static const float kernel[3] = {.375f, .25f, .0625f};
    SpvId guide = vecAt(s, &d, d.guide, d.pixel), center = vecAt(s, &d, from, d.pixel);
    SpvId bits = AS_UINT(W(guide));
    SpvId n = packedNormal(s, bits);
    SpvId distance = spvGlsl1(s, SPV_GLSL_Length, s->floatType, SUB_V3(spvShuffle3(s, guide), d.eye));
    SpvId reach = ADD_F(MUL_F(distance, FLOAT(.02f)), FLOAT(1e-3f));
    SpvId leave = EITHER(spvOp2(s, 170 /* OpIEqual */, s->boolType, bits, UINT(0)), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, FLOOR_F(W(vecAt(s, &d, d.next, d.pixel))), FLOAT(48.f)));
    SpvId sum = same3(s, 0.f), total = FLOAT(0.f);
    SpvId ix = spvOp1(s, SPV_OP_Bitcast, s->intType, d.x), iy = spvOp1(s, SPV_OP_Bitcast, s->intType, d.y);
    SpvId iw = spvOp1(s, SPV_OP_Bitcast, s->intType, d.width), ih = spvOp1(s, SPV_OP_Bitcast, s->intType, d.height);
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            SpvId qx = spvOp2(s, SPV_OP_IAdd, s->intType, ix, spvConstInt(s, dx * step)), qy = spvOp2(s, SPV_OP_IAdd, s->intType, iy, spvConstInt(s, dy * step));
            SpvId zero = spvConstInt(s, 0);
            SpvId inside = BOTH(BOTH(spvOp2(s, 175 /* OpSGreaterThanEqual */, s->boolType, qx, zero), spvOp2(s, 177 /* OpSLessThan */, s->boolType, qx, iw)), BOTH(spvOp2(s, 175, s->boolType, qy, zero), spvOp2(s, 177, s->boolType, qy, ih)));
            SpvId cx = spvGlsl2(s, 42 /* SMax */, s->intType, spvGlsl2(s, 39 /* SMin */, s->intType, qx, spvOp2(s, SPV_OP_ISub, s->intType, iw, spvConstInt(s, 1))), zero);
            SpvId cy = spvGlsl2(s, 42, s->intType, spvGlsl2(s, 39, s->intType, qy, spvOp2(s, SPV_OP_ISub, s->intType, ih, spvConstInt(s, 1))), zero);
            SpvId j = ADD_U(spvOp1(s, SPV_OP_Bitcast, s->uintType, cx), MUL_U(spvOp1(s, SPV_OP_Bitcast, s->uintType, cy), d.width));
            SpvId other = vecAt(s, &d, d.guide, j);
            SpvId otherBits = AS_UINT(W(other));
            SpvId qn = packedNormal(s, otherBits);
            SpvId apart = DIV_F(SUB_F(spvGlsl1(s, SPV_GLSL_Length, s->floatType, SUB_V3(spvShuffle3(s, other), d.eye)), distance), reach);
            SpvId facing = MAX_F(DOT3(n, qn), FLOAT(0.f)), f2 = MUL_F(facing, facing);
            SpvId weight = DIV_F(MUL_F(FLOAT(kernel[abs(dx)] * kernel[abs(dy)]), MUL_F(f2, f2)), ADD_F(FLOAT(1.f), MUL_F(apart, apart)));
            weight = PICK_F(BOTH(inside, NOT_EQ(otherBits, UINT(0))), weight, FLOAT(0.f));
            sum = ADD_V3(sum, SCALE_V3(spvShuffle3(s, vecAt(s, &d, from, j)), weight));
            total = ADD_F(total, weight);
        }
    SpvId filtered = SCALE_V3(sum, PICK_F(MORE_F(total, FLOAT(0.f)), DIV_F(FLOAT(1.f), total), FLOAT(0.f)));
    vecPut(s, &d, to, d.pixel, vec4Of(s, pick3(s, leave, spvShuffle3(s, center), filtered), W(center)));
    return denoiseEnd(s, main, global, merge, size);
}

/* Light again, with its color: into the frame's light, as tracing leaves it. */
uint32_t* artfShaderDenoiseEnd(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    Denoise d;
    memset(&d, 0, sizeof d);
    SpvId global, merge;
    SpvId main = denoiseStart(s, &d, &global, &merge);
    /* three passes end in ping: next to ping, ping to pong, pong to ping */
    SpvId from = vecAt(s, &d, d.ping, d.pixel), albedo = spvShuffle3(s, vecAt(s, &d, d.albedo, d.pixel));
    SpvId alpha = SUB_F(W(from), FLOOR_F(W(from)));
    SpvId lit = MUL_V3(spvShuffle3(s, from), spvGlsl2(s, SPV_GLSL_FMax, s->vec3, albedo, same3(s, .02f)));
    vecPut(s, &d, d.noisy, d.pixel, vec4Of(s, SCALE_V3(lit, alpha), alpha));
    return denoiseEnd(s, main, global, merge, size);
}

/* ---- emitters of many: as artfSwarmMove and artfSwarmFace (particles.c) ----
 *
 * Bindings: 1 and 2, written: an instance of the quad for each particle, and its copy; 65 the particles, three vec4s
 * each; 66 this frame's rows: of each emitter, where its own start, and there: how many steps, places, where its
 * particles start, whether they start over; where its instances and copies go, and the quad; four rows of facing;
 * then each step's ARTF_SWARM_ROWS rows. Push constant: which emitter. An invocation is a place, and does all of it. */

/* As hashOf. */
static SpvId swarmHash(Spv* s, SpvId x) {
    x = XOR_U(x, SHR_U(x, 16));
    x = MUL_U(x, UINT(0x7FEB352Du));
    x = XOR_U(x, SHR_U(x, 15));
    x = MUL_U(x, UINT(0x846CA68Bu));
    return XOR_U(x, SHR_U(x, 16));
}

/* As randomAt. */
static SpvId swarmRandom(Spv* s, SpvId seed, SpvId birth, uint32_t k) {
    SpvId h = swarmHash(s, XOR_U(seed, swarmHash(s, ADD_U(MUL_U(birth, UINT(16)), UINT(k)))));
    return MUL_F(TO_F(SHR_U(h, 8)), FLOAT(1.f / 16777216.f));
}

/* As latticeOf: x, y and z are ints. */
static SpvId swarmLattice(Spv* s, SpvId x, SpvId y, SpvId z, SpvId seed) {
    SpvId h = XOR_U(XOR_U(MUL_U(AS_UINT(x), UINT(0x8DA6B343u)), MUL_U(AS_UINT(y), UINT(0xD8163841u))), XOR_U(MUL_U(AS_UINT(z), UINT(0xCB1AB31Fu)), seed));
    h = XOR_U(h, SHR_U(h, 16));
    h = MUL_U(h, UINT(0x7FEB352Du));
    h = XOR_U(h, SHR_U(h, 15));
    return SUB_F(MUL_F(TO_F(SHR_U(h, 8)), FLOAT(2.f / 16777216.f)), FLOAT(1.f));
}

#define SW_INT(i) spvConstInt(s, i)
#define SW_ADD_I(a, b) spvOp2(s, SPV_OP_IAdd, s->intType, a, b)
#define SW_TO_I(f) spvOp1(s, SPV_OP_ConvertFToS, s->intType, f)
#define SW_TO_F(i) spvOp1(s, 111 /* OpConvertSToF */, s->floatType, i)
#define SW_NONZERO(f) spvOp2(s, 182 /* OpFOrdNotEqual */, s->boolType, f, FLOAT(0.f))
#define SW_SMOOTH(t) MUL_F(MUL_F(t, t), SUB_F(FLOAT(3.f), MUL_F(FLOAT(2.f), t)))

/* As noiseOf. */
static SpvId swarmNoise(Spv* s, SpvId x, SpvId y, SpvId z, SpvId seed) {
    const SpvId big = FLOAT(65536.f);
    SpvId fx = SUB_F(SW_TO_F(SW_TO_I(ADD_F(x, big))), big), fy = SUB_F(SW_TO_F(SW_TO_I(ADD_F(y, big))), big), fz = SUB_F(SW_TO_F(SW_TO_I(ADD_F(z, big))), big);
    SpvId ix = SW_TO_I(fx), iy = SW_TO_I(fy), iz = SW_TO_I(fz);
    SpvId tx = SW_SMOOTH(SUB_F(x, fx)), ty = SW_SMOOTH(SUB_F(y, fy)), tz = SW_SMOOTH(SUB_F(z, fz));
    SpvId c[2][2];
    for (int j = 0; j < 2; ++j)
        for (int k = 0; k < 2; ++k) {
            SpvId yy = SW_ADD_I(iy, SW_INT(j)), zz = SW_ADD_I(iz, SW_INT(k));
            SpvId a = swarmLattice(s, ix, yy, zz, seed), b = swarmLattice(s, SW_ADD_I(ix, SW_INT(1)), yy, zz, seed);
            c[j][k] = ADD_F(a, MUL_F(SUB_F(b, a), tx));
        }
    SpvId nearer = ADD_F(c[0][0], MUL_F(SUB_F(c[1][0], c[0][0]), ty)), farther = ADD_F(c[0][1], MUL_F(SUB_F(c[1][1], c[0][1]), ty));
    return ADD_F(nearer, MUL_F(SUB_F(farther, nearer), tz));
}

/* As artfSinFast. */
static SpvId swarmSine(Spv* s, SpvId x) {
    SpvId half = PICK_F(LESS_F(x, FLOAT(0.f)), FLOAT(-.5f), FLOAT(.5f));
    x = SUB_F(x, MUL_F(FLOAT(6.28318531f), SW_TO_F(SW_TO_I(ADD_F(MUL_F(x, FLOAT(.159154943f)), half)))));
    x = PICK_F(MORE_F(x, FLOAT(1.57079633f)), SUB_F(FLOAT(3.14159265f), x), PICK_F(LESS_F(x, FLOAT(-1.57079633f)), SUB_F(FLOAT(-3.14159265f), x), x));
    SpvId x2 = MUL_F(x, x);
    SpvId p = SUB_F(FLOAT(.00019841270f), MUL_F(x2, FLOAT(.0000027557319f)));
    p = SUB_F(FLOAT(.0083333333f), MUL_F(x2, p));
    p = SUB_F(FLOAT(.16666667f), MUL_F(x2, p));
    return MUL_F(x, SUB_F(FLOAT(1.f), MUL_F(x2, p)));
}

uint32_t* artfShaderSwarm(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    memset(&r, 0, sizeof r);
    r.zero = spvConstInt(s, 0);
    r.ptrStorageVec4 = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->vec4);
    r.ptrStorageUint = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->uintType);
    r.vec4Array = spvTypeArray(s, s->vec4, 0, 16);
    r.uintArray = spvTypeArray(s, s->uintType, 0, 4);
    SpvId instances = spvResource(s, blockOf(s, r.vec4Array, true), SPV_STORAGE_StorageBuffer, 1);
    SpvId visible = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 2);
    SpvId state = spvResource(s, blockOf(s, r.vec4Array, true), SPV_STORAGE_StorageBuffer, 66);
    SpvId rows = spvResource(s, blockOf(s, r.vec4Array, false), SPV_STORAGE_StorageBuffer, 67);
    SpvId pushBlock = spvTypeStruct(s, &s->uintType, 1);
    spvDecorate(s, pushBlock, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, pushBlock, 0, SPV_DECORATE_Offset, 0);
    SpvId push = spvVariable(s, spvTypePointer(s, SPV_STORAGE_PushConstant, pushBlock), SPV_STORAGE_PushConstant);
    SpvId uvec3 = spvNewId(s);
    spvEmit(&s->types, 23 /* OpTypeVector */, SPV_WORDS(uvec3, s->uintType, 3));
    SpvId global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, uvec3), SPV_STORAGE_Input);
    spvDecorate(s, global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);
    SpvId ptrFn4 = spvTypePointer(s, SPV_STORAGE_Function, s->vec4), ptrFnUint = spvTypePointer(s, SPV_STORAGE_Function, s->uintType);
    SpvId bvec4 = spvTypeVector(s, s->boolType, 4);

    SpvId main = spvBeginMain(s);
    SpvId vp0 = VAR(ptrFn4), vp1 = VAR(ptrFn4), vp2 = VAR(ptrFn4), vi = VAR(ptrFnUint);
    SpvId x = spvExtract(s, s->uintType, spvLoad(s, uvec3, global), 0);
    SpvId which = spvLoad(s, s->uintType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_PushConstant, s->uintType), push, &r.zero, 1));
    SpvId start = AS_UINT(X(bufferVec(s, &r, rows, which)));
    SpvId head = bufferVec(s, &r, rows, start), where = bufferVec(s, &r, rows, ADD_U(start, UINT(1)));
    SpvId stepCount = AS_UINT(X(head)), places = AS_UINT(Y(head)), offset = AS_UINT(Z(head)), fresh = NOT_EQ(AS_UINT(W(head)), UINT(0));
    SpvId merge;
    spvIfBegin(s, LESS_U(x, places), &merge);
    SpvId at = MUL_U(ADD_U(offset, x), UINT(3));
    const SpvId zero4[4] = {FLOAT(0.f), FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)}, none4[4] = {FLOAT(0.f), FLOAT(0.f), FLOAT(0.f), FLOAT(-1.f)};
    SpvId zero = spvCompose(s, s->vec4, zero4, 4), none = spvCompose(s, s->vec4, none4, 4);
    SpvId fresh4 = spvCompose(s, bvec4, (const SpvId[4]){fresh, fresh, fresh, fresh}, 4);
    spvStore(s, vp0, spvSelect(s, s->vec4, fresh4, none, bufferVec(s, &r, state, at)));
    spvStore(s, vp1, spvSelect(s, s->vec4, fresh4, zero, bufferVec(s, &r, state, ADD_U(at, UINT(1)))));
    spvStore(s, vp2, spvSelect(s, s->vec4, fresh4, zero, bufferVec(s, &r, state, ADD_U(at, UINT(2)))));
    spvStore(s, vi, UINT(0));

    /* each step it has not seen */
    Loop loop;
    loopOpen(s, &loop);
    SpvId i = LOAD_U(vi);
    loopWhile(s, &loop, LESS_U(i, stepCount));
    {
        SpvId row = ADD_U(ADD_U(start, UINT(6)), MUL_U(i, UINT(ARTF_SWARM_ROWS)));
        SpvId q[7];
        for (uint32_t k = 0; k < 7; ++k) q[k] = bufferVec(s, &r, rows, ADD_U(row, UINT(k)));
        SpvId first = AS_UINT(Y(q[5])), count = AS_UINT(Z(q[5])), firstBirth = AS_UINT(W(q[5])), stepPlaces = AS_UINT(X(q[6])), seed = AS_UINT(Y(q[6]));
        SpvId seconds = X(q[3]), life = Y(q[3]), spin = X(q[5]);
        SpvId j = spvOp2(s, SPV_OP_UMod, s->uintType, SUB_U(ADD_U(x, stepPlaces), first), stepPlaces);
        /* born here */
        SpvId born;
        spvIfBegin(s, LESS_U(j, count), &born);
        {
            SpvId birth = ADD_U(firstBirth, j);
            SpvId rnd[9];
            for (uint32_t k = 0; k < 9; ++k) rnd[k] = swarmRandom(s, seed, birth, k);
            SpvId p[3], v[3];
            for (int k = 0; k < 3; ++k) {
                SpvId origin = spvExtract(s, s->floatType, q[0], (uint32_t)k), leave = spvExtract(s, s->floatType, q[1], (uint32_t)k);
                p[k] = ADD_F(origin, MUL_F(SUB_F(MUL_F(rnd[3 + k], FLOAT(2.f)), FLOAT(1.f)), W(q[0])));
                v[k] = ADD_F(leave, MUL_F(SUB_F(MUL_F(rnd[k], FLOAT(2.f)), FLOAT(1.f)), W(q[1])));
            }
            SpvId turns = SW_NONZERO(spin);
            SpvId angle = PICK_F(turns, MUL_F(rnd[7], FLOAT(6.28318531f)), FLOAT(0.f));
            SpvId turning = PICK_F(turns, MUL_F(SUB_F(MUL_F(rnd[8], FLOAT(2.f)), FLOAT(1.f)), spin), FLOAT(0.f));
            const SpvId a0[4] = {p[0], p[1], p[2], MUL_F(rnd[6], seconds)}, a1[4] = {v[0], v[1], v[2], angle};
            const SpvId a2[4] = {turning, spvOp1(s, SPV_OP_Bitcast, s->floatType, birth), FLOAT(0.f), FLOAT(0.f)};
            spvStore(s, vp0, spvCompose(s, s->vec4, a0, 4));
            spvStore(s, vp1, spvCompose(s, s->vec4, a1, 4));
            spvStore(s, vp2, spvCompose(s, s->vec4, a2, 4));
        }
        spvIfEnd(s, born);
        /* moved, if it is there */
        SpvId alive;
        SpvId p0 = spvLoad(s, s->vec4, vp0);
        spvIfBegin(s, spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, W(p0), FLOAT(0.f)), &alive);
        {
            SpvId before = spvLoad(s, s->vec4, vp1); /* (what dies keeps it) */
            SpvId age = ADD_F(W(spvLoad(s, s->vec4, vp0)), seconds);
            SpvId dies = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, age, life);
            SpvId turbulence = Z(q[3]);
            SpvId swirl;
            spvIfBegin(s, SW_NONZERO(turbulence), &swirl);
            {
                SpvId here = spvLoad(s, s->vec4, vp0), going = spvLoad(s, s->vec4, vp1);
                SpvId sx = MUL_F(X(here), W(q[3])), sy = MUL_F(Y(here), W(q[3])), sz = MUL_F(Z(here), W(q[3]));
                SpvId pushed[3];
                for (int k = 0; k < 3; ++k) {
                    SpvId n = swarmNoise(s, ADD_F(sx, FLOAT(31.4f * (float)k)), sy, sz, UINT(0x51ED270Bu + (uint32_t)k));
                    pushed[k] = ADD_F(spvExtract(s, s->floatType, going, (uint32_t)k), MUL_F(MUL_F(n, turbulence), seconds));
                }
                const SpvId g4[4] = {pushed[0], pushed[1], pushed[2], W(going)};
                spvStore(s, vp1, spvCompose(s, s->vec4, g4, 4));
            }
            spvIfEnd(s, swirl);
            SpvId here = spvLoad(s, s->vec4, vp0), going = spvLoad(s, s->vec4, vp1), turn = spvLoad(s, s->vec4, vp2);
            SpvId slow = DIV_F(FLOAT(1.f), ADD_F(FLOAT(1.f), MUL_F(W(q[2]), seconds)));
            SpvId p[3], v[3];
            for (uint32_t k = 0; k < 3; ++k) {
                v[k] = MUL_F(ADD_F(spvExtract(s, s->floatType, going, k), MUL_F(spvExtract(s, s->floatType, q[2], k), seconds)), slow);
                p[k] = ADD_F(spvExtract(s, s->floatType, here, k), MUL_F(v[k], seconds));
            }
            /* off the ground */
            SpvId ground = BOTH(SW_NONZERO(X(q[4])), LESS_F(p[1], Y(q[4])));
            SpvId bounce = Z(q[4]), keep = W(q[4]);
            p[1] = PICK_F(ground, ADD_F(Y(q[4]), MUL_F(SUB_F(Y(q[4]), p[1]), bounce)), p[1]);
            v[1] = PICK_F(BOTH(ground, LESS_F(v[1], FLOAT(0.f))), MUL_F(spvOp1(s, SPV_OP_FNegate, s->floatType, v[1]), bounce), v[1]);
            v[0] = PICK_F(ground, MUL_F(v[0], keep), v[0]);
            v[2] = PICK_F(ground, MUL_F(v[2], keep), v[2]);
            SpvId angle = ADD_F(W(going), MUL_F(X(turn), seconds));
            /* (what dies keeps where it was, as on the CPU) */
            const SpvId a0[4] = {PICK_F(dies, X(here), p[0]), PICK_F(dies, Y(here), p[1]), PICK_F(dies, Z(here), p[2]), PICK_F(dies, FLOAT(-1.f), age)};
            const SpvId a1[4] = {PICK_F(dies, X(before), v[0]), PICK_F(dies, Y(before), v[1]), PICK_F(dies, Z(before), v[2]), PICK_F(dies, W(before), angle)};
            spvStore(s, vp0, spvCompose(s, s->vec4, a0, 4));
            spvStore(s, vp1, spvCompose(s, s->vec4, a1, 4));
        }
        spvIfEnd(s, alive);
        spvStore(s, vi, ADD_U(i, UINT(1)));
    }
    loopClose(s, &loop);

    SpvId p0 = spvLoad(s, s->vec4, vp0), p1 = spvLoad(s, s->vec4, vp1), p2 = spvLoad(s, s->vec4, vp2);
    spvStore(s, spvAccess(s, r.ptrStorageVec4, state, (const SpvId[2]){r.zero, at}, 2), p0);
    spvStore(s, spvAccess(s, r.ptrStorageVec4, state, (const SpvId[2]){r.zero, ADD_U(at, UINT(1))}, 2), p1);
    spvStore(s, spvAccess(s, r.ptrStorageVec4, state, (const SpvId[2]){r.zero, ADD_U(at, UINT(2))}, 2), p2);

    /* facing the eye, as an instance of the quad */
    SpvId f[4];
    for (uint32_t k = 0; k < 4; ++k) f[k] = bufferVec(s, &r, rows, ADD_U(start, UINT(2 + k)));
    SpvId alive = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, W(p0), FLOAT(0.f));
    SpvId t = DIV_F(W(p0), W(f[2]));
    SpvId sized = ADD_F(W(f[0]), MUL_F(SUB_F(W(f[1]), W(f[0])), t));
    SpvId angle = W(p1), turned = SW_NONZERO(angle);
    SpvId c = PICK_F(turned, swarmSine(s, ADD_F(angle, FLOAT(1.57079633f))), FLOAT(1.f)), sn = PICK_F(turned, swarmSine(s, angle), FLOAT(0.f));
    SpvId instance = ADD_U(AS_UINT(X(where)), x), quad = AS_UINT(Z(where));
    SpvId alive4 = spvCompose(s, bvec4, (const SpvId[4]){alive, alive, alive, alive}, 4);
    for (uint32_t k = 0; k < 3; ++k) {
        SpvId right = spvExtract(s, s->floatType, f[0], k), forward = spvExtract(s, s->floatType, f[1], k), up = spvExtract(s, s->floatType, f[2], k);
        const SpvId m[4] = {MUL_F(ADD_F(MUL_F(right, c), MUL_F(up, sn)), sized), MUL_F(spvOp1(s, SPV_OP_FNegate, s->floatType, forward), sized),
                            MUL_F(spvOp1(s, SPV_OP_FNegate, s->floatType, SUB_F(MUL_F(up, c), MUL_F(right, sn))), sized), spvExtract(s, s->floatType, p0, k)};
        SpvId rowValue = spvSelect(s, s->vec4, alive4, spvCompose(s, s->vec4, m, 4), zero);
        spvStore(s, spvAccess(s, r.ptrStorageVec4, instances, (const SpvId[2]){r.zero, ADD_U(MUL_U(instance, UINT(4)), UINT(k))}, 2), rowValue);
    }
    SpvId shade = TO_U(MIN_F(MAX_F(FLOOR_F(MUL_F(t, FLOAT((float)ARTF_SWARM_SHADES))), FLOAT(0.f)), FLOAT((float)(ARTF_SWARM_SHADES - 1))));
    SpvId material = PICK_F(alive, spvOp1(s, SPV_OP_Bitcast, s->floatType, ADD_U(AS_UINT(Z(f[3])), shade)), Z(f[3]));
    SpvId opacity = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, ADD_F(MUL_F(MIN_F(MAX_F(ADD_F(X(f[3]), MUL_F(SUB_F(Y(f[3]), X(f[3])), t)), FLOAT(0.f)), FLOAT(1.f)), FLOAT(65535.f)), FLOAT(.5f)));
    opacity = spvSelect(s, s->uintType, alive, opacity, UINT(0));
    SpvId bits = spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, opacity, UINT((uint32_t)(ARTF_INSTANCE_UNIFORM | ARTF_INSTANCE_CUTOUT | ARTF_INSTANCE_PARTICLE) << 16 | 1u << 24));
    const SpvId meta[4] = {spvOp1(s, SPV_OP_Bitcast, s->floatType, quad), material, PICK_F(alive, ABS_F(sized), FLOAT(0.f)), spvOp1(s, SPV_OP_Bitcast, s->floatType, bits)};
    spvStore(s, spvAccess(s, r.ptrStorageVec4, instances, (const SpvId[2]){r.zero, ADD_U(MUL_U(instance, UINT(4)), UINT(3))}, 2), spvCompose(s, s->vec4, meta, 4));
    SpvId copy = MUL_U(ADD_U(AS_UINT(Y(where)), x), UINT(2));
    spvStore(s, bufferUintPtr(s, &r, visible, copy), instance);
    spvStore(s, bufferUintPtr(s, &r, visible, ADD_U(copy, UINT(1))), quad);
    spvIfEnd(s, merge);
    spvEndMain(s);
    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &global, 1);
    spvExecutionMode(s, main, 17 /* LocalSize */, 64, 1, 1, 3);
    return finish(s, size);
}

#undef SW_INT
#undef SW_ADD_I
#undef SW_TO_I
#undef SW_TO_F
#undef SW_SMOOTH
#undef SW_NONZERO

#include "vulkan/postShaders.inc"
