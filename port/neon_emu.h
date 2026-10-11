/*
 * neon_emu.h -- plain C versions of the NEON intrinsics used by gfx_sw.c.
 *
 * Only for building the renderer on a PC (the replay / benchmark tools), so the NEON
 * pixel pipeline can be checked pixel by pixel against the scalar one. The Miyoo build
 * uses the real <arm_neon.h>. Each function follows the ARM definition of the
 * instruction (saturation, rounding, lane order).
 */
#ifndef NEON_EMU_H
#define NEON_EMU_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#define NE_RANGE(n, lo, hi) do { if ((n) < (lo) || (n) > (hi)) abort(); } while (0)

typedef struct { uint8_t v[8]; } uint8x8_t;
typedef struct { int8_t v[8]; } int8x8_t;
typedef struct { uint8x8_t val[4]; } uint8x8x4_t;
typedef struct { uint16_t v[4]; } uint16x4_t;
typedef struct { int16_t v[4]; } int16x4_t;
typedef struct { uint16_t v[8]; } uint16x8_t;
typedef struct { int16_t v[8]; } int16x8_t;
typedef struct { uint32_t v[4]; } uint32x4_t;
typedef struct { int32_t v[4]; } int32x4_t;
typedef struct { uint64_t v[1]; } uint64x1_t;

#define NE_STATIC static inline

/* ---- dup / load / store ---- */
NE_STATIC uint8x8_t vdup_n_u8(uint8_t x) { uint8x8_t r; for (int i = 0; i < 8; i++) r.v[i] = x; return r; }
NE_STATIC uint16x4_t vdup_n_u16(uint16_t x) { uint16x4_t r; for (int i = 0; i < 4; i++) r.v[i] = x; return r; }
NE_STATIC uint16x8_t vdupq_n_u16(uint16_t x) { uint16x8_t r; for (int i = 0; i < 8; i++) r.v[i] = x; return r; }
NE_STATIC uint32x4_t vdupq_n_u32(uint32_t x) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = x; return r; }
NE_STATIC int32x4_t vdupq_n_s32(int32_t x) { int32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = x; return r; }
NE_STATIC uint32x4_t vld1q_u32(const uint32_t *p) { uint32x4_t r; memcpy(r.v, p, 16); return r; }
NE_STATIC int32x4_t vld1q_s32(const int32_t *p) { int32x4_t r; memcpy(r.v, p, 16); return r; }
NE_STATIC uint16x8_t vld1q_u16(const uint16_t *p) { uint16x8_t r; memcpy(r.v, p, 16); return r; }
NE_STATIC void vst1q_u32(uint32_t *p, uint32x4_t a) { memcpy(p, a.v, 16); }
NE_STATIC void vst1q_u16(uint16_t *p, uint16x8_t a) { memcpy(p, a.v, 16); }
NE_STATIC uint8x8x4_t vld4_u8(const uint8_t *p) {
    uint8x8x4_t r;
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) r.val[k].v[i] = p[i * 4 + k];
    return r;
}

