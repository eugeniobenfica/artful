/* artful-2d — the GPU's shaders, built at start with the library's SPIR-V writer (src/vulkan/spirv.c): the same
 * arithmetic as the CPU's (draw2d.c, shadow2d.c, post2d.c), a pixel an invocation. A polygon is the CPU's four rows a
 * pixel, each exact across it while no more than two of its edges cross within the pixel on that row. */
#include "2d/vulkan2d.h"
#include "vulkan/spirv.h"

enum {
    OP_IEqual = 170, OP_INotEqual = 171, OP_UGreaterThanEqual = 174, OP_SGreaterThanEqual = 175, OP_SLessThan = 177, OP_LogicalOr = 166,
    OP_ConvertSToF = 111, OP_BitwiseOr = 197, OP_SDiv = 135, OP_FOrdEqual = 180
};
enum { GLSL_FClamp = 43, GLSL_SMax = 42 };

typedef struct Sh {
    Spv* s;
    SpvId zero;
    SpvId uintArray, vec4Array, floatArray;
    SpvId ptrVec4, ptrUint, ptrFloat;
    SpvId fnFloat, fnUint, fnInt, fnVec4;
    SpvId buffers[B2_COUNT];
    SpvId push, uvec3, global;
} Sh;

#define S (h->s)
#define UINT(v) spvConstUint(S, v)
#define INT(v) spvConstInt(S, v)
#define FLOAT(v) spvConstFloat(S, v)
#define ADD_U(a, b) spvOp2(S, SPV_OP_IAdd, S->uintType, a, b)
#define SUB_U(a, b) spvOp2(S, SPV_OP_ISub, S->uintType, a, b)
#define MUL_U(a, b) spvOp2(S, SPV_OP_IMul, S->uintType, a, b)
#define DIV_U(a, b) spvOp2(S, SPV_OP_UDiv, S->uintType, a, b)
#define AND_U(a, b) spvOp2(S, SPV_OP_BitwiseAnd, S->uintType, a, b)
#define OR_U(a, b) spvOp2(S, OP_BitwiseOr, S->uintType, a, b)
#define SHR_U(a, n) spvOp2(S, SPV_OP_ShiftRightLogical, S->uintType, a, UINT(n))
#define SHL_U(a, n) spvOp2(S, SPV_OP_ShiftLeftLogical, S->uintType, a, UINT(n))
#define ADD_I(a, b) spvOp2(S, SPV_OP_IAdd, S->intType, a, b)
#define SUB_I(a, b) spvOp2(S, SPV_OP_ISub, S->intType, a, b)
#define AND_I(a, b) spvOp2(S, SPV_OP_BitwiseAnd, S->intType, a, b)
#define ADD_F(a, b) spvOp2(S, SPV_OP_FAdd, S->floatType, a, b)
#define SUB_F(a, b) spvOp2(S, SPV_OP_FSub, S->floatType, a, b)
#define MUL_F(a, b) spvOp2(S, SPV_OP_FMul, S->floatType, a, b)
#define DIV_F(a, b) spvOp2(S, SPV_OP_FDiv, S->floatType, a, b)
#define NEG_F(a) spvOp1(S, SPV_OP_FNegate, S->floatType, a)
#define MAX_F(a, b) spvGlsl2(S, SPV_GLSL_FMax, S->floatType, a, b)
#define MIN_F(a, b) spvGlsl2(S, SPV_GLSL_FMin, S->floatType, a, b)
#define CLAMP_F(a, lo, hi) MIN_F(MAX_F(a, lo), hi)
#define ABS_F(a) spvGlsl1(S, SPV_GLSL_FAbs, S->floatType, a)
#define SQRT_F(a) spvGlsl1(S, SPV_GLSL_Sqrt, S->floatType, a)
#define FLOOR_F(a) spvGlsl1(S, SPV_GLSL_Floor, S->floatType, a)
#define LESS_F(a, b) spvOp2(S, SPV_OP_FOrdLessThan, S->boolType, a, b)
#define MORE_F(a, b) spvOp2(S, SPV_OP_FOrdGreaterThan, S->boolType, a, b)
#define LESS_EQ_F(a, b) spvOp2(S, SPV_OP_FOrdLessThanEqual, S->boolType, a, b)
#define MORE_EQ_F(a, b) spvOp2(S, SPV_OP_FOrdGreaterThanEqual, S->boolType, a, b)
#define LESS_U(a, b) spvOp2(S, SPV_OP_ULessThan, S->boolType, a, b)
#define MORE_EQ_U(a, b) spvOp2(S, OP_UGreaterThanEqual, S->boolType, a, b)
#define EQ_U(a, b) spvOp2(S, OP_IEqual, S->boolType, a, b)
#define NE_U(a, b) spvOp2(S, OP_INotEqual, S->boolType, a, b)
#define NE_I(a, b) spvOp2(S, OP_INotEqual, S->boolType, a, b)
#define LESS_I(a, b) spvOp2(S, OP_SLessThan, S->boolType, a, b)
#define MORE_EQ_I(a, b) spvOp2(S, OP_SGreaterThanEqual, S->boolType, a, b)
#define AND_B(a, b) spvOp2(S, SPV_OP_LogicalAnd, S->boolType, a, b)
#define OR_B(a, b) spvOp2(S, OP_LogicalOr, S->boolType, a, b)
#define NOT_B(a) spvOp1(S, SPV_OP_LogicalNot, S->boolType, a)
#define PICK_F(c, a, b) spvSelect(S, S->floatType, c, a, b)
#define PICK_U(c, a, b) spvSelect(S, S->uintType, c, a, b)
#define PICK_I(c, a, b) spvSelect(S, S->intType, c, a, b)
#define TO_F(u) spvOp1(S, SPV_OP_ConvertUToF, S->floatType, u)
#define I_TO_F(i) spvOp1(S, OP_ConvertSToF, S->floatType, i)
#define TO_U(f) spvOp1(S, SPV_OP_ConvertFToU, S->uintType, f)
#define TO_I(f) spvOp1(S, SPV_OP_ConvertFToS, S->intType, f)
#define I_AS_U(i) spvOp1(S, SPV_OP_Bitcast, S->uintType, i)
#define AS_U(f) spvOp1(S, SPV_OP_Bitcast, S->uintType, f)
#define AS_F(u) spvOp1(S, SPV_OP_Bitcast, S->floatType, u)
#define X(v) spvExtract(S, S->floatType, v, 0)
#define Y(v) spvExtract(S, S->floatType, v, 1)
#define Z(v) spvExtract(S, S->floatType, v, 2)
#define W(v) spvExtract(S, S->floatType, v, 3)
#define LOAD_F(p) spvLoad(S, S->floatType, p)
#define LOAD_U(p) spvLoad(S, S->uintType, p)
#define LOAD_I(p) spvLoad(S, S->intType, p)
#define LOAD_V4(p) spvLoad(S, S->vec4, p)

static SpvId blockOf(Spv* s, SpvId array) {
    SpvId block = spvTypeStruct(s, &array, 1);
    spvDecorate(s, block, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, block, 0, SPV_DECORATE_Offset, 0);
    return block;
}

/* The types, the buffers asked for, eight uints of push constants, and the invocation's place. */
static void begin(Sh* h, Spv* s, const int* bindings, int count) {
    memset(h, 0, sizeof *h);
    h->s = s;
    spvInit(s);
    h->zero = spvConstInt(s, 0);
    h->uintArray = spvTypeArray(s, s->uintType, 0, 4);
    h->vec4Array = spvTypeArray(s, s->vec4, 0, 16);
    h->floatArray = spvTypeArray(s, s->floatType, 0, 4);
    h->ptrVec4 = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->vec4);
    h->ptrUint = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->uintType);
    h->ptrFloat = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->floatType);
    h->fnFloat = spvTypePointer(s, SPV_STORAGE_Function, s->floatType);
    h->fnUint = spvTypePointer(s, SPV_STORAGE_Function, s->uintType);
    h->fnInt = spvTypePointer(s, SPV_STORAGE_Function, s->intType);
    h->fnVec4 = spvTypePointer(s, SPV_STORAGE_Function, s->vec4);
    for (int i = 0; i < count; ++i) {
        const int b = bindings[i];
        const bool floats = b == B2_GI || b == B2_DIST || b == B2_SCRATCH || b == B2_GI_TMP;
        const bool uints = b == B2_TILES || b == B2_SHADOWS || b == B2_TEXELS || b == B2_LEVELS || b == B2_PIXELS || b == B2_EMITTERS || b == B2_SOLID;
        const SpvId array = floats ? h->floatArray : uints ? h->uintArray : h->vec4Array;
        h->buffers[b] = spvResource(s, blockOf(s, array), SPV_STORAGE_StorageBuffer, (uint32_t)b);
    }
    SpvId members[8];
    for (int i = 0; i < 8; ++i) members[i] = s->uintType;
    SpvId pushBlock = spvTypeStruct(s, members, 8);
    spvDecorate(s, pushBlock, SPV_DECORATE_Block, -1);
    for (uint32_t i = 0; i < 8; ++i) spvMemberDecorate(s, pushBlock, i, SPV_DECORATE_Offset, (int)(i * 4));
    h->push = spvVariable(s, spvTypePointer(s, SPV_STORAGE_PushConstant, pushBlock), SPV_STORAGE_PushConstant);
    h->uvec3 = spvTypeVector(s, s->uintType, 3);
    h->global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, h->uvec3), SPV_STORAGE_Input);
    spvDecorate(s, h->global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);
}

