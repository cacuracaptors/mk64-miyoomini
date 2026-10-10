/*
 * sh4zam.h -- portable C replacement for the Dreamcast-only include/sh4zam.h
 * (Miyoo Mini port). Same API, same results as the SH4 versions:
 *
 *  - XMTRX (the SH4's hidden 4x4 matrix register) becomes a per-thread
 *    float[16] in column-major order: shz_xmtrx_[col*4 + row], exactly the
 *    order the SH4 code loads it from memory (XF0..XF15).
 *  - ftrv / fipr become plain float multiply-adds.
 *  - fsca angles keep the SH4 rule: radians * 10430.37835f, truncated to an
 *    int, low 16 bits = fraction of a full turn.
 *  - fsrra (fast 1/sqrt) becomes an exact 1.0f / sqrtf().
 *
 * Two routines are buggy in the Dreamcast copy (registers used before they
 * are loaded); here they do what they were written to do, which is also what
 * the original SH4ZAM library does:
 *  - shz_xmtrx_apply_rotation_z     (XMTRX = XMTRX * Rz)
 *  - shz_xmtrx_apply_3x3_transpose  (XMTRX = XMTRX * transpose(m))
 *
 * This file is put in front of include/ on the include path, so the
 * game's #include "sh4zam.h" picks it up without touching the game code.
 */
#ifndef SH4ZAM_H
#define SH4ZAM_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#define SHZ_NOEXCEPT
#define SHZ_ALIGNAS(n)          __attribute__((aligned(n)))
#define SHZ_INLINE              inline static
#define SHZ_FORCE_INLINE        __attribute__((always_inline)) SHZ_INLINE
#define SHZ_NO_INLINE           __attribute__((noinline))
#define SHZ_ALIASING            __attribute__((__may_alias__))

#define SHZ_FSCA_RAD_FACTOR     10430.37835f

#define TRIG_ARG_SCALE 0.00009587f
#define SHZ_ANGLE(a) (((float)((uint16_t)a)) * TRIG_ARG_SCALE)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shz_sincos {
    float sin;
    float cos;
} shz_sincos_t;

typedef struct shz_vec2 {
    union {
        struct {
            float x;
            float y;
        };
        float e[2];
    };
} shz_vec2_t;

typedef struct shz_vec3 {
    union {
        struct {
            union {
                struct {
                    float x;
                    float y;
                };
                shz_vec2_t vec2;
            };
            float z;
        };
        float e[3];
    };
} shz_vec3_t;

typedef struct shz_vec4 {
    union {
        struct {
            union {
                struct {
                    union {
                        struct {
                            float x;
                            float y;
                        };
                        shz_vec2_t vec2;
                    };
                    float z;
                };
                shz_vec3_t vec3;
            };
            float w;
        };
        float e[4];
    };
} shz_vec4_t;

typedef SHZ_ALIGNAS(8) union shz_matrix_2x2 {
    float       elem[4];
    float       elem2D[2][2];
    shz_vec2_t  col[2];
} shz_matrix_2x2_t;

typedef union shz_matrix_3x3 {
    float      elem[9];
    float      elem2D[3][3];
    shz_vec3_t col[3];
    struct {
        shz_vec3_t left;
        shz_vec3_t up;
        shz_vec3_t forward;
    };
} shz_matrix_3x3_t;

typedef union shz_matrix_3x4 {
    float      elem[12];
    float      elem2D[3][4];
    shz_vec3_t col[4];
    struct {
        shz_vec3_t left;
        shz_vec3_t up;
        shz_vec3_t forward;
        shz_vec3_t pos;
    };
} shz_matrix_3x4_t;

typedef SHZ_ALIGNAS(8) union shz_matrix_4x4 {
    float      elem[16];
    float      elem2D[4][4];
    shz_vec4_t col[4];
    struct {
        shz_vec4_t left;
        shz_vec4_t up;
        shz_vec4_t forward;
        shz_vec4_t pos;
    };
} shz_matrix_4x4_t;

