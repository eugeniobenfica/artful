/* artful — the shading model, written once: what a surface gives back of a light, and of what comes from all around.
 *
 * It is written on a few operations, and included once for each way it is run, with those operations defined as that
 * way has them:
 *
 *   scalar    floats (sceneInternal.h, for trace.c and for what the kernels work out lane by lane): artfShade...
 *   lanes     eight lanes, V8F (kernels.inc, once for each instruction set): lanes...
 *   SPIR-V    calls that build a shader (shaders.c, for the scene's pixel shader and the tracing shader): shader...
 *
 * so the formulas are one, and what differs stays where they are called: lanes against branches, where light is
 * added up, what stands in the way. Each formula keeps the order of its operations, as floats round, so that the
 * CPU's frames keep their bytes; the GPU's follow the same order.
 *
 * Before including it, a way defines:
 *   S_FN(name)         the name of this way's copy of a function
 *   S_DEF              how they are declared (static and inline)
 *   S_SELF, S_PASS     a first parameter and its argument that every function takes ("Spv* s," and "s,"), or nothing
 *   S_F, S_V3          a number and three of them; S_FLAG a choice known before a light's loop (bool, or a SpvId)
 *   S_WIDE             1 for lanes: what the lanes work out one at a time (sheen, a thin film) is not there
 *   S_C(x)             a constant
 *   S_ADD S_SUB S_MUL S_DIV S_MIN S_MAX (a, b), S_SQRT(a)
 *   S_GT(a, b), and S_SELECT(c, a, b): c ? a : b, lane by lane
 *   S_PICK(flag, a, b) flag ? a : b, for a flag that may be no flag at all: b alone is worked out then
 *   S_V3_OF(x, y, z), S_X(v) S_Y(v) S_Z(v), S_DOT3(a, b), S_NORMALIZE(a)
 *   and, where there are no lanes: S_LOG2, S_EXP2 (artfLog2Fast and artfExp2Fast, the same everywhere) and S_COS
 *   (artfCosFast), and S_CLAMP(a, lo, hi).
 * A way may also define S_ADD3 S_SUB3 S_MUL3 S_MAX3 (a, b), S_SCALE3(v, f) and S_SPLAT3(f) where it has vectors of its
 * own; else they are worked out channel by channel here.
 *
 * Lights have pi taken out, as they always had here: a white surface under a light of 1 shows 1. So the distribution
 * has no 1 / pi either. Nothing here calls a C library's pow, exp or sin: fifth powers are multiplied out. */

/* ---- three numbers, channel by channel where a way has no vectors ---- */

#ifndef S_ADD3
S_DEF S_V3 S_FN(Add3)(S_SELF S_V3 a, S_V3 b) { return S_V3_OF(S_ADD(S_X(a), S_X(b)), S_ADD(S_Y(a), S_Y(b)), S_ADD(S_Z(a), S_Z(b))); }
S_DEF S_V3 S_FN(Sub3)(S_SELF S_V3 a, S_V3 b) { return S_V3_OF(S_SUB(S_X(a), S_X(b)), S_SUB(S_Y(a), S_Y(b)), S_SUB(S_Z(a), S_Z(b))); }
S_DEF S_V3 S_FN(Mul3)(S_SELF S_V3 a, S_V3 b) { return S_V3_OF(S_MUL(S_X(a), S_X(b)), S_MUL(S_Y(a), S_Y(b)), S_MUL(S_Z(a), S_Z(b))); }
S_DEF S_V3 S_FN(Max3)(S_SELF S_V3 a, S_V3 b) { return S_V3_OF(S_MAX(S_X(a), S_X(b)), S_MAX(S_Y(a), S_Y(b)), S_MAX(S_Z(a), S_Z(b))); }
S_DEF S_V3 S_FN(Scale3)(S_SELF S_V3 a, S_F f) { return S_V3_OF(S_MUL(S_X(a), f), S_MUL(S_Y(a), f), S_MUL(S_Z(a), f)); }
#define S_ADD3(a, b) S_FN(Add3)(S_PASS a, b)
#define S_SUB3(a, b) S_FN(Sub3)(S_PASS a, b)
#define S_MUL3(a, b) S_FN(Mul3)(S_PASS a, b)
#define S_MAX3(a, b) S_FN(Max3)(S_PASS a, b)
#define S_SCALE3(a, f) S_FN(Scale3)(S_PASS a, f)
#define S_SPLAT3(f) S_V3_OF(f, f, f)
#define S_OWN_V3_
#endif

/* ---- a surface's highlight: Cook and Torrance's ---- */

/* Schlick's (1 - x)^5, multiplied out: as y^2 squared, times y. */
S_DEF S_F S_FN(Fifth)(S_SELF S_F x) {
    const S_F y = S_SUB(S_C(1.f), x), y2 = S_MUL(y, y);
    return S_MUL(S_MUL(y2, y2), y);
}
/* The same power, multiplied in a row (y y y y y): what the tracer's lights and its coat have always done. Kept apart
 * only so that traced frames keep their bytes; the two differ in the last bit at most. */
