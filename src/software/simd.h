/* artful — eight lanes at a time. The kernels are written once on these operations, and built once for
 * each instruction set: with AVX2 (each operation one instruction), with SSE4.1 or NEON (two registers of
 * four lanes), and portable (loops over eight lanes, which the compiler makes what it can of). Each operation gives the
 * same bits in all of them, so they draw the same pixels: no fused multiply-adds, no approximate
 * reciprocals, and max and min as x86 defines them. */
#ifndef ARTFUL_SIMD_H
#define ARTFUL_SIMD_H

#include <math.h>
#include <stdint.h>

#ifdef _MSC_VER
#include <string.h>
#define V8_INLINE static __forceinline
#define __builtin_memcpy memcpy
#else
#define V8_INLINE static inline __attribute__((always_inline))
#endif

#if defined(ARTF_KERNEL_SSE41) || defined(ARTF_KERNEL_NEON)

/* Eight lanes as two registers of four. */

#ifdef ARTF_KERNEL_SSE41
#include <smmintrin.h>
typedef __m128 V4F;
typedef __m128i V4I;
#define v4fSet _mm_set1_ps
#define v4fLoad _mm_loadu_ps
#define v4fStore _mm_storeu_ps
#define v4fAdd _mm_add_ps
#define v4fSub _mm_sub_ps
#define v4fMul _mm_mul_ps
#define v4fDiv _mm_div_ps
#define v4fMin _mm_min_ps
#define v4fMax _mm_max_ps
#define v4fSqrt _mm_sqrt_ps
#define v4fFloor _mm_floor_ps
#define v4fAbs(a) _mm_andnot_ps(_mm_set1_ps(-0.f), a)
#define v4fGt(a, b) _mm_castps_si128(_mm_cmpgt_ps(a, b))
#define v4fLt(a, b) _mm_castps_si128(_mm_cmplt_ps(a, b))
#define v4fToI _mm_cvttps_epi32
#define v4fSelect(m, a, b) _mm_blendv_ps(a, b, _mm_castsi128_ps(m))
#define v4iSet _mm_set1_epi32
#define v4iLoad(p) _mm_loadu_si128((const __m128i*)(p))
#define v4iStore(p, a) _mm_storeu_si128((__m128i*)(p), a)
#define v4iAdd _mm_add_epi32
#define v4iSub _mm_sub_epi32
#define v4iMul _mm_mullo_epi32
#define v4iAnd _mm_and_si128
#define v4iOr _mm_or_si128
#define v4iXor _mm_xor_si128
#define v4iAndNot _mm_andnot_si128
#define v4iShl _mm_slli_epi32
#define v4iShr _mm_srli_epi32
#define v4iEq _mm_cmpeq_epi32
#define v4iGt _mm_cmpgt_epi32
#define v4iSelect(m, a, b) _mm_blendv_epi8(a, b, m)
#define v4iToF _mm_cvtepi32_ps
#define v4iAsF _mm_castsi128_ps
#define v4iSigns(a) _mm_movemask_ps(_mm_castsi128_ps(a))
#else
#include <arm_neon.h>
typedef float32x4_t V4F;
typedef int32x4_t V4I;
#define v4fSet vdupq_n_f32
#define v4fLoad vld1q_f32
#define v4fStore vst1q_f32
#define v4fAdd vaddq_f32
#define v4fSub vsubq_f32
#define v4fMul vmulq_f32
#define v4fDiv vdivq_f32
/* (as x86 does: with a NaN, the second; so not vminq and vmaxq) */
V8_INLINE V4F v4fMin(V4F a, V4F b) { return vbslq_f32(vcltq_f32(a, b), a, b); }
V8_INLINE V4F v4fMax(V4F a, V4F b) { return vbslq_f32(vcgtq_f32(a, b), a, b); }
#define v4fSqrt vsqrtq_f32
#define v4fFloor vrndmq_f32
#define v4fAbs vabsq_f32
#define v4fGt(a, b) vreinterpretq_s32_u32(vcgtq_f32(a, b))
#define v4fLt(a, b) vreinterpretq_s32_u32(vcltq_f32(a, b))
#define v4fToI vcvtq_s32_f32
/* (by its top bit, as on x86: masks here are whole lanes) */
#define v4fSelect(m, a, b) vbslq_f32(vreinterpretq_u32_s32(vshrq_n_s32(m, 31)), b, a)
#define v4iSet vdupq_n_s32
#define v4iLoad(p) vld1q_s32((const int32_t*)(p))
#define v4iStore(p, a) vst1q_s32((int32_t*)(p), a)
#define v4iAdd vaddq_s32
#define v4iSub vsubq_s32
#define v4iMul vmulq_s32
#define v4iAnd vandq_s32
#define v4iOr vorrq_s32
#define v4iXor veorq_s32
#define v4iAndNot(a, b) vbicq_s32(b, a)
#define v4iShl vshlq_n_s32
/* (by a register: the immediate form cannot shift by 0) */
#define v4iShr(a, n) vreinterpretq_s32_u32(vshlq_u32(vreinterpretq_u32_s32(a), vdupq_n_s32(-(n))))
#define v4iEq(a, b) vreinterpretq_s32_u32(vceqq_s32(a, b))
#define v4iGt(a, b) vreinterpretq_s32_u32(vcgtq_s32(a, b))
#define v4iSelect(m, a, b) vbslq_s32(vreinterpretq_u32_s32(vshrq_n_s32(m, 31)), b, a)
#define v4iToF vcvtq_f32_s32
#define v4iAsF vreinterpretq_f32_s32
V8_INLINE int v4iSigns(V4I a) {
    static const int32_t bits[4] = {1, 2, 4, 8};
    return vaddvq_s32(vandq_s32(vshrq_n_s32(a, 31), vld1q_s32(bits)));
}
#endif