/* the emulated XMTRX register, defined in port/sh4zam_state.c */
extern __thread float shz_xmtrx_[16];
#define SHZ_X (shz_xmtrx_)

/* ---------------------------------------------------------------- scalar */

SHZ_FORCE_INLINE float shz_mag_sqr4f(float x, float y, float z, float w) {
    return x * x + y * y + z * z + w * w;
}

SHZ_FORCE_INLINE float shz_dot8f(float x1, float y1, float z1, float w1,
                                 float x2, float y2, float z2, float w2) {
    return x1 * x2 + y1 * y2 + z1 * z2 + w1 * w2;
}

/* 1.0f/sqrtf(x) (fsrra on the SH4) */
SHZ_FORCE_INLINE float shz_inverse_sqrtf(float x) {
    return 1.0f / sqrtf(x);
}

/* 1/|x| (fsrra(x*x) on the SH4: always positive) */
SHZ_FORCE_INLINE float shz_inverse_posf(float x) {
    return 1.0f / fabsf(x);
}

SHZ_FORCE_INLINE float shz_div_posf(float num, float denom) {
    return num * shz_inverse_posf(denom);
}

SHZ_FORCE_INLINE float shz_fast_invf(float x) {
    return 1.0f / x;
}

SHZ_FORCE_INLINE shz_vec3_t shz_vec3_scale(shz_vec3_t vec, float factor) SHZ_NOEXCEPT {
    return (shz_vec3_t) { .x = vec.x * factor, .y = vec.y * factor, .z = vec.z * factor };
}

SHZ_FORCE_INLINE float shz_vec3_magnitude_sqr(shz_vec3_t vec) SHZ_NOEXCEPT {
    return shz_mag_sqr4f(vec.x, vec.y, vec.z, 0.0f);
}

SHZ_FORCE_INLINE float shz_vec3_magnitude_inv(shz_vec3_t vec) SHZ_NOEXCEPT {
    return shz_inverse_sqrtf(shz_vec3_magnitude_sqr(vec));
}

SHZ_FORCE_INLINE shz_vec3_t shz_vec3_normalize(shz_vec3_t vec) SHZ_NOEXCEPT {
    return shz_vec3_scale(vec, shz_vec3_magnitude_inv(vec));
}

/* ------------------------------------------------------------ sin / cos */

/* fsca: 0x10000 = full turn */
SHZ_FORCE_INLINE shz_sincos_t shz_sincosu16(uint16_t radians16) {
    float a = (float)radians16 * (6.28318530717958647692f / 65536.0f);
    return (shz_sincos_t){ sinf(a), cosf(a) };
}

/* radians -> fsca units, truncated like ftrc (never through a negative
   float -> unsigned conversion, which saturates to 0 on ARM) */
SHZ_FORCE_INLINE uint16_t shz_fsca_units_(float radians) {
    return (uint16_t)(int32_t)(radians * SHZ_FSCA_RAD_FACTOR);
}

SHZ_FORCE_INLINE shz_sincos_t shz_sincosf(float radians) {
    return shz_sincosu16(shz_fsca_units_(radians));
}

SHZ_FORCE_INLINE float shz_sincos_tanf(shz_sincos_t sincos) {
    return sincos.sin / sincos.cos;
}

SHZ_FORCE_INLINE float shz_sinf(float radians) {
    return shz_sincosf(radians).sin;
}

SHZ_FORCE_INLINE float shz_cosf(float radians) {
    return shz_sincosf(radians).cos;
}

SHZ_FORCE_INLINE float shz_tanf(float radians) {
    return shz_sincos_tanf(shz_sincosf(radians));
}

/* movca.l: allocates a cache line without reading it; nothing to do here */
SHZ_FORCE_INLINE void shz_dcache_alloc_line(void *src) {
    (void)src;
}

/* --------------------------------------------------------- XMTRX * vector */

SHZ_FORCE_INLINE shz_vec4_t shz_xmtrx_trans_vec4(shz_vec4_t v) {
    const float *m = SHZ_X;
    shz_vec4_t r;
    r.x = m[0] * v.x + m[4] * v.y + m[8]  * v.z + m[12] * v.w;
    r.y = m[1] * v.x + m[5] * v.y + m[9]  * v.z + m[13] * v.w;
    r.z = m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w;
    r.w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w;
    return r;
}