S_DEF S_F S_FN(FifthInRow)(S_SELF S_F x) {
    const S_F y = S_SUB(S_C(1.f), x);
    return S_MUL(S_MUL(S_MUL(S_MUL(y, y), y), y), y);
}

/* Schlick's Fresnel: what a surface reflects head on (f0), toward all of it at a grazing angle. far: Fifth(v . h). */
S_DEF S_F S_FN(Schlick)(S_SELF S_F f0, S_F far) { return S_ADD(f0, S_MUL(S_SUB(S_C(1.f), f0), far)); }
S_DEF S_V3 S_FN(Schlick3)(S_SELF S_V3 f0, S_F far) { return S_ADD3(f0, S_SCALE3(S_SUB3(S_SPLAT3(S_C(1.f)), f0), far)); }
/* The same for a clear coat: of no color, F0 .04. */
S_DEF S_F S_FN(CoatFresnel)(S_SELF S_F far) { return S_ADD(S_C(.04f), S_MUL(S_C(.96f), far)); }

/* What a surface reflects head on: a dielectric's (.04 x specular, of a color or not) where it is not metal, its color
 * where it is. */
S_DEF S_F S_FN(Reflects)(S_SELF S_F dielectric, S_F notMetal, S_F albedo, S_F metal) { return S_ADD(S_MUL(dielectric, notMetal), S_MUL(albedo, metal)); }
S_DEF S_V3 S_FN(Reflects3)(S_SELF S_V3 dielectric, S_F notMetal, S_V3 albedo, S_F metal) { return S_ADD3(S_SCALE3(dielectric, notMetal), S_SCALE3(albedo, metal)); }

/* Trowbridge and Reitz's distribution (GGX), without 1 / pi: below is d in a2 / d^2, which the tracer's coat wants as
 * it is. a2: alpha squared, alpha being roughness squared. */
S_DEF S_F S_FN(GgxBelow)(S_SELF S_F ndh, S_F a2) { return S_ADD(S_MUL(S_MUL(ndh, ndh), S_SUB(a2, S_C(1.f))), S_C(1.f)); }
S_DEF S_F S_FN(Ggx)(S_SELF S_F ndh, S_F a2) {
    const S_F d = S_FN(GgxBelow)(S_PASS ndh, a2);
    return S_DIV(a2, S_MUL(d, d));
}
/* The same drawn out (KHR_materials_anisotropy): x and y are h along and across the way it is drawn, each over its
 * alpha, and scale 1 over both alphas; the distribution is then scale / (x^2 + y^2 + (n . h)^2)^2. */
S_DEF S_F S_FN(GgxDrawn)(S_SELF S_F x, S_F y, S_F ndh, S_F scale) {
    const S_F q = S_ADD(S_ADD(S_MUL(x, x), S_MUL(y, y)), S_MUL(ndh, ndh));
    return S_DIV(scale, S_MUL(q, q));
}

/* Smith's visibility, as Schlick's k = (roughness + 1)^2 / 8 makes it: what the eye and the light see of the facets,
 * times 4 n.v n.l of the lobe's denominator, without the 4. */
S_DEF S_F S_FN(Seen)(S_SELF S_F ndv, S_F ndl, S_F k) {
    const S_F rest = S_SUB(S_C(1.f), k);
    return S_MUL(S_ADD(S_MUL(ndv, rest), k), S_ADD(S_MUL(ndl, rest), k));
}

/* The lobe without its Fresnel: D (n . l) scale / (4 seen), and nothing from a light behind the surface. scale: the
 * light's strength times how much the surface reflects. */
S_DEF S_F S_FN(Specular)(S_SELF S_F distribution, S_F ndl, S_F ndv, S_F k, S_F scale) {
    const S_F seen = S_FN(Seen)(S_PASS ndv, ndl, k);
    const S_F lit = S_DIV(S_MUL(S_MUL(distribution, ndl), scale), S_MUL(seen, S_C(4.f)));
    return S_SELECT(S_GT(ndl, S_C(0.f)), lit, S_C(0.f));
}

/* What a lobe needs of a surface: alpha squared, Schlick's k and n . v; and drawn out, the ways along and across, each
 * over its alpha, and 1 over both alphas, where drawn says so (no flag: never). */
typedef struct S_FN(Lobe) {
    S_F a2, k, ndv;
    S_FLAG drawn;
    S_V3 along, across;
    S_F scale;
} S_FN(Lobe);

S_DEF S_FN(Lobe) S_FN(LobeOf)(S_SELF S_F rough, S_F ndv) {
    const S_F a = S_MUL(rough, rough), plusOne = S_ADD(rough, S_C(1.f));
    S_FN(Lobe) l;
    l.a2 = S_MUL(a, a);
    l.k = S_MUL(S_MUL(plusOne, plusOne), S_C(.125f));
    l.ndv = ndv;
    l.drawn = 0; /* (along, across and scale are read only drawn out: Draw sets them) */
    return l;
}

