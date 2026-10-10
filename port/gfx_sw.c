/*
 * gfx_sw.c -- software rasterizer backend for the MK64 Fast3D front-end
 * (src/gfx/gfx_retro_dc.c), replacing the Dreamcast PowerVR backend gfx_pvr.c.
 *
 * It exports the same symbols as gfx_pvr.c (gfx_pvr_api, pvr_reserve,
 * pvr_submit_op, gfx_pvr_set_*, ...) and reproduces what the PVR did with
 * the vertices the front-end bakes:
 *   - screen x,y in 640x480 pixels (scaled here to the render buffer size),
 *     z = 1/w (larger = nearer; constant for 2D / depth-off geometry),
 *   - texture u,v normalised over the texture, interpolated perspective
 *     correctly through z (affine when z is constant, like the PVR),
 *   - per-vertex colour (ARGB) and offset colour (oargb; alpha = fog density),
 *   - texenv REPLACE / MODULATE / DECAL / MODULATEALPHA, offset added,
 *     vertex fog towards the fog colour,
 *   - three lists: OP drawn immediately, PT (alpha-test cutouts) and TR
 *     (alpha blend) deferred and drawn at end_frame, PT first,
 *   - depth test GEQUAL (or always), depth write on/off; TR never writes z.
 * Textures are kept as ARGB8888 at their real size (no power-of-two padding,
 * so the u/v scale factors are 1).
 */
#include <PR/gbi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <kos.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "gfx/gfx_cc.h"
#include "gfx/gfx_rendering_api.h"
#include "macros.h"
#include "gfx/gl_fast_vert.h"
#include "port_miyoo.h"

/* ------------------------------------------------------------------------- */
/* shaders (only the combiner feature flags matter here)                      */
/* ------------------------------------------------------------------------- */

struct ShaderProgram {
    uint32_t shader_id;
    struct CCFeatures cc;
    uint8_t texture_used[2];
    int num_inputs;
};

static struct ShaderProgram shader_pool[128];
static int shader_pool_size;
static struct ShaderProgram *cur_shader;

/* ------------------------------------------------------------------------- */
/* textures                                                                   */
/* ------------------------------------------------------------------------- */

#define TEX_MAX 1024

struct SwTex {
    uint32_t *px;          /* ARGB8888, w*h */
    uint32_t alloc;        /* texels allocated */
    uint16_t w, h;
    uint8_t filter;        /* 1 = bilinear requested (point sampled for now) */
    uint8_t clampu, clampv, mirru, mirrv;
};

static struct SwTex sTex[TEX_MAX];
static uint32_t sTexCount;
static uint32_t sBound[2];
static uint32_t sCurBound;
static int sCacheFlushPending;

/* ------------------------------------------------------------------------- */
/* render state                                                               */
/* ------------------------------------------------------------------------- */

enum { KIND_OP = 0, KIND_PT = 1, KIND_TR = 2 };

typedef struct {
    uint32_t tex;          /* 0 = untextured */
    uint8_t texenv;
    uint8_t depth_test, depth_write;
    uint8_t fog;
    uint8_t blend_src, blend_dst;
    uint8_t clampu, clampv, mirru, mirrv;
    uint8_t filter;        /* bilinear requested: sampled at the nearest texel centre */
} RState;

static uint8_t sDepthTest = 1, sDepthWrite = 1, sFog = 0, sTexEnv = GFX_TEXENV_MODULATE;
static uint8_t sBlendSrc = GFX_BLENDF_SRCALPHA, sBlendDst = GFX_BLENDF_INVSRCALPHA;
static uint8_t sListKind = KIND_OP;
static uint32_t sFogColor = 0;   /* 0x00RRGGBB */

/* exported for the front-end (only used by its DC inline OP path) */
uint8_t gfx_pvr_op_dirty = 1;

#define BUCKET_VERTS 32768
#define BUCKET_BATCH 2048

typedef struct {
    RState st;
    uint32_t start, count;
} Batch;

typedef struct {
    dc_fast_t *verts;
    Batch *batch;
    uint32_t nverts;
    int nbatch, cur;
    uint8_t dirty;
} Bucket;

static dc_fast_t sOpVerts[BUCKET_VERTS], sPtVerts[BUCKET_VERTS], sTrVerts[BUCKET_VERTS];
static Batch sOpBatch[BUCKET_BATCH], sPtBatch[BUCKET_BATCH], sTrBatch[BUCKET_BATCH];
/* All three lists are kept for the end of the frame, as on the PVR (OP, then PT, then TR),
   and then drawn by both cores at once, each on alternating bands of 8 rows. */
static Bucket sOP = { sOpVerts, sOpBatch, 0, 0, -1, 1 };
static Bucket sPT = { sPtVerts, sPtBatch, 0, 0, -1, 1 };
static Bucket sTR = { sTrVerts, sTrBatch, 0, 0, -1, 1 };
static int sDropLog;

static void mark_dirty(void) {
    sOP.dirty = 1;
    sPT.dirty = 1;
    sTR.dirty = 1;
    gfx_pvr_op_dirty = 1;
}

