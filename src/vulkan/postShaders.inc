/* artful — what comes after a frame in floats, on the GPU: post.c's passes, as compute shaders, with the same
 * arithmetic. Part of shaders.c.
 *
 * Bindings: in set 1, 0 light (four floats a pixel, premultiplied), 1 a second light, 2 the frame before's (three buffers
 * that change parts in place of being copied: vulkan.c's postSwap); in set 0, 51 depth (1 / w), 52 surface (packSurface's
 * word), 55 occlusion, 56 the grid of froxels (light the air adds, what it leaves),
 * 57 bloom's levels, one after the other, then auto exposure's state and the look-up table; 58 and 59 what the scene
 * drew, light and surface, as images; 69 motion blur's velocities, of each pixel, then the largest of each tile and of
 * its neighbours. The push constant says a pass's level. Each invocation is a pixel, 64 at a time.
 *
 * With the scene drawn smaller (ArtfRendererOptions.renderScale), depth, surface and occlusion keep the scene's size,
 * as does the light until it is upscaled; from then on the light is the frame's size, and a pixel finds depth and
 * surface at the scene's sample nearest its center, as post.c's planeAt. */

#define POST_BLOOM_LEVELS 6 /* as ARTF_POST_BLOOM_LEVELS */

typedef struct Post {
    Resources r;
    SpvId light, depth, surface, scratch, history, occlusion, fog, bloom;
    SpvId uvec3, global, push;
    SpvId width, height, widthF, heightF;  /* of the light, in this pass */
    /* of depth and surface, the scene's; once upscaled, the scene's size over the frame's, and the jitter to find a
     * pixel's sample by (none when the two are the same size); and what wasAt takes out of a pixel's place */
    SpvId sceneW, sceneH, sceneWF, sceneHF, ratioX, ratioY, mapX, mapY, ownX, ownY;
    bool frameSize; /* this pass is at the frame's size (postFrameSizes) */
} Post;

static SpvId postExp2(Spv* s, SpvId x); /* (below) */
static SpvId postBuffer(Spv* s, Post* p, SpvId array, uint32_t binding) { return spvResource(s, blockOf(s, array, true), SPV_STORAGE_StorageBuffer, binding); }
/* Light, the second light and the history: in set 1, by the part each plays (the renderer gives the parts to its three
 * buffers in turn, in place of copying). Each pixel's four as halves, in two words (postLight, postLightStore): half
 * the memory of floats, and 11 bits of each, finer than what a screen shows of it. */
static SpvId postRole(Spv* s, Post* p, uint32_t part) {
    const SpvId id = spvVariable(s, spvTypePointer(s, SPV_STORAGE_StorageBuffer, blockOf(s, p->r.uintArray, true)), SPV_STORAGE_StorageBuffer);
    spvDecorate(s, id, SPV_DECORATE_DescriptorSet, 1);
    spvDecorate(s, id, SPV_DECORATE_Binding, (int)part);
    return id;
}

static void postDeclare(Spv* s, Post* p) {
    memset(p, 0, sizeof *p);
    declare(s, &p->r, false);
    SpvId floats = p->r.floatArray;
    p->light = postRole(s, p, 0);
    p->depth = postBuffer(s, p, floats, 51);
    p->surface = postBuffer(s, p, p->r.uintArray, 52);
    p->scratch = postRole(s, p, 1);
    p->history = postRole(s, p, 2);
    p->occlusion = postBuffer(s, p, floats, 55);
    p->fog = postBuffer(s, p, p->r.vec4Array, 56);
    p->bloom = postBuffer(s, p, p->r.vec4Array, 57);
    p->uvec3 = spvTypeVector(s, s->uintType, 3);
    p->global = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, p->uvec3), SPV_STORAGE_Input);
    spvDecorate(s, p->global, SPV_DECORATE_BuiltIn, SPV_BUILTIN_GlobalInvocationId);
    SpvId pushBlock = spvTypeStruct(s, &s->uintType, 1);
    spvDecorate(s, pushBlock, SPV_DECORATE_Block, -1);
    spvMemberDecorate(s, pushBlock, 0, SPV_DECORATE_Offset, 0);
    p->push = spvVariable(s, spvTypePointer(s, SPV_STORAGE_PushConstant, pushBlock), SPV_STORAGE_PushConstant);
}

/* After spvBeginMain: the scene's size. */
static void postSizes(Spv* s, Post* p) {
    p->widthF = W(frameVec(s, &p->r, F_FOG));
    p->heightF = W(frameVec(s, &p->r, F_EYE + 1));
    p->width = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, p->widthF);
    p->height = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, p->heightF);
    p->sceneW = p->width;
    p->sceneH = p->height;
    p->sceneWF = p->widthF;
    p->sceneHF = p->heightF;
    SpvId jitter = frameVec(s, &p->r, F_POST + 2);
    p->ownX = Z(jitter);
    p->ownY = W(jitter);
}

/* Or the frame's: for what comes after upscaling, which finds depth and surface at the scene's size. */
static void postFrameSizes(Spv* s, Post* p) {
    postSizes(s, p);
    SpvId output = frameVec(s, &p->r, F_OUTPUT);
    p->frameSize = true;
    p->widthF = X(output);
    p->heightF = Y(output);
    p->width = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, p->widthF);
    p->height = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, p->heightF);
    p->ratioX = DIV_F(p->sceneWF, p->widthF);
    p->ratioY = DIV_F(p->sceneHF, p->heightF);
    SpvId scaled = spvOp2(s, 166 /* OpLogicalOr */, s->boolType, spvOp2(s, 182 /* OpFOrdNotEqual */, s->boolType, p->sceneWF, p->widthF), spvOp2(s, 182, s->boolType, p->sceneHF, p->heightF));
    SpvId jitter = frameVec(s, &p->r, F_POST + 2);
    p->mapX = PICK_F(scaled, Z(jitter), FLOAT(0.f));
    p->mapY = PICK_F(scaled, W(jitter), FLOAT(0.f));
    p->ownX = PICK_F(scaled, FLOAT(0.f), Z(jitter));
    p->ownY = PICK_F(scaled, FLOAT(0.f), W(jitter));
}

static SpvId postPush(Spv* s, Post* p) { return spvLoad(s, s->uintType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_PushConstant, s->uintType), p->push, &p->r.zero, 1)); }

static SpvId postPtr(Spv* s, Post* p, SpvId buffer, SpvId type, SpvId i) {
    const SpvId idx[2] = {p->r.zero, i};
    return spvAccess(s, spvTypePointer(s, SPV_STORAGE_StorageBuffer, type), buffer, idx, 2);
}
static SpvId postIsRole(const Post* p, SpvId buffer) { return buffer == p->light || buffer == p->scratch || buffer == p->history; }
/* A pixel of a part (light, the second light, the history): its two words, as halves. */
static SpvId postLight(Spv* s, Post* p, SpvId buffer, SpvId i) {
    SpvId at = spvOp2(s, SPV_OP_IMul, s->uintType, i, spvConstUint(s, 2));
    SpvId lo = spvLoad(s, s->uintType, postPtr(s, p, buffer, s->uintType, at));
    SpvId hi = spvLoad(s, s->uintType, postPtr(s, p, buffer, s->uintType, spvOp2(s, SPV_OP_IAdd, s->uintType, at, spvConstUint(s, 1))));
    SpvId a = spvGlsl1(s, 62 /* UnpackHalf2x16 */, s->vec2, lo), b = spvGlsl1(s, 62, s->vec2, hi);
    const SpvId parts[4] = {spvExtract(s, s->floatType, a, 0), spvExtract(s, s->floatType, a, 1), spvExtract(s, s->floatType, b, 0), spvExtract(s, s->floatType, b, 1)};
    return spvCompose(s, s->vec4, parts, 4);
}
static void postLightStore(Spv* s, Post* p, SpvId buffer, SpvId i, SpvId v) {
    SpvId at = spvOp2(s, SPV_OP_IMul, s->uintType, i, spvConstUint(s, 2));
    const SpvId xy[2] = {spvExtract(s, s->floatType, v, 0), spvExtract(s, s->floatType, v, 1)}, zw[2] = {spvExtract(s, s->floatType, v, 2), spvExtract(s, s->floatType, v, 3)};
    spvStore(s, postPtr(s, p, buffer, s->uintType, at), spvGlsl1(s, 58 /* PackHalf2x16 */, s->uintType, spvCompose(s, s->vec2, xy, 2)));
    spvStore(s, postPtr(s, p, buffer, s->uintType, spvOp2(s, SPV_OP_IAdd, s->uintType, at, spvConstUint(s, 1))), spvGlsl1(s, 58, s->uintType, spvCompose(s, s->vec2, zw, 2)));
}
static SpvId postVec4(Spv* s, Post* p, SpvId buffer, SpvId i) { return postIsRole(p, buffer) ? postLight(s, p, buffer, i) : spvLoad(s, s->vec4, postPtr(s, p, buffer, s->vec4, i)); }
static SpvId postFloat(Spv* s, Post* p, SpvId buffer, SpvId i) { return spvLoad(s, s->floatType, postPtr(s, p, buffer, s->floatType, i)); }
static SpvId postUint(Spv* s, Post* p, SpvId buffer, SpvId i) { return spvLoad(s, s->uintType, postPtr(s, p, buffer, s->uintType, i)); }

#define P_LESS_U(a, b) spvOp2(s, SPV_OP_ULessThan, s->boolType, a, b)
#define P_MORE(a, b) spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, a, b)
#define P_LESS(a, b) spvOp2(s, SPV_OP_FOrdLessThan, s->boolType, a, b)
#define P_AND(a, b) spvOp2(s, SPV_OP_LogicalAnd, s->boolType, a, b)
#define P_OR(a, b) spvOp2(s, 166 /* OpLogicalOr */, s->boolType, a, b)
#define P_NOT(a) spvOp1(s, SPV_OP_LogicalNot, s->boolType, a)
#define P_CLAMP(v, lo, hi) MIN_F(MAX_F(v, lo), hi)
#define P_TOU(f) spvOp1(s, SPV_OP_ConvertFToU, s->uintType, f)
#define P_MIX3(a, b, t) ADD_V3(a, SCALE_V3(SUB_V3(b, a), t))
#define SUB_V4(a, b) spvOp2(s, SPV_OP_FSub, s->vec4, a, b)

/* The pixel's index, x and y; false past the frame. */
static SpvId postBegin(Spv* s, Post* p, SpvId* i, SpvId* x, SpvId* y, SpvId* merge, SpvId width, SpvId height) {
    *i = spvExtract(s, s->uintType, spvLoad(s, p->uvec3, p->global), 0);
    *x = spvOp2(s, SPV_OP_UMod, s->uintType, *i, width);
    *y = spvOp2(s, SPV_OP_UDiv, s->uintType, *i, width);
    spvIfBegin(s, P_LESS_U(*i, MUL_U(width, height)), merge);
    return *i;
}

/* Where a pixel's depth and surface are (whole numbers, clamped): itself, or at the frame's size, the scene's pixel
 * whose sample is nearest its center. */
static SpvId postPlane(Spv* s, Post* p, SpvId xf, SpvId yf) {
    if (!p->frameSize) {
        SpvId x = P_TOU(P_CLAMP(xf, FLOAT(0.f), SUB_F(p->widthF, FLOAT(1.f)))), y = P_TOU(P_CLAMP(yf, FLOAT(0.f), SUB_F(p->heightF, FLOAT(1.f))));
        return ADD_U(MUL_U(y, p->width), x);
    }
    SpvId x = FLOOR_F(ADD_F(MUL_F(ADD_F(xf, FLOAT(.5f)), p->ratioX), p->mapX)), y = FLOOR_F(SUB_F(MUL_F(ADD_F(yf, FLOAT(.5f)), p->ratioY), p->mapY));
    x = P_TOU(P_CLAMP(x, FLOAT(0.f), SUB_F(p->sceneWF, FLOAT(1.f))));
    y = P_TOU(P_CLAMP(y, FLOAT(0.f), SUB_F(p->sceneHF, FLOAT(1.f))));
    return ADD_U(MUL_U(y, p->sceneW), x);
}

/* Distance along the view from what the depth buffer holds: 1 / w, or an orthographic view's depth, as post.c's
 * distanceAt reads it; 0 where nothing is. */
static SpvId postAlong(Spv* s, Post* p, SpvId d) {
    SpvId depthRow = frameVec(s, &p->r, F_ORTHO);
    SpvId along = PICK_F(orthoOn(s, &p->r), SUB_F(W(depthRow), MUL_F(d, Z(depthRow))), DIV_F(FLOAT(1.f), MAX_F(d, FLOAT(1e-30f))));
    return PICK_F(P_MORE(d, FLOAT(0.f)), along, FLOAT(0.f));
}

/* Distance along the view at a pixel (whole numbers, clamped); 0 where nothing is. */
static SpvId postDistance(Spv* s, Post* p, SpvId xf, SpvId yf) {
    return postAlong(s, p, postFloat(s, p, p->depth, postPlane(s, p, xf, yf)));
}

/* The world at a pixel's center, d along the view: on the ray from the eye, or in an orthographic view, from its
 * place on the view's plane. */
static SpvId postWorld(Spv* s, Post* p, SpvId xf, SpvId yf, SpvId d) {
    SpvId px = SUB_F(MUL_F(DIV_F(ADD_F(xf, FLOAT(.5f)), p->widthF), FLOAT(2.f)), FLOAT(1.f));
    SpvId py = SUB_F(FLOAT(1.f), MUL_F(DIV_F(ADD_F(yf, FLOAT(.5f)), p->heightF), FLOAT(2.f)));
    /* (by a 0 or a 1 of the view, and not by a choice of results, so that a perspective's arithmetic is as it was, to
     * the bit: orthographic, the place moves across, and the way spreads no more) */
    SpvId ortho = orthoOn(s, &p->r), across = PICK_F(ortho, FLOAT(1.f), FLOAT(0.f)), spread = PICK_F(ortho, FLOAT(0.f), FLOAT(1.f));
    SpvId right = spvShuffle3(s, frameVec(s, &p->r, F_EYE)), up = spvShuffle3(s, frameVec(s, &p->r, F_EYE + 1));
    SpvId from = ADD_V3(ADD_V3(spvShuffle3(s, frameVec(s, &p->r, F_CAMERA)), SCALE_V3(right, MUL_F(px, across))), SCALE_V3(up, MUL_F(py, across)));
    SpvId dir = ADD_V3(ADD_V3(spvShuffle3(s, frameVec(s, &p->r, F_EYE + 2)), SCALE_V3(right, MUL_F(px, spread))), SCALE_V3(up, MUL_F(py, spread)));
    return ADD_V3(from, SCALE_V3(dir, d));
}

/* Where a point falls through a matrix (four rows of the frame, by columns), in pixels; w along the view; *ok: in
 * front of the eye. */
static void postProject(Spv* s, Post* p, uint32_t matrix, SpvId q, SpvId* sx, SpvId* sy, SpvId* w, SpvId* ok) {
    SpvId q4 = vec4Of(s, q, FLOAT(1.f));
    SpvId c0 = frameVec(s, &p->r, matrix), c1 = frameVec(s, &p->r, matrix + 1), c2 = frameVec(s, &p->r, matrix + 2), c3 = frameVec(s, &p->r, matrix + 3);
    SpvId clip = ADD_V4(ADD_V4(SCALE_V4(c0, X(q4)), SCALE_V4(c1, Y(q4))), ADD_V4(SCALE_V4(c2, Z(q4)), c3));
    SpvId cw = W(clip), depthRow = frameVec(s, &p->r, F_ORTHO);
    /* (orthographic: w is 1, and how far along the view comes from the depth) */
    *w = PICK_F(orthoOn(s, &p->r), SUB_F(W(depthRow), MUL_F(Z(clip), Z(depthRow))), cw);
    *ok = P_MORE(cw, FLOAT(1e-5f));
    SpvId inv = DIV_F(FLOAT(1.f), MAX_F(cw, FLOAT(1e-5f)));
    *sx = SUB_F(MUL_F(ADD_F(MUL_F(MUL_F(X(clip), inv), FLOAT(.5f)), FLOAT(.5f)), p->widthF), FLOAT(.5f));
    *sy = SUB_F(MUL_F(SUB_F(FLOAT(.5f), MUL_F(MUL_F(Y(clip), inv), FLOAT(.5f))), p->heightF), FLOAT(.5f));
}

/* Bilinear in a buffer of vec4s, start, w x h, at pixel coordinates. */
static SpvId postTap(Spv* s, Post* p, SpvId buffer, SpvId start, SpvId w, SpvId h, SpvId x, SpvId y) {
    SpvId wf = TO_F(w), hf = TO_F(h);
    x = P_CLAMP(x, FLOAT(0.f), SUB_F(wf, FLOAT(1.f)));
    y = P_CLAMP(y, FLOAT(0.f), SUB_F(hf, FLOAT(1.f)));
    SpvId bx = FLOOR_F(x), by = FLOOR_F(y), fx = SUB_F(x, bx), fy = SUB_F(y, by);
    SpvId x0 = P_TOU(bx), y0 = P_TOU(by);
    SpvId x1 = spvGlsl2(s, 38 /* UMin */, s->uintType, ADD_U(x0, UINT(1)), SUB_U(w, UINT(1))), y1 = spvGlsl2(s, 38, s->uintType, ADD_U(y0, UINT(1)), SUB_U(h, UINT(1)));
    SpvId a = postVec4(s, p, buffer, ADD_U(start, ADD_U(MUL_U(y0, w), x0))), b = postVec4(s, p, buffer, ADD_U(start, ADD_U(MUL_U(y0, w), x1)));
    SpvId c = postVec4(s, p, buffer, ADD_U(start, ADD_U(MUL_U(y1, w), x0))), d = postVec4(s, p, buffer, ADD_U(start, ADD_U(MUL_U(y1, w), x1)));
    SpvId top = ADD_V4(a, SCALE_V4(SUB_V4(b, a), fx)), bottom = ADD_V4(c, SCALE_V4(SUB_V4(d, c), fx));
    return ADD_V4(top, SCALE_V4(SUB_V4(bottom, top), fy));
}

/* post.c's tapSharp: the history at (x, y) by Catmull-Rom's cubic over the 4 x 4 around it, the sides held; never
 * below 0. The same arithmetic, in the same order. */
static SpvId postTapSharp(Spv* s, Post* p, SpvId buffer, SpvId w, SpvId h, SpvId x, SpvId y) {
    SpvId wf = TO_F(w), hf = TO_F(h);
    x = P_CLAMP(x, FLOAT(0.f), SUB_F(wf, FLOAT(1.f)));
    y = P_CLAMP(y, FLOAT(0.f), SUB_F(hf, FLOAT(1.f)));
    SpvId x0 = P_TOU(x), y0 = P_TOU(y);
    SpvId wx[4], wy[4];
    for (int axis = 0; axis < 2; ++axis) {
        SpvId t = axis ? SUB_F(y, TO_F(y0)) : SUB_F(x, TO_F(x0)), t2 = MUL_F(t, t), t3 = MUL_F(t2, t);
        SpvId* out = axis ? wy : wx;
        out[0] = MUL_F(FLOAT(.5f), SUB_F(SUB_F(MUL_F(FLOAT(2.f), t2), t3), t));
        out[1] = MUL_F(FLOAT(.5f), ADD_F(SUB_F(MUL_F(FLOAT(3.f), t3), MUL_F(FLOAT(5.f), t2)), FLOAT(2.f)));
        out[2] = MUL_F(FLOAT(.5f), ADD_F(SUB_F(MUL_F(FLOAT(4.f), t2), MUL_F(FLOAT(3.f), t3)), t));
        out[3] = MUL_F(FLOAT(.5f), SUB_F(t3, t2));
    }
    SpvId cols[4];
    for (int i = 0; i < 4; ++i) {
        /* (x0 - 1 + i, held within 0 and w - 1, as signed) */
        SpvId c = spvOp2(s, SPV_OP_IAdd, s->intType, spvOp1(s, SPV_OP_Bitcast, s->intType, x0), spvConstInt(s, i - 1));
        cols[i] = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvGlsl2(s, 39 /* SMin */, s->intType, spvGlsl2(s, 42 /* SMax */, s->intType, c, spvConstInt(s, 0)), spvOp1(s, SPV_OP_Bitcast, s->intType, SUB_U(w, UINT(1)))));
    }
    SpvId total = 0;
    for (int j = 0; j < 4; ++j) {
        SpvId r = spvOp2(s, SPV_OP_IAdd, s->intType, spvOp1(s, SPV_OP_Bitcast, s->intType, y0), spvConstInt(s, j - 1));
        r = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvGlsl2(s, 39, s->intType, spvGlsl2(s, 42, s->intType, r, spvConstInt(s, 0)), spvOp1(s, SPV_OP_Bitcast, s->intType, SUB_U(h, UINT(1)))));
        SpvId at = MUL_U(r, w);
        SpvId across = ADD_V4(ADD_V4(ADD_V4(SCALE_V4(postVec4(s, p, buffer, ADD_U(at, cols[0])), wx[0]), SCALE_V4(postVec4(s, p, buffer, ADD_U(at, cols[1])), wx[1])),
                                     SCALE_V4(postVec4(s, p, buffer, ADD_U(at, cols[2])), wx[2])),
                              SCALE_V4(postVec4(s, p, buffer, ADD_U(at, cols[3])), wx[3]));
        total = j ? ADD_V4(total, SCALE_V4(across, wy[j])) : SCALE_V4(across, wy[j]);
    }
    return spvGlsl2(s, SPV_GLSL_FMax, s->vec4, total, vec4Of(s, same3(s, 0.f), FLOAT(0.f)));
}
/* The history where a pixel was: bilinear (postTap), or with ArtfScreenDesc.sharpHistory (the frame's temporal row, 2)
 * by postTapSharp, read only then. */
static SpvId postHistoryTap(Spv* s, Post* p, SpvId x, SpvId y) {
    SpvId plain = postTap(s, p, p->history, UINT(0), p->width, p->height, x, y);
    SpvId before = s->block, merge;
    spvIfBegin(s, P_MORE(W(frameVec(s, &p->r, F_POST + 1)), FLOAT(1.5f)), &merge);
    SpvId sharp = postTapSharp(s, p, p->history, p->width, p->height, x, y);
    SpvId read = s->block;
    spvIfEnd(s, merge);
    return spvPhi(s, s->vec4, sharp, read, plain, before);
}

static SpvId postNormal(Spv* s, SpvId word) {
#define PN_F(v) spvConstFloat(s, v)
    SpvId a = SUB_F(MUL_F(DIV_F(TO_F(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, word, UINT(511))), PN_F(511.f)), PN_F(2.f)), PN_F(1.f));
    SpvId b = SUB_F(MUL_F(DIV_F(TO_F(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, word, UINT(9)), UINT(511))), PN_F(511.f)), PN_F(2.f)), PN_F(1.f));
    SpvId z = SUB_F(SUB_F(PN_F(1.f), ABS_F(a)), ABS_F(b));
    SpvId below = P_LESS(z, PN_F(0.f));
    SpvId sa = PICK_F(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, a, PN_F(0.f)), PN_F(1.f), PN_F(-1.f));
    SpvId sb = PICK_F(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, b, PN_F(0.f)), PN_F(1.f), PN_F(-1.f));
    SpvId fa = MUL_F(SUB_F(PN_F(1.f), ABS_F(b)), sa), fb = MUL_F(SUB_F(PN_F(1.f), ABS_F(a)), sb);
    return NORMALIZE(vec3Of(s, PICK_F(below, fa, a), PICK_F(below, fb, b), z));
