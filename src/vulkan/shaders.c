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
 *
 * Lighting here is the same arithmetic as the CPU's (kernels.inc), so that both draw the same scene. */
#include "vulkan/shaders.h"
#include "scene/sceneInternal.h"
#include "vulkan/spirv.h"

#include <stdlib.h>

/* where things are in the frame's vec4s */
enum { F_VIEWPROJ = 0, F_PLANES = 4, F_CAMERA = 9, F_TOSUN = 10, F_SUNCOLOR = 11, F_SKY = 12, F_GROUND = 13, F_COUNTS = 15, F_LIGHTS = 16, F_LIGHT_ROWS = 3, F_PROJECTION = 64, F_HIZ = 65, F_LOOK = 80, F_FOG = 81, F_EYE = 82, F_SIZE = 85 };

typedef struct Resources {
    SpvId frame, instances, visible, meshes, materials, indirect, hiz, flags, environment;
    SpvId ptrUniformVec4, ptrStorageVec4, ptrStorageUint, ptrStorageFloat, uintArray, vec4Array;
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
    r->environment = spvResource(s, blockOf(s, spvTypeArray(s, s->floatType, 0, 4), false), SPV_STORAGE_StorageBuffer, 49);
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
static SpvId same3(Spv* s, float v) { SpvId c = spvConstFloat(s, v); return vec3Of(s, c, c, c); }
/* condition ? a : b */
static SpvId pick3(Spv* s, SpvId condition, SpvId a, SpvId b) {
    SpvId x = spvSelect(s, s->floatType, condition, spvExtract(s, s->floatType, a, 0), spvExtract(s, s->floatType, b, 0));
    SpvId y = spvSelect(s, s->floatType, condition, spvExtract(s, s->floatType, a, 1), spvExtract(s, s->floatType, b, 1));
    SpvId z = spvSelect(s, s->floatType, condition, spvExtract(s, s->floatType, a, 2), spvExtract(s, s->floatType, b, 2));
    return vec3Of(s, x, y, z);
}
/* As toneMap in kernels.inc, where exposure is above 0. */
static SpvId exposed(Spv* s, SpvId light, SpvId exposure) {
    SpvId v = spvOp2(s, SPV_OP_VectorTimesScalar, s->vec3, light, exposure);
    SpvId top = spvOp2(s, SPV_OP_FMul, s->vec3, v, spvOp2(s, SPV_OP_FAdd, s->vec3, spvOp2(s, SPV_OP_FMul, s->vec3, v, same3(s, 2.51f)), same3(s, .03f)));
    SpvId bottom = spvOp2(s, SPV_OP_FAdd, s->vec3, spvOp2(s, SPV_OP_FMul, s->vec3, v, spvOp2(s, SPV_OP_FAdd, s->vec3, spvOp2(s, SPV_OP_FMul, s->vec3, v, same3(s, 2.43f)), same3(s, .59f))), same3(s, .14f));
    SpvId mapped = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvOp2(s, SPV_OP_FDiv, s->vec3, top, bottom), same3(s, 1.f));
    return pick3(s, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, exposure, spvConstFloat(s, 0.f)), mapped, light);
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
    SpvId closest = SUB_F(cw, radius);
    SpvId safe = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, closest, nearW);
    SpvId nearest = MAX_F(closest, nearW), farthest = ADD_F(cw, radius);
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
    SpvId depth = DIV_F(nearW, nearest);
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

uint32_t* artfShaderSceneVertex(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    Resources r;
    declare(s, &r, false);
    SpvId colors = spvResource(s, blockOf(s, r.uintArray, false), SPV_STORAGE_StorageBuffer, 16);

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

    SpvId main = spvBeginMain(s);
    /* the copy drawn is this instance */
    SpvId copy = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvLoad(s, s->intType, instanceIndex));
    SpvId at = MUL_U(copy, UINT(2));
    SpvId id = spvLoad(s, s->uintType, bufferUintPtr(s, &r, r.visible, at));
    /* (a mesh's number, and above it how far it is in a fade) */
    SpvId meshWord = spvLoad(s, s->uintType, bufferUintPtr(s, &r, r.visible, ADD_U(at, UINT(1))));
    SpvId mesh = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, meshWord, UINT((1u << 23) - 1));
    spvStore(s, vFade, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, meshWord, UINT(23)));
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

    /* its color, where the mesh has colors; squared, as a texture's texels are; and how much of it shows */
    SpvId colorBase = AS_UINT(W(scaleRow)), firstVertex = AS_UINT(W(offsetRow));
    SpvId vertex = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvLoad(s, s->intType, vertexIndex));
    SpvId colored = spvOp2(s, SPV_OP_INotEqual, s->boolType, colorBase, UINT(0));
    SpvId colorAt = spvSelect(s, s->uintType, colored, ADD_U(colorBase, spvOp2(s, SPV_OP_ISub, s->uintType, vertex, firstVertex)), UINT(0));
    SpvId bgra = spvGlsl1(s, SPV_GLSL_UnpackUnorm4x8, s->vec4, spvLoad(s, s->uintType, bufferUintPtr(s, &r, colors, colorAt)));
    SpvId rgb = vec3Of(s, Z(bgra), Y(bgra), X(bgra));
    SpvId opacityBits = spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, AS_UINT(W(meta)), UINT(65535));
    SpvId opacity = DIV_F(spvOp1(s, SPV_OP_ConvertUToF, s->floatType, opacityBits), FLOAT(65535.f));
    spvStore(s, vColor, vec4Of(s, MUL_V3(rgb, rgb), W(bgra)));
    spvStore(s, vOpacity, opacity);
    spvEndMain(s);

    const SpvId interface[14] = {aPosition, aNormal, aUv, instanceIndex, vertexIndex, outVertex, vWorld, vNormal, vUv, vMaterial, vColor, vOpacity, vFade, vInstance};
    spvEntryPoint(s, SPV_MODEL_Vertex, main, interface, 14);
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
            v = vec4Of(s, spvOp2(s, SPV_OP_FMul, s->vec3, rgb, rgb), spvExtract(s, s->floatType, t, 3));
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
enum { S_SPLITS = 0, S_SUN = 1, S_LIGHTS = 2, S_VIEWS = 18, S_VIEW_ROWS = 6, S_INFO = 4, S_PLACE = 5, S_ROWS = S_VIEWS + ARTF_SHADOW_VIEWS * S_VIEW_ROWS };

typedef struct Shadows { SpvId block, atlas, imageType, sampled, ivec2; const Resources* r; } Shadows;

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
    SpvId size = spvOp1(s, SPV_OP_ConvertUToF, s->floatType, AS_UINT(Z(shadowRow(s, h, UINT(S_SUN)))));
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

/* What a surface reflects: as Reflecting in kernels.inc. */
typedef struct Reflecting { SpvId a2, k, ndv, strength, f0; } Reflecting;

static SpvId fifth(Spv* s, SpvId x) {
    SpvId y = SUB_F(FLOAT(1.f), x), y2 = MUL_F(y, y);
    return MUL_F(MUL_F(y2, y2), y);
}

/* The same as addLight in kernels.inc, but for its highlight's Fresnel: that needs the color of what is lit. */
static SpvId lightHighlight(Spv* s, const Reflecting* f, SpvId n, SpvId view, SpvId toLight, SpvId ndl, SpvId strength) {
    SpvId h = NORMALIZE(ADD_V3(view, toLight));
    SpvId ndh = MAX_F(DOT3(n, h), FLOAT(0.f)), vdh = MAX_F(DOT3(view, h), FLOAT(0.f));
    SpvId d = ADD_F(MUL_F(MUL_F(ndh, ndh), SUB_F(f->a2, FLOAT(1.f))), FLOAT(1.f));
    SpvId distribution = DIV_F(f->a2, MUL_F(d, d));
    SpvId rest = SUB_F(FLOAT(1.f), f->k);
    SpvId seen = MUL_F(ADD_F(MUL_F(f->ndv, rest), f->k), ADD_F(MUL_F(ndl, rest), f->k));
    SpvId lit = DIV_F(MUL_F(MUL_F(distribution, ndl), MUL_F(strength, f->strength)), MUL_F(seen, FLOAT(4.f)));
    lit = spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, ndl, FLOAT(0.f)), lit, FLOAT(0.f));
    SpvId fresnel = ADD_V3(f->f0, SCALE_V3(SUB_V3(same3(s, 1.f), f->f0), fifth(s, vdh)));
    return SCALE_V3(fresnel, lit);
}

static void addLight(Spv* s, const Accumulators* a, const Reflecting* f, SpvId n, SpvId view, SpvId toLight, SpvId strength, SpvId color) {
    SpvId ndl = MAX_F(DOT3(n, toLight), FLOAT(0.f));
    SpvId amount = MUL_F(ndl, strength);
    spvStore(s, a->diffuse, ADD_V3(spvLoad(s, s->vec3, a->diffuse), SCALE_V3(color, amount)));
    spvStore(s, a->highlight, ADD_V3(spvLoad(s, s->vec3, a->highlight), MUL_V3(color, lightHighlight(s, f, n, view, toLight, ndl, strength))));
}

/* A texture of the array, by an index that differs from pixel to pixel. */
static SpvId textureAt(Spv* s, SpvId textures, SpvId sampled, SpvId index, SpvId uv) {
    spvDecorate(s, index, SPV_DECORATE_NonUniform, -1);
    SpvId slot = spvAccess(s, spvTypePointer(s, SPV_STORAGE_UniformConstant, sampled), textures, &index, 1);
    spvDecorate(s, slot, SPV_DECORATE_NonUniform, -1);
    SpvId image = spvLoad(s, sampled, slot);
    spvDecorate(s, image, SPV_DECORATE_NonUniform, -1);
    return spvImageSample(s, image, uv);
}