/* Drawn out: how long the highlight is, over its alpha, for glTF's strength squared (alpha mixed toward 1 by it). */
S_DEF S_F S_FN(DrawnLong)(S_SELF S_F a, S_F strength2) { return S_DIV(S_C(1.f), S_ADD(a, S_MUL(S_SUB(S_C(1.f), a), strength2))); }
/* t and b: the way u grows, square to the normal, and the normal across it; turned by (c, sn); long: DrawnLong's. */
S_DEF void S_FN(Draw)(S_SELF S_FN(Lobe)* l, S_FLAG drawn, S_V3 t, S_V3 b, S_F c, S_F sn, S_F a, S_F longer) {
    const S_F wide = S_DIV(S_C(1.f), S_MAX(a, S_C(1e-4f)));
    l->along = S_SCALE3(S_ADD3(S_SCALE3(t, c), S_SCALE3(b, sn)), longer);
    l->across = S_SCALE3(S_SUB3(S_SCALE3(b, c), S_SCALE3(t, sn)), wide);
    l->scale = S_MUL(longer, wide);
    l->drawn = drawn;
}

/* Halfway between the eye and the light. */
S_DEF S_V3 S_FN(Halfway)(S_SELF S_V3 view, S_V3 toLight) { return S_NORMALIZE(S_ADD3(view, toLight)); }

/* One light's highlight, without its Fresnel (which takes Fifth(*vdh)): h from Halfway, ndl n . l (above 0 to count),
 * scale as Specular's. drawn: the lobe's, or 0 where it is known never to be (a coat's), which leaves that out. */
S_DEF S_F S_FN(Highlight)(S_SELF const S_FN(Lobe)* l, S_FLAG drawn, S_V3 n, S_V3 view, S_V3 h, S_F ndl, S_F scale, S_F* vdh) {
    const S_F ndh = S_MAX(S_DOT3(n, h), S_C(0.f));
    *vdh = S_MAX(S_DOT3(view, h), S_C(0.f));
    const S_F distribution = S_PICK(drawn, S_FN(GgxDrawn)(S_PASS S_DOT3(l->along, h), S_DOT3(l->across, h), ndh, l->scale), S_FN(Ggx)(S_PASS ndh, l->a2));
    return S_FN(Specular)(S_PASS distribution, ndl, l->ndv, l->k, scale);
}

/* ---- light the lobe loses ----
 *
 * The lobe is light that leaves the surface from one facet. What meets a second facet first is not in it, and a rough
 * metal is darker than it is: GGX's of roughness 1, Smith's masking, Fresnel 1, gives back .31 of light from straight
 * on. Turquin (2019) puts it back, the lobe times 1 + f0 (1 / E - 1), E being all the lobe gives back from n . v (its
 * albedo): a white metal then gives back all it gets, a dielectric little more, as what the second facet sends on is
 * reflected as often. */

/* 1 / E - 1, of n . v and roughness: least squares to E of GGX's lobe (Smith's masking, height-correlated: what the
 * tracer's paths reflect by), in x = n . v / (n . v + alpha sin), as masking goes by alpha tan, and in roughness. Off
 * by .8% of what it makes whole at most, at any angle; 0 at roughness 0. */
#define S_ROW_(c1, c2, c3, c4, c5) \
    S_MUL(S_ADD(S_MUL(S_ADD(S_MUL(S_ADD(S_MUL(S_ADD(S_MUL(S_C(c5), rough), S_C(c4)), rough), S_C(c3)), rough), S_C(c2)), rough), S_C(c1)), rough)
S_DEF S_F S_FN(Unkept)(S_SELF S_F ndv, S_F rough) {
    const S_F a = S_MUL(rough, rough), sine = S_SQRT(S_MAX(S_SUB(S_C(1.f), S_MUL(ndv, ndv)), S_C(0.f)));
    const S_F x = S_DIV(ndv, S_MAX(S_ADD(ndv, S_MUL(a, sine)), S_C(1e-20f)));
    S_F g = S_ROW_(-36.8876762f, 179.17543f, -259.47818f, 111.392197f, 16.6388912f);
    g = S_ADD(S_MUL(g, x), S_ROW_(157.066681f, -767.819092f, 1298.90527f, -892.311279f, 179.991226f));
    g = S_ADD(S_MUL(g, x), S_ROW_(-226.132965f, 1119.03772f, -2049.99438f, 1643.00623f, -470.446014f));
    g = S_ADD(S_MUL(g, x), S_ROW_(132.335831f, -667.578064f, 1273.3894f, -1086.42896f, 344.982513f));
    g = S_ADD(S_MUL(g, x), S_ROW_(-28.9286957f, 150.582138f, -289.063629f, 247.651779f, -76.8523941f));
    g = S_ADD(S_MUL(g, x), S_ROW_(2.54451299f, -13.2217445f, 25.5772476f, -21.719202f, 6.84259319f));
    return S_MAX(g, S_C(0.f));
}
#undef S_ROW_
/* What the lobe is multiplied by, a channel and three: 1 + f0 Unkept. */
S_DEF S_F S_FN(Gain)(S_SELF S_F f0, S_F unkept) { return S_ADD(S_C(1.f), S_MUL(f0, unkept)); }
S_DEF S_V3 S_FN(Gain3)(S_SELF S_V3 f0, S_F unkept) { return S_ADD3(S_SPLAT3(S_C(1.f)), S_SCALE3(f0, unkept)); }