#undef PN_F
}
static SpvId postField(Spv* s, SpvId word, uint32_t shift, uint32_t mask, float top) {
    return DIV_F(TO_F(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, word, UINT(shift)), UINT(mask))), FLOAT(top));
}

static SpvId postHash(Spv* s, SpvId x) {
    x = MUL_U(spvOp2(s, 198 /* OpBitwiseXor */, s->uintType, x, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, x, UINT(16))), UINT(0x7FEB352Du));
    x = MUL_U(spvOp2(s, 198, s->uintType, x, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, x, UINT(15))), UINT(0x846CA68Bu));
    return spvOp2(s, 198, s->uintType, x, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, x, UINT(16)));
}

#define POST_MAIN(name)                                                      \
    uint32_t* name(size_t* size) {                                           \
        Spv b, *s = &b;                                                      \
        spvInit(s);                                                          \
        Post post, *p = &post;                                               \
        postDeclare(s, p);
#define POST_FINISH()                                                        \
        spvIfEnd(s, merge);                                                  \
        spvEndMain(s);                                                       \
        spvEntryPoint(s, SPV_MODEL_GLCompute, main, &p->global, 1);          \
        spvExecutionMode(s, main, 17, 64, 1, 1, 3);                          \
        return finish(s, size);                                              \
    }

/* ---- what the scene drew, into the buffers: with several samples a pixel, the light as they resolved to, and the
 * first sample's depth and surface, as on the CPU ---- */
uint32_t* artfShaderPostGather(size_t* size, int samples) {
    Spv b, *s = &b;
    spvInit(s);
    Post post, *p = &post;
    postDeclare(s, p);
    const bool many = samples > 1;
    SpvId floatImage, floatSampled = spvTypeSampledImage2D(s, &floatImage);
    SpvId uintImage, uintSampled = many ? spvTypeSampledImage2DMSOf(s, s->uintType, &uintImage) : spvTypeSampledImage2DOf(s, s->uintType, &uintImage);
    SpvId depthType, depthSampled = many ? spvTypeSampledImage2DMS(s, &depthType) : floatSampled;
    if (!many) depthType = floatImage;
    SpvId color = spvResource(s, floatSampled, SPV_STORAGE_UniformConstant, 58);
    SpvId surfaceImage = spvResource(s, uintSampled, SPV_STORAGE_UniformConstant, 59);
    SpvId depthImage = spvResource(s, depthSampled, SPV_STORAGE_UniformConstant, 8);
    SpvId ivec2 = spvTypeVector(s, s->intType, 2), uvec4 = spvTypeVector(s, s->uintType, 4);
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    const SpvId parts[2] = {spvOp1(s, SPV_OP_Bitcast, s->intType, x), spvOp1(s, SPV_OP_Bitcast, s->intType, y)};
    SpvId at = spvCompose(s, ivec2, parts, 2);
    /* (the push constant: 1 for depth alone, after what is solid and before layers write theirs; 0 for the rest) */
    SpvId depthAlone = spvOp2(s, SPV_OP_INotEqual, s->boolType, postPush(s, p), UINT(0)), rest, depthOnly;
    spvIfBegin(s, P_NOT(depthAlone), &rest);
    postLightStore(s, p, p->light, i, spvImageFetchAs(s, s->vec4, floatImage, spvLoad(s, floatSampled, color), at));
    SpvId word = many ? spvImageFetchSampleAs(s, uvec4, uintImage, spvLoad(s, uintSampled, surfaceImage), at, spvConstInt(s, 0)) : spvImageFetchAs(s, uvec4, uintImage, spvLoad(s, uintSampled, surfaceImage), at);
    spvStore(s, postPtr(s, p, p->surface, s->uintType, i), spvExtract(s, s->uintType, word, 0));
    /* (the instance that shows, after all the words: when something moved, for what follows it) */
    SpvId movedMerge;
    spvIfBegin(s, P_OR(P_MORE(Y(frameVec(s, &p->r, F_CULL)), FLOAT(0.f)), P_MORE(X(frameVec(s, &p->r, F_MORE)), FLOAT(0.f))), &movedMerge);
    spvStore(s, postPtr(s, p, p->surface, s->uintType, ADD_U(MUL_U(p->width, p->height), i)), spvExtract(s, s->uintType, word, 1));
    spvIfEnd(s, movedMerge);
    spvIfEnd(s, rest);
    spvIfBegin(s, depthAlone, &depthOnly);
    /* (reversed depth is near / distance: 1 / w is depth over near; an orthographic view's is kept as it is) */
    SpvId z = X(many ? spvImageFetchSampleAs(s, s->vec4, depthType, spvLoad(s, depthSampled, depthImage), at, spvConstInt(s, 0)) : spvImageFetchAs(s, s->vec4, floatImage, spvLoad(s, floatSampled, depthImage), at));
    spvStore(s, postPtr(s, p, p->depth, s->floatType, i), PICK_F(orthoOn(s, &p->r), z, DIV_F(z, Z(frameVec(s, &p->r, F_PROJECTION)))));
    spvIfEnd(s, depthOnly);
POST_FINISH()

/* ---- ambient occlusion: as occlusionRow and occlusionBlurRow ---- */
POST_MAIN(artfShaderPostOcclusion)
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId d = postDistance(s, p, xf, yf), word = postUint(s, p, p->surface, i);
    SpvId radius = W(frameVec(s, &p->r, F_POST));
    SpvId n = postNormal(s, word), at = postWorld(s, p, xf, yf, d);
    SpvId bigX = P_LESS(ABS_F(X(n)), FLOAT(.9f));
    SpvId t0 = vec3Of(s, PICK_F(bigX, FLOAT(1.f), FLOAT(0.f)), PICK_F(bigX, FLOAT(0.f), FLOAT(1.f)), FLOAT(0.f));
    SpvId t = NORMALIZE(SUB_V3(t0, SCALE_V3(n, DOT3(t0, n))));
    SpvId bt = spvGlsl2(s, 68 /* Cross */, s->vec3, n, t);
    static const float cosines[16] = {1, .9238795f, .7071068f, .3826834f, 0, -.3826834f, -.7071068f, -.9238795f, -1, -.9238795f, -.7071068f, -.3826834f, 0, .3826834f, .7071068f, .9238795f};
    static const float kernel[8][3] = {{.30f, .10f, .25f}, {-.20f, .45f, .35f}, {-.55f, -.15f, .20f}, {.10f, -.60f, .40f},
                                       {.70f, .35f, .45f}, {-.35f, .75f, .30f}, {-.80f, -.40f, .35f}, {.25f, -.85f, .40f}};
    /* the pattern's turn, from a table: (x & 3) * 4 + (y & 3) */
    SpvId step = ADD_U(MUL_U(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, x, UINT(3)), UINT(4)), spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, y, UINT(3)));
    SpvId c = FLOAT(cosines[15]), sn = FLOAT(cosines[(15 + 12) & 15]);
    for (int k = 14; k >= 0; --k) {
        SpvId is = spvOp2(s, 170 /* OpIEqual */, s->boolType, step, UINT((uint32_t)k));
        c = PICK_F(is, FLOAT(cosines[k]), c);
        sn = PICK_F(is, FLOAT(cosines[(k + 12) & 15]), sn);
    }
    SpvId hidden = FLOAT(0.f);
    for (int k = 0; k < 8; ++k) {
        SpvId kx = SUB_F(MUL_F(FLOAT(kernel[k][0]), c), MUL_F(FLOAT(kernel[k][1]), sn)), ky = ADD_F(MUL_F(FLOAT(kernel[k][0]), sn), MUL_F(FLOAT(kernel[k][1]), c));
        SpvId q = ADD_V3(at, SCALE_V3(ADD_V3(ADD_V3(SCALE_V3(t, kx), SCALE_V3(bt, ky)), SCALE_V3(n, FLOAT(kernel[k][2]))), radius));
        SpvId sx, sy, w, ok;
        postProject(s, p, F_VIEWPROJ, q, &sx, &sy, &w, &ok);
        SpvId qx = FLOOR_F(ADD_F(sx, FLOAT(.5f))), qy = FLOOR_F(ADD_F(sy, FLOAT(.5f)));
        SpvId inside = P_AND(P_AND(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, qx, FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, qy, FLOAT(0.f))), P_AND(P_LESS(qx, p->widthF), P_LESS(qy, p->heightF)));
        SpvId there = postDistance(s, p, qx, qy), gap = SUB_F(w, there);
        SpvId shades = P_AND(P_AND(ok, inside), P_AND(P_MORE(there, FLOAT(0.f)), P_AND(P_MORE(gap, MUL_F(FLOAT(.02f), radius)), P_LESS(gap, radius))));
        hidden = ADD_F(hidden, PICK_F(shades, FLOAT(1.f), FLOAT(0.f)));
    }
    SpvId open = SUB_F(FLOAT(1.f), DIV_F(hidden, FLOAT(8.f)));
    SpvId none = P_OR(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, d, FLOAT(0.f)), spvOp2(s, 170, s->boolType, word, UINT(0)));
    spvStore(s, postPtr(s, p, p->occlusion, s->floatType, i), PICK_F(none, FLOAT(1.f), open));
POST_FINISH()

POST_MAIN(artfShaderPostOcclusionBlur)
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId d = postDistance(s, p, xf, yf);
    SpvId sum = FLOAT(0.f), weight = FLOAT(0.f);
    for (int dy = -2; dy < 2; ++dy)
        for (int dx = -2; dx < 2; ++dx) {
            SpvId qx = P_CLAMP(ADD_F(xf, FLOAT((float)dx)), FLOAT(0.f), SUB_F(p->widthF, FLOAT(1.f))), qy = P_CLAMP(ADD_F(yf, FLOAT((float)dy)), FLOAT(0.f), SUB_F(p->heightF, FLOAT(1.f)));
            SpvId there = postDistance(s, p, qx, qy);
            SpvId w = PICK_F(P_LESS(ABS_F(SUB_F(there, d)), MUL_F(FLOAT(.05f), d)), FLOAT(1.f), FLOAT(0.f));
            sum = ADD_F(sum, MUL_F(postFloat(s, p, p->occlusion, ADD_U(MUL_U(P_TOU(qy), p->width), P_TOU(qx))), w));
            weight = ADD_F(weight, w);
        }
    SpvId open = PICK_F(P_MORE(weight, FLOAT(0.f)), DIV_F(sum, MAX_F(weight, FLOAT(1e-6f))), FLOAT(1.f));
    SpvId around = postField(s, postUint(s, p, p->surface, i), 27, 31, 31.f);
    SpvId keep = SUB_F(FLOAT(1.f), MUL_F(MUL_F(around, SUB_F(FLOAT(1.f), open)), Z(frameVec(s, &p->r, F_POST))));
    keep = PICK_F(P_MORE(d, FLOAT(0.f)), keep, FLOAT(1.f));
    SpvId light = postLight(s, p, p->light, i);
    postLightStore(s, p, p->light, i, vec4Of(s, SCALE_V3(spvShuffle3(s, light), keep), W(light)));
POST_FINISH()

/* ---- reflections: as reflectionRow, into the second light ---- */
POST_MAIN(artfShaderPostReflection)
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    /* (at half the resolution, as reflectionRow: into bloom's buffer, before bloom needs it) */
    SpvId hw = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->width, UINT(1)), UINT(2)), hh = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->height, UINT(1)), UINT(2));
    SpvId half, halfX, halfY, merge;
    postBegin(s, p, &half, &halfX, &halfY, &merge, hw, hh);
    SpvId x = spvGlsl2(s, 38 /* UMin */, s->uintType, MUL_U(halfX, UINT(2)), SUB_U(p->width, UINT(1))), y = spvGlsl2(s, 38, s->uintType, MUL_U(halfY, UINT(2)), SUB_U(p->height, UINT(1)));
    SpvId i = ADD_U(MUL_U(y, p->width), x);
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId mine = postVec4(s, p, p->light, i);
    SpvId word = postUint(s, p, p->surface, i), d = postDistance(s, p, xf, yf);
    SpvId rough = postField(s, word, 18, 31, 31.f), metal = postField(s, word, 23, 15, 15.f);
    SpvId n = postNormal(s, word), at = postWorld(s, p, xf, yf, d);
    SpvId v = NORMALIZE(SUB_V3(at, eyeOf(s, &p->r, at)));
    SpvId vn = DOT3(v, n);
    SpvId r = SUB_V3(v, SCALE_V3(n, MUL_F(FLOAT(2.f), vn)));
    SpvId away = DOT3(r, spvShuffle3(s, frameVec(s, &p->r, F_EYE + 2)));
    /* (the march only where something smooth enough faces away from the eye: everything else is copied) */
    spvStore(s, postPtr(s, p, p->bloom, s->vec4, half), vec4Of(s, same3(s, 0.f), FLOAT(0.f)));
    SpvId eligible = P_AND(P_AND(P_MORE(d, FLOAT(0.f)), spvOp2(s, SPV_OP_INotEqual, s->boolType, word, UINT(0))), P_AND(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, rough, FLOAT(.6f)), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, away, FLOAT(-.2f))));
    eligible = P_AND(eligible, P_MORE(X(frameVec(s, &p->r, F_POST + 1)), FLOAT(0.f)));
    SpvId marching;
    spvIfBegin(s, eligible, &marching);
    SpvId stride = ADD_F(MUL_F(d, FLOAT(.04f)), FLOAT(.02f)), thickness = ADD_F(MUL_F(d, FLOAT(.03f)), FLOAT(.05f));
    /* as reflectionRow: the ray across the frame as a line, along which 1 / w goes evenly */
    SpvId sx0, sy0, w0, ok0, sx1, sy1, w1, ok1;
    postProject(s, p, F_VIEWPROJ, at, &sx0, &sy0, &w0, &ok0);
    SpvId wDrop = spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(r, spvShuffle3(s, frameVec(s, &p->r, F_EYE + 2))));
    SpvId reach = MUL_F(stride, FLOAT(70.f));
    /* (not past the eye; orthographic: not past near) */
    SpvId ortho = orthoOn(s, &p->r), depthRow = frameVec(s, &p->r, F_ORTHO);
    SpvId room = SUB_F(w0, PICK_F(ortho, SUB_F(W(depthRow), Z(depthRow)), FLOAT(0.f)));
    reach = PICK_F(P_MORE(wDrop, FLOAT(0.f)), MIN_F(reach, DIV_F(MUL_F(room, FLOAT(.9f)), MAX_F(wDrop, FLOAT(1e-20f)))), reach);
    postProject(s, p, F_VIEWPROJ, ADD_V3(at, SCALE_V3(r, reach)), &sx1, &sy1, &w1, &ok1);
    SpvId inv0 = DIV_F(FLOAT(1.f), w0), invStep = SUB_F(DIV_F(FLOAT(1.f), MAX_F(w1, FLOAT(1e-5f))), inv0);
    SpvId jitter = DIV_F(TO_F(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, postHash(s, i), UINT(255))), FLOAT(256.f));
    SpvId found = spvConstBool(s, false), before = spvConstBool(s, true), stopped = P_NOT(ok1), hx = FLOAT(0.f), hy = FLOAT(0.f);
    for (int step = 1; step <= 24; ++step) {
        SpvId share = DIV_F(ADD_F(FLOAT((float)step), jitter), FLOAT(25.f)), t = MUL_F(share, share);
        SpvId sx = ADD_F(sx0, MUL_F(SUB_F(sx1, sx0), t)), sy = ADD_F(sy0, MUL_F(SUB_F(sy1, sy0), t));
        /* (orthographic: w itself goes evenly) */
        SpvId w = PICK_F(ortho, ADD_F(w0, MUL_F(SUB_F(w1, w0), t)), DIV_F(FLOAT(1.f), ADD_F(inv0, MUL_F(invStep, t))));
        SpvId qx = FLOOR_F(ADD_F(sx, FLOAT(.5f))), qy = FLOOR_F(ADD_F(sy, FLOAT(.5f)));
        SpvId inside = P_AND(P_AND(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, qx, FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, qy, FLOAT(0.f))), P_AND(P_LESS(qx, p->widthF), P_LESS(qy, p->heightF)));
        stopped = P_OR(stopped, P_NOT(inside));
        SpvId there = postDistance(s, p, qx, qy);
        SpvId behind = P_AND(P_MORE(there, FLOAT(0.f)), P_MORE(w, there));
        SpvId moved = ADD_F(ABS_F(SUB_F(sx, xf)), ABS_F(SUB_F(sy, yf)));
        SpvId hit = P_AND(P_AND(P_NOT(found), P_NOT(stopped)), P_AND(P_AND(behind, before), P_AND(P_LESS(SUB_F(w, there), thickness), P_MORE(moved, FLOAT(2.f)))));
        hx = PICK_F(hit, sx, hx);
        hy = PICK_F(hit, sy, hy);
        found = P_OR(found, hit);
        before = P_NOT(behind);
    }
    SpvId edge = MIN_F(MIN_F(hx, SUB_F(SUB_F(p->widthF, FLOAT(1.f)), hx)), MIN_F(hy, SUB_F(SUB_F(p->heightF, FLOAT(1.f)), hy)));
    SpvId fade = P_CLAMP(DIV_F(edge, MUL_F(p->widthF, FLOAT(.08f))), FLOAT(0.f), FLOAT(1.f));
    SpvId smooth = SUB_F(FLOAT(1.f), DIV_F(rough, FLOAT(.6f)));
    /* as the surface was lit from all around in the mirror direction: what the march found takes its place */
    SpvId f0 = ADD_F(FLOAT(.04f), MUL_F(FLOAT(.96f), metal));
    SpvId c1 = SUB_F(FLOAT(1.f), MAX_F(spvOp1(s, SPV_OP_FNegate, s->floatType, vn), FLOAT(1e-4f))), c2 = MUL_F(c1, c1), fresnel = MUL_F(MUL_F(c2, c2), c1);
    SpvId mirror = ADD_F(f0, MUL_F(SUB_F(MAX_F(SUB_F(FLOAT(1.f), rough), f0), f0), fresnel));
    SpvId sure = MUL_F(MUL_F(MUL_F(smooth, smooth), fade), X(frameVec(s, &p->r, F_POST + 1)));
    SpvId skyRow = frameVec(s, &p->r, F_SKY), ground = spvShuffle3(s, frameVec(s, &p->r, F_GROUND));
    SpvId up = ADD_F(MUL_F(Y(r), FLOAT(.5f)), FLOAT(.5f));
    SpvId plain = ADD_V3(ground, SCALE_V3(SUB_V3(spvShuffle3(s, skyRow), ground), up));
    SpvId pictured = SCALE_V3(envReflected(s, &p->r, rough, r), W(skyRow));
    SpvId around = pick3(s, P_MORE(W(skyRow), FLOAT(0.f)), pictured, plain);
    SpvId sum = ADD_F(ADD_F(X(mine), Y(mine)), Z(mine));
    SpvId tint = ADD_V3(same3(s, 1.f), SCALE_V3(SUB_V3(SCALE_V3(spvShuffle3(s, mine), DIV_F(FLOAT(3.f), MAX_F(sum, FLOAT(1e-6f)))), same3(s, 1.f)), PICK_F(P_MORE(sum, FLOAT(1e-6f)), metal, FLOAT(0.f))));
    SpvId seen = spvShuffle3(s, postTap(s, p, p->light, UINT(0), p->width, p->height, hx, hy));
    SpvId reflected = SCALE_V3(MUL_V3(SUB_V3(seen, around), tint), MUL_F(mirror, sure));
    SpvId use = P_AND(P_AND(found, P_MORE(d, FLOAT(0.f))), P_AND(spvOp2(s, SPV_OP_INotEqual, s->boolType, word, UINT(0)), spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, rough, FLOAT(.6f))));
    use = P_AND(use, spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, away, FLOAT(-.2f)));
    spvStore(s, postPtr(s, p, p->bloom, s->vec4, half), vec4Of(s, pick3(s, use, reflected, same3(s, 0.f)), FLOAT(0.f)));
    spvIfEnd(s, marching);
POST_FINISH()

/* ---- reflections from rays (ArtfPostDesc.screen.tracedReflections): as reflectionRow's, with artfTracedReflection in place
 * of the march ---- */