static uint32_t* endSized(Sh* h, SpvId main, size_t* size, uint32_t x, uint32_t y) {
    Spv* s = h->s;
    spvEndMain(s);
    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &h->global, 1);
    spvExecutionMode(s, main, 17 /* LocalSize */, x, y, 1, 3);
    size_t words;
    uint32_t* code = spvFinish(s, &words);
    spvFini(s);
    *size = words * 4;
    return code;
}

static uint32_t* end(Sh* h, SpvId main, size_t* size) { return endSized(h, main, size, 8, 8); }

static SpvId vecAt(Sh* h, int b, SpvId i) {
    const SpvId idx[2] = {h->zero, i};
    return spvLoad(S, S->vec4, spvAccess(S, h->ptrVec4, h->buffers[b], idx, 2));
}
static SpvId vecPtr(Sh* h, int b, SpvId i) {
    const SpvId idx[2] = {h->zero, i};
    return spvAccess(S, h->ptrVec4, h->buffers[b], idx, 2);
}
static SpvId uintAt(Sh* h, int b, SpvId i) {
    const SpvId idx[2] = {h->zero, i};
    return spvLoad(S, S->uintType, spvAccess(S, h->ptrUint, h->buffers[b], idx, 2));
}
static SpvId uintPtr(Sh* h, int b, SpvId i) {
    const SpvId idx[2] = {h->zero, i};
    return spvAccess(S, h->ptrUint, h->buffers[b], idx, 2);
}
static SpvId floatAt(Sh* h, int b, SpvId i) {
    const SpvId idx[2] = {h->zero, i};
    return spvLoad(S, S->floatType, spvAccess(S, h->ptrFloat, h->buffers[b], idx, 2));
}
static SpvId pushAt(Sh* h, uint32_t i) {
    const SpvId idx[1] = {UINT(i)};
    return spvLoad(S, S->uintType, spvAccess(S, spvTypePointer(S, SPV_STORAGE_PushConstant, S->uintType), h->push, idx, 1));
}
static SpvId vec4Of(Sh* h, SpvId x, SpvId y, SpvId z, SpvId w) {
    const SpvId p[4] = {x, y, z, w};
    return spvCompose(S, S->vec4, p, 4);
}
static SpvId fnVar(Sh* h, SpvId pointerType) { return spvVariable(S, pointerType, SPV_STORAGE_Function); }

/* ---- loops: for (i = 0; i < count; ++i), i a uint variable of the function ---- */

typedef struct Loop2D { SpvId header, test, body, next, done, counter; } Loop2D;

static SpvId loopBegin(Sh* h, Loop2D* l, SpvId counter, SpvId count) {
    l->header = spvNewId(S), l->test = spvNewId(S), l->body = spvNewId(S), l->next = spvNewId(S), l->done = spvNewId(S);
    l->counter = counter;
    spvStore(S, counter, UINT(0));
    spvBranch(S, l->header);
    spvLabel(S, l->header);
    spvLoopMerge(S, l->done, l->next);
    spvBranch(S, l->test);
    spvLabel(S, l->test);
    SpvId i = LOAD_U(counter);
    spvBranchIf(S, LESS_U(i, count), l->body, l->done);
    spvLabel(S, l->body);
    return i;
}

static void loopEnd(Sh* h, Loop2D* l) {
    spvBranch(S, l->next);
    spvLabel(S, l->next);
    spvStore(S, l->counter, ADD_U(LOAD_U(l->counter), UINT(1)));
    spvBranch(S, l->header);
    spvLabel(S, l->done);
}

/* ---- arithmetic as the CPU's ---- */

/* artf2DAtan2 */
static SpvId atan2Of(Sh* h, SpvId y, SpvId x) {
    SpvId ax = ABS_F(x), ay = ABS_F(y);
    SpvId mx = MAX_F(ax, ay), mn = MIN_F(ax, ay);
    SpvId a = DIV_F(mn, MAX_F(mx, FLOAT(1e-30f))), q = MUL_F(a, a);
    SpvId r = ADD_F(MUL_F(FLOAT(-.0117212f), q), FLOAT(.05265332f));
    r = SUB_F(MUL_F(r, q), FLOAT(.11643287f));
    r = ADD_F(MUL_F(r, q), FLOAT(.19354346f));
    r = SUB_F(MUL_F(r, q), FLOAT(.33262347f));
    r = MUL_F(ADD_F(MUL_F(r, q), FLOAT(.99997726f)), a);
    r = PICK_F(MORE_F(ay, ax), SUB_F(FLOAT(1.57079633f), r), r);
    r = PICK_F(LESS_F(x, FLOAT(0.f)), SUB_F(FLOAT(3.14159265f), r), r);
    r = PICK_F(LESS_F(y, FLOAT(0.f)), NEG_F(r), r);
    return PICK_F(spvOp2(S, OP_FOrdEqual, S->boolType, mx, FLOAT(0.f)), FLOAT(0.f), r);
}

/* artf2DTone: premultiplied rgba to a pixel's bytes */
/* swap: red where blue goes, for an RGBA window */
static SpvId tone(Sh* h, SpvId rgba, SpvId exposure, SpvId curve, SpvId swap) {
    SpvId c[3] = {X(rgba), Y(rgba), Z(rgba)};
    SpvId most = MAX_F(c[0], MAX_F(c[1], c[2]));
    SpvId alpha = CLAMP_F(MAX_F(W(rgba), MIN_F(MUL_F(most, exposure), FLOAT(1.f))), FLOAT(0.f), FLOAT(1.f));
    SpvId k = DIV_F(exposure, MAX_F(alpha, FLOAT(1e-30f)));
    SpvId out = SHL_U(TO_U(ADD_F(MUL_F(alpha, FLOAT(255.f)), FLOAT(.5f))), 24);
    for (int i = 0; i < 3; ++i) {
        SpvId x = MUL_F(c[i], k);
        x = PICK_F(MORE_F(x, FLOAT(0.f)), MIN_F(x, FLOAT(65504.f)), FLOAT(0.f));
        SpvId aces = DIV_F(MUL_F(x, ADD_F(MUL_F(x, FLOAT(2.51f)), FLOAT(.03f))), ADD_F(MUL_F(x, ADD_F(MUL_F(x, FLOAT(2.43f)), FLOAT(.59f))), FLOAT(.14f)));
        SpvId reinhard = DIV_F(x, ADD_F(FLOAT(1.f), x));
        x = PICK_F(EQ_U(curve, UINT(ARTF_TONE2D_ACES)), aces, PICK_F(EQ_U(curve, UINT(ARTF_TONE2D_REINHARD)), reinhard, x));
        SpvId v = spvSrgbEncode(S, CLAMP_F(x, FLOAT(0.f), FLOAT(1.f)));
        SpvId byte = TO_U(ADD_F(MUL_F(MUL_F(v, alpha), FLOAT(255.f)), FLOAT(.5f)));
        SpvId shift = i == 1 ? UINT(8) : PICK_U(swap, UINT((uint32_t)(8 * i)), UINT((uint32_t)(16 - 8 * i)));
        out = OR_U(out, spvOp2(S, SPV_OP_ShiftLeftLogical, S->uintType, byte, shift));
    }
    return PICK_U(MORE_F(alpha, FLOAT(0.f)), out, UINT(0));
}

/* ---- textures ---- */