static void current_state(RState *s) {
    uint32_t t = sBound[0];
    int textured = cur_shader && cur_shader->texture_used[0] && t && t < TEX_MAX && sTex[t].px;
    s->tex = textured ? t : 0;
    s->texenv = sTexEnv;
    s->depth_test = sDepthTest;
    s->depth_write = sDepthWrite;
    s->fog = sFog;
    s->blend_src = sBlendSrc;
    s->blend_dst = sBlendDst;
    if (textured) {
        s->clampu = sTex[t].clampu;
        s->filter = sTex[t].filter;
        s->clampv = sTex[t].clampv;
        s->mirru = sTex[t].mirru;
        s->mirrv = sTex[t].mirrv;
    } else {
        s->clampu = s->clampv = s->mirru = s->mirrv = 0;
        s->filter = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* statistics                                                                 */
/* ------------------------------------------------------------------------- */

static uint32_t stTris, stPixels, stTexUp, stOpTris, stPtTris, stTrTris;
static __thread uint32_t tlPixels, tlTris;   /* per raster thread, summed after each frame */
static double stRasterMs, stWalkMs, stCore0Ms, stCore1Ms;

void gfx_sw_perf_report(int frames, double ms) {
    (void) ms;
    if (frames <= 0)
        return;
    fprintf(stderr, "perf gfx: tris %u (op %u pt %u tr %u)  pixels %u  tex uploads %u  per frame | walk %.2f  raster %.2f (core0 %.2f core1 %.2f) ms/frame  %dx%d\n",
            stTris / frames, stOpTris / frames, stPtTris / frames, stTrTris / frames, stPixels / frames,
            stTexUp / frames, stWalkMs / frames, stRasterMs / frames, stCore0Ms / frames, stCore1Ms / frames,
            port_render_w, port_render_h);
    stTris = stPixels = stTexUp = stOpTris = stPtTris = stTrTris = 0;
    stRasterMs = stWalkMs = stCore0Ms = stCore1Ms = 0;
}

/* ------------------------------------------------------------------------- */
/* the rasterizer                                                             */
/* ------------------------------------------------------------------------- */

/* attribute slots */
enum { A_Z, A_U, A_V, A_R, A_G, A_B, A_A, A_OR, A_OG, A_OB, A_F, NATTR };

static inline int iclamp255(float v) {
    int i = (int) v;
    return i < 0 ? 0 : (i > 255 ? 255 : i);
}

static inline int wrap_coord(int i, int n, int clamp, int mirror) {
    if (clamp) {
        return i < 0 ? 0 : (i >= n ? n - 1 : i);
    }
    if (mirror) {
        int p = n * 2;
        int m = i % p;
        if (m < 0)
            m += p;
        return m < n ? m : p - 1 - m;
    }
    if ((n & (n - 1)) == 0)
        return i & (n - 1);
    int m = i % n;
    return m < 0 ? m + n : m;
}

static inline int ifloor(float f) {
    int i = (int) f;
    return (f < (float) i) ? i - 1 : i;
}

/* ---- per-triangle constants for the span loops ---- */
enum { WR_REPEAT2, WR_CLAMP, WR_MIRROR2, WR_REPEATN, WR_MIRRORN };

typedef struct {
    const uint32_t *tpx;
    int tw, th, umask, vmask, uwrap, vwrap;
    float ftw, fth;
    float bias;              /* -0.5 texel for filtered textures (see raster_tri) */
    int texenv, dtest, dwrite, fog, persp;
    int fr, fg, fb, bs, bd;
    int r, g, b, a;          /* flat colour (GOUR == 0) */
} TriCtx;

static uint8_t sExp5[32], sExp6[64];   /* 565 -> 8 bit, same rounding as before (v*255/max) */

static inline int tc_wrap(int i, int n, int mask, int mode) {
    switch (mode) {
        case WR_REPEAT2: return i & mask;
        case WR_CLAMP: return i < 0 ? 0 : (i >= n ? n - 1 : i);
        case WR_MIRROR2: { int m = i & (2 * n - 1); return m < n ? m : 2 * n - 1 - m; }
        case WR_REPEATN: return wrap_coord(i, n, 0, 0);
        default: return wrap_coord(i, n, 0, 1);
    }
}

static inline int clamp8(int v) {
    if ((unsigned) v > 255u)
        v = v < 0 ? 0 : 255;
    return v;
}

#define DIV255(x) (((x) + 128 + (((x) + 128) >> 8)) >> 8)

/* keeps a texel coordinate (in texels) in a range where 16.16 fixed point can't overflow */
static inline float tc_fold(float u, int n, int mode) {
    if (mode == WR_CLAMP)
        return u < -64.0f ? -64.0f : (u > (float) n + 64.0f ? (float) n + 64.0f : u);
    float period = (mode == WR_MIRROR2 || mode == WR_MIRRORN) ? 2.0f * n : (float) n;
    if (u >= 0.0f && u < 16384.0f && (mode == WR_REPEAT2 || mode == WR_MIRROR2))
        return u;   /* pow2 wrap only looks at the low bits: no folding needed */
    return u - floorf(u / period) * period;
}

/* ---- the pixel pipeline, shared by all span loops (inlined, constants fold away) ---- */

static inline __attribute__((always_inline)) void tex_combine(const int env, uint32_t tc, int *r, int *g, int *b, int *al) {
    int tr = (tc >> 16) & 0xFF, tg = (tc >> 8) & 0xFF, tb = tc & 0xFF, ta = tc >> 24;
    switch (env) {
        case GFX_TEXENV_REPLACE:
            *r = tr; *g = tg; *b = tb; *al = ta;
            break;
        case GFX_TEXENV_MODULATE:
            *r = DIV255(*r * tr); *g = DIV255(*g * tg); *b = DIV255(*b * tb); *al = ta;
            break;
        case GFX_TEXENV_DECAL:
            *r = DIV255(*r * (255 - ta) + tr * ta);
            *g = DIV255(*g * (255 - ta) + tg * ta);
            *b = DIV255(*b * (255 - ta) + tb * ta);
            break;
        default: /* MODULATEALPHA */
            *r = DIV255(*r * tr); *g = DIV255(*g * tg); *b = DIV255(*b * tb);
            *al = DIV255(*al * ta);
            break;
    }
}

/* fog, list-specific step (PT alpha test / TR blend) and the store */
static inline __attribute__((always_inline)) void px_finish(uint16_t *cp, float *zp, float z, int r, int g, int b, int al,
                                                            int f, const TriCtx *c, int dwrite, const int KIND, const int FOG) {
    if (FOG && c->fog) {
        r += ((c->fr - r) * f) / 255;
        g += ((c->fg - g) * f) / 255;
        b += ((c->fb - b) * f) / 255;
    }
    if (KIND == KIND_PT) {
        if (al <= 0xC0)
            return;
    } else if (KIND == KIND_TR) {
        if (!(al == 255 && c->bs == GFX_BLENDF_SRCALPHA && c->bd == GFX_BLENDF_INVSRCALPHA)) {
            uint32_t d = *cp;
            int dr = sExp5[(d >> 11) & 31], dg = sExp6[(d >> 5) & 63], db = sExp5[d & 31];
            int sf, df;
            switch (c->bs) {
                case GFX_BLENDF_ZERO: sf = 0; break;
                case GFX_BLENDF_SRCALPHA: sf = al; break;
                case GFX_BLENDF_INVSRCALPHA: sf = 255 - al; break;
                case GFX_BLENDF_ONE: case GFX_BLENDF_DSTALPHA: sf = 255; break;
                default: sf = 0; break;
            }
            switch (c->bd) {
                case GFX_BLENDF_ZERO: df = 0; break;
                case GFX_BLENDF_SRCALPHA: df = al; break;
                case GFX_BLENDF_INVSRCALPHA: df = 255 - al; break;
                case GFX_BLENDF_ONE: case GFX_BLENDF_DSTALPHA: df = 255; break;
                default: df = 0; break;
            }
            r = DIV255(r * sf + dr * df);
            g = DIV255(g * sf + dg * df);
            b = DIV255(b * sf + db * df);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
        }
    }
    *cp = (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    if (dwrite)
        *zp = z;
}

/* Can a run of n pixels index the texture without per-pixel wrapping? Yes when the
   coordinate stays inside the texture (mask = all ones) or the axis repeats with a
   power-of-two size (mask = size - 1). */
static inline int axis_mask(int32_t U, int32_t dU, int n, int size, int mode, uint32_t *mask) {
    int64_t end = (int64_t) U + (int64_t) dU * (n - 1);
    int32_t i0 = U >> 16, i1 = (int32_t) (end >> 16);
    int lo = i0 < i1 ? i0 : i1, hi = i0 < i1 ? i1 : i0;
    if (lo >= 0 && hi < size && end > -2147483647LL && end < 2147483647LL) {
        *mask = 0xFFFFFFFFu;
        return 1;
    }
    if (mode == WR_REPEAT2) {
        *mask = (uint32_t) (size - 1);
        return 1;
    }
    return 0;
}

/* one horizontal span: pixels [xs, xe) of row py, attributes a[] at pixel xs */
static inline __attribute__((always_inline)) uint32_t span_body(const TriCtx *c, int py, int xs, int xe,
                                                                 const float *a, const float *ddx,
                                                                 const int KIND, const int TEX, const int GOUR) {
    /* TEX: 0 = untextured, 1 = textured (c->persp picks affine or perspective-correct) */
    const int W = port_render_w;
    uint16_t *restrict cp = port_color + py * W + xs;
    float *restrict zp = port_depth + py * W + xs;
    float z = a[A_Z];
    const float dz = ddx[A_Z];
    const int dtest = c->dtest, dwrite = c->dwrite;
    int32_t R = 0, G = 0, B = 0, A = 0, OR = 0, OG = 0, OB = 0, F = 0;
    int32_t dR = 0, dG = 0, dB = 0, dA = 0, dOR = 0, dOG = 0, dOB = 0, dF = 0;
    const int fr = c->r, fg = c->g, fb = c->b, fa = c->a;
    if (GOUR) {
        R = (int32_t) (a[A_R] * 65536.0f); dR = (int32_t) (ddx[A_R] * 65536.0f);
        G = (int32_t) (a[A_G] * 65536.0f); dG = (int32_t) (ddx[A_G] * 65536.0f);
        B = (int32_t) (a[A_B] * 65536.0f); dB = (int32_t) (ddx[A_B] * 65536.0f);
        A = (int32_t) (a[A_A] * 65536.0f); dA = (int32_t) (ddx[A_A] * 65536.0f);
    }
    if (GOUR == 2) {
        OR = (int32_t) (a[A_OR] * 65536.0f); dOR = (int32_t) (ddx[A_OR] * 65536.0f);
        OG = (int32_t) (a[A_OG] * 65536.0f); dOG = (int32_t) (ddx[A_OG] * 65536.0f);
        OB = (int32_t) (a[A_OB] * 65536.0f); dOB = (int32_t) (ddx[A_OB] * 65536.0f);
        F = (int32_t) (a[A_F] * 65536.0f); dF = (int32_t) (ddx[A_F] * 65536.0f);
    }

#define PX_COLOUR()                                                                              \
    int r, g, b, al;                                                                             \
    if (GOUR) {                                                                                  \
        r = clamp8(R >> 16); g = clamp8(G >> 16); b = clamp8(B >> 16); al = clamp8(A >> 16);     \
    } else {                                                                                     \
        r = fr; g = fg; b = fb; al = fa;                                                         \
    }
#define PX_OFFSET()                                                                              \
    if (GOUR == 2) {                                                                             \
        r += clamp8(OR >> 16); g += clamp8(OG >> 16); b += clamp8(OB >> 16);                     \
        if (r > 255) r = 255;                                                                    \
        if (g > 255) g = 255;                                                                    \
        if (b > 255) b = 255;                                                                    \
    }
#define PX_STEP()                                                                                \
    z += dz;                                                                                     \
    if (GOUR) { R += dR; G += dG; B += dB; A += dA; }                                            \
    if (GOUR == 2) { OR += dOR; OG += dOG; OB += dOB; F += dF; }

    if (TEX == 0) {
        for (int n = xe - xs; n > 0; n--, cp++, zp++) {
            if (!dtest || z >= *zp) {
                PX_COLOUR();
                px_finish(cp, zp, z, r, g, b, al, clamp8(F >> 16), c, dwrite, KIND, GOUR == 2);
            }
            PX_STEP();
        }
        return (uint32_t) (xe - xs);
    }

    /* textured: runs of pixels with linear texel coordinates (the whole span when affine,
       16 pixels when perspective-correct), each drawn by the fastest loop that fits */
    const uint32_t *const tpx = c->tpx;
    const int tw = c->tw, th = c->th;
    float uz = 0, vz = 0, duz = 0, dvz = 0, zz = 0;
    int32_t U = 0, V = 0, dU = 0, dV = 0;
    const int persp = c->persp;
    if (!persp) {
        float u = tc_fold(a[A_U] * c->ftw + c->bias, tw, c->uwrap), v = tc_fold(a[A_V] * c->fth + c->bias, th, c->vwrap);
        /* 1:1 2D images land exactly on texel edges: round the step and start a hair (1/256
           texel) above the edge so every pixel picks the same texel, with no drift */
        U = (int32_t) lrintf(u * 65536.0f) + 256; dU = (int32_t) lrintf(ddx[A_U] * c->ftw * 65536.0f);
        V = (int32_t) lrintf(v * 65536.0f) + 256; dV = (int32_t) lrintf(ddx[A_V] * c->fth * 65536.0f);
    } else {
        uz = a[A_U]; vz = a[A_V]; zz = z;
        duz = ddx[A_U]; dvz = ddx[A_V];
    }
    int px = xs;
    while (px < xe) {
        int len = xe - px;
        if (persp) {
            if (len > 16)
                len = 16;
            float iz0 = 1.0f / zz;
            float u0 = uz * iz0 * c->ftw + c->bias, v0 = vz * iz0 * c->fth + c->bias;
            float zz1 = zz + dz * len;
            float iz1 = 1.0f / zz1;
            float u1 = (uz + duz * len) * iz1 * c->ftw + c->bias, v1 = (vz + dvz * len) * iz1 * c->fth + c->bias;
            float fu = tc_fold(u0, tw, c->uwrap), fv = tc_fold(v0, th, c->vwrap);
            u1 += fu - u0;
            v1 += fv - v0;
            float il = 1.0f / (float) len;
            U = (int32_t) lrintf(fu * 65536.0f) + 256; dU = (int32_t) lrintf((u1 - fu) * il * 65536.0f);
            V = (int32_t) lrintf(fv * 65536.0f) + 256; dV = (int32_t) lrintf((v1 - fv) * il * 65536.0f);
            uz += duz * len; vz += dvz * len; zz = zz1;
        }
        uint32_t um, vm;
        if (axis_mask(U, dU, len, tw, c->uwrap, &um) && axis_mask(V, dV, len, th, c->vwrap, &vm)) {
#define FAST_LOOP(ENV)                                                                           \
            for (int n = len; n > 0; n--, cp++, zp++) {                                          \
                if (!dtest || z >= *zp) {                                                        \
                    PX_COLOUR();                                                                 \
                    uint32_t tc = tpx[((uint32_t) (V >> 16) & vm) * tw + ((uint32_t) (U >> 16) & um)]; \
                    tex_combine(ENV, tc, &r, &g, &b, &al);                                       \
                    PX_OFFSET();                                                                 \
                    px_finish(cp, zp, z, r, g, b, al, clamp8(F >> 16), c, dwrite, KIND, GOUR == 2); \
                }                                                                                \
                PX_STEP();                                                                       \
                U += dU; V += dV;                                                                \
            }
            switch (c->texenv) {
                case GFX_TEXENV_REPLACE: FAST_LOOP(GFX_TEXENV_REPLACE); break;
                case GFX_TEXENV_MODULATE: FAST_LOOP(GFX_TEXENV_MODULATE); break;
                case GFX_TEXENV_DECAL: FAST_LOOP(GFX_TEXENV_DECAL); break;
                default: FAST_LOOP(GFX_TEXENV_MODULATEALPHA); break;
            }
#undef FAST_LOOP
        } else {
            /* clamp / mirror / odd sizes crossing an edge: wrap every texel */
            const int env = c->texenv;
            for (int n = len; n > 0; n--, cp++, zp++) {
                if (!dtest || z >= *zp) {
                    PX_COLOUR();
                    int iu = tc_wrap(U >> 16, tw, c->umask, c->uwrap);
                    int iv = tc_wrap(V >> 16, th, c->vmask, c->vwrap);
                    tex_combine(env, tpx[iv * tw + iu], &r, &g, &b, &al);
                    PX_OFFSET();
                    px_finish(cp, zp, z, r, g, b, al, clamp8(F >> 16), c, dwrite, KIND, GOUR == 2);
                }
                PX_STEP();
                U += dU; V += dV;
            }
        }
        px += len;
    }
#undef PX_COLOUR
#undef PX_OFFSET
#undef PX_STEP
    return (uint32_t) (xe - xs);
}

typedef uint32_t (*SpanFn)(const TriCtx *, int, int, int, const float *, const float *);

#define SPAN_DEF(K, T, G) \
    static uint32_t span_##K##_##T##_##G(const TriCtx *c, int py, int xs, int xe, const float *a, const float *d) { \
        return span_body(c, py, xs, xe, a, d, K, T, G); \
    }
#define SPAN_DEF_T(K, T) SPAN_DEF(K, T, 0) SPAN_DEF(K, T, 1) SPAN_DEF(K, T, 2)
#define SPAN_DEF_K(K) SPAN_DEF_T(K, 0) SPAN_DEF_T(K, 1)
SPAN_DEF_K(0)
SPAN_DEF_K(1)
SPAN_DEF_K(2)
#define SPAN_REF_T(K, T) { span_##K##_##T##_0, span_##K##_##T##_1, span_##K##_##T##_2 }
#define SPAN_REF(K) { SPAN_REF_T(K, 0), SPAN_REF_T(K, 1) }
/* [list kind][0 untextured, 1 textured][0 flat colour, 1 gouraud, 2 gouraud + offset/fog] */
static const SpanFn sSpanFns[3][2][3] = { SPAN_REF(0), SPAN_REF(1), SPAN_REF(2) };

static int wrap_mode(int n, int clamp, int mirror) {
    int pow2 = (n & (n - 1)) == 0;
    if (clamp) return WR_CLAMP;
    if (mirror) return pow2 ? WR_MIRROR2 : WR_MIRRORN;
    return pow2 ? WR_REPEAT2 : WR_REPEATN;
}

/* rows handled by this call: all, or (for the two-core split) bands of 8 rows by parity */
static __thread int sBandMask = 0, sBandVal = 0;

/* draw one triangle with state st and list kind */
static void raster_tri(const dc_fast_t *v0, const dc_fast_t *v1, const dc_fast_t *v2, const RState *st, int kind) {
    const int W = port_render_w, H = port_render_h;
    const float sx = (float) W * (1.0f / 640.0f), sy = (float) H * (1.0f / 480.0f);
    const dc_fast_t *vv[3] = { v0, v1, v2 };
    float X[3], Y[3];
    float at[3][NATTR];

    for (int i = 0; i < 3; i++) {
        const dc_fast_t *v = vv[i];
        X[i] = v->vert.x * sx;
        Y[i] = v->vert.y * sy;
        if (!(X[i] == X[i]) || !(Y[i] == Y[i]) || fabsf(X[i]) > 1e6f || fabsf(Y[i]) > 1e6f)
            return;
        uint32_t cc = v->color.packed, o = v->pad0.vertindex;
        at[i][A_Z] = v->vert.z;
        at[i][A_U] = v->texture.u;
        at[i][A_V] = v->texture.v;
        at[i][A_R] = (float) ((cc >> 16) & 0xFF);
        at[i][A_G] = (float) ((cc >> 8) & 0xFF);
        at[i][A_B] = (float) (cc & 0xFF);
        at[i][A_A] = (float) (cc >> 24);
        at[i][A_OR] = (float) ((o >> 16) & 0xFF);
        at[i][A_OG] = (float) ((o >> 8) & 0xFF);
        at[i][A_OB] = (float) (o & 0xFF);
        at[i][A_F] = (float) (o >> 24);
    }

    float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (!(fabsf(area) > 1e-6f))
        return;

    const struct SwTex *tx = st->tex ? &sTex[st->tex] : NULL;
    const int textured = tx && tx->px;
    const int persp = textured && !(at[0][A_Z] == at[1][A_Z] && at[0][A_Z] == at[2][A_Z]) &&
                      at[0][A_Z] > 0.0f && at[1][A_Z] > 0.0f && at[2][A_Z] > 0.0f;
    if (persp) {
        for (int i = 0; i < 3; i++) {
            at[i][A_U] *= at[i][A_Z];
            at[i][A_V] *= at[i][A_Z];
        }
    }

    TriCtx c;
    c.texenv = st->texenv;
    /* PVR translucent list (autosort): always depth-tested against the opaque/punch-through
       depth, whatever the poly's compare mode, and never writes depth */
    c.dtest = st->depth_test || kind == KIND_TR;
    c.dwrite = st->depth_write && kind != KIND_TR;
    c.fog = st->fog;
    c.fr = (sFogColor >> 16) & 0xFF; c.fg = (sFogColor >> 8) & 0xFF; c.fb = sFogColor & 0xFF;
    c.bs = st->blend_src; c.bd = st->blend_dst;
    if (textured) {
        c.tpx = tx->px;
        c.tw = tx->w; c.th = tx->h;
        c.ftw = (float) tx->w; c.fth = (float) tx->h;
        c.umask = tx->w - 1; c.vmask = tx->h - 1;
        c.uwrap = wrap_mode(tx->w, st->clampu, st->mirru);
        c.bias = 0.0f;
        c.vwrap = wrap_mode(tx->h, st->clampv, st->mirrv);
    } else {
        c.tpx = NULL; c.tw = c.th = 1; c.ftw = c.fth = 1.0f; c.bias = 0.0f; c.umask = c.vmask = 0; c.uwrap = c.vwrap = WR_REPEAT2;
    }
    /* colour path: flat (same colour on the three vertices), gouraud, or gouraud with the
       offset colour (only used when textured) and fog (only when enabled) */
    const uint32_t c0 = v0->color.packed;
    const uint32_t oall = v0->pad0.vertindex | v1->pad0.vertindex | v2->pad0.vertindex;
    const int extras = (textured && (oall & 0x00FFFFFFu)) || (st->fog && (oall & 0xFF000000u));
    const int flat = !extras && v1->color.packed == c0 && v2->color.packed == c0;
    c.r = (c0 >> 16) & 0xFF; c.g = (c0 >> 8) & 0xFF; c.b = c0 & 0xFF; c.a = c0 >> 24;
    /* white flat colour: modulate does nothing, the texel goes straight through */
    if (flat && textured && c0 == 0xFFFFFFFFu && c.texenv != GFX_TEXENV_DECAL)
        c.texenv = GFX_TEXENV_REPLACE;
    c.persp = persp;
    const SpanFn span = sSpanFns[kind][textured ? 1 : 0][extras ? 2 : (flat ? 0 : 1)];

    /* attribute gradients */
    const float inv = 1.0f / area;
    const float dy1 = Y[1] - Y[0], dy2 = Y[2] - Y[0], dx1 = X[1] - X[0], dx2 = X[2] - X[0];
    float ddx[NATTR], ddy[NATTR];
    for (int k = 0; k < NATTR; k++) {
        float a1 = at[1][k] - at[0][k], a2 = at[2][k] - at[0][k];
        ddx[k] = (a1 * dy2 - a2 * dy1) * inv;
        ddy[k] = (a2 * dx1 - a1 * dx2) * inv;
    }

    /* sort by y for the span walk */
    int i0 = 0, i1 = 1, i2 = 2, t;
    if (Y[i1] < Y[i0]) { t = i0; i0 = i1; i1 = t; }
    if (Y[i2] < Y[i0]) { t = i0; i0 = i2; i2 = t; }
    if (Y[i2] < Y[i1]) { t = i1; i1 = i2; i2 = t; }
    const float xa = X[i0], ya = Y[i0], xb = X[i1], yb = Y[i1], xc = X[i2], yc = Y[i2];

    int ystart = (int) ceilf(ya - 0.5f), yend = (int) ceilf(yc - 0.5f);
    if (ystart < 0) ystart = 0;
    if (yend > H) yend = H;
    if (ystart >= yend)
        return;

    const float slope_ac = (yc - ya) != 0.0f ? (xc - xa) / (yc - ya) : 0.0f;
    const float slope_ab = (yb - ya) != 0.0f ? (xb - xa) / (yb - ya) : 0.0f;
    const float slope_bc = (yc - yb) != 0.0f ? (xc - xb) / (yc - yb) : 0.0f;
    uint32_t pixels = 0;

    for (int py = ystart; py < yend; py++) {
        if (((py >> 3) & sBandMask) != sBandVal)
            continue;
        const float fy = (float) py + 0.5f;
        float xl = xa + (fy - ya) * slope_ac, xr;
        if (fy < yb)
            xr = xa + (fy - ya) * slope_ab;
        else
            xr = xb + (fy - yb) * slope_bc;
        if (xl > xr) {
            float tmp = xl;
            xl = xr;
            xr = tmp;
        }
        int xs = (int) ceilf(xl - 0.5f), xe = (int) ceilf(xr - 0.5f);
        if (xs < 0) xs = 0;
        if (xe > W) xe = W;
        if (xs >= xe)
            continue;

        const float ox = (float) xs + 0.5f - X[0], oy = fy - Y[0];
        float a[NATTR];
        for (int k = 0; k < NATTR; k++)
            a[k] = at[0][k] + ddx[k] * ox + ddy[k] * oy;
        pixels += span(&c, py, xs, xe, a, ddx);
    }
    tlPixels += pixels;
    tlTris++;
}

/* ------------------------------------------------------------------------- */
/* frame capture (SELECT + X): every draw of one frame, the textures it uses and */
/* the finished picture go to capture_NN.bin in the game folder, so the frame   */
/* can be replayed and inspected off the device.                                */
/* ------------------------------------------------------------------------- */

static FILE *sCapF;
static int sCapRequest, sCapCount;
static uint8_t sCapTexSeen[TEX_MAX];

void gfx_sw_request_capture(void) {
    sCapRequest = 1;
}

static void cap_u32(uint32_t v) { fwrite(&v, 4, 1, sCapF); }

static void cap_begin(void) {
    char name[64];
    snprintf(name, sizeof(name), "capture_%02d.bin", sCapCount++);
    sCapF = fopen(name, "wb");
    if (!sCapF) {
        printf("captura: nao consegui criar %s\n", name);
        return;
    }
    memset(sCapTexSeen, 0, sizeof(sCapTexSeen));
    fwrite("MK64CAP1", 8, 1, sCapF);
    cap_u32((uint32_t) port_render_w);
    cap_u32((uint32_t) port_render_h);
    printf("captura: gravando %s\n", name);
}

static void cap_draw(const dc_fast_t *v, size_t n, const RState *st, int kind) {
    if (st->tex && !sCapTexSeen[st->tex] && sTex[st->tex].px) {
        const struct SwTex *t = &sTex[st->tex];
        sCapTexSeen[st->tex] = 1;
        fwrite("T", 1, 1, sCapF);
        cap_u32(st->tex);
        cap_u32(t->w);
        cap_u32(t->h);
        fwrite(t->px, 4, (size_t) t->w * t->h, sCapF);
    }
    fwrite("D", 1, 1, sCapF);
    cap_u32((uint32_t) kind);
    cap_u32(st->tex);
    {
        uint8_t b[12] = { st->texenv, st->depth_test, st->depth_write, st->fog, st->blend_src, st->blend_dst,
                          st->clampu, st->clampv, st->mirru, st->mirrv, st->filter, 0 };
        fwrite(b, 1, sizeof(b), sCapF);
    }
    cap_u32(sFogColor);
    cap_u32(cur_shader ? cur_shader->shader_id : 0xFFFFFFFFu);
    cap_u32(cur_shader ? cur_shader->texture_used[0] : 0);
    cap_u32(sBound[0]);
    cap_u32((uint32_t) n);
    fwrite(v, sizeof(dc_fast_t), n, sCapF);
}

static void cap_end(void) {
    size_t np = (size_t) port_render_w * port_render_h;
    fwrite("F", 1, 1, sCapF);
    fwrite(port_color, 2, np, sCapF);
    fwrite(port_depth, 4, np, sCapF);
    fclose(sCapF);
    sCapF = NULL;
    printf("captura: pronta\n");
}

static void raster_list(const dc_fast_t *v, size_t n, const RState *st, int kind) {
    for (size_t i = 0; i + 2 < n; i += 3)
        raster_tri(&v[i], &v[i + 1], &v[i + 2], st, kind);
}

/* ------------------------------------------------------------------------- */
/* geometry entry points used by the front-end                                */
/* ------------------------------------------------------------------------- */

static dc_fast_t *bucket_reserve(Bucket *b, size_t n) {
    if (b->dirty || b->cur < 0) {
        if (b->nbatch >= BUCKET_BATCH) {
            if (sDropLog++ < 8)
                printf("SW_DROP batch\n");
            return NULL;
        }
        b->cur = b->nbatch++;
        current_state(&b->batch[b->cur].st);
        b->batch[b->cur].start = b->nverts;
        b->batch[b->cur].count = 0;
        b->dirty = 0;
    }
    if (b->nverts + n > BUCKET_VERTS) {
        if (sDropLog++ < 8)
            printf("SW_DROP verts\n");
        return NULL;
    }
    dc_fast_t *out = &b->verts[b->nverts];
    b->nverts += (uint32_t) n;
    b->batch[b->cur].count += (uint32_t) n;
    return out;
}

/* PT (kind 1) / TR (kind 2): the front-end bakes vertices straight into the bucket */
dc_fast_t *pvr_reserve(int kind, size_t n) {
    return bucket_reserve(kind == KIND_TR ? &sTR : &sPT, n);
}

/* OP: draw now */
void pvr_submit_op(const dc_fast_t *tris, size_t n) {
    dc_fast_t *d = bucket_reserve(&sOP, n);
    if (d)
        memcpy(d, tris, n * sizeof(dc_fast_t));
}

void __attribute__((noinline)) pvr_emit_op_header_slow(void) {
    gfx_pvr_op_dirty = 0;
}

static void route(const dc_fast_t *v, size_t n) {
    if (sListKind == KIND_OP) {
        pvr_submit_op(v, n);
        return;
    }
    dc_fast_t *d = bucket_reserve(sListKind == KIND_TR ? &sTR : &sPT, n);
    if (d)
        memcpy(d, v, n * sizeof(dc_fast_t));
}

extern float screen_2d_z;
extern int in_intro;

static void sw_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    (void) buf_vbo_len;
    route((const dc_fast_t *) buf_vbo, buf_vbo_num_tris * 3);
}

void gfx_pvr_draw_triangles_2d(void *buf_vbo, size_t buf_vbo_len, size_t use_texture) {
    (void) buf_vbo_len;
    (void) use_texture;
    dc_fast_t *v = (dc_fast_t *) buf_vbo;
    /* portrait/face black background (authored override, as in gfx_pvr.c) */
    if (cur_shader && cur_shader->shader_id == 0x01200A00 && !in_intro)
        for (int i = 0; i < 6; i++)
            v[i].color.packed = 0xFF000000;
    route(v, 6);
}

/* ------------------------------------------------------------------------- */
/* state entry points                                                         */
/* ------------------------------------------------------------------------- */

void gfx_pvr_set_blend(uint8_t kind) {
    sListKind = kind;
}

void gfx_pvr_set_blend_factors(uint8_t src_code, uint8_t dst_code) {
    if (src_code != sBlendSrc || dst_code != sBlendDst) {
        sBlendSrc = src_code;
        sBlendDst = dst_code;
        sTR.dirty = 1;
    }
}

void gfx_pvr_set_fog(uint8_t enabled) {
    enabled = enabled ? 1 : 0;
    if (sFog != enabled) {
        sFog = enabled;
        mark_dirty();
    }
}

void gfx_pvr_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    (void) a;
    sFogColor = ((uint32_t) r << 16) | ((uint32_t) g << 8) | b;
}