uint32_t* artfShaderPostReflectionTraced(size_t* size, uint32_t maxTextures, const ArtfGraphData* graphs, uint32_t graphCount, bool hardware) {
    Spv b, *s = &b;
    spvInit(s);
    spvCapability(s, 5301); /* ShaderNonUniform */
    spvCapability(s, 5307); /* SampledImageArrayNonUniformIndexing */
    spvCapability(s, 50); /* ImageQuery: a texture's size, for the tracer's cones */
    if (hardware) spvCapability(s, 4472 /* RayQueryKHR */);
    spvExtension(s, "SPV_EXT_descriptor_indexing");
    Post post, *p = &post;
    postDeclare(s, p);
    SpvId imageType, sampled = spvTypeSampledImage2D(s, &imageType);
    SpvId textures = spvResource(s, spvTypeArray(s, sampled, maxTextures, 0), SPV_STORAGE_UniformConstant, 6);
    Tracer tracer, *t = &tracer;
    tracerDeclare(s, t, &p->r, textures, sampled, graphs, graphCount, hardware);
    SpvId main = spvBeginMain(s);
    tracerVariables(t);
    SpvId ptrFn3 = spvTypePointer(s, SPV_STORAGE_Function, s->vec3), ptrFnUint = spvTypePointer(s, SPV_STORAGE_Function, s->uintType);
    SpvId found = VAR(ptrFnUint), seenVar = VAR(ptrFn3), litVar = VAR(ptrFn3), lightIndex = VAR(ptrFnUint);
    postSizes(s, p);
    SpvId hw = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->width, UINT(1)), UINT(2)), hh = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->height, UINT(1)), UINT(2));
    SpvId half, halfX, halfY, merge;
    postBegin(s, p, &half, &halfX, &halfY, &merge, hw, hh);
    SpvId x = spvGlsl2(s, 38 /* UMin */, s->uintType, MUL_U(halfX, UINT(2)), SUB_U(p->width, UINT(1))), y = spvGlsl2(s, 38, s->uintType, MUL_U(halfY, UINT(2)), SUB_U(p->height, UINT(1)));
    SpvId i = ADD_U(MUL_U(y, p->width), x);
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId word = postUint(s, p, p->surface, i), d = postDistance(s, p, xf, yf);
    SpvId rough = postField(s, word, 18, 31, 31.f), metal = postField(s, word, 23, 15, 15.f);
    SpvId n = postNormal(s, word), at = postWorld(s, p, xf, yf, d);
    SpvId v = NORMALIZE(SUB_V3(at, spvShuffle3(s, frameVec(s, &p->r, F_CAMERA))));
    SpvId vn = DOT3(v, n);
    SpvId strength = X(frameVec(s, &p->r, F_POST + 1));
    spvStore(s, postPtr(s, p, p->bloom, s->vec4, half), vec4Of(s, same3(s, 0.f), FLOAT(0.f)));
    SpvId eligible = P_AND(P_AND(P_MORE(d, FLOAT(0.f)), spvOp2(s, SPV_OP_INotEqual, s->boolType, word, UINT(0))), P_AND(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, rough, FLOAT(.6f)), P_MORE(strength, FLOAT(0.f))));
    SpvId tracing;
    spvIfBegin(s, eligible, &tracing);
    {
        /* as artfTracedReflection: the ray, strayed by roughness, off the surface */
        SpvId point = ADD_U(ADD_U(AND_U(halfX, UINT(3)), MUL_U(AND_U(halfY, UINT(3)), UINT(4))), MUL_U(AS_UINT(Z(frameVec(s, &p->r, F_TRACED))), UINT(5)));
        SpvId mirror = SUB_V3(v, SCALE_V3(n, MUL_F(FLOAT(2.f), vn)));
        SpvId strayed = NORMALIZE(ADD_V3(mirror, SCALE_V3(tracedBallAt(t, AND_U(point, UINT(15))), MUL_F(rough, rough))));
        spvStore(s, t->direction, pick3(s, spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, DOT3(strayed, n), FLOAT(0.f)), mirror, strayed));
        spvStore(s, t->origin, tracedLiftSpv(s, &p->r, at, n, 0));
        spvStore(s, found, UINT(0));
        spvStore(s, seenVar, same3(s, 0.f));
        spvStore(s, t->step, UINT(0));
        Loop steps;
        loopOpen(s, &steps);
        loopWhile(s, &steps, LESS_U(LOAD_U(t->step), UINT(4)));
        {
            SpvId o = LOAD_V3(t->origin), dir = LOAD_V3(t->direction);
            closest(t, o, dir, FLOAT(3e37f), false);
            breakIf(s, &steps, IS_EQ(LOAD_U(t->bestInstance), UINT(0xFFFFFFFFu)));
            Met m = surfaceAt(t, o, dir, FLOAT(0.f));
            SpvId q = ADD_V3(o, SCALE_V3(dir, LOAD_F(t->bestT)));
            SpvId through = P_LESS(m.alpha, FLOAT(.5f)), branch;
            spvIfBegin(s, through, &branch);
            spvStore(s, t->origin, ADD_V3(q, SCALE_V3(dir, reachOf(s, q))));
            spvIfEnd(s, branch);
            spvIfBegin(s, P_NOT(through), &branch);
            {
                /* its color under light from all around, the sun and the frame's lights, with no shadows; its own light */
                SpvId skyRow = frameVec(s, &p->r, F_SKY), ground = spvShuffle3(s, frameVec(s, &p->r, F_GROUND));
                SpvId up = ADD_F(MUL_F(Y(m.normal), FLOAT(.5f)), FLOAT(.5f));
                SpvId plain = SCALE_V3(ADD_V3(ground, SCALE_V3(SUB_V3(spvShuffle3(s, skyRow), ground), up)), m.open);
                SpvId pictured = SCALE_V3(envSample(s, &p->r, UINT(ARTF_ENV_DIFFUSE), m.normal), MUL_F(W(skyRow), m.open));
                SpvId sun = MAX_F(DOT3(m.normal, spvShuffle3(s, frameVec(s, &p->r, F_TOSUN))), FLOAT(0.f));
                spvStore(s, litVar, ADD_V3(pick3(s, P_MORE(W(skyRow), FLOAT(0.f)), pictured, plain), SCALE_V3(spvShuffle3(s, frameVec(s, &p->r, F_SUNCOLOR)), sun)));
                SpvId lightCount = AS_UINT(X(frameVec(s, &p->r, F_COUNTS)));
                spvStore(s, lightIndex, UINT(0));
                Loop lights;
                loopOpen(s, &lights);
                loopWhile(s, &lights, LESS_U(LOAD_U(lightIndex), lightCount));
                {
                    SpvId li = LOAD_U(lightIndex);
                    SpvId row = ADD_U(UINT(F_LIGHTS), MUL_U(li, UINT(F_LIGHT_ROWS)));
                    SpvId lightA = frameVecAt(s, &p->r, row), lightB = frameVecAt(s, &p->r, ADD_U(row, UINT(1))), lightC = frameVecAt(s, &p->r, ADD_U(row, UINT(2)));
                    SpvId toward = SUB_V3(spvShuffle3(s, lightA), q);
                    SpvId d2 = DOT3(toward, toward);
                    SpvId to = SCALE_V3(toward, DIV_F(FLOAT(1.f), SQRT_F(MAX_F(d2, FLOAT(1e-8f)))));
                    SpvId fade = MAX_F(SUB_F(FLOAT(1.f), DIV_F(d2, MUL_F(W(lightA), W(lightA)))), FLOAT(0.f));
                    SpvId spot = P_CLAMP(MUL_F(SUB_F(spvOp1(s, SPV_OP_FNegate, s->floatType, DOT3(to, spvShuffle3(s, lightC))), W(lightC)), W(lightB)), FLOAT(0.f), FLOAT(1.f));
                    SpvId amount = MUL_F(MUL_F(MUL_F(MAX_F(DOT3(m.normal, to), FLOAT(0.f)), fade), fade), spot);
                    spvStore(s, litVar, ADD_V3(LOAD_V3(litVar), SCALE_V3(spvShuffle3(s, lightB), amount)));
                    spvStore(s, lightIndex, ADD_U(li, UINT(1)));
                }
                loopClose(s, &lights);
                spvStore(s, seenVar, ADD_V3(m.emissive, MUL_V3(ADD_V3(m.albedo, m.f0), LOAD_V3(litVar))));
                spvStore(s, found, UINT(1));
            }
            spvIfEnd(s, branch);
            breakIf(s, &steps, P_NOT(through));
            spvStore(s, t->step, ADD_U(LOAD_U(t->step), UINT(1)));
        }
        loopClose(s, &steps);
        /* as reflectionUse, sure throughout: the frame's edges do not matter to a ray */
        SpvId mine = postVec4(s, p, p->light, i);
        SpvId smooth = SUB_F(FLOAT(1.f), DIV_F(rough, FLOAT(.6f)));
        SpvId f0 = ADD_F(FLOAT(.04f), MUL_F(FLOAT(.96f), metal));
        SpvId c1 = SUB_F(FLOAT(1.f), MAX_F(spvOp1(s, SPV_OP_FNegate, s->floatType, vn), FLOAT(1e-4f))), c2 = MUL_F(c1, c1), fresnel = MUL_F(MUL_F(c2, c2), c1);
        SpvId reflecting = ADD_F(f0, MUL_F(SUB_F(MAX_F(SUB_F(FLOAT(1.f), rough), f0), f0), fresnel));
        SpvId sure = MUL_F(MUL_F(MUL_F(smooth, smooth), FLOAT(1.f)), strength);
        SpvId r = mirror;
        SpvId skyRow = frameVec(s, &p->r, F_SKY), ground = spvShuffle3(s, frameVec(s, &p->r, F_GROUND));
        SpvId up = ADD_F(MUL_F(Y(r), FLOAT(.5f)), FLOAT(.5f));
        SpvId plain = ADD_V3(ground, SCALE_V3(SUB_V3(spvShuffle3(s, skyRow), ground), up));
        SpvId pictured = SCALE_V3(envReflected(s, &p->r, rough, r), W(skyRow));
        SpvId around = pick3(s, P_MORE(W(skyRow), FLOAT(0.f)), pictured, plain);
        SpvId sum = ADD_F(ADD_F(X(mine), Y(mine)), Z(mine));
        SpvId tint = ADD_V3(same3(s, 1.f), SCALE_V3(SUB_V3(SCALE_V3(spvShuffle3(s, mine), DIV_F(FLOAT(3.f), MAX_F(sum, FLOAT(1e-6f)))), same3(s, 1.f)), PICK_F(P_MORE(sum, FLOAT(1e-6f)), metal, FLOAT(0.f))));
        SpvId reflected = SCALE_V3(MUL_V3(SUB_V3(LOAD_V3(seenVar), around), tint), MUL_F(reflecting, sure));
        SpvId use = spvOp2(s, SPV_OP_INotEqual, s->boolType, LOAD_U(found), UINT(0));
        spvStore(s, postPtr(s, p, p->bloom, s->vec4, half), vec4Of(s, pick3(s, use, reflected, same3(s, 0.f)), FLOAT(0.f)));
    }
    spvIfEnd(s, tracing);
    spvIfEnd(s, merge);
    spvEndMain(s);
    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &p->global, 1);
    spvExecutionMode(s, main, 17, 64, 1, 1, 3);
    return finish(s, size);
}

POST_MAIN(artfShaderPostReflectionApply)
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId hw = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->width, UINT(1)), UINT(2)), hh = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->height, UINT(1)), UINT(2));
    SpvId got = postTap(s, p, p->bloom, UINT(0), hw, hh, SUB_F(MUL_F(ADD_F(TO_F(x), FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f)), SUB_F(MUL_F(ADD_F(TO_F(y), FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f)));
    SpvId light = postLight(s, p, p->light, i);
    postLightStore(s, p, p->light, i, vec4Of(s, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, ADD_V3(spvShuffle3(s, light), spvShuffle3(s, got)), same3(s, 0.f)), W(light)));
POST_FINISH()

/* ---- fog that light goes through: the grid of froxels, as froxelRow, before the frame is drawn ---- */
POST_MAIN(artfShaderPostFroxels)
    Shadows shadows;
    memset(&shadows, 0, sizeof shadows);
    SpvId shadowImage, shadowSampled = spvTypeSampledImage2D(s, &shadowImage);
    shadows.block = spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, S_ROWS, 16), true), SPV_STORAGE_Uniform, 24);
    shadows.atlas = spvResource(s, shadowSampled, SPV_STORAGE_UniformConstant, 25);
    shadows.imageType = shadowImage;
    shadows.sampled = shadowSampled;
    shadows.ivec2 = spvTypeVector(s, s->intType, 2);
    shadows.r = &p->r;
    SpvId main = spvBeginMain(s);
    SpvId ptrFn3 = spvTypePointer(s, SPV_STORAGE_Function, s->vec3), ptrFnF = spvTypePointer(s, SPV_STORAGE_Function, s->floatType), ptrFnU = spvTypePointer(s, SPV_STORAGE_Function, s->uintType);
    SpvId lightVar = spvVariable(s, ptrFn3, SPV_STORAGE_Function), leftVar = spvVariable(s, ptrFnF, SPV_STORAGE_Function), counter = spvVariable(s, ptrFnU, SPV_STORAGE_Function);
    postSizes(s, p);
    SpvId gx = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->width, UINT(ARTF_FROXEL_TILE - 1)), UINT(ARTF_FROXEL_TILE));
    SpvId gy = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->height, UINT(ARTF_FROXEL_TILE - 1)), UINT(ARTF_FROXEL_TILE));
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, gx, gy);
    SpvId look = frameVec(s, &p->r, F_LOOK), postRow = frameVec(s, &p->r, F_POST + 1);
    SpvId density = Y(look), amount = Y(postRow), g = Z(postRow);
    SpvId far = DIV_F(FLOAT(4.f), MAX_F(density, FLOAT(1e-6f)));
    SpvId cx = ADD_F(MUL_F(TO_F(x), FLOAT((float)ARTF_FROXEL_TILE)), FLOAT(ARTF_FROXEL_TILE * .5f - .5f)), cy = ADD_F(MUL_F(TO_F(y), FLOAT((float)ARTF_FROXEL_TILE)), FLOAT(ARTF_FROXEL_TILE * .5f - .5f));
    /* (orthographic: each column starts from its place on the view's plane, and goes the way the view looks) */
    SpvId eye = pick3(s, orthoOn(s, &p->r), postWorld(s, p, cx, cy, FLOAT(0.f)), spvShuffle3(s, frameVec(s, &p->r, F_CAMERA)));
    SpvId dir = SUB_V3(postWorld(s, p, cx, cy, FLOAT(1.f)), eye);
    SpvId along = spvGlsl1(s, SPV_GLSL_Length, s->floatType, dir);
    SpvId toSun = spvShuffle3(s, frameVec(s, &p->r, F_TOSUN)), sunColor = spvShuffle3(s, frameVec(s, &p->r, F_SUNCOLOR));
    SpvId mu = DIV_F(DOT3(dir, toSun), along);
    SpvId hg = SUB_F(ADD_F(FLOAT(1.f), MUL_F(g, g)), MUL_F(MUL_F(FLOAT(2.f), g), mu));
    SpvId phase = DIV_F(SUB_F(FLOAT(1.f), MUL_F(g, g)), MUL_F(MUL_F(FLOAT(12.566371f), hg), SQRT_F(hg)));
    SpvId fogColor = spvShuffle3(s, frameVec(s, &p->r, F_FOG));
    SpvId sunRow = shadowRow(s, &shadows, UINT(S_SUN)), splits = shadowRow(s, &shadows, UINT(S_SPLITS));
    SpvId casts = spvOp2(s, SPV_OP_INotEqual, s->boolType, AS_UINT(X(sunRow)), UINT(0));
    SpvId count = AS_UINT(Y(sunRow));
    SpvId lastSplit = PICK_F(spvOp2(s, 170, s->boolType, count, UINT(1)), X(splits), PICK_F(spvOp2(s, 170, s->boolType, count, UINT(2)), Y(splits), Z(splits)));
    spvStore(s, lightVar, same3(s, 0.f));
    spvStore(s, leftVar, FLOAT(1.f));
    spvStore(s, counter, UINT(0));
    SpvId header = spvNewId(s), test = spvNewId(s), body = spvNewId(s), next = spvNewId(s), done = spvNewId(s);
    spvBranch(s, header);
    spvLabel(s, header);
    spvLoopMerge(s, done, next);
    spvBranch(s, test);
    spvLabel(s, test);
    SpvId k = spvLoad(s, s->uintType, counter);
    spvBranchIf(s, P_LESS_U(k, UINT(ARTF_FROXEL_SLICES)), body, done);
    spvLabel(s, body);
    {
        SpvId a = DIV_F(TO_F(k), FLOAT((float)ARTF_FROXEL_SLICES)), b = DIV_F(TO_F(ADD_U(k, UINT(1))), FLOAT((float)ARTF_FROXEL_SLICES));
        SpvId d0 = MUL_F(far, MUL_F(a, a)), d1 = MUL_F(far, MUL_F(b, b)), t = MUL_F(ADD_F(d0, d1), FLOAT(.5f));
        SpvId q = ADD_V3(eye, SCALE_V3(dir, t));
        /* as artfShadowOfSun, with the normal toward the sun */
        SpvId distance = spvGlsl1(s, SPV_GLSL_Length, s->floatType, SUB_V3(q, eye));
        SpvId past0 = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, distance, X(splits)), past1 = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, distance, Y(splits));
        SpvId cascade = spvSelect(s, s->uintType, past1, UINT(2), spvSelect(s, s->uintType, past0, UINT(1), UINT(0)));
        cascade = spvGlsl2(s, 38 /* UMin */, s->uintType, cascade, SUB_U(spvGlsl2(s, 41 /* UMax */, s->uintType, count, UINT(1)), UINT(1)));
        SpvId view = ADD_U(ADD_U(AS_UINT(X(sunRow)), UINT(0xFFFFFFFFu)), cascade);
        SpvId base = ADD_U(UINT(S_VIEWS), MUL_U(view, UINT(S_VIEW_ROWS)));
        SpvId facing = MAX_F(DOT3(toSun, spvShuffle3(s, shadowRow(s, &shadows, ADD_U(base, UINT(S_PLACE))))), FLOAT(.15f));
        SpvId lift = DIV_F(MUL_F(Z(shadowRow(s, &shadows, ADD_U(base, UINT(S_INFO)))), FLOAT(2.f)), facing);
        SpvId seen = shadowLookup(s, &shadows, view, ADD_V3(q, SCALE_V3(toSun, lift)));
        SpvId sun = PICK_F(P_AND(casts, P_LESS(distance, lastSplit)), seen, FLOAT(1.f));
        /* fog by height: as artfFogHeight */
        SpvId heights = frameVec(s, &p->r, F_MATERIALS);
        SpvId thinned = MIN_F(postExp2(s, MUL_F(MUL_F(SUB_F(Y(heights), Y(q)), Z(heights)), FLOAT(1.44269504f))), FLOAT(16.f));
        SpvId xx = MUL_F(MUL_F(MUL_F(density, PICK_F(P_MORE(Z(heights), FLOAT(0.f)), thinned, FLOAT(1.f))), SUB_F(d1, d0)), along);
        SpvId stepThrough = DIV_F(FLOAT(1.f), ADD_F(ADD_F(FLOAT(1.f), xx), MUL_F(MUL_F(xx, xx), FLOAT(.5f))));
        SpvId left = spvLoad(s, s->floatType, leftVar);
        SpvId taken = MUL_F(left, SUB_F(FLOAT(1.f), stepThrough));
        SpvId light = ADD_V3(spvLoad(s, s->vec3, lightVar), SCALE_V3(ADD_V3(SCALE_V3(sunColor, MUL_F(MUL_F(sun, phase), FLOAT(12.566371f))), fogColor), taken));
        left = MUL_F(left, stepThrough);
        spvStore(s, lightVar, light);
        spvStore(s, leftVar, left);
        spvStore(s, postPtr(s, p, p->fog, s->vec4, ADD_U(MUL_U(i, UINT(ARTF_FROXEL_SLICES)), k)), vec4Of(s, SCALE_V3(light, amount), SUB_F(FLOAT(1.f), MUL_F(SUB_F(FLOAT(1.f), left), amount))));
    }
    spvBranch(s, next);
    spvLabel(s, next);
    spvStore(s, counter, ADD_U(spvLoad(s, s->uintType, counter), UINT(1)));
    spvBranch(s, header);
    spvLabel(s, done);
POST_FINISH()

/* As post.c's wasAt: where the surface at a pixel was in the frame before, by its depth and how its instance moved, or
 * for what a pose bent, by where its vertices were (ARTF_MOTION_VECTOR's word). */
static void postWasAt(Spv* s, Post* p, SpvId xf, SpvId yf, SpvId* sx, SpvId* sy, SpvId* ok) {
    SpvId plane = postPlane(s, p, xf, yf);
    SpvId depth = postFloat(s, p, p->depth, plane);
    SpvId d = postAlong(s, p, depth);
    SpvId ax = SUB_F(xf, p->ownX), ay = ADD_F(yf, p->ownY);
    SpvId at = postWorld(s, p, ax, ay, PICK_F(P_MORE(d, FLOAT(0.f)), d, FLOAT(1e4f)));
    /* (only when something moved: what stood still is read for nothing) */
    SpvId moving = P_AND(P_MORE(Y(frameVec(s, &p->r, F_CULL)), FLOAT(0.f)), P_MORE(d, FLOAT(0.f)));
    SpvId before = s->block, merge;
    spvIfBegin(s, moving, &merge);
    SpvId who = postUint(s, p, p->surface, ADD_U(MUL_U(p->sceneW, p->sceneH), plane));
    /* (a vector, in quarter pixels of the scene's size: as artfMotionUnpack) */
    SpvId vector = spvOp2(s, SPV_OP_INotEqual, s->boolType, AND_U(who, UINT(ARTF_MOTION_VECTOR)), UINT(0));
    /* (the motion, after the instances: four rows each. A vector's word is no instance: its rows are not read, and
     * what is read in its place is the first's, so that no read goes past the instances' buffer) */
    SpvId instances = AS_UINT(Y(frameVec(s, &p->r, F_COUNTS)));
    SpvId which = spvSelect(s, s->uintType, vector, UINT(0), SUB_U(spvGlsl2(s, 41 /* UMax */, s->uintType, who, UINT(1)), UINT(1)));
    SpvId row = ADD_U(MUL_U(instances, UINT(4)), MUL_U(which, UINT(4)));
    SpvId m0 = bufferVec(s, &p->r, p->r.instances, row), m1 = bufferVec(s, &p->r, p->r.instances, ADD_U(row, UINT(1))), m2 = bufferVec(s, &p->r, p->r.instances, ADD_U(row, UINT(2)));
    SpvId at4 = vec4Of(s, at, FLOAT(1.f));
    SpvId was = pick3(s, spvOp2(s, SPV_OP_INotEqual, s->boolType, who, UINT(0)), vec3Of(s, DOT4(m0, at4), DOT4(m1, at4), DOT4(m2, at4)), at);
    SpvId dx = MUL_F(SUB_F(TO_F(AND_U(who, UINT(32767))), FLOAT(16384.f)), FLOAT(.25f)), dy = MUL_F(SUB_F(TO_F(AND_U(SHR_U(who, 15), UINT(32767))), FLOAT(16384.f)), FLOAT(.25f));
    SpvId vx = p->frameSize ? ADD_F(ax, DIV_F(dx, p->ratioX)) : ADD_F(ax, dx), vy = p->frameSize ? ADD_F(ay, DIV_F(dy, p->ratioY)) : ADD_F(ay, dy);
    SpvId known = P_LESS(dx, FLOAT(4095.f));
    SpvId movedFrom = s->block;
    spvIfEnd(s, merge);
    at = spvPhi(s, s->vec3, was, movedFrom, at, before);
    SpvId isVector = spvPhi(s, s->boolType, vector, movedFrom, spvConstBool(s, false), before);
    vx = spvPhi(s, s->floatType, vx, movedFrom, FLOAT(0.f), before);
    vy = spvPhi(s, s->floatType, vy, movedFrom, FLOAT(0.f), before);
    known = spvPhi(s, s->boolType, known, movedFrom, spvConstBool(s, true), before);
    SpvId w, px, py, pok;
    postProject(s, p, F_PREVIOUS, at, &px, &py, &w, &pok);
    *sx = PICK_F(isVector, vx, px);
    *sy = PICK_F(isVector, vy, py);
    *ok = spvSelect(s, s->boolType, isVector, known, pok);
}

/* ---- motion blur: as post.c's velocityRow, tileMostRow, nearMostRow and blurRow ---- */

#define POST_BLUR_TILE 16 /* as post.c's */
#define POST_BLUR_REACH 32.f
#define POST_BLUR_TAPS 12

/* Motion blur's buffer (69): velocities, two floats a pixel; then the largest of each tile, and of each tile's
 * neighbours. */
static SpvId postBlurBuffer(Spv* s, Post* p) { return postBuffer(s, p, p->r.floatArray, 69); }
static void postBlurTiles(Spv* s, Post* p, SpvId* tilesX, SpvId* tilesY) {
    *tilesX = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->width, UINT(POST_BLUR_TILE - 1)), UINT(POST_BLUR_TILE));
    *tilesY = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->height, UINT(POST_BLUR_TILE - 1)), UINT(POST_BLUR_TILE));
}
static SpvId postPair(Spv* s, Post* p, SpvId buffer, SpvId at) {
    const SpvId parts[2] = {postFloat(s, p, buffer, at), postFloat(s, p, buffer, ADD_U(at, UINT(1)))};
    return spvCompose(s, s->vec2, parts, 2);
}
static void postPairPut(Spv* s, Post* p, SpvId buffer, SpvId at, SpvId x, SpvId y) {
    spvStore(s, postPtr(s, p, buffer, s->floatType, at), x);
    spvStore(s, postPtr(s, p, buffer, s->floatType, ADD_U(at, UINT(1))), y);
}