/* ---- highlights smaller than a pixel ----
 *
 * Where the normal turns much from one pixel to the next, a highlight narrower than that falls between pixels, and
 * flickers as things move. Tokuyoshi and Kaplanyan (2019, 2021) widen the lobe by as much as the normal turns over a
 * pixel: alpha^2 grows by twice the normal's variance there, by at most .18. variance: of the normal over the pixel
 * (what turns it from pixel to pixel, times a pixel's own variance, 1/4; and that of a normal map's texels, Toksvig's
 * (1 - |n|) / |n|, as their mean is shorter where they differ). Roughness as it was where nothing turns. */
S_DEF S_F S_FN(Widened)(S_SELF S_F rough, S_F variance) {
    const S_F a = S_MUL(rough, rough);
    const S_F a2 = S_MIN(S_ADD(S_MUL(a, a), S_MIN(S_MUL(variance, S_C(2.f)), S_C(.18f))), S_C(1.f));
    return S_SELECT(S_GT(variance, S_C(0.f)), S_SQRT(S_SQRT(a2)), rough);
}
/* A normal map's texel, as it was read (between texels and levels, each channel from 0 to 1): the variance of what it
 * is the mean of, by how much shorter than 1 it is; less a little, for bytes round what was of length 1. */
S_DEF S_F S_FN(MapVariance)(S_SELF S_F x, S_F y, S_F z) {
    const S_F bx = S_SUB(S_MUL(x, S_C(2.f)), S_C(1.f)), by = S_SUB(S_MUL(y, S_C(2.f)), S_C(1.f)), bz = S_SUB(S_MUL(z, S_C(2.f)), S_C(1.f));
    const S_F length = S_SQRT(S_ADD(S_ADD(S_MUL(bx, bx), S_MUL(by, by)), S_MUL(bz, bz)));
    return S_DIV(S_MAX(S_SUB(S_SUB(S_C(1.f), length), S_C(1.f / 128.f)), S_C(0.f)), S_MAX(length, S_C(.1f)));
}

/* ---- lights with a size ---- */

/* The mirror ray. */
S_DEF S_V3 S_FN(MirrorRay)(S_SELF S_V3 n, S_V3 view) { return S_SUB3(S_SCALE3(n, S_MUL(S_C(2.f), S_DOT3(n, view))), view); }

/* A ball of light this size, its middle d away (not of length 1), seen by a surface facing n from view: the direction to
 * its point nearest the mirror ray, which its highlight comes from, and how much that highlight keeps, as it is spread
 * over as wide as the ball looks (Karis, 2013): energy. */
S_DEF S_V3 S_FN(BallNearest)(S_SELF S_V3 n, S_V3 view, S_V3 d, S_F size, S_F rough, S_F* energy) {
    const S_V3 r = S_FN(MirrorRay)(S_PASS n, view);
    const S_V3 across = S_SUB3(S_SCALE3(r, S_DOT3(d, r)), d);
    const S_F share = S_MIN(S_DIV(size, S_MAX(S_SQRT(S_DOT3(across, across)), S_C(1e-6f))), S_C(1.f));
    const S_V3 nearest = S_ADD3(d, S_SCALE3(across, share));
    const S_F a = S_MUL(rough, rough), wider = S_MIN(S_ADD(a, S_DIV(S_MUL(size, S_C(.5f)), S_SQRT(S_MAX(S_DOT3(d, d), S_C(1e-8f))))), S_C(1.f));
    const S_F kept = S_DIV(a, wider);
    *energy = S_MUL(kept, kept);
    return S_NORMALIZE(nearest);
}

/* ---- what comes from all around ---- */

/* Light from all around, with no picture of it: the ground's below, the sky's above (rise: the sky's less the
 * ground's), by up, from 0 straight down to 1 straight up. */
S_DEF S_F S_FN(SkyGround)(S_SELF S_F ground, S_F rise, S_F up) { return S_ADD(ground, S_MUL(rise, up)); }
S_DEF S_V3 S_FN(SkyGround3)(S_SELF S_V3 ground, S_V3 sky, S_F up) { return S_ADD3(ground, S_SCALE3(S_SUB3(sky, ground), up)); }

/* What a surface shows of what comes from all around in its mirror direction, a channel: what it reflects head on,
 * toward max(smooth, f0) at a grazing angle (smooth: 1 - roughness; far: Fifth(n . v) times open, as open is how much
 * of all around reaches it). Where there is no occlusion, open is 1. */
S_DEF S_F S_FN(Mirror)(S_SELF S_F f0, S_F smooth, S_F far, S_F open) { return S_ADD(S_MUL(f0, open), S_MUL(S_SUB(S_MAX(smooth, f0), f0), far)); }
S_DEF S_V3 S_FN(Mirror3)(S_SELF S_V3 f0, S_F smooth, S_F far, S_F open) {
    return S_ADD3(S_SCALE3(f0, open), S_SCALE3(S_SUB3(S_MAX3(S_SPLAT3(smooth), f0), f0), far));
}

