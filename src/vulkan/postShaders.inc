/* artful — what comes after a frame in floats, on the GPU: post.c's passes, as compute shaders, with the same
 * arithmetic. Part of shaders.c.
 *
 * Bindings: 50 light (four floats a pixel, premultiplied), 51 depth (1 / w), 52 surface (packSurface's word),
 * 53 a second light, 54 the frame before's, 55 occlusion, 56 fog at a quarter (light it adds, and what it leaves),
 * 57 bloom's levels, one after the other; 58 and 59 what the scene drew, light and surface, as images. The push
 * constant says a pass's level. Each invocation is a pixel, 64 at a time. */

#define POST_BLOOM_LEVELS 6 /* as ARTF_POST_BLOOM_LEVELS */
#define POST_FOG_SCALE 4
#define POST_FOG_STEPS 16

typedef struct Post {
    Resources r;
    SpvId light, depth, surface, scratch, history, occlusion, fog, bloom;
    SpvId uvec3, global, push;
    SpvId width, height, widthF, heightF;
} Post;

static SpvId postBuffer(Spv* s, Post* p, SpvId array, uint32_t binding) { return spvResource(s, blockOf(s, array, true), SPV_STORAGE_StorageBuffer, binding); }

static void postDeclare(Spv* s, Post* p) {
    memset(p, 0, sizeof *p);
    declare(s, &p->r, false);
    SpvId floats = p->r.floatArray;
    p->light = postBuffer(s, p, p->r.vec4Array, 50);
    p->depth = postBuffer(s, p, floats, 51);
    p->surface = postBuffer(s, p, p->r.uintArray, 52);
    p->scratch = postBuffer(s, p, p->r.vec4Array, 53);
    p->history = postBuffer(s, p, p->r.vec4Array, 54);
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

/* After spvBeginMain: the frame's size. */
static void postSizes(Spv* s, Post* p) {
    p->widthF = W(frameVec(s, &p->r, F_FOG));
    p->heightF = W(frameVec(s, &p->r, F_EYE + 1));
    p->width = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, p->widthF);
    p->height = spvOp1(s, SPV_OP_ConvertFToU, s->uintType, p->heightF);
}

static SpvId postPush(Spv* s, Post* p) { return spvLoad(s, s->uintType, spvAccess(s, spvTypePointer(s, SPV_STORAGE_PushConstant, s->uintType), p->push, &p->r.zero, 1)); }

static SpvId postPtr(Spv* s, Post* p, SpvId buffer, SpvId type, SpvId i) {
    const SpvId idx[2] = {p->r.zero, i};
    return spvAccess(s, spvTypePointer(s, SPV_STORAGE_StorageBuffer, type), buffer, idx, 2);
}
static SpvId postVec4(Spv* s, Post* p, SpvId buffer, SpvId i) { return spvLoad(s, s->vec4, postPtr(s, p, buffer, s->vec4, i)); }
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

/* Distance along the view at a pixel (whole numbers, clamped); 0 where nothing is. */
static SpvId postDistance(Spv* s, Post* p, SpvId xf, SpvId yf) {
    SpvId x = P_TOU(P_CLAMP(xf, FLOAT(0.f), SUB_F(p->widthF, FLOAT(1.f)))), y = P_TOU(P_CLAMP(yf, FLOAT(0.f), SUB_F(p->heightF, FLOAT(1.f))));
    SpvId d = postFloat(s, p, p->depth, ADD_U(MUL_U(y, p->width), x));
    return PICK_F(P_MORE(d, FLOAT(0.f)), DIV_F(FLOAT(1.f), MAX_F(d, FLOAT(1e-30f))), FLOAT(0.f));
}

/* The world at a pixel's center, d along the view. */
static SpvId postWorld(Spv* s, Post* p, SpvId xf, SpvId yf, SpvId d) {
    SpvId px = SUB_F(MUL_F(DIV_F(ADD_F(xf, FLOAT(.5f)), p->widthF), FLOAT(2.f)), FLOAT(1.f));
    SpvId py = SUB_F(FLOAT(1.f), MUL_F(DIV_F(ADD_F(yf, FLOAT(.5f)), p->heightF), FLOAT(2.f)));
    SpvId dir = ADD_V3(ADD_V3(spvShuffle3(s, frameVec(s, &p->r, F_EYE + 2)), SCALE_V3(spvShuffle3(s, frameVec(s, &p->r, F_EYE)), px)), SCALE_V3(spvShuffle3(s, frameVec(s, &p->r, F_EYE + 1)), py));
    return ADD_V3(spvShuffle3(s, frameVec(s, &p->r, F_CAMERA)), SCALE_V3(dir, d));
}