POST_MAIN(artfShaderPostVelocity)
    SpvId blur = postBlurBuffer(s, p);
    SpvId main = spvBeginMain(s);
    postFrameSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId sx, sy, ok;
    postWasAt(s, p, xf, yf, &sx, &sy, &ok);
    SpvId shutter = W(frameVec(s, &p->r, F_MATERIALS));
    SpvId vx = PICK_F(ok, MUL_F(SUB_F(xf, sx), shutter), FLOAT(0.f)), vy = PICK_F(ok, MUL_F(SUB_F(yf, sy), shutter), FLOAT(0.f));
    SpvId length = ADD_F(MUL_F(vx, vx), MUL_F(vy, vy));
    SpvId scale = PICK_F(P_MORE(length, FLOAT(POST_BLUR_REACH * POST_BLUR_REACH)), DIV_F(FLOAT(POST_BLUR_REACH), SQRT_F(MAX_F(length, FLOAT(1e-12f)))), FLOAT(1.f));
    postPairPut(s, p, blur, MUL_U(i, UINT(2)), MUL_F(vx, scale), MUL_F(vy, scale));
POST_FINISH()

/* The largest of a tile (push 0) or of a tile and its neighbours (push 1): one invocation a tile. */
POST_MAIN(artfShaderPostTileMost)
    SpvId blur = postBlurBuffer(s, p);
    SpvId main = spvBeginMain(s);
    SpvId ptrFn2 = spvTypePointer(s, SPV_STORAGE_Function, s->vec2), ptrFnF = spvTypePointer(s, SPV_STORAGE_Function, s->floatType), ptrFnU = spvTypePointer(s, SPV_STORAGE_Function, s->uintType);
    SpvId mostVar = spvVariable(s, ptrFn2, SPV_STORAGE_Function), lengthVar = spvVariable(s, ptrFnF, SPV_STORAGE_Function), counter = spvVariable(s, ptrFnU, SPV_STORAGE_Function);
    postFrameSizes(s, p);
    SpvId tilesX, tilesY;
    postBlurTiles(s, p, &tilesX, &tilesY);
    SpvId t, tx, ty, merge;
    postBegin(s, p, &t, &tx, &ty, &merge, tilesX, tilesY);
    SpvId neighbours = spvOp2(s, SPV_OP_INotEqual, s->boolType, postPush(s, p), UINT(0));
    SpvId pixels2 = MUL_U(MUL_U(p->width, p->height), UINT(2)), tiles2 = MUL_U(MUL_U(tilesX, tilesY), UINT(2));
    const SpvId zero2[2] = {FLOAT(0.f), FLOAT(0.f)};
    spvStore(s, mostVar, spvCompose(s, s->vec2, zero2, 2));
    spvStore(s, lengthVar, FLOAT(-1.f));
    spvStore(s, counter, UINT(0));
    /* (a tile's pixels, or the nine tiles around: whichever there are, in order, the first largest kept) */
    SpvId count = spvSelect(s, s->uintType, neighbours, UINT(9), UINT(POST_BLUR_TILE * POST_BLUR_TILE));
    SpvId header = spvNewId(s), test = spvNewId(s), body = spvNewId(s), next = spvNewId(s), done = spvNewId(s);
    spvBranch(s, header);
    spvLabel(s, header);
    spvLoopMerge(s, done, next);
    spvBranch(s, test);
    spvLabel(s, test);
    SpvId k = spvLoad(s, s->uintType, counter);
    spvBranchIf(s, P_LESS_U(k, count), body, done);
    spvLabel(s, body);
    {
        /* a pixel of the tile */
        SpvId px = ADD_U(MUL_U(tx, UINT(POST_BLUR_TILE)), spvOp2(s, SPV_OP_UMod, s->uintType, k, UINT(POST_BLUR_TILE)));
        SpvId py = ADD_U(MUL_U(ty, UINT(POST_BLUR_TILE)), spvOp2(s, SPV_OP_UDiv, s->uintType, k, UINT(POST_BLUR_TILE)));
        SpvId inside = P_AND(P_LESS_U(px, p->width), P_LESS_U(py, p->height));
        SpvId pixelAt = MUL_U(ADD_U(MUL_U(spvGlsl2(s, 38, s->uintType, py, SUB_U(p->height, UINT(1))), p->width), spvGlsl2(s, 38, s->uintType, px, SUB_U(p->width, UINT(1)))), UINT(2));
        /* or a tile around, clamped to the frame's */
        SpvId nx = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvGlsl2(s, 42 /* SMax */, s->intType, spvOp2(s, SPV_OP_ISub, s->intType, spvOp1(s, SPV_OP_Bitcast, s->intType, ADD_U(tx, spvOp2(s, SPV_OP_UMod, s->uintType, k, UINT(3)))), spvConstInt(s, 1)), spvConstInt(s, 0)));
        SpvId ny = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvGlsl2(s, 42, s->intType, spvOp2(s, SPV_OP_ISub, s->intType, spvOp1(s, SPV_OP_Bitcast, s->intType, ADD_U(ty, spvOp2(s, SPV_OP_UDiv, s->uintType, k, UINT(3)))), spvConstInt(s, 1)), spvConstInt(s, 0)));
        nx = spvGlsl2(s, 38, s->uintType, nx, SUB_U(tilesX, UINT(1)));
        ny = spvGlsl2(s, 38, s->uintType, ny, SUB_U(tilesY, UINT(1)));
        SpvId tileAt = ADD_U(pixels2, MUL_U(ADD_U(MUL_U(ny, tilesX), nx), UINT(2)));
        SpvId v = postPair(s, p, blur, spvSelect(s, s->uintType, neighbours, tileAt, pixelAt));
        SpvId l = spvOp2(s, SPV_OP_Dot, s->floatType, v, v);
        SpvId larger = P_AND(spvOp2(s, 166, s->boolType, neighbours, inside), P_MORE(l, spvLoad(s, s->floatType, lengthVar)));
        SpvId kept = spvLoad(s, s->vec2, mostVar);
        const SpvId chosen[2] = {PICK_F(larger, spvExtract(s, s->floatType, v, 0), spvExtract(s, s->floatType, kept, 0)), PICK_F(larger, spvExtract(s, s->floatType, v, 1), spvExtract(s, s->floatType, kept, 1))};
        spvStore(s, mostVar, spvCompose(s, s->vec2, chosen, 2));
        spvStore(s, lengthVar, PICK_F(larger, l, spvLoad(s, s->floatType, lengthVar)));
    }
    spvBranch(s, next);
    spvLabel(s, next);
    spvStore(s, counter, ADD_U(spvLoad(s, s->uintType, counter), UINT(1)));
    spvBranch(s, header);
    spvLabel(s, done);
    SpvId most = spvLoad(s, s->vec2, mostVar);
    SpvId out = ADD_U(ADD_U(pixels2, spvSelect(s, s->uintType, neighbours, tiles2, UINT(0))), MUL_U(t, UINT(2)));
    postPairPut(s, p, blur, out, spvExtract(s, s->floatType, most, 0), spvExtract(s, s->floatType, most, 1));
POST_FINISH()

/* a in front of b, softly; a cone's and a cylinder's reach: as post.c's */
static SpvId postInFront(Spv* s, SpvId a, SpvId b) { return P_CLAMP(SUB_F(FLOAT(1.f), DIV_F(SUB_F(a, b), ADD_F(MUL_F(FLOAT(.02f), MIN_F(a, b)), FLOAT(1e-3f)))), FLOAT(0.f), FLOAT(1.f)); }
static SpvId postCone(Spv* s, SpvId d, SpvId reach) { return P_CLAMP(SUB_F(FLOAT(1.f), DIV_F(d, reach)), FLOAT(0.f), FLOAT(1.f)); }
static SpvId postCylinder(Spv* s, SpvId d, SpvId reach) {
    SpvId t = P_CLAMP(DIV_F(SUB_F(d, MUL_F(FLOAT(.95f), reach)), MUL_F(FLOAT(.1f), reach)), FLOAT(0.f), FLOAT(1.f));
    return SUB_F(FLOAT(1.f), MUL_F(MUL_F(t, t), SUB_F(FLOAT(3.f), MUL_F(FLOAT(2.f), t))));
}

POST_MAIN(artfShaderPostMotionBlur)
    SpvId blur = postBlurBuffer(s, p);
    SpvId main = spvBeginMain(s);
    postFrameSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId tilesX, tilesY;
    postBlurTiles(s, p, &tilesX, &tilesY);
    SpvId pixels2 = MUL_U(MUL_U(p->width, p->height), UINT(2)), tiles2 = MUL_U(MUL_U(tilesX, tilesY), UINT(2));
    SpvId tile = ADD_U(MUL_U(spvOp2(s, SPV_OP_UDiv, s->uintType, y, UINT(POST_BLUR_TILE)), tilesX), spvOp2(s, SPV_OP_UDiv, s->uintType, x, UINT(POST_BLUR_TILE)));
    SpvId reachAll = postPair(s, p, blur, ADD_U(ADD_U(pixels2, tiles2), MUL_U(tile, UINT(2))));
    SpvId nx = spvExtract(s, s->floatType, reachAll, 0), ny = spvExtract(s, s->floatType, reachAll, 1);
    SpvId now = postVec4(s, p, p->light, i);
    /* (only where something can reach: elsewhere what was there stays, and costs nothing more) */
    SpvId reaching = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, spvOp2(s, SPV_OP_Dot, s->floatType, reachAll, reachAll), FLOAT(.25f));
    SpvId nowRgb = spvShuffle3(s, now); /* (before the if: what a phi reads comes first) */
    SpvId before = s->block, blurMerge;
    spvIfBegin(s, reaching, &blurMerge);
    SpvId here = postPair(s, p, blur, MUL_U(i, UINT(2)));
    SpvId reach = MUL_F(MAX_F(SQRT_F(spvOp2(s, SPV_OP_Dot, s->floatType, here, here)), FLOAT(.5f)), FLOAT(.5f));
    SpvId z = postDistance(s, p, xf, yf);
    z = PICK_F(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, z, FLOAT(0.f)), FLOAT(1e6f), z);
    SpvId weight = DIV_F(FLOAT(.5f), reach);
    SpvId sum = SCALE_V3(spvShuffle3(s, now), weight);
    SpvId jitter = SUB_F(DIV_F(TO_F(AND_U(postHash(s, i), UINT(255))), FLOAT(256.f)), FLOAT(.5f));
    for (int k = 0; k < POST_BLUR_TAPS; ++k) {
        SpvId along = SUB_F(DIV_F(ADD_F(FLOAT((float)k + .5f), jitter), FLOAT((float)POST_BLUR_TAPS)), FLOAT(.5f));
        SpvId qx = P_CLAMP(FLOOR_F(ADD_F(ADD_F(xf, MUL_F(nx, along)), FLOAT(.5f))), FLOAT(0.f), SUB_F(p->widthF, FLOAT(1.f)));
        SpvId qy = P_CLAMP(FLOOR_F(ADD_F(ADD_F(yf, MUL_F(ny, along)), FLOAT(.5f))), FLOAT(0.f), SUB_F(p->heightF, FLOAT(1.f)));
        SpvId q = ADD_U(MUL_U(P_TOU(qy), p->width), P_TOU(qx));
        SpvId dx = SUB_F(qx, xf), dy = SUB_F(qy, yf), d = SQRT_F(ADD_F(MUL_F(dx, dx), MUL_F(dy, dy)));
        SpvId there = postPair(s, p, blur, MUL_U(q, UINT(2)));
        SpvId reachThere = MUL_F(MAX_F(SQRT_F(spvOp2(s, SPV_OP_Dot, s->floatType, there, there)), FLOAT(.5f)), FLOAT(.5f));
        SpvId zThere = postDistance(s, p, qx, qy);
        zThere = PICK_F(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, zThere, FLOAT(0.f)), FLOAT(1e6f), zThere);
        SpvId a = ADD_F(ADD_F(MUL_F(postInFront(s, zThere, z), postCone(s, d, reachThere)), MUL_F(postInFront(s, z, zThere), postCone(s, d, reach))),
                        MUL_F(MUL_F(postCylinder(s, d, reachThere), postCylinder(s, d, reach)), FLOAT(2.f)));
        weight = ADD_F(weight, a);
        sum = ADD_V3(sum, SCALE_V3(spvShuffle3(s, postVec4(s, p, p->light, q)), a));
    }
    SpvId blurred = SCALE_V3(sum, DIV_F(FLOAT(1.f), weight));
    SpvId blurredFrom = s->block;
    spvIfEnd(s, blurMerge);
    SpvId got = spvPhi(s, s->vec3, blurred, blurredFrom, nowRgb, before);
    postLightStore(s, p, p->scratch, i, vec4Of(s, got, W(now)));
POST_FINISH()

/* ---- temporal antialiasing: as temporalRow, into the second light ---- */
/* ---- lights sampled: what they gave gathered over frames (post.c's sampledRow). Binding 72: two vec4s a pixel of the
 * scene, the gathered light and how many frames, then how far along the view; the halves in turn. The push constant's
 * bit 0 is 1 when what is there is of the frame before, bit 1 the half this frame writes ---- */
POST_MAIN(artfShaderPostSampled)
    SpvId gathered = postBuffer(s, p, p->r.vec4Array, 72);
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId push = postPush(s, p), valid = spvOp2(s, SPV_OP_INotEqual, s->boolType, AND_U(push, UINT(1)), UINT(0)), half = AND_U(spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, push, UINT(1)), UINT(1));
    SpvId pixels = MUL_U(p->width, p->height);
    SpvId depth = postFloat(s, p, p->depth, i);
    SpvId along = PICK_F(P_MORE(depth, FLOAT(0.f)), postAlong(s, p, depth), FLOAT(0.f));
    SpvId now = postVec4(s, p, p->light, i);
    SpvId sx, sy, ok;
    postWasAt(s, p, xf, yf, &sx, &sy, &ok);
    SpvId inside = P_AND(P_AND(P_MORE(sx, FLOAT(-.5f)), P_MORE(sy, FLOAT(-.5f))), P_AND(P_LESS(sx, SUB_F(p->widthF, FLOAT(.5f))), P_LESS(sy, SUB_F(p->heightF, FLOAT(.5f)))));
    SpvId follows = P_AND(P_AND(valid, ok), P_AND(inside, P_MORE(along, FLOAT(0.f))));
    SpvId jx = P_TOU(P_CLAMP(FLOOR_F(ADD_F(sx, FLOAT(.5f))), FLOAT(0.f), SUB_F(p->widthF, FLOAT(1.f))));
    SpvId jy = P_TOU(P_CLAMP(FLOOR_F(ADD_F(sy, FLOAT(.5f))), FLOAT(0.f), SUB_F(p->heightF, FLOAT(1.f))));
    SpvId before = MUL_U(ADD_U(MUL_U(SUB_U(UINT(1), half), pixels), ADD_U(MUL_U(jy, p->width), jx)), UINT(2));
    SpvId was = bufferVec(s, &p->r, gathered, before), there = X(bufferVec(s, &p->r, gathered, ADD_U(before, UINT(1))));
    SpvId keeps = P_AND(follows, P_AND(P_MORE(W(was), FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, G_ABS(SUB_F(there, along)), MUL_F(FLOAT(.02f), along))));
    SpvId frames = PICK_F(keeps, MIN_F(ADD_F(W(was), FLOAT(1.f)), FLOAT(64.f)), FLOAT(1.f));
    SpvId light = spvShuffle3(s, now);
    SpvId mixed = ADD_V3(spvShuffle3(s, was), spvOp2(s, SPV_OP_FDiv, s->vec3, SUB_V3(light, spvShuffle3(s, was)), vec3Of(s, frames, frames, frames)));
    SpvId got = pick3(s, P_AND(keeps, P_MORE(frames, FLOAT(1.f))), mixed, light);
    SpvId mine = MUL_U(ADD_U(MUL_U(half, pixels), i), UINT(2));
    spvStore(s, spvAccess(s, p->r.ptrStorageVec4, gathered, (SpvId[]){p->r.zero, mine}, 2), vec4Of(s, got, frames));
    spvStore(s, spvAccess(s, p->r.ptrStorageVec4, gathered, (SpvId[]){p->r.zero, ADD_U(mine, UINT(1))}, 2), vec4Of(s, vec3Of(s, along, FLOAT(0.f), FLOAT(0.f)), FLOAT(0.f)));
    postLightStore(s, p, p->scratch, i, vec4Of(s, got, W(now)));
POST_FINISH()

/* What was gathered, across its neighbours: post.c's sampledSpaceRow. */
POST_MAIN(artfShaderPostSampledSpace)
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    static const float kernel[3] = {.375f, .25f, .0625f};
    SpvId depth = postFloat(s, p, p->depth, i), here = postVec4(s, p, p->light, i);
    SpvId along = postAlong(s, p, MAX_F(depth, FLOAT(1e-30f))), reach = ADD_F(MUL_F(along, FLOAT(.02f)), FLOAT(1e-3f));
    SpvId n = postNormal(s, postUint(s, p, p->surface, i));
    SpvId sum = same3(s, 0.f), total = FLOAT(0.f);
    SpvId xf = TO_F(x), yf = TO_F(y);
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            SpvId qxf = ADD_F(xf, FLOAT((float)dx)), qyf = ADD_F(yf, FLOAT((float)dy));
            SpvId within = P_AND(P_AND(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, qxf, FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, qyf, FLOAT(0.f))),
                                 P_AND(P_LESS(qxf, p->widthF), P_LESS(qyf, p->heightF)));
            SpvId j = ADD_U(MUL_U(P_TOU(P_CLAMP(qyf, FLOAT(0.f), SUB_F(p->heightF, FLOAT(1.f)))), p->width), P_TOU(P_CLAMP(qxf, FLOAT(0.f), SUB_F(p->widthF, FLOAT(1.f)))));
            SpvId qd = postFloat(s, p, p->depth, j);
            SpvId qn = postNormal(s, postUint(s, p, p->surface, j));
            SpvId apart = DIV_F(SUB_F(postAlong(s, p, MAX_F(qd, FLOAT(1e-30f))), along), reach);
            SpvId facing = MAX_F(ADD_F(ADD_F(MUL_F(X(n), X(qn)), MUL_F(Y(n), Y(qn))), MUL_F(Z(n), Z(qn))), FLOAT(0.f)), f2 = MUL_F(facing, facing);
            SpvId weight = DIV_F(MUL_F(FLOAT(kernel[dx < 0 ? -dx : dx] * kernel[dy < 0 ? -dy : dy]), MUL_F(f2, f2)), ADD_F(FLOAT(1.f), MUL_F(apart, apart)));
            weight = PICK_F(P_AND(within, P_MORE(qd, FLOAT(0.f))), weight, FLOAT(0.f));
            sum = ADD_V3(sum, SCALE_V3(spvShuffle3(s, postVec4(s, p, p->light, j)), weight));
            total = ADD_F(total, weight);
        }
    SpvId filtered = SCALE_V3(sum, DIV_F(FLOAT(1.f), MAX_F(total, FLOAT(1e-30f))));
    SpvId got = pick3(s, P_AND(P_MORE(depth, FLOAT(0.f)), P_MORE(total, FLOAT(0.f))), filtered, spvShuffle3(s, here));
    postLightStore(s, p, p->scratch, i, vec4Of(s, got, W(here)));
POST_FINISH()

/* post.c's reactiveAt: of the instance a plane's pixel shows, its material's ArtfMaterialDesc.reactive; 0 for nothing
 * drawn, a vector's word, or no reactive material in the frame. */
static SpvId postReactive(Spv* s, Post* p, SpvId plane) {
    SpvId on = P_AND(P_MORE(X(frameVec(s, &p->r, F_MORE)), FLOAT(0.f)), P_MORE(postFloat(s, p, p->depth, plane), FLOAT(0.f)));
    SpvId before = s->block, merge;
    spvIfBegin(s, on, &merge);
    SpvId who = postUint(s, p, p->surface, ADD_U(MUL_U(p->sceneW, p->sceneH), plane));
    SpvId valid = P_AND(spvOp2(s, SPV_OP_INotEqual, s->boolType, who, UINT(0)), spvOp2(s, 170 /* OpIEqual */, s->boolType, AND_U(who, UINT(ARTF_MOTION_VECTOR)), UINT(0)));
    SpvId which = SUB_U(spvGlsl2(s, 41 /* UMax */, s->uintType, spvSelect(s, s->uintType, valid, who, UINT(1)), UINT(1)), UINT(1));
    SpvId material = AS_UINT(Y(bufferVec(s, &p->r, p->r.instances, ADD_U(MUL_U(which, UINT(4)), UINT(3)))));
    SpvId row = ADD_U(ADD_U(MUL_U(AS_UINT(X(frameVec(s, &p->r, F_MATERIALS))), UINT(ARTF_MATERIAL_ROWS)), MUL_U(material, UINT(ARTF_MATERIAL_MORE_ROWS))), UINT(9));
    SpvId got = PICK_F(valid, X(bufferVec(s, &p->r, p->r.materials, row)), FLOAT(0.f)), from = s->block;
    spvIfEnd(s, merge);
    return spvPhi(s, s->floatType, got, from, FLOAT(0.f), before);
}

POST_MAIN(artfShaderPostTemporal)
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId lo = same3(s, 3e38f), hi = same3(s, -3e38f);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            SpvId qx = P_CLAMP(ADD_F(xf, FLOAT((float)dx)), FLOAT(0.f), SUB_F(p->widthF, FLOAT(1.f))), qy = P_CLAMP(ADD_F(yf, FLOAT((float)dy)), FLOAT(0.f), SUB_F(p->heightF, FLOAT(1.f)));
            SpvId c = spvShuffle3(s, postVec4(s, p, p->light, ADD_U(MUL_U(P_TOU(qy), p->width), P_TOU(qx))));
            lo = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, lo, c);
            hi = spvGlsl2(s, SPV_GLSL_FMax, s->vec3, hi, c);
        }
    SpvId sx, sy, ok;
    postWasAt(s, p, xf, yf, &sx, &sy, &ok);
    SpvId valid = P_AND(ok, P_AND(P_AND(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, sx, FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, sy, FLOAT(0.f))), P_AND(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, sx, SUB_F(p->widthF, FLOAT(1.f))), spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, sy, SUB_F(p->heightF, FLOAT(1.f))))));
    /* (the push constant is 1 when there is a frame before to follow) */
    valid = P_AND(valid, spvOp2(s, SPV_OP_INotEqual, s->boolType, postPush(s, p), UINT(0)));
    SpvId now = postVec4(s, p, p->light, i);
    SpvId was = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, spvShuffle3(s, postHistoryTap(s, p, sx, sy)), lo), hi);
    SpvId share = PICK_F(valid, FLOAT(.1f), FLOAT(1.f));
    SpvId mixed = P_MIX3(was, spvShuffle3(s, now), share);
    /* what is reactive takes that much more of what it shows now (post.c's temporalRow) */
    mixed = P_MIX3(mixed, spvShuffle3(s, now), postReactive(s, p, postPlane(s, p, xf, yf)));
    postLightStore(s, p, p->scratch, i, vec4Of(s, mixed, W(now)));
POST_FINISH()

/* ---- upscaling: as upscaleRow, from the scene's light into the second light at the frame's size. The push constant's
 * bit 0 is 1 when there is a frame before to follow; bit 1 says which half of the locks (71: two floats a pixel of the
 * frame, this frame's half and the one before's) this frame writes ---- */