/* 0..n-1 of i (a float, from -1 to n), as a uint */
static SpvId wrapOf(Sh* h, SpvId i, SpvId n) {
    SpvId w = PICK_F(LESS_F(i, FLOAT(0.f)), ADD_F(i, n), PICK_F(MORE_EQ_F(i, n), SUB_F(i, n), i));
    return TO_U(CLAMP_F(w, FLOAT(0.f), SUB_F(n, FLOAT(1.f))));
}

/* A texel's light, premultiplied (a normal map's: -1..1), and its alpha. */
static SpvId texelOf(Sh* h, SpvId first, SpvId width, SpvId normal, SpvId x, SpvId y) {
    SpvId p = uintAt(h, B2_TEXELS, ADD_U(first, ADD_U(MUL_U(y, width), x)));
    SpvId a = MUL_F(TO_F(SHR_U(p, 24)), FLOAT(1.f / 255.f));
    SpvId c[3];
    for (int i = 0; i < 3; ++i) {
        SpvId byte = TO_F(AND_U(SHR_U(p, (uint32_t)(16 - 8 * i)), UINT(255)));
        SpvId v = DIV_F(byte, FLOAT(255.f));
        SpvId light = MUL_F(spvSrgbDecode(S, v), a);
        SpvId n = SUB_F(MUL_F(byte, FLOAT(2.f / 255.f)), FLOAT(1.f));
        c[i] = PICK_F(normal, n, light);
    }
    return vec4Of(h, c[0], c[1], c[2], a);
}

/* As sampleTexture: the level's texels, repeating, nearest or between four. */
static SpvId sampleOf(Sh* h, SpvId level, SpvId nearest, SpvId u, SpvId v) {
    SpvId at = MUL_U(level, UINT(4));
    SpvId first = uintAt(h, B2_LEVELS, at), w = uintAt(h, B2_LEVELS, ADD_U(at, UINT(1))), hh = uintAt(h, B2_LEVELS, ADD_U(at, UINT(2)));
    SpvId normal = NE_U(uintAt(h, B2_LEVELS, ADD_U(at, UINT(3))), UINT(0));
    SpvId wf = TO_F(w), hf = TO_F(hh);
    u = SUB_F(u, FLOOR_F(u));
    v = SUB_F(v, FLOOR_F(v));
    SpvId fx = MUL_F(u, wf), fy = MUL_F(v, hf);
    fx = PICK_F(nearest, fx, SUB_F(fx, FLOAT(.5f)));
    fy = PICK_F(nearest, fy, SUB_F(fy, FLOAT(.5f)));
    SpvId x0 = FLOOR_F(fx), y0 = FLOOR_F(fy);
    SpvId wx = PICK_F(nearest, FLOAT(0.f), SUB_F(fx, x0)), wy = PICK_F(nearest, FLOAT(0.f), SUB_F(fy, y0));
    SpvId ix = wrapOf(h, x0, wf), iy = wrapOf(h, y0, hf);
    SpvId ix1 = PICK_U(LESS_U(ADD_U(ix, UINT(1)), w), ADD_U(ix, UINT(1)), UINT(0));
    SpvId iy1 = PICK_U(LESS_U(ADD_U(iy, UINT(1)), hh), ADD_U(iy, UINT(1)), UINT(0));
    SpvId a = texelOf(h, first, w, normal, ix, iy), b = texelOf(h, first, w, normal, ix1, iy);
    SpvId c = texelOf(h, first, w, normal, ix, iy1), d = texelOf(h, first, w, normal, ix1, iy1);
    SpvId out[4];
    for (uint32_t k = 0; k < 4; ++k) {
        SpvId ak = spvExtract(S, S->floatType, a, k), bk = spvExtract(S, S->floatType, b, k);
        SpvId ck = spvExtract(S, S->floatType, c, k), dk = spvExtract(S, S->floatType, d, k);
        SpvId top = ADD_F(ak, MUL_F(SUB_F(bk, ak), wx)), bottom = ADD_F(ck, MUL_F(SUB_F(dk, ck), wx));
        out[k] = ADD_F(top, MUL_F(SUB_F(bottom, top), wy));
    }
    return vec4Of(h, out[0], out[1], out[2], out[3]);
}

/* ---- shadows: as artf2DShadowSee ---- */

typedef struct Map2D {
    SpvId offset, bins, wraps, owner, dd;
} Map2D;

static SpvId depthOf(Sh* h, const Map2D* m, SpvId i /* int */) {
    /* (a lamp's bins are a power of two) */
    SpvId wrapped = I_AS_U(AND_I(i, spvOp1(S, SPV_OP_Bitcast, S->intType, SUB_U(m->bins, UINT(1)))));
    SpvId inside = AND_B(MORE_EQ_I(i, INT(0)), LESS_I(i, spvOp1(S, SPV_OP_Bitcast, S->intType, m->bins)));
    SpvId at = PICK_U(m->wraps, wrapped, PICK_U(inside, I_AS_U(i), UINT(0)));
    SpvId near_ = AS_F(uintAt(h, B2_SHADOWS, ADD_U(m->offset, at)));
    SpvId owner = uintAt(h, B2_SHADOWS, ADD_U(ADD_U(m->offset, m->bins), at));
    SpvId past = AS_F(uintAt(h, B2_SHADOWS, ADD_U(ADD_U(m->offset, MUL_U(m->bins, UINT(2))), at)));
    SpvId t = PICK_F(AND_B(NE_U(m->owner, UINT(0)), EQ_U(owner, m->owner)), past, near_);
    return PICK_F(OR_B(m->wraps, inside), t, FLOAT(3.4e38f));
}

static SpvId litBetween(Sh* h, const Map2D* m, SpvId x) {
    SpvId fl = FLOOR_F(x), w = SUB_F(x, fl);
    SpvId i = TO_I(fl);
    SpvId a = PICK_F(MORE_EQ_F(depthOf(h, m, i), m->dd), FLOAT(1.f), FLOAT(0.f));
    SpvId b = PICK_F(MORE_EQ_F(depthOf(h, m, ADD_I(i, INT(1))), m->dd), FLOAT(1.f), FLOAT(0.f));
    return ADD_F(MUL_F(a, SUB_F(FLOAT(1.f), w)), MUL_F(b, w));
}

static SpvId shadowSee(Sh* h, SpvId l0, SpvId l2, SpvId l3, SpvId l4, SpvId l5, SpvId px, SpvId py, SpvId owner) {
    SpvId sun = EQ_U(AS_U(X(l0)), UINT(ARTF_LIGHT2D_SUN));
    Map2D m = {.offset = AS_U(X(l5)), .bins = AS_U(W(l4)), .owner = owner};
    m.wraps = NOT_B(sun);
    SpvId binsF = TO_F(m.bins);
    /* the sun: across and along its light */
    SpvId gx = NEG_F(X(l3)), gy = NEG_F(Y(l3));
    SpvId planar = MAX_F(SQRT_F(ADD_F(MUL_F(gx, gx), MUL_F(gy, gy))), FLOAT(1e-30f));
    SpvId ax = DIV_F(gx, planar), ay = DIV_F(gy, planar);
    SpvId sunC = SUB_F(MUL_F(SUB_F(ADD_F(MUL_F(NEG_F(ay), px), MUL_F(ax, py)), Y(l4)), Z(l4)), FLOAT(.5f));
    SpvId sunD = ADD_F(MUL_F(ax, px), MUL_F(ay, py));
    /* a lamp: around it */
    SpvId vx = SUB_F(px, Y(l0)), vy = SUB_F(py, Z(l0));
    SpvId lampD = SQRT_F(ADD_F(MUL_F(vx, vx), MUL_F(vy, vy)));
    SpvId perRadian = DIV_F(binsF, FLOAT(2.f * PI2D));
    SpvId lampC = SUB_F(MUL_F(ADD_F(atan2Of(h, vy, vx), FLOAT(PI2D)), perRadian), FLOAT(.5f));
    SpvId c = PICK_F(sun, sunC, lampC), d = PICK_F(sun, sunD, lampD);
    SpvId perUnit = PICK_F(sun, Z(l4), PICK_F(MORE_F(lampD, FLOAT(0.f)), DIV_F(perRadian, MAX_F(lampD, FLOAT(1e-30f))), perRadian));
    SpvId bias = ADD_F(FLOAT(1.5f), PICK_F(sun, FLOAT(0.f), MUL_F(d, DIV_F(FLOAT(2.f * PI2D), binsF))));
    m.dd = SUB_F(d, bias);
    SpvId radius = Y(l2);
    SpvId hard = litBetween(h, &m, c);

    SpvId limit = MUL_F(binsF, FLOAT(.125f));
    SpvId reach = PICK_F(sun, MUL_F(MUL_F(radius, d), perUnit), MUL_F(radius, perUnit));
    SpvId search = CLAMP_F(reach, FLOAT(1.f), limit);
    SpvId sum = FLOAT(0.f), blockers = FLOAT(0.f);
    for (int k = 0; k < 8; ++k) {
        SpvId x = ADD_F(c, MUL_F(search, FLOAT(((float)k + .5f) * (2.f / 8.f) - 1.f)));
        SpvId t = depthOf(h, &m, TO_I(FLOOR_F(ADD_F(x, FLOAT(.5f)))));
        SpvId in = LESS_F(t, m.dd);
        sum = ADD_F(sum, PICK_F(in, t, FLOAT(0.f)));
        blockers = ADD_F(blockers, PICK_F(in, FLOAT(1.f), FLOAT(0.f)));
    }
    SpvId blocker = MAX_F(DIV_F(sum, MAX_F(blockers, FLOAT(1.f))), FLOAT(1.f));
    SpvId past = SUB_F(d, blocker);
    SpvId wide = PICK_F(sun, MUL_F(MUL_F(radius, past), perUnit), MUL_F(DIV_F(MUL_F(radius, past), blocker), perUnit));
    SpvId half = CLAMP_F(wide, FLOAT(.5f), limit);
    SpvId seen = FLOAT(0.f);
    for (int k = 0; k < 16; ++k) seen = ADD_F(seen, litBetween(h, &m, ADD_F(c, MUL_F(half, FLOAT(((float)k + .5f) * (2.f / 16.f) - 1.f)))));
    SpvId soft = PICK_F(MORE_F(blockers, FLOAT(0.f)), MUL_F(seen, FLOAT(1.f / 16.f)), hard);
    return PICK_F(LESS_EQ_F(radius, FLOAT(0.f)), hard, soft);
}

