/* artful — a black hole's light on the GPU: a compute shader for its background pass, written as SPIR-V on the
 * library's builder (src/vulkan/spirv.h), as the library's own shaders are.
 *
 * Bindings: 11 the pass's block: the hole's ArtfHoleFrame from its 17th vec4 on    12 hdr: light, a vec4 a pixel
 *           14 map: the disc, floats (the pass's data)                             15 fluid: the gas, floats (its stream)
 *
 * The arithmetic of lens.inc, which says why it is what it is. */
#include "holeInternal.h"

#ifdef ARTFUL_VULKAN
#include "vulkan/spirv.h"

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
#define SCALE_V3(a, f) spvOp2(s, SPV_OP_VectorTimesScalar, s->vec3, a, f)
#define DOT3(a, b) spvOp2(s, SPV_OP_Dot, s->floatType, a, b)
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

static SpvId blockOf(Spv* s, SpvId array, bool writable) {
    SpvId block = spvTypeStruct(s, &array, 1);
    spvDecorate(s, block, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, block, 0, SPV_DECORATE_Offset, 0);
    if (!writable) spvMemberDecorate(s, block, 0, SPV_DECORATE_NonWritable, -1);
    return block;
}

enum { H_ER = 0, H_ETHETA = 1, H_EPHI = 2, H_RIGHT = 3, H_UP = 4, H_FORWARD = 5, H_METRIC = 6, H_PLACE = 7, H_SPIN = 8, H_FLOW = 9, H_BOOST = 10, H_DISC = 11, H_LOOK = 12, H_IMAGE = 13, H_SIZE = 14,
       H_COUNT = 15 };

#define HOLE_STEP .13089969f
#define HOLE_STEPS 192
#define HOLE_SLICES 3

typedef struct HoleResources {
    SpvId hole, hdr, map, fluid;
    SpvId ptrUniformVec4, ptrStorageVec4, ptrStorageFloat;
    SpvId zero;
} HoleResources;

static void holeDeclare(Spv* s, HoleResources* r) {
    r->zero = spvConstInt(s, 0);
    r->ptrUniformVec4 = spvTypePointer(s, SPV_STORAGE_Uniform, s->vec4);
    r->ptrStorageVec4 = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->vec4);
    r->ptrStorageFloat = spvTypePointer(s, SPV_STORAGE_StorageBuffer, s->floatType);
    r->hole = spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, ARTF_HOLE_PARAMS_AT + H_COUNT, 16), true), SPV_STORAGE_Uniform, 11);
    r->hdr = spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, 0, 16), true), SPV_STORAGE_StorageBuffer, 12);
    SpvId floatArray = spvTypeArray(s, s->floatType, 0, 4);
    r->map = spvResource(s, blockOf(s, floatArray, false), SPV_STORAGE_StorageBuffer, 14);
    r->fluid = spvResource(s, blockOf(s, floatArray, false), SPV_STORAGE_StorageBuffer, 15);
}

/* (the params start at byte 256: the renderer's part is before them) */
static SpvId holeVec(Spv* s, const HoleResources* r, uint32_t i) {
    const SpvId idx[2] = {r->zero, spvConstUint(s, ARTF_HOLE_PARAMS_AT + i)};
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
uint32_t* artfBlackHoleShader(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    HoleResources r;
    holeDeclare(s, &r);
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
    size_t words;
    uint32_t* code = spvFinish(s, &words);
    *size = words * 4;
    spvFini(s);
    return code;
}

#else

uint32_t* artfBlackHoleShader(size_t* size) {
    *size = 0;
    return NULL;
}

#endif