static SpvId postLuma(Spv* s, SpvId c) { return DOT3(c, vec3Of(s, FLOAT(.2126f), FLOAT(.7152f), FLOAT(.0722f))); }
POST_MAIN(artfShaderPostUpscale)
    SpvId locks = postBuffer(s, p, p->r.floatArray, 71);
    SpvId main = spvBeginMain(s);
    postFrameSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId jitter = frameVec(s, &p->r, F_POST + 2);
    SpvId u = ADD_F(SUB_F(MUL_F(ADD_F(xf, FLOAT(.5f)), p->ratioX), FLOAT(.5f)), Z(jitter)), v = SUB_F(SUB_F(MUL_F(ADD_F(yf, FLOAT(.5f)), p->ratioY), FLOAT(.5f)), W(jitter));
    SpvId bx = FLOOR_F(u), by = FLOOR_F(v), fx = SUB_F(u, bx), fy = SUB_F(v, by);
    SpvId lastX = SUB_F(p->sceneWF, FLOAT(1.f)), lastY = SUB_F(p->sceneHF, FLOAT(1.f));
    SpvId x0 = P_TOU(P_CLAMP(bx, FLOAT(0.f), lastX)), x1 = P_TOU(P_CLAMP(ADD_F(bx, FLOAT(1.f)), FLOAT(0.f), lastX));
    SpvId y0 = MUL_U(P_TOU(P_CLAMP(by, FLOAT(0.f), lastY)), p->sceneW), y1 = MUL_U(P_TOU(P_CLAMP(ADD_F(by, FLOAT(1.f)), FLOAT(0.f), lastY)), p->sceneW);
    SpvId c00 = postVec4(s, p, p->light, ADD_U(y0, x0)), c10 = postVec4(s, p, p->light, ADD_U(y0, x1)), c01 = postVec4(s, p, p->light, ADD_U(y1, x0)), c11 = postVec4(s, p, p->light, ADD_U(y1, x1));
    SpvId gx = SUB_F(FLOAT(1.f), fx), gy = SUB_F(FLOAT(1.f), fy);
    SpvId spatial = ADD_V4(ADD_V4(SCALE_V4(c00, MUL_F(gx, gy)), SCALE_V4(c10, MUL_F(fx, gy))), ADD_V4(SCALE_V4(c01, MUL_F(gx, fy)), SCALE_V4(c11, MUL_F(fx, fy))));
    SpvId sx, sy, ok;
    postWasAt(s, p, xf, yf, &sx, &sy, &ok);
    SpvId push = postPush(s, p), half = AND_U(spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, push, UINT(1)), UINT(1));
    SpvId framePixels = MUL_U(p->width, p->height);
    SpvId follows = P_AND(P_AND(ok, spvOp2(s, SPV_OP_INotEqual, s->boolType, AND_U(push, UINT(1)), UINT(0))), P_MORE(W(frameVec(s, &p->r, F_POST + 1)), FLOAT(0.f)));
    follows = P_AND(follows, P_AND(P_AND(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, sx, FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, sy, FLOAT(0.f))),
                                   P_AND(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, sx, SUB_F(p->widthF, FLOAT(1.f))), spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, sy, SUB_F(p->heightF, FLOAT(1.f))))));
    /* (only what follows reads the frame before) */
    SpvId spatialRgb = spvShuffle3(s, spatial);
    SpvId before = s->block, followMerge;
    spvIfBegin(s, follows, &followMerge);
    /* each sample's nearness to the center, in pixels of the frame */
    SpvId ox0 = DIV_F(fx, p->ratioX), ox1 = DIV_F(gx, p->ratioX), oy0 = DIV_F(fy, p->ratioY), oy1 = DIV_F(gy, p->ratioY);
    const SpvId d2[4] = {ADD_F(MUL_F(ox0, ox0), MUL_F(oy0, oy0)), ADD_F(MUL_F(ox1, ox1), MUL_F(oy0, oy0)), ADD_F(MUL_F(ox0, ox0), MUL_F(oy1, oy1)), ADD_F(MUL_F(ox1, ox1), MUL_F(oy1, oy1))};
    const SpvId c[4] = {c00, c10, c01, c11};
    SpvId near = FLOAT(0.f), fresh = same3(s, 0.f);
    for (int k = 0; k < 4; ++k) {
        SpvId one = SUB_F(FLOAT(1.f), d2[k]);
        SpvId w = PICK_F(P_LESS(d2[k], FLOAT(1.f)), MUL_F(one, one), FLOAT(0.f));
        near = ADD_F(near, w);
        fresh = ADD_V3(fresh, SCALE_V3(spvShuffle3(s, c[k]), w));
    }
    SpvId gain = DIV_F(FLOAT(.1f), MUL_F(p->ratioX, p->ratioY));
    SpvId share = MIN_F(MUL_F(near, gain), FLOAT(1.f)), each = PICK_F(P_MORE(near, FLOAT(0.f)), DIV_F(FLOAT(1.f), MAX_F(near, FLOAT(1e-30f))), FLOAT(0.f));
    /* the box, of the nearest sample's neighbours: their least and most, held to their mean and three quarters of their
     * deviation (boxLanes); and whether the nearest is a ridge */
    SpvId nearX = PICK_F(P_LESS(fx, FLOAT(.5f)), bx, ADD_F(bx, FLOAT(1.f))), nearY = PICK_F(P_LESS(fy, FLOAT(.5f)), by, ADD_F(by, FLOAT(1.f)));
    SpvId lo = same3(s, 3e38f), hi = same3(s, -3e38f), sum = same3(s, 0.f), squares = same3(s, 0.f);
    SpvId lumas[3][3];
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            SpvId qx = P_TOU(P_CLAMP(ADD_F(nearX, FLOAT((float)dx)), FLOAT(0.f), lastX)), qy = P_TOU(P_CLAMP(ADD_F(nearY, FLOAT((float)dy)), FLOAT(0.f), lastY));
            SpvId q = spvShuffle3(s, postVec4(s, p, p->light, ADD_U(MUL_U(qy, p->sceneW), qx)));
            lo = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, lo, q);
            hi = spvGlsl2(s, SPV_GLSL_FMax, s->vec3, hi, q);
            sum = ADD_V3(sum, q);
            squares = ADD_V3(squares, MUL_V3(q, q));
            lumas[dy + 1][dx + 1] = postLuma(s, q);
        }
    SpvId mean = SCALE_V3(sum, FLOAT(1.f / 9.f));
    SpvId deviation = spvGlsl1(s, SPV_GLSL_Sqrt, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, SUB_V3(SCALE_V3(squares, FLOAT(1.f / 9.f)), MUL_V3(mean, mean)), same3(s, 0.f)));
    lo = spvGlsl2(s, SPV_GLSL_FMax, s->vec3, lo, SUB_V3(mean, SCALE_V3(deviation, FLOAT(.75f))));
    hi = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, hi, ADD_V3(mean, SCALE_V3(deviation, FLOAT(.75f))));
    SpvId l = lumas[1][1], a1 = SUB_F(l, lumas[1][0]), a2 = SUB_F(l, lumas[1][2]), b1 = SUB_F(l, lumas[0][1]), b2 = SUB_F(l, lumas[2][1]);
    SpvId across = MUL_F(MAX_F(MAX_F(l, lumas[1][0]), lumas[1][2]), FLOAT(.25f)), along = MUL_F(MAX_F(MAX_F(l, lumas[0][1]), lumas[2][1]), FLOAT(.25f));
    SpvId ridge = P_OR(P_AND(P_MORE(MUL_F(a1, a2), FLOAT(0.f)), P_AND(P_MORE(G_ABS(a1), across), P_MORE(G_ABS(a2), across))),
                       P_AND(P_MORE(MUL_F(b1, b2), FLOAT(0.f)), P_AND(P_MORE(G_ABS(b1), along), P_MORE(G_ABS(b2), along))));
    /* the lock: a ridge here, or what it was where the pixel was, worn (upscaleRow) */
    SpvId lockAt = ADD_U(MUL_U(P_TOU(FLOOR_F(ADD_F(sy, FLOAT(.5f)))), p->width), P_TOU(FLOOR_F(ADD_F(sx, FLOAT(.5f)))));
    SpvId lockWas = postFloat(s, p, locks, ADD_U(MUL_U(SUB_U(UINT(1), half), framePixels), spvGlsl2(s, 38 /* UMin */, s->uintType, lockAt, SUB_U(framePixels, UINT(1)))));
    SpvId lock = PICK_F(ridge, FLOAT(1.f), MAX_F(SUB_F(lockWas, FLOAT(1.f / 24.f)), FLOAT(0.f)));
    SpvId had = spvShuffle3(s, postHistoryTap(s, p, sx, sy));
    SpvId kept = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, had, lo), hi);
    SpvId was = ADD_V3(kept, SCALE_V3(SUB_V3(had, kept), lock));
    SpvId fresher = SCALE_V3(fresh, each);
    /* (each by 1 / (1 + its luma)) */
    SpvId byNow = DIV_F(share, ADD_F(FLOAT(1.f), postLuma(s, fresher))), byWas = DIV_F(SUB_F(FLOAT(1.f), share), ADD_F(FLOAT(1.f), postLuma(s, was)));
    SpvId gathered = P_MIX3(was, fresher, DIV_F(byNow, ADD_F(byNow, byWas)));
    /* what is reactive takes that much more of the scene read between its pixels now (post.c's upscaleRow) */
    gathered = P_MIX3(gathered, spatialRgb, postReactive(s, p, postPlane(s, p, xf, yf)));
    SpvId gatheredFrom = s->block;
    spvIfEnd(s, followMerge);
    SpvId got = spvPhi(s, s->vec3, gathered, gatheredFrom, spatialRgb, before);
    SpvId locked = spvPhi(s, s->floatType, lock, gatheredFrom, FLOAT(0.f), before);
    postLightStore(s, p, p->scratch, i, vec4Of(s, got, W(spatial)));
    spvStore(s, postPtr(s, p, locks, s->floatType, ADD_U(MUL_U(half, framePixels), i)), locked);
POST_FINISH()

/* artfGradeCct: grading in ACEScct, of the light before the curve; nothing when shownSpace (and nothing asked). */
static SpvId postLog2(Spv* s, SpvId x);
static SpvId postExp2(Spv* s, SpvId x);
static SpvId postExposureSlot(Spv* s, Post* p);
static SpvId cctOf(Spv* s, SpvId x) { return PICK_F(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, x, FLOAT(.0078125f)), ADD_F(MUL_F(FLOAT(10.5402377f), x), FLOAT(.0729055342f)), MUL_F(ADD_F(postLog2(s, x), FLOAT(9.72f)), FLOAT(1.f / 17.52f))); }
static SpvId cctLinear(Spv* s, SpvId c) { return PICK_F(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, c, FLOAT(.155251142f)), MUL_F(SUB_F(c, FLOAT(.0729055342f)), FLOAT(1.f / 10.5402377f)), postExp2(s, SUB_F(MUL_F(c, FLOAT(17.52f)), FLOAT(9.72f)))); }
static SpvId gradeCct(Spv* s, Post* p, SpvId v, SpvId shownSpace) {
    SpvId g0 = frameVec(s, &p->r, F_GRADE2), g1 = frameVec(s, &p->r, F_GRADE2 + 1), g2 = frameVec(s, &p->r, F_GRADE2 + 2);
    SpvId g3 = frameVec(s, &p->r, F_GRADE2 + 3), g4 = frameVec(s, &p->r, F_GRADE2 + 4), g5 = frameVec(s, &p->r, F_GRADE2 + 5);
    SpvId curves = P_MORE(W(g0), FLOAT(0.f)), side = W(g3), looked = P_MORE(side, FLOAT(1.5f));
    SpvId asked = P_AND(P_NOT(shownSpace), P_OR(curves, looked));
    SpvId before = s->block, merge;
    spvIfBegin(s, asked, &merge);
    SpvId x = X(v), y = Y(v), z = Z(v);
    SpvId a[3] = {ADD_F(ADD_F(MUL_F(FLOAT(.6130974f), x), MUL_F(FLOAT(.3395231f), y)), MUL_F(FLOAT(.0473795f), z)),
                  ADD_F(ADD_F(MUL_F(FLOAT(.0701937f), x), MUL_F(FLOAT(.9163539f), y)), MUL_F(FLOAT(.0134524f), z)),
                  ADD_F(ADD_F(MUL_F(FLOAT(.0206156f), x), MUL_F(FLOAT(.1095698f), y)), MUL_F(FLOAT(.8698147f), z))};
    SpvId c[3];
    for (uint32_t k = 0; k < 3; ++k) c[k] = cctOf(s, a[k]);
    SpvId graded[3];
    for (uint32_t k = 0; k < 3; ++k) {
        SpvId o = MUL_F(spvExtract(s, s->floatType, g2, k), ADD_F(c[k], MUL_F(spvExtract(s, s->floatType, g0, k), SUB_F(FLOAT(1.f), c[k]))));
        SpvId inverse = spvExtract(s, s->floatType, g1, k);
        graded[k] = PICK_F(spvOp2(s, 180 /* OpFOrdEqual */, s->boolType, inverse, FLOAT(1.f)), o, PICK_F(P_MORE(o, FLOAT(0.f)), postExp2(s, MUL_F(postLog2(s, o), inverse)), FLOAT(0.f)));
    }
    SpvId l = P_CLAMP(ADD_F(ADD_F(MUL_F(FLOAT(.2126f), graded[0]), MUL_F(FLOAT(.7152f), graded[1])), MUL_F(FLOAT(.0722f), graded[2])), FLOAT(0.f), FLOAT(1.f));
    SpvId rest = SUB_F(FLOAT(1.f), l);
    SpvId dark = MUL_F(rest, rest), middle = MUL_F(MUL_F(FLOAT(2.f), l), rest), bright = MUL_F(l, l);
    for (uint32_t k = 0; k < 3; ++k) {
        SpvId wheel = ADD_F(ADD_F(MUL_F(dark, spvExtract(s, s->floatType, g3, k)), MUL_F(middle, spvExtract(s, s->floatType, g4, k))), MUL_F(bright, spvExtract(s, s->floatType, g5, k)));
        c[k] = PICK_F(curves, MUL_F(graded[k], wheel), c[k]);
    }
    /* the table, in ACEScct: as the shown one's, from bloom's buffer */
    {
        SpvId n = P_TOU(MAX_F(side, FLOAT(2.f))), lutStart = ADD_U(postExposureSlot(s, p), UINT(1));
        SpvId at[3], t[3];
        for (uint32_t k = 0; k < 3; ++k) {
            SpvId qk = MUL_F(P_CLAMP(c[k], FLOAT(0.f), FLOAT(1.f)), SUB_F(MAX_F(side, FLOAT(2.f)), FLOAT(1.f)));
            at[k] = spvGlsl2(s, SPV_GLSL_UMin, s->uintType, P_TOU(qk), SUB_U(n, UINT(2)));
            t[k] = SUB_F(qk, TO_F(at[k]));
        }
        SpvId base = ADD_U(lutStart, ADD_U(MUL_U(ADD_U(MUL_U(at[2], n), at[1]), n), at[0]));
        SpvId corners[8];
        for (uint32_t k = 0; k < 8; ++k) {
            SpvId i = base;
            if (k & 1) i = ADD_U(i, UINT(1));
            if (k & 2) i = ADD_U(i, n);
            if (k & 4) i = ADD_U(i, MUL_U(n, n));
            corners[k] = spvShuffle3(s, postVec4(s, p, p->bloom, i));
        }
#define LERP3(a, b, f) ADD_V3(a, SCALE_V3(SUB_V3(b, a), f))
        SpvId a0 = LERP3(corners[0], corners[1], t[0]), b0 = LERP3(corners[2], corners[3], t[0]);
        SpvId d0 = LERP3(corners[4], corners[5], t[0]), e0 = LERP3(corners[6], corners[7], t[0]);
        SpvId found = LERP3(LERP3(a0, b0, t[1]), LERP3(d0, e0, t[1]), t[2]);
#undef LERP3
        SpvId amount = W(g4);
        for (uint32_t k = 0; k < 3; ++k) c[k] = PICK_F(looked, ADD_F(c[k], MUL_F(SUB_F(spvExtract(s, s->floatType, found, k), c[k]), amount)), c[k]);
    }
    for (uint32_t k = 0; k < 3; ++k) c[k] = MAX_F(cctLinear(s, c[k]), FLOAT(0.f));
    SpvId back = vec3Of(s, ADD_F(ADD_F(MUL_F(FLOAT(1.7048587f), c[0]), MUL_F(FLOAT(-.6217160f), c[1])), MUL_F(FLOAT(-.0832542f), c[2])),
                        ADD_F(ADD_F(MUL_F(FLOAT(-.1300768f), c[0]), MUL_F(FLOAT(1.1407357f), c[1])), MUL_F(FLOAT(-.0105536f), c[2])),
                        ADD_F(ADD_F(MUL_F(FLOAT(-.0239640f), c[0]), MUL_F(FLOAT(-.1289755f), c[1])), MUL_F(FLOAT(1.1530393f), c[2])));
    back = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, back, same3(s, 0.f)), same3(s, 65504.f));
    SpvId from = s->block;
    spvIfEnd(s, merge);
    return spvPhi(s, s->vec3, back, from, v, before);
}

/* ---- sharpening: as sharpenRow, into the second light ---- */
POST_MAIN(artfShaderPostSharpen)
    SpvId main = spvBeginMain(s);
    postFrameSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId amount = Z(frameVec(s, &p->r, F_OUTPUT));
    SpvId lastX = SUB_U(p->width, UINT(1)), lastY = SUB_U(p->height, UINT(1));
    SpvId up = spvSelect(s, s->uintType, spvOp2(s, 170, s->boolType, y, UINT(0)), y, SUB_U(y, UINT(1))), down = spvGlsl2(s, 38, s->uintType, ADD_U(y, UINT(1)), lastY);
    SpvId left = spvSelect(s, s->uintType, spvOp2(s, 170, s->boolType, x, UINT(0)), x, SUB_U(x, UINT(1))), right = spvGlsl2(s, 38, s->uintType, ADD_U(x, UINT(1)), lastX);
    SpvId here = postVec4(s, p, p->light, i);
    const SpvId around[4] = {postVec4(s, p, p->light, ADD_U(MUL_U(up, p->width), x)), postVec4(s, p, p->light, ADD_U(MUL_U(down, p->width), x)),
                             postVec4(s, p, p->light, ADD_U(MUL_U(y, p->width), left)), postVec4(s, p, p->light, ADD_U(MUL_U(y, p->width), right))};
    SpvId h = spvShuffle3(s, here), lo = h, hi = h, sum = same3(s, 0.f);
    for (int k = 0; k < 4; ++k) {
        SpvId t = spvShuffle3(s, around[k]);
        lo = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, lo, t);
        hi = spvGlsl2(s, SPV_GLSL_FMax, s->vec3, hi, t);
        sum = ADD_V3(sum, t);
    }
    SpvId sharper = ADD_V3(h, SCALE_V3(SUB_V3(h, SCALE_V3(sum, FLOAT(.25f))), amount));
    sharper = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, sharper, lo), hi);
    postLightStore(s, p, p->scratch, i, vec4Of(s, sharper, W(here)));
POST_FINISH()

/* ---- depth of field: as focusRow, into the second light ---- */
static SpvId postConfusion(Spv* s, Post* p, SpvId d) {
    SpvId post = frameVec(s, &p->r, F_POST + 2);
    d = PICK_F(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, d, FLOAT(0.f)), FLOAT(1e6f), d);
    return MIN_F(MUL_F(Y(post), ABS_F(SUB_F(FLOAT(1.f), DIV_F(X(post), d)))), FLOAT(24.f));
}

POST_MAIN(artfShaderPostFocus)
    static const float disc[24][2] = {{.33f, 0}, {-.17f, .29f}, {-.17f, -.29f}, {.62f, .17f}, {.17f, .62f}, {-.45f, .45f}, {-.62f, -.17f}, {-.17f, -.62f}, {.45f, -.45f},
                                      {.95f, 0}, {.82f, .48f}, {.48f, .82f}, {0, .95f}, {-.48f, .82f}, {-.82f, .48f}, {-.95f, 0}, {-.82f, -.48f}, {-.48f, -.82f},
                                      {0, -.95f}, {.48f, -.82f}, {.82f, -.48f}, {.3f, .3f}, {-.3f, .1f}, {.1f, -.3f}};
    SpvId main = spvBeginMain(s);
    postFrameSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId mine = postConfusion(s, p, postDistance(s, p, xf, yf));
    SpvId here = postVec4(s, p, p->light, i);
    SpvId sum = spvShuffle3(s, here), weight = FLOAT(1.f);
    for (int k = 0; k < 24; ++k) {
        SpvId qx = P_CLAMP(FLOOR_F(ADD_F(ADD_F(xf, MUL_F(FLOAT(disc[k][0]), mine)), FLOAT(.5f))), FLOAT(0.f), SUB_F(p->widthF, FLOAT(1.f)));
        SpvId qy = P_CLAMP(FLOOR_F(ADD_F(ADD_F(yf, MUL_F(FLOAT(disc[k][1]), mine)), FLOAT(.5f))), FLOAT(0.f), SUB_F(p->heightF, FLOAT(1.f)));
        SpvId theirs = postConfusion(s, p, postDistance(s, p, qx, qy));
        SpvId distance = MUL_F(FLOAT(sqrtf(disc[k][0] * disc[k][0] + disc[k][1] * disc[k][1])), mine);
        SpvId w = PICK_F(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, theirs, distance), FLOAT(1.f), P_CLAMP(DIV_F(theirs, MAX_F(distance, FLOAT(1e-3f))), FLOAT(0.f), FLOAT(1.f)));
        sum = ADD_V3(sum, SCALE_V3(spvShuffle3(s, postVec4(s, p, p->light, ADD_U(MUL_U(P_TOU(qy), p->width), P_TOU(qx)))), w));
        weight = ADD_F(weight, w);
    }
    SpvId blurred = SCALE_V3(sum, DIV_F(FLOAT(1.f), weight));
    postLightStore(s, p, p->scratch, i, vec4Of(s, pick3(s, P_LESS(mine, FLOAT(.5f)), spvShuffle3(s, here), blurred), W(here)));
POST_FINISH()

/* ---- bloom: as bloomDownRow and bloomUpRow; the push constant is the level ---- */

/* A level's size and where it starts, from the frame's size: halves, rounded up. */
static void postLevel(Spv* s, Post* p, SpvId level, SpvId* start, SpvId* w, SpvId* h) {
    SpvId at = UINT(0), lw = p->width, lh = p->height;
    *start = UINT(0);
    *w = UINT(1);
    *h = UINT(1);
    for (uint32_t l = 0; l < POST_BLOOM_LEVELS; ++l) {
        lw = spvGlsl2(s, 41 /* UMax */, s->uintType, spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(lw, UINT(1)), UINT(2)), UINT(1));
        lh = spvGlsl2(s, 41, s->uintType, spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(lh, UINT(1)), UINT(2)), UINT(1));
        SpvId is = spvOp2(s, 170, s->boolType, level, UINT(l));
        *start = spvSelect(s, s->uintType, is, at, *start);
        *w = spvSelect(s, s->uintType, is, lw, *w);
        *h = spvSelect(s, s->uintType, is, lh, *h);
        at = ADD_U(at, MUL_U(lw, lh));
    }
}