/* Where a point falls through a matrix (four rows of the frame, by columns), in pixels; w along the view; *ok: in
 * front of the eye. */
static void postProject(Spv* s, Post* p, uint32_t matrix, SpvId q, SpvId* sx, SpvId* sy, SpvId* w, SpvId* ok) {
    SpvId q4 = vec4Of(s, q, FLOAT(1.f));
    SpvId c0 = frameVec(s, &p->r, matrix), c1 = frameVec(s, &p->r, matrix + 1), c2 = frameVec(s, &p->r, matrix + 2), c3 = frameVec(s, &p->r, matrix + 3);
    SpvId clip = ADD_V4(ADD_V4(SCALE_V4(c0, X(q4)), SCALE_V4(c1, Y(q4))), ADD_V4(SCALE_V4(c2, Z(q4)), c3));
    *w = W(clip);
    *ok = P_MORE(*w, FLOAT(1e-5f));
    SpvId inv = DIV_F(FLOAT(1.f), MAX_F(*w, FLOAT(1e-5f)));
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

/* ---- what the scene drew, into the buffers ---- */
POST_MAIN(artfShaderPostGather)
    SpvId floatImage, floatSampled = spvTypeSampledImage2D(s, &floatImage);
    SpvId uintImage, uintSampled = spvTypeSampledImage2DOf(s, s->uintType, &uintImage);
    SpvId color = spvResource(s, floatSampled, SPV_STORAGE_UniformConstant, 58);
    SpvId surfaceImage = spvResource(s, uintSampled, SPV_STORAGE_UniformConstant, 59);
    SpvId depthImage = spvResource(s, floatSampled, SPV_STORAGE_UniformConstant, 8);
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
    spvStore(s, postPtr(s, p, p->light, s->vec4, i), spvImageFetchAs(s, s->vec4, floatImage, spvLoad(s, floatSampled, color), at));
    spvStore(s, postPtr(s, p, p->surface, s->uintType, i), spvExtract(s, s->uintType, spvImageFetchAs(s, uvec4, uintImage, spvLoad(s, uintSampled, surfaceImage), at), 0));
    spvIfEnd(s, rest);
    spvIfBegin(s, depthAlone, &depthOnly);
    /* (reversed depth is near / distance: 1 / w is depth over near) */
    SpvId z = X(spvImageFetchAs(s, s->vec4, floatImage, spvLoad(s, floatSampled, depthImage), at));
    spvStore(s, postPtr(s, p, p->depth, s->floatType, i), DIV_F(z, Z(frameVec(s, &p->r, F_PROJECTION))));
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
    SpvId here = postPtr(s, p, p->light, s->vec4, i), light = spvLoad(s, s->vec4, here);
    spvStore(s, here, vec4Of(s, SCALE_V3(spvShuffle3(s, light), keep), W(light)));
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
    SpvId v = NORMALIZE(SUB_V3(at, spvShuffle3(s, frameVec(s, &p->r, F_CAMERA))));
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
    reach = PICK_F(P_MORE(wDrop, FLOAT(0.f)), MIN_F(reach, DIV_F(MUL_F(w0, FLOAT(.9f)), MAX_F(wDrop, FLOAT(1e-20f)))), reach);
    postProject(s, p, F_VIEWPROJ, ADD_V3(at, SCALE_V3(r, reach)), &sx1, &sy1, &w1, &ok1);
    SpvId inv0 = DIV_F(FLOAT(1.f), w0), invStep = SUB_F(DIV_F(FLOAT(1.f), MAX_F(w1, FLOAT(1e-5f))), inv0);
    SpvId jitter = DIV_F(TO_F(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, postHash(s, i), UINT(255))), FLOAT(256.f));
    SpvId found = spvConstBool(s, false), before = spvConstBool(s, true), stopped = P_NOT(ok1), hx = FLOAT(0.f), hy = FLOAT(0.f);
    for (int step = 1; step <= 24; ++step) {
        SpvId share = DIV_F(ADD_F(FLOAT((float)step), jitter), FLOAT(25.f)), t = MUL_F(share, share);
        SpvId sx = ADD_F(sx0, MUL_F(SUB_F(sx1, sx0), t)), sy = ADD_F(sy0, MUL_F(SUB_F(sy1, sy0), t));
        SpvId w = DIV_F(FLOAT(1.f), ADD_F(inv0, MUL_F(invStep, t)));
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
    SpvId c1 = ADD_F(FLOAT(1.f), vn), c2 = MUL_F(c1, c1), fresnel = MUL_F(MUL_F(c2, c2), c1);
    SpvId f0 = ADD_F(FLOAT(.04f), MUL_F(FLOAT(.96f), metal));
    SpvId mirror = MUL_F(MUL_F(MUL_F(ADD_F(f0, MUL_F(SUB_F(FLOAT(1.f), f0), fresnel)), MUL_F(smooth, smooth)), fade), X(frameVec(s, &p->r, F_POST + 1)));
    SpvId sum = ADD_F(ADD_F(X(mine), Y(mine)), Z(mine));
    SpvId tint = ADD_V3(same3(s, 1.f), SCALE_V3(SUB_V3(SCALE_V3(spvShuffle3(s, mine), DIV_F(FLOAT(3.f), MAX_F(sum, FLOAT(1e-6f)))), same3(s, 1.f)), PICK_F(P_MORE(sum, FLOAT(1e-6f)), metal, FLOAT(0.f))));
    SpvId seen = spvShuffle3(s, postTap(s, p, p->light, UINT(0), p->width, p->height, hx, hy));
    SpvId reflected = SCALE_V3(MUL_V3(seen, tint), mirror);
    SpvId use = P_AND(P_AND(found, P_MORE(d, FLOAT(0.f))), P_AND(spvOp2(s, SPV_OP_INotEqual, s->boolType, word, UINT(0)), spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, rough, FLOAT(.6f))));
    use = P_AND(use, spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, away, FLOAT(-.2f)));
    spvStore(s, postPtr(s, p, p->bloom, s->vec4, half), vec4Of(s, pick3(s, use, reflected, same3(s, 0.f)), PICK_F(use, mirror, FLOAT(0.f))));
    spvIfEnd(s, marching);