void gfx_pvr_request_cache_flush(void) {
    sCacheFlushPending = 1;
}

void gfx_pvr_set_alloc_floor(uint32_t full_w, uint32_t full_h) {
    (void) full_w;
    (void) full_h;
}

int gfx_pvr_texture_resident(uint32_t id) {
    return id < TEX_MAX && sTex[id].px != NULL;
}

float gfx_pvr_get_u_scale(void) {
    return 1.0f;
}

float gfx_pvr_get_v_scale(void) {
    return 1.0f;
}

void gfx_pvr_clear_all_textures(void) {
    for (uint32_t i = 0; i < TEX_MAX; i++) {
        free(sTex[i].px);
        memset(&sTex[i], 0, sizeof(sTex[i]));
    }
    sTexCount = 0;
    sBound[0] = sBound[1] = 0;
    sCurBound = 0;
    mark_dirty();
}

extern void reset_texcache(void);
void nuke_everything(void) {
    gfx_pvr_clear_all_textures();
    reset_texcache();
}

/* ------------------------------------------------------------------------- */
/* GfxRenderingAPI                                                            */
/* ------------------------------------------------------------------------- */

static uint8_t sw_z_is_from_0_to_1(void) {
    return 0;
}

static void sw_unload_shader(struct ShaderProgram *old_prg) {
    (void) old_prg;
    cur_shader = NULL;
}