/* Where auto exposure keeps its state: the vec4 past bloom's last level (stops, 2 to that). */
static SpvId postExposureSlot(Spv* s, Post* p) {
    SpvId at = UINT(0), lw = p->width, lh = p->height;
    for (uint32_t l = 0; l < POST_BLOOM_LEVELS; ++l) {
        lw = spvGlsl2(s, 41 /* UMax */, s->uintType, spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(lw, UINT(1)), UINT(2)), UINT(1));
        lh = spvGlsl2(s, 41, s->uintType, spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(lh, UINT(1)), UINT(2)), UINT(1));
        at = ADD_U(at, MUL_U(lw, lh));
    }
    return at;
}

/* As post.c's exposureOf: the scene's (with auto exposure, or 1) times what auto exposure got to. */
static SpvId postExposure(Spv* s, Post* p) {
    SpvId base = X(frameVec(s, &p->r, F_LOOK));
    SpvId on = P_MORE(Y(frameVec(s, &p->r, F_GRADE)), FLOAT(0.f));
    /* (its state is read only when it is on) */
    SpvId before = s->block, merge;
    spvIfBegin(s, on, &merge);
    SpvId scale = Y(postVec4(s, p, p->bloom, postExposureSlot(s, p)));
    SpvId read = s->block;
    spvIfEnd(s, merge);
    scale = spvPhi(s, s->floatType, scale, read, FLOAT(1.f), before);
    return PICK_F(on, MUL_F(PICK_F(P_MORE(base, FLOAT(0.f)), base, FLOAT(1.f)), scale), base);
}

#define P_BITS_F(u) spvOp1(s, SPV_OP_Bitcast, s->floatType, u)
/* artfLog2Fast and artfExp2Fast, step for step. */
static SpvId postLog2(Spv* s, SpvId x) {
    SpvId bits = AS_UINT(x);
    SpvId e = SUB_F(TO_F(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, bits, UINT(23)), UINT(255))), FLOAT(127.f));
    SpvId m = P_BITS_F(spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, bits, UINT(0x7FFFFFu)), UINT(0x3F800000u)));
    SpvId z = DIV_F(SUB_F(m, FLOAT(1.f)), ADD_F(m, FLOAT(1.f))), z2 = MUL_F(z, z);
    SpvId series = ADD_F(FLOAT(1.f), MUL_F(z2, ADD_F(FLOAT(.33333333f), MUL_F(z2, ADD_F(FLOAT(.2f), MUL_F(z2, FLOAT(.14285714f)))))));
    return ADD_F(e, MUL_F(MUL_F(z, FLOAT(2.8853901f)), series));
}
static SpvId postExp2(Spv* s, SpvId x) {
    x = P_CLAMP(x, FLOAT(-100.f), FLOAT(100.f));
    SpvId whole = FLOOR_F(x);
    SpvId u = MUL_F(SUB_F(x, whole), FLOAT(.69314718f));
    SpvId f = FLOAT(.0013888889f);
    const float c[6] = {.0083333333f, .041666668f, .16666667f, .5f, 1.f, 1.f};
    for (int k = 0; k < 6; ++k) f = ADD_F(FLOAT(c[k]), MUL_F(u, f));
    SpvId i = spvOp1(s, SPV_OP_ConvertFToS, s->intType, whole);
    SpvId bits = spvOp2(s, SPV_OP_ShiftLeftLogical, s->uintType, spvOp1(s, SPV_OP_Bitcast, s->uintType, spvOp2(s, SPV_OP_IAdd, s->intType, i, spvConstInt(s, 127))), UINT(23));
    return MUL_F(f, P_BITS_F(bits));
}

/* A loop of count turns, its body between the two calls; counter: a uint variable of the function's. */
typedef struct PostLoop { SpvId counter, header, next, done; } PostLoop;
static SpvId postLoopBegin(Spv* s, PostLoop* l, SpvId counter, SpvId count) {
    l->counter = counter;
    spvStore(s, counter, UINT(0));
    l->header = spvNewId(s);
    l->next = spvNewId(s);
    l->done = spvNewId(s);
    SpvId test = spvNewId(s), body = spvNewId(s);
    spvBranch(s, l->header);
    spvLabel(s, l->header);
    spvLoopMerge(s, l->done, l->next);
    spvBranch(s, test);
    spvLabel(s, test);
    SpvId k = spvLoad(s, s->uintType, counter);
    spvBranchIf(s, P_LESS_U(k, count), body, l->done);
    spvLabel(s, body);
    return k;
}
static void postLoopEnd(Spv* s, PostLoop* l) {
    spvBranch(s, l->next);
    spvLabel(s, l->next);
    spvStore(s, l->counter, ADD_U(spvLoad(s, s->uintType, l->counter), UINT(1)));
    spvBranch(s, l->header);
    spvLabel(s, l->done);
}

/* As post.c's meterPoint: the weight of a point of the frame (u, v from 0 to 1), 0 where nothing covers it, and its
 * luminance in stops. */
static SpvId postMeterPoint(Spv* s, Post* p, SpvId u, SpvId v, SpvId* stops) {
    SpvId at = ADD_U(MUL_U(P_TOU(MUL_F(v, p->heightF)), p->width), P_TOU(MUL_F(u, p->widthF)));
    SpvId light = postVec4(s, p, p->light, at);
    SpvId alpha = P_CLAMP(W(light), FLOAT(0.f), FLOAT(1.f));
    SpvId covered = P_MORE(alpha, FLOAT(0.f));
    SpvId luminance = DIV_F(DOT3(spvShuffle3(s, light), vec3Of(s, FLOAT(.2126f), FLOAT(.7152f), FLOAT(.0722f))), MAX_F(alpha, FLOAT(1e-6f)));
    SpvId du = SUB_F(MUL_F(u, FLOAT(2.f)), FLOAT(1.f)), dv = SUB_F(MUL_F(v, FLOAT(2.f)), FLOAT(1.f));
    *stops = postLog2(s, P_CLAMP(luminance, FLOAT(1.f / 4096.f), FLOAT(4096.f)));
    return PICK_F(covered, MUL_F(alpha, SUB_F(FLOAT(1.f), MUL_F(FLOAT(.25f), ADD_F(MUL_F(du, du), MUL_F(dv, dv))))), FLOAT(0.f));
}

/* ---- auto exposure: as post.c's meter, into the slot past bloom's levels. The mean, by one invocation; a histogram,
 * by all 64 of the group, each a column of the grid, into bins of whole numbers that they add to at once (so that the
 * order does not matter, and the sums are the CPU's), then walked by one. The push constant is 1 when there is a state
 * to go on from ---- */
static void postBarrier(Spv* s) { spvEmit(&s->code, 224 /* OpControlBarrier */, SPV_WORDS(UINT(2), UINT(2), UINT(0x108))); /* the group's, acquire and release, its memory */ }
static void postSharedAdd(Spv* s, SpvId pointer, SpvId value) {
    spvEmit(&s->code, 234 /* OpAtomicIAdd */, SPV_WORDS(s->uintType, spvNewId(s), pointer, UINT(2) /* Workgroup */, UINT(0), value));
}

uint32_t* artfShaderPostMeter(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    Post post, *p = &post;
    postDeclare(s, p);
    SpvId binsType = spvTypeArray(s, s->uintType, ARTF_HISTOGRAM_BINS, 0);
    SpvId ptrShared = spvTypePointer(s, 4 /* Workgroup */, s->uintType);
    SpvId binWeight = spvVariable(s, spvTypePointer(s, 4, binsType), 4), binStops = spvVariable(s, spvTypePointer(s, 4, binsType), 4);
    SpvId main = spvBeginMain(s);
    SpvId ptrFnF = spvTypePointer(s, SPV_STORAGE_Function, s->floatType), ptrFnU = spvTypePointer(s, SPV_STORAGE_Function, s->uintType);
    SpvId sumVar = spvVariable(s, ptrFnF, SPV_STORAGE_Function), weightVar = spvVariable(s, ptrFnF, SPV_STORAGE_Function), counter = spvVariable(s, ptrFnU, SPV_STORAGE_Function);
    SpvId beforeVar = spvVariable(s, ptrFnF, SPV_STORAGE_Function), allVar = spvVariable(s, ptrFnU, SPV_STORAGE_Function);
    postFrameSizes(s, p);
    SpvId i = spvExtract(s, s->uintType, spvLoad(s, p->uvec3, p->global), 0);
    SpvId meter = frameVec(s, &p->r, F_GRADE2 + 6);
    SpvId histogram = P_MORE(W(meter), FLOAT(0.f));
    PostLoop loop;
    /* the histogram's bins, by all (the condition is the same for all of them) */
    SpvId fillMerge;
    spvIfBegin(s, histogram, &fillMerge);
    {
        spvStore(s, spvAccess(s, ptrShared, binWeight, &i, 1), UINT(0));
        spvStore(s, spvAccess(s, ptrShared, binStops, &i, 1), UINT(0));
        postBarrier(s);
        SpvId gy = postLoopBegin(s, &loop, counter, UINT(ARTF_HISTOGRAM_Y));
        {
            SpvId u = DIV_F(ADD_F(TO_F(i), FLOAT(.5f)), FLOAT((float)ARTF_HISTOGRAM_X)), v = DIV_F(ADD_F(TO_F(gy), FLOAT(.5f)), FLOAT((float)ARTF_HISTOGRAM_Y));
            SpvId stops, weight = postMeterPoint(s, p, u, v, &stops);
            SpvId bin = spvGlsl2(s, SPV_GLSL_UMin, s->uintType, P_TOU(MUL_F(MAX_F(ADD_F(stops, FLOAT(12.f)), FLOAT(0.f)), FLOAT((float)ARTF_HISTOGRAM_BINS / 24.f))), UINT(ARTF_HISTOGRAM_BINS - 1));
            SpvId whole = P_TOU(ADD_F(MUL_F(weight, FLOAT(65536.f)), FLOAT(.5f)));
            SpvId timesStops = spvOp1(s, SPV_OP_Bitcast, s->uintType, spvOp1(s, SPV_OP_ConvertFToS, s->intType, FLOOR_F(ADD_F(MUL_F(MUL_F(weight, stops), FLOAT(4096.f)), FLOAT(.5f)))));
            postSharedAdd(s, spvAccess(s, ptrShared, binWeight, &bin, 1), whole);
            postSharedAdd(s, spvAccess(s, ptrShared, binStops, &bin, 1), timesStops);
        }
        postLoopEnd(s, &loop);
        postBarrier(s);
    }
    spvIfEnd(s, fillMerge);
    SpvId merge;
    spvIfBegin(s, spvOp2(s, 170 /* OpIEqual */, s->boolType, i, UINT(0)), &merge);
    spvStore(s, sumVar, FLOAT(0.f));
    spvStore(s, weightVar, FLOAT(0.f));
    /* the mean of all */
    SpvId plainMerge;
    spvIfBegin(s, P_NOT(histogram), &plainMerge);
    {
        SpvId k = postLoopBegin(s, &loop, counter, UINT(ARTF_METER_X * ARTF_METER_Y));
        SpvId gx = spvOp2(s, SPV_OP_UMod, s->uintType, k, UINT(ARTF_METER_X)), gy = spvOp2(s, SPV_OP_UDiv, s->uintType, k, UINT(ARTF_METER_X));
        SpvId u = DIV_F(ADD_F(TO_F(gx), FLOAT(.5f)), FLOAT((float)ARTF_METER_X)), v = DIV_F(ADD_F(TO_F(gy), FLOAT(.5f)), FLOAT((float)ARTF_METER_Y));
        SpvId stops, weight = postMeterPoint(s, p, u, v, &stops);
        spvStore(s, sumVar, ADD_F(spvLoad(s, s->floatType, sumVar), MUL_F(weight, stops)));
        spvStore(s, weightVar, ADD_F(spvLoad(s, s->floatType, weightVar), weight));
        postLoopEnd(s, &loop);
    }
    spvIfEnd(s, plainMerge);
    /* the histogram walked: what is left of each bin between the shares left out */
    SpvId walkMerge;
    spvIfBegin(s, histogram, &walkMerge);
    {
        spvStore(s, allVar, UINT(0));
        SpvId k = postLoopBegin(s, &loop, counter, UINT(ARTF_HISTOGRAM_BINS));
        spvStore(s, allVar, ADD_U(spvLoad(s, s->uintType, allVar), spvLoad(s, s->uintType, spvAccess(s, ptrShared, binWeight, &k, 1))));
        postLoopEnd(s, &loop);
        SpvId all = MUL_F(TO_F(spvLoad(s, s->uintType, allVar)), FLOAT(1.f / 65536.f));
        SpvId from = MUL_F(all, X(meter)), to = MUL_F(all, SUB_F(FLOAT(1.f), Y(meter)));
        spvStore(s, beforeVar, FLOAT(0.f));
        k = postLoopBegin(s, &loop, counter, UINT(ARTF_HISTOGRAM_BINS));
        {
            SpvId w = MUL_F(TO_F(spvLoad(s, s->uintType, spvAccess(s, ptrShared, binWeight, &k, 1))), FLOAT(1.f / 65536.f));
            SpvId st = MUL_F(spvOp1(s, 111 /* OpConvertSToF */, s->floatType, spvOp1(s, SPV_OP_Bitcast, s->intType, spvLoad(s, s->uintType, spvAccess(s, ptrShared, binStops, &k, 1)))), FLOAT(1.f / 4096.f));
            SpvId before = spvLoad(s, s->floatType, beforeVar);
            SpvId kept = SUB_F(MIN_F(ADD_F(before, w), to), MAX_F(before, from));
            SpvId ok = P_AND(P_MORE(kept, FLOAT(0.f)), P_MORE(w, FLOAT(0.f)));
            spvStore(s, sumVar, ADD_F(spvLoad(s, s->floatType, sumVar), PICK_F(ok, MUL_F(st, DIV_F(kept, MAX_F(w, FLOAT(1e-30f)))), FLOAT(0.f))));
            spvStore(s, weightVar, ADD_F(spvLoad(s, s->floatType, weightVar), PICK_F(ok, kept, FLOAT(0.f))));
            spvStore(s, beforeVar, ADD_F(before, w));
        }
        postLoopEnd(s, &loop);
    }
    spvIfEnd(s, walkMerge);
    {
        SpvId grade0 = frameVec(s, &p->r, F_GRADE), grade1 = frameVec(s, &p->r, F_GRADE + 1);
        SpvId slot = postPtr(s, p, p->bloom, s->vec4, postExposureSlot(s, p));
        SpvId was = X(spvLoad(s, s->vec4, slot));
        SpvId valid = spvOp2(s, SPV_OP_INotEqual, s->boolType, postPush(s, p), UINT(0));
        SpvId sum = spvLoad(s, s->floatType, sumVar), weights = spvLoad(s, s->floatType, weightVar);
        SpvId target = PICK_F(P_MORE(weights, FLOAT(0.f)), SUB_F(Z(grade0), DIV_F(sum, MAX_F(weights, FLOAT(1e-20f)))), PICK_F(valid, was, FLOAT(0.f)));
        target = P_CLAMP(target, X(grade1), Y(grade1));
        /* (one speed up, another down) */
        SpvId speed = PICK_F(P_LESS(target, was), Z(meter), W(grade0));
        SpvId now = PICK_F(valid, ADD_F(was, MUL_F(SUB_F(target, was), speed)), target);
        spvStore(s, slot, vec4Of(s, vec3Of(s, now, postExp2(s, now), FLOAT(0.f)), FLOAT(0.f)));
    }
    spvIfEnd(s, merge);
    spvEndMain(s);
    spvEntryPoint(s, SPV_MODEL_GLCompute, main, &p->global, 1);
    spvExecutionMode(s, main, 17, 64, 1, 1, 3);
    return finish(s, size);
}

POST_MAIN(artfShaderPostBloomDown)
    SpvId main = spvBeginMain(s);
    postFrameSizes(s, p);
    SpvId level = postPush(s, p);
    SpvId start, w, h, aboveStart, aboveW, aboveH;
    postLevel(s, p, level, &start, &w, &h);
    postLevel(s, p, SUB_U(spvGlsl2(s, 41, s->uintType, level, UINT(1)), UINT(1)), &aboveStart, &aboveW, &aboveH);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, w, h);
    SpvId first = spvOp2(s, 170, s->boolType, level, UINT(0));
    SpvId shownAt = postExposure(s, p);
    SpvId exposure = PICK_F(P_MORE(shownAt, FLOAT(0.f)), shownAt, FLOAT(1.f));
    SpvId threshold = Y(frameVec(s, &p->r, F_POST));
    SpvId sum = same3(s, 0.f);
    for (int k = 0; k < 4; ++k) {
        SpvId sx = ADD_F(ADD_F(MUL_F(TO_F(x), FLOAT(2.f)), FLOAT(.5f)), FLOAT(k & 1 ? .5f : -.5f)), sy = ADD_F(ADD_F(MUL_F(TO_F(y), FLOAT(2.f)), FLOAT(.5f)), FLOAT(k & 2 ? .5f : -.5f));
        SpvId fromFrame = SCALE_V3(spvShuffle3(s, postTap(s, p, p->light, UINT(0), p->width, p->height, sx, sy)), exposure);
        SpvId most = MAX_F(X(fromFrame), MAX_F(Y(fromFrame), Z(fromFrame)));
        SpvId past = PICK_F(P_MORE(most, threshold), DIV_F(SUB_F(most, threshold), MAX_F(most, FLOAT(1e-6f))), FLOAT(0.f));
        SpvId fromAbove = spvShuffle3(s, postTap(s, p, p->bloom, aboveStart, aboveW, aboveH, sx, sy));
        sum = ADD_V3(sum, pick3(s, first, SCALE_V3(fromFrame, past), fromAbove));
    }
    spvStore(s, postPtr(s, p, p->bloom, s->vec4, ADD_U(start, i)), vec4Of(s, SCALE_V3(sum, FLOAT(.25f)), FLOAT(0.f)));
POST_FINISH()