/* As artf2DCoverAt: how far (x, y) of a sprite's own space is from the edge of its rect, ellipse or line. */
static SpvId shapeDistance(Sh* h, SpvId shapeRow, SpvId line, SpvId shape, SpvId lx, SpvId ly) {
    SpvId hw = X(shapeRow), hhh = Y(shapeRow);
    SpvId r = CLAMP_F(Z(shapeRow), FLOAT(0.f), MIN_F(hw, hhh));
    SpvId qx = ADD_F(SUB_F(ABS_F(lx), hw), r), qy = ADD_F(SUB_F(ABS_F(ly), hhh), r);
    SpvId ox = MAX_F(qx, FLOAT(0.f)), oy = MAX_F(qy, FLOAT(0.f));
    SpvId rectD = SUB_F(ADD_F(SQRT_F(ADD_F(MUL_F(ox, ox), MUL_F(oy, oy))), MIN_F(MAX_F(qx, qy), FLOAT(0.f))), r);
    SpvId shw = MAX_F(hw, FLOAT(1e-30f)), shh = MAX_F(hhh, FLOAT(1e-30f));
    SpvId ux = DIV_F(lx, shw), uy = DIV_F(ly, shh), vx = DIV_F(lx, MUL_F(shw, shw)), vy = DIV_F(ly, MUL_F(shh, shh));
    SpvId k1 = SQRT_F(ADD_F(MUL_F(ux, ux), MUL_F(uy, uy))), k2 = SQRT_F(ADD_F(MUL_F(vx, vx), MUL_F(vy, vy)));
    SpvId ellipseD = PICK_F(MORE_F(k2, FLOAT(0.f)), DIV_F(MUL_F(k1, SUB_F(k1, FLOAT(1.f))), MAX_F(k2, FLOAT(1e-30f))), NEG_F(MIN_F(hw, hhh)));
    ellipseD = PICK_F(OR_B(LESS_EQ_F(hw, FLOAT(0.f)), LESS_EQ_F(hhh, FLOAT(0.f))), FLOAT(1e30f), ellipseD);
    SpvId ex = SUB_F(Z(line), X(line)), ey = SUB_F(W(line), Y(line)), len2 = ADD_F(MUL_F(ex, ex), MUL_F(ey, ey));
    SpvId t = PICK_F(MORE_F(len2, FLOAT(0.f)), DIV_F(ADD_F(MUL_F(SUB_F(lx, X(line)), ex), MUL_F(SUB_F(ly, Y(line)), ey)), MAX_F(len2, FLOAT(1e-30f))), FLOAT(0.f));
    t = CLAMP_F(t, FLOAT(0.f), FLOAT(1.f));
    SpvId dx = SUB_F(SUB_F(lx, X(line)), MUL_F(ex, t)), dy = SUB_F(SUB_F(ly, Y(line)), MUL_F(ey, t));
    SpvId lineD = SUB_F(SQRT_F(ADD_F(MUL_F(dx, dx), MUL_F(dy, dy))), W(shapeRow));
    return PICK_F(EQ_U(shape, UINT(ARTF_SHAPE_RECT)), rectD, PICK_F(EQ_U(shape, UINT(ARTF_SHAPE_ELLIPSE)), ellipseD, lineD));
}

/* ---- the tiles: a pixel an invocation, its tile's sprites over it in order ---- */