typedef struct V8F { V4F a, b; } V8F;
typedef struct V8I { V4I a, b; } V8I;

#define V8F_OP(name, op) V8_INLINE V8F name(V8F x, V8F y) { V8F r = {op(x.a, y.a), op(x.b, y.b)}; return r; }
#define V8F_OP1(name, op) V8_INLINE V8F name(V8F x) { V8F r = {op(x.a), op(x.b)}; return r; }
#define V8I_OP(name, op) V8_INLINE V8I name(V8I x, V8I y) { V8I r = {op(x.a, y.a), op(x.b, y.b)}; return r; }

V8_INLINE V8F v8fSet(float s) { V8F r = {v4fSet(s), v4fSet(s)}; return r; }
V8_INLINE V8F v8fLoad(const float* p) { V8F r = {v4fLoad(p), v4fLoad(p + 4)}; return r; }
V8_INLINE void v8fStore(float* p, V8F x) { v4fStore(p, x.a); v4fStore(p + 4, x.b); }
V8F_OP(v8fAdd, v4fAdd)
V8F_OP(v8fSub, v4fSub)
V8F_OP(v8fMul, v4fMul)
V8F_OP(v8fDiv, v4fDiv)
V8F_OP(v8fMin, v4fMin)
V8F_OP(v8fMax, v4fMax)
V8F_OP1(v8fSqrt, v4fSqrt)
V8F_OP1(v8fFloor, v4fFloor)
V8F_OP1(v8fAbs, v4fAbs)
V8_INLINE V8I v8fGt(V8F x, V8F y) { V8I r = {v4fGt(x.a, y.a), v4fGt(x.b, y.b)}; return r; }
V8_INLINE V8I v8fLt(V8F x, V8F y) { V8I r = {v4fLt(x.a, y.a), v4fLt(x.b, y.b)}; return r; }
V8_INLINE V8I v8fToI(V8F x) { V8I r = {v4fToI(x.a), v4fToI(x.b)}; return r; }
V8_INLINE V8F v8fSelect(V8I m, V8F x, V8F y) { V8F r = {v4fSelect(m.a, x.a, y.a), v4fSelect(m.b, x.b, y.b)}; return r; }