static void sw_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    mark_dirty();
}

static struct ShaderProgram *sw_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);
    if (shader_pool_size >= (int) (sizeof(shader_pool) / sizeof(shader_pool[0]))) {
        printf("SW: shader pool full\n");
        shader_pool_size = 0;
    }
    struct ShaderProgram *prg = &shader_pool[shader_pool_size++];
    prg->shader_id = shader_id;
    prg->cc = ccf;
    prg->num_inputs = ccf.num_inputs;
    prg->texture_used[0] = ccf.used_textures[0];
    prg->texture_used[1] = ccf.used_textures[1];
    sw_load_shader(prg);
    return prg;
}

static struct ShaderProgram *sw_lookup_shader(uint32_t shader_id) {
    for (int i = 0; i < shader_pool_size; i++)
        if (shader_pool[i].shader_id == shader_id)
            return &shader_pool[i];
    return NULL;
}

static void sw_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, uint8_t used_textures[2]) {
    *num_inputs = (uint8_t) prg->num_inputs;
    used_textures[0] = prg->texture_used[0];
    used_textures[1] = prg->texture_used[1];
}

static uint32_t sw_new_texture(void) {
    uint32_t id = ++sTexCount;
    if (id >= TEX_MAX) {
        static int logged;
        if (logged++ < 8)
            printf("SW_TEXID_CLAMP %u\n", (unsigned) id);
        id = TEX_MAX - 1;
    }
    free(sTex[id].px);
    memset(&sTex[id], 0, sizeof(sTex[id]));
    return id;
}