uint32_t* artf2DShaderTiles(size_t* size) {
    Spv spv;
    Sh hh, *h = &hh;
    static const int used[] = {B2_GLOBALS, B2_ITEMS, B2_EDGES, B2_TILES, B2_LAMPS, B2_SHADOWS, B2_TEXELS, B2_LEVELS, B2_GI, B2_CHAIN, B2_PIXELS, B2_REFLECT};
    begin(h, &spv, used, (int)ARTF_COUNT(used));
    SpvId main = spvBeginMain(S);
    /* (the function's variables, first) */
    SpvId color = fnVar(h, h->fnVec4), reflected = fnVar(h, h->fnVec4), cover = fnVar(h, h->fnFloat);
    SpvId itemI = fnVar(h, h->fnUint), lampI = fnVar(h, h->fnUint), edgeI = fnVar(h, h->fnUint);
    SpvId diffuse[3], highlight[3], wind[16];
    for (int i = 0; i < 3; ++i) diffuse[i] = fnVar(h, h->fnFloat), highlight[i] = fnVar(h, h->fnFloat);
    for (int i = 0; i < 16; ++i) wind[i] = fnVar(h, h->fnInt);
    SpvId crossAt[8];
    for (int i = 0; i < 8; ++i) crossAt[i] = fnVar(h, h->fnFloat);

    SpvId id = spvLoad(S, h->uvec3, h->global);
    SpvId x = spvExtract(S, S->uintType, id, 0), y = spvExtract(S, S->uintType, id, 1);
    SpvId gSize = vecAt(h, B2_GLOBALS, UINT(G_SIZE)), gAmbient = vecAt(h, B2_GLOBALS, UINT(G_AMBIENT));
    SpvId gBack = vecAt(h, B2_GLOBALS, UINT(G_BACK)), gMode = vecAt(h, B2_GLOBALS, UINT(G_MODE)), gFlags = vecAt(h, B2_GLOBALS, UINT(G_FLAGS));
    SpvId width = AS_U(X(gSize)), height = AS_U(Y(gSize)), tilesX = AS_U(Z(gSize)), giScale = AS_U(W(gSize));
    SpvId inFrame;
    spvIfBegin(S, AND_B(LESS_U(x, width), LESS_U(y, height)), &inFrame);
    {
        SpvId tile = ADD_U(MUL_U(DIV_U(y, UINT(TILE2D)), tilesX), DIV_U(x, UINT(TILE2D)));
        SpvId itemStart = uintAt(h, B2_TILES, MUL_U(tile, UINT(4))), itemCount = uintAt(h, B2_TILES, ADD_U(MUL_U(tile, UINT(4)), UINT(1)));
        SpvId lampStart = uintAt(h, B2_TILES, ADD_U(MUL_U(tile, UINT(4)), UINT(2))), lampCount = uintAt(h, B2_TILES, ADD_U(MUL_U(tile, UINT(4)), UINT(3)));
        SpvId px = ADD_F(TO_F(x), FLOAT(.5f)), py = ADD_F(TO_F(y), FLOAT(.5f));
        SpvId hasGi = NE_U(AS_U(W(gMode)), UINT(0)), reflects = NE_U(AS_U(Y(gFlags)), UINT(0));
        spvStore(S, color, gBack);
        spvStore(S, reflected, vec4Of(h, FLOAT(0.f), FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)));
        /* gi's light here, between its texels (as giAt) */
        SpvId gi[3] = {FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)};
        {
            SpvId giW = AS_U(Y(gMode)), giH = AS_U(Z(gMode));
            SpvId inv = DIV_F(FLOAT(1.f), MAX_F(TO_F(giScale), FLOAT(1.f)));
            SpvId gx = SUB_F(MUL_F(px, inv), FLOAT(.5f)), gy = SUB_F(MUL_F(py, inv), FLOAT(.5f));
            SpvId fx = FLOOR_F(gx), fy = FLOOR_F(gy), wx = SUB_F(gx, fx), wy = SUB_F(gy, fy);
            SpvId lastX = SUB_F(MAX_F(TO_F(giW), FLOAT(1.f)), FLOAT(1.f)), lastY = SUB_F(MAX_F(TO_F(giH), FLOAT(1.f)), FLOAT(1.f));
            SpvId x0 = TO_U(CLAMP_F(fx, FLOAT(0.f), lastX)), x1 = TO_U(CLAMP_F(ADD_F(fx, FLOAT(1.f)), FLOAT(0.f), lastX));
            SpvId y0 = TO_U(CLAMP_F(fy, FLOAT(0.f), lastY)), y1 = TO_U(CLAMP_F(ADD_F(fy, FLOAT(1.f)), FLOAT(0.f), lastY));
            /* (where there is no gi, its buffer is three floats: read the first) */
            SpvId g00 = PICK_U(hasGi, MUL_U(ADD_U(MUL_U(y0, giW), x0), UINT(3)), UINT(0));
            SpvId g10 = PICK_U(hasGi, MUL_U(ADD_U(MUL_U(y0, giW), x1), UINT(3)), UINT(0));
            SpvId g01 = PICK_U(hasGi, MUL_U(ADD_U(MUL_U(y1, giW), x0), UINT(3)), UINT(0));
            SpvId g11 = PICK_U(hasGi, MUL_U(ADD_U(MUL_U(y1, giW), x1), UINT(3)), UINT(0));
            for (int c = 0; c < 3; ++c) {
                SpvId a = floatAt(h, B2_GI, ADD_U(g00, UINT((uint32_t)c))), b = floatAt(h, B2_GI, ADD_U(g10, UINT((uint32_t)c)));
                SpvId cc = floatAt(h, B2_GI, ADD_U(g01, UINT((uint32_t)c))), d = floatAt(h, B2_GI, ADD_U(g11, UINT((uint32_t)c)));
                SpvId top = ADD_F(a, MUL_F(SUB_F(b, a), wx)), bottom = ADD_F(cc, MUL_F(SUB_F(d, cc), wx));
                gi[c] = PICK_F(hasGi, ADD_F(top, MUL_F(SUB_F(bottom, top), wy)), FLOAT(0.f));
            }
        }

        Loop2D items;
        SpvId n = loopBegin(h, &items, itemI, itemCount);
        {
            SpvId item = uintAt(h, B2_TILES, ADD_U(itemStart, n));
            SpvId base = MUL_U(item, UINT(ITEM_ROWS));
#define ROW(r) vecAt(h, B2_ITEMS, ADD_U(base, UINT(r)))
            SpvId box = ROW(I_BOX);
            SpvId inBox = AND_B(AND_B(MORE_EQ_U(x, AS_U(X(box))), LESS_U(x, AS_U(Z(box)))), AND_B(MORE_EQ_U(y, AS_U(Y(box))), LESS_U(y, AS_U(W(box)))));
            SpvId boxMerge;
            spvIfBegin(S, inBox, &boxMerge);
            {
                SpvId m0 = ROW(I_MAP0), m1 = ROW(I_MAP1), shapeRow = ROW(I_SHAPE), line = ROW(I_LINE), uv = ROW(I_UV);
                SpvId colorRow = ROW(I_COLOR), emitRow = ROW(I_EMIT), turn = ROW(I_TURN), flagsRow = ROW(I_FLAGS), texRow = ROW(I_TEX);
                SpvId lx = ADD_F(ADD_F(MUL_F(X(m0), px), MUL_F(Y(m0), py)), Z(m0));
                SpvId ly = ADD_F(ADD_F(MUL_F(W(m0), px), MUL_F(X(m1), py)), Y(m1));
                SpvId zoom = Z(m1), shape = AS_U(W(m1));
                SpvId flags = AS_U(X(flagsRow));
                SpvId lit = NE_U(AND_U(flags, UINT(I2_LIT)), UINT(0)), nearest = NE_U(AND_U(flags, UINT(I2_NEAREST)), UINT(0));
                SpvId polygon = NE_U(AND_U(flags, UINT(I2_POLYGON)), UINT(0)), emits = NE_U(AND_U(flags, UINT(I2_EMITS)), UINT(0));
                SpvId owner = AS_U(Y(flagsRow));
                SpvId dist = shapeDistance(h, shapeRow, line, shape, lx, ly);
                spvStore(S, cover, CLAMP_F(SUB_F(FLOAT(.5f), MUL_F(dist, zoom)), FLOAT(0.f), FLOAT(1.f)));
                /* a polygon: four rows of four samples, by non-zero winding */
                SpvId polyMerge;
                spvIfBegin(S, polygon, &polyMerge);
                {
                    /* Each of four rows: the winding at the pixel's left side (of the crossings right of it), and the
                     * first two crossings within the pixel, in order (wind: 0 that winding, 1 how many within, 2 and 3
                     * their directions; at: their places). Across the pixel it is then exact, as the CPU's spans. */
                    for (int i = 0; i < 16; ++i) spvStore(S, wind[i], INT(0));
                    for (int i = 0; i < 8; ++i) spvStore(S, crossAt[i], FLOAT(0.f));
                    SpvId first = AS_U(Z(flagsRow)), count = AS_U(W(flagsRow));
                    SpvId left = TO_F(x), right = ADD_F(TO_F(x), FLOAT(1.f));
                    Loop2D edges;
                    SpvId e = loopBegin(h, &edges, edgeI, count);
                    {
                        SpvId g = vecAt(h, B2_EDGES, ADD_U(first, e));
                        SpvId dir = PICK_I(MORE_F(W(g), Y(g)), INT(1), INT(-1));
                        for (int sr = 0; sr < 4; ++sr) {
                            SpvId sy = ADD_F(TO_F(y), FLOAT(((float)sr + .5f) * .25f));
                            SpvId crosses = spvOp2(S, 165 /* OpLogicalNotEqual */, S->boolType, LESS_EQ_F(Y(g), sy), LESS_EQ_F(W(g), sy));
                            SpvId xc = ADD_F(X(g), MUL_F(DIV_F(SUB_F(sy, Y(g)), SUB_F(W(g), Y(g))), SUB_F(Z(g), X(g))));
                            SpvId* w = &wind[sr * 4];
                            SpvId* at = &crossAt[sr * 2];
                            SpvId rightOf = AND_B(crosses, MORE_F(xc, left));
                            spvStore(S, w[0], ADD_I(LOAD_I(w[0]), PICK_I(rightOf, dir, INT(0))));
                            SpvId within = AND_B(rightOf, LESS_F(xc, right));
                            SpvId n = LOAD_I(w[1]);
                            SpvId c1 = LOAD_F(at[0]), d1 = LOAD_I(w[2]);
                            SpvId before = LESS_F(xc, c1);
                            SpvId isFirst = spvOp2(S, OP_IEqual, S->boolType, n, INT(0)), isSecond = spvOp2(S, OP_IEqual, S->boolType, n, INT(1));
                            /* n 0: it is the first; n 1: first or second, by place; more: left out */
                            SpvId newC1 = PICK_F(OR_B(isFirst, AND_B(isSecond, before)), xc, c1);
                            SpvId newD1 = PICK_I(OR_B(isFirst, AND_B(isSecond, before)), dir, d1);
                            SpvId newC2 = PICK_F(AND_B(isSecond, before), c1, PICK_F(isSecond, xc, LOAD_F(at[1])));
                            SpvId newD2 = PICK_I(AND_B(isSecond, before), d1, PICK_I(isSecond, dir, LOAD_I(w[3])));
                            spvStore(S, at[0], PICK_F(within, newC1, c1));
                            spvStore(S, w[2], PICK_I(within, newD1, d1));
                            spvStore(S, at[1], PICK_F(within, newC2, LOAD_F(at[1])));
                            spvStore(S, w[3], PICK_I(within, newD2, LOAD_I(w[3])));
                            spvStore(S, w[1], ADD_I(n, PICK_I(within, INT(1), INT(0))));
                        }
                    }
                    loopEnd(h, &edges);
                    SpvId inside = FLOAT(0.f);
                    for (int sr = 0; sr < 4; ++sr) {
                        SpvId* w = &wind[sr * 4];
                        SpvId* at = &crossAt[sr * 2];
                        SpvId n = LOAD_I(w[1]);
                        SpvId w0 = LOAD_I(w[0]), w1 = SUB_I(w0, LOAD_I(w[2])), w2 = SUB_I(w1, LOAD_I(w[3]));
                        SpvId c1 = PICK_F(MORE_EQ_I(n, INT(1)), LOAD_F(at[0]), right);
                        SpvId c2 = PICK_F(MORE_EQ_I(n, INT(2)), LOAD_F(at[1]), right);
                        SpvId part = ADD_F(ADD_F(PICK_F(NE_I(w0, INT(0)), SUB_F(c1, left), FLOAT(0.f)), PICK_F(NE_I(w1, INT(0)), SUB_F(c2, c1), FLOAT(0.f))),
                                           PICK_F(NE_I(w2, INT(0)), SUB_F(right, c2), FLOAT(0.f)));
                        inside = ADD_F(inside, MUL_F(part, FLOAT(.25f)));
                    }
                    spvStore(S, cover, MIN_F(inside, FLOAT(1.f)));
                }
                spvIfEnd(S, polyMerge);
                SpvId cov = LOAD_F(cover);
                SpvId covered;
                spvIfBegin(S, MORE_F(cov, FLOAT(0.f)), &covered);
                {
                    /* as artf2DSample */
                    SpvId u = ADD_F(X(uv), MUL_F(lx, Y(uv))), v = ADD_F(Z(uv), MUL_F(ly, W(uv)));
                    const SpvId none = UINT(0xFFFFFFFFu);
                    SpvId tex0 = AS_U(X(texRow)), tex1 = AS_U(Y(texRow)), tex2 = AS_U(Z(texRow));
                    SpvId t0 = sampleOf(h, PICK_U(EQ_U(tex0, none), UINT(0), tex0), nearest, u, v);
                    SpvId has0 = NE_U(tex0, none);
                    SpvId tc[4];
                    for (uint32_t k = 0; k < 4; ++k) tc[k] = PICK_F(has0, spvExtract(S, S->floatType, t0, k), FLOAT(1.f));
                    SpvId kk = MUL_F(W(colorRow), cov);
                    SpvId rgba[4] = {MUL_F(MUL_F(X(colorRow), tc[0]), kk), MUL_F(MUL_F(Y(colorRow), tc[1]), kk), MUL_F(MUL_F(Z(colorRow), tc[2]), kk), MUL_F(tc[3], kk)};
                    SpvId t2 = sampleOf(h, PICK_U(EQ_U(tex2, none), UINT(0), tex2), nearest, u, v);
                    SpvId has2 = NE_U(tex2, none);
                    SpvId ke = MUL_F(kk, tc[3]);
                    SpvId emit[3];
                    for (uint32_t k = 0; k < 3; ++k) {
                        SpvId e = PICK_F(has2, spvExtract(S, S->floatType, t2, k), FLOAT(1.f));
                        emit[k] = PICK_F(emits, MUL_F(MUL_F(spvExtract(S, S->floatType, emitRow, k), e), ke), FLOAT(0.f));
                    }
                    SpvId shows = OR_B(MORE_F(rgba[3], FLOAT(0.f)), MORE_F(ADD_F(ADD_F(emit[0], emit[1]), emit[2]), FLOAT(0.f)));
                    SpvId showMerge;
                    spvIfBegin(S, shows, &showMerge);
                    {
                        /* which way it faces */
                        SpvId t1 = sampleOf(h, PICK_U(EQ_U(tex1, none), UINT(0), tex1), nearest, u, v);
                        SpvId has1 = NE_U(tex1, none);
                        SpvId nx = X(t1), ny = NEG_F(Y(t1)), nz = MAX_F(Z(t1), FLOAT(0.f));
                        SpvId len = SQRT_F(ADD_F(ADD_F(MUL_F(nx, nx), MUL_F(ny, ny)), MUL_F(nz, nz)));
                        SpvId sInv = PICK_F(MORE_F(len, FLOAT(0.f)), DIV_F(FLOAT(1.f), MAX_F(len, FLOAT(1e-30f))), FLOAT(0.f));
                        SpvId tcos = Y(turn), tsin = Z(turn);
                        SpvId N[3];
                        N[0] = PICK_F(has1, MUL_F(SUB_F(MUL_F(tcos, nx), MUL_F(tsin, ny)), sInv), FLOAT(0.f));
                        N[1] = PICK_F(has1, MUL_F(ADD_F(MUL_F(tsin, nx), MUL_F(tcos, ny)), sInv), FLOAT(0.f));
                        N[2] = PICK_F(has1, PICK_F(MORE_F(len, FLOAT(0.f)), MUL_F(nz, sInv), FLOAT(1.f)), FLOAT(1.f));
                        for (int c = 0; c < 3; ++c) {
                            spvStore(S, diffuse[c], ADD_F(spvExtract(S, S->floatType, gAmbient, (uint32_t)c), gi[c]));
                            spvStore(S, highlight[c], FLOAT(0.f));
                        }
                        SpvId litMerge;
                        spvIfBegin(S, lit, &litMerge);
                        {
                            /* as lightAt */
                            SpvId specular = W(emitRow), lightMask = AS_U(W(turn));
                            SpvId rough = CLAMP_F(X(turn), FLOAT(.05f), FLOAT(1.f)), alpha = MUL_F(rough, rough), alpha2 = MUL_F(alpha, alpha);
                            Loop2D lamps;
                            SpvId j = loopBegin(h, &lamps, lampI, lampCount);
                            {
                                SpvId lamp = uintAt(h, B2_TILES, ADD_U(lampStart, j));
                                SpvId lb = MUL_U(lamp, UINT(LAMP_ROWS));
                                SpvId l0 = vecAt(h, B2_LAMPS, lb), l1 = vecAt(h, B2_LAMPS, ADD_U(lb, UINT(1))), l2 = vecAt(h, B2_LAMPS, ADD_U(lb, UINT(2)));
                                SpvId l3 = vecAt(h, B2_LAMPS, ADD_U(lb, UINT(3))), l4 = vecAt(h, B2_LAMPS, ADD_U(lb, UINT(4))), l5 = vecAt(h, B2_LAMPS, ADD_U(lb, UINT(5)));
                                SpvId kind = AS_U(X(l0));
                                SpvId isSun = EQ_U(kind, UINT(ARTF_LIGHT2D_SUN)), isSpot = EQ_U(kind, UINT(ARTF_LIGHT2D_SPOT));
                                SpvId masked = NE_U(AND_U(AS_U(W(l0)), lightMask), UINT(0));
                                SpvId wx = SUB_F(Y(l0), px), wy = SUB_F(Z(l0), py), d2 = ADD_F(MUL_F(wx, wx), MUL_F(wy, wy));
                                SpvId inRange = OR_B(isSun, LESS_F(d2, W(l1)));
                                SpvId xx = SUB_F(FLOAT(1.f), MUL_F(d2, X(l2)));
                                SpvId k = PICK_F(isSun, FLOAT(1.f), MUL_F(xx, xx));
                                SpvId dist = SQRT_F(d2);
                                SpvId cc = PICK_F(MORE_F(dist, FLOAT(0.f)), DIV_F(NEG_F(ADD_F(MUL_F(wx, X(l3)), MUL_F(wy, Y(l3)))), MAX_F(dist, FLOAT(1e-30f))), FLOAT(1.f));
                                SpvId inCone = OR_B(NOT_B(isSpot), MORE_F(cc, X(l4)));
                                SpvId tt = DIV_F(SUB_F(cc, X(l4)), SUB_F(W(l3), X(l4)));
                                SpvId smooth = PICK_F(LESS_F(cc, W(l3)), MUL_F(MUL_F(tt, tt), SUB_F(FLOAT(3.f), MUL_F(FLOAT(2.f), tt))), FLOAT(1.f));
                                k = PICK_F(isSpot, MUL_F(k, smooth), k);
                                SpvId height = Z(l2);
                                SpvId inv = DIV_F(FLOAT(1.f), SQRT_F(MAX_F(ADD_F(d2, MUL_F(height, height)), FLOAT(1e-30f))));
                                SpvId Lx = PICK_F(isSun, X(l3), MUL_F(wx, inv)), Ly = PICK_F(isSun, Y(l3), MUL_F(wy, inv)), Lz = PICK_F(isSun, Z(l3), MUL_F(height, inv));
                                SpvId ndl = ADD_F(ADD_F(MUL_F(N[0], Lx), MUL_F(N[1], Ly)), MUL_F(N[2], Lz));
                                SpvId reaches;
                                spvIfBegin(S, AND_B(AND_B(masked, inRange), AND_B(inCone, MORE_F(ndl, FLOAT(0.f)))), &reaches);
                                {
                                    SpvId shadowed = NE_U(AS_U(W(l2)), UINT(0));
                                    /* (the map is read only where there is one) */
                                    SpvId seenVar = cover; /* (cover is no longer needed: it holds what the light sees) */
                                    spvStore(S, seenVar, FLOAT(1.f));
                                    SpvId mapMerge;
                                    spvIfBegin(S, shadowed, &mapMerge);
                                    spvStore(S, seenVar, shadowSee(h, l0, l2, l3, l4, l5, px, py, owner));
                                    spvIfEnd(S, mapMerge);
                                    SpvId w = MUL_F(MUL_F(k, LOAD_F(seenVar)), ndl);
                                    SpvId hx = Lx, hy = Ly, hz = ADD_F(Lz, FLOAT(1.f));
                                    SpvId hl = SQRT_F(ADD_F(ADD_F(MUL_F(hx, hx), MUL_F(hy, hy)), MUL_F(hz, hz)));
                                    SpvId nh = MAX_F(DIV_F(ADD_F(ADD_F(MUL_F(N[0], hx), MUL_F(N[1], hy)), MUL_F(N[2], hz)), hl), FLOAT(0.f));
                                    SpvId dd = ADD_F(MUL_F(MUL_F(nh, nh), SUB_F(alpha2, FLOAT(1.f))), FLOAT(1.f));
                                    SpvId spec = MUL_F(DIV_F(MUL_F(MUL_F(specular, FLOAT(.25f)), alpha2), MUL_F(dd, dd)), w);
                                    spec = PICK_F(MORE_F(specular, FLOAT(0.f)), spec, FLOAT(0.f));
                                    for (int c = 0; c < 3; ++c) {
                                        SpvId lc = spvExtract(S, S->floatType, l1, (uint32_t)c);
                                        spvStore(S, diffuse[c], ADD_F(LOAD_F(diffuse[c]), MUL_F(lc, w)));
                                        spvStore(S, highlight[c], ADD_F(LOAD_F(highlight[c]), MUL_F(lc, spec)));
                                    }
                                }
                                spvIfEnd(S, reaches);
                            }
                            loopEnd(h, &lamps);
                        }
                        spvIfEnd(S, litMerge);
                        SpvId light[3], hi[3];
                        for (int c = 0; c < 3; ++c) light[c] = LOAD_F(diffuse[c]), hi[c] = LOAD_F(highlight[c]);
                        SpvId out[3];
                        for (int c = 0; c < 3; ++c)
                            out[c] = PICK_F(lit, ADD_F(ADD_F(MUL_F(rgba[c], light[c]), MUL_F(hi[c], rgba[3])), emit[c]), ADD_F(rgba[c], emit[c]));
                        /* what this wall sends on, for gi's next frame */
                        SpvId sends = AND_B(AND_B(reflects, lit), AND_B(NE_U(owner, UINT(0)), MORE_F(rgba[3], FLOAT(.5f))));
                        SpvId invA = DIV_F(FLOAT(1.f), MAX_F(rgba[3], FLOAT(1e-30f)));
                        SpvId was = spvLoad(S, S->vec4, reflected);
                        SpvId add = vec4Of(h, MUL_F(MUL_F(rgba[0], invA), light[0]), MUL_F(MUL_F(rgba[1], invA), light[1]), MUL_F(MUL_F(rgba[2], invA), light[2]), FLOAT(1.f));
                        /* (SPIR-V 1.3 selects vectors only by a vector of conditions: a factor of 0 or 1) */
                        SpvId on = PICK_F(sends, FLOAT(1.f), FLOAT(0.f));
                        spvStore(S, reflected, spvOp2(S, SPV_OP_FAdd, S->vec4, was, spvOp2(S, SPV_OP_VectorTimesScalar, S->vec4, add, on)));
                        SpvId dst = spvLoad(S, S->vec4, color);
                        SpvId keep = SUB_F(FLOAT(1.f), rgba[3]);
                        spvStore(S, color, vec4Of(h, ADD_F(out[0], MUL_F(X(dst), keep)), ADD_F(out[1], MUL_F(Y(dst), keep)), ADD_F(out[2], MUL_F(Z(dst), keep)),
                                                  ADD_F(rgba[3], MUL_F(W(dst), keep))));
                    }
                    spvIfEnd(S, showMerge);
                }
                spvIfEnd(S, covered);
            }
            spvIfEnd(S, boxMerge);
#undef ROW
        }
        loopEnd(h, &items);

        SpvId at = ADD_U(MUL_U(y, width), x);
        SpvId final = spvLoad(S, S->vec4, color);
        SpvId hdr = NE_U(AS_U(X(gFlags)), UINT(0));
        SpvId hdrMerge;
        spvIfBegin(S, hdr, &hdrMerge);
        spvStore(S, vecPtr(h, B2_CHAIN, at), final);
        spvIfEnd(S, hdrMerge);
        SpvId plainMerge;
        spvIfBegin(S, NOT_B(hdr), &plainMerge);
        SpvId swap = NE_U(AS_U(X(vecAt(h, B2_GLOBALS, UINT(G_OUT)))), UINT(0));
        spvStore(S, uintPtr(h, B2_PIXELS, at), tone(h, final, W(gAmbient), AS_U(X(gMode)), swap));
        spvIfEnd(S, plainMerge);
        SpvId reflectMerge;
        spvIfBegin(S, reflects, &reflectMerge);
        spvStore(S, vecPtr(h, B2_REFLECT, at), spvLoad(S, S->vec4, reflected));
        spvIfEnd(S, reflectMerge);
    }
    spvIfEnd(S, inFrame);
    return end(h, main, size);
}