SHZ_FORCE_INLINE shz_vec3_t shz_xmtrx_trans_vec3(shz_vec3_t vec) {
    /* w = 0, like the SH4 version's compound literal (unnamed members = 0) */
    shz_vec4_t v4;
    v4.x = vec.x; v4.y = vec.y; v4.z = vec.z; v4.w = 0.0f;
    return shz_xmtrx_trans_vec4(v4).vec3;
}

SHZ_FORCE_INLINE shz_vec2_t shz_xmtrx_trans_vec2(shz_vec2_t vec) {
    shz_vec3_t v3;
    v3.x = vec.x; v3.y = vec.y; v3.z = 0.0f;
    return shz_xmtrx_trans_vec3(v3).vec2;
}

/* ---------------------------------------------------------- load / store */

SHZ_INLINE void shz_xmtrx_store_4x4(shz_matrix_4x4_t *matrix) {
    memcpy(matrix->elem, SHZ_X, 16 * sizeof(float));
}

SHZ_INLINE void shz_xmtrx_store_4x4_unaligned(float matrix[16]) {
    memcpy(matrix, SHZ_X, 16 * sizeof(float));
}

SHZ_INLINE void shz_xmtrx_store_4x4_transpose(shz_matrix_4x4_t *matrix) {
    int r, c;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++)
            matrix->elem[r * 4 + c] = SHZ_X[c * 4 + r];
}

SHZ_INLINE void shz_xmtrx_load_4x4(const shz_matrix_4x4_t *matrix) {
    memcpy(SHZ_X, matrix->elem, 16 * sizeof(float));
}

SHZ_INLINE void shz_xmtrx_load_4x4_unaligned(const float matrix[16]) {
    memcpy(SHZ_X, matrix, 16 * sizeof(float));
}

SHZ_INLINE void shz_xmtrx_load_4x4_transpose(const shz_matrix_4x4_t *matrix) {
    int r, c;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++)
            SHZ_X[c * 4 + r] = matrix->elem[r * 4 + c];
}

/* out = a * b, column-major 4x4 (out may alias neither input) */
SHZ_FORCE_INLINE void shz_mul4x4_(float *out, const float *a, const float *b) {
    int c;
    for (c = 0; c < 4; c++) {
        const float bx = b[c * 4 + 0], by = b[c * 4 + 1], bz = b[c * 4 + 2], bw = b[c * 4 + 3];
        out[c * 4 + 0] = a[0] * bx + a[4] * by + a[8]  * bz + a[12] * bw;
        out[c * 4 + 1] = a[1] * bx + a[5] * by + a[9]  * bz + a[13] * bw;
        out[c * 4 + 2] = a[2] * bx + a[6] * by + a[10] * bz + a[14] * bw;
        out[c * 4 + 3] = a[3] * bx + a[7] * by + a[11] * bz + a[15] * bw;
    }
}

/* XMTRX = matrix1 * matrix2 */
SHZ_INLINE void shz_xmtrx_load_4x4_apply(const shz_matrix_4x4_t *matrix1,
                                         const shz_matrix_4x4_t *matrix2) {
    float tmp[16];
    shz_mul4x4_(tmp, matrix1->elem, matrix2->elem);
    memcpy(SHZ_X, tmp, sizeof(tmp));
}

/* out = matrix1 * matrix2; XMTRX = matrix1 afterwards */
SHZ_INLINE void shz_xmtrx_load_4x4_apply_store(shz_matrix_4x4_t *out,
                                               const shz_matrix_4x4_t *matrix1,
                                               const shz_matrix_4x4_t *matrix2) {
    float tmp[16];
    shz_mul4x4_(tmp, matrix1->elem, matrix2->elem);
    memcpy(SHZ_X, matrix1->elem, 16 * sizeof(float));
    memcpy(out->elem, tmp, sizeof(tmp));
}

