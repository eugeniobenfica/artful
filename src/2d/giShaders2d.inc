/* artful-2d — gi on the GPU (as gi2d.c): included at the end of shaders2d.c, on its helpers. */

/* while (condition) { ... }: the condition is worked out between whileBegin and whileTest. */
typedef struct While2D { SpvId header, test, body, next, done; } While2D;

static void whileBegin(Sh* h, While2D* l) {
    l->header = spvNewId(S), l->test = spvNewId(S), l->body = spvNewId(S), l->next = spvNewId(S), l->done = spvNewId(S);
    spvBranch(S, l->header);
    spvLabel(S, l->header);
    spvLoopMerge(S, l->done, l->next);
    spvBranch(S, l->test);
    spvLabel(S, l->test);
}
static void whileTest(Sh* h, While2D* l, SpvId condition) {
    spvBranchIf(S, condition, l->body, l->done);
    spvLabel(S, l->body);
}
static void whileEnd(Sh* h, While2D* l) {
    spvBranch(S, l->next);
    spvLabel(S, l->next);
    spvBranch(S, l->header);
    spvLabel(S, l->done);
}

static SpvId floatPtr(Sh* h, int b, SpvId i) {
    const SpvId idx[2] = {h->zero, i};
    return spvAccess(S, h->ptrFloat, h->buffers[b], idx, 2);
}
static SpvId zero4(Sh* h) { return vec4Of(h, FLOAT(0.f), FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)); }
static SpvId asInt(Sh* h, SpvId u) { return spvOp1(S, SPV_OP_Bitcast, S->intType, u); }

/* ---- what is solid at each texel, and what it sends: outlines, what walls sent on the frame before, what glows ---- */