/* ---- bloom (as post2d.c) ---- */

/* Push: from, its width and height; to, its width and height (offsets in vec4s of binding 9). */
uint32_t* artf2DShaderBloomDown(size_t* size) {
    Spv spv;
    Sh hh, *h = &hh;
    static const int used[] = {B2_CHAIN};
    begin(h, &spv, used, 1);
    SpvId main = spvBeginMain(S);
    SpvId id = spvLoad(S, h->uvec3, h->global);
    SpvId x = spvExtract(S, S->uintType, id, 0), y = spvExtract(S, S->uintType, id, 1);
    SpvId from = pushAt(h, 0), fw = pushAt(h, 1), fh = pushAt(h, 2), to = pushAt(h, 3), tw = pushAt(h, 4), th = pushAt(h, 5);
    SpvId merge;
    spvIfBegin(S, AND_B(LESS_U(x, tw), LESS_U(y, th)), &merge);
    {
        SpvId lastX = SUB_U(fw, UINT(1)), lastY = SUB_U(fh, UINT(1));
        SpvId x0 = spvGlsl2(S, SPV_GLSL_UMin, S->uintType, MUL_U(x, UINT(2)), lastX), x1 = spvGlsl2(S, SPV_GLSL_UMin, S->uintType, ADD_U(MUL_U(x, UINT(2)), UINT(1)), lastX);
        SpvId y0 = spvGlsl2(S, SPV_GLSL_UMin, S->uintType, MUL_U(y, UINT(2)), lastY), y1 = spvGlsl2(S, SPV_GLSL_UMin, S->uintType, ADD_U(MUL_U(y, UINT(2)), UINT(1)), lastY);
        SpvId a = vecAt(h, B2_CHAIN, ADD_U(from, ADD_U(MUL_U(y0, fw), x0))), b = vecAt(h, B2_CHAIN, ADD_U(from, ADD_U(MUL_U(y0, fw), x1)));
        SpvId c = vecAt(h, B2_CHAIN, ADD_U(from, ADD_U(MUL_U(y1, fw), x0))), d = vecAt(h, B2_CHAIN, ADD_U(from, ADD_U(MUL_U(y1, fw), x1)));
        SpvId out[3];
        for (uint32_t k = 0; k < 3; ++k)
            out[k] = MUL_F(ADD_F(ADD_F(ADD_F(spvExtract(S, S->floatType, a, k), spvExtract(S, S->floatType, b, k)), spvExtract(S, S->floatType, c, k)), spvExtract(S, S->floatType, d, k)), FLOAT(.25f));
        spvStore(S, vecPtr(h, B2_CHAIN, ADD_U(to, ADD_U(MUL_U(y, tw), x))), vec4Of(h, out[0], out[1], out[2], FLOAT(0.f)));
    }
    spvIfEnd(S, merge);
    return end(h, main, size);
}