POST_FINISH()

POST_MAIN(artfShaderPostReflectionApply)
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId hw = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->width, UINT(1)), UINT(2)), hh = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->height, UINT(1)), UINT(2));
    SpvId got = postTap(s, p, p->bloom, UINT(0), hw, hh, SUB_F(MUL_F(ADD_F(TO_F(x), FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f)), SUB_F(MUL_F(ADD_F(TO_F(y), FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f)));
    SpvId here = postPtr(s, p, p->light, s->vec4, i), light = spvLoad(s, s->vec4, here);
    spvStore(s, here, vec4Of(s, ADD_V3(SCALE_V3(spvShuffle3(s, light), SUB_F(FLOAT(1.f), W(got))), spvShuffle3(s, got)), W(light)));
POST_FINISH()

/* ---- fog that light goes through: as fogRow, at a quarter, then fogApplyRow ---- */
POST_MAIN(artfShaderPostFog)
    Shadows shadows;
    SpvId shadowImage, shadowSampled = spvTypeSampledImage2D(s, &shadowImage);
    shadows.block = spvResource(s, blockOf(s, spvTypeArray(s, s->vec4, S_ROWS, 16), true), SPV_STORAGE_Uniform, 24);
    shadows.atlas = spvResource(s, shadowSampled, SPV_STORAGE_UniformConstant, 25);
    shadows.imageType = shadowImage;
    shadows.sampled = shadowSampled;
    shadows.ivec2 = spvTypeVector(s, s->intType, 2);
    shadows.r = &p->r;
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId fw = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->width, UINT(POST_FOG_SCALE - 1)), UINT(POST_FOG_SCALE));
    SpvId fh = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->height, UINT(POST_FOG_SCALE - 1)), UINT(POST_FOG_SCALE));
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, fw, fh);
    SpvId look = frameVec(s, &p->r, F_LOOK), postRow = frameVec(s, &p->r, F_POST + 1);
    SpvId density = Y(look), amount = Y(postRow), g = Z(postRow);
    SpvId d = FLOAT(3e38f);
    for (int dy = 0; dy < POST_FOG_SCALE; ++dy)
        for (int dx = 0; dx < POST_FOG_SCALE; ++dx) {
            SpvId there = postDistance(s, p, ADD_F(MUL_F(TO_F(x), FLOAT((float)POST_FOG_SCALE)), FLOAT((float)dx)), ADD_F(MUL_F(TO_F(y), FLOAT((float)POST_FOG_SCALE)), FLOAT((float)dy)));
            d = MIN_F(d, PICK_F(P_MORE(there, FLOAT(0.f)), there, FLOAT(3e38f)));
        }
    SpvId far = MIN_F(d, DIV_F(FLOAT(4.f), MAX_F(density, FLOAT(1e-6f))));
    SpvId cx = SUB_F(ADD_F(MUL_F(TO_F(x), FLOAT((float)POST_FOG_SCALE)), FLOAT(POST_FOG_SCALE * .5f)), FLOAT(.5f)), cy = SUB_F(ADD_F(MUL_F(TO_F(y), FLOAT((float)POST_FOG_SCALE)), FLOAT(POST_FOG_SCALE * .5f)), FLOAT(.5f));
    SpvId eye = spvShuffle3(s, frameVec(s, &p->r, F_CAMERA));
    SpvId dir = SUB_V3(postWorld(s, p, cx, cy, FLOAT(1.f)), eye);
    SpvId along = spvGlsl1(s, SPV_GLSL_Length, s->floatType, dir);
    SpvId toSun = spvShuffle3(s, frameVec(s, &p->r, F_TOSUN)), sunColor = spvShuffle3(s, frameVec(s, &p->r, F_SUNCOLOR));
    SpvId mu = DIV_F(DOT3(dir, toSun), along);
    SpvId hg = SUB_F(ADD_F(FLOAT(1.f), MUL_F(g, g)), MUL_F(MUL_F(FLOAT(2.f), g), mu));
    SpvId phase = DIV_F(SUB_F(FLOAT(1.f), MUL_F(g, g)), MUL_F(MUL_F(FLOAT(12.566371f), hg), SQRT_F(hg)));
    SpvId step = DIV_F(far, FLOAT((float)POST_FOG_STEPS));
    SpvId jitter = DIV_F(TO_F(spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, postHash(s, MUL_U(ADD_U(MUL_U(y, fw), x), UINT(2654435761u))), UINT(255))), FLOAT(256.f));
    SpvId xx = MUL_F(MUL_F(density, step), along);
    SpvId stepThrough = DIV_F(FLOAT(1.f), ADD_F(ADD_F(FLOAT(1.f), xx), MUL_F(MUL_F(xx, xx), FLOAT(.5f))));
    SpvId fogColor = spvShuffle3(s, frameVec(s, &p->r, F_FOG));
    SpvId sunRow = shadowRow(s, &shadows, UINT(S_SUN)), splits = shadowRow(s, &shadows, UINT(S_SPLITS));
    SpvId casts = spvOp2(s, SPV_OP_INotEqual, s->boolType, AS_UINT(X(sunRow)), UINT(0));
    SpvId count = AS_UINT(Y(sunRow));
    SpvId lastSplit = PICK_F(spvOp2(s, 170, s->boolType, count, UINT(1)), X(splits), PICK_F(spvOp2(s, 170, s->boolType, count, UINT(2)), Y(splits), Z(splits)));
    SpvId light = same3(s, 0.f), left = FLOAT(1.f);
    for (int k = 0; k < POST_FOG_STEPS; ++k) {
        SpvId t = MUL_F(ADD_F(FLOAT((float)k), jitter), step);
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
        SpvId taken = MUL_F(left, SUB_F(FLOAT(1.f), stepThrough));
        light = ADD_V3(light, SCALE_V3(ADD_V3(SCALE_V3(sunColor, MUL_F(MUL_F(sun, phase), FLOAT(12.566371f))), fogColor), taken));
        left = MUL_F(left, stepThrough);
    }
    spvStore(s, postPtr(s, p, p->fog, s->vec4, i), vec4Of(s, SCALE_V3(light, amount), SUB_F(FLOAT(1.f), MUL_F(SUB_F(FLOAT(1.f), left), amount))));