uint32_t* artf2DShaderGiScene(size_t* size) {
    Spv spv;
    Sh hh, *h = &hh;
    static const int used[] = {B2_GLOBALS, B2_ITEMS, B2_EDGES, B2_TEXELS, B2_LEVELS, B2_OUTLINES, B2_POINTS, B2_EMITTERS, B2_SOLID, B2_SOURCE, B2_CELLS_PREV};
    begin(h, &spv, used, (int)ARTF_COUNT(used));
    SpvId main = spvBeginMain(S);
    SpvId solid = fnVar(h, h->fnUint), source = fnVar(h, h->fnVec4), wind = fnVar(h, h->fnInt);
    SpvId outlineI = fnVar(h, h->fnUint), pointI = fnVar(h, h->fnUint), emitterI = fnVar(h, h->fnUint), edgeI = fnVar(h, h->fnUint);
    SpvId id = spvLoad(S, h->uvec3, h->global);
    SpvId gx = spvExtract(S, S->uintType, id, 0), gy = spvExtract(S, S->uintType, id, 1);
    SpvId gSize = vecAt(h, B2_GLOBALS, UINT(G_SIZE)), gMode = vecAt(h, B2_GLOBALS, UINT(G_MODE)), gGi = vecAt(h, B2_GLOBALS, UINT(G_GI));
    SpvId w = AS_U(Y(gMode)), hgt = AS_U(Z(gMode)), scale = AS_U(W(gSize));
    SpvId merge;
    spvIfBegin(S, AND_B(LESS_U(gx, w), LESS_U(gy, hgt)), &merge);
    {
        SpvId scaleF = TO_F(scale);
        SpvId x = MUL_F(ADD_F(TO_F(gx), FLOAT(.5f)), scaleF), y = MUL_F(ADD_F(TO_F(gy), FLOAT(.5f)), scaleF);
        SpvId reach = MUL_F(scaleF, FLOAT(.75f));
        spvStore(S, solid, UINT(0));
        spvStore(S, source, zero4(h));
        /* outlines: closed ones as their inside (non-zero winding), open ones as lines a texel wide */
        Loop2D outlines;
        SpvId o = loopBegin(h, &outlines, outlineI, AS_U(Z(gGi)));
        {
            SpvId head = vecAt(h, B2_OUTLINES, MUL_U(o, UINT(2))), box = vecAt(h, B2_OUTLINES, ADD_U(MUL_U(o, UINT(2)), UINT(1)));
            SpvId start = AS_U(X(head)), count = AS_U(Y(head));
            SpvId closed = AND_B(NE_U(AS_U(Z(head)), UINT(0)), MORE_EQ_U(count, UINT(3)));
            SpvId near_;
            spvIfBegin(S, AND_B(MORE_EQ_F(y, SUB_F(Y(box), reach)), LESS_EQ_F(y, ADD_F(W(box), reach))), &near_);
            {
                SpvId lo = spvGlsl2(S, GLSL_SMax, S->intType, TO_I(DIV_F(X(box), scaleF)), INT(0));
                SpvId hi = ADD_I(TO_I(DIV_F(Z(box), scaleF)), INT(1));
                SpvId gxi = asInt(h, gx);
                SpvId inside;
                spvIfBegin(S, AND_B(closed, AND_B(MORE_EQ_I(gxi, lo), LESS_I(gxi, hi))), &inside);
                {
                    spvStore(S, wind, INT(0));
                    Loop2D points;
                    SpvId i = loopBegin(h, &points, pointI, count);
                    {
                        SpvId next = ADD_U(i, UINT(1));
                        next = PICK_U(EQ_U(next, count), UINT(0), next);
                        SpvId a = vecAt(h, B2_POINTS, ADD_U(start, i)), b = vecAt(h, B2_POINTS, ADD_U(start, next));
                        SpvId crosses = spvOp2(S, 165 /* OpLogicalNotEqual */, S->boolType, LESS_EQ_F(Y(a), y), LESS_EQ_F(Y(b), y));
                        SpvId cx = ADD_F(X(a), MUL_F(DIV_F(SUB_F(y, Y(a)), SUB_F(Y(b), Y(a))), SUB_F(X(b), X(a))));
                        SpvId add = PICK_I(AND_B(crosses, MORE_F(cx, x)), PICK_I(MORE_F(Y(b), Y(a)), INT(1), INT(-1)), INT(0));
                        spvStore(S, wind, ADD_I(LOAD_I(wind), add));
                    }
                    loopEnd(h, &points);
                    spvStore(S, solid, PICK_U(NE_I(LOAD_I(wind), INT(0)), UINT(1), LOAD_U(solid)));
                }
                spvIfEnd(S, inside);
                SpvId open;
                spvIfBegin(S, NOT_B(closed), &open);
                {
                    Loop2D segments;
                    SpvId i = loopBegin(h, &segments, pointI, SUB_U(spvGlsl2(S, SPV_GLSL_UMax, S->uintType, count, UINT(1)), UINT(1)));
                    {
                        SpvId a = vecAt(h, B2_POINTS, ADD_U(start, i)), b = vecAt(h, B2_POINTS, ADD_U(start, ADD_U(i, UINT(1))));
                        SpvId ex = SUB_F(X(b), X(a)), ey = SUB_F(Y(b), Y(a)), len2 = ADD_F(MUL_F(ex, ex), MUL_F(ey, ey));
                        SpvId along = DIV_F(ADD_F(MUL_F(SUB_F(x, X(a)), ex), MUL_F(SUB_F(y, Y(a)), ey)), MAX_F(len2, FLOAT(1e-30f)));
                        SpvId t = PICK_F(MORE_F(len2, FLOAT(0.f)), CLAMP_F(along, FLOAT(0.f), FLOAT(1.f)), FLOAT(0.f));
                        SpvId dx = SUB_F(SUB_F(x, X(a)), MUL_F(ex, t)), dy = SUB_F(SUB_F(y, Y(a)), MUL_F(ey, t));
                        SpvId hit = LESS_EQ_F(ADD_F(MUL_F(dx, dx), MUL_F(dy, dy)), MUL_F(reach, reach));
                        spvStore(S, solid, PICK_U(hit, UINT(1), LOAD_U(solid)));
                    }
                    loopEnd(h, &segments);
                }
                spvIfEnd(S, open);
            }
            spvIfEnd(S, near_);
        }
        loopEnd(h, &outlines);
        /* what walls send on, from the frame before */
        SpvId at = ADD_U(MUL_U(gy, w), gx);
        SpvId back = vecAt(h, B2_CELLS_PREV, at);
        SpvId bounce = Y(gGi);
        SpvId sends = AND_B(AND_B(NE_U(LOAD_U(solid), UINT(0)), MORE_F(W(back), FLOAT(0.f))), MORE_F(bounce, FLOAT(0.f)));
        SpvId k = DIV_F(bounce, MAX_F(W(back), FLOAT(1e-30f)));
        spvStore(S, source, spvOp2(S, SPV_OP_VectorTimesScalar, S->vec4, vec4Of(h, X(back), Y(back), Z(back), FLOAT(0.f)), PICK_F(sends, k, FLOAT(0.f))));
        /* what glows */
        Loop2D emitters;
        SpvId e = loopBegin(h, &emitters, emitterI, AS_U(W(gGi)));
        {
            SpvId item = uintAt(h, B2_EMITTERS, e);
            SpvId base = MUL_U(item, UINT(ITEM_ROWS));
#define ROW(r) vecAt(h, B2_ITEMS, ADD_U(base, UINT(r)))
            SpvId box = ROW(I_BOX);
            SpvId gx0 = DIV_U(AS_U(X(box)), scale), gx1 = spvGlsl2(S, SPV_GLSL_UMin, S->uintType, DIV_U(SUB_U(ADD_U(AS_U(Z(box)), scale), UINT(1)), scale), w);
            SpvId inBox = AND_B(AND_B(MORE_EQ_F(y, TO_F(AS_U(Y(box)))), LESS_F(y, TO_F(AS_U(W(box))))), AND_B(MORE_EQ_U(gx, gx0), LESS_U(gx, gx1)));
            SpvId boxMerge;
            spvIfBegin(S, inBox, &boxMerge);
            {
                SpvId m0 = ROW(I_MAP0), m1 = ROW(I_MAP1), flagsRow = ROW(I_FLAGS);
                SpvId lx = ADD_F(ADD_F(MUL_F(X(m0), x), MUL_F(Y(m0), y)), Z(m0));
                SpvId ly = ADD_F(ADD_F(MUL_F(W(m0), x), MUL_F(X(m1), y)), Y(m1));
                SpvId flags = AS_U(X(flagsRow));
                SpvId polygon = NE_U(AND_U(flags, UINT(I2_POLYGON)), UINT(0)), nearest = NE_U(AND_U(flags, UINT(I2_NEAREST)), UINT(0));
                SpvId dist = shapeDistance(h, ROW(I_SHAPE), ROW(I_LINE), AS_U(W(m1)), lx, ly);
                SpvId shaped = CLAMP_F(SUB_F(FLOAT(.5f), MUL_F(dist, Z(m1))), FLOAT(0.f), FLOAT(1.f));
                /* a polygon: inside or not, by its edges in the frame */
                spvStore(S, wind, INT(0));
                SpvId polyMerge;
                spvIfBegin(S, polygon, &polyMerge);
                {
                    Loop2D edges;
                    SpvId i = loopBegin(h, &edges, edgeI, AS_U(W(flagsRow)));
                    {
                        SpvId g = vecAt(h, B2_EDGES, ADD_U(AS_U(Z(flagsRow)), i));
                        SpvId crosses = spvOp2(S, 165, S->boolType, LESS_EQ_F(Y(g), y), LESS_EQ_F(W(g), y));
                        SpvId cx = ADD_F(X(g), MUL_F(DIV_F(SUB_F(y, Y(g)), SUB_F(W(g), Y(g))), SUB_F(Z(g), X(g))));
                        SpvId add = PICK_I(AND_B(crosses, MORE_F(cx, x)), PICK_I(MORE_F(W(g), Y(g)), INT(1), INT(-1)), INT(0));
                        spvStore(S, wind, ADD_I(LOAD_I(wind), add));
                    }
                    loopEnd(h, &edges);
                }
                spvIfEnd(S, polyMerge);
                SpvId cover = PICK_F(polygon, PICK_F(NE_I(LOAD_I(wind), INT(0)), FLOAT(1.f), FLOAT(0.f)), shaped);
                SpvId covered;
                spvIfBegin(S, MORE_EQ_F(cover, FLOAT(.5f)), &covered);
                {
                    SpvId uv = ROW(I_UV), colorRow = ROW(I_COLOR), emitRow = ROW(I_EMIT), texRow = ROW(I_TEX);
                    SpvId u = ADD_F(X(uv), MUL_F(lx, Y(uv))), v = ADD_F(Z(uv), MUL_F(ly, W(uv)));
                    const SpvId none = UINT(0xFFFFFFFFu);
                    SpvId tex0 = AS_U(X(texRow)), tex2 = AS_U(Z(texRow));
                    SpvId t0 = sampleOf(h, PICK_U(EQ_U(tex0, none), UINT(0), tex0), nearest, u, v);
                    SpvId t2 = sampleOf(h, PICK_U(EQ_U(tex2, none), UINT(0), tex2), nearest, u, v);
                    SpvId alpha = PICK_F(NE_U(tex0, none), W(t0), FLOAT(1.f));
                    SpvId ke = MUL_F(W(colorRow), alpha);
                    SpvId emit[3];
                    for (uint32_t c = 0; c < 3; ++c) {
                        SpvId m = PICK_F(NE_U(tex2, none), spvExtract(S, S->floatType, t2, c), FLOAT(1.f));
                        emit[c] = MUL_F(MUL_F(spvExtract(S, S->floatType, emitRow, c), m), ke);
                    }
                    SpvId glows;
                    spvIfBegin(S, MORE_F(ADD_F(ADD_F(emit[0], emit[1]), emit[2]), FLOAT(0.f)), &glows);
                    spvStore(S, solid, UINT(1));
                    spvStore(S, source, spvOp2(S, SPV_OP_FAdd, S->vec4, spvLoad(S, S->vec4, source), vec4Of(h, emit[0], emit[1], emit[2], FLOAT(0.f))));
                    spvIfEnd(S, glows);
                }
                spvIfEnd(S, covered);
            }
            spvIfEnd(S, boxMerge);
#undef ROW
        }
        loopEnd(h, &emitters);
        spvStore(S, uintPtr(h, B2_SOLID, at), LOAD_U(solid));
        spvStore(S, vecPtr(h, B2_SOURCE, at), spvLoad(S, S->vec4, source));
    }
    spvIfEnd(S, merge);
    return end(h, main, size);
}