V8_INLINE V8I v8iSet(int32_t s) { V8I r = {v4iSet(s), v4iSet(s)}; return r; }
V8_INLINE V8I v8iLoadU(const void* p) { V8I r = {v4iLoad(p), v4iLoad((const char*)p + 16)}; return r; }
V8_INLINE V8I v8iLoad(const void* p) { return v8iLoadU(p); }
V8_INLINE V8I v8iIota(void) { static const int32_t lanes[8] = {0, 1, 2, 3, 4, 5, 6, 7}; return v8iLoadU(lanes); }
V8_INLINE void v8iStoreU(void* p, V8I x) { v4iStore(p, x.a); v4iStore((char*)p + 16, x.b); }
V8_INLINE void v8iStore(void* p, V8I x) { v8iStoreU(p, x); }
V8I_OP(v8iAdd, v4iAdd)
V8I_OP(v8iSub, v4iSub)
V8I_OP(v8iMul, v4iMul)
V8I_OP(v8iAnd, v4iAnd)
V8I_OP(v8iOr, v4iOr)
V8I_OP(v8iXor, v4iXor)
V8I_OP(v8iAndNot, v4iAndNot)
V8I_OP(v8iEq, v4iEq)
V8I_OP(v8iGt, v4iGt)
#define v8iShl(x, n) ((V8I){v4iShl((x).a, n), v4iShl((x).b, n)})
#define v8iShr(x, n) ((V8I){v4iShr((x).a, n), v4iShr((x).b, n)})
V8_INLINE V8I v8iSelect(V8I m, V8I x, V8I y) { V8I r = {v4iSelect(m.a, x.a, y.a), v4iSelect(m.b, x.b, y.b)}; return r; }
V8_INLINE V8F v8iToF(V8I x) { V8F r = {v4iToF(x.a), v4iToF(x.b)}; return r; }
V8_INLINE V8F v8iAsF(V8I x) { V8F r = {v4iAsF(x.a), v4iAsF(x.b)}; return r; }
V8_INLINE V8I v8iGather(const uint32_t* base, V8I index) {
    _Alignas(16) int32_t k[8];
    _Alignas(16) uint32_t out[8];
    v8iStoreU(k, index);
    for (int i = 0; i < 8; ++i) out[i] = base[k[i]];
    return v8iLoadU(out);
}
V8_INLINE int v8iSigns(V8I x) { return v4iSigns(x.a) | v4iSigns(x.b) << 4; }

#elif defined(ARTF_KERNEL_AVX2)

#include <immintrin.h>

typedef __m256 V8F;
typedef __m256i V8I;

V8_INLINE V8F v8fSet(float a) { return _mm256_set1_ps(a); }
V8_INLINE V8F v8fLoad(const float* p) { return _mm256_load_ps(p); }
V8_INLINE void v8fStore(float* p, V8F a) { _mm256_store_ps(p, a); }
V8_INLINE V8F v8fAdd(V8F a, V8F b) { return _mm256_add_ps(a, b); }
V8_INLINE V8F v8fSub(V8F a, V8F b) { return _mm256_sub_ps(a, b); }
V8_INLINE V8F v8fMul(V8F a, V8F b) { return _mm256_mul_ps(a, b); }
V8_INLINE V8F v8fDiv(V8F a, V8F b) { return _mm256_div_ps(a, b); }
V8_INLINE V8F v8fMin(V8F a, V8F b) { return _mm256_min_ps(a, b); }
V8_INLINE V8F v8fMax(V8F a, V8F b) { return _mm256_max_ps(a, b); }
V8_INLINE V8F v8fSqrt(V8F a) { return _mm256_sqrt_ps(a); }
V8_INLINE V8F v8fFloor(V8F a) { return _mm256_floor_ps(a); }
V8_INLINE V8F v8fAbs(V8F a) { return _mm256_andnot_ps(_mm256_set1_ps(-0.f), a); }
V8_INLINE V8I v8fGt(V8F a, V8F b) { return _mm256_castps_si256(_mm256_cmp_ps(a, b, _CMP_GT_OQ)); }
V8_INLINE V8I v8fLt(V8F a, V8F b) { return _mm256_castps_si256(_mm256_cmp_ps(a, b, _CMP_LT_OQ)); }
V8_INLINE V8I v8fToI(V8F a) { return _mm256_cvttps_epi32(a); }
/* mask ? b : a */
V8_INLINE V8F v8fSelect(V8I mask, V8F a, V8F b) { return _mm256_blendv_ps(a, b, _mm256_castsi256_ps(mask)); }