uint32_t* artfShaderSceneFragment(size_t* size, uint32_t maxTextures, bool cutout, const ArtfGraphData* graphs, uint32_t graphCount, int lists) {
    Spv b, *s = &b;
    spvInit(s);
    spvCapability(s, 5301); /* ShaderNonUniform */
    spvCapability(s, 5307); /* SampledImageArrayNonUniformIndexing */
    spvExtension(s, "SPV_EXT_descriptor_indexing");
    Resources r;
    declare(s, &r, false);

    SpvId imageType, sampled = spvTypeSampledImage2D(s, &imageType);
    SpvId textures = spvResource(s, spvTypeArray(s, sampled, maxTextures, 0), SPV_STORAGE_UniformConstant, 6);
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
    if (lists) {
        vInstance = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->uintType), SPV_STORAGE_Input);
        spvDecorate(s, vInstance, SPV_DECORATE_Location, 7);
        spvDecorate(s, vInstance, SPV_DECORATE_Flat, -1);
        listCounts = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 44);
        listFragments = spvResource(s, blockOf(s, r.uintArray, true), SPV_STORAGE_StorageBuffer, 45);
    }
    if (cutout) {
        vFade = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->uintType), SPV_STORAGE_Input);
        spvDecorate(s, vFade, SPV_DECORATE_Location, 6);
        spvDecorate(s, vFade, SPV_DECORATE_Flat, -1);
        fragCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
        spvDecorate(s, fragCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
        vOpacity = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->floatType), SPV_STORAGE_Input);
        spvDecorate(s, vOpacity, SPV_DECORATE_Location, 5);
        spvDecorate(s, vOpacity, SPV_DECORATE_Flat, -1);
        frontFacing = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->boolType), SPV_STORAGE_Input);
        spvDecorate(s, frontFacing, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FrontFacing);
    }
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);

    SpvId main = spvBeginMain(s);
    SpvId ptrFn3 = spvTypePointer(s, SPV_STORAGE_Function, s->vec3), ptrFnUint = spvTypePointer(s, SPV_STORAGE_Function, s->uintType);
    Accumulators acc = {spvVariable(s, ptrFn3, SPV_STORAGE_Function), spvVariable(s, ptrFn3, SPV_STORAGE_Function)};
    SpvId ptrFnFloat = spvTypePointer(s, SPV_STORAGE_Function, s->floatType);
    SpvId shadowed = spvVariable(s, ptrFnFloat, SPV_STORAGE_Function);
    const GraphVars own = {spvVariable(s, ptrFn3, SPV_STORAGE_Function), spvVariable(s, ptrFn3, SPV_STORAGE_Function), spvVariable(s, ptrFn3, SPV_STORAGE_Function),
                           spvVariable(s, ptrFnFloat, SPV_STORAGE_Function), spvVariable(s, ptrFnFloat, SPV_STORAGE_Function), spvVariable(s, ptrFnFloat, SPV_STORAGE_Function)};
    SpvId counter = spvVariable(s, ptrFnUint, SPV_STORAGE_Function);

    SpvId world = spvLoad(s, s->vec3, vWorld);
    SpvId n = NORMALIZE(spvLoad(s, s->vec3, vNormal));
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
    SpvId tangentRaw = SUB_V3(SCALE_V3(dpx, spvExtract(s, s->floatType, duy, 1)), SCALE_V3(dpy, spvExtract(s, s->floatType, dux, 1)));
    SpvId bitangentRaw = SUB_V3(SCALE_V3(dpy, spvExtract(s, s->floatType, dux, 0)), SCALE_V3(dpx, spvExtract(s, s->floatType, duy, 0)));
    SpvId det = SUB_F(MUL_F(spvExtract(s, s->floatType, dux, 0), spvExtract(s, s->floatType, duy, 1)), MUL_F(spvExtract(s, s->floatType, duy, 0), spvExtract(s, s->floatType, dux, 1)));
    SpvId orient = spvSelect(s, s->floatType, spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, det, FLOAT(0.f)), FLOAT(-1.f), FLOAT(1.f));
    SpvId tangent = SCALE_V3(NORMALIZE(tangentRaw), orient), bitangent = SCALE_V3(NORMALIZE(bitangentRaw), orient);
    SpvId bx = MUL_F(SUB_F(MUL_F(X(bumps), FLOAT(2.f)), FLOAT(1.f)), Z(m3)), by = MUL_F(SUB_F(FLOAT(1.f), MUL_F(Y(bumps), FLOAT(2.f))), Z(m3)), bz = SUB_F(MUL_F(Z(bumps), FLOAT(2.f)), FLOAT(1.f));
    SpvId bent = NORMALIZE(ADD_V3(ADD_V3(SCALE_V3(tangent, bx), SCALE_V3(bitangent, by)), SCALE_V3(n, bz)));
    n = pick3(s, spvOp2(s, SPV_OP_INotEqual, s->boolType, AS_UINT(X(m4)), UINT(0)), bent, n);

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
        SpvId hiddenBack = spvOp2(s, SPV_OP_LogicalAnd, s->boolType, spvOp1(s, SPV_OP_LogicalNot, s->boolType, front), spvOp1(s, SPV_OP_LogicalNot, s->boolType, isDouble));
        /* between two levels: as maskHoles */
        SpvId fade = spvLoad(s, s->uintType, vFade), coord = spvLoad(s, s->vec4, fragCoord);
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
    SpvId albedo = MUL_V3(MUL_V3(spvShuffle3(s, m0), MUL_V3(texel, texel)), spvShuffle3(s, tint));

    SpvId specular = W(m0);
    SpvId view = NORMALIZE(SUB_V3(spvShuffle3(s, frameVec(s, &r, F_CAMERA)), world));
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
    SpvId a = MUL_F(rough, rough), plusOne = ADD_F(rough, FLOAT(1.f)), notMetal = SUB_F(FLOAT(1.f), metal);
    SpvId ndv = MAX_F(DOT3(n, view), FLOAT(1e-4f));
    SpvId f0 = ADD_V3(same3(s, 0.f), ADD_V3(SCALE_V3(same3(s, .04f), MUL_F(specular, notMetal)), SCALE_V3(albedo, metal)));
    Reflecting reflecting = {MUL_F(a, a), MUL_F(MUL_F(plusOne, plusOne), FLOAT(.125f)), ndv, specular, f0};
    SpvId shiny = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, ADD_F(specular, metal), FLOAT(0.f));

    SpvId up = ADD_F(MUL_F(spvExtract(s, s->floatType, n, 1), FLOAT(.5f)), FLOAT(.5f));
    SpvId ground = spvShuffle3(s, frameVec(s, &r, F_GROUND)), sky = spvShuffle3(s, frameVec(s, &r, F_SKY));
    spvStore(s, acc.diffuse, SCALE_V3(ADD_V3(ground, SCALE_V3(SUB_V3(sky, ground), up)), open));
    /* what comes from all around, in the mirror direction: all a metal shows in a plain sky */
    SpvId twice = MUL_F(DOT3(n, view), FLOAT(2.f));
    SpvId reflectedUp = ADD_F(MUL_F(SUB_F(MUL_F(Y(n), twice), Y(view)), FLOAT(.5f)), FLOAT(.5f));
    SpvId env = ADD_V3(ground, SCALE_V3(SUB_V3(sky, ground), reflectedUp));
    SpvId smooth = same3(s, 0.f);
    smooth = SUB_V3(spvGlsl2(s, SPV_GLSL_FMax, s->vec3, vec3Of(s, SUB_F(FLOAT(1.f), rough), SUB_F(FLOAT(1.f), rough), SUB_F(FLOAT(1.f), rough)), f0), f0);
    SpvId mirrored = MUL_V3(env, ADD_V3(SCALE_V3(f0, open), SCALE_V3(smooth, MUL_F(fifth(s, ndv), open))));
    spvStore(s, acc.highlight, pick3(s, shiny, mirrored, same3(s, 0.f)));
    /* or from a picture of all around */
    SpvId environmentLight = W(frameVec(s, &r, F_SKY)), aroundMerge;
    spvIfBegin(s, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, environmentLight, FLOAT(0.f)), &aroundMerge);
    {
        spvStore(s, acc.diffuse, SCALE_V3(envSample(s, &r, UINT(ARTF_ENV_DIFFUSE), n), MUL_F(environmentLight, open)));
        SpvId reflected = SUB_V3(SCALE_V3(n, twice), view);
        SpvId around = SCALE_V3(envReflected(s, &r, rough, reflected), environmentLight);
        SpvId kept = MUL_V3(around, ADD_V3(SCALE_V3(f0, open), SCALE_V3(smooth, MUL_F(fifth(s, ndv), open))));
        spvStore(s, acc.highlight, pick3(s, shiny, kept, same3(s, 0.f)));
    }
    spvIfEnd(s, aroundMerge);

    /* the sun's shadow: as artfShadowOfSun */
    SpvId eye = spvShuffle3(s, frameVec(s, &r, F_CAMERA));
    SpvId sunRow = shadowRow(s, &shadows, UINT(S_SUN)), splits = shadowRow(s, &shadows, UINT(S_SPLITS));
    spvStore(s, shadowed, FLOAT(1.f));
    SpvId sunCasts;
    spvIfBegin(s, spvOp2(s, SPV_OP_INotEqual, s->boolType, AS_UINT(X(sunRow)), UINT(0)), &sunCasts);
    {
        SpvId distance = spvGlsl1(s, SPV_GLSL_Length, s->floatType, SUB_V3(world, eye));
        SpvId past0 = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, distance, X(splits)), past1 = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, distance, Y(splits));
        SpvId past2 = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, distance, Z(splits));
        SpvId cascade = spvSelect(s, s->uintType, past1, UINT(2), spvSelect(s, s->uintType, past0, UINT(1), UINT(0)));
        SpvId sunView = ADD_U(ADD_U(AS_UINT(X(sunRow)), UINT(0xFFFFFFFFu)), cascade);
        SpvId viewBase = ADD_U(UINT(S_VIEWS), MUL_U(sunView, UINT(S_VIEW_ROWS)));
        SpvId facing = MAX_F(DOT3(n, spvShuffle3(s, shadowRow(s, &shadows, ADD_U(viewBase, UINT(S_PLACE))))), FLOAT(.15f));
        SpvId lift = DIV_F(MUL_F(Z(shadowRow(s, &shadows, ADD_U(viewBase, UINT(S_INFO)))), FLOAT(2.f)), facing);
        SpvId seen = shadowLookup(s, &shadows, sunView, ADD_V3(world, SCALE_V3(n, lift)));
        spvStore(s, shadowed, spvSelect(s, s->floatType, past2, FLOAT(1.f), seen));
    }
    spvIfEnd(s, sunCasts);
    addLight(s, &acc, &reflecting, n, view, spvShuffle3(s, frameVec(s, &r, F_TOSUN)), spvLoad(s, s->floatType, shadowed), spvShuffle3(s, frameVec(s, &r, F_SUNCOLOR)));

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
        SpvId toLight = SCALE_V3(d, invLength);
        SpvId spot = MUL_F(SUB_F(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(toLight, spvShuffle3(s, lightC))), W(lightC)), W(lightB));
        spot = MIN_F(MAX_F(spot, FLOAT(0.f)), FLOAT(1.f));
        /* its shadow: as artfShadowOfLight */
        SpvId lamp = AS_UINT(X(shadowRow(s, &shadows, ADD_U(UINT(S_LIGHTS), i))));
        spvStore(s, shadowed, FLOAT(1.f));
        SpvId lampCasts;
        spvIfBegin(s, spvOp2(s, SPV_OP_INotEqual, s->boolType, lamp, UINT(0)), &lampCasts);
        {
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
            SpvId texel = Z(shadowRow(s, &shadows, ADD_U(ADD_U(UINT(S_VIEWS), MUL_U(lampView, UINT(S_VIEW_ROWS))), UINT(S_INFO))));
            SpvId lift = DIV_F(MUL_F(MUL_F(texel, distance), FLOAT(2.f)), MAX_F(DOT3(n, toLight), FLOAT(.15f)));
            spvStore(s, shadowed, shadowLookup(s, &shadows, lampView, ADD_V3(world, SCALE_V3(n, lift))));
        }
        spvIfEnd(s, lampCasts);
        addLight(s, &acc, &reflecting, n, view, toLight, MUL_F(MUL_F(MUL_F(fade, fade), spot), spvLoad(s, s->floatType, shadowed)), spvShuffle3(s, lightB));
    }
    spvBranch(s, next);
    spvLabel(s, next);
    spvStore(s, counter, ADD_U(spvLoad(s, s->uintType, counter), UINT(1)));
    spvBranch(s, header);
    spvLabel(s, done);

    SpvId lit = ADD_V3(ADD_V3(MUL_V3(SCALE_V3(albedo, notMetal), spvLoad(s, s->vec3, acc.diffuse)), spvLoad(s, s->vec3, acc.highlight)), emission);
    /* air, then film, as on the CPU */
    SpvId look = frameVec(s, &r, F_LOOK), fog = spvShuffle3(s, frameVec(s, &r, F_FOG));
    SpvId air = MUL_F(spvGlsl1(s, SPV_GLSL_Length, s->floatType, SUB_V3(world, spvShuffle3(s, frameVec(s, &r, F_CAMERA)))), Y(look));
    SpvId keep = DIV_F(FLOAT(1.f), ADD_F(ADD_F(FLOAT(1.f), air), MUL_F(MUL_F(air, air), FLOAT(.5f))));
    lit = pick3(s, spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, Y(look), FLOAT(0.f)), ADD_V3(fog, SCALE_V3(SUB_V3(lit, fog), keep)), lit);
    lit = exposed(s, lit, X(look));
    /* to the frame's colors: the square root of light, as on the CPU */
    SpvId lo = vec3Of(s, FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)), hi = vec3Of(s, FLOAT(1.f), FLOAT(1.f), FLOAT(1.f));
    SpvId clamped = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, lit, lo), hi);
    /* premultiplied, as the frame is: 1 for all that is solid */
    SpvId alpha = MIN_F(MAX_F(shown, FLOAT(0.f)), FLOAT(1.f));
    SpvId finished = vec4Of(s, SCALE_V3(spvGlsl1(s, SPV_GLSL_Sqrt, s->vec3, clamped), alpha), alpha);
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
        SpvId who = spvLoad(s, s->uintType, vInstance);
        if (lists > 1) {
            SpvId mask = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvLoad(s, s->intType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_Input, s->intType), sampleMask, &r.zero, 1)));
            who = spvOp2(s, 197, s->uintType, who, spvOp2(s, 196, s->uintType, mask, UINT(28)));
        }
        spvStore(s, bufferUintPtr(s, &r, listFragments, ADD_U(at, UINT(2))), who);
        spvIfEnd(s, fits);
    }
    spvEndMain(s);

    const SpvId interface[12] = {vWorld, vNormal, vUv, vMaterial, vColor, outColor, vOpacity, frontFacing, vFade, fragCoord, vInstance, sampleMask};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, interface, lists > 1 ? 12 : lists ? 11 : cutout ? 10 : 6);
    spvExecutionMode(s, main, 7 /* OriginUpperLeft */, 0, 0, 0, 0);
    if (lists) spvExecutionMode(s, main, 9 /* EarlyFragmentTests */, 0, 0, 0, 0);
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