/* ---- the distances: squared along each row, then along each column, and their roots ----
 * (the lower envelope of parabolas, as distanceLine: a line an invocation, its scratch of 3 n + 2 floats its own) */

uint32_t* artf2DShaderGiDistance(size_t* size, bool columns) {
    Spv spv;
    Sh hh, *h = &hh;
    static const int used[] = {B2_GLOBALS, B2_SOLID, B2_DIST, B2_SCRATCH};
    begin(h, &spv, used, (int)ARTF_COUNT(used));
    SpvId main = spvBeginMain(S);
    SpvId k = fnVar(h, h->fnInt), qI = fnVar(h, h->fnUint), sVar = fnVar(h, h->fnFloat);
    SpvId id = spvLoad(S, h->uvec3, h->global);
    SpvId line = spvExtract(S, S->uintType, id, 0);
    SpvId gMode = vecAt(h, B2_GLOBALS, UINT(G_MODE));
    SpvId w = AS_U(Y(gMode)), hgt = AS_U(Z(gMode));
    SpvId n = columns ? hgt : w, lines = columns ? w : hgt;
    SpvId merge;
    spvIfBegin(S, LESS_U(line, lines), &merge);
    {
        SpvId region = MUL_U(line, ADD_U(MUL_U(n, UINT(3)), UINT(2)));
        SpvId dBase = region, zBase = ADD_U(region, n), vBase = ADD_U(ADD_U(region, MUL_U(n, UINT(2))), UINT(1));
#define D(q) floatPtr(h, B2_SCRATCH, ADD_U(dBase, q))
#define Z_(q) floatPtr(h, B2_SCRATCH, ADD_U(zBase, q))
#define V(q) floatPtr(h, B2_SCRATCH, ADD_U(vBase, q))
#define AT(q) (columns ? ADD_U(MUL_U(q, w), line) : ADD_U(MUL_U(line, w), q))
        const float big = 3.4e38f;
        spvStore(S, k, INT(-1));
        Loop2D first;
        SpvId q = loopBegin(h, &first, qI, n);
        {
            SpvId f = columns ? LOAD_F(floatPtr(h, B2_DIST, AT(q))) : PICK_F(NE_U(uintAt(h, B2_SOLID, AT(q)), UINT(0)), FLOAT(0.f), FLOAT(1e20f));
            spvStore(S, D(q), f);
            SpvId some;
            spvIfBegin(S, LESS_F(f, FLOAT(1e19f)), &some);
            {
                SpvId isFirst = LESS_I(LOAD_I(k), INT(0));
                SpvId qf = TO_F(q);
                SpvId firstMerge;
                spvIfBegin(S, isFirst, &firstMerge);
                spvStore(S, k, INT(0));
                spvStore(S, V(UINT(0)), qf);
                spvStore(S, Z_(UINT(0)), FLOAT(-big));
                spvStore(S, Z_(UINT(1)), FLOAT(big));
                spvIfEnd(S, firstMerge);
                SpvId laterMerge;
                spvIfBegin(S, NOT_B(isFirst), &laterMerge);
                {
                    While2D back;
                    whileBegin(h, &back);
                    SpvId kk = I_AS_U(LOAD_I(k));
                    SpvId vk = LOAD_F(V(kk));
                    SpvId dv = LOAD_F(D(TO_U(vk)));
                    SpvId s = DIV_F(SUB_F(ADD_F(f, MUL_F(qf, qf)), ADD_F(dv, MUL_F(vk, vk))), MUL_F(FLOAT(2.f), SUB_F(qf, vk)));
                    spvStore(S, sVar, s);
                    whileTest(h, &back, LESS_EQ_F(s, LOAD_F(Z_(kk))));
                    spvStore(S, k, SUB_I(LOAD_I(k), INT(1)));
                    whileEnd(h, &back);
                    SpvId k1 = ADD_I(LOAD_I(k), INT(1));
                    spvStore(S, k, k1);
                    spvStore(S, V(I_AS_U(k1)), qf);
                    spvStore(S, Z_(I_AS_U(k1)), LOAD_F(sVar));
                    spvStore(S, Z_(ADD_U(I_AS_U(k1), UINT(1))), FLOAT(big));
                }
                spvIfEnd(S, laterMerge);
            }
            spvIfEnd(S, some);
        }
        loopEnd(h, &first);
        SpvId empty = LESS_I(LOAD_I(k), INT(0));
        SpvId emptyMerge;
        spvIfBegin(S, empty, &emptyMerge);
        {
            Loop2D fill;
            SpvId qq = loopBegin(h, &fill, qI, n);
            spvStore(S, floatPtr(h, B2_DIST, AT(qq)), FLOAT(columns ? 1e10f : 1e20f));
            loopEnd(h, &fill);
        }
        spvIfEnd(S, emptyMerge);
        SpvId fullMerge;
        spvIfBegin(S, NOT_B(empty), &fullMerge);
        {
            spvStore(S, k, INT(0));
            Loop2D out;
            SpvId qq = loopBegin(h, &out, qI, n);
            {
                SpvId qf = TO_F(qq);
                While2D ahead;
                whileBegin(h, &ahead);
                whileTest(h, &ahead, LESS_F(LOAD_F(Z_(ADD_U(I_AS_U(LOAD_I(k)), UINT(1)))), qf));
                spvStore(S, k, ADD_I(LOAD_I(k), INT(1)));
                whileEnd(h, &ahead);
                SpvId vk = LOAD_F(V(I_AS_U(LOAD_I(k))));
                SpvId dq = SUB_F(qf, vk);
                SpvId value = ADD_F(MUL_F(dq, dq), LOAD_F(D(TO_U(vk))));
                spvStore(S, floatPtr(h, B2_DIST, AT(qq)), columns ? SQRT_F(value) : value);
            }
            loopEnd(h, &out);
        }
        spvIfEnd(S, fullMerge);
#undef D
#undef Z_
#undef V
#undef AT
    }
    spvIfEnd(S, merge);
    return endSized(h, main, size, 64, 1);
}