V8_INLINE V8I v8iSet(int32_t a) { return _mm256_set1_epi32(a); }
V8_INLINE V8I v8iIota(void) { return _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7); }
V8_INLINE V8I v8iLoad(const void* p) { return _mm256_load_si256((const __m256i*)p); }
V8_INLINE V8I v8iLoadU(const void* p) { return _mm256_loadu_si256((const __m256i*)p); }
V8_INLINE void v8iStore(void* p, V8I a) { _mm256_store_si256((__m256i*)p, a); }
V8_INLINE void v8iStoreU(void* p, V8I a) { _mm256_storeu_si256((__m256i*)p, a); }
V8_INLINE V8I v8iAdd(V8I a, V8I b) { return _mm256_add_epi32(a, b); }
V8_INLINE V8I v8iSub(V8I a, V8I b) { return _mm256_sub_epi32(a, b); }
V8_INLINE V8I v8iMul(V8I a, V8I b) { return _mm256_mullo_epi32(a, b); }
V8_INLINE V8I v8iAnd(V8I a, V8I b) { return _mm256_and_si256(a, b); }
V8_INLINE V8I v8iOr(V8I a, V8I b) { return _mm256_or_si256(a, b); }
V8_INLINE V8I v8iXor(V8I a, V8I b) { return _mm256_xor_si256(a, b); }
V8_INLINE V8I v8iAndNot(V8I a, V8I b) { return _mm256_andnot_si256(a, b); } /* ~a & b */
#define v8iShl(a, n) _mm256_slli_epi32(a, n)
#define v8iShr(a, n) _mm256_srli_epi32(a, n)
V8_INLINE V8I v8iEq(V8I a, V8I b) { return _mm256_cmpeq_epi32(a, b); }
V8_INLINE V8I v8iGt(V8I a, V8I b) { return _mm256_cmpgt_epi32(a, b); }
V8_INLINE V8I v8iSelect(V8I mask, V8I a, V8I b) { return _mm256_blendv_epi8(a, b, mask); }
V8_INLINE V8F v8iToF(V8I a) { return _mm256_cvtepi32_ps(a); }
V8_INLINE V8F v8iAsF(V8I a) { return _mm256_castsi256_ps(a); }
V8_INLINE V8I v8iGather(const uint32_t* base, V8I index) { return _mm256_i32gather_epi32((const int*)base, index, 4); }
/* a bit for each lane whose top bit is set */
V8_INLINE int v8iSigns(V8I a) { return _mm256_movemask_ps(_mm256_castsi256_ps(a)); }

#else

typedef struct V8F { _Alignas(32) float v[8]; } V8F;
typedef struct V8I { _Alignas(32) int32_t v[8]; } V8I;

#define V8_EACH for (int i = 0; i < 8; ++i)
#define V8F_OP(name, expr) V8_INLINE V8F name(V8F a, V8F b) { V8F r; V8_EACH r.v[i] = expr; return r; }
#define V8I_OP(name, expr) V8_INLINE V8I name(V8I a, V8I b) { V8I r; V8_EACH r.v[i] = expr; return r; }