/* What sheen takes from all around, of what lights a surface: a fit by eye to the integral of Charlie's below. */
S_DEF S_F S_FN(SheenAround)(S_SELF S_F far, S_F rough) { return S_ADD(S_MUL(S_C(.14f), rough), S_MUL(S_SUB(S_C(.5f), S_MUL(S_C(.2f), rough)), far)); }

/* ---- the air ---- */

/* What plain fog leaves of what is this far away, times its density (air): 1 / (1 + air + air^2 / 2), which is e^-air
 * for little air, with no exp, whose bits differ from one C library to the next. */
S_DEF S_F S_FN(FogKeep)(S_SELF S_F air) { return S_DIV(S_C(1.f), S_ADD(S_ADD(S_C(1.f), air), S_MUL(S_MUL(air, air), S_C(.5f)))); }

/* ---- under the surface ---- */

/* Light under the surface wraps past where the surface turns from it: what that adds to n . l (towardLight: n . l as
 * it is; ndl: at least 0). */
S_DEF S_F S_FN(Wrap)(S_SELF S_F towardLight, S_F ndl) { return S_SUB(S_DIV(S_MAX(S_ADD(towardLight, S_C(.5f)), S_C(0.f)), S_C(1.5f)), ndl); }

#if !S_WIDE
/* ---- what the lanes work out one at a time ---- */

/* Sheen, Charlie's (Estevez and Kulla, 2017): its distribution, with pi put back as lights here have it taken out,
 * times Neubelt's visibility. inverse: 1 / its alpha. */
S_DEF S_F S_FN(Charlie)(S_SELF S_F ndh, S_F ndl, S_F ndv, S_F inverse) {
    const S_F sin2 = S_MAX(S_SUB(S_C(1.f), S_MUL(ndh, ndh)), S_C(1e-7f));
    const S_F d = S_MUL(S_MUL(S_ADD(S_C(2.f), inverse), S_EXP2(S_MUL(S_MUL(S_LOG2(sin2), S_C(.5f)), inverse))), S_C(.5f));
    return S_DIV(d, S_ADD(S_MUL(S_C(4.f), S_SUB(S_ADD(ndl, ndv), S_MUL(ndl, ndv))), S_C(1e-4f)));
}

/* What a thin film makes of what a surface reflects head on, seen at cosine (n . v): two beams, off its top and its
 * bottom, at red, green and blue's wavelengths, as much as amount says (KHR_materials_iridescence). */
S_DEF S_F S_FN(FilmChannel)(S_SELF S_F was, S_F r12, S_F ior, S_F phase, S_F amount) {
    const S_F root = S_SQRT(S_CLAMP(was, S_C(0.f), S_C(.99f))), under = S_DIV(S_ADD(S_C(1.f), root), S_SUB(S_C(1.f), root));
    const S_F r23 = S_DIV(S_SUB(ior, under), S_ADD(ior, under));
    const S_F twice = S_MUL(S_MUL(S_MUL(S_C(2.f), r12), r23), S_COS(phase));
    const S_F both = S_DIV(S_ADD(S_ADD(S_MUL(r12, r12), S_MUL(r23, r23)), twice), S_ADD(S_ADD(S_C(1.f), S_MUL(S_MUL(S_MUL(r12, r12), r23), r23)), twice));
    return S_ADD(was, S_MUL(S_SUB(both, was), amount));
}
S_DEF S_V3 S_FN(Iridescence)(S_SELF S_V3 f0, S_F cosine, S_F amount, S_F ior, S_F thickness) {
    const S_F sin2 = S_DIV(S_SUB(S_C(1.f), S_MUL(cosine, cosine)), S_MUL(ior, ior)), inside = S_SQRT(S_MAX(S_SUB(S_C(1.f), sin2), S_C(0.f)));
    const S_F r12 = S_DIV(S_SUB(S_C(1.f), ior), S_ADD(S_C(1.f), ior));
    /* (the path's difference, over a wavelength: 4 pi ior thickness cos inside / lambda) */
    const S_F phase = S_MUL(S_MUL(S_MUL(S_C(12.5663706f), ior), thickness), inside);
    const S_F r = S_FN(FilmChannel)(S_PASS S_X(f0), r12, ior, S_MUL(phase, S_C(1.f / 650.f)), amount);
    const S_F g = S_FN(FilmChannel)(S_PASS S_Y(f0), r12, ior, S_MUL(phase, S_C(1.f / 510.f)), amount);
    const S_F b = S_FN(FilmChannel)(S_PASS S_Z(f0), r12, ior, S_MUL(phase, S_C(1.f / 475.f)), amount);
    return S_V3_OF(r, g, b);
}