POST_FINISH()

POST_MAIN(artfShaderPostFogApply)
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId fw = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->width, UINT(POST_FOG_SCALE - 1)), UINT(POST_FOG_SCALE));
    SpvId fh = spvOp2(s, SPV_OP_UDiv, s->uintType, ADD_U(p->height, UINT(POST_FOG_SCALE - 1)), UINT(POST_FOG_SCALE));
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, p->width, p->height);
    SpvId fx = SUB_F(DIV_F(ADD_F(TO_F(x), FLOAT(.5f)), FLOAT((float)POST_FOG_SCALE)), FLOAT(.5f)), fy = SUB_F(DIV_F(ADD_F(TO_F(y), FLOAT(.5f)), FLOAT((float)POST_FOG_SCALE)), FLOAT(.5f));
    SpvId fog = postTap(s, p, p->fog, UINT(0), fw, fh, fx, fy);
    SpvId here = postPtr(s, p, p->light, s->vec4, i), light = spvLoad(s, s->vec4, here);
    spvStore(s, here, vec4Of(s, ADD_V3(SCALE_V3(spvShuffle3(s, light), W(fog)), SCALE_V3(spvShuffle3(s, fog), W(light))), W(light)));
POST_FINISH()

/* ---- temporal antialiasing: as temporalRow, into the second light ---- */
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
    SpvId d = postDistance(s, p, xf, yf);
    SpvId jitter = frameVec(s, &p->r, F_POST + 2);
    SpvId at = postWorld(s, p, SUB_F(xf, Z(jitter)), ADD_F(yf, W(jitter)), PICK_F(P_MORE(d, FLOAT(0.f)), d, FLOAT(1e4f)));
    SpvId sx, sy, w, ok;
    postProject(s, p, F_PREVIOUS, at, &sx, &sy, &w, &ok);
    SpvId valid = P_AND(ok, P_AND(P_AND(spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, sx, FLOAT(0.f)), spvOp2(s, SPV_OP_FOrdGreaterThanEqual, s->boolType, sy, FLOAT(0.f))), P_AND(spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, sx, SUB_F(p->widthF, FLOAT(1.f))), spvOp2(s, SPV_OP_FOrdLessThanEqual, s->boolType, sy, SUB_F(p->heightF, FLOAT(1.f))))));
    /* (the push constant is 1 when there is a frame before to follow) */
    valid = P_AND(valid, spvOp2(s, SPV_OP_INotEqual, s->boolType, postPush(s, p), UINT(0)));
    SpvId now = postVec4(s, p, p->light, i);
    SpvId was = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, spvShuffle3(s, postTap(s, p, p->history, UINT(0), p->width, p->height, sx, sy)), lo), hi);
    SpvId share = PICK_F(valid, FLOAT(.1f), FLOAT(1.f));
    spvStore(s, postPtr(s, p, p->scratch, s->vec4, i), vec4Of(s, P_MIX3(was, spvShuffle3(s, now), share), W(now)));
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
    postSizes(s, p);
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
    spvStore(s, postPtr(s, p, p->scratch, s->vec4, i), vec4Of(s, pick3(s, P_LESS(mine, FLOAT(.5f)), spvShuffle3(s, here), blurred), W(here)));
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