POST_MAIN(artfShaderPostBloomUp)
    SpvId main = spvBeginMain(s);
    postFrameSizes(s, p);
    SpvId level = postPush(s, p);
    SpvId start, w, h, belowStart, belowW, belowH;
    postLevel(s, p, level, &start, &w, &h);
    postLevel(s, p, ADD_U(level, UINT(1)), &belowStart, &belowW, &belowH);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, w, h);
    SpvId sx = SUB_F(MUL_F(ADD_F(TO_F(x), FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f)), sy = SUB_F(MUL_F(ADD_F(TO_F(y), FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f));
    SpvId here = postPtr(s, p, p->bloom, s->vec4, ADD_U(start, i));
    spvStore(s, here, ADD_V4(spvLoad(s, s->vec4, here), postTap(s, p, p->bloom, belowStart, belowW, belowH, sx, sy)));
POST_FINISH()

/* ---- the frame: as finalRow, a pixel shader drawn over the whole frame ---- */
uint32_t* artfShaderPostFinal(size_t* size, bool turns) { return artfShaderPostFinalFor(size, turns, NULL); }
uint32_t* artfShaderPostFinalFor(size_t* size, bool turns, const ArtfShaderDisplay* display) {
    const int kind = display ? display->kind : ARTF_DISPLAY_SRGB;
    Spv b, *s = &b;
    spvInit(s);
    Post post, *p = &post;
    postDeclare(s, p);
    SpvId turnPush = turns ? turnPushOf(s) : 0;
    SpvId fragCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
    spvDecorate(s, fragCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);
    SpvId main = spvBeginMain(s);
    postFrameSizes(s, p);
    SpvId coord = spvLoad(s, s->vec4, fragCoord);
    SpvId placeX = X(coord), placeY = Y(coord);
    if (turns) turnedPlace(s, turnPush, coord, &placeX, &placeY); /* (preRotate: the frame's place, from the image's) */
    SpvId x = P_TOU(FLOOR_F(placeX)), y = P_TOU(FLOOR_F(placeY));
    SpvId light = postVec4(s, p, p->light, ADD_U(MUL_U(y, p->width), x));
    SpvId alpha = P_CLAMP(W(light), FLOAT(0.f), FLOAT(1.f));
    SpvId postRow = frameVec(s, &p->r, F_POST);
    SpvId grade0 = frameVec(s, &p->r, F_GRADE), grade1 = frameVec(s, &p->r, F_GRADE + 1), grade2 = frameVec(s, &p->r, F_GRADE + 2);
    SpvId exposure = postExposure(s, p), bloom = X(postRow);
    SpvId curve = X(grade0), saturation = Z(grade1), contrast = W(grade1);
    SpvId temperature = X(grade2), tint = Y(grade2), vignette = Z(grade2), chromatic = W(grade2);
    /* (from the middle, as a share of the way to a corner) */
    SpvId cx = MUL_F(p->widthF, FLOAT(.5f)), cy = MUL_F(p->heightF, FLOAT(.5f));
    SpvId corner = DIV_F(FLOAT(1.f), ADD_F(MUL_F(cx, cx), MUL_F(cy, cy)));
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId dx = SUB_F(ADD_F(xf, FLOAT(.5f)), cx), dy = SUB_F(ADD_F(yf, FLOAT(.5f)), cy);
    SpvId apart = MUL_F(chromatic, SQRT_F(corner));
    SpvId parted = P_MORE(chromatic, FLOAT(0.f));
    /* (red and blue apart only when asked: a frame without pays no reads for it) */
    SpvId plainRed = X(light), plainBlue = Z(light); /* (before the if: what a phi reads comes first) */
    SpvId before = s->block, partedMerge;
    spvIfBegin(s, parted, &partedMerge);
    SpvId red = X(postTap(s, p, p->light, UINT(0), p->width, p->height, ADD_F(xf, MUL_F(dx, apart)), ADD_F(yf, MUL_F(dy, apart))));
    SpvId blue = Z(postTap(s, p, p->light, UINT(0), p->width, p->height, SUB_F(xf, MUL_F(dx, apart)), SUB_F(yf, MUL_F(dy, apart))));
    SpvId partedFrom = s->block;
    spvIfEnd(s, partedMerge);
    red = spvPhi(s, s->floatType, red, partedFrom, plainRed, before);
    blue = spvPhi(s, s->floatType, blue, partedFrom, plainBlue, before);
    SpvId v = vec3Of(s, red, Y(light), blue);
    v = SCALE_V3(v, PICK_F(P_MORE(alpha, FLOAT(0.f)), DIV_F(FLOAT(1.f), MAX_F(alpha, FLOAT(1e-6f))), FLOAT(1.f)));
    v = SCALE_V3(v, PICK_F(P_MORE(exposure, FLOAT(0.f)), exposure, FLOAT(1.f)));
    SpvId start, bw, bh;
    postLevel(s, p, UINT(0), &start, &bw, &bh);
    SpvId glow = spvShuffle3(s, postTap(s, p, p->bloom, UINT(0), bw, bh, SUB_F(MUL_F(ADD_F(xf, FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f)), SUB_F(MUL_F(ADD_F(yf, FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f))));
    v = pick3(s, P_MORE(bloom, FLOAT(0.f)), ADD_V3(v, SCALE_V3(glow, DIV_F(bloom, MAX_F(alpha, FLOAT(1e-3f))))), v);
    /* lens flare, as post.c's flareAt: only when asked */
    {
        static const float scales[3] = {1.f, .5f, 1.8f};
        static const float tints[3][3] = {{1.f, .7f, .4f}, {.4f, .7f, 1.f}, {.7f, 1.f, .6f}};
        SpvId flare = W(frameVec(s, &p->r, F_GRADE2 + 5));
        SpvId flareBefore = s->block, flareMerge;
        spvIfBegin(s, P_MORE(flare, FLOAT(0.f)), &flareMerge);
        SpvId ghosts = same3(s, 0.f);
        for (int g = 0; g < 3; ++g) {
            const float k = scales[g];
            SpvId ax = DIV_F(MUL_F(dx, FLOAT(k)), cx), ay = DIV_F(MUL_F(dy, FLOAT(k)), cy);
            SpvId fade = MUL_F(MAX_F(SUB_F(FLOAT(1.f), MUL_F(ax, ax)), FLOAT(0.f)), MAX_F(SUB_F(FLOAT(1.f), MUL_F(ay, ay)), FLOAT(0.f)));
            SpvId gx = SUB_F(MUL_F(SUB_F(cx, MUL_F(dx, FLOAT(k))), FLOAT(.5f)), FLOAT(.5f)), gy = SUB_F(MUL_F(SUB_F(cy, MUL_F(dy, FLOAT(k))), FLOAT(.5f)), FLOAT(.5f));
            SpvId tinted = SCALE_V3(vec3Of(s, FLOAT(tints[g][0]), FLOAT(tints[g][1]), FLOAT(tints[g][2])), fade);
            ghosts = ADD_V3(ghosts, MUL_V3(tinted, spvShuffle3(s, postTap(s, p, p->bloom, UINT(0), bw, bh, gx, gy))));
        }
        SpvId m = MAX_F(alpha, FLOAT(1e-3f));
        SpvId flared = ADD_V3(v, spvOp2(s, SPV_OP_FDiv, s->vec3, SCALE_V3(ghosts, flare), vec3Of(s, m, m, m)));
        SpvId flareFrom = s->block;
        spvIfEnd(s, flareMerge);
        v = spvPhi(s, s->vec3, flared, flareFrom, v, flareBefore);
    }
    /* white balance, saturation, vignette */
    v = MUL_V3(v, vec3Of(s, ADD_F(FLOAT(1.f), MUL_F(FLOAT(.2f), temperature)), SUB_F(FLOAT(1.f), MUL_F(FLOAT(.2f), tint)), SUB_F(FLOAT(1.f), MUL_F(FLOAT(.2f), temperature))));
    SpvId grey = DOT3(v, vec3Of(s, FLOAT(.2126f), FLOAT(.7152f), FLOAT(.0722f)));
    SpvId greys = vec3Of(s, grey, grey, grey);
    v = pick3(s, spvOp2(s, 182 /* OpFOrdNotEqual */, s->boolType, saturation, FLOAT(0.f)), ADD_V3(greys, SCALE_V3(SUB_V3(v, greys), ADD_F(FLOAT(1.f), saturation))), v);
    SpvId q = ADD_F(FLOAT(1.f), MUL_F(MUL_F(vignette, ADD_F(MUL_F(dx, dx), MUL_F(dy, dy))), corner));
    v = SCALE_V3(v, DIV_F(FLOAT(1.f), MUL_F(q, q)));
    v = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, v, same3(s, 0.f)), same3(s, 65504.f));
    /* grading in ACEScct (artfGradeCct), unless it is in what is shown */
    SpvId shownSpace = P_MORE(W(frameVec(s, &p->r, F_CULL)), FLOAT(0.f));
    v = gradeCct(s, p, v, shownSpace);
    /* (HDR10: the curve's white at peak over paper white, as post.c's finalLight) */
    if (kind == ARTF_DISPLAY_HDR10) v = SCALE_V3(v, FLOAT(display->inverse));
    /* the curves: ACES's fit, Khronos's neutral, Reinhard's, none */
    SpvId vc[5];
    const float numbers[5] = {2.51f, .03f, 2.43f, .59f, .14f};
    for (int k = 0; k < 5; ++k) vc[k] = same3(s, numbers[k]);
    SpvId aces = spvOp2(s, SPV_OP_FDiv, s->vec3, MUL_V3(v, ADD_V3(MUL_V3(v, vc[0]), vc[1])), ADD_V3(MUL_V3(v, ADD_V3(MUL_V3(v, vc[2]), vc[3])), vc[4]));
    SpvId reinhard = spvOp2(s, SPV_OP_FDiv, s->vec3, v, ADD_V3(v, same3(s, 1.f)));
    SpvId neutral;
    {
        SpvId least = MIN_F(X(v), MIN_F(Y(v), Z(v)));
        SpvId offset = PICK_F(P_LESS(least, FLOAT(.08f)), SUB_F(least, MUL_F(FLOAT(6.25f), MUL_F(least, least))), FLOAT(.04f));
        SpvId c = SUB_V3(v, vec3Of(s, offset, offset, offset));
        SpvId peak = MAX_F(X(c), MAX_F(Y(c), Z(c)));
        const float begin = .76f, d = 1.f - .76f;
        SpvId top = SUB_F(FLOAT(1.f), DIV_F(FLOAT(d * d), ADD_F(peak, FLOAT(d - begin))));
        SpvId g = SUB_F(FLOAT(1.f), DIV_F(FLOAT(1.f), ADD_F(MUL_F(FLOAT(.15f), SUB_F(peak, top)), FLOAT(1.f))));
        SpvId rolled = SCALE_V3(c, DIV_F(top, MAX_F(peak, FLOAT(1e-6f))));
        rolled = ADD_V3(rolled, SCALE_V3(SUB_V3(vec3Of(s, top, top, top), rolled), g));
        neutral = pick3(s, P_LESS(peak, FLOAT(begin)), c, rolled);
    }
    SpvId mapped = pick3(s, P_LESS(curve, FLOAT(.5f)), aces, pick3(s, P_LESS(curve, FLOAT(1.5f)), neutral, pick3(s, P_LESS(curve, FLOAT(2.5f)), reinhard, v)));
    v = pick3(s, P_MORE(exposure, FLOAT(0.f)), mapped, v);
    SpvId shown = srgbEncode3(s, spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, v, same3(s, 0.f)), same3(s, 1.f)));
    SpvId steeper = ADD_V3(same3(s, .5f), SCALE_V3(SUB_V3(shown, same3(s, .5f)), ADD_F(FLOAT(1.f), contrast)));
    steeper = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, steeper, same3(s, 0.f)), same3(s, 1.f));
    shown = pick3(s, spvOp2(s, 182, s->boolType, contrast, FLOAT(0.f)), steeper, shown);
    /* as post.c's gradeShown: lift, gamma and gain, the color wheels; the look-up table; the grain */
    {
        SpvId g0 = frameVec(s, &p->r, F_GRADE2), g1 = frameVec(s, &p->r, F_GRADE2 + 1), g2 = frameVec(s, &p->r, F_GRADE2 + 2);
        SpvId g3 = frameVec(s, &p->r, F_GRADE2 + 3), g4 = frameVec(s, &p->r, F_GRADE2 + 4), g5 = frameVec(s, &p->r, F_GRADE2 + 5);
        SpvId one = same3(s, 1.f), zero = same3(s, 0.f);
        SpvId o = MUL_V3(spvShuffle3(s, g2), ADD_V3(shown, MUL_V3(spvShuffle3(s, g0), SUB_V3(one, shown))));
        SpvId powered[3];
        for (uint32_t k = 0; k < 3; ++k) {
            SpvId ok = spvExtract(s, s->floatType, o, k), inverse = spvExtract(s, s->floatType, g1, k);
            powered[k] = PICK_F(spvOp2(s, 180 /* OpFOrdEqual */, s->boolType, inverse, FLOAT(1.f)), ok,
                                PICK_F(P_MORE(ok, FLOAT(0.f)), postExp2(s, MUL_F(postLog2(s, ok), inverse)), FLOAT(0.f)));
        }
        SpvId c = vec3Of(s, powered[0], powered[1], powered[2]);
        SpvId l = P_CLAMP(ADD_F(ADD_F(MUL_F(FLOAT(.2126f), X(c)), MUL_F(FLOAT(.7152f), Y(c))), MUL_F(FLOAT(.0722f), Z(c))), FLOAT(0.f), FLOAT(1.f));
        SpvId rest = SUB_F(FLOAT(1.f), l);
        SpvId dark = MUL_F(rest, rest), middle = MUL_F(MUL_F(FLOAT(2.f), l), rest), bright = MUL_F(l, l);
        SpvId wheel = ADD_V3(ADD_V3(SCALE_V3(spvShuffle3(s, g3), dark), SCALE_V3(spvShuffle3(s, g4), middle)), SCALE_V3(spvShuffle3(s, g5), bright));
        c = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, MUL_V3(c, wheel), zero), one);
        shown = pick3(s, P_AND(shownSpace, P_MORE(W(g0), FLOAT(0.f))), c, shown);
        /* the look-up table, past auto exposure's state in bloom's buffer: only when there is one */
        SpvId side = W(g3);
        SpvId lutBefore = s->block, lutMerge;
        spvIfBegin(s, P_AND(shownSpace, P_MORE(side, FLOAT(1.5f))), &lutMerge);
        SpvId n = P_TOU(side), lutStart = ADD_U(postExposureSlot(s, p), UINT(1));
        SpvId q = SCALE_V3(spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, shown, zero), one), SUB_F(side, FLOAT(1.f)));
        SpvId at[3], t[3];
        for (uint32_t k = 0; k < 3; ++k) {
            SpvId qk = spvExtract(s, s->floatType, q, k);
            at[k] = spvGlsl2(s, SPV_GLSL_UMin, s->uintType, P_TOU(qk), SUB_U(n, UINT(2)));
            t[k] = SUB_F(qk, TO_F(at[k]));
        }
        SpvId rowStep = n, sliceStep = MUL_U(n, n);
        SpvId base = ADD_U(lutStart, ADD_U(MUL_U(ADD_U(MUL_U(at[2], n), at[1]), n), at[0]));
        SpvId corners[8];
        for (uint32_t k = 0; k < 8; ++k) {
            SpvId i = base;
            if (k & 1) i = ADD_U(i, UINT(1));
            if (k & 2) i = ADD_U(i, rowStep);
            if (k & 4) i = ADD_U(i, sliceStep);
            corners[k] = postVec4(s, p, p->bloom, i);
        }
#define LERP4(a, b, f) ADD_V4(a, SCALE_V4(SUB_V4(b, a), f))
        SpvId a0 = LERP4(corners[0], corners[1], t[0]), b0 = LERP4(corners[2], corners[3], t[0]);
        SpvId d0 = LERP4(corners[4], corners[5], t[0]), e0 = LERP4(corners[6], corners[7], t[0]);
        SpvId near_ = LERP4(a0, b0, t[1]), far_ = LERP4(d0, e0, t[1]);
        SpvId looked = spvShuffle3(s, LERP4(near_, far_, t[2]));
#undef LERP4
        SpvId mixed = ADD_V3(shown, SCALE_V3(SUB_V3(looked, shown), W(g4)));
        mixed = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, mixed, zero), one);
        SpvId lutFrom = s->block;
        spvIfEnd(s, lutMerge);
        shown = spvPhi(s, s->vec3, mixed, lutFrom, shown, lutBefore);
        /* the grain: the same hash as the CPU's, of the pixel and the frame's number */
        SpvId grain = W(g1);
        SpvId h = postHash(s, ADD_U(ADD_U(MUL_U(y, p->width), x), postHash(s, P_TOU(W(g2)))));
        SpvId noise = SUB_F(MUL_F(TO_F(spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, h, UINT(8))), FLOAT(1.f / 16777216.f)), FLOAT(.5f));
        SpvId amount = ADD_V3(same3(s, .0625f), MUL_V3(SCALE_V3(shown, FLOAT(.75f)), SUB_V3(one, shown)));
        SpvId grained = ADD_V3(shown, SCALE_V3(amount, MUL_F(noise, grain)));
        grained = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, grained, zero), one);
        shown = pick3(s, P_MORE(grain, FLOAT(0.f)), grained, shown);
    }
    /* made for another screen, as post.c's finalOut (ArtfRendererOptions.output) */
    if (kind != ARTF_DISPLAY_SRGB) shown = displaySpv(s, kind, display->peak, srgbDecode3(s, shown));
    spvStore(s, outColor, vec4Of(s, SCALE_V3(shown, alpha), alpha));
    spvEndMain(s);
    const SpvId io[2] = {fragCoord, outColor};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, io, 2);
    spvExecutionMode(s, main, 7, 0, 0, 0, 0);
    return finish(s, size);
}


/* ---- clouds in the scene: sky.c's artfCloudsColumn and artfCloudsAt, the same arithmetic in the same order. Binding
 * 73: the field (ArtfScene.clouds); 74: the grid's cells (ARTF_CLOUD_CELL floats each). The column's push constant: how
 * many columns across. ---- */

/* (sky.c's own: the earth, the top of the air, the air's table, the clouds' noise and where they are) */
enum { C_TRANS_W = 256, C_TRANS_H = 64, C_NOISE = 64, C_WEATHER = 128, C_LIGHT_STEPS = 6 };
#define C_EARTH 6360.f
#define C_TOP 6460.f
#define C_HORIZON 1132.2544f
#define C_SHAPE_KM 12.f
#define C_DETAIL_KM 2.3f
#define C_WEATHER_KM 60.f
#define C_CLOUD_FAR 90.f

typedef struct Clouds {
    Post* p;
    SpvId field, base, top, density, coverage, wind[3], unit, toSun[3], eyeR, sun[3], ambient[3];
} Clouds;

static SpvId cloudField(Spv* s, const Clouds* c, SpvId at) { return postFloat(s, c->p, c->field, at); }
static SpvId cloudExp(Spv* s, SpvId x) { return spvExp2Fast(s, MUL_F(x, FLOAT(1.44269504f))); }
/* (C's (int)(x + k) - k, a floor for what is above -k) */
static SpvId cloudFloor(Spv* s, SpvId x, float k) {
    return spvOp2(s, SPV_OP_ISub, s->intType, spvOp1(s, SPV_OP_ConvertFToS, s->intType, ADD_F(x, FLOAT(k))), spvConstInt(s, (int)k));
}
static SpvId cloudWrap(Spv* s, SpvId i, uint32_t mask) { return AND_U(spvOp1(s, SPV_OP_Bitcast, s->uintType, i), UINT(mask)); }

/* sky.c's noiseAt */
static SpvId cloudNoise(Spv* s, const Clouds* c, SpvId x, SpvId y, SpvId z) {
    SpvId ix = cloudFloor(s, x, 65536.f), iy = cloudFloor(s, y, 65536.f), iz = cloudFloor(s, z, 65536.f);
    SpvId fx = SUB_F(x, spvOp1(s, 111 /* OpConvertSToF */, s->floatType, ix)), fy = SUB_F(y, spvOp1(s, 111 /* OpConvertSToF */, s->floatType, iy)), fz = SUB_F(z, spvOp1(s, 111 /* OpConvertSToF */, s->floatType, iz));
    SpvId x0 = cloudWrap(s, ix, C_NOISE - 1), y0 = cloudWrap(s, iy, C_NOISE - 1), z0 = cloudWrap(s, iz, C_NOISE - 1);
    SpvId x1 = AND_U(ADD_U(x0, UINT(1)), UINT(C_NOISE - 1)), y1 = AND_U(ADD_U(y0, UINT(1)), UINT(C_NOISE - 1)), z1 = AND_U(ADD_U(z0, UINT(1)), UINT(C_NOISE - 1));
#define N(a, b, cc) cloudField(s, c, ADD_U(UINT(ARTF_CLOUD_NOISE), ADD_U(MUL_U(ADD_U(MUL_U(cc, UINT(C_NOISE)), b), UINT(C_NOISE)), a)))
    SpvId n000 = N(x0, y0, z0), n100 = N(x1, y0, z0), n010 = N(x0, y1, z0), n110 = N(x1, y1, z0);
    SpvId n001 = N(x0, y0, z1), n101 = N(x1, y0, z1), n011 = N(x0, y1, z1), n111 = N(x1, y1, z1);
#undef N
    SpvId a = ADD_F(n000, MUL_F(SUB_F(n100, n000), fx)), b = ADD_F(n010, MUL_F(SUB_F(n110, n010), fx));
    SpvId cc = ADD_F(n001, MUL_F(SUB_F(n101, n001), fx)), d = ADD_F(n011, MUL_F(SUB_F(n111, n011), fx));
    SpvId near_ = ADD_F(a, MUL_F(SUB_F(b, a), fy)), far = ADD_F(cc, MUL_F(SUB_F(d, cc), fy));
    return ADD_F(near_, MUL_F(SUB_F(far, near_), fz));
}

/* sky.c's weatherAt */
static SpvId cloudWeather(Spv* s, const Clouds* c, SpvId x, SpvId y) {
    SpvId ix = cloudFloor(s, x, 65536.f), iy = cloudFloor(s, y, 65536.f);
    SpvId fx = SUB_F(x, spvOp1(s, 111 /* OpConvertSToF */, s->floatType, ix)), fy = SUB_F(y, spvOp1(s, 111 /* OpConvertSToF */, s->floatType, iy));
    SpvId x0 = cloudWrap(s, ix, C_WEATHER - 1), y0 = cloudWrap(s, iy, C_WEATHER - 1), x1 = AND_U(ADD_U(x0, UINT(1)), UINT(C_WEATHER - 1)), y1 = AND_U(ADD_U(y0, UINT(1)), UINT(C_WEATHER - 1));
#define WT(a, b) cloudField(s, c, ADD_U(UINT(ARTF_CLOUD_WEATHER), ADD_U(MUL_U(b, UINT(C_WEATHER)), a)))
    SpvId a = WT(x0, y0), b = WT(x1, y0), cc = WT(x0, y1), d = WT(x1, y1);
#undef WT
    SpvId top = ADD_F(a, MUL_F(SUB_F(b, a), fx)), bottom = ADD_F(cc, MUL_F(SUB_F(d, cc), fx));
    return ADD_F(top, MUL_F(SUB_F(bottom, top), fy));
}

/* sky.c's cloudAt: what is taken at p, h above the ground, per kilometer; 0 out of them (by choices of results, not of
 * branches: the same numbers where they are) */
static SpvId cloudAt(Spv* s, const Clouds* c, const SpvId* q, SpvId h, bool fine) {
    SpvId frac = DIV_F(SUB_F(h, c->base), SUB_F(c->top, c->base));
    SpvId in = P_AND(P_MORE(frac, FLOAT(0.f)), P_LESS(frac, FLOAT(1.f)));
    SpvId x = ADD_F(q[0], c->wind[0]), y = ADD_F(h, c->wind[1]), z = ADD_F(q[2], c->wind[2]);
    const float wScale = (float)C_WEATHER / C_WEATHER_KM, nScale = (float)C_NOISE / C_SHAPE_KM, dScale = (float)C_NOISE / C_DETAIL_KM;
    SpvId local = P_CLAMP(ADD_F(c->coverage, MUL_F(SUB_F(cloudWeather(s, c, MUL_F(x, FLOAT(wScale)), MUL_F(z, FLOAT(wScale))), FLOAT(.5f)), FLOAT(1.2f))), FLOAT(0.f), FLOAT(1.f));
    in = P_AND(in, P_MORE(local, FLOAT(0.f)));
    SpvId rise = P_CLAMP(MUL_F(frac, FLOAT(6.f)), FLOAT(0.f), FLOAT(1.f)), fall = P_CLAMP(MUL_F(SUB_F(FLOAT(1.f), frac), FLOAT(1.8f)), FLOAT(0.f), FLOAT(1.f));
    SpvId shape = MUL_F(MUL_F(cloudNoise(s, c, MUL_F(x, FLOAT(nScale)), MUL_F(y, FLOAT(nScale)), MUL_F(z, FLOAT(nScale))), rise), MUL_F(fall, SUB_F(FLOAT(2.f), fall)));
    SpvId d = DIV_F(SUB_F(shape, SUB_F(FLOAT(1.f), local)), MAX_F(local, FLOAT(.05f)));
    in = P_AND(in, P_MORE(d, FLOAT(0.f)));
    if (fine) {
        SpvId wear = cloudNoise(s, c, ADD_F(MUL_F(x, FLOAT(dScale)), FLOAT(17.f)), MUL_F(y, FLOAT(dScale)), ADD_F(MUL_F(z, FLOAT(dScale)), FLOAT(31.f)));
        d = SUB_F(d, MUL_F(MUL_F(SUB_F(FLOAT(1.f), wear), FLOAT(.35f)), SUB_F(FLOAT(1.f), d)));
        in = P_AND(in, P_MORE(d, FLOAT(0.f)));
    }
    return PICK_F(in, MUL_F(MUL_F(MIN_F(d, FLOAT(1.f)), c->density), FLOAT(45.f)), FLOAT(0.f));
}

/* sky.c's sphereSpan (b, c and the root; ok: met) and toSphere */
static void cloudSpan(Spv* s, const SpvId* q, const SpvId* d, float r, SpvId* near_, SpvId* far, SpvId* ok) {
    SpvId b = ADD_F(ADD_F(MUL_F(q[0], d[0]), MUL_F(q[1], d[1])), MUL_F(q[2], d[2]));
    SpvId cc = SUB_F(ADD_F(ADD_F(MUL_F(q[0], q[0]), MUL_F(q[1], q[1])), MUL_F(q[2], q[2])), FLOAT(r * r));
    SpvId disc = SUB_F(MUL_F(b, b), cc);
    *ok = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, disc, FLOAT(0.f));
    SpvId root = SQRT_F(MAX_F(disc, FLOAT(0.f))), minusB = spvOp1(s, SPV_OP_FNegate, s->floatType, b);
    *near_ = SUB_F(minusB, root);
    *far = ADD_F(minusB, root);
}
static SpvId cloudToSphere(Spv* s, const SpvId* q, const SpvId* d, float r) {
    SpvId near_, far, ok;
    cloudSpan(s, q, d, r, &near_, &far, &ok);
    return PICK_F(ok, PICK_F(P_MORE(near_, FLOAT(0.f)), near_, PICK_F(P_MORE(far, FLOAT(0.f)), far, FLOAT(-1.f))), FLOAT(-1.f));
}