/* ---- 32-bit lanes ---- */
NE_STATIC uint32x4_t vaddq_u32(uint32x4_t a, uint32x4_t b) { for (int i = 0; i < 4; i++) a.v[i] += b.v[i]; return a; }
NE_STATIC int32x4_t vaddq_s32(int32x4_t a, int32x4_t b) {
    for (int i = 0; i < 4; i++) a.v[i] = (int32_t) ((uint32_t) a.v[i] + (uint32_t) b.v[i]);
    return a;
}
NE_STATIC uint16x8_t vcltq_u16(uint16x8_t a, uint16x8_t b) { uint16x8_t r; for (int i = 0; i < 8; i++) r.v[i] = a.v[i] < b.v[i] ? 0xFFFF : 0; return r; }
NE_STATIC uint32x4_t vandq_u32(uint32x4_t a, uint32x4_t b) { for (int i = 0; i < 4; i++) a.v[i] &= b.v[i]; return a; }
NE_STATIC uint8x8_t vld1_u8(const uint8_t *p) { uint8x8_t r; memcpy(r.v, p, 8); return r; }
NE_STATIC int32x4_t vshrq_n_s32(int32x4_t a, int n) { NE_RANGE(n, 1, 32); for (int i = 0; i < 4; i++) a.v[i] >>= n; return a; }
NE_STATIC int32x4_t vandq_s32(int32x4_t a, int32x4_t b) { for (int i = 0; i < 4; i++) a.v[i] &= b.v[i]; return a; }
NE_STATIC int32x4_t vsubq_s32(int32x4_t a, int32x4_t b) { for (int i = 0; i < 4; i++) a.v[i] = (int32_t) ((uint32_t) a.v[i] - (uint32_t) b.v[i]); return a; }
NE_STATIC int32x4_t vmaxq_s32(int32x4_t a, int32x4_t b) { for (int i = 0; i < 4; i++) a.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return a; }
NE_STATIC int32x4_t vminq_s32(int32x4_t a, int32x4_t b) { for (int i = 0; i < 4; i++) a.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; return a; }
NE_STATIC int32x4_t vmulq_n_s32(int32x4_t a, int32_t b) { for (int i = 0; i < 4; i++) a.v[i] = (int32_t) ((uint32_t) a.v[i] * (uint32_t) b); return a; }
NE_STATIC int32x4_t vmlaq_n_s32(int32x4_t acc, int32x4_t a, int32_t b) { for (int i = 0; i < 4; i++) acc.v[i] = (int32_t) ((uint32_t) acc.v[i] + (uint32_t) a.v[i] * (uint32_t) b); return acc; }
NE_STATIC uint32x4_t vcgtq_s32(int32x4_t a, int32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] > b.v[i] ? 0xFFFFFFFFu : 0; return r; }
NE_STATIC int32x4_t vbslq_s32(uint32x4_t m, int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = (int32_t) ((m.v[i] & (uint32_t) a.v[i]) | (~m.v[i] & (uint32_t) b.v[i])); return r; }
NE_STATIC void vst1q_s32(int32_t *p, int32x4_t a) { memcpy(p, a.v, 16); }
typedef struct { float v[4]; } float32x4_t;
typedef struct { float v[2]; } float32x2_t;
typedef struct { int32_t v[2]; } int32x2_t;
typedef struct { uint32_t v[2]; } uint32x2_t;
NE_STATIC float32x4_t vdupq_n_f32(float x) { float32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = x; return r; }
/* VFP/NEON conversion: round towards zero, saturating, NaN -> 0 */
NE_STATIC uint32_t ne_f2u(float f) { if (!(f > 0.0f)) return 0; if (f >= 4294967296.0f) return 0xFFFFFFFFu; return (uint32_t) f; }
NE_STATIC int32_t ne_f2s(float f) { if (f != f) return 0; if (f >= 2147483648.0f) return 0x7FFFFFFF; if (f < -2147483648.0f) return (int32_t) 0x80000000u; return (int32_t) f; }
NE_STATIC uint32x4_t vcvtq_u32_f32(float32x4_t a) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = ne_f2u(a.v[i]); return r; }
NE_STATIC uint32x4_t vminq_u32(uint32x4_t a, uint32x4_t b) { for (int i = 0; i < 4; i++) a.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; return a; }
NE_STATIC float32x2_t vdup_n_f32(float x) { float32x2_t r; r.v[0] = r.v[1] = x; return r; }
NE_STATIC float32x2_t vset_lane_f32(float x, float32x2_t a, int n) { a.v[n] = x; return a; }
NE_STATIC int32x2_t vcvt_s32_f32(float32x2_t a) { int32x2_t r; for (int i = 0; i < 2; i++) r.v[i] = ne_f2s(a.v[i]); return r; }
NE_STATIC float32x2_t vcvt_f32_s32(int32x2_t a) { float32x2_t r; for (int i = 0; i < 2; i++) r.v[i] = (float) a.v[i]; return r; }
NE_STATIC uint32x2_t vcgt_f32(float32x2_t a, float32x2_t b) { uint32x2_t r; for (int i = 0; i < 2; i++) r.v[i] = a.v[i] > b.v[i] ? 0xFFFFFFFFu : 0; return r; }
NE_STATIC int32x2_t vsub_s32(int32x2_t a, int32x2_t b) { for (int i = 0; i < 2; i++) a.v[i] = (int32_t) ((uint32_t) a.v[i] - (uint32_t) b.v[i]); return a; }
NE_STATIC int32x2_t vreinterpret_s32_u32(uint32x2_t a) { int32x2_t r; memcpy(r.v, a.v, 8); return r; }
NE_STATIC int32x2_t vpmin_s32(int32x2_t a, int32x2_t b) { int32x2_t r; r.v[0] = a.v[0] < a.v[1] ? a.v[0] : a.v[1]; r.v[1] = b.v[0] < b.v[1] ? b.v[0] : b.v[1]; return r; }
NE_STATIC int32x2_t vpmax_s32(int32x2_t a, int32x2_t b) { int32x2_t r; r.v[0] = a.v[0] > a.v[1] ? a.v[0] : a.v[1]; r.v[1] = b.v[0] > b.v[1] ? b.v[0] : b.v[1]; return r; }
NE_STATIC int32_t vget_lane_s32(int32x2_t a, int n) { return a.v[n]; }
NE_STATIC uint32x4_t vmaxq_u32(uint32x4_t a, uint32x4_t b) { for (int i = 0; i < 4; i++) a.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return a; }
NE_STATIC uint32x2_t vpmax_u32(uint32x2_t a, uint32x2_t b) { uint32x2_t r; r.v[0] = a.v[0] > a.v[1] ? a.v[0] : a.v[1]; r.v[1] = b.v[0] > b.v[1] ? b.v[0] : b.v[1]; return r; }
NE_STATIC uint32_t vget_lane_u32(uint32x2_t a, int n) { return a.v[n]; }
NE_STATIC uint32_t vgetq_lane_u32(uint32x4_t a, int n) { return a.v[n]; }
NE_STATIC float32x4_t vfmaq_f32(float32x4_t a, float32x4_t b, float32x4_t c) { for (int i = 0; i < 4; i++) a.v[i] = fmaf(b.v[i], c.v[i], a.v[i]); return a; }
NE_STATIC void vst1q_f32(float *p, float32x4_t a) { memcpy(p, a.v, 16); }
NE_STATIC float32x4_t vld1q_f32(const float *p) { float32x4_t r; memcpy(r.v, p, 16); return r; }
NE_STATIC int32x2_t vmax_s32(int32x2_t a, int32x2_t b) { for (int i = 0; i < 2; i++) a.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return a; }
NE_STATIC int32x2_t vmin_s32(int32x2_t a, int32x2_t b) { for (int i = 0; i < 2; i++) a.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; return a; }
NE_STATIC int32x2_t vld1_s32(const int32_t *p) { int32x2_t r; memcpy(r.v, p, 8); return r; }
NE_STATIC float vget_lane_f32(float32x2_t a, int n) { return a.v[n]; }
NE_STATIC uint32x2_t vget_low_u32(uint32x4_t a) { uint32x2_t r; r.v[0] = a.v[0]; r.v[1] = a.v[1]; return r; }
NE_STATIC uint32x2_t vget_high_u32(uint32x4_t a) { uint32x2_t r; r.v[0] = a.v[2]; r.v[1] = a.v[3]; return r; }
NE_STATIC int16x4_t vshrn_n_s32(int32x4_t a, int n) { NE_RANGE(n, 1, 16); int16x4_t r; for (int i = 0; i < 4; i++) r.v[i] = (int16_t) (a.v[i] >> n); return r; }
NE_STATIC int16x8_t vcombine_s16(int16x4_t lo, int16x4_t hi) { int16x8_t r; memcpy(r.v, lo.v, 8); memcpy(r.v + 4, hi.v, 8); return r; }
NE_STATIC int16x8_t vdupq_n_s16(int16_t x) { int16x8_t r; for (int i = 0; i < 8; i++) r.v[i] = x; return r; }
NE_STATIC int16x8_t vandq_s16(int16x8_t a, int16x8_t b) { for (int i = 0; i < 8; i++) a.v[i] &= b.v[i]; return a; }
NE_STATIC int16x8_t vaddq_s16(int16x8_t a, int16x8_t b) { for (int i = 0; i < 8; i++) a.v[i] = (int16_t) (a.v[i] + b.v[i]); return a; }
NE_STATIC int16x8_t vsubq_s16(int16x8_t a, int16x8_t b) { for (int i = 0; i < 8; i++) a.v[i] = (int16_t) (a.v[i] - b.v[i]); return a; }
NE_STATIC int16x8_t vminq_s16(int16x8_t a, int16x8_t b) { for (int i = 0; i < 8; i++) a.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; return a; }
NE_STATIC int16x8_t vmaxq_s16(int16x8_t a, int16x8_t b) { for (int i = 0; i < 8; i++) a.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return a; }
NE_STATIC uint16x8_t vcgtq_s16(int16x8_t a, int16x8_t b) { uint16x8_t r; for (int i = 0; i < 8; i++) r.v[i] = a.v[i] > b.v[i] ? 0xFFFF : 0; return r; }
NE_STATIC uint16x8_t vmulq_n_u16(uint16x8_t a, uint16_t b) { for (int i = 0; i < 8; i++) a.v[i] = (uint16_t) (a.v[i] * b); return a; }
NE_STATIC uint16x8_t vorrq_u16(uint16x8_t a, uint16x8_t b) { for (int i = 0; i < 8; i++) a.v[i] |= b.v[i]; return a; }
NE_STATIC uint32x4_t vorrq_u32(uint32x4_t a, uint32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] | b.v[i]; return r; }
NE_STATIC uint32x4_t vcgeq_u32(uint32x4_t a, uint32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? 0xFFFFFFFFu : 0; return r; }
NE_STATIC uint32x4_t vbslq_u32(uint32x4_t m, uint32x4_t a, uint32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = (m.v[i] & a.v[i]) | (~m.v[i] & b.v[i]); return r; }
NE_STATIC uint16x4_t vmovn_u32(uint32x4_t a) { uint16x4_t r; for (int i = 0; i < 4; i++) r.v[i] = (uint16_t) a.v[i]; return r; }
NE_STATIC uint16x4_t vqshrun_n_s32(int32x4_t a, int n) { NE_RANGE(n, 1, 16);
    uint16x4_t r;
    for (int i = 0; i < 4; i++) { int32_t x = a.v[i] >> n; r.v[i] = (uint16_t) (x < 0 ? 0 : (x > 65535 ? 65535 : x)); }
    return r;
}
NE_STATIC uint16x4_t vshrn_n_u32(uint32x4_t a, int n) { NE_RANGE(n, 1, 16); uint16x4_t r; for (int i = 0; i < 4; i++) r.v[i] = (uint16_t) (a.v[i] >> n); return r; }
NE_STATIC uint32x4_t vmull_u16(uint16x4_t a, uint16x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = (uint32_t) a.v[i] * b.v[i]; return r; }
NE_STATIC int32x4_t vmovl_s16(int16x4_t a) { int32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i]; return r; }