/* ---- a black hole ----
 *
 * Bindings: 11 the hole: ArtfHoleFrame, as vec4s      12 hdr: light, a vec4 a pixel
 *           13 bloom: the levels, a vec4 a texel      14 map: the disc, floats
 *           15 fluid: the gas, floats
 *
 * The arithmetic of software/lens.inc, which says why it is what it is. */

enum { H_ER = 0, H_ETHETA = 1, H_EPHI = 2, H_RIGHT = 3, H_UP = 4, H_FORWARD = 5, H_METRIC = 6, H_PLACE = 7, H_SPIN = 8, H_FLOW = 9, H_BOOST = 10, H_DISC = 11, H_LOOK = 12, H_IMAGE = 13, H_SIZE = 14,
       H_BLOOM = 15, H_COUNT = 15 + ARTF_BLOOM_LEVELS };

#define HOLE_STEP .13089969f
#define HOLE_STEPS 192
#define HOLE_SLICES 3

typedef struct HoleResources {
    SpvId hole, hdr, bloom, map, fluid;
    SpvId ptrUniformVec4, ptrStorageVec4, ptrStorageFloat;
    SpvId zero;
} HoleResources;

static void holeDeclare(Spv* s, HoleResources* r, bool writeHdr, bool writeBloom, bool withMap) {
    r->zero = spvConstInt(s, 0);
    r->ptrUniformVec4 = spvTypePointer(s, SPV_STORAGE_Uniform, s->vec4);
    r->ptrStorageVec4 = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->vec4);
    r->ptrStorageFloat = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->floatType);
    r->hole = spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, H_COUNT, 16), true), SPV_STORAGE_Uniform, 11);
    SpvId vec4Array = spvTypeArray(s, s->vec4, 0, 16);
    r->hdr = spvResource(s, blockOf(s, vec4Array, writeHdr), SPV_STORAGE_StorageBuffer, 12);
    r->bloom = spvResource(s, blockOf(s, vec4Array, writeBloom), SPV_STORAGE_StorageBuffer, 13);
    if (withMap) {
        SpvId floatArray = spvTypeArray(s, s->floatType, 0, 4);
        r->map = spvResource(s, blockOf(s, floatArray, false), SPV_STORAGE_StorageBuffer, 14);
        r->fluid = spvResource(s, blockOf(s, floatArray, false), SPV_STORAGE_StorageBuffer, 15);
    }
}

static SpvId holeVec(Spv* s, const HoleResources* r, uint32_t i) {
    const SpvId idx[2] = {r->zero, spvConstUint(s, i)};
    return spvLoad(s, s->vec4, spvAccess(s, r->ptrUniformVec4, r->hole, idx, 2));
}
static SpvId holeVecAt(Spv* s, const HoleResources* r, SpvId i) {
    const SpvId idx[2] = {r->zero, i};
    return spvLoad(s, s->vec4, spvAccess(s, r->ptrUniformVec4, r->hole, idx, 2));
}
static SpvId pixelPtr(Spv* s, const HoleResources* r, SpvId buffer, SpvId i) {
    const SpvId idx[2] = {r->zero, i};
    return spvAccess(s, r->ptrStorageVec4, buffer, idx, 2);
}
static SpvId floatOf(Spv* s, const HoleResources* r, SpvId buffer, SpvId i) {
    const SpvId idx[2] = {r->zero, i};
    return spvLoad(s, s->floatType, spvAccess(s, r->ptrStorageFloat, buffer, idx, 2));
}
static SpvId mapFloat(Spv* s, const HoleResources* r, SpvId i) { return floatOf(s, r, r->map, i); }

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

static SpvId holeHash(Spv* s, SpvId x, SpvId y, SpvId z) {
    SpvId h = XOR_U(XOR_U(MUL_U(x, UINT(73856093)), MUL_U(y, UINT(19349663))), MUL_U(z, UINT(83492791)));
    h = MUL_U(XOR_U(h, SHR_U(h, 15)), UINT(0x2C1B3C6D));
    h = MUL_U(XOR_U(h, SHR_U(h, 12)), UINT(0x297A2D39));
    return XOR_U(h, SHR_U(h, 15));
}

static SpvId holeStars(Spv* s, SpvId dir, float cells, uint32_t density, float size, float gain) {
    SpvId p = SCALE_V3(dir, FLOAT(cells));
    SpvId cell = spvGlsl1(s, SPV_GLSL_Floor, s->vec3, p);
    SpvId h = holeHash(s, TO_U(X(cell)), TO_U(Y(cell)), TO_U(Z(cell)));
    SpvId here = spvOp2(s, SPV_OP_ULessThan, s->boolType, AND_U(h, UINT(255)), UINT(density));
    SpvId d = SUB_V3(SUB_V3(p, cell), vec3Of(s, BYTE_OF(h, 8), BYTE_OF(h, 16), BYTE_OF(h, 24)));
    SpvId light = MAX_F(SUB_F(FLOAT(1.f), MUL_F(DOT3(d, d), FLOAT(1.f / (size * size)))), FLOAT(0.f));
    light = PICK_F(here, MUL_F(MUL_F(light, light), FLOAT(gain)), FLOAT(0.f));
    SpvId warm = BYTE_OF(h, 3);
    return SCALE_V3(vec3Of(s, ADD_F(FLOAT(.75f), MUL_F(warm, FLOAT(.25f))), FLOAT(.82f), SUB_F(FLOAT(1.f), MUL_F(warm, FLOAT(.45f)))), light);
}