/* ---- light from a rectangle or a tube ----
 *
 * A light with a shape is, as the tracer has it, the mean of point lights over it: each as bright as a point light, its
 * own fade by its own distance, no falling off with the square of it. Over the shape's solid angle that is the lobe
 * times d^2 fade^2 / area, d the distance to each point of it.
 *
 * Diffuse: the shape by Gauss's three points each way, which the mean of point lights is smooth enough for (a
 * rectangle's nine, a tube's three, for which its width is 0).
 *
 * The highlight: the lobe's integral over the shape (Heitz, Dupuy, Hill and Neubelt, 2016: the lobe is a cosine put
 * through a matrix, M^-1 taking it back; src/shading/ltc.inc, fitted to this lobe by tools/ltc/fit.py), the polygon's
 * edges by acos as artfAcosFast's polynomial, and what is below the horizon taken off as from a sphere of the same
 * vector form factor (Snyder's, as Frostbite has it); times d^2 fade^2 / area at the point of the shape where the
 * lobe's own centroid points. A tube's highlight is a rectangle's as wide as the tube, turned to face the surface. */

/* acos(x) / sqrt(1 - x^2), by artfAcosFast's polynomial P: for x >= 0, acos is sqrt(1 - x) P(x), so the root goes. */
S_DEF S_F S_FN(AcosPoly)(S_SELF S_F a) { return S_ADD(S_C(1.5707288f), S_MUL(a, S_ADD(S_C(-.2121144f), S_MUL(a, S_SUB(S_C(.0742610f), S_MUL(a, S_C(.0187293f))))))); }
S_DEF S_F S_FN(ThetaOverSin)(S_SELF S_F x) {
    const S_F a = S_MIN(S_MAX(x, S_SUB(S_C(0.f), x)), S_C(1.f)), p = S_FN(AcosPoly)(S_PASS a);
    const S_F ahead = S_DIV(p, S_SQRT(S_ADD(S_C(1.f), a)));
    const S_F back = S_DIV(S_SUB(S_C(3.14159265f), S_MUL(S_SQRT(S_SUB(S_C(1.f), a)), p)), S_SQRT(S_MAX(S_SUB(S_C(1.f), S_MUL(x, x)), S_C(1e-7f))));
    return S_SELECT(S_GT(S_C(0.f), x), back, ahead);
}
/* acos, as artfAcosFast; and atan2 of y and x both at least 0, as artfAtan2Fast. */
S_DEF S_F S_FN(Acos)(S_SELF S_F x) {
    const S_F a = S_MIN(S_MAX(x, S_SUB(S_C(0.f), x)), S_C(1.f)), r = S_MUL(S_SQRT(S_SUB(S_C(1.f), a)), S_FN(AcosPoly)(S_PASS a));
    return S_SELECT(S_GT(S_C(0.f), x), S_SUB(S_C(3.14159265f), r), r);
}
S_DEF S_F S_FN(AtanUp)(S_SELF S_F y, S_F x) {
    const S_F big = S_MAX(x, y), small = S_MIN(x, y);
    const S_F z = S_DIV(small, S_MAX(big, S_C(1e-30f))), z2 = S_MUL(z, z);
    S_F p = S_SUB(S_C(.05265332f), S_MUL(z2, S_C(.01172120f)));
    p = S_ADD(S_C(-.11643287f), S_MUL(z2, p));
    p = S_ADD(S_C(.19354346f), S_MUL(z2, p));
    p = S_ADD(S_C(-.33262347f), S_MUL(z2, p));
    const S_F r = S_MUL(z, S_ADD(S_C(.99997726f), S_MUL(z2, p)));
    return S_SELECT(S_GT(y, x), S_SUB(S_C(1.57079633f), r), r);
}
S_DEF S_V3 S_FN(Cross)(S_SELF S_V3 a, S_V3 b) {
    return S_V3_OF(S_SUB(S_MUL(S_Y(a), S_Z(b)), S_MUL(S_Z(a), S_Y(b))), S_SUB(S_MUL(S_Z(a), S_X(b)), S_MUL(S_X(a), S_Z(b))), S_SUB(S_MUL(S_X(a), S_Y(b)), S_MUL(S_Y(a), S_X(b))));
}
#define S_SELECT3_(c, a, b) S_V3_OF(S_SELECT(c, S_X(a), S_X(b)), S_SELECT(c, S_Y(a), S_Y(b)), S_SELECT(c, S_Z(a), S_Z(b)))

/* A corner (from the surface) in the lobe's frame (t1, t2, n), taken back to the cosine by M^-1 = [[a, 0, b], [0, c, 0],
 * [d, 0, 1]], of length 1. */
S_DEF S_V3 S_FN(ToCosine)(S_SELF S_V3 q, S_V3 t1, S_V3 t2, S_V3 n, S_F a, S_F b, S_F c, S_F d) {
    const S_F x = S_DOT3(q, t1), y = S_DOT3(q, t2), z = S_DOT3(q, n);
    return S_NORMALIZE(S_V3_OF(S_ADD(S_MUL(a, x), S_MUL(b, z)), S_MUL(c, y), S_ADD(S_MUL(d, x), z)));
}
/* An edge's share of the vector form factor. */
S_DEF S_V3 S_FN(Edge)(S_SELF S_V3 v1, S_V3 v2) { return S_SCALE3(S_FN(Cross)(S_PASS v1, v2), S_FN(ThetaOverSin)(S_PASS S_DOT3(v1, v2))); }