/* As tapsFor: where texel i of a level twice as large reads, and how much (tent, or between two). */
static void tapsOf(Sh* h, SpvId i, SpvId n, bool two, SpvId at[3], SpvId weight[3]) {
    SpvId half = SHR_U(i, 1);
    SpvId odd = NE_U(AND_U(i, UINT(1)), UINT(0));
    at[0] = PICK_U(EQ_U(half, UINT(0)), UINT(0), SUB_U(half, UINT(1)));
    at[1] = spvGlsl2(S, SPV_GLSL_UMin, S->uintType, half, SUB_U(n, UINT(1)));
    at[2] = spvGlsl2(S, SPV_GLSL_UMin, S->uintType, ADD_U(half, UINT(1)), SUB_U(n, UINT(1)));
    if (two) {
        weight[0] = PICK_F(odd, FLOAT(0.f), FLOAT(.25f)), weight[1] = FLOAT(.75f), weight[2] = PICK_F(odd, FLOAT(.25f), FLOAT(0.f));
    } else {
        weight[0] = PICK_F(odd, FLOAT(.125f), FLOAT(.375f)), weight[1] = FLOAT(.5f), weight[2] = PICK_F(odd, FLOAT(.375f), FLOAT(.125f));
    }
}

static SpvId upsampleOf(Sh* h, SpvId from, SpvId fw, SpvId fh, SpvId x, SpvId y, bool two) {
    SpvId ax[3], wx[3], ay[3], wy[3];
    tapsOf(h, x, fw, two, ax, wx);
    tapsOf(h, y, fh, two, ay, wy);
    SpvId out[3] = {FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)};
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) {
            SpvId p = vecAt(h, B2_CHAIN, ADD_U(from, ADD_U(MUL_U(ay[j], fw), ax[k])));
            SpvId w = MUL_F(wy[j], wx[k]);
            for (uint32_t c = 0; c < 3; ++c) out[c] = ADD_F(out[c], MUL_F(spvExtract(S, S->floatType, p, c), w));
        }
    return vec4Of(h, out[0], out[1], out[2], FLOAT(0.f));
}