/* What a row of a map holds at a turn, between two columns. width: a power of two. */
static SpvId rowOf(Spv* s, const HoleResources* r, SpvId buffer, uint32_t start, uint32_t width, SpvId row, SpvId turn) {
    SpvId at = MUL_F(SUB_F(turn, FLOOR_F(turn)), FLOAT((float)width));
    SpvId column = FLOOR_F(at), part = SUB_F(at, column);
    SpvId base = ADD_U(UINT(start), MUL_U(row, UINT(width)));
    SpvId c0 = AND_U(TO_U(column), UINT(width - 1)), c1 = AND_U(ADD_U(c0, UINT(1)), UINT(width - 1));
    SpvId a = floatOf(s, r, buffer, ADD_U(base, c0)), b = floatOf(s, r, buffer, ADD_U(base, c1));
    return ADD_F(a, MUL_F(SUB_F(b, a), part));
}
static SpvId holeRow(Spv* s, const HoleResources* r, uint32_t start, uint32_t width, SpvId row, SpvId turn) { return rowOf(s, r, r->map, start, width, row, turn); }

/* The loop's state lives in these variables. */
typedef struct RayState { SpvId u, w, n, v, psi, drag, through, sum, running, count; } RayState;

/* What goes on along a ray: fixed for it. */
typedef struct RayConstants { SpvId big, uA, uB, uC, a, a2, twoA2, xiA; } RayConstants;

static SpvId rayBend(Spv* s, const RayConstants* k, SpvId x) { return MUL_F(x, ADD_F(k->uA, MUL_F(x, ADD_F(k->uB, MUL_F(x, k->uC))))); }

static SpvId rayDrag(Spv* s, const RayConstants* k, SpvId x) {
    SpvId top = MUL_F(MUL_F(MUL_F(FLOAT(-1.f), k->a), x), SUB_F(FLOAT(2.f), MUL_F(k->xiA, x)));
    SpvId bottom = MAX_F(ADD_F(SUB_F(FLOAT(1.f), ADD_F(x, x)), MUL_F(MUL_F(k->a2, x), x)), FLOAT(1e-3f));
    return DIV_F(top, bottom);
}

/* n'' for a place n on the sphere. */
static SpvId rayPull(Spv* s, const RayConstants* k, SpvId n) {
    SpvId m = Z(n);
    SpvId c = ADD_F(k->big, MUL_F(k->twoA2, MUL_F(m, m)));
    return ADD_V3(SCALE_V3(n, MUL_F(FLOAT(-1.f), c)), vec3Of(s, FLOAT(0.f), FLOAT(0.f), MUL_F(k->a2, m)));
}