/* Of a polygon of four corners (from the surface, its front toward it), what of the cosine through M it holds above
 * the horizon (a form factor: 0 to 1); and toward: which way the lobe's light from it comes, on the whole. */
S_DEF S_F S_FN(LtcQuad)(S_SELF S_V3 t1, S_V3 t2, S_V3 n, S_F a, S_F b, S_F c, S_F d, S_V3 q0, S_V3 q1, S_V3 q2, S_V3 q3, S_V3* toward) {
    const S_V3 v0 = S_FN(ToCosine)(S_PASS q0, t1, t2, n, a, b, c, d), v1 = S_FN(ToCosine)(S_PASS q1, t1, t2, n, a, b, c, d);
    const S_V3 v2 = S_FN(ToCosine)(S_PASS q2, t1, t2, n, a, b, c, d), v3 = S_FN(ToCosine)(S_PASS q3, t1, t2, n, a, b, c, d);
    S_V3 f = S_ADD3(S_ADD3(S_FN(Edge)(S_PASS v0, v1), S_FN(Edge)(S_PASS v1, v2)), S_ADD3(S_FN(Edge)(S_PASS v2, v3), S_FN(Edge)(S_PASS v3, v0)));
    f = S_SCALE3(f, S_C(-.159154943f)); /* (- 1 / 2 pi: the corners go round the other way, seen from the surface) */
    const S_F l = S_SQRT(S_DOT3(f, f));
    const S_F cosT = S_DIV(S_Z(f), S_MAX(l, S_C(1e-12f))), s2 = S_MIN(S_MAX(l, S_C(1e-9f)), S_C(.9999f));
    /* all above the horizon, or some below it */
    const S_F above = S_MUL(S_MAX(cosT, S_C(0.f)), s2);
    const S_F sinT = S_SQRT(S_MAX(S_SUB(S_C(1.f), S_MUL(cosT, cosT)), S_C(1e-9f)));
    const S_F x = S_SQRT(S_SUB(S_DIV(S_C(1.f), s2), S_C(1.f)));
    const S_F y = S_MIN(S_MAX(S_DIV(S_MUL(S_SUB(S_C(0.f), x), cosT), sinT), S_C(-1.f)), S_C(1.f));
    const S_F sy = S_MUL(sinT, S_SQRT(S_MAX(S_SUB(S_C(1.f), S_MUL(y, y)), S_C(0.f))));
    const S_F cut = S_MAX(S_MUL(S_ADD(S_MUL(S_SUB(S_MUL(cosT, S_FN(Acos)(S_PASS y)), S_MUL(x, sy)), s2), S_FN(AtanUp)(S_PASS sy, x)), S_C(.318309886f)), S_C(0.f));
    const S_F held = S_SELECT(S_GT(S_MUL(cosT, cosT), s2), above, cut);
    /* toward: f's way, through M (M^-1 inverted: its xz block's, and 1 / c), in the world */
    const S_F fx = S_X(f), fy = S_Y(f), fz = S_Z(f);
    const S_V3 back = S_V3_OF(S_SUB(fx, S_MUL(b, fz)), S_DIV(S_MUL(S_SUB(a, S_MUL(b, d)), fy), c), S_SUB(S_MUL(a, fz), S_MUL(d, fx)));
    *toward = S_NORMALIZE(S_ADD3(S_ADD3(S_SCALE3(t1, S_X(back)), S_SCALE3(t2, S_Y(back))), S_SCALE3(n, S_Z(back))));
    return S_SELECT(S_GT(l, S_C(1e-9f)), held, S_C(0.f));
}

/* A light with a shape, from a surface at p facing n, seen along view: d, its middle from p; u its way along and hl half
 * its length; f its way forward and hw half its width (0: a tube, its radius ball); reach2 1 / its radius^2.
 * ltc: the table's a, b, c, d and the lobe's integral at this roughness and n . v (artfLtcAt). Out: its diffuse light
 * (n . l in it, and its fade); its highlight, which the lobe's Fresnel and the surface's scale multiply (*toward: which
 * way it comes from, on the whole, for the Fresnel). Neither has the light's color, cone or profile. */