POST_MAIN(artfShaderPostBloomDown)
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId level = postPush(s, p);
    SpvId start, w, h, aboveStart, aboveW, aboveH;
    postLevel(s, p, level, &start, &w, &h);
    postLevel(s, p, SUB_U(spvGlsl2(s, 41, s->uintType, level, UINT(1)), UINT(1)), &aboveStart, &aboveW, &aboveH);
    SpvId i, x, y, merge;
    postBegin(s, p, &i, &x, &y, &merge, w, h);
    SpvId first = spvOp2(s, 170, s->boolType, level, UINT(0));
    SpvId look = frameVec(s, &p->r, F_LOOK);
    SpvId exposure = PICK_F(P_MORE(X(look), FLOAT(0.f)), X(look), FLOAT(1.f));
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
    postSizes(s, p);
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
uint32_t* artfShaderPostFinal(size_t* size) {
    Spv b, *s = &b;
    spvInit(s);
    Post post, *p = &post;
    postDeclare(s, p);
    SpvId fragCoord = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Input, s->vec4), SPV_STORAGE_Input);
    spvDecorate(s, fragCoord, SPV_DECORATE_BuiltIn, SPV_BUILTIN_FragCoord);
    SpvId outColor = spvVariable(s, spvTypePointer(s, SPV_STORAGE_Output, s->vec4), SPV_STORAGE_Output);
    spvDecorate(s, outColor, SPV_DECORATE_Location, 0);
    SpvId main = spvBeginMain(s);
    postSizes(s, p);
    SpvId coord = spvLoad(s, s->vec4, fragCoord);
    SpvId x = P_TOU(FLOOR_F(X(coord))), y = P_TOU(FLOOR_F(Y(coord)));
    SpvId light = postVec4(s, p, p->light, ADD_U(MUL_U(y, p->width), x));
    SpvId alpha = P_CLAMP(W(light), FLOAT(0.f), FLOAT(1.f));
    SpvId look = frameVec(s, &p->r, F_LOOK), postRow = frameVec(s, &p->r, F_POST);
    SpvId exposure = X(look), bloom = X(postRow);
    SpvId v = SCALE_V3(spvShuffle3(s, light), PICK_F(P_MORE(alpha, FLOAT(0.f)), DIV_F(FLOAT(1.f), MAX_F(alpha, FLOAT(1e-6f))), FLOAT(1.f)));
    v = SCALE_V3(v, PICK_F(P_MORE(exposure, FLOAT(0.f)), exposure, FLOAT(1.f)));
    SpvId start, bw, bh;
    postLevel(s, p, UINT(0), &start, &bw, &bh);
    SpvId glow = spvShuffle3(s, postTap(s, p, p->bloom, UINT(0), bw, bh, SUB_F(MUL_F(ADD_F(TO_F(x), FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f)), SUB_F(MUL_F(ADD_F(TO_F(y), FLOAT(.5f)), FLOAT(.5f)), FLOAT(.5f))));
    v = pick3(s, P_MORE(bloom, FLOAT(0.f)), ADD_V3(v, SCALE_V3(glow, DIV_F(bloom, MAX_F(alpha, FLOAT(1e-3f))))), v);
    v = spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, v, same3(s, 0.f)), same3(s, 65504.f));
    SpvId vc[5];
    const float numbers[5] = {2.51f, .03f, 2.43f, .59f, .14f};
    for (int k = 0; k < 5; ++k) vc[k] = same3(s, numbers[k]);
    SpvId mapped = spvOp2(s, SPV_OP_FDiv, s->vec3, MUL_V3(v, ADD_V3(MUL_V3(v, vc[0]), vc[1])), ADD_V3(MUL_V3(v, ADD_V3(MUL_V3(v, vc[2]), vc[3])), vc[4]));
    v = pick3(s, P_MORE(exposure, FLOAT(0.f)), mapped, v);
    SpvId shown = spvGlsl1(s, SPV_GLSL_Sqrt, s->vec3, spvGlsl2(s, SPV_GLSL_FMin, s->vec3, spvGlsl2(s, SPV_GLSL_FMax, s->vec3, v, same3(s, 0.f)), same3(s, 1.f)));
    spvStore(s, outColor, vec4Of(s, SCALE_V3(shown, alpha), alpha));
    spvEndMain(s);
    const SpvId io[2] = {fragCoord, outColor};
    spvEntryPoint(s, SPV_MODEL_Fragment, main, io, 2);
    spvExecutionMode(s, main, 7, 0, 0, 0, 0);
    return finish(s, size);
}

#undef POST_MAIN
#undef POST_FINISH