/* ---- a cascade: a ray an invocation, marched through the distances; what it did not meet, from the one above ---- */

uint32_t* artf2DShaderGiCascade(size_t* size, bool odd) {
    Spv spv;
    Sh hh, *h = &hh;
    static const int used[] = {B2_GLOBALS, B2_SOLID, B2_SOURCE, B2_DIST, B2_RAYS_EVEN, B2_RAYS_ODD};
    begin(h, &spv, used, (int)ARTF_COUNT(used));
    const int out = odd ? B2_RAYS_ODD : B2_RAYS_EVEN, up = odd ? B2_RAYS_EVEN : B2_RAYS_ODD;
    SpvId main = spvBeginMain(S);
    SpvId tVar = fnVar(h, h->fnFloat), done = fnVar(h, h->fnUint), light = fnVar(h, h->fnVec4), through = fnVar(h, h->fnFloat);
    SpvId id = spvLoad(S, h->uvec3, h->global);
    SpvId L = ADD_U(MUL_U(spvExtract(S, S->uintType, id, 1), UINT(GI_ROW)), spvExtract(S, S->uintType, id, 0));
    SpvId gMode = vecAt(h, B2_GLOBALS, UINT(G_MODE));
    SpvId w = AS_U(Y(gMode)), hgt = AS_U(Z(gMode));
    SpvId s = pushAt(h, 0), n = pushAt(h, 1), pw = pushAt(h, 2), ph = pushAt(h, 3), top = NE_U(pushAt(h, 4), UINT(0));
    SpvId t0 = AS_F(pushAt(h, 5)), t1 = AS_F(pushAt(h, 6));
    SpvId merge;
    spvIfBegin(S, LESS_U(L, MUL_U(MUL_U(pw, ph), n)), &merge);
    {
        SpvId probe = DIV_U(L, n), k = SUB_U(L, MUL_U(probe, n));
        SpvId px = spvOp2(S, SPV_OP_UMod, S->uintType, probe, pw), py = DIV_U(probe, pw);
        SpvId sf = TO_F(s);
        SpvId ox = MUL_F(ADD_F(TO_F(px), FLOAT(.5f)), sf), oy = MUL_F(ADD_F(TO_F(py), FLOAT(.5f)), sf);
        SpvId angle = MUL_F(DIV_F(ADD_F(TO_F(k), FLOAT(.5f)), TO_F(n)), FLOAT(2.f * PI2D));
        SpvId dx = spvGlsl1(S, SPV_GLSL_Cos, S->floatType, angle), dy = spvGlsl1(S, SPV_GLSL_Sin, S->floatType, angle);
        SpvId wf = TO_F(w), hf = TO_F(hgt);
        spvStore(S, tVar, t0);
        spvStore(S, done, UINT(0));
        spvStore(S, light, zero4(h));
        spvStore(S, through, FLOAT(1.f));
        While2D march;
        whileBegin(h, &march);
        SpvId t = LOAD_F(tVar);
        whileTest(h, &march, AND_B(LESS_F(t, t1), EQ_U(LOAD_U(done), UINT(0))));
        {
            SpvId x = ADD_F(ox, MUL_F(dx, t)), y = ADD_F(oy, MUL_F(dy, t));
            SpvId outside = OR_B(OR_B(LESS_F(x, FLOAT(0.f)), LESS_F(y, FLOAT(0.f))), OR_B(MORE_EQ_F(x, wf), MORE_EQ_F(y, hf)));
            SpvId leaves;
            spvIfBegin(S, outside, &leaves);
            spvStore(S, done, UINT(1));
            spvIfEnd(S, leaves);
            SpvId stays;
            spvIfBegin(S, NOT_B(outside), &stays);
            {
                SpvId i = ADD_U(MUL_U(TO_U(y), w), TO_U(x));
                SpvId hit = NE_U(uintAt(h, B2_SOLID, i), UINT(0));
                SpvId hitMerge;
                spvIfBegin(S, hit, &hitMerge);
                spvStore(S, light, vecAt(h, B2_SOURCE, i));
                spvStore(S, through, FLOAT(0.f));
                spvStore(S, done, UINT(1));
                spvIfEnd(S, hitMerge);
                SpvId missMerge;
                spvIfBegin(S, NOT_B(hit), &missMerge);
                spvStore(S, tVar, ADD_F(t, MAX_F(SUB_F(LOAD_F(floatPtr(h, B2_DIST, i)), FLOAT(1.5f)), FLOAT(.5f))));
                spvIfEnd(S, missMerge);
            }
            spvIfEnd(S, stays);
        }
        whileEnd(h, &march);
        SpvId farMerge;
        spvIfBegin(S, AND_B(MORE_F(LOAD_F(through), FLOAT(0.f)), NOT_B(top)), &farMerge);
        {
            /* (the four rays above nearest in direction, at the four probes above, blended) */
            SpvId us = MUL_U(s, UINT(2));
            SpvId upw = DIV_U(SUB_U(ADD_U(w, us), UINT(1)), us), uph = DIV_U(SUB_U(ADD_U(hgt, us), UINT(1)), us), un = MUL_U(n, UINT(4));
            SpvId usf = TO_F(us);
            SpvId fx = SUB_F(DIV_F(ox, usf), FLOAT(.5f)), fy = SUB_F(DIV_F(oy, usf), FLOAT(.5f));
            SpvId flx = FLOOR_F(fx), fly = FLOOR_F(fy);
            SpvId wx = CLAMP_F(SUB_F(fx, flx), FLOAT(0.f), FLOAT(1.f)), wy = CLAMP_F(SUB_F(fy, fly), FLOAT(0.f), FLOAT(1.f));
            SpvId lastX = SUB_F(TO_F(upw), FLOAT(1.f)), lastY = SUB_F(TO_F(uph), FLOAT(1.f));
            SpvId ui[2] = {TO_U(CLAMP_F(flx, FLOAT(0.f), lastX)), TO_U(CLAMP_F(ADD_F(flx, FLOAT(1.f)), FLOAT(0.f), lastX))};
            SpvId uj[2] = {TO_U(CLAMP_F(fly, FLOAT(0.f), lastY)), TO_U(CLAMP_F(ADD_F(fly, FLOAT(1.f)), FLOAT(0.f), lastY))};
            SpvId weight[4] = {MUL_F(SUB_F(FLOAT(1.f), wx), SUB_F(FLOAT(1.f), wy)), MUL_F(wx, SUB_F(FLOAT(1.f), wy)), MUL_F(SUB_F(FLOAT(1.f), wx), wy), MUL_F(wx, wy)};
            SpvId far[3] = {FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)};
            for (int q = 0; q < 4; ++q) {
                SpvId first = ADD_U(MUL_U(ADD_U(MUL_U(uj[q >> 1], upw), ui[q & 1]), un), MUL_U(k, UINT(4)));
                SpvId a = vecAt(h, up, first), b = vecAt(h, up, ADD_U(first, UINT(1))), c = vecAt(h, up, ADD_U(first, UINT(2))), d = vecAt(h, up, ADD_U(first, UINT(3)));
                for (uint32_t ch = 0; ch < 3; ++ch) {
                    SpvId sum = ADD_F(ADD_F(ADD_F(spvExtract(S, S->floatType, a, ch), spvExtract(S, S->floatType, b, ch)), spvExtract(S, S->floatType, c, ch)),
                                      spvExtract(S, S->floatType, d, ch));
                    far[ch] = ADD_F(far[ch], MUL_F(MUL_F(weight[q], FLOAT(.25f)), sum));
                }
            }
            SpvId was = spvLoad(S, S->vec4, light), th = LOAD_F(through);
            spvStore(S, light, vec4Of(h, ADD_F(X(was), MUL_F(far[0], th)), ADD_F(Y(was), MUL_F(far[1], th)), ADD_F(Z(was), MUL_F(far[2], th)), FLOAT(0.f)));
        }
        spvIfEnd(S, farMerge);
        spvStore(S, vecPtr(h, out, L), spvLoad(S, S->vec4, light));
    }
    spvIfEnd(S, merge);
    return endSized(h, main, size, 64, 1);
}

