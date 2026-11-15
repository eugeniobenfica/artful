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