static void sw_select_texture(int tile, uint32_t texture_id) {
    if (texture_id >= TEX_MAX)
        texture_id = 0;
    if (sBound[tile] != texture_id)
        mark_dirty();
    sBound[tile] = texture_id;
    sCurBound = texture_id;
}

static void sw_upload_texture(const uint8_t *buf16, int width, int height, unsigned int type) {
    struct SwTex *t = &sTex[sCurBound];
    if (width <= 0 || height <= 0)
        return;
    uint32_t n = (uint32_t) width * (uint32_t) height;
    if (!t->px || t->alloc < n) {
        uint32_t *p = realloc(t->px, n * 4);
        if (!p) {
            printf("SW: texture alloc failed %dx%d\n", width, height);
            return;
        }
        t->px = p;
        t->alloc = n;
    }
    const uint16_t *s = (const uint16_t *) buf16;
    uint32_t *d = t->px;
    uint32_t fmt = type & (3u << 27);
    if (fmt == PVR_TXRFMT_ARGB4444) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = s[i];
            d[i] = ((c >> 12) * 0x11u) << 24 | (((c >> 8) & 15) * 0x11u) << 16 | (((c >> 4) & 15) * 0x11u) << 8 |
                   ((c & 15) * 0x11u);
        }
    } else if (fmt == PVR_TXRFMT_RGB565) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = s[i];
            uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            d[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
    } else { /* ARGB1555 */
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = s[i];
            uint32_t r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
            d[i] = ((c & 0x8000) ? 0xFF000000u : 0) | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) |
                   (b << 3 | b >> 2);
        }
    }
    t->w = (uint16_t) width;
    t->h = (uint16_t) height;
    stTexUp++;
    mark_dirty();
}