/* What a ray from the pixel at (nx, ny) brings back, at this moment. */
static SpvId holeTrace(Spv* s, const HoleResources* r, const RayState* v, SpvId nx, SpvId ny, SpvId time) {
    SpvId right = spvShuffle3(s, holeVec(s, r, H_RIGHT)), up = spvShuffle3(s, holeVec(s, r, H_UP)), forward = spvShuffle3(s, holeVec(s, r, H_FORWARD));
    SpvId d = NORMALIZE(ADD_V3(ADD_V3(forward, SCALE_V3(right, nx)), SCALE_V3(up, ny)));

    /* a moving eye */
    SpvId boost = holeVec(s, r, H_BOOST);
    SpvId beta = spvShuffle3(s, boost), gamma = W(boost);
    SpvId share = DIV_F(gamma, ADD_F(gamma, FLOAT(1.f)));
    SpvId c = MUL_F(FLOAT(-1.f), DOT3(d, beta));
    SpvId push = MUL_F(gamma, ADD_F(FLOAT(1.f), MUL_F(share, c)));
    SpvId moved = DIV_F(FLOAT(1.f), MUL_F(gamma, ADD_F(FLOAT(1.f), c)));
    d = SCALE_V3(SUB_V3(d, SCALE_V3(beta, push)), moved);

    /* what does not change along the ray */
    SpvId metric = holeVec(s, r, H_METRIC), place = holeVec(s, r, H_PLACE), spin = holeVec(s, r, H_SPIN);
    SpvId rho = X(place), rootDelta = Y(place), u0 = Z(place), mu0 = W(place);
    RayConstants k;
    k.a = X(spin);
    k.a2 = MUL_F(k.a, k.a);
    k.twoA2 = MUL_F(k.a2, FLOAT(2.f));
    SpvId dr = X(d), dt = Y(d), dp = Z(d);
    SpvId invEnergy = DIV_F(FLOAT(1.f), SUB_F(X(metric), MUL_F(Y(metric), dp)));
    SpvId xi = MUL_F(FLOAT(-1.f), MUL_F(MUL_F(Z(metric), dp), invEnergy));
    SpvId xiAxis = MUL_F(MUL_F(W(metric), dp), invEnergy);
    SpvId across = MUL_F(MUL_F(rho, dt), invEnergy);
    SpvId eta = ADD_F(MUL_F(across, across), MUL_F(MUL_F(mu0, mu0), SUB_F(MUL_F(xiAxis, xiAxis), k.a2)));
    k.big = ADD_F(eta, MUL_F(xi, xi));
    SpvId step = DIV_F(FLOAT(HOLE_STEP), SQRT_F(MAX_F(k.big, FLOAT(4.f))));
    SpvId step2 = MUL_F(step, step);
    k.uA = SUB_F(k.a2, k.big);
    SpvId xiOff = SUB_F(xi, k.a);
    k.uB = MUL_F(FLOAT(3.f), ADD_F(eta, MUL_F(xiOff, xiOff)));
    k.uC = MUL_F(MUL_F(FLOAT(-2.f), k.a2), eta);
    k.xiA = MUL_F(xi, k.a);

    SpvId disc = holeVec(s, r, H_DISC), look = holeVec(s, r, H_LOOK);
    SpvId inner = X(disc), thickness = W(disc), gain = X(look), doppler = Y(look), starGain = Z(look), discHeight = Z(spin), uHorizon = Y(spin);
    SpvId rhoScale = DIV_F(FLOAT(1.f), SUB_F(FLOAT(1.f), DIV_F(inner, Y(disc))));

    spvStore(s, v->u, u0);
    spvStore(s, v->w, MUL_F(MUL_F(MUL_F(MUL_F(FLOAT(-1.f), MUL_F(MUL_F(u0, u0), MUL_F(rootDelta, rho))), dr), invEnergy), FLOAT(1.f)));
    spvStore(s, v->n, spvShuffle3(s, holeVec(s, r, H_ER)));
    spvStore(s, v->v, ADD_V3(SCALE_V3(spvShuffle3(s, holeVec(s, r, H_ETHETA)), across), SCALE_V3(spvShuffle3(s, holeVec(s, r, H_EPHI)), xiAxis)));
    spvStore(s, v->psi, FLOAT(0.f));
    spvStore(s, v->drag, rayDrag(s, &k, u0));
    spvStore(s, v->through, FLOAT(1.f));
    spvStore(s, v->sum, vec3Of(s, FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)));
    spvStore(s, v->running, spvConstBool(s, true));
    spvStore(s, v->count, UINT(0));

    SpvId header = spvNewId(s), test = spvNewId(s), body = spvNewId(s), next = spvNewId(s), done = spvNewId(s);
    spvBranch(s, header);
    spvLabel(s, header);
    spvLoopMerge(s, done, next);
    spvBranch(s, test);
    spvLabel(s, test);
    spvBranchIf(s, spvLoad(s, s->boolType, v->running), body, done);
    spvLabel(s, body);
    {
        SpvId u = spvLoad(s, s->floatType, v->u), w = spvLoad(s, s->floatType, v->w), n = spvLoad(s, s->vec3, v->n), vel = spvLoad(s, s->vec3, v->v);
        SpvId psi = spvLoad(s, s->floatType, v->psi), drag = spvLoad(s, s->floatType, v->drag);
        SpvId half = MUL_F(step, FLOAT(.5f)), eighth = MUL_F(step2, FLOAT(.125f)), halfSquare = MUL_F(step2, FLOAT(.5f)), sixthSquare = MUL_F(step2, FLOAT(1.f / 6.f)), sixth = MUL_F(step, FLOAT(1.f / 6.f));
        SpvId k1 = rayBend(s, &k, u), a1 = rayPull(s, &k, n);
        SpvId midU = ADD_F(ADD_F(u, MUL_F(half, w)), MUL_F(eighth, k1));
        SpvId midN = ADD_V3(ADD_V3(n, SCALE_V3(vel, half)), SCALE_V3(a1, eighth));
        SpvId k2 = rayBend(s, &k, midU), a2 = rayPull(s, &k, midN);
        SpvId endU = ADD_F(ADD_F(u, MUL_F(step, w)), MUL_F(halfSquare, k2));
        SpvId endN = ADD_V3(ADD_V3(n, SCALE_V3(vel, step)), SCALE_V3(a2, halfSquare));
        SpvId k3 = rayBend(s, &k, endU), a3 = rayPull(s, &k, endN);
        SpvId nextU = ADD_F(ADD_F(u, MUL_F(step, w)), MUL_F(sixthSquare, ADD_F(k1, ADD_F(k2, k2))));
        SpvId nextW = ADD_F(w, MUL_F(sixth, ADD_F(ADD_F(k1, k3), MUL_F(FLOAT(4.f), k2))));
        SpvId nextN = ADD_V3(ADD_V3(n, SCALE_V3(vel, step)), SCALE_V3(ADD_V3(a1, ADD_V3(a2, a2)), sixthSquare));
        SpvId nextV = ADD_V3(vel, SCALE_V3(ADD_V3(ADD_V3(a1, a3), SCALE_V3(a2, FLOAT(4.f))), sixth));
        SpvId nextDrag = rayDrag(s, &k, nextU);
        SpvId nextPsi = ADD_F(psi, MUL_F(sixth, ADD_F(ADD_F(drag, nextDrag), MUL_F(FLOAT(4.f), rayDrag(s, &k, midU)))));

        /* through the disc's plane? */
        SpvId m = Z(n), nextM = Z(nextN);
        SpvId crossMerge;
        spvIfBegin(s, LESS_F(MUL_F(m, nextM), FLOAT(0.f)), &crossMerge);
        {
            SpvId part = DIV_F(m, SUB_F(m, nextM));
            SpvId hu = MUL_F(part, step), hh = MUL_F(MUL_F(hu, hu), FLOAT(.5f));
            SpvId uc = ADD_F(ADD_F(u, MUL_F(hu, w)), MUL_F(hh, k1));
            SpvId rhoCenter = MUL_F(SUB_F(FLOAT(1.f), MUL_F(inner, uc)), rhoScale);
            SpvId hitMerge;
            spvIfBegin(s, BOTH(MORE_F(rhoCenter, FLOAT(-.03f)), LESS_F(rhoCenter, FLOAT(1.02f))), &hitMerge);
            {
                SpvId wc = ADD_F(w, MUL_F(hu, k1));
                SpvId placeC = ADD_V3(ADD_V3(n, SCALE_V3(vel, hu)), SCALE_V3(a1, hh));
                SpvId velC = ADD_V3(vel, SCALE_V3(a1, hu));
                SpvId cx = X(placeC), cy = Y(placeC), vx = X(velC), vy = Y(velC), vz = Z(velC);
                SpvId psiC = ADD_F(psi, MUL_F(part, SUB_F(nextPsi, psi))), dragC = ADD_F(drag, MUL_F(part, SUB_F(nextDrag, drag)));
                SpvId turn = ADD_F(MUL_F(spvGlsl2(s, SPV_GLSL_Atan2, s->floatType, cy, cx), FLOAT(.15915494f)), MUL_F(psiC, FLOAT(.15915494f)));
                SpvId rate = MUL_F(ADD_F(DIV_F(SUB_F(MUL_F(cx, vy), MUL_F(cy, vx)), MAX_F(ADD_F(MUL_F(cx, cx), MUL_F(cy, cy)), FLOAT(1e-6f))), dragC), FLOAT(.15915494f));

                SpvId slope = DIV_F(wc, uc);
                SpvId speed2 = ADD_F(DOT3(velC, velC), MUL_F(slope, slope));
                SpvId mu = MAX_F(DIV_F(ABS_F(vz), SQRT_F(speed2)), FLOAT(.02f));
                SpvId slant = DIV_F(MUL_F(thickness, FLOAT(1.f / (float)HOLE_SLICES)), mu);
                SpvId reach = MIN_F(DIV_F(discHeight, MAX_F(ABS_F(vz), FLOAT(1e-5f))), MUL_F(step, FLOAT(1.5f)));

                SpvId glow = FLOAT(0.f), clear = spvLoad(s, s->floatType, v->through);
                for (int slice = 0; slice < HOLE_SLICES; ++slice) {
                    const float t = ((float)slice + .5f) * (2.f / (float)HOLE_SLICES) - 1.f;
                    SpvId ds = MUL_F(reach, FLOAT(t));
                    SpvId uk = ADD_F(uc, MUL_F(wc, ds));
                    SpvId rhoK = MUL_F(SUB_F(FLOAT(1.f), MUL_F(inner, uk)), rhoScale);
                    SpvId inside = BOTH(MORE_F(rhoK, FLOAT(0.f)), LESS_F(rhoK, FLOAT(1.f)));
                    SpvId rhoSafe = MIN_F(MAX_F(rhoK, FLOAT(0.f)), FLOAT(1.f));
                    SpvId rowAt = MUL_F(rhoSafe, FLOAT((float)(ARTF_HOLE_MAP_HEIGHT - 1)));
                    SpvId rowFloor = MIN_F(FLOOR_F(rowAt), FLOAT((float)(ARTF_HOLE_MAP_HEIGHT - 2)));
                    SpvId rowPart = SUB_F(rowAt, rowFloor);
                    SpvId row0 = TO_U(rowFloor), row1 = ADD_U(row0, UINT(1));
                    SpvId rows = UINT(ARTF_HOLE_ROWS_AT);
                    SpvId speed0 = mapFloat(s, r, ADD_U(rows, ADD_U(row0, row0))), speed1 = mapFloat(s, r, ADD_U(rows, ADD_U(row1, row1)));
                    SpvId turnK = ADD_F(turn, MUL_F(rate, ds));
                    SpvId d0 = holeRow(s, r, 0, ARTF_HOLE_MAP_WIDTH, row0, SUB_F(turnK, MUL_F(speed0, time)));
                    SpvId d1 = holeRow(s, r, 0, ARTF_HOLE_MAP_WIDTH, row1, SUB_F(turnK, MUL_F(speed1, time)));
                    SpvId density = ADD_F(d0, MUL_F(SUB_F(d1, d0), rowPart));
                    SpvId fluidAt = MUL_F(rhoSafe, FLOAT((float)(ARTF_FLUID_HEIGHT - 1)));
                    SpvId fluidFloor = MIN_F(FLOOR_F(fluidAt), FLOAT((float)(ARTF_FLUID_HEIGHT - 2)));
                    SpvId fluidRow = TO_U(fluidFloor);
                    SpvId fluidTurn = SUB_F(turnK, MUL_F(speed0, SUB_F(time, X(holeVec(s, r, H_FLOW)))));
                    SpvId g0 = rowOf(s, r, r->fluid, 0, ARTF_FLUID_WIDTH, fluidRow, fluidTurn), g1 = rowOf(s, r, r->fluid, 0, ARTF_FLUID_WIDTH, ADD_U(fluidRow, UINT(1)), fluidTurn);
                    SpvId gas = ADD_F(g0, MUL_F(SUB_F(g1, g0), SUB_F(fluidAt, fluidFloor)));
                    density = MUL_F(density, ADD_F(FLOAT(1.f - ARTF_HOLE_FLUID), MUL_F(FLOAT(ARTF_HOLE_FLUID), gas)));
                    SpvId dustRow = TO_U(MIN_F(FLOOR_F(MUL_F(rhoSafe, FLOAT((float)(ARTF_HOLE_DUST_HEIGHT - 1)))), FLOAT((float)(ARTF_HOLE_DUST_HEIGHT - 1))));
                    SpvId dusty = holeRow(s, r, ARTF_HOLE_DUST_AT, ARTF_HOLE_DUST_WIDTH, dustRow, SUB_F(turnK, MUL_F(speed0, time)));
                    SpvId lit = SUB_F(FLOAT(1.f), MUL_F(FLOAT(ARTF_HOLE_DUST_DIM), dusty));
                    SpvId height = MUL_F(FLOAT(t), DIV_F(reach, discHeight));
                    SpvId level = MAX_F(SUB_F(FLOAT(1.f), MUL_F(MUL_F(vz, vz), MUL_F(height, height))), FLOAT(0.f));
                    density = PICK_F(inside, MUL_F(density, level), FLOAT(0.f));
                    SpvId depth = MUL_F(MUL_F(slant, density), ADD_F(FLOAT(1.f), MUL_F(FLOAT(ARTF_HOLE_DUST_DENSE), dusty)));
                    SpvId alpha = DIV_F(depth, ADD_F(FLOAT(1.f), depth));
                    glow = ADD_F(glow, MUL_F(MUL_F(clear, alpha), MUL_F(density, lit)));
                    clear = MUL_F(clear, SUB_F(FLOAT(1.f), alpha));
                }

                SpvId rowAt = MUL_F(MIN_F(MAX_F(rhoCenter, FLOAT(0.f)), FLOAT(1.f)), FLOAT((float)(ARTF_HOLE_MAP_HEIGHT - 1)));
                SpvId rowIndex = TO_U(rowAt);
                SpvId heat = mapFloat(s, r, ADD_U(UINT(ARTF_HOLE_ROWS_AT + 1), ADD_U(rowIndex, rowIndex)));
                SpvId safeU = MAX_F(uc, FLOAT(1e-4f));
                SpvId root = MUL_F(safeU, SQRT_F(safeU));
                SpvId lift = ADD_F(FLOAT(1.f), MUL_F(k.a, root));
                SpvId orbit = DIV_F(root, lift);
                SpvId beneath = MAX_F(ADD_F(SUB_F(FLOAT(1.f), MUL_F(FLOAT(3.f), safeU)), MUL_F(MUL_F(FLOAT(2.f), k.a), root)), FLOAT(1e-4f));
                SpvId shift = DIV_F(MUL_F(SQRT_F(beneath), MUL_F(invEnergy, moved)), MUL_F(lift, SUB_F(FLOAT(1.f), MUL_F(orbit, xi))));
                SpvId g = ADD_F(FLOAT(1.f), MUL_F(doppler, SUB_F(shift, FLOAT(1.f))));
                SpvId g2 = MUL_F(g, g);
                SpvId gives = mapFloat(s, r, ADD_U(UINT(ARTF_HOLE_GLOW_AT), rowIndex));
                SpvId light = MUL_F(MUL_F(MUL_F(gain, glow), gives), MUL_F(g2, g2));
                heat = MUL_F(heat, g);

                SpvId colorAt = MIN_F(MUL_F(heat, FLOAT((float)(ARTF_HOLE_COLORS / 2))), FLOAT((float)(ARTF_HOLE_COLORS - 1)));
                SpvId index = TO_U(colorAt);
                SpvId at = ADD_U(UINT(ARTF_HOLE_COLORS_AT), ADD_U(ADD_U(index, index), index));
                SpvId color = vec3Of(s, mapFloat(s, r, at), mapFloat(s, r, ADD_U(at, UINT(1))), mapFloat(s, r, ADD_U(at, UINT(2))));
                spvStore(s, v->sum, ADD_V3(spvLoad(s, s->vec3, v->sum), SCALE_V3(color, light)));
                spvStore(s, v->through, clear);
            }
            spvIfEnd(s, hitMerge);
        }
        spvIfEnd(s, crossMerge);

        /* out to the stars */
        SpvId escaped = MORE_F(FLOAT(0.f), nextU);
        SpvId outMerge;
        spvIfBegin(s, escaped, &outMerge);
        {
            SpvId part = DIV_F(u, SUB_F(u, nextU));
            SpvId awayN = NORMALIZE(ADD_V3(n, SCALE_V3(SUB_V3(nextN, n), part)));
            SpvId awayPsi = ADD_F(psi, MUL_F(part, SUB_F(nextPsi, psi)));
            SpvId sine = spvGlsl1(s, SPV_GLSL_Sin, s->floatType, awayPsi), cosine = spvGlsl1(s, SPV_GLSL_Cos, s->floatType, awayPsi);
            SpvId x = X(awayN), y = Y(awayN), height = Z(awayN);
            SpvId dir = vec3Of(s, SUB_F(MUL_F(x, cosine), MUL_F(y, sine)), ADD_F(MUL_F(x, sine), MUL_F(y, cosine)), height);
            SpvId stars = ADD_V3(holeStars(s, dir, 90.f, 26, .1f, 1.6f), holeStars(s, dir, 240.f, 70, .17f, .22f));

            SpvId skyTurn = MUL_F(spvGlsl2(s, SPV_GLSL_Atan2, s->floatType, Y(dir), X(dir)), FLOAT(.15915494f));
            SpvId heightAt = MUL_F(ADD_F(MUL_F(height, FLOAT(.5f)), FLOAT(.5f)), FLOAT((float)(ARTF_HOLE_SKY_HEIGHT - 1)));
            SpvId skyFloor = MIN_F(FLOOR_F(heightAt), FLOAT((float)(ARTF_HOLE_SKY_HEIGHT - 2)));
            SpvId skyRow = TO_U(skyFloor);
            SpvId s0 = holeRow(s, r, ARTF_HOLE_SKY_AT, ARTF_HOLE_SKY_WIDTH, skyRow, skyTurn), s1 = holeRow(s, r, ARTF_HOLE_SKY_AT, ARTF_HOLE_SKY_WIDTH, ADD_U(skyRow, UINT(1)), skyTurn);
            SpvId dust = ADD_F(s0, MUL_F(SUB_F(s1, s0), SUB_F(heightAt, skyFloor)));
            stars = ADD_V3(stars, SCALE_V3(vec3Of(s, FLOAT(.92f), FLOAT(.86f), FLOAT(1.f)), dust));

            SpvId g1 = ADD_F(FLOAT(1.f), MUL_F(doppler, SUB_F(MUL_F(invEnergy, moved), FLOAT(1.f))));
            SpvId g2 = MUL_F(g1, g1);
            SpvId shown = MUL_F(MUL_F(spvLoad(s, s->floatType, v->through), starGain), MUL_F(g2, g2));
            spvStore(s, v->sum, ADD_V3(spvLoad(s, s->vec3, v->sum), SCALE_V3(stars, shown)));
        }
        spvIfEnd(s, outMerge);

        SpvId count = ADD_U(spvLoad(s, s->uintType, v->count), UINT(1));
        spvStore(s, v->count, count);
        spvStore(s, v->u, nextU);
        spvStore(s, v->w, nextW);
        spvStore(s, v->n, nextN);
        spvStore(s, v->v, nextV);
        spvStore(s, v->psi, nextPsi);
        spvStore(s, v->drag, nextDrag);
        SpvId going = BOTH(spvOp1(s, SPV_OP_LogicalNot, s->boolType, escaped), BOTH(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, nextU, uHorizon), MORE_F(spvLoad(s, s->floatType, v->through), FLOAT(.004f))));
        spvStore(s, v->running, BOTH(going, spvOp2(s, SPV_OP_ULessThan, s->boolType, count, UINT(HOLE_STEPS))));
    }
    spvBranch(s, next);
    spvLabel(s, next);
    spvBranch(s, header);
    spvLabel(s, done);
    return spvLoad(s, s->vec3, v->sum);
}