/* ---- 16-bit lanes ---- */
NE_STATIC uint16x8_t vcombine_u16(uint16x4_t lo, uint16x4_t hi) { uint16x8_t r; memcpy(r.v, lo.v, 8); memcpy(r.v + 4, hi.v, 8); return r; }
NE_STATIC uint16x4_t vget_low_u16(uint16x8_t a) { uint16x4_t r; memcpy(r.v, a.v, 8); return r; }
NE_STATIC uint16x4_t vget_high_u16(uint16x8_t a) { uint16x4_t r; memcpy(r.v, a.v + 4, 8); return r; }
NE_STATIC uint16x8_t vaddq_u16(uint16x8_t a, uint16x8_t b) { for (int i = 0; i < 8; i++) a.v[i] = (uint16_t) (a.v[i] + b.v[i]); return a; }
NE_STATIC uint16x8_t vandq_u16(uint16x8_t a, uint16x8_t b) { for (int i = 0; i < 8; i++) a.v[i] &= b.v[i]; return a; }
NE_STATIC uint16x4_t vorr_u16(uint16x4_t a, uint16x4_t b) { for (int i = 0; i < 4; i++) a.v[i] |= b.v[i]; return a; }
NE_STATIC uint16x8_t vbslq_u16(uint16x8_t m, uint16x8_t a, uint16x8_t b) { uint16x8_t r; for (int i = 0; i < 8; i++) r.v[i] = (uint16_t) ((m.v[i] & a.v[i]) | (~m.v[i] & b.v[i])); return r; }
NE_STATIC uint16x8_t vshrq_n_u16(uint16x8_t a, int n) { NE_RANGE(n, 1, 16); for (int i = 0; i < 8; i++) a.v[i] >>= n; return a; }
NE_STATIC uint16x8_t vrshrq_n_u16(uint16x8_t a, int n) { NE_RANGE(n, 1, 16); for (int i = 0; i < 8; i++) a.v[i] = (uint16_t) (((uint32_t) a.v[i] + (1u << (n - 1))) >> n); return a; }
NE_STATIC uint8x8_t vrshrn_n_u16(uint16x8_t a, int n) { NE_RANGE(n, 1, 8); uint8x8_t r; for (int i = 0; i < 8; i++) r.v[i] = (uint8_t) (((uint32_t) a.v[i] + (1u << (n - 1))) >> n); return r; }
NE_STATIC uint8x8_t vshrn_n_u16(uint16x8_t a, int n) { NE_RANGE(n, 1, 8); uint8x8_t r; for (int i = 0; i < 8; i++) r.v[i] = (uint8_t) (a.v[i] >> n); return r; }
NE_STATIC uint8x8_t vmovn_u16(uint16x8_t a) { uint8x8_t r; for (int i = 0; i < 8; i++) r.v[i] = (uint8_t) a.v[i]; return r; }
NE_STATIC uint8x8_t vqmovn_u16(uint16x8_t a) { uint8x8_t r; for (int i = 0; i < 8; i++) r.v[i] = (uint8_t) (a.v[i] > 255 ? 255 : a.v[i]); return r; }
NE_STATIC uint16x8_t vsriq_n_u16(uint16x8_t a, uint16x8_t b, int n) { NE_RANGE(n, 1, 16);
    uint16_t keep = (uint16_t) ~(0xFFFFu >> n);
    for (int i = 0; i < 8; i++) a.v[i] = (uint16_t) ((a.v[i] & keep) | (b.v[i] >> n));
    return a;
}
NE_STATIC int16x4_t vreinterpret_s16_u16(uint16x4_t a) { int16x4_t r; memcpy(r.v, a.v, 8); return r; }
NE_STATIC uint16x8_t vreinterpretq_u16_s16(int16x8_t a) { uint16x8_t r; memcpy(r.v, a.v, 16); return r; }
NE_STATIC uint32x4_t vreinterpretq_u32_s32(int32x4_t a) { uint32x4_t r; memcpy(r.v, a.v, 16); return r; }
NE_STATIC uint64x1_t vreinterpret_u64_u16(uint16x4_t a) { uint64x1_t r; memcpy(r.v, a.v, 8); return r; }
NE_STATIC uint8x8x4_t vld4_dup_u8(const uint8_t *p) { uint8x8x4_t r; for (int k = 0; k < 4; k++) for (int i = 0; i < 8; i++) r.val[k].v[i] = p[k]; return r; }
NE_STATIC uint8x8_t vbsl_u8(uint8x8_t m, uint8x8_t a, uint8x8_t b) { uint8x8_t r; for (int i = 0; i < 8; i++) r.v[i] = (uint8_t) ((m.v[i] & a.v[i]) | (~m.v[i] & b.v[i])); return r; }
NE_STATIC uint64_t vget_lane_u64(uint64x1_t a, int n) { (void) n; return a.v[0]; }