static void sw_set_sampler_parameters(int tile, uint8_t linear_filter, uint32_t cms, uint32_t cmt) {
    struct SwTex *t = &sTex[sBound[tile]];
    t->filter = linear_filter ? 1 : 0;
    t->clampu = (cms & G_TX_CLAMP) ? 1 : 0;
    t->clampv = (cmt & G_TX_CLAMP) ? 1 : 0;
    t->mirru = (!(cms & G_TX_CLAMP) && (cms & G_TX_MIRROR)) ? 1 : 0;
    t->mirrv = (!(cmt & G_TX_CLAMP) && (cmt & G_TX_MIRROR)) ? 1 : 0;
    mark_dirty();
}

static void sw_set_depth_test(uint8_t depth_test) {
    if (sDepthTest != depth_test) {
        sDepthTest = depth_test;
        mark_dirty();
    }
}

static void sw_set_depth_mask(uint8_t z_upd) {
    if (sDepthWrite != z_upd) {
        sDepthWrite = z_upd;
        mark_dirty();
    }
}

static void sw_set_zmode_decal(uint8_t zmode_decal) {
    (void) zmode_decal; /* handled by the front-end (decal z bias) */
}

static void sw_set_tex_env(uint32_t mode) {
    if (sTexEnv != (uint8_t) mode) {
        sTexEnv = (uint8_t) mode;
        mark_dirty();
    }
}