/* A pixel an invocation, in blocks of 8 x 8: neighbours take much the same path around the hole. */
uint32_t* artfShaderHoleTrace(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    HoleResources r;
    holeDeclare(s, &r, true, false, true);
    SpvId uvec3 = spvNewId(s);
    spvEmit(&s->types, 23, SPV_WORDS(uvec3, s->uintType, 3));
    SpvId global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, uvec3), SPV_STORAGE_Input);
    spvDecorate(s, global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);

    SpvId main = spvBeginMain(s);
    SpvId ptrFloat = spvTypePointer(s, SPV_STORAGE_Function, s->floatType), ptrVec3 = spvTypePointer(s, SPV_STORAGE_Function, s->vec3), ptrUint = spvTypePointer(s, SPV_STORAGE_Function, s->uintType), ptrBool = spvTypePointer(s, SPV_STORAGE_Function, s->boolType);
    RayState v;
    v.u = spvVariable(s, ptrFloat, SPV_STORAGE_Function);
    v.w = spvVariable(s, ptrFloat, SPV_STORAGE_Function);
    v.n = spvVariable(s, ptrVec3, SPV_STORAGE_Function);
    v.v = spvVariable(s, ptrVec3, SPV_STORAGE_Function);
    v.psi = spvVariable(s, ptrFloat, SPV_STORAGE_Function);
    v.drag = spvVariable(s, ptrFloat, SPV_STORAGE_Function);
    v.through = spvVariable(s, ptrFloat, SPV_STORAGE_Function);
    v.sum = spvVariable(s, ptrVec3, SPV_STORAGE_Function);
    v.running = spvVariable(s, ptrBool, SPV_STORAGE_Function);
    v.count = spvVariable(s, ptrUint, SPV_STORAGE_Function);
    SpvId total = spvVariable(s, ptrVec3, SPV_STORAGE_Function), sample = spvVariable(s, ptrUint, SPV_STORAGE_Function);

    SpvId id = spvLoad(s, uvec3, global);
    SpvId x = spvExtract(s, s->uintType, id, 0), y = spvExtract(s, s->uintType, id, 1);
    SpvId sizes = holeVec(s, &r, H_SIZE), image = holeVec(s, &r, H_IMAGE);
    SpvId width = AS_UINT(X(sizes)), height = AS_UINT(Y(sizes)), side = AS_UINT(Z(sizes));
    SpvId merge;
    spvIfBegin(s, BOTH(spvOp2(s, SPV_OP_ULessThan, s->boolType, x, width), spvOp2(s, SPV_OP_ULessThan, s->boolType, y, height)), &merge);
    {
        spvStore(s, total, vec3Of(s, FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)));
        spvStore(s, sample, UINT(0));
        SpvId header = spvNewId(s), test = spvNewId(s), body = spvNewId(s), next = spvNewId(s), done = spvNewId(s);
        spvBranch(s, header);
        spvLabel(s, header);
        spvLoopMerge(s, done, next);
        spvBranch(s, test);
        spvLabel(s, test);
        SpvId i = spvLoad(s, s->uintType, sample);
        SpvId all = MUL_U(side, side);
        spvBranchIf(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, i, all), body, done);
        spvLabel(s, body);
        {
            SpvId sideF = TO_F(side);
            SpvId dx = DIV_F(ADD_F(TO_F(spvOp2(s, SPV_OP_UMod, s->uintType, i, side)), FLOAT(.5f)), sideF);
            SpvId dy = DIV_F(ADD_F(TO_F(spvOp2(s, SPV_OP_UDiv, s->uintType, i, side)), FLOAT(.5f)), sideF);
            SpvId nx = SUB_F(MUL_F(ADD_F(TO_F(x), dx), Y(image)), FLOAT(1.f));
            SpvId ny = SUB_F(FLOAT(1.f), MUL_F(ADD_F(TO_F(y), dy), Z(image)));
            /* its moment of the shutter's opening, as lensTile */
            SpvId moment = SUB_F(MUL_F(ADD_F(TO_F(spvOp2(s, SPV_OP_UMod, s->uintType, MUL_U(i, UINT(5)), all)), FLOAT(.5f)), W(image)), FLOAT(.5f));
            SpvId time = ADD_F(Z(holeVec(s, &r, H_DISC)), MUL_F(W(holeVec(s, &r, H_SPIN)), moment));
            SpvId light = holeTrace(s, &r, &v, nx, ny, time);
            spvStore(s, total, ADD_V3(spvLoad(s, s->vec3, total), light));
        }
        spvBranch(s, next);
        spvLabel(s, next);
        spvStore(s, sample, ADD_U(spvLoad(s, s->uintType, sample), UINT(1)));
        spvBranch(s, header);
        spvLabel(s, done);
        spvStore(s, pixelPtr(s, &r, r.hdr, ADD_U(MUL_U(y, width), x)), vec4Of(s, SCALE_V3(spvLoad(s, s->vec3, total), W(image)), FLOAT(0.f)));
    }
    spvIfEnd(s, merge);
    spvEndMain(s);

    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &global, 1);
    spvExecutionMode(s, main, 17, 8, 8, 1, 3);
    return finish(s, size);
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
static SpvId bloomTap(Spv* s, const HoleResources* r, SpvId start, SpvId width, SpvId height, SpvId sx, SpvId sy) {
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
    HoleResources r;
    holeDeclare(s, &r, false, true, false);
    BloomPass p;
    bloomDeclare(s, &p);

    SpvId main = spvBeginMain(s);
    SpvId i = spvExtract(s, s->uintType, spvLoad(s, p.uvec3, p.global), 0);
    SpvId level = spvLoad(s, s->uintType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_PushConstant, s->uintType), p.push, &r.zero, 1));
    SpvId row = holeVecAt(s, &r, ADD_U(UINT(H_BLOOM), level));
    SpvId start = AS_UINT(X(row)), width = AS_UINT(Y(row)), height = AS_UINT(Z(row));
    SpvId merge;
    spvIfBegin(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, i, MUL_U(width, height)), &merge);
    SpvId x = spvOp2(s, SPV_OP_UMod, s->uintType, i, width), y = spvOp2(s, SPV_OP_UDiv, s->uintType, i, width);
    SpvId from = fromLight ? holeVec(s, &r, H_SIZE) : holeVecAt(s, &r, ADD_U(UINT(H_BLOOM - 1), level));
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
    HoleResources r;
    holeDeclare(s, &r, false, true, false);
    BloomPass p;
    bloomDeclare(s, &p);

    SpvId main = spvBeginMain(s);
    SpvId i = spvExtract(s, s->uintType, spvLoad(s, p.uvec3, p.global), 0);
    SpvId level = spvLoad(s, s->uintType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_PushConstant, s->uintType), p.push, &r.zero, 1));
    SpvId row = holeVecAt(s, &r, ADD_U(UINT(H_BLOOM), level));
    SpvId start = AS_UINT(X(row)), width = AS_UINT(Y(row)), height = AS_UINT(Z(row));
    SpvId merge;
    spvIfBegin(s, spvOp2(s, SPV_OP_ULessThan, s->boolType, i, MUL_U(width, height)), &merge);
    SpvId x = spvOp2(s, SPV_OP_UMod, s->uintType, i, width), y = spvOp2(s, SPV_OP_UDiv, s->uintType, i, width);
    SpvId above = holeVecAt(s, &r, ADD_U(UINT(H_BLOOM + 1), level));
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
uint32_t* artfShaderEnvironmentBackdrop(size_t* size) {
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
    SpvId px = SUB_F(MUL_F(DIV_F(X(coord), W(right)), FLOAT(2.f)), FLOAT(1.f));
    SpvId py = SUB_F(FLOAT(1.f), MUL_F(DIV_F(Y(coord), W(up)), FLOAT(2.f)));
    SpvId d = ADD_V3(ADD_V3(forward, SCALE_V3(spvShuffle3(s, right), px)), SCALE_V3(spvShuffle3(s, up), py));
    SpvId light = SCALE_V3(envSample(s, &r, UINT(0), d), W(frameVec(s, &r, F_SKY)));
    light = exposed(s, light, X(frameVec(s, &r, F_LOOK)));
    SpvId shown = spvGlsl1(s, SPV_GLSL_Sqrt, s->vec3, spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, light, same3(s, 0.f)), same3(s, 1.f)));
    SpvId alpha = W(frameVec(s, &r, 14));
    spvStore(s, outColor, vec4Of(s, SCALE_V3(shown, alpha), alpha));
    spvEndMain(s);

    const SpvId interface[2] = {fragCoord, outColor};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, interface, 2);
    spvExecutionMode(s, main, 7, 0, 0, 0, 0);
    return finish(s, size);
}