/* ---- the light at each texel: cascade 0's four rays, averaged ---- */

uint32_t* artf2DShaderGiGather(size_t* size) {
    Spv spv;
    Sh hh, *h = &hh;
    static const int used[] = {B2_GLOBALS, B2_GI, B2_RAYS_EVEN};
    begin(h, &spv, used, (int)ARTF_COUNT(used));
    SpvId main = spvBeginMain(S);
    SpvId id = spvLoad(S, h->uvec3, h->global);
    SpvId gx = spvExtract(S, S->uintType, id, 0), gy = spvExtract(S, S->uintType, id, 1);
    SpvId gMode = vecAt(h, B2_GLOBALS, UINT(G_MODE)), gGi = vecAt(h, B2_GLOBALS, UINT(G_GI));
    SpvId w = AS_U(Y(gMode)), hgt = AS_U(Z(gMode));
    SpvId merge;
    spvIfBegin(S, AND_B(LESS_U(gx, w), LESS_U(gy, hgt)), &merge);
    {
        SpvId i = ADD_U(MUL_U(gy, w), gx);
        SpvId a = vecAt(h, B2_RAYS_EVEN, MUL_U(i, UINT(4))), b = vecAt(h, B2_RAYS_EVEN, ADD_U(MUL_U(i, UINT(4)), UINT(1)));
        SpvId c = vecAt(h, B2_RAYS_EVEN, ADD_U(MUL_U(i, UINT(4)), UINT(2))), d = vecAt(h, B2_RAYS_EVEN, ADD_U(MUL_U(i, UINT(4)), UINT(3)));
        for (uint32_t ch = 0; ch < 3; ++ch) {
            SpvId sum = ADD_F(ADD_F(ADD_F(spvExtract(S, S->floatType, a, ch), spvExtract(S, S->floatType, b, ch)), spvExtract(S, S->floatType, c, ch)),
                              spvExtract(S, S->floatType, d, ch));
            spvStore(S, floatPtr(h, B2_GI, ADD_U(MUL_U(i, UINT(3)), UINT(ch))), MUL_F(MUL_F(sum, FLOAT(.25f)), X(gGi)));
        }
    }
    spvIfEnd(S, merge);
    return end(h, main, size);
}