static void sw_set_viewport(int x, int y, int w, int h) {
    (void) x; (void) y; (void) w; (void) h;
}

static void sw_set_scissor(int x, int y, int w, int h) {
    (void) x; (void) y; (void) w; (void) h; /* the front-end clips in software */
}

static void sw_set_use_alpha(uint8_t use_alpha) {
    (void) use_alpha;
}

static void start_worker(void);
static void sw_init(void) {
    start_worker();
}

static void sw_on_resize(void) {
}

static double sFrameT0;

static void sw_start_frame(void) {
    sFrameT0 = port_now_ms();
    if (sCapRequest && !sCapF) {
        sCapRequest = 0;
        cap_begin();
    }
    if (sCacheFlushPending) {
        gfx_pvr_clear_all_textures();
        reset_texcache();
        sCacheFlushPending = 0;
    }
    screen_2d_z = 1.0f;

    sOP.nverts = 0; sOP.nbatch = 0; sOP.cur = -1; sOP.dirty = 1;
    sPT.nverts = 0; sPT.nbatch = 0; sPT.cur = -1; sPT.dirty = 1;
    sTR.nverts = 0; sTR.nbatch = 0; sTR.cur = -1; sTR.dirty = 1;
    gfx_pvr_op_dirty = 1;

    {
        extern int cur_frame_persp, prev_frame_had_persp, has_done_3d;
        prev_frame_had_persp = cur_frame_persp;
        cur_frame_persp = 0;
        has_done_3d = 0;
    }
}

static void flush_bucket(Bucket *b, int kind) {
    for (int i = 0; i < b->nbatch; i++) {
        Batch *bt = &b->batch[i];
        if (bt->count)
            raster_list(&b->verts[bt->start], bt->count, &bt->st, kind);
    }
}

/* TR list = PVR autosort: translucent triangles are composited back to front by depth
   (z = 1/w, smaller = farther), not in submission order. Approximated per triangle by
   its mean z; equal depths keep submission order (2D overlays step z per quad). */
typedef struct {
    float key;
    uint32_t seq;
    uint16_t batch;
} TrSort;

static TrSort sTrSort[BUCKET_VERTS / 3];
static uint32_t sTrSortN;

static int tr_cmp(const void *a, const void *b) {
    const TrSort *x = a, *y = b;
    if (x->key < y->key) return -1;
    if (x->key > y->key) return 1;
    return (x->seq > y->seq) - (x->seq < y->seq);
}