/* The frame's background: the light and its glow, tone mapped, under whatever the scene draws. */
uint32_t* artfShaderHoleBackdrop(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    HoleResources r;
    holeDeclare(s, &r, false, false, false);
    SpvId fragCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
    spvDecorate(s, fragCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);

    SpvId main = spvBeginMain(s);
    SpvId coord = spvLoad(s, s->vec4, fragCoord);
    SpvId fx = X(coord), fy = Y(coord); /* (pixel centers: x + .5) */
    SpvId x = TO_U(FLOOR_F(fx)), y = TO_U(FLOOR_F(fy));
    SpvId sizes = holeVec(s, &r, H_SIZE), look = holeVec(s, &r, H_LOOK), image = holeVec(s, &r, H_IMAGE), level = holeVec(s, &r, H_BLOOM);
    SpvId light = spvShuffle3(s, spvLoad(s, s->vec4, pixelPtr(s, &r, r.hdr, ADD_U(MUL_U(y, AS_UINT(X(sizes))), x))));
    SpvId sx = SUB_F(MUL_F(fx, FLOAT(.5f)), FLOAT(.5f)), sy = SUB_F(MUL_F(fy, FLOAT(.5f)), FLOAT(.5f));
    SpvId glow = bloomTap(s, &r, AS_UINT(X(level)), AS_UINT(Y(level)), AS_UINT(Z(level)), sx, sy);
    /* (as lensFill: a lens's glow is nearly white) */
    SpvId grey = ADD_F(ADD_F(MUL_F(X(glow), FLOAT(.3f)), MUL_F(Y(glow), FLOAT(.59f))), MUL_F(Z(glow), FLOAT(.11f)));
    glow = ADD_V3(glow, SCALE_V3(SUB_V3(vec3Of(s, grey, grey, grey), glow), FLOAT(ARTF_BLOOM_WHITE)));
    SpvId all = SCALE_V3(ADD_V3(light, SCALE_V3(glow, MUL_F(W(look), FLOAT(1.f - ARTF_BLOOM_FADE)))), X(image));

    /* as lensToneMap */
    SpvId vec3Const[5];
    const float numbers[5] = {2.51f, .03f, 2.43f, .59f, .14f};
    for (int i = 0; i < 5; ++i) vec3Const[i] = vec3Of(s, FLOAT(numbers[i]), FLOAT(numbers[i]), FLOAT(numbers[i]));
    SpvId top = MUL_V3(all, ADD_V3(MUL_V3(all, vec3Const[0]), vec3Const[1]));
    SpvId bottom = ADD_V3(MUL_V3(all, ADD_V3(MUL_V3(all, vec3Const[2]), vec3Const[3])), vec3Const[4]);
    SpvId mapped = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvOp2(s, SPV_OP_FDiv, s->vec3, top, bottom), vec3Of(s, FLOAT(1.f), FLOAT(1.f), FLOAT(1.f)));
    SpvId shown = spvGlsl1(s, SPV_GLSL_Sqrt, s->vec3, mapped);

    SpvId noise = holeHash(s, ADD_U(x, MUL_U(y, UINT(8192))), UINT(7), UINT(13));
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
    SpvId state, origin, direction, path, light, covered, bounces, step, sampleIndex;
    SpvId shadowOrigin, through, limit, shadowStep, lightIndex, ballStep, ball, travelled, keep;
    GraphVars own;
    const ArtfGraphData* graphs;
    uint32_t graphCount;
    bool guides;
    SpvId guide, albedoGuide, shiftGuide;
    SpvId guideBits, guidePlace, guideColor, guideInstance, guideShift; /* what a pixel's first surface was */
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

/* As closest: leaves what it found in bestT and the others. */
static void closest(const Tracer* t, SpvId o, SpvId d, SpvId limit, bool bothSides) {
    Spv* s = t->s;
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
        hitMesh(t, i, AS_UINT(X(meta)), bothSides ? spvConstBool(s, true) : NOT_EQ(AS_UINT(W(meta)), UINT(0)), oo, od);
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

typedef struct Met { SpvId normal, albedo, alpha, emissive, front, ior, shiny, rough, open, mirror, f0; Reflecting reflecting; } Met;

static SpvId maxOf3(Spv* s, SpvId v) { return MAX_F(X(v), MAX_F(Y(v), Z(v))); }

/* A texture of the array, in its largest mipmap. */
static SpvId traceTexture(const Tracer* t, SpvId index, SpvId uv) {
    Spv* s = t->s;
    spvDecorate(s, index, SPV_DECORATE_NonUniform, -1);
    SpvId slot = spvAccess(s, spvTypePointer(s, SPV_STORAGE_UniformConstant, t->sampledType), t->textures, &index, 1);
    spvDecorate(s, slot, SPV_DECORATE_NonUniform, -1);
    SpvId image = spvLoad(s, t->sampledType, slot);
    spvDecorate(s, image, SPV_DECORATE_NonUniform, -1);
    return spvImageSampleLod(s, image, uv, FLOAT(0.f));
}

/* As surfaceAt, for what closest left. */
static Met surfaceAt(const Tracer* t, SpvId origin, SpvId d) {
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
        SpvId n = SCALE_V3(spvShuffle3(s, b), weight[i]), color = SCALE_V3(MUL_V3(rgb, rgb), weight[i]);
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
    SpvId texel = traceTexture(t, AS_UINT(X(m2)), uv), bumps = traceTexture(t, AS_UINT(X(m4)), uv), surface = traceTexture(t, AS_UINT(Y(m4)), uv), occlusion = traceTexture(t, AS_UINT(Z(m4)), uv);
    SpvId rgbTexel = spvShuffle3(s, texel);

    /* where u and v go on the triangle, in the object: as in trace.c */
    SpvId du1 = SUB_F(cornerU[1], cornerU[0]), dv1 = SUB_F(cornerV[1], cornerV[0]), du2 = SUB_F(cornerU[2], cornerU[0]), dv2 = SUB_F(cornerV[2], cornerV[0]);
    SpvId e1 = SUB_V3(corner[1], corner[0]), e2 = SUB_V3(corner[2], corner[0]);
    SpvId det = SUB_F(MUL_F(du1, dv2), MUL_F(du2, dv1));
    SpvId orient = PICK_F(LESS_F(det, FLOAT(0.f)), FLOAT(-1.f), FLOAT(1.f));
    SpvId tangent = SCALE_V3(NORMALIZE(SUB_V3(SCALE_V3(e1, dv2), SCALE_V3(e2, dv1))), orient), bitangent = SCALE_V3(NORMALIZE(SUB_V3(SCALE_V3(e2, du1), SCALE_V3(e1, du2))), orient);
    SpvId bx = MUL_F(SUB_F(MUL_F(X(bumps), FLOAT(2.f)), FLOAT(1.f)), Z(m3)), by = MUL_F(SUB_F(FLOAT(1.f), MUL_F(Y(bumps), FLOAT(2.f))), Z(m3)), bz = SUB_F(MUL_F(Z(bumps), FLOAT(2.f)), FLOAT(1.f));
    SpvId bent = ADD_V3(ADD_V3(SCALE_V3(tangent, bx), SCALE_V3(bitangent, by)), SCALE_V3(on, bz));
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
    SpvId albedo = MUL_V3(MUL_V3(spvShuffle3(s, m0), MUL_V3(rgbTexel, rgbTexel)), tint);
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
    SpvId notMetal = SUB_F(FLOAT(1.f), metal), a = MUL_F(rough, rough), plusOne = ADD_F(rough, FLOAT(1.f));
    SpvId ndv = MAX_F(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(normal, d)), FLOAT(1e-4f));
    SpvId f0 = ADD_V3(SCALE_V3(same3(s, .04f), MUL_F(specular, notMetal)), SCALE_V3(albedo, metal));
    SpvId shiny = EITHER(MORE_F(ADD_F(specular, metal), FLOAT(0.f)), NOT_EQ(AS_UINT(W(m4)), UINT(0)));
    SpvId smooth = SUB_F(FLOAT(1.f), rough);
    SpvId mirror = ADD_V3(f0, SCALE_V3(SUB_V3(spvGlsl2(s, SPV_GLSL_FMax, s->vec3, vec3Of(s, smooth, smooth, smooth), f0), f0), fifth(s, ndv)));
    Met m = {normal, SCALE_V3(albedo, notMetal), PICK_F(hole, FLOAT(0.f), shown), emission, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, DOT3(n, d), FLOAT(0.f)), X(m3),
             shiny, rough, ADD_F(FLOAT(1.f), MUL_F(SUB_F(X(occlusion), FLOAT(1.f)), W(m3))), pick3(s, shiny, mirror, same3(s, 0.f)), f0,
             {MUL_F(a, a), MUL_F(MUL_F(plusOne, plusOne), FLOAT(.125f)), ndv, specular, f0}};
    return m;
}