/* plain copy; the SH4 version leaves XF0..XF7 = src[8..15] behind */
SHZ_INLINE void shz_matrix_4x4_copy(shz_matrix_4x4_t *dst, const shz_matrix_4x4_t *src) {
    float tail[8];
    memcpy(tail, &src->elem[8], sizeof(tail));
    memmove(dst->elem, src->elem, 16 * sizeof(float));
    memcpy(SHZ_X, tail, sizeof(tail));
}

SHZ_INLINE void shz_xmtrx_load_3x3(const shz_matrix_3x3_t *matrix) {
    const float *m = matrix->elem;
    float *x = SHZ_X;
    x[0] = m[0]; x[1] = m[1]; x[2]  = m[2]; x[3]  = 0.0f;
    x[4] = m[3]; x[5] = m[4]; x[6]  = m[5]; x[7]  = 0.0f;
    x[8] = m[6]; x[9] = m[7]; x[10] = m[8]; x[11] = 0.0f;
    x[12] = 0.0f; x[13] = 0.0f; x[14] = 0.0f; x[15] = 1.0f;
}

SHZ_INLINE void shz_xmtrx_load_3x3_transpose(const float *m) {
    float *x = SHZ_X;
    x[0] = m[0]; x[4] = m[1]; x[8]  = m[2];
    x[1] = m[3]; x[5] = m[4]; x[9]  = m[5];
    x[2] = m[6]; x[6] = m[7]; x[10] = m[8];
    x[3] = 0.0f; x[7] = 0.0f; x[11] = 0.0f;
    x[12] = 0.0f; x[13] = 0.0f; x[14] = 0.0f; x[15] = 1.0f;
}

SHZ_INLINE void shz_xmtrx_store_3x3(shz_matrix_3x3_t *matrix) {
    float *m = matrix->elem;
    const float *x = SHZ_X;
    m[0] = x[0]; m[1] = x[1]; m[2] = x[2];
    m[3] = x[4]; m[4] = x[5]; m[5] = x[6];
    m[6] = x[8]; m[7] = x[9]; m[8] = x[10];
}

SHZ_INLINE void shz_xmtrx_store_3x3_transpose(shz_matrix_3x3_t *matrix) {
    float *m = matrix->elem;
    const float *x = SHZ_X;
    m[0] = x[0]; m[1] = x[4]; m[2] = x[8];
    m[3] = x[1]; m[4] = x[5]; m[5] = x[9];
    m[6] = x[2]; m[7] = x[6]; m[8] = x[10];
}

