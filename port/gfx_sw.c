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

static dc_fast_t sPtVerts[BUCKET_VERTS], sTrVerts[BUCKET_VERTS];
static Batch sPtBatch[BUCKET_BATCH], sTrBatch[BUCKET_BATCH];
static Bucket sPT = { sPtVerts, sPtBatch, 0, 0, -1, 1 };
static Bucket sTR = { sTrVerts, sTrBatch, 0, 0, -1, 1 };
static int sDropLog;

static void mark_dirty(void) {
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
        s->clampv = sTex[t].clampv;
        s->mirru = sTex[t].mirru;
        s->mirrv = sTex[t].mirrv;
    } else {
        s->clampu = s->clampv = s->mirru = s->mirrv = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* statistics                                                                 */
/* ------------------------------------------------------------------------- */

static uint32_t stTris, stPixels, stTexUp, stOpTris, stPtTris, stTrTris;
static double stRasterMs;

void gfx_sw_perf_report(int frames, double ms) {
    (void) ms;
    if (frames <= 0)
        return;
    fprintf(stderr, "perf gfx: tris %u (op %u pt %u tr %u)  pixels %u  tex uploads %u  per frame | gfx walk+raster %.2f ms/frame\n",
            stTris / frames, stOpTris / frames, stPtTris / frames, stTrTris / frames, stPixels / frames,
            stTexUp / frames, stRasterMs / frames);
    stTris = stPixels = stTexUp = stOpTris = stPtTris = stTrTris = 0;
    stRasterMs = 0;
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
        uint32_t c = v->color.packed, o = v->pad0.vertindex;
        at[i][A_Z] = v->vert.z;
        at[i][A_U] = v->texture.u;
        at[i][A_V] = v->texture.v;
        at[i][A_R] = (float) ((c >> 16) & 0xFF);
        at[i][A_G] = (float) ((c >> 8) & 0xFF);
        at[i][A_B] = (float) (c & 0xFF);
        at[i][A_A] = (float) (c >> 24);
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

    const int tw = textured ? tx->w : 1, th = textured ? tx->h : 1;
    const float ftw = (float) tw, fth = (float) th;
    const uint32_t *tpx = textured ? tx->px : NULL;
    const int texenv = st->texenv;
    const int dtest = st->depth_test;
    const int dwrite = st->depth_write && kind != KIND_TR;
    const int fog = st->fog;
    const int fr = (sFogColor >> 16) & 0xFF, fg = (sFogColor >> 8) & 0xFF, fb = sFogColor & 0xFF;
    const int bs = st->blend_src, bd = st->blend_dst;
    const int cu = st->clampu, cv = st->clampv, mu = st->mirru, mv = st->mirrv;
    uint16_t *const cbuf = port_color;
    float *const zbuf = port_depth;
    uint32_t pixels = 0;

    for (int py = ystart; py < yend; py++) {
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
        pixels += (uint32_t) (xe - xs);

        const float ox = (float) xs + 0.5f - X[0], oy = fy - Y[0];
        float a[NATTR];
        for (int k = 0; k < NATTR; k++)
            a[k] = at[0][k] + ddx[k] * ox + ddy[k] * oy;

        uint16_t *cp = cbuf + py * W + xs;
        float *zp = zbuf + py * W + xs;
        for (int px = xs; px < xe; px++, cp++, zp++) {
            const float z = a[A_Z];
            if (!dtest || z >= *zp) {
                int r = iclamp255(a[A_R]), g = iclamp255(a[A_G]), b = iclamp255(a[A_B]), al = iclamp255(a[A_A]);
                if (textured) {
                    float u = a[A_U], v = a[A_V];
                    if (persp) {
                        float iz = 1.0f / z;
                        u *= iz;
                        v *= iz;
                    }
                    int iu = wrap_coord(ifloor(u * ftw), tw, cu, mu);
                    int iv = wrap_coord(ifloor(v * fth), th, cv, mv);
                    uint32_t tc = tpx[iv * tw + iu];
                    int tr = (tc >> 16) & 0xFF, tg = (tc >> 8) & 0xFF, tb = tc & 0xFF, ta = tc >> 24;
                    switch (texenv) {
                        case GFX_TEXENV_REPLACE:
                            r = tr; g = tg; b = tb; al = ta;
                            break;
                        case GFX_TEXENV_MODULATE:
                            r = (r * tr + 127) / 255; g = (g * tg + 127) / 255; b = (b * tb + 127) / 255; al = ta;
                            break;
                        case GFX_TEXENV_DECAL:
                            r = (r * (255 - ta) + tr * ta + 127) / 255;
                            g = (g * (255 - ta) + tg * ta + 127) / 255;
                            b = (b * (255 - ta) + tb * ta + 127) / 255;
                            break;
                        default: /* MODULATEALPHA */
                            r = (r * tr + 127) / 255; g = (g * tg + 127) / 255; b = (b * tb + 127) / 255;
                            al = (al * ta + 127) / 255;
                            break;
                    }
                    r += iclamp255(a[A_OR]); g += iclamp255(a[A_OG]); b += iclamp255(a[A_OB]);
                    if (r > 255) r = 255;
                    if (g > 255) g = 255;
                    if (b > 255) b = 255;
                }
                if (fog) {
                    int f = iclamp255(a[A_F]);
                    r += ((fr - r) * f) / 255;
                    g += ((fg - g) * f) / 255;
                    b += ((fb - b) * f) / 255;
                }
                if (kind == KIND_PT) {
                    if (al <= 0xC0)
                        goto next;
                } else if (kind == KIND_TR) {
                    uint32_t d = *cp;
                    int dr = ((d >> 11) & 31) * 255 / 31, dg = ((d >> 5) & 63) * 255 / 63, db = (d & 31) * 255 / 31;
                    int sf, df;
                    switch (bs) {
                        case GFX_BLENDF_ZERO: sf = 0; break;
                        case GFX_BLENDF_ONE: sf = 255; break;
                        case GFX_BLENDF_SRCALPHA: sf = al; break;
                        case GFX_BLENDF_INVSRCALPHA: sf = 255 - al; break;
                        case GFX_BLENDF_DSTALPHA: sf = 255; break;
                        default: sf = 0; break;
                    }
                    switch (bd) {
                        case GFX_BLENDF_ZERO: df = 0; break;
                        case GFX_BLENDF_ONE: df = 255; break;
                        case GFX_BLENDF_SRCALPHA: df = al; break;
                        case GFX_BLENDF_INVSRCALPHA: df = 255 - al; break;
                        case GFX_BLENDF_DSTALPHA: df = 255; break;
                        default: df = 0; break;
                    }
                    r = (r * sf + dr * df + 127) / 255;
                    g = (g * sf + dg * df + 127) / 255;
                    b = (b * sf + db * df + 127) / 255;
                    if (r > 255) r = 255;
                    if (g > 255) g = 255;
                    if (b > 255) b = 255;
                }
                *cp = (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
                if (dwrite)
                    *zp = z;
            }
        next:
            for (int k = 0; k < NATTR; k++)
                a[k] += ddx[k];
        }
    }
    stPixels += pixels;
    stTris++;
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
                          st->clampu, st->clampv, st->mirru, st->mirrv, 0, 0 };
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
    if (sCapF)
        cap_draw(v, n, st, kind);
    for (size_t i = 0; i + 2 < n; i += 3)
        raster_tri(&v[i], &v[i + 1], &v[i + 2], st, kind);
    if (kind == KIND_OP)
        stOpTris += (uint32_t) (n / 3);
    else if (kind == KIND_PT)
        stPtTris += (uint32_t) (n / 3);
    else
        stTrTris += (uint32_t) (n / 3);
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
    RState st;
    current_state(&st);
    raster_list(tris, n, &st, KIND_OP);
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

static void sw_init(void) {
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
    /* black, depth = 0 (farthest; GEQUAL keeps the nearer = larger 1/w) */
    memset(port_color, 0, (size_t) port_render_w * port_render_h * sizeof(uint16_t));
    memset(port_depth, 0, (size_t) port_render_w * port_render_h * sizeof(float));
    screen_2d_z = 1.0f;

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

static void sw_end_frame(void) {
    flush_bucket(&sPT, KIND_PT);
    flush_bucket(&sTR, KIND_TR);
    stRasterMs += port_now_ms() - sFrameT0;
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