static SpvId traceHash(Spv* s, SpvId x) {
    x = MUL_U(XOR_U(x, SHR_U(x, 16)), UINT(0x7FEB352Du));
    x = MUL_U(XOR_U(x, SHR_U(x, 15)), UINT(0x846CA68Bu));
    return XOR_U(x, SHR_U(x, 16));
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
    Met m = surfaceAt(t, o, to);
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

uint32_t* artfShaderTrace(size_t* size, uint32_t maxTextures, const ArtfGraphData* graphs, uint32_t graphCount, bool guides) {
    Spv b, *s = &b;
    spvInit(s);
    spvCapability(s, 5301); /* ShaderNonUniform */
    spvCapability(s, 5307); /* SampledImageArrayNonUniformIndexing */
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
    t->shadowOrigin = VAR(ptrVec3); t->ball = VAR(ptrVec3);
    t->through = VAR(ptrFloat); t->limit = VAR(ptrFloat); t->travelled = VAR(ptrFloat); t->keep = VAR(ptrFloat);
    t->shadowStep = VAR(ptrUint); t->lightIndex = VAR(ptrUint); t->ballStep = VAR(ptrUint);
    t->graphs = graphs;
    t->graphCount = graphCount;
    t->guideBits = VAR(ptrUint); t->guidePlace = VAR(ptrVec3); t->guideColor = VAR(ptrVec3); t->guideInstance = VAR(ptrUint); t->guideShift = VAR(ptrVec3);
    t->own.color = VAR(ptrVec3); t->own.emissive = VAR(ptrVec3); t->own.normal = VAR(ptrVec3);
    t->own.rough = VAR(ptrFloat); t->own.metal = VAR(ptrFloat); t->own.alpha = VAR(ptrFloat);

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
        SpvId jx = traceRandom(t), jy = traceRandom(t);
        SpvId px = SUB_F(MUL_F(DIV_F(ADD_F(TO_F(x), jx), TO_F(width)), FLOAT(2.f)), FLOAT(1.f));
        SpvId py = SUB_F(FLOAT(1.f), MUL_F(DIV_F(ADD_F(TO_F(y), jy), TO_F(height)), FLOAT(2.f)));
        spvStore(s, t->origin, eye);
        spvStore(s, t->direction, NORMALIZE(ADD_V3(ADD_V3(forward, SCALE_V3(right, px)), SCALE_V3(up, py))));
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
            SpvId env = ADD_V3(ground, SCALE_V3(SUB_V3(sky, ground), share));
            spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), MUL_V3(LOAD_V3(t->path), env)));
        }
        spvIfEnd(s, merge);
        spvIfBegin(s, BOTH(BOTH(missed, NOT_EQ(LOAD_U(t->covered), UINT(0))), aroundOn), &aroundMerge);
        {
            SpvId env = SCALE_V3(envSample(s, &t->r, UINT(0), d), environmentLight);
            spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), MUL_V3(LOAD_V3(t->path), env)));
        }
        spvIfEnd(s, aroundMerge);
        breakIf(s, &steps, missed);

        Met m = surfaceAt(t, o, d);
        SpvId p = ADD_V3(o, SCALE_V3(d, LOAD_F(t->bestT)));
        SpvId reach = reachOf(s, p);
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
                SpvId air = MUL_F(travelled, Y(look));
                spvStore(s, t->keep, DIV_F(FLOAT(1.f), ADD_F(ADD_F(FLOAT(1.f), air), MUL_F(MUL_F(air, air), FLOAT(.5f)))));
            }
            spvIfEnd(s, first);
            spvStore(s, t->covered, UINT(1));
            SpvId view = spvOp1(s, SPV_OP_FNegate, s->vec3, d);
            SpvId lifted = ADD_V3(p, SCALE_V3(m.normal, reach));
            spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), MUL_V3(LOAD_V3(t->path), m.emissive)));

            /* a random direction in a ball */
            spvStore(s, t->ballStep, UINT(0));
            Loop ball;
            loopOpen(s, &ball);
            loopWhile(s, &ball, LESS_U(LOAD_U(t->ballStep), UINT(16)));
            {
                SpvId bx = SUB_F(MUL_F(traceRandom(t), FLOAT(2.f)), FLOAT(1.f)), by = SUB_F(MUL_F(traceRandom(t), FLOAT(2.f)), FLOAT(1.f)), bz = SUB_F(MUL_F(traceRandom(t), FLOAT(2.f)), FLOAT(1.f));
                SpvId v = vec3Of(s, bx, by, bz);
                spvStore(s, t->ball, v);
                breakIf(s, &ball, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, DOT3(v, v), FLOAT(1.f)));
                spvStore(s, t->ballStep, ADD_U(LOAD_U(t->ballStep), UINT(1)));
            }
            loopClose(s, &ball);
            SpvId ballDir = LOAD_V3(t->ball);
            SpvId toSun = NORMALIZE(ADD_V3(spvShuffle3(s, frameVec(s, &t->r, F_TOSUN)), SCALE_V3(ballDir, Z(look))));

            /* the lights, and last the sun */
            spvStore(s, t->lightIndex, UINT(0));
            Loop lights;
            loopOpen(s, &lights);
            loopWhile(s, &lights, spvOp2(s, 178 /* OpULessThanEqual */, s->boolType, LOAD_U(t->lightIndex), lightCount));
            {
                SpvId li = LOAD_U(t->lightIndex);
                SpvId isSun = IS_EQ(li, lightCount);
                SpvId row = ADD_U(UINT(F_LIGHTS), MUL_U(spvGlsl2(s, SPV_GLSL_UMin, s->uintType, li, UINT(ARTF_MAX_LIGHTS - 1)), UINT(F_LIGHT_ROWS)));
                SpvId lightA = frameVecAt(s, &t->r, row), lightB = frameVecAt(s, &t->r, ADD_U(row, UINT(1))), lightC = frameVecAt(s, &t->r, ADD_U(row, UINT(2)));
                SpvId toPoint = SUB_V3(spvShuffle3(s, lightA), lifted);
                SpvId d2 = DOT3(toPoint, toPoint), distance = SQRT_F(MAX_F(d2, FLOAT(1e-8f)));
                SpvId radius = W(lightA);
                SpvId fade = MAX_F(SUB_F(FLOAT(1.f), DIV_F(d2, MUL_F(radius, radius))), FLOAT(0.f));
                SpvId toLamp = SCALE_V3(toPoint, DIV_F(FLOAT(1.f), distance));
                SpvId to = pick3(s, isSun, toSun, toLamp);
                SpvId spot = MUL_F(SUB_F(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(toLamp, spvShuffle3(s, lightC))), W(lightC)), W(lightB));
                SpvId strength = PICK_F(isSun, FLOAT(1.f), MUL_F(MUL_F(fade, fade), MIN_F(MAX_F(spot, FLOAT(0.f)), FLOAT(1.f))));
                SpvId color = pick3(s, isSun, spvShuffle3(s, frameVec(s, &t->r, F_SUNCOLOR)), spvShuffle3(s, lightB));
                SpvId limit = PICK_F(isSun, FLOAT(3e37f), distance);
                SpvId ndl = DOT3(m.normal, to);
                SpvId lit;
                spvIfBegin(s, BOTH(BOTH(MORE_F(ndl, FLOAT(0.f)), MORE_F(strength, FLOAT(0.f))), MORE_F(DOT3(color, color), FLOAT(0.f))), &lit);
                {
                    lightThrough(t, lifted, to, limit);
                    SpvId highlight = lightHighlight(s, &m.reflecting, m.normal, view, to, ndl, FLOAT(1.f));
                    SpvId amount = MUL_F(strength, LOAD_F(t->through));
                    SpvId reflected = ADD_V3(SCALE_V3(m.albedo, ndl), highlight);
                    spvStore(s, t->light, ADD_V3(LOAD_V3(t->light), SCALE_V3(MUL_V3(MUL_V3(LOAD_V3(t->path), color), reflected), amount)));
                }
                spvIfEnd(s, lit);
                spvStore(s, t->lightIndex, ADD_U(li, UINT(1)));
            }
            loopClose(s, &lights);

            SpvId mirror = SUB_V3(d, SCALE_V3(m.normal, MUL_F(FLOAT(2.f), DOT3(d, m.normal))));
            SpvId glossy = ADD_V3(mirror, SCALE_V3(ballDir, MUL_F(m.rough, m.rough)));
            SpvId last = spvOp2(s, SPV_OP_ULessThan, s->boolType, LOAD_U(t->bounces), maxBounces);
            last = NOT(last);

            /* rays: light from all around, and a mirror for what reflects */
            SpvId share = ADD_F(MUL_F(Y(m.normal), FLOAT(.5f)), FLOAT(.5f));
            SpvId ambient = ADD_V3(ground, SCALE_V3(SUB_V3(sky, ground), share));
            SpvId reflectedUp = ADD_F(MUL_F(Y(mirror), FLOAT(.5f)), FLOAT(.5f));
            SpvId reflectedSky = ADD_V3(ground, SCALE_V3(SUB_V3(sky, ground), reflectedUp));
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
            SpvId most = maxOf3(s, m.mirror);
            SpvId chance = PICK_F(m.shiny, MIN_F(MAX_F(most, FLOAT(.05f)), FLOAT(.95f)), FLOAT(0.f));
            SpvId isMirror = EITHER(NOT(paths), LESS_F(traceRandom(t), chance));
            SpvId diffuse = ADD_V3(SCALE_V3(m.normal, FLOAT(1.0001f)), ballDir);
            SpvId next = NORMALIZE(pick3(s, isMirror, glossy, diffuse));
            SpvId keep = DIV_F(MUL_F(m.open, SUB_F(FLOAT(1.f), most)), SUB_F(FLOAT(1.f), chance));
            SpvId tint = pick3(s, isMirror, pick3(s, paths, SCALE_V3(m.mirror, DIV_F(FLOAT(1.f), MAX_F(chance, FLOAT(1e-6f)))), m.mirror), SCALE_V3(m.albedo, keep));
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

uint32_t* artfShaderTraceShow(size_t* size) {
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
    SpvId light = exposed(s, SCALE_V3(spvShuffle3(s, gathered), DIV_F(FLOAT(1.f), MAX_F(covered, FLOAT(1e-6f)))), X(frameVec(s, &r, F_LOOK)));
    SpvId lo = vec3Of(s, FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)), hi = vec3Of(s, FLOAT(1.f), FLOAT(1.f), FLOAT(1.f));
    SpvId clamped = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, light, lo), hi);
    spvStore(s, outColor, vec4Of(s, SCALE_V3(spvGlsl1(s, SPV_GLSL_Sqrt, s->vec3, clamped), alpha), alpha));
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
    SpvId side = spvOp1(s, SPV_OP_ConvertUToF, s->floatType, AS_UINT(Z(shadowRow(s, &shadows, UINT(S_SUN)))));
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