S_DEF void S_FN(AreaLight)(S_SELF S_V3 d, S_V3 u, S_F hl, S_V3 f, S_F hw, S_F ball, S_F reach2, S_V3 n, S_V3 view, S_F a, S_F b, S_F c, S_F dd, S_F total,
                           S_F* diffuse, S_F* highlight, S_V3* toward) {
    const S_FLAG flat = S_GT(hw, S_C(0.f));
    const S_V3 side = S_FN(Cross)(S_PASS f, u);
    /* diffuse: Gauss's points, at -sqrt(3/5), 0 and sqrt(3/5) of half the length and of half the width */
    static const float at[3] = {-.774596669f, 0.f, .774596669f}, weight[3] = {.277777778f, .444444444f, .277777778f};
    S_F sum = S_C(0.f);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const S_V3 q = S_ADD3(S_ADD3(d, S_SCALE3(u, S_MUL(hl, S_C(at[i])))), S_SCALE3(side, S_MUL(hw, S_C(at[j]))));
            const S_F q2 = S_MAX(S_DOT3(q, q), S_C(1e-8f));
            const S_V3 l = S_SCALE3(q, S_DIV(S_C(1.f), S_SQRT(q2)));
            const S_F share = S_SELECT(flat, S_MAX(S_SUB(S_C(0.f), S_DOT3(l, f)), S_C(0.f)), S_C(1.f));
            const S_F fade = S_MAX(S_SUB(S_C(1.f), S_MUL(q2, reach2)), S_C(0.f));
            sum = S_ADD(sum, S_MUL(S_C(weight[i] * weight[j]), S_MUL(S_MUL(share, S_MAX(S_DOT3(n, l), S_C(0.f))), S_MUL(fade, fade))));
        }
    *diffuse = sum;
    /* the highlight's polygon: the rectangle, or the tube as one turned to face p, as wide as it */
    const S_V3 out = S_SUB3(S_SCALE3(u, S_DOT3(d, u)), d); /* (from the tube's line to p) */
    const S_V3 faces = S_SELECT3_(flat, f, S_NORMALIZE(out));
    const S_V3 across = S_FN(Cross)(S_PASS faces, u);
    const S_F wide = S_SELECT(flat, hw, S_MAX(ball, S_MUL(hl, S_C(.02f))));
    const S_V3 along = S_SCALE3(u, hl), aside = S_SCALE3(across, wide);
    const S_V3 q0 = S_SUB3(S_SUB3(d, along), aside), q1 = S_SUB3(S_ADD3(d, along), aside), q2 = S_ADD3(S_ADD3(d, along), aside), q3 = S_ADD3(S_SUB3(d, along), aside);
    /* the lobe's frame: n, and v's way across it (any, where v is n) */
    const S_V3 flatView = S_SUB3(view, S_SCALE3(n, S_DOT3(n, view)));
    const S_V3 other = S_SELECT3_(S_GT(S_C(.9f), S_MAX(S_X(n), S_SUB(S_C(0.f), S_X(n)))), S_V3_OF(S_C(1.f), S_C(0.f), S_C(0.f)), S_V3_OF(S_C(0.f), S_C(1.f), S_C(0.f)));
    const S_V3 t1 = S_NORMALIZE(S_SELECT3_(S_GT(S_DOT3(flatView, flatView), S_C(1e-8f)), flatView, S_FN(Cross)(S_PASS n, other)));
    const S_V3 t2 = S_FN(Cross)(S_PASS n, t1);
    S_V3 way;
    const S_F held = S_MUL(total, S_FN(LtcQuad)(S_PASS t1, t2, n, a, b, c, dd, q0, q1, q2, q3, &way));
    /* where that way meets the shape (kept on it): its d^2 fade^2 / area; a tube's over its share there, as a
     * tube's points light all ways and the rectangle's share is in the integral */
    const S_F before = S_SUB(S_C(0.f), S_DOT3(d, faces));
    const S_F t = S_DIV(before, S_MAX(S_SUB(S_C(0.f), S_DOT3(way, faces)), S_C(1e-4f)));
    const S_V3 e = S_SUB3(S_SCALE3(way, t), d);
    const S_F ea = S_CLAMP(S_DOT3(e, u), S_SUB(S_C(0.f), hl), hl), eb = S_CLAMP(S_DOT3(e, across), S_SUB(S_C(0.f), wide), wide);
    const S_V3 hit = S_ADD3(S_ADD3(d, S_SCALE3(u, ea)), S_SCALE3(across, eb));
    const S_F h2 = S_MAX(S_DOT3(hit, hit), S_C(1e-8f)), hitFade = S_MAX(S_SUB(S_C(1.f), S_MUL(h2, reach2)), S_C(0.f));
    const S_F tubeShare = S_MAX(S_DIV(before, S_SQRT(h2)), S_C(1e-3f));
    const S_F per = S_DIV(S_MUL(h2, S_MUL(hitFade, hitFade)), S_MUL(S_MUL(S_C(4.f), hl), wide));
    *highlight = S_SELECT(S_GT(before, S_C(0.f)), S_DIV(S_MUL(held, per), S_SELECT(flat, S_C(1.f), tubeShare)), S_C(0.f));
    *toward = way;
}
#undef S_SELECT3_
#endif

/* (what a way defined goes with it: the next defines its own) */
#undef S_FN
#undef S_DEF
#undef S_SELF
#undef S_PASS
#undef S_F
#undef S_V3
#undef S_FLAG
#undef S_WIDE
#undef S_C
#undef S_ADD
#undef S_SUB
#undef S_MUL
#undef S_DIV
#undef S_MIN
#undef S_MAX
#undef S_SQRT
#undef S_GT
#undef S_SELECT
#undef S_PICK
#undef S_PICK3
#undef S_V3_OF
#undef S_X
#undef S_Y
#undef S_Z
#undef S_DOT3
#undef S_NORMALIZE
#undef S_CLAMP
#undef S_LOG2
#undef S_EXP2
#undef S_COS
#undef S_ADD3
#undef S_SUB3
#undef S_MUL3
#undef S_MAX3
#undef S_SCALE3
#undef S_SPLAT3
#undef S_OWN_V3_