static void sort_tr(void) {
    Bucket *b = &sTR;
    if (!sExp6[63]) {   /* blend tables (first frame, before any raster thread runs) */
        for (int i = 0; i < 32; i++) sExp5[i] = (uint8_t) (i * 255 / 31);
        for (int i = 0; i < 64; i++) sExp6[i] = (uint8_t) (i * 255 / 63);
    }
    uint32_t nt = 0;
    for (int i = 0; i < b->nbatch; i++) {
        Batch *bt = &b->batch[i];
        for (uint32_t t = 0; t + 2 < bt->count; t += 3) {
            const dc_fast_t *v = &b->verts[bt->start + t];
            sTrSort[nt].key = v[0].vert.z + v[1].vert.z + v[2].vert.z;
            sTrSort[nt].seq = bt->start + t;
            sTrSort[nt].batch = (uint16_t) i;
            nt++;
        }
    }
    qsort(sTrSort, nt, sizeof(sTrSort[0]), tr_cmp);
    sTrSortN = nt;
}

static void raster_tr_sorted(void) {
    Bucket *b = &sTR;
    for (uint32_t i = 0; i < sTrSortN; i++) {
        const dc_fast_t *v = &b->verts[sTrSort[i].seq];
        raster_tri(&v[0], &v[1], &v[2], &b->batch[sTrSort[i].batch].st, KIND_TR);
    }
}

/* clears this thread's rows (black, depth 0 = farthest) and draws the three lists on them */
static void raster_frame_band(int mask, int val) {
    const int W = port_render_w, H = port_render_h;
    sBandMask = mask;
    sBandVal = val;
    for (int y0 = 0; y0 < H; y0 += 8) {
        if (((y0 >> 3) & mask) != val)
            continue;
        int rows = H - y0 < 8 ? H - y0 : 8;
        memset(port_color + y0 * W, 0, (size_t) rows * W * sizeof(uint16_t));
        memset(port_depth + y0 * W, 0, (size_t) rows * W * sizeof(float));
    }
    flush_bucket(&sOP, KIND_OP);
    flush_bucket(&sPT, KIND_PT);
    raster_tr_sorted();
}

static void flush_tr_sorted(void) {   /* replay tool: single thread, whole frame */
    sort_tr();
    sBandMask = 0;
    sBandVal = 0;
    raster_tr_sorted();
}

/* ---- second core ---- */
#include <pthread.h>
static pthread_t sWorker;
static pthread_mutex_t sWMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sWCond = PTHREAD_COND_INITIALIZER;
static int sWorkerOk, sWorkGen, sWorkDone;
static uint32_t sWorkerPixels, sWorkerTris;
static double sWorkerMs;

static void *worker_proc(void *arg) {
    (void) arg;
    unsigned long mask = 2;   /* core 1 */
    syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask);
    int seen = 0;
    for (;;) {
        pthread_mutex_lock(&sWMutex);
        while (sWorkGen == seen)
            pthread_cond_wait(&sWCond, &sWMutex);
        seen = sWorkGen;
        pthread_mutex_unlock(&sWMutex);

        double t0 = port_now_ms();
        tlPixels = tlTris = 0;
        raster_frame_band(1, 1);

        pthread_mutex_lock(&sWMutex);
        sWorkerPixels = tlPixels;
        sWorkerTris = tlTris;
        sWorkerMs = port_now_ms() - t0;
        sWorkDone = seen;
        pthread_cond_broadcast(&sWCond);
        pthread_mutex_unlock(&sWMutex);
    }
    return NULL;
}

static void start_worker(void) {
    if (pthread_create(&sWorker, NULL, worker_proc, NULL) == 0) {
        pthread_detach(sWorker);
        sWorkerOk = 1;
    }
    printf("gfx: desenho em %s\n", sWorkerOk ? "2 nucleos" : "1 nucleo");
}

static void raster_frame(void) {
    tlPixels = tlTris = 0;
    if (!sWorkerOk) {
        raster_frame_band(0, 0);
    } else {
        pthread_mutex_lock(&sWMutex);
        int gen = ++sWorkGen;
        pthread_cond_broadcast(&sWCond);
        pthread_mutex_unlock(&sWMutex);

        double t0 = port_now_ms();
        raster_frame_band(1, 0);
        stCore0Ms += port_now_ms() - t0;

        pthread_mutex_lock(&sWMutex);
        while (sWorkDone != gen)
            pthread_cond_wait(&sWCond, &sWMutex);
        stCore1Ms += sWorkerMs;
        tlPixels += sWorkerPixels;
        tlTris += sWorkerTris;
        pthread_mutex_unlock(&sWMutex);
    }
    stPixels += tlPixels;
}

/* replay tool only: append a captured batch with its recorded state */
static __attribute__((unused)) void bucket_add_batch(Bucket *b, const RState *st, const dc_fast_t *v, size_t n) {
    if (b->nbatch >= BUCKET_BATCH || b->nverts + n > BUCKET_VERTS)
        return;
    Batch *bt = &b->batch[b->nbatch++];
    bt->st = *st;
    bt->start = b->nverts;
    bt->count = (uint32_t) n;
    memcpy(&b->verts[b->nverts], v, n * sizeof(dc_fast_t));
    b->nverts += (uint32_t) n;
    b->cur = -1;
}

static void count_list(Bucket *b, uint32_t *ntris, int kind) {
    for (int i = 0; i < b->nbatch; i++) {
        Batch *bt = &b->batch[i];
        *ntris += bt->count / 3;
        if (sCapF && bt->count)
            cap_draw(&b->verts[bt->start], bt->count, &bt->st, kind);
    }
}

static void sw_end_frame(void) {
    double t0 = port_now_ms();
    stWalkMs += t0 - sFrameT0;
    count_list(&sOP, &stOpTris, KIND_OP);
    count_list(&sPT, &stPtTris, KIND_PT);
    count_list(&sTR, &stTrTris, KIND_TR);
    stTris += (uint32_t) ((sOP.nverts + sPT.nverts + sTR.nverts) / 3);
    sort_tr();
    raster_frame();
    stRasterMs += port_now_ms() - t0;
    if (sCapF)
        cap_end();
}

static void sw_finish_render(void) {
}

struct GfxRenderingAPI gfx_pvr_api = {
    sw_z_is_from_0_to_1,
    sw_unload_shader,
    sw_load_shader,
    sw_create_and_load_new_shader,
    sw_lookup_shader,
    sw_shader_get_info,
    sw_new_texture,
    sw_select_texture,
    sw_upload_texture,
    sw_set_sampler_parameters,
    sw_set_depth_test,
    sw_set_depth_mask,
    sw_set_zmode_decal,
    sw_set_tex_env,
    sw_set_viewport,
    sw_set_scissor,
    sw_set_use_alpha,
    sw_draw_triangles,
    sw_init,
    sw_on_resize,
    sw_start_frame,
    sw_end_frame,
    sw_finish_render,
};