/* sky.c's sunThroughTable: what the air leaves of the sun at p */
static SpvId cloudSunThrough(Spv* s, const Clouds* c, const SpvId* q) {
    SpvId behind = P_MORE(cloudToSphere(s, q, c->toSun, C_EARTH), FLOAT(0.f));
    SpvId r = SQRT_F(ADD_F(ADD_F(MUL_F(q[0], q[0]), MUL_F(q[1], q[1])), MUL_F(q[2], q[2])));
    SpvId mu = DIV_F(ADD_F(ADD_F(MUL_F(q[0], c->toSun[0]), MUL_F(q[1], c->toSun[1])), MUL_F(q[2], c->toSun[2])), r);
    SpvId rho = SQRT_F(MAX_F(SUB_F(MUL_F(r, r), FLOAT(C_EARTH * C_EARTH)), FLOAT(0.f)));
    SpvId minusR = spvOp1(s, SPV_OP_FNegate, s->floatType, r);
    SpvId dd = MAX_F(ADD_F(MUL_F(minusR, mu), SQRT_F(MAX_F(ADD_F(MUL_F(MUL_F(r, r), SUB_F(MUL_F(mu, mu), FLOAT(1.f))), FLOAT(C_TOP * C_TOP)), FLOAT(0.f)))), FLOAT(0.f));
    SpvId least = SUB_F(FLOAT(C_TOP), r), most = ADD_F(rho, FLOAT(C_HORIZON));
    SpvId tx = DIV_F(SUB_F(dd, least), MAX_F(SUB_F(most, least), FLOAT(1e-6f))), ty = DIV_F(rho, FLOAT(C_HORIZON));
    /* table3, bilinear */
    SpvId x = P_CLAMP(MUL_F(tx, FLOAT((float)(C_TRANS_W - 1))), FLOAT(0.f), FLOAT((float)(C_TRANS_W - 1)));
    SpvId y = P_CLAMP(MUL_F(ty, FLOAT((float)(C_TRANS_H - 1))), FLOAT(0.f), FLOAT((float)(C_TRANS_H - 1)));
    SpvId x0 = spvGlsl2(s, 39 /* SMin */, s->intType, spvOp1(s, SPV_OP_ConvertFToS, s->intType, x), spvConstInt(s, C_TRANS_W - 2));
    SpvId y0 = spvGlsl2(s, 39, s->intType, spvOp1(s, SPV_OP_ConvertFToS, s->intType, y), spvConstInt(s, C_TRANS_H - 2));
    SpvId fx = SUB_F(x, spvOp1(s, 111 /* OpConvertSToF */, s->floatType, x0)), fy = SUB_F(y, spvOp1(s, 111 /* OpConvertSToF */, s->floatType, y0));
    SpvId a = ADD_U(UINT(ARTF_CLOUD_TRANS), MUL_U(UINT(3), ADD_U(MUL_U(spvOp1(s, SPV_OP_Bitcast, s->uintType, y0), UINT(C_TRANS_W)), spvOp1(s, SPV_OP_Bitcast, s->uintType, x0))));
    SpvId bAt = ADD_U(a, UINT(3 * C_TRANS_W));
    SpvId out[3];
    for (int k = 0; k < 3; ++k) {
        SpvId a0 = cloudField(s, c, ADD_U(a, UINT((uint32_t)k))), a1 = cloudField(s, c, ADD_U(a, UINT(3u + (uint32_t)k)));
        SpvId b0 = cloudField(s, c, ADD_U(bAt, UINT((uint32_t)k))), b1 = cloudField(s, c, ADD_U(bAt, UINT(3u + (uint32_t)k)));
        SpvId top = ADD_F(a0, MUL_F(SUB_F(a1, a0), fx)), bottom = ADD_F(b0, MUL_F(SUB_F(b1, b0), fx));
        out[k] = PICK_F(behind, FLOAT(0.f), cloudExp(s, spvOp1(s, SPV_OP_FNegate, s->floatType, ADD_F(top, MUL_F(SUB_F(bottom, top), fy)))));
    }
    return vec3Of(s, out[0], out[1], out[2]);
}

static SpvId cloudHenyey(Spv* s, float g, SpvId mu) {
    SpvId q = SUB_F(FLOAT(1.f + g * g), MUL_F(FLOAT(2.f * g), mu));
    return DIV_F(FLOAT(.079577472f * (1.f - g * g)), MUL_F(q, SQRT_F(q)));
}

static void cloudsOf(Spv* s, Clouds* c, Post* p, SpvId field) {
    c->p = p;
    c->field = field;
    SpvId at[20];
    for (int k = 0; k < 20; ++k) at[k] = cloudField(s, c, UINT((uint32_t)k));
    c->base = at[0];
    c->top = at[1];
    c->density = at[2];
    c->coverage = at[3];
    for (int k = 0; k < 3; ++k) {
        c->wind[k] = at[4 + k];
        c->toSun[k] = at[8 + k];
        c->sun[k] = at[12 + k];
        c->ambient[k] = at[16 + k];
    }
    c->unit = at[7];
    c->eyeR = at[11];
}

/* sky.c's artfCloudJitter */
static SpvId cloudJitter(Spv* s, SpvId i, SpvId j) {
    SpvId h = spvOp2(s, 198 /* OpBitwiseXor */, s->uintType, MUL_U(i, UINT(0x9E3779B1u)), MUL_U(j, UINT(0x85EBCA77u)));
    h = MUL_U(spvOp2(s, 198, s->uintType, h, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, h, UINT(15))), UINT(0x2C1B3C6Du));
    h = spvOp2(s, 198, s->uintType, h, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, h, UINT(12)));
    return MUL_F(TO_F(spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, h, UINT(8))), FLOAT(1.f / 16777216.f));
}

static void cloudCellStore(Spv* s, Post* p, SpvId grid, SpvId at, SpvId value) {
    spvStore(s, spvAccess(s, p->r.ptrStorageVec4, grid, (SpvId[]){p->r.zero, at}, 2), value);
}

POST_MAIN(artfShaderPostCloudColumns)
    SpvId field = postBuffer(s, p, p->r.floatArray, 73), grid = postBuffer(s, p, p->r.vec4Array, 74);
    SpvId main = spvBeginMain(s);
    SpvId ptrFn3 = spvTypePointer(s, SPV_STORAGE_Function, s->vec3), ptrFnF = spvTypePointer(s, SPV_STORAGE_Function, s->floatType), ptrFnU = spvTypePointer(s, SPV_STORAGE_Function, s->uintType);
    SpvId lightVar = spvVariable(s, ptrFn3, SPV_STORAGE_Function), leftVar = spvVariable(s, ptrFnF, SPV_STORAGE_Function), counter = spvVariable(s, ptrFnU, SPV_STORAGE_Function);
    SpvId farVar = spvVariable(s, ptrFnF, SPV_STORAGE_Function), around = spvVariable(s, ptrFnU, SPV_STORAGE_Function);
    postSizes(s, p);
    SpvId columns = postPush(s, p), rows = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->height, UINT(ARTF_CLOUD_TILE - 1)), UINT(ARTF_CLOUD_TILE));
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, columns, rows);
    Clouds c;
    cloudsOf(s, &c, p, field);
    SpvId cell = MUL_U(i, UINT(1 + ARTF_CLOUD_SLICES));
    /* nothing, to start with */
    cloudCellStore(s, p, grid, cell, vec4Of(s, same3(s, 0.f), FLOAT(0.f)));
    for (int k = 0; k < ARTF_CLOUD_SLICES; ++k) cloudCellStore(s, p, grid, ADD_U(cell, UINT(1u + (uint32_t)k)), vec4Of(s, same3(s, 0.f), FLOAT(1.f)));
    SpvId cx = ADD_F(MUL_F(TO_F(x), FLOAT((float)ARTF_CLOUD_TILE)), FLOAT(ARTF_CLOUD_TILE * .5f - .5f)), cy = ADD_F(MUL_F(TO_F(y), FLOAT((float)ARTF_CLOUD_TILE)), FLOAT(ARTF_CLOUD_TILE * .5f - .5f));
    SpvId way = SUB_V3(postWorld(s, p, cx, cy, FLOAT(1.f)), spvShuffle3(s, frameVec(s, &p->r, F_CAMERA)));
    SpvId length = SQRT_F(ADD_F(ADD_F(MUL_F(X(way), X(way)), MUL_F(Y(way), Y(way))), MUL_F(Z(way), Z(way))));
    const SpvId dir[3] = {DIV_F(X(way), length), DIV_F(Y(way), length), DIV_F(Z(way), length)};
    SpvId jitter = cloudJitter(s, x, y);
    /* the farthest surface among the pixels around it: post.c's cloudFarthest */
    spvStore(s, farVar, FLOAT(0.f));
    {
        PostLoop near_;
        SpvId k = postLoopBegin(s, &near_, around, UINT(ARTF_CLOUD_TILE * ARTF_CLOUD_TILE * 9));
        SpvId side = UINT(ARTF_CLOUD_TILE * 3);
        SpvId qx = spvOp2(s, SPV_OP_ISub, s->intType, spvOp1(s, SPV_OP_Bitcast, s->intType, ADD_U(MUL_U(x, UINT(ARTF_CLOUD_TILE)), spvOp2(s, SPV_OP_UMod, s->uintType, k, side))), spvConstInt(s, ARTF_CLOUD_TILE));
        SpvId qy = spvOp2(s, SPV_OP_ISub, s->intType, spvOp1(s, SPV_OP_Bitcast, s->intType, ADD_U(MUL_U(y, UINT(ARTF_CLOUD_TILE)), spvOp2(s, SPV_OP_UDiv, s->uintType, k, side))), spvConstInt(s, ARTF_CLOUD_TILE));
        SpvId qxf = spvOp1(s, 111 /* OpConvertSToF */, s->floatType, qx), qyf = spvOp1(s, 111, s->floatType, qy);
        SpvId inside = P_AND(P_AND(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, qxf, FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, qyf, FLOAT(0.f))), P_AND(P_LESS(qxf, p->widthF), P_LESS(qyf, p->heightF)));
        SpvId q = ADD_U(MUL_U(P_TOU(P_CLAMP(qyf, FLOAT(0.f), SUB_F(p->heightF, FLOAT(1.f)))), p->width), P_TOU(P_CLAMP(qxf, FLOAT(0.f), SUB_F(p->widthF, FLOAT(1.f)))));
        SpvId depth = postFloat(s, p, p->depth, q);
        SpvId at = SUB_V3(postWorld(s, p, qxf, qyf, postAlong(s, p, depth)), spvShuffle3(s, frameVec(s, &p->r, F_CAMERA)));
        SpvId far = SQRT_F(ADD_F(ADD_F(MUL_F(X(at), X(at)), MUL_F(Y(at), Y(at))), MUL_F(Z(at), Z(at))));
        SpvId was = spvLoad(s, s->floatType, farVar);
        spvStore(s, farVar, PICK_F(P_AND(inside, P_MORE(depth, FLOAT(0.f))), MAX_F(was, far), was));
        postLoopEnd(s, &near_);
    }
    SpvId farthest = MUL_F(spvLoad(s, s->floatType, farVar), c.unit);
    /* where the layer is along the ray: as artfCloudsColumn */
    const SpvId eye[3] = {FLOAT(0.f), c.eyeR, FLOAT(0.f)};
    SpvId a0, a1, aOk, b0, b1, bOk;
    /* (the radii are the field's: by its values, not constants) */
    SpvId inner = ADD_F(FLOAT(C_EARTH), c.base), outer = ADD_F(FLOAT(C_EARTH), c.top);
    {
        SpvId b = ADD_F(ADD_F(MUL_F(eye[0], dir[0]), MUL_F(eye[1], dir[1])), MUL_F(eye[2], dir[2]));
        SpvId e2 = ADD_F(ADD_F(MUL_F(eye[0], eye[0]), MUL_F(eye[1], eye[1])), MUL_F(eye[2], eye[2]));
        SpvId minusB = spvOp1(s, SPV_OP_FNegate, s->floatType, b);
        SpvId discB = SUB_F(MUL_F(b, b), SUB_F(e2, MUL_F(outer, outer))), discA = SUB_F(MUL_F(b, b), SUB_F(e2, MUL_F(inner, inner)));
        bOk = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, discB, FLOAT(0.f));
        aOk = spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, discA, FLOAT(0.f));
        SpvId rootB = SQRT_F(MAX_F(discB, FLOAT(0.f))), rootA = SQRT_F(MAX_F(discA, FLOAT(0.f)));
        b0 = SUB_F(minusB, rootB);
        b1 = ADD_F(minusB, rootB);
        a0 = SUB_F(minusB, rootA);
        a1 = ADD_F(minusB, rootA);
    }
    SpvId r = c.eyeR, ground = cloudToSphere(s, eye, dir, C_EARTH);
    SpvId below = P_LESS(r, inner), within = P_LESS(r, outer);
    SpvId groundFirst = P_AND(P_MORE(ground, FLOAT(0.f)), P_LESS(ground, a1));
    SpvId upTo = PICK_F(P_AND(aOk, P_MORE(a0, FLOAT(0.f))), a0, b1);
    SpvId from = PICK_F(below, a1, PICK_F(within, FLOAT(0.f), MAX_F(b0, FLOAT(0.f))));
    SpvId to = PICK_F(below, b1, upTo);
    SpvId valid = P_AND(bOk, P_MORE(b1, FLOAT(0.f)));
    valid = P_AND(valid, P_NOT(P_AND(below, P_OR(P_NOT(aOk), groundFirst))));
    valid = P_AND(valid, P_NOT(P_MORE(from, FLOAT(C_CLOUD_FAR))));
    valid = P_AND(valid, P_MORE(farthest, from));
    to = MIN_F(MIN_F(to, ADD_F(from, FLOAT(30.f))), farthest);
    SpvId marching;
    spvIfBegin(s, valid, &marching);
    cloudCellStore(s, p, grid, cell, vec4Of(s, vec3Of(s, DIV_F(from, c.unit), DIV_F(to, c.unit), FLOAT(0.f)), FLOAT(0.f)));
    const int steps = ARTF_CLOUD_SLICES * ARTF_CLOUD_STEPS;
    SpvId step = DIV_F(SUB_F(to, from), FLOAT((float)steps));
    SpvId mu = ADD_F(ADD_F(MUL_F(dir[0], c.toSun[0]), MUL_F(dir[1], c.toSun[1])), MUL_F(dir[2], c.toSun[2]));
    SpvId phase[3];
    for (int o = 0; o < 3; ++o) {
        const float g = o == 0 ? 1.f : o == 1 ? .5f : .25f;
        phase[o] = ADD_F(MUL_F(cloudHenyey(s, .8f * g, mu), FLOAT(.7f)), MUL_F(cloudHenyey(s, -.3f * g, mu), FLOAT(.3f)));
    }
    spvStore(s, lightVar, same3(s, 0.f));
    spvStore(s, leftVar, FLOAT(1.f));
    PostLoop loop;
    SpvId slice = postLoopBegin(s, &loop, counter, UINT(ARTF_CLOUD_SLICES));
    for (int k = 0; k < ARTF_CLOUD_STEPS; ++k) {
        SpvId index = TO_F(ADD_U(MUL_U(slice, UINT(ARTF_CLOUD_STEPS)), UINT((uint32_t)k)));
        SpvId t = ADD_F(from, MUL_F(ADD_F(index, jitter), step));
        const SpvId q[3] = {ADD_F(eye[0], MUL_F(dir[0], t)), ADD_F(eye[1], MUL_F(dir[1], t)), ADD_F(eye[2], MUL_F(dir[2], t))};
        SpvId h = SUB_F(SQRT_F(ADD_F(ADD_F(MUL_F(q[0], q[0]), MUL_F(q[1], q[1])), MUL_F(q[2], q[2]))), FLOAT(C_EARTH));
        SpvId left = spvLoad(s, s->floatType, leftVar);
        SpvId sigma = PICK_F(P_MORE(left, FLOAT(.005f)), cloudAt(s, &c, q, h, true), FLOAT(0.f));
        SpvId some;
        spvIfBegin(s, P_MORE(sigma, FLOAT(0.f)), &some);
        {
            /* toward the sun, in steps that grow */
            SpvId tau = FLOAT(0.f);
            float along = 0, sl = .06f;
            for (int sIdx = 0; sIdx < C_LIGHT_STEPS; ++sIdx) {
                const float atF = along + sl * .5f;
                const SpvId qq[3] = {ADD_F(q[0], MUL_F(c.toSun[0], FLOAT(atF))), ADD_F(q[1], MUL_F(c.toSun[1], FLOAT(atF))), ADD_F(q[2], MUL_F(c.toSun[2], FLOAT(atF)))};
                SpvId hh = SUB_F(SQRT_F(ADD_F(ADD_F(MUL_F(qq[0], qq[0]), MUL_F(qq[1], qq[1])), MUL_F(qq[2], qq[2]))), FLOAT(C_EARTH));
                tau = ADD_F(tau, MUL_F(cloudAt(s, &c, qq, hh, sIdx < 3), FLOAT(sl)));
                along += sl;
                sl *= 1.8f;
            }
            SpvId sun = cloudSunThrough(s, &c, q);
            SpvId powder = SUB_F(FLOAT(1.f), cloudExp(s, MUL_F(spvOp1(s, SPV_OP_FNegate, s->floatType, sigma), FLOAT(.2f))));
            SpvId edge = ADD_F(powder, MUL_F(SUB_F(FLOAT(1.f), powder), ADD_F(MUL_F(mu, FLOAT(.5f)), FLOAT(.5f))));
            SpvId beer = FLOAT(0.f);
            for (int o = 0; o < 3; ++o) {
                const float share = o == 0 ? 1.f : o == 1 ? .5f : .25f;
                beer = ADD_F(beer, MUL_F(MUL_F(FLOAT(share), cloudExp(s, MUL_F(spvOp1(s, SPV_OP_FNegate, s->floatType, tau), FLOAT(share)))), phase[o]));
            }
            SpvId frac = DIV_F(SUB_F(h, c.base), SUB_F(c.top, c.base)), sky = ADD_F(FLOAT(.25f), MUL_F(FLOAT(.75f), frac));
            SpvId through = cloudExp(s, MUL_F(spvOp1(s, SPV_OP_FNegate, s->floatType, sigma), step)), taken = SUB_F(FLOAT(1.f), through);
            SpvId keep, add;
            aerialSpv(s, &p->r, DIV_F(t, c.unit), mu, &keep, &add);
            const SpvId sunC[3] = {X(sun), Y(sun), Z(sun)}, keepC[3] = {X(keep), Y(keep), Z(keep)};
            SpvId lightNow = spvLoad(s, s->vec3, lightVar);
            const SpvId was[3] = {X(lightNow), Y(lightNow), Z(lightNow)};
            SpvId got[3];
            for (int cc = 0; cc < 3; ++cc) {
                SpvId lit = ADD_F(MUL_F(MUL_F(MUL_F(MUL_F(c.sun[cc], FLOAT(3.14159265f)), sunC[cc]), beer), edge), MUL_F(c.ambient[cc], sky));
                got[cc] = ADD_F(was[cc], MUL_F(MUL_F(MUL_F(left, lit), taken), keepC[cc]));
            }
            spvStore(s, lightVar, vec3Of(s, got[0], got[1], got[2]));
            spvStore(s, leftVar, MUL_F(left, through));
        }
        spvIfEnd(s, some);
    }
    cloudCellStore(s, p, grid, ADD_U(cell, ADD_U(slice, UINT(1))), vec4Of(s, spvLoad(s, s->vec3, lightVar), spvLoad(s, s->floatType, leftVar)));
    postLoopEnd(s, &loop);
    spvIfEnd(s, marching);
POST_FINISH()

/* Each pixel with something drawn, by what is in front of it: artfCloudsAt, into the second light. Push constant: how
 * many columns across. */
POST_MAIN(artfShaderPostCloudApply)
    SpvId grid = postBuffer(s, p, p->r.vec4Array, 74);
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId columns = postPush(s, p), rows = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->height, UINT(ARTF_CLOUD_TILE - 1)), UINT(ARTF_CLOUD_TILE));
    SpvId xf = TO_F(x), yf = TO_F(y);
    SpvId depth = postFloat(s, p, p->depth, i), here = postVec4(s, p, p->light, i);
    SpvId at = SUB_V3(postWorld(s, p, xf, yf, postAlong(s, p, depth)), spvShuffle3(s, frameVec(s, &p->r, F_CAMERA)));
    SpvId distance = SQRT_F(ADD_F(ADD_F(MUL_F(X(at), X(at)), MUL_F(Y(at), Y(at))), MUL_F(Z(at), Z(at))));
    SpvId fx = P_CLAMP(DIV_F(SUB_F(xf, FLOAT(ARTF_CLOUD_TILE * .5f - .5f)), FLOAT((float)ARTF_CLOUD_TILE)), FLOAT(0.f), TO_F(SUB_U(columns, UINT(1))));
    SpvId fy = P_CLAMP(DIV_F(SUB_F(yf, FLOAT(ARTF_CLOUD_TILE * .5f - .5f)), FLOAT((float)ARTF_CLOUD_TILE)), FLOAT(0.f), TO_F(SUB_U(rows, UINT(1))));
    SpvId x0 = P_TOU(fx), y0 = P_TOU(fy);
    SpvId x1 = spvGlsl2(s, 38 /* UMin */, s->uintType, ADD_U(x0, UINT(1)), SUB_U(columns, UINT(1))), y1 = spvGlsl2(s, 38, s->uintType, ADD_U(y0, UINT(1)), SUB_U(rows, UINT(1)));
    SpvId tx = SUB_F(fx, TO_F(x0)), ty = SUB_F(fy, TO_F(y0));
    const SpvId cxs[4] = {x0, x1, x0, x1}, cys[4] = {y0, y0, y1, y1};
    const SpvId weight[4] = {MUL_F(SUB_F(FLOAT(1.f), tx), SUB_F(FLOAT(1.f), ty)), MUL_F(tx, SUB_F(FLOAT(1.f), ty)), MUL_F(SUB_F(FLOAT(1.f), tx), ty), MUL_F(tx, ty)};
    SpvId sum = vec4Of(s, same3(s, 0.f), FLOAT(0.f));
    for (int n = 0; n < 4; ++n) {
        SpvId cell = MUL_U(ADD_U(MUL_U(cys[n], columns), cxs[n]), UINT(1 + ARTF_CLOUD_SLICES));
        SpvId span = bufferVec(s, &p->r, grid, cell);
        SpvId has = P_AND(P_MORE(Y(span), X(span)), P_MORE(distance, X(span)));
        SpvId z = MUL_F(MIN_F(DIV_F(SUB_F(distance, X(span)), MAX_F(SUB_F(Y(span), X(span)), FLOAT(1e-30f))), FLOAT(1.f)), FLOAT((float)ARTF_CLOUD_SLICES));
        SpvId k1 = spvGlsl2(s, 38, s->uintType, P_TOU(MAX_F(z, FLOAT(0.f))), UINT(ARTF_CLOUD_SLICES - 1));
        SpvId tz = SUB_F(z, TO_F(k1));
        SpvId b = bufferVec(s, &p->r, grid, ADD_U(cell, ADD_U(k1, UINT(1))));
        SpvId first = spvOp2(s, 170 /* OpIEqual */, s->boolType, k1, UINT(0));
        SpvId before = bufferVec(s, &p->r, grid, ADD_U(cell, spvGlsl2(s, 41 /* UMax */, s->uintType, k1, UINT(1))));
        SpvId aRgb = pick3(s, first, same3(s, 0.f), spvShuffle3(s, before)), aW = PICK_F(first, FLOAT(1.f), W(before));
        SpvId hereRgb = pick3(s, has, ADD_V3(aRgb, SCALE_V3(SUB_V3(spvShuffle3(s, b), aRgb), tz)), same3(s, 0.f));
        SpvId hereW = PICK_F(has, ADD_F(aW, MUL_F(SUB_F(W(b), aW), tz)), FLOAT(1.f));
        sum = spvOp2(s, SPV_OP_FAdd, s->vec4, sum, spvOp2(s, SPV_OP_VectorTimesScalar, s->vec4, vec4Of(s, hereRgb, hereW), weight[n]));
    }
    SpvId lit = ADD_V3(SCALE_V3(spvShuffle3(s, here), W(sum)), spvShuffle3(s, sum));
    postLightStore(s, p, p->scratch, i, vec4Of(s, pick3(s, P_MORE(depth, FLOAT(0.f)), lit, spvShuffle3(s, here)), W(here)));
POST_FINISH()

#undef POST_MAIN
#undef POST_FINISH