uint32_t* artf2DShaderBloomUp(size_t* size) {
    Spv spv;
    Sh hh, *h = &hh;
    static const int used[] = {B2_CHAIN};
    begin(h, &spv, used, 1);
    SpvId main = spvBeginMain(S);
    SpvId id = spvLoad(S, h->uvec3, h->global);
    SpvId x = spvExtract(S, S->uintType, id, 0), y = spvExtract(S, S->uintType, id, 1);
    SpvId from = pushAt(h, 0), fw = pushAt(h, 1), fh = pushAt(h, 2), to = pushAt(h, 3), tw = pushAt(h, 4), th = pushAt(h, 5);
    SpvId merge;
    spvIfBegin(S, AND_B(LESS_U(x, tw), LESS_U(y, th)), &merge);
    {
        SpvId at = ADD_U(to, ADD_U(MUL_U(y, tw), x));
        SpvId t = upsampleOf(h, from, fw, fh, x, y, false);
        spvStore(S, vecPtr(h, B2_CHAIN, at), spvOp2(S, SPV_OP_FAdd, S->vec4, vecAt(h, B2_CHAIN, at), t));
    }
    spvIfEnd(S, merge);
    return end(h, main, size);
}

/* Push: level 1 (from, its width and height); the frame's width and height. */
uint32_t* artf2DShaderFinish(size_t* size) {
    Spv spv;
    Sh hh, *h = &hh;
    static const int used[] = {B2_GLOBALS, B2_CHAIN, B2_PIXELS};
    begin(h, &spv, used, 3);
    SpvId main = spvBeginMain(S);
    SpvId id = spvLoad(S, h->uvec3, h->global);
    SpvId x = spvExtract(S, S->uintType, id, 0), y = spvExtract(S, S->uintType, id, 1);
    SpvId from = pushAt(h, 0), fw = pushAt(h, 1), fh = pushAt(h, 2), tw = pushAt(h, 4), th = pushAt(h, 5);
    SpvId gAmbient = vecAt(h, B2_GLOBALS, UINT(G_AMBIENT)), gMode = vecAt(h, B2_GLOBALS, UINT(G_MODE)), gFlags = vecAt(h, B2_GLOBALS, UINT(G_FLAGS));
    SpvId merge;
    spvIfBegin(S, AND_B(LESS_U(x, tw), LESS_U(y, th)), &merge);
    {
        SpvId at = ADD_U(MUL_U(y, tw), x);
        SpvId p = vecAt(h, B2_CHAIN, at);
        SpvId glow = upsampleOf(h, from, fw, fh, x, y, true);
        SpvId keep = Z(gFlags), share = W(gFlags);
        SpvId v = vec4Of(h, ADD_F(MUL_F(X(p), keep), MUL_F(X(glow), share)), ADD_F(MUL_F(Y(p), keep), MUL_F(Y(glow), share)),
                         ADD_F(MUL_F(Z(p), keep), MUL_F(Z(glow), share)), W(p));
        SpvId swap = NE_U(AS_U(X(vecAt(h, B2_GLOBALS, UINT(G_OUT)))), UINT(0));
        spvStore(S, uintPtr(h, B2_PIXELS, at), tone(h, v, W(gAmbient), AS_U(X(gMode)), swap));
    }
    spvIfEnd(S, merge);
    return end(h, main, size);
}

/* What walls send on, a texel of gi each: the sum of its pixels'. Push: the frame's width and height (1, 2), gi's
 * (4, 5), its scale (6). */
uint32_t* artf2DShaderReflect(size_t* size) {
    Spv spv;
    Sh hh, *h = &hh;
    static const int used[] = {B2_REFLECT, B2_CELLS};
    begin(h, &spv, used, 2);
    SpvId main = spvBeginMain(S);
    SpvId sum = fnVar(h, h->fnVec4), row = fnVar(h, h->fnUint), column = fnVar(h, h->fnUint);
    SpvId id = spvLoad(S, h->uvec3, h->global);
    SpvId x = spvExtract(S, S->uintType, id, 0), y = spvExtract(S, S->uintType, id, 1);
    SpvId fw = pushAt(h, 1), fh = pushAt(h, 2), tw = pushAt(h, 4), th = pushAt(h, 5), scale = pushAt(h, 6);
    SpvId merge;
    spvIfBegin(S, AND_B(LESS_U(x, tw), LESS_U(y, th)), &merge);
    {
        spvStore(S, sum, vec4Of(h, FLOAT(0.f), FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)));
        Loop2D rows;
        SpvId j = loopBegin(h, &rows, row, scale);
        {
            SpvId py = ADD_U(MUL_U(y, scale), j);
            Loop2D columns;
            SpvId i = loopBegin(h, &columns, column, scale);
            {
                SpvId pxl = ADD_U(MUL_U(x, scale), i);
                SpvId in;
                spvIfBegin(S, AND_B(LESS_U(pxl, fw), LESS_U(py, fh)), &in);
                spvStore(S, sum, spvOp2(S, SPV_OP_FAdd, S->vec4, spvLoad(S, S->vec4, sum), vecAt(h, B2_REFLECT, ADD_U(MUL_U(py, fw), pxl))));
                spvIfEnd(S, in);
            }
            loopEnd(h, &columns);
        }
        loopEnd(h, &rows);
        spvStore(S, vecPtr(h, B2_CELLS, ADD_U(MUL_U(y, tw), x)), spvLoad(S, S->vec4, sum));
    }
    spvIfEnd(S, merge);
    return end(h, main, size);
}

/* gi: giShaders2d.inc, on the helpers above */
#include "2d/giShaders2d.inc"