/* ---- 8-bit lanes ---- */
NE_STATIC uint16x8_t vmull_u8(uint8x8_t a, uint8x8_t b) { uint16x8_t r; for (int i = 0; i < 8; i++) r.v[i] = (uint16_t) (a.v[i] * b.v[i]); return r; }
NE_STATIC uint16x8_t vmlal_u8(uint16x8_t acc, uint8x8_t a, uint8x8_t b) { for (int i = 0; i < 8; i++) acc.v[i] = (uint16_t) (acc.v[i] + a.v[i] * b.v[i]); return acc; }
NE_STATIC uint16x8_t vshll_n_u8(uint8x8_t a, int n) { NE_RANGE(n, 0, 8); uint16x8_t r; for (int i = 0; i < 8; i++) r.v[i] = (uint16_t) (a.v[i] << n); return r; }
NE_STATIC uint8x8_t vmvn_u8(uint8x8_t a) { for (int i = 0; i < 8; i++) a.v[i] = (uint8_t) ~a.v[i]; return a; }
NE_STATIC uint8x8_t vadd_u8(uint8x8_t a, uint8x8_t b) { for (int i = 0; i < 8; i++) a.v[i] = (uint8_t) (a.v[i] + b.v[i]); return a; }
NE_STATIC uint8x8_t vsub_u8(uint8x8_t a, uint8x8_t b) { for (int i = 0; i < 8; i++) a.v[i] = (uint8_t) (a.v[i] - b.v[i]); return a; }
NE_STATIC uint8x8_t vqadd_u8(uint8x8_t a, uint8x8_t b) { for (int i = 0; i < 8; i++) { int s = a.v[i] + b.v[i]; a.v[i] = (uint8_t) (s > 255 ? 255 : s); } return a; }
NE_STATIC uint8x8_t vqsub_u8(uint8x8_t a, uint8x8_t b) { for (int i = 0; i < 8; i++) { int s = a.v[i] - b.v[i]; a.v[i] = (uint8_t) (s < 0 ? 0 : s); } return a; }
NE_STATIC uint8x8_t vshr_n_u8(uint8x8_t a, int n) { NE_RANGE(n, 1, 8); for (int i = 0; i < 8; i++) a.v[i] >>= n; return a; }
NE_STATIC uint8x8_t vshl_n_u8(uint8x8_t a, int n) { NE_RANGE(n, 0, 7); for (int i = 0; i < 8; i++) a.v[i] = (uint8_t) (a.v[i] << n); return a; }
NE_STATIC uint8x8_t vcgt_u8(uint8x8_t a, uint8x8_t b) { for (int i = 0; i < 8; i++) a.v[i] = a.v[i] > b.v[i] ? 0xFF : 0; return a; }
NE_STATIC int8x8_t vreinterpret_s8_u8(uint8x8_t a) { int8x8_t r; memcpy(r.v, a.v, 8); return r; }
NE_STATIC int16x8_t vmovl_s8(int8x8_t a) { int16x8_t r; for (int i = 0; i < 8; i++) r.v[i] = a.v[i]; return r; }