/* ---- into what is solid, from its neighbours: pass 0 from what is open (binding 8 into 22), 1 from all (22 into 8) ---- */

uint32_t* artf2DShaderGiSpread(size_t* size, int pass) {
    Spv spv;
    Sh hh, *h = &hh;
    static const int used[] = {B2_GLOBALS, B2_GI, B2_GI_TMP, B2_SOLID};
    begin(h, &spv, used, (int)ARTF_COUNT(used));
    const int from = pass ? B2_GI_TMP : B2_GI, to = pass ? B2_GI : B2_GI_TMP;
    SpvId main = spvBeginMain(S);
    SpvId id = spvLoad(S, h->uvec3, h->global);
    SpvId gx = spvExtract(S, S->uintType, id, 0), gy = spvExtract(S, S->uintType, id, 1);
    SpvId gMode = vecAt(h, B2_GLOBALS, UINT(G_MODE));
    SpvId w = AS_U(Y(gMode)), hgt = AS_U(Z(gMode));
    SpvId merge;
    spvIfBegin(S, AND_B(LESS_U(gx, w), LESS_U(gy, hgt)), &merge);
    {
        SpvId i = ADD_U(MUL_U(gy, w), gx);
        SpvId isSolid = NE_U(uintAt(h, B2_SOLID, i), UINT(0));
        SpvId sum[3] = {FLOAT(0.f), FLOAT(0.f), FLOAT(0.f)}, count = FLOAT(0.f);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                SpvId nx = ADD_I(asInt(h, gx), INT(dx)), ny = ADD_I(asInt(h, gy), INT(dy));
                SpvId within = AND_B(AND_B(MORE_EQ_I(nx, INT(0)), MORE_EQ_I(ny, INT(0))), AND_B(LESS_I(nx, asInt(h, w)), LESS_I(ny, asInt(h, hgt))));
                SpvId j = PICK_U(within, ADD_U(MUL_U(I_AS_U(ny), w), I_AS_U(nx)), UINT(0));
                SpvId takes = within;
                if (pass == 0) takes = AND_B(takes, EQ_U(uintAt(h, B2_SOLID, j), UINT(0)));
                for (int c = 0; c < 3; ++c)
                    sum[c] = ADD_F(sum[c], PICK_F(takes, floatAt(h, from, ADD_U(MUL_U(j, UINT(3)), UINT((uint32_t)c))), FLOAT(0.f)));
                count = ADD_F(count, PICK_F(takes, FLOAT(1.f), FLOAT(0.f)));
            }
        for (int c = 0; c < 3; ++c) {
            SpvId own = floatAt(h, from, ADD_U(MUL_U(i, UINT(3)), UINT((uint32_t)c)));
            SpvId mean = PICK_F(MORE_F(count, FLOAT(0.f)), DIV_F(sum[c], MAX_F(count, FLOAT(1.f))), FLOAT(0.f));
            spvStore(S, floatPtr(h, to, ADD_U(MUL_U(i, UINT(3)), UINT((uint32_t)c))), PICK_F(isSolid, mean, own));
        }
    }
    spvIfEnd(S, merge);
    return end(h, main, size);
}