/* XMTRX = XMTRX * E, E = m as 3x3 (column-major), column 3 kept */
SHZ_INLINE void shz_xmtrx_apply_3x3(const shz_matrix_3x3_t *matrix) {
    const float *m = matrix->elem;
    float e[16] = {
        m[0], m[1], m[2], 0.0f,
        m[3], m[4], m[5], 0.0f,
        m[6], m[7], m[8], 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    float tmp[16];
    shz_mul4x4_(tmp, SHZ_X, e);
    memcpy(SHZ_X, tmp, sizeof(tmp));
}

SHZ_INLINE void shz_xmtrx_apply_3x3_transpose(const shz_matrix_3x3_t *matrix) {
    const float *m = matrix->elem;
    float e[16] = {
        m[0], m[3], m[6], 0.0f,
        m[1], m[4], m[7], 0.0f,
        m[2], m[5], m[8], 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    float tmp[16];
    shz_mul4x4_(tmp, SHZ_X, e);
    memcpy(SHZ_X, tmp, sizeof(tmp));
}

/* XMTRX = XMTRX * matrix */
SHZ_INLINE void shz_xmtrx_apply_4x4_unaligned(const float matrix[16]) {
    float tmp[16];
    shz_mul4x4_(tmp, SHZ_X, matrix);
    memcpy(SHZ_X, tmp, sizeof(tmp));
}

/* ------------------------------------------------------------- builders */

SHZ_INLINE void shz_xmtrx_init_diagonal(float x, float y, float z, float w) {
    float *m = SHZ_X;
    memset(m, 0, 16 * sizeof(float));
    m[0] = x; m[5] = y; m[10] = z; m[15] = w;
}

SHZ_INLINE void shz_xmtrx_init_identity(void) {
    shz_xmtrx_init_diagonal(1.0f, 1.0f, 1.0f, 1.0f);
}

SHZ_INLINE void shz_xmtrx_init_translation(float x, float y, float z) {
    shz_xmtrx_init_identity();
    SHZ_X[12] = x; SHZ_X[13] = y; SHZ_X[14] = z;
}

SHZ_FORCE_INLINE void shz_xmtrx_init_scale(float x, float y, float z) {
    shz_xmtrx_init_diagonal(x, y, z, 1.0f);
}

SHZ_INLINE void shz_xmtrx_init_rotation_x(float x) {
    shz_sincos_t sc = shz_sincosf(x);
    float *m = SHZ_X;
    shz_xmtrx_init_identity();
    m[5] = sc.cos;  m[6]  = sc.sin;
    m[9] = -sc.sin; m[10] = sc.cos;
}

SHZ_INLINE void shz_xmtrx_init_rotation_y(float y) {
    shz_sincos_t sc = shz_sincosf(y);
    float *m = SHZ_X;
    shz_xmtrx_init_identity();
    m[0] = sc.cos; m[2]  = -sc.sin;
    m[8] = sc.sin; m[10] = sc.cos;
}

SHZ_INLINE void shz_xmtrx_init_rotation_z(float z) {
    shz_sincos_t sc = shz_sincosf(z);
    float *m = SHZ_X;
    shz_xmtrx_init_identity();
    m[0] = sc.cos;  m[1] = sc.sin;
    m[4] = -sc.sin; m[5] = sc.cos;
}

/* new column a = XMTRX * (column vector u), new column b = XMTRX * v */
SHZ_FORCE_INLINE void shz_xmtrx_set_cols_(int a, const float u[4], int b, const float v[4]) {
    shz_vec4_t ua, vb;
    ua.x = u[0]; ua.y = u[1]; ua.z = u[2]; ua.w = u[3];
    vb.x = v[0]; vb.y = v[1]; vb.z = v[2]; vb.w = v[3];
    ua = shz_xmtrx_trans_vec4(ua);
    vb = shz_xmtrx_trans_vec4(vb);
    memcpy(&SHZ_X[a * 4], ua.e, 4 * sizeof(float));
    memcpy(&SHZ_X[b * 4], vb.e, 4 * sizeof(float));
}

SHZ_INLINE void shz_xmtrx_apply_rotation_x(float x) {
    shz_sincos_t sc = shz_sincosf(x);
    const float c1[4] = { 0.0f, sc.cos, sc.sin, 0.0f };
    const float c2[4] = { 0.0f, -sc.sin, sc.cos, 0.0f };
    shz_xmtrx_set_cols_(1, c1, 2, c2);
}

SHZ_INLINE void shz_xmtrx_apply_rotation_y(float y) {
    shz_sincos_t sc = shz_sincosf(y);
    const float c0[4] = { sc.cos, 0.0f, -sc.sin, 0.0f };
    const float c2[4] = { sc.sin, 0.0f, sc.cos, 0.0f };
    shz_xmtrx_set_cols_(0, c0, 2, c2);
}

SHZ_INLINE void shz_xmtrx_apply_rotation_z(float z) {
    shz_sincos_t sc = shz_sincosf(z);
    const float c0[4] = { sc.cos, sc.sin, 0.0f, 0.0f };
    const float c1[4] = { -sc.sin, sc.cos, 0.0f, 0.0f };
    shz_xmtrx_set_cols_(0, c0, 1, c1);
}

SHZ_FORCE_INLINE void shz_xmtrx_set_translation(float x, float y, float z) {
    SHZ_X[12] = x; SHZ_X[13] = y; SHZ_X[14] = z;
}

/* adds to the translation column directly (not XMTRX * T) */
SHZ_INLINE void shz_xmtrx_apply_translation(float x, float y, float z) {
    SHZ_X[12] += x; SHZ_X[13] += y; SHZ_X[14] += z;
}

/* XMTRX = XMTRX * S (x/y/z of columns 0..2 scaled) */
SHZ_INLINE void shz_xmtrx_apply_scale(float x, float y, float z) {
    float *m = SHZ_X;
    m[0] *= x; m[1] *= x; m[2]  *= x;
    m[4] *= y; m[5] *= y; m[6]  *= y;
    m[8] *= z; m[9] *= z; m[10] *= z;
}

SHZ_INLINE void shz_xmtrx_init_rotation(float roll, float pitch, float yaw) {
    shz_xmtrx_init_rotation_z(yaw);
    shz_xmtrx_apply_rotation_y(pitch);
    shz_xmtrx_apply_rotation_x(roll);
}

SHZ_INLINE void shz_xmtrx_apply_rotation(float roll, float pitch, float yaw) {
    shz_xmtrx_apply_rotation_x(roll);
    shz_xmtrx_apply_rotation_y(pitch);
    shz_xmtrx_apply_rotation_z(yaw);
}

SHZ_INLINE void shz_xmtrx_transpose(void) {
    float t[16];
    int r, c;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++)
            t[r * 4 + c] = SHZ_X[c * 4 + r];
    memcpy(SHZ_X, t, sizeof(t));
}

/* ------------------------------------------------- matrix * vec3 (no w) */

SHZ_INLINE shz_vec3_t shz_matrix4x4_trans_vec3(const shz_matrix_4x4_t *m, shz_vec3_t v) {
    shz_vec3_t out;
    out.x = v.x * m->elem2D[0][0] + v.y * m->elem2D[1][0] + v.z * m->elem2D[2][0];
    out.y = v.x * m->elem2D[0][1] + v.y * m->elem2D[1][1] + v.z * m->elem2D[2][1];
    out.z = v.x * m->elem2D[0][2] + v.y * m->elem2D[1][2] + v.z * m->elem2D[2][2];
    return out;
}

SHZ_INLINE shz_vec3_t shz_matrix3x3_trans_vec3(const shz_matrix_3x3_t *m, shz_vec3_t v) {
    shz_vec3_t out;
    out.x = v.x * m->elem2D[0][0] + v.y * m->elem2D[1][0] + v.z * m->elem2D[2][0];
    out.y = v.x * m->elem2D[0][1] + v.y * m->elem2D[1][1] + v.z * m->elem2D[2][1];
    out.z = v.x * m->elem2D[0][2] + v.y * m->elem2D[1][2] + v.z * m->elem2D[2][2];
    return out;
}

SHZ_INLINE shz_vec3_t shz_matrix4x4_trans_vec3_transpose(const shz_matrix_4x4_t *m, shz_vec3_t v) {
    shz_vec3_t out;
    out.x = v.x * m->elem2D[0][0] + v.y * m->elem2D[0][1] + v.z * m->elem2D[0][2];
    out.y = v.x * m->elem2D[1][0] + v.y * m->elem2D[1][1] + v.z * m->elem2D[1][2];
    out.z = v.x * m->elem2D[2][0] + v.y * m->elem2D[2][1] + v.z * m->elem2D[2][2];
    return out;
}

SHZ_INLINE shz_vec3_t shz_matrix3x3_trans_vec3_transpose(const shz_matrix_3x3_t *m, shz_vec3_t v) {
    shz_vec3_t out;
    out.x = v.x * m->elem2D[0][0] + v.y * m->elem2D[0][1] + v.z * m->elem2D[0][2];
    out.y = v.x * m->elem2D[1][0] + v.y * m->elem2D[1][1] + v.z * m->elem2D[1][2];
    out.z = v.x * m->elem2D[2][0] + v.y * m->elem2D[2][1] + v.z * m->elem2D[2][2];
    return out;
}

SHZ_INLINE void shz_memcpy4_16(void *dst, const void *src) {
    memcpy(dst, src, 64);
}

#undef SHZ_X

#ifdef __cplusplus
}
#endif

#endif /* SH4ZAM_H */