V8_INLINE V8F v8fSet(float a) { V8F r; V8_EACH r.v[i] = a; return r; }
V8_INLINE V8F v8fLoad(const float* p) { V8F r; V8_EACH r.v[i] = p[i]; return r; }
V8_INLINE void v8fStore(float* p, V8F a) { V8_EACH p[i] = a.v[i]; }
V8F_OP(v8fAdd, a.v[i] + b.v[i])
V8F_OP(v8fSub, a.v[i] - b.v[i])
V8F_OP(v8fMul, a.v[i] * b.v[i])
V8F_OP(v8fDiv, a.v[i] / b.v[i])
V8F_OP(v8fMin, a.v[i] < b.v[i] ? a.v[i] : b.v[i])
V8F_OP(v8fMax, a.v[i] > b.v[i] ? a.v[i] : b.v[i])
V8_INLINE V8F v8fSqrt(V8F a) { V8F r; V8_EACH r.v[i] = sqrtf(a.v[i]); return r; }
V8_INLINE V8F v8fFloor(V8F a) { V8F r; V8_EACH r.v[i] = floorf(a.v[i]); return r; }
V8_INLINE V8F v8fAbs(V8F a) { V8F r; V8_EACH r.v[i] = fabsf(a.v[i]); return r; }
V8_INLINE V8I v8fGt(V8F a, V8F b) { V8I r; V8_EACH r.v[i] = a.v[i] > b.v[i] ? -1 : 0; return r; }
V8_INLINE V8I v8fLt(V8F a, V8F b) { V8I r; V8_EACH r.v[i] = a.v[i] < b.v[i] ? -1 : 0; return r; }
V8_INLINE V8I v8fToI(V8F a) { V8I r; V8_EACH r.v[i] = (int32_t)a.v[i]; return r; }
V8_INLINE V8F v8fSelect(V8I m, V8F a, V8F b) { V8F r; V8_EACH r.v[i] = m.v[i] < 0 ? b.v[i] : a.v[i]; return r; }

V8_INLINE V8I v8iSet(int32_t a) { V8I r; V8_EACH r.v[i] = a; return r; }
V8_INLINE V8I v8iIota(void) { V8I r; V8_EACH r.v[i] = i; return r; }
V8_INLINE V8I v8iLoadU(const void* p) { V8I r; __builtin_memcpy(r.v, p, 32); return r; }
V8_INLINE V8I v8iLoad(const void* p) { return v8iLoadU(p); }
V8_INLINE void v8iStoreU(void* p, V8I a) { __builtin_memcpy(p, a.v, 32); }
V8_INLINE void v8iStore(void* p, V8I a) { v8iStoreU(p, a); }
V8I_OP(v8iAdd, (int32_t)((uint32_t)a.v[i] + (uint32_t)b.v[i]))
V8I_OP(v8iSub, (int32_t)((uint32_t)a.v[i] - (uint32_t)b.v[i]))
V8I_OP(v8iMul, (int32_t)((uint32_t)a.v[i] * (uint32_t)b.v[i]))
V8I_OP(v8iAnd, a.v[i] & b.v[i])
V8I_OP(v8iOr, a.v[i] | b.v[i])
V8I_OP(v8iXor, a.v[i] ^ b.v[i])
V8I_OP(v8iAndNot, ~a.v[i] & b.v[i])
V8_INLINE V8I v8iShl(V8I a, int n) { V8I r; V8_EACH r.v[i] = (int32_t)((uint32_t)a.v[i] << n); return r; }
V8_INLINE V8I v8iShr(V8I a, int n) { V8I r; V8_EACH r.v[i] = (int32_t)((uint32_t)a.v[i] >> n); return r; }
V8I_OP(v8iEq, a.v[i] == b.v[i] ? -1 : 0)
V8I_OP(v8iGt, a.v[i] > b.v[i] ? -1 : 0)
V8_INLINE V8I v8iSelect(V8I m, V8I a, V8I b) { V8I r; V8_EACH r.v[i] = m.v[i] < 0 ? b.v[i] : a.v[i]; return r; }
V8_INLINE V8F v8iToF(V8I a) { V8F r; V8_EACH r.v[i] = (float)a.v[i]; return r; }
V8_INLINE V8F v8iAsF(V8I a) { V8F r; __builtin_memcpy(r.v, a.v, 32); return r; }
V8_INLINE V8I v8iGather(const uint32_t* base, V8I index) { V8I r; V8_EACH r.v[i] = (int32_t)base[index.v[i]]; return r; }
V8_INLINE int v8iSigns(V8I a) { int r = 0; V8_EACH r |= (a.v[i] < 0) << i; return r; }

#endif

#endif