/* ---- used by video_miyoo.c ---- */
typedef struct { uint8x8_t val[2]; } uint8x8x2_t;
NE_STATIC uint16x8_t vrev64q_u16(uint16x8_t a) { uint16x8_t r; for (int i = 0; i < 8; i++) r.v[i] = a.v[(i & 4) + 3 - (i & 3)]; return r; }
NE_STATIC uint16x8_t vshlq_n_u16(uint16x8_t a, int n) { NE_RANGE(n, 0, 15); for (int i = 0; i < 8; i++) a.v[i] = (uint16_t) (a.v[i] << n); return a; }
NE_STATIC uint8x8_t vsri_n_u8(uint8x8_t a, uint8x8_t b, int n) {
    NE_RANGE(n, 1, 8);
    uint8_t keep = (uint8_t) ~(0xFFu >> n);
    for (int i = 0; i < 8; i++) a.v[i] = (uint8_t) ((a.v[i] & keep) | (b.v[i] >> n));
    return a;
}
NE_STATIC uint8x8x2_t vzip_u8(uint8x8_t a, uint8x8_t b) {
    uint8x8x2_t r;
    for (int i = 0; i < 8; i++) { r.val[i / 4].v[(i % 4) * 2] = a.v[i]; r.val[i / 4].v[(i % 4) * 2 + 1] = b.v[i]; }
    return r;
}
NE_STATIC void vst4_u8(uint8_t *p, uint8x8x4_t a) {
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) p[i * 4 + k] = a.val[k].v[i];
}

#endif
