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
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#else
#include "neon_emu.h"   /* PC builds of the replay / benchmark tools */
#endif

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
    uint8_t opaque;        /* every texel has alpha 255 */
    uint8_t margin;        /* some row starts or ends with invisible texels (see ext) */
    uint16_t exth;         /* rows allocated in ext */
    int16_t *ext;          /* per row: first and last texel with alpha > 0, then with alpha > 0xC0 */
    uint32_t *pf;          /* -filtro: the 3-point filter worked out in advance (see prefilter_prepare) */
    uint8_t pfwrap;        /* the clamp flags it was made with (bit 0 u, bit 1 v) */
    uint8_t pfs;           /* its cells per texel side, as a shift (see pf_shift) */
    /* -filtro on a mirrored texture (Royal Raceway's road): the texels doubled along the
       mirrored axes with the mirror image in place, so the filter sees a plain repeat */
    uint32_t *mpx;
    uint16_t mw, mh;
    uint8_t mbake;         /* the axes doubled (bit 0 u, bit 1 v) */
    uint32_t *mpf;         /* its filter worked out in advance */
    uint8_t mpfwrap, mpfs;
    uint8_t pfnever;       /* changes too often to be worth it */
    uint16_t ups;          /* uploads into this texture */
};

static struct SwTex sTex[TEX_MAX];

/* called after the texels change */
#define GFX_SW_TEX_OPAQUE 1
static void tex_update_opaque(struct SwTex *t) {
    uint32_t n = (uint32_t) t->w * t->h, a = 0xFF000000u;
    for (uint32_t i = 0; i < n; i++)
        a &= t->px[i];
    t->opaque = a == 0xFF000000u;
    /* the visible part of each row: translucent and punch-through spans that only cover
       invisible texels at their ends skip them (alpha 0 blends to the old pixel, alpha
       <= 0xC0 fails the punch-through test) */
    t->margin = 0;
    if (t->opaque)
        return;
    if (t->exth < t->h) {
        int16_t *e = realloc(t->ext, (size_t) t->h * 4 * sizeof(int16_t));
        if (!e)
            return;
        t->ext = e;
        t->exth = t->h;
    }
    for (int y = 0; y < t->h; y++) {
        const uint32_t *row = t->px + (size_t) y * t->w;
        int16_t *e = t->ext + 4 * y;
        e[0] = e[2] = (int16_t) t->w;
        e[1] = e[3] = -1;
        for (int x = 0; x < t->w; x++) {
            const uint32_t al = row[x] >> 24;
            if (al) {
                if (e[0] > x) e[0] = (int16_t) x;
                e[1] = (int16_t) x;
            }
            if (al > 0xC0) {
                if (e[2] > x) e[2] = (int16_t) x;
                e[3] = (int16_t) x;
            }
        }
        if (e[0] > 0 || e[1] < t->w - 1 || e[2] > 0 || e[3] < t->w - 1)
            t->margin = 1;
    }
}
/* -paralelo (the frame drawn on core 1 while the game prepares the next one): the frame
   being drawn reads its own copy of the entries of the textures it uses (sTexR), taken
   when it was handed over. Until it is finished, the texels and row extents of those
   textures are not touched: a texture changed or freed meanwhile gets new memory, and
   the old one is freed once that frame is done. Without -paralelo sTexRd is sTex. */
static struct SwTex sTexR[TEX_MAX];
static const struct SwTex *sTexRd = sTex;
static uint8_t sTexShared[TEX_MAX];    /* used by the frame being drawn */
static uint16_t sSharedIds[TEX_MAX];
static int sSharedN;
static void *sDeferred[5 * TEX_MAX];   /* (each texture lets go of px, ext, pf, mpx and mpf at most once per frame) */
static int sDeferredN;
static void tex_detach(struct SwTex *t) {
    const uint32_t id = (uint32_t) (t - sTex);
    if (!sTexShared[id])
        return;
    sTexShared[id] = 0;
    if (t->px)
        sDeferred[sDeferredN++] = t->px;
    if (t->ext)
        sDeferred[sDeferredN++] = t->ext;
    if (t->pf)
        sDeferred[sDeferredN++] = t->pf;
    if (t->mpx)
        sDeferred[sDeferredN++] = t->mpx;
    if (t->mpf)
        sDeferred[sDeferredN++] = t->mpf;
    t->px = NULL;
    t->alloc = 0;
    t->ext = NULL;
    t->exth = 0;
    t->pf = NULL;
    t->mpx = NULL;
    t->mpf = NULL;
}
static void pf_job_cancel(uint32_t id);
static void tex_free(struct SwTex *t) {
    pf_job_cancel((uint32_t) (t - sTex));
    tex_detach(t);
    free(t->px);
    free(t->ext);
    free(t->pf);
    free(t->mpx);
    free(t->mpf);
    memset(t, 0, sizeof(*t));
}
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
    uint8_t pane;          /* the winner's pane growing over the others (see gfx_sw_set_pane) */
    uint8_t early;         /* drawn before the other translucent triangles (see gfx_sw_set_early) */
} RState;

static uint8_t sDepthTest = 1, sDepthWrite = 1, sFog = 0, sTexEnv = GFX_TEXENV_MODULATE;
static uint8_t sBlendSrc = GFX_BLENDF_SRCALPHA, sBlendDst = GFX_BLENDF_INVSRCALPHA;
static uint8_t sListKind = KIND_OP;
static uint32_t sFogColor = 0;   /* 0x00RRGGBB */
static uint32_t sFogR;           /* -paralelo: the fog colour of the frame being drawn */
static const uint32_t *sFogRd = &sFogColor;

/* exported for the front-end (only used by its DC inline OP path) */
uint8_t gfx_pvr_op_dirty = 1;

/* (twice the first sizes: the results screen after a win draws two whole tracks in one frame;
   anything that does not fit is dropped and noted in descartes.txt, see drop_note) */
#define BUCKET_VERTS 65536
#define BUCKET_BATCH 4096

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
   and then drawn by both cores at once, each on alternating bands of 8 rows.
   -paralelo keeps a second set (allocated when it is turned on): the game fills one
   (sRecB) while the previous frame is drawn from the other (sRenB). Otherwise both
   point at the same set. */
static Bucket sBk[2][3] = { { { sOpVerts, sOpBatch, 0, 0, -1, 1 },
                              { sPtVerts, sPtBatch, 0, 0, -1, 1 },
                              { sTrVerts, sTrBatch, 0, 0, -1, 1 } } };
static Bucket *sRecB = sBk[0], *sRenB = sBk[0];
#define sOP (sRecB[KIND_OP])
#define sPT (sRecB[KIND_PT])
#define sTR (sRecB[KIND_TR])
static int sDropLog;

/* Dropped triangles (a list full) are counted per frame and, at the end of a frame that lost
   some, noted in descartes.txt in the game folder. That file is appended to and never cleared
   by the game (log.txt is rewritten at every start), so a rare case shows up later too. The
   first such frame of a run also asks for a capture of the next frame (as SELECT + X does). */
static uint32_t sDropTris[3], sDropBatch[3];
static uint32_t sPeakVerts[3], sPeakBatch[3];
static int sDropNotes, sDropCaptured;
static uint32_t sFrameNo;
extern s32 gGamestate;
extern s16 gCurrentCourseId;
void gfx_sw_request_capture(void);
static void drop_note(void) {
    if (sDropNotes < 100) {
        FILE *f = fopen("descartes.txt", "a");
        if (f) {
            if (sDropNotes == 0)
                fprintf(f, "--- nova execucao (listas de %d vertices e %d estados)\n", BUCKET_VERTS, BUCKET_BATCH);
            fprintf(f, "quadro %u, %.1f s: estado do jogo %d, pista %d | triangulos perdidos op %u pt %u tr %u, "
                       "estados perdidos op %u pt %u tr %u | maximo usado: vertices %u/%u/%u, estados %u/%u/%u\n",
                    (unsigned) sFrameNo, port_now_ms() / 1000.0, (int) gGamestate, (int) gCurrentCourseId,
                    (unsigned) sDropTris[0], (unsigned) sDropTris[1], (unsigned) sDropTris[2],
                    (unsigned) sDropBatch[0], (unsigned) sDropBatch[1], (unsigned) sDropBatch[2],
                    (unsigned) sPeakVerts[0], (unsigned) sPeakVerts[1], (unsigned) sPeakVerts[2],
                    (unsigned) sPeakBatch[0], (unsigned) sPeakBatch[1], (unsigned) sPeakBatch[2]);
            fclose(f);
        }
        sDropNotes++;
    }
    if (!sDropCaptured) {
        sDropCaptured = 1;
        gfx_sw_request_capture();
    }
    for (int k = 0; k < 3; k++)
        sDropTris[k] = sDropBatch[k] = 0;
}

static void mark_dirty(void) {
    sOP.dirty = 1;
    sPT.dirty = 1;
    sTR.dirty = 1;
    gfx_pvr_op_dirty = 1;
}

/* Battle/VS end and the end of a race: the winner's pane grows over the others. All panes
   share one depth buffer, so the DC front-end added 64 to every depth that pane emits
   (fine for the PVR's float depth); here depth is fixed point and saturates at 16, which
   made that pane's depths all equal and its textures lose their perspective. Instead the
   front-end leaves the depths alone and says which triangles belong to that pane: their
   fixed-point depth gets 2^30 added (every other depth stays below 2^29), so the pane is
   in front of everything else and keeps its own depth order. */
static uint8_t sPane;
void gfx_sw_set_pane(int on) {
    on = on ? 1 : 0;
    if (sPane != on) {
        sPane = (uint8_t) on;
        mark_dirty();
    }
}
#define PANE_ZOFF 0x40000000u

/* Sherbet Land: the kart's reflection is drawn under the ice before the ice itself (the
   course's translucent pass comes after the players on the N64), so the ice is blended over
   it. Sorted by depth like the rest of the translucent list, the reflection came before or
   after the big ice triangles depending on where the kart was: it flickered. Its triangles
   now go first in the translucent list, in depth order among themselves. */
static uint8_t sEarly;
void gfx_sw_set_early(int on) {
    on = on ? 1 : 0;
    if (sEarly != on) {
        sEarly = (uint8_t) on;
        mark_dirty();
    }
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
    s->pane = sPane;
    s->early = sEarly;
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
/* per core, per phase (clear, sky, OP, PT, TR, TR in screen space = HUD) -- measurement */
static double stPhase[2][6];
static float sCore0Share;   /* defined with the band split below */
static double stLeadMs, stPresentMs, stWaitMs;   /* -paralelo: core 1 alone, showing the frame, core 0 waiting for core 1 */
static int sPipe;                       /* -paralelo on (see pipe_init) */
static volatile int sCpuMhz;            /* -perf: the CPU clock as measured (watchdog thread) */
static int sResFrames[4];               /* -resdin: frames drawn at each size since the last report */
/* this frame only, for the hitch log (gfx_wm_miyoo.c): where a slow frame's time went */
static SwFrameStats sFS;
void gfx_sw_frame_stats(SwFrameStats *out) {
    *out = sFS;
    memset(&sFS, 0, sizeof(sFS));
}
/* -perf watchdog: where the game thread and core 1 are when frames stop coming */
static volatile int sMainStage, sWorkerStage;
static volatile unsigned sFramesDone;

void gfx_sw_perf_report(int frames, double ms) {
    (void) ms;
    if (frames <= 0)
        return;
    fprintf(stderr, "perf gfx: tris %u (op %u pt %u tr %u)  pixels %u  tex uploads %u  per frame | walk %.2f  raster %.2f (core0 %.2f core1 %.2f) ms/frame  %dx%d\n",
            stTris / frames, stOpTris / frames, stPtTris / frames, stTrTris / frames, stPixels / frames,
            stTexUp / frames, stWalkMs / frames, stRasterMs / frames, stCore0Ms / frames, stCore1Ms / frames,
            port_render_w, port_render_h);
    for (int k = 0; k < 2; k++)
        fprintf(stderr, "perf fases core%d (ms/quadro): limpa %.2f ceu %.2f op %.2f pt %.2f tr %.2f hud %.2f%s",
                k, stPhase[k][0] / frames, stPhase[k][1] / frames, stPhase[k][2] / frames, stPhase[k][3] / frames,
                stPhase[k][4] / frames, stPhase[k][5] / frames, k ? "" : " | ");
    fprintf(stderr, " | linhas do core0 %.0f%%\n", sCore0Share * 100.0);
    {
        char khz[32] = "?";
        FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
        if (f) {
            if (fgets(khz, sizeof khz, f))
                khz[strcspn(khz, "\n")] = 0;
            fclose(f);
        }
        fprintf(stderr, "perf cpu: medido %d MHz (1200 MHz mede ~1040) | o sistema diz %s kHz\n", sCpuMhz, khz);
    }
    if (sPipe)
        fprintf(stderr, "perf paralelo: core1 sozinho %.2f  core0 esperando core1 %.2f  mostrar %.2f ms/quadro\n",
                stLeadMs / frames, stWaitMs / frames, stPresentMs / frames);
    stLeadMs = stPresentMs = stWaitMs = 0;
    if (port_opt_filter_test) {
        /* -filtroteste: the next stretch of the race is drawn with the next filter mode, so
           every mode is measured over the same parts of the course */
        static const char *const names[4] = { "sem filtro", "leve 4x4", "leve niveis", "leve 2x2" };
        static int mode;
        fprintf(stderr, "perf filtroteste: este trecho foi com %s\n", names[mode]);
        mode = (mode + 1) % 4;
        port_opt_filter = mode != 0;
        port_opt_filter_lite = mode != 0;
        port_opt_pf_level = mode ? mode - 1 : 0;
    }
    if (port_opt_resdin) {
        fprintf(stderr, "perf resdin: quadros em 640x480 %d  576x432 %d  512x384 %d  448x336 %d\n",
                sResFrames[0], sResFrames[1], sResFrames[2], sResFrames[3]);
        memset(sResFrames, 0, sizeof(sResFrames));
    }
    memset(stPhase, 0, sizeof(stPhase));
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

/* lrintf (round to nearest even, the default FPSCR mode) without the library call */
static inline int32_t rnd_i(float f) {
#if defined(__arm__) && defined(__ARM_FP)
    float t;
    int32_t r;
    __asm__("vcvtr.s32.f32 %0, %1" : "=t"(t) : "t"(f));
    memcpy(&r, &t, 4);
    return r;
#else
    return (int32_t) lrintf(f);
#endif
}

/* ceil_i of two values, returned as (smaller, larger), with one move from NEON to the
   integer registers instead of two float compares */
static inline void ceil2_minmax(float a, float b, int *lo, int *hi) {
    const float32x2_t x = vset_lane_f32(b, vdup_n_f32(a), 1);
    const int32x2_t t = vcvt_s32_f32(x);
    const uint32x2_t up = vcgt_f32(x, vcvt_f32_s32(t));
    const int32x2_t c = vsub_s32(t, vreinterpret_s32_u32(up));
    const int32x2_t mm = vpmax_s32(vpmin_s32(c, c), c);   /* lane 0 = min, lane 1 = max */
    *lo = vget_lane_s32(mm, 0);
    *hi = vget_lane_s32(mm, 1);
}

/* the same, with the start clamped to 0 and the end to w, and the start also as a float
   (made in NEON: no trip through the integer registers back to the VFP) */
static inline float ceil2_clamp(float a, float b, int w, int *lo, int *hi) {
    static const int32_t kMin[2] = { 0, -2147483647 - 1 };
    const float32x2_t x = vset_lane_f32(b, vdup_n_f32(a), 1);
    const int32x2_t t = vcvt_s32_f32(x);
    const uint32x2_t up = vcgt_f32(x, vcvt_f32_s32(t));
    const int32x2_t c = vsub_s32(t, vreinterpret_s32_u32(up));
    int32x2_t mm = vpmax_s32(vpmin_s32(c, c), c);   /* lane 0 = min, lane 1 = max */
    const int32_t kMax[2] = { 2147483647, w };
    mm = vmin_s32(vmax_s32(mm, vld1_s32(kMin)), vld1_s32(kMax));
    *lo = vget_lane_s32(mm, 0);
    *hi = vget_lane_s32(mm, 1);
    return vget_lane_f32(vcvt_f32_s32(mm), 0);
}

/* (int) ceilf(f) for |f| < 2^24, without the library call */
static inline int ceil_i(float f) {
    int i = (int) f;
    return (float) i < f ? i + 1 : i;
}

static inline int ifloor(float f) {
    int i = (int) f;
    return (f < (float) i) ? i - 1 : i;
}

/* -filtro worked out in advance: cells per texel side (see fill_pf) */
#define PF_SHIFT 2
#define PF_SUB (1 << PF_SHIFT)

/* ---- per-triangle constants for the span loops ---- */
enum { WR_REPEAT2, WR_CLAMP, WR_MIRROR2, WR_REPEATN, WR_MIRRORN };

typedef struct {
    const uint32_t *tpx;
    int tw, th, umask, vmask, uwrap, vwrap;
    float ftw, fth;
    float bias;              /* -0.5 texel for filtered textures (see raster_tri) */
    int texenv, dtest, dwrite, fog, persp;
    uint32_t zoff;   /* added to every fixed-point depth (the winner's pane) */
    int fr, fg, fb, bs, bd;
    int r, g, b, a;          /* flat colour (GOUR == 0) */
    int orr, og, ob;         /* flat offset colour (NEON colour mode 5) */
    const void *nvs;         /* NvSteps of the NEON spans (per triangle) */
    int filt;                /* 3-point filter where the texture is magnified (-filtro) */
    const int16_t *ext;      /* the texture's visible extent per row (NULL: draw every texel) */
    const uint32_t *pf;      /* -filtro: the filter worked out in advance (2^pfs x 2^pfs cells per texel) */
    int pfs;
    const uint32_t *pf1;     /* the same with half as many cells per side (2^(pfs-1)), right after it */
    int32_t flim;            /* the exact filter up to this step per pixel (16.16 texels) */
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
    if (mode == WR_CLAMP) {
        /* already in range, decided on the float's bits (each float compare makes the
           Cortex-A7 wait for the VFP flags): -64 <= u <= n + 64 gives u back */
        const float hi = (float) n + 64.0f;
        uint32_t b, hb;
        memcpy(&b, &u, 4);
        memcpy(&hb, &hi, 4);
        if ((b & 0x80000000u) ? (b & 0x7FFFFFFFu) <= 0x42800000u : b <= hb)
            return u;
        return u < -64.0f ? -64.0f : (u > hi ? hi : u);
    }
    float period = (mode == WR_MIRROR2 || mode == WR_MIRRORN) ? 2.0f * n : (float) n;
    if (mode == WR_REPEAT2 || mode == WR_MIRROR2) {
        /* pow2 wrap only looks at the low bits: no folding needed while |u| < 16384
           (tested on the float's bits: no compare, so no wait for the VFP flags) */
        uint32_t b;
        memcpy(&b, &u, 4);
        if ((b & 0x7FFFFFFFu) < 0x46800000u)
            return u;
    }
    float q = u / period;
    float fq;
    if (q > -8388608.0f && q < 8388608.0f) {   /* floorf without the library call */
        int i = (int) q;
        fq = (float) ((float) i > q ? i - 1 : i);
    } else {
        fq = floorf(q);
    }
    return u - fq * period;
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
static inline __attribute__((always_inline)) void px_finish(uint16_t *cp, uint32_t *zp, uint32_t z, int r, int g, int b, int al,
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

/* Depth is stored as an unsigned integer: z (= 1/w, larger = nearer) x 2^28. Comparing
   integers keeps the per-pixel test out of the VFP (a float compare plus moving its flags
   to the ARM core costs a pipeline stall on the Cortex-A7 at every pixel). It is stepped
   in integers and recomputed from the exact float value every 16 pixels. */
#define ZSCALE 268435456.0f
static inline uint32_t zfix(float z) {
    const float t = z * ZSCALE;
#if defined(__arm__) && defined(__ARM_FP)
    /* the VFP conversion saturates (negative and NaN -> 0, too big -> 0xFFFFFFFF): no
       compare, so no wait for the VFP flags */
    float c;
    uint32_t r;
    __asm__("vcvt.u32.f32 %0, %1" : "=t"(c) : "t"(t));
    memcpy(&r, &c, 4);
#else
    uint32_t r;
    if (!(t > 0.0f))
        r = 0;
    else if (t >= 4294967296.0f)
        r = 0xFFFFFFFFu;
    else
        r = (uint32_t) t;
#endif
    return r > 0xFFFFFFF0u ? 0xFFFFFFF0u : r;
}

/* one horizontal span: pixels [xs, xe) of row py, attributes a[] at pixel xs */
static inline __attribute__((always_inline)) uint32_t span_body(const TriCtx *c, int py, int xs, int xe,
                                                                 const float *a, const float *ddx,
                                                                 const int KIND, const int TEX, const int GOUR) {
    /* TEX: 0 = untextured, 1 = textured (c->persp picks affine or perspective-correct) */
    const int W = port_render_w;
    uint16_t *restrict cp = port_color + py * W + xs;
    uint32_t *restrict zp = port_depth + py * W + xs;
    float zf = a[A_Z];
    const float dz = ddx[A_Z];
    const int32_t dzi = rnd_i(dz * ZSCALE);
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
    zi += (uint32_t) dzi;                                                                        \
    if (GOUR) { R += dR; G += dG; B += dB; A += dA; }                                            \
    if (GOUR == 2) { OR += dOR; OG += dOG; OB += dOB; F += dF; }

    if (TEX == 0) {
        for (int px = xs; px < xe; px += 16) {
            int len = xe - px < 16 ? xe - px : 16;
            uint32_t zi = zfix(zf) + c->zoff;
            for (int n = len; n > 0; n--, cp++, zp++) {
                if (!dtest || zi >= *zp) {
                    PX_COLOUR();
                    px_finish(cp, zp, zi, r, g, b, al, clamp8(F >> 16), c, dwrite, KIND, GOUR == 2);
                }
                PX_STEP();
            }
            zf += dz * len;
        }
        return (uint32_t) (xe - xs);
    }

    /* textured: runs of 16 pixels with linear texel coordinates (exact at each run start when
       perspective-correct), each drawn by the fastest loop that fits */
    const uint32_t *const tpx = c->tpx;
    const int tw = c->tw, th = c->th;
    float uz = 0, vz = 0, duz = 0, dvz = 0;
    int32_t U = 0, V = 0, dU = 0, dV = 0;
    const int persp = c->persp;
    if (!persp) {
        float u = tc_fold(a[A_U] * c->ftw + c->bias, tw, c->uwrap), v = tc_fold(a[A_V] * c->fth + c->bias, th, c->vwrap);
        /* 1:1 2D images land exactly on texel edges: round the step and start a hair (1/256
           texel) above the edge so every pixel picks the same texel, with no drift */
        U = rnd_i(u * 65536.0f) + 256; dU = rnd_i(ddx[A_U] * c->ftw * 65536.0f);
        V = rnd_i(v * 65536.0f) + 256; dV = rnd_i(ddx[A_V] * c->fth * 65536.0f);
    } else {
        uz = a[A_U]; vz = a[A_V];
        duz = ddx[A_U]; dvz = ddx[A_V];
    }
    for (int px = xs; px < xe; px += 16) {
        int len = xe - px < 16 ? xe - px : 16;
        uint32_t zi = zfix(zf) + c->zoff;
        if (persp) {
            float iz0 = 1.0f / zf;
            float u0 = uz * iz0 * c->ftw + c->bias, v0 = vz * iz0 * c->fth + c->bias;
            float zz1 = zf + dz * len;
            float iz1 = 1.0f / zz1;
            float u1 = (uz + duz * len) * iz1 * c->ftw + c->bias, v1 = (vz + dvz * len) * iz1 * c->fth + c->bias;
            float fu = tc_fold(u0, tw, c->uwrap), fv = tc_fold(v0, th, c->vwrap);
            u1 += fu - u0;
            v1 += fv - v0;
            float il = 1.0f / (float) len;
            U = rnd_i(fu * 65536.0f) + 256; dU = rnd_i((u1 - fu) * il * 65536.0f);
            V = rnd_i(fv * 65536.0f) + 256; dV = rnd_i((v1 - fv) * il * 65536.0f);
            uz += duz * len; vz += dvz * len;
        }
        uint32_t um, vm;
        if (axis_mask(U, dU, len, tw, c->uwrap, &um) && axis_mask(V, dV, len, th, c->vwrap, &vm)) {
#define FAST_LOOP(ENV)                                                                           \
            for (int n = len; n > 0; n--, cp++, zp++) {                                          \
                if (!dtest || zi >= *zp) {                                                       \
                    PX_COLOUR();                                                                 \
                    uint32_t tc = tpx[((uint32_t) (V >> 16) & vm) * tw + ((uint32_t) (U >> 16) & um)]; \
                    tex_combine(ENV, tc, &r, &g, &b, &al);                                       \
                    PX_OFFSET();                                                                 \
                    px_finish(cp, zp, zi, r, g, b, al, clamp8(F >> 16), c, dwrite, KIND, GOUR == 2); \
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
                if (!dtest || zi >= *zp) {
                    PX_COLOUR();
                    int iu = tc_wrap(U >> 16, tw, c->umask, c->uwrap);
                    int iv = tc_wrap(V >> 16, th, c->vmask, c->vwrap);
                    tex_combine(env, tpx[iv * tw + iu], &r, &g, &b, &al);
                    PX_OFFSET();
                    px_finish(cp, zp, zi, r, g, b, al, clamp8(F >> 16), c, dwrite, KIND, GOUR == 2);
                }
                PX_STEP();
                U += dU; V += dV;
            }
        }
        zf += dz * len;
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

/* ------------------------------------------------------------------------- */
/* NEON pixel pipeline: 8 pixels at a time, same result as span_body bit for bit.
 * Per run of 16 pixels the texels are fetched first (scalar, into a small array),
 * then each group of 8 goes through depth test, colour, texture combine, offset,
 * fog and the list's output step (store / alpha test / standard alpha blend) in
 * NEON registers. Used for every list and texenv; only translucent polygons with a
 * blend other than SRCALPHA / INVSRCALPHA stay on span_body.
 *   OUT: KIND_OP, KIND_PT, KIND_TR (standard blend)
 *   TEX: -1 untextured, else the texenv
 *   COL: 0 flat, 1 gouraud, 2 gouraud + offset, 3 gouraud + offset + fog,
 *        4 gouraud that only changes from row to row (flat along each span: the sky) */
/* ------------------------------------------------------------------------- */

/* 1 / n for a run of n pixels (the same value a division gives, without the division) */
static const float kInvLen[33] = {
    0.0f, 1.0f / 1, 1.0f / 2, 1.0f / 3, 1.0f / 4, 1.0f / 5, 1.0f / 6, 1.0f / 7, 1.0f / 8, 1.0f / 9, 1.0f / 10,
    1.0f / 11, 1.0f / 12, 1.0f / 13, 1.0f / 14, 1.0f / 15, 1.0f / 16, 1.0f / 17, 1.0f / 18, 1.0f / 19, 1.0f / 20,
    1.0f / 21, 1.0f / 22, 1.0f / 23, 1.0f / 24, 1.0f / 25, 1.0f / 26, 1.0f / 27, 1.0f / 28, 1.0f / 29, 1.0f / 30,
    1.0f / 31, 1.0f / 32,
};

/* extra "texenv" of the NEON spans: MODULATE by a black flat colour plus a flat offset
   colour (the front-end's way of drawing a texture's shape in one colour: the clouds, the
   speedometer...) is that offset colour with the texel's alpha */
#define NV_TEXA 4

/* exact DIV255 of a product (<= 255 * 255) */
static inline __attribute__((always_inline)) uint8x8_t nv_div255(uint16x8_t x) {
    return vrshrn_n_u16(vaddq_u16(x, vrshrq_n_u16(x, 8)), 8);
}
/* floor(x / 255), x <= 255 * 255 */
static inline __attribute__((always_inline)) uint8x8_t nv_floor255(uint16x8_t x) {
    const uint16x4_t k = vdup_n_u16(0x8081);
    uint16x4_t lo = vshrn_n_u32(vmull_u16(vget_low_u16(x), k), 16);
    uint16x4_t hi = vshrn_n_u32(vmull_u16(vget_high_u16(x), k), 16);
    return vshrn_n_u16(vcombine_u16(lo, hi), 7);
}
/* clamp8(X >> 16) for 8 lanes X + i * dX (o0 = {0..3} * dX, o1 = {4..7} * dX) */
static inline __attribute__((always_inline)) uint8x8_t nv_lane8(int32_t X, int32x4_t o0, int32x4_t o1) {
    int32x4_t v = vdupq_n_s32(X);
    return vqmovn_u16(vcombine_u16(vqshrun_n_s32(vaddq_s32(v, o0), 16), vqshrun_n_s32(vaddq_s32(v, o1), 16)));
}
static inline __attribute__((always_inline)) uint16x8_t nv_pack565(uint8x8_t r, uint8x8_t g, uint8x8_t b) {
    return vsriq_n_u16(vsriq_n_u16(vshll_n_u8(r, 8), vshll_n_u8(g, 8), 5), vshll_n_u8(b, 8), 11);
}
/* 0x00 / 0xFF bytes -> 0x0000 / 0xFFFF halfwords */
static inline __attribute__((always_inline)) uint16x8_t nv_wide_mask(uint8x8_t m) {
    return vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(m)));
}
static inline __attribute__((always_inline)) uint32x4_t nv_wide_mask32(uint16x4_t m) {
    return vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(m)));
}

typedef struct {
    int32x4_t o0[8], o1[8];   /* per-lane steps of R G B A OR OG OB F */
    uint32x4_t zo0, zo1;      /* per-lane depth steps */
    int32_t dX[8], dX8[8];    /* 1 and 8 steps of each channel */
    int32_t dzi;              /* depth step */
    uint32_t dz8;             /* 8 depth steps */
    uint32x4_t dz8v;          /* the same in all lanes */
} NvSteps;

static const int kNvAttr[8] = { A_R, A_G, A_B, A_A, A_OR, A_OG, A_OB, A_F };

/* the steps only depend on the triangle's gradients: built once per triangle */
static void nv_prepare(NvSteps *s, const float *ddx, int col) {
    const int nch = col == 0 || col == 5 ? 0 : (col == 1 || col == 4 ? 4 : 8);
    int32_t lo[4], hi[4];
    for (int k = 0; k < nch; k++) {
        int32_t d = (int32_t) (ddx[kNvAttr[k]] * 65536.0f);
        s->dX[k] = d;
        s->dX8[k] = (int32_t) ((uint32_t) d * 8);
        for (int i = 0; i < 4; i++) {
            lo[i] = (int32_t) ((uint32_t) i * (uint32_t) d);
            hi[i] = (int32_t) ((uint32_t) (i + 4) * (uint32_t) d);
        }
        s->o0[k] = vld1q_s32(lo);
        s->o1[k] = vld1q_s32(hi);
    }
    const int32_t dzi = rnd_i(ddx[A_Z] * ZSCALE);
    s->dzi = dzi;
    s->dz8 = (uint32_t) dzi * 8;
    s->dz8v = vdupq_n_u32(s->dz8);
    uint32_t zl[4], zh[4];
    for (int i = 0; i < 4; i++) {
        zl[i] = (uint32_t) i * (uint32_t) dzi;
        zh[i] = (uint32_t) (i + 4) * (uint32_t) dzi;
    }
    s->zo0 = vld1q_u32(zl);
    s->zo1 = vld1q_u32(zh);
}

/* the pixel pipeline in three stages (depth test, shading, output), so that two groups
   of 8 can be written side by side (nv_group2): the in-order Cortex-A7 then works on
   one group while the other waits for its results */
typedef struct {
    uint32x4_t z0, z1, d0, d1, m0, m1;
    uint16x8_t m;
} NvDepth;

static inline __attribute__((always_inline)) void nv_depth(NvDepth *D, const uint32_t *zp, uint32x4_t zv, const NvSteps *s,
                                                           const TriCtx *c, int n, const int OUT) {
    D->z0 = vaddq_u32(zv, s->zo0);
    D->z1 = vaddq_u32(zv, s->zo1);
    if (OUT == KIND_TR && !c->dtest) {
        /* translucent in front of everything: depth is neither tested nor written */
        D->d0 = D->z0;
        D->d1 = D->z1;
        D->m0 = D->m1 = vdupq_n_u32(0xFFFFFFFFu);
    } else {
        D->d0 = vld1q_u32(zp);
        D->d1 = vld1q_u32(zp + 4);
        if (c->dtest) {
            D->m0 = vcgeq_u32(D->z0, D->d0);
            D->m1 = vcgeq_u32(D->z1, D->d1);
        } else {
            D->m0 = D->m1 = vdupq_n_u32(0xFFFFFFFFu);
        }
    }
    D->m = vcombine_u16(vmovn_u32(D->m0), vmovn_u32(D->m1));
    if (n < 8) {
        /* last pixels of a span: lanes n..7 belong to the next pixels of the row, which
           are left exactly as they are (their old values are written back) */
        static const uint16_t kLane[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
        const uint16x8_t lm = vcltq_u16(vld1q_u16(kLane), vdupq_n_u16((uint16_t) (n < 0 ? 0 : n)));
        D->m = vandq_u16(D->m, lm);
        D->m0 = vandq_u32(D->m0, nv_wide_mask32(vget_low_u16(lm)));
        D->m1 = vandq_u32(D->m1, nv_wide_mask32(vget_high_u16(lm)));
    }
}

static inline __attribute__((always_inline)) void nv_shade(uint8x8_t *pr, uint8x8_t *pg, uint8x8_t *pb, uint8x8_t *pa,
                                                           const NvSteps *s, const int32_t *X, const uint32_t *tex,
                                                           const TriCtx *c, const uint8x8_t *flatc, const int TEX, const int COL) {
    /* vertex colour */
    uint8x8_t r, g, b, al;
    if (COL == 0 || COL == 4 || COL == 5) {
        r = flatc[0]; g = flatc[1]; b = flatc[2]; al = flatc[3];
    } else {
        r = nv_lane8(X[0], s->o0[0], s->o1[0]);
        g = nv_lane8(X[1], s->o0[1], s->o1[1]);
        b = nv_lane8(X[2], s->o0[2], s->o1[2]);
        al = nv_lane8(X[3], s->o0[3], s->o1[3]);
    }

    /* texture */
    if (TEX >= 0) {
        uint8x8x4_t t = vld4_u8((const uint8_t *) tex);   /* b g r a */
        if (TEX == GFX_TEXENV_REPLACE) {
            b = t.val[0]; g = t.val[1]; r = t.val[2]; al = t.val[3];
        } else if (TEX == GFX_TEXENV_MODULATE) {
            r = nv_div255(vmull_u8(r, t.val[2])); g = nv_div255(vmull_u8(g, t.val[1])); b = nv_div255(vmull_u8(b, t.val[0]));
            al = t.val[3];
        } else if (TEX == NV_TEXA) {
            al = t.val[3];   /* r g b: the flat colour (the offset colour, see raster_tri) */
        } else if (TEX == GFX_TEXENV_DECAL) {
            uint8x8_t ia = vmvn_u8(t.val[3]);
            r = nv_div255(vmlal_u8(vmull_u8(r, ia), t.val[2], t.val[3]));
            g = nv_div255(vmlal_u8(vmull_u8(g, ia), t.val[1], t.val[3]));
            b = nv_div255(vmlal_u8(vmull_u8(b, ia), t.val[0], t.val[3]));
        } else {
            r = nv_div255(vmull_u8(r, t.val[2])); g = nv_div255(vmull_u8(g, t.val[1])); b = nv_div255(vmull_u8(b, t.val[0]));
            al = nv_div255(vmull_u8(al, t.val[3]));
        }
        if (COL == 2 || COL == 3) {
            r = vqadd_u8(r, nv_lane8(X[4], s->o0[4], s->o1[4]));
            g = vqadd_u8(g, nv_lane8(X[5], s->o0[5], s->o1[5]));
            b = vqadd_u8(b, nv_lane8(X[6], s->o0[6], s->o1[6]));
        } else if (COL == 5) {
            r = vqadd_u8(r, flatc[4]);
            g = vqadd_u8(g, flatc[5]);
            b = vqadd_u8(b, flatc[6]);
        }
    }

    /* fog: c += (fogc - c) * f / 255, rounded towards zero */
    if (COL == 3) {
        uint8x8_t f = nv_lane8(X[7], s->o0[7], s->o1[7]);
        uint8x8_t fr = vdup_n_u8((uint8_t) c->fr), fg = vdup_n_u8((uint8_t) c->fg), fb = vdup_n_u8((uint8_t) c->fb);
        r = vsub_u8(vadd_u8(r, nv_floor255(vmull_u8(vqsub_u8(fr, r), f))), nv_floor255(vmull_u8(vqsub_u8(r, fr), f)));
        g = vsub_u8(vadd_u8(g, nv_floor255(vmull_u8(vqsub_u8(fg, g), f))), nv_floor255(vmull_u8(vqsub_u8(g, fg), f)));
        b = vsub_u8(vadd_u8(b, nv_floor255(vmull_u8(vqsub_u8(fb, b), f))), nv_floor255(vmull_u8(vqsub_u8(b, fb), f)));
    }
    *pr = r; *pg = g; *pb = b; *pa = al;
}

static inline __attribute__((always_inline)) void nv_out(uint16_t *cp, uint32_t *zp, const NvDepth *D, uint8x8_t r, uint8x8_t g,
                                                         uint8x8_t b, uint8x8_t al, const TriCtx *c, const int OUT) {
    uint16x8_t m = D->m;
    uint32x4_t m0 = D->m0, m1 = D->m1;
    uint16x8_t dst = vld1q_u16(cp);
    if (OUT == KIND_PT) {
        uint16x8_t am = nv_wide_mask(vcgt_u8(al, vdup_n_u8(0xC0)));
        m = vandq_u16(m, am);
        m0 = nv_wide_mask32(vget_low_u16(m));
        m1 = nv_wide_mask32(vget_high_u16(m));
    }
    if (OUT == KIND_TR) {
        /* standard alpha blend with the 565 pixel expanded as v * 255 / max */
        uint8x8_t d5r = vmovn_u16(vshrq_n_u16(dst, 11));
        uint8x8_t d6g = vmovn_u16(vandq_u16(vshrq_n_u16(dst, 5), vdupq_n_u16(0x3F)));
        uint8x8_t d5b = vmovn_u16(vandq_u16(dst, vdupq_n_u16(0x1F)));
        const uint8x8_t k58 = vdup_n_u8(58), k49 = vdup_n_u8(49);
        uint8x8_t dr = vadd_u8(vshl_n_u8(d5r, 3), vshrn_n_u16(vmull_u8(d5r, k58), 8));
        uint8x8_t db = vadd_u8(vshl_n_u8(d5b, 3), vshrn_n_u16(vmull_u8(d5b, k58), 8));
        uint8x8_t dg = vadd_u8(vshl_n_u8(d6g, 2), vshr_n_u8(vshrn_n_u16(vmull_u8(d6g, k49), 8), 2));
        uint8x8_t ia = vmvn_u8(al);
        r = nv_div255(vmlal_u8(vmull_u8(r, al), dr, ia));
        g = nv_div255(vmlal_u8(vmull_u8(g, al), dg, ia));
        b = nv_div255(vmlal_u8(vmull_u8(b, al), db, ia));
    }
    vst1q_u16(cp, vbslq_u16(m, nv_pack565(r, g, b), dst));
    if (OUT == KIND_PT && c->dwrite) {
        vst1q_u32(zp, vbslq_u32(m0, D->z0, D->d0));
        vst1q_u32(zp + 4, vbslq_u32(m1, D->z1, D->d1));
    }
}

/* opaque list: the new depth only depends on the depth test, so it is stored at once and
   the depth vectors do not stay alive through the shading */
static inline __attribute__((always_inline)) void nv_zstore(uint32_t *zp, const NvDepth *D, const TriCtx *c, const int OUT) {
    if (OUT == KIND_OP && c->dwrite) {
        vst1q_u32(zp, vbslq_u32(D->m0, D->z0, D->d0));
        vst1q_u32(zp + 4, vbslq_u32(D->m1, D->z1, D->d1));
    }
}

static inline __attribute__((always_inline)) int nv_none(uint16x8_t m) {
    return vget_lane_u64(vreinterpret_u64_u16(vorr_u16(vget_low_u16(m), vget_high_u16(m))), 0) == 0;
}

/* one group of 8 pixels; cp / zp point at 8 writable pixels */
static inline __attribute__((always_inline)) void nv_group(uint16_t *cp, uint32_t *zp, uint32x4_t zi, const NvSteps *s,
                                                           const int32_t *X, const uint32_t *tex, const TriCtx *c,
                                                           const uint8x8_t *flatc, int n, const int OUT, const int TEX, const int COL) {
    NvDepth D;
    nv_depth(&D, zp, zi, s, c, n, OUT);
    nv_zstore(zp, &D, c, OUT);
    if (nv_none(D.m))
        return;   /* all 8 hidden */
    uint8x8_t r, g, b, al;
    nv_shade(&r, &g, &b, &al, s, X, tex, c, flatc, TEX, COL);
    nv_out(cp, zp, &D, r, g, b, al, c, OUT);
}

/* two groups of 8 (16 pixels), written side by side */
static inline __attribute__((always_inline)) void nv_group2(uint16_t *cp, uint32_t *zp, uint32x4_t zi, const NvSteps *s,
                                                            const int32_t *X, const uint32_t *tex, const TriCtx *c,
                                                            const uint8x8_t *flatc, int n, const int OUT, const int TEX, const int COL) {
    /* n = pixels to draw of these 16 (9..16) */
    NvDepth A, B;
    /* opaque depth stored before the visibility test (with nothing visible it writes the
       same values back), one half after the other to keep the registers free */
    nv_depth(&A, zp, zi, s, c, 8, OUT);
    nv_zstore(zp, &A, c, OUT);
    nv_depth(&B, zp + 8, vaddq_u32(zi, s->dz8v), s, c, n - 8, OUT);
    nv_zstore(zp + 8, &B, c, OUT);
    if (nv_none(vorrq_u16(A.m, B.m)))
        return;   /* all 16 hidden */
    int32_t XB[8];
    const int nch = COL == 0 || COL == 5 ? 0 : (COL == 1 || COL == 4 ? 4 : 8);
    for (int k = 0; k < nch; k++)
        XB[k] = (int32_t) ((uint32_t) X[k] + (uint32_t) s->dX8[k]);
    uint8x8_t ra, ga, ba, aa, rb, gb, bb, ab;
    nv_shade(&ra, &ga, &ba, &aa, s, X, tex, c, flatc, TEX, COL);
    nv_shade(&rb, &gb, &bb, &ab, s, XB, tex + 8, c, flatc, TEX, COL);
    nv_out(cp, zp, &A, ra, ga, ba, aa, c, OUT);
    nv_out(cp + 8, zp + 8, &B, rb, gb, bb, ab, c, OUT);
}

/* the 3-point blend of one channel set; written out per channel: with a loop over .val[k]
   GCC 8 keeps the vector structs in memory and goes through the stack for every channel */
#define NV_BLEND1(r, a, b, cc, w0, s8, t8, k) \
    (r).val[k] = vrshrn_n_u16(vmlal_u8(vmlal_u8(vmull_u8((a).val[k], (w0)), (b).val[k], (s8)), (cc).val[k], (t8)), 7)
#define NV_BLEND3(r, a, b, cc, w0, s8, t8) do { \
        NV_BLEND1(r, a, b, cc, w0, s8, t8, 0); NV_BLEND1(r, a, b, cc, w0, s8, t8, 1); \
        NV_BLEND1(r, a, b, cc, w0, s8, t8, 2); NV_BLEND1(r, a, b, cc, w0, s8, t8, 3); \
    } while (0)

/* N64-style 3-point texture filter for a run of up to 16 pixels (-filtro). Each pixel
   blends the texel at its corner with its two neighbours along s and t, on the side of
   the texel square's diagonal it falls on, as the N64 RDP does. Weights in 1/128.
   Used only where the texture is magnified (less than one texel per pixel step), with
   repeat (power-of-two) or clamp wrapping on both axes. */
#ifdef GFX_SW_FILTER_REF
static void nv_fetch3_ref(uint32_t *out, int len, int32_t U, int32_t dU, int32_t V, int32_t dV, const TriCtx *c) {
    uint32_t A[16] __attribute__((aligned(16))), B[16] __attribute__((aligned(16))), C[16] __attribute__((aligned(16)));
    uint8_t ws[16] __attribute__((aligned(8))), wt[16] __attribute__((aligned(8)));
    const uint32_t *tpx = c->tpx;
    const int tw = c->tw, th = c->th;
    const int ur = c->uwrap == WR_REPEAT2, vr = c->vwrap == WR_REPEAT2;
    U -= 32768 + 256;   /* texel centres (and without the 1/256 nudge of the point sampler) */
    V -= 32768 + 256;
    for (int i = 0; i < len; i++) {
        int iu = U >> 16, iv = V >> 16;
        int fs = (U >> 9) & 127, ft = (V >> 9) & 127;
        int u0, u1, v0, v1;
        if (ur) {
            u0 = iu & (tw - 1);
            u1 = (iu + 1) & (tw - 1);
        } else {
            u0 = iu < 0 ? 0 : (iu >= tw ? tw - 1 : iu);
            u1 = iu + 1 < 0 ? 0 : (iu + 1 >= tw ? tw - 1 : iu + 1);
        }
        if (vr) {
            v0 = iv & (th - 1);
            v1 = (iv + 1) & (th - 1);
        } else {
            v0 = iv < 0 ? 0 : (iv >= th ? th - 1 : iv);
            v1 = iv + 1 < 0 ? 0 : (iv + 1 >= th ? th - 1 : iv + 1);
        }
        const uint32_t *r0 = tpx + v0 * tw, *r1 = tpx + v1 * tw;
        if (fs + ft <= 128) {
            A[i] = r0[u0]; B[i] = r0[u1]; C[i] = r1[u0];
            ws[i] = (uint8_t) fs; wt[i] = (uint8_t) ft;
        } else {
            A[i] = r1[u1]; B[i] = r1[u0]; C[i] = r0[u1];
            ws[i] = (uint8_t) (128 - fs); wt[i] = (uint8_t) (128 - ft);
        }
        U = (int32_t) ((uint32_t) U + (uint32_t) dU);
        V = (int32_t) ((uint32_t) V + (uint32_t) dV);
    }
    for (int o = 0; o < len; o += 8) {
        uint8x8x4_t a = vld4_u8((const uint8_t *) (A + o)), b = vld4_u8((const uint8_t *) (B + o)),
                    cc = vld4_u8((const uint8_t *) (C + o)), r;
        uint8x8_t s8 = vld1_u8(ws + o), t8 = vld1_u8(wt + o);
        uint8x8_t w0 = vsub_u8(vsub_u8(vdup_n_u8(128), s8), t8);
        NV_BLEND3(r, a, b, cc, w0, s8, t8);
        vst4_u8((uint8_t *) (out + o), r);
    }
}
#endif

static void nv_fetch3_32(uint32_t *out, int len, int32_t U, int32_t dU, int32_t V, int32_t dV, const TriCtx *c) {
    uint32_t ia[8] __attribute__((aligned(16))), ib[8] __attribute__((aligned(16))), ic[8] __attribute__((aligned(16)));
    uint32_t A[8] __attribute__((aligned(16))), B[8] __attribute__((aligned(16))), C[8] __attribute__((aligned(16)));
    static const int32_t kLane[4] = { 0, 1, 2, 3 };
    const uint32_t *tpx = c->tpx;
    const int tw = c->tw, th = c->th;
    const int ur = c->uwrap == WR_REPEAT2, vr = c->vwrap == WR_REPEAT2;
    const int32x4_t lane = vld1q_s32(kLane), k127 = vdupq_n_s32(127), k128 = vdupq_n_s32(128), one = vdupq_n_s32(1),
                    zero = vdupq_n_s32(0), umax = vdupq_n_s32(tw - 1), vmax = vdupq_n_s32(th - 1);
    U -= 32768 + 256;   /* texel centres (and without the 1/256 nudge of the point sampler) */
    V -= 32768 + 256;
    for (int o = 0; o < len; o += 8) {
        uint16x4_t sh[2], th4[2];
        for (int h = 0; h < 2; h++) {
            const int32_t k = o + 4 * h;
            const int32x4_t u = vmlaq_n_s32(vdupq_n_s32((int32_t) ((uint32_t) U + (uint32_t) dU * (uint32_t) k)), lane, dU);
            const int32x4_t v = vmlaq_n_s32(vdupq_n_s32((int32_t) ((uint32_t) V + (uint32_t) dV * (uint32_t) k)), lane, dV);
            const int32x4_t iu = vshrq_n_s32(u, 16), iv = vshrq_n_s32(v, 16);
            const int32x4_t fs = vandq_s32(vshrq_n_s32(u, 9), k127), ft = vandq_s32(vshrq_n_s32(v, 9), k127);
            int32x4_t u0, u1, v0, v1;
            if (ur) {
                u0 = vandq_s32(iu, umax);
                u1 = vandq_s32(vaddq_s32(iu, one), umax);
            } else {
                u0 = vmaxq_s32(vminq_s32(iu, umax), zero);
                u1 = vmaxq_s32(vminq_s32(vaddq_s32(iu, one), umax), zero);
            }
            if (vr) {
                v0 = vandq_s32(iv, vmax);
                v1 = vandq_s32(vaddq_s32(iv, one), vmax);
            } else {
                v0 = vmaxq_s32(vminq_s32(iv, vmax), zero);
                v1 = vmaxq_s32(vminq_s32(vaddq_s32(iv, one), vmax), zero);
            }
            const int32x4_t r0 = vmulq_n_s32(v0, tw), r1 = vmulq_n_s32(v1, tw);
            /* far side of the diagonal (s + t > 1): the opposite corner and 1 - s, 1 - t */
            const uint32x4_t far = vcgtq_s32(vaddq_s32(fs, ft), k128);
            vst1q_s32((int32_t *) ia + 4 * h, vbslq_s32(far, vaddq_s32(r1, u1), vaddq_s32(r0, u0)));
            vst1q_s32((int32_t *) ib + 4 * h, vbslq_s32(far, vaddq_s32(r1, u0), vaddq_s32(r0, u1)));
            vst1q_s32((int32_t *) ic + 4 * h, vbslq_s32(far, vaddq_s32(r0, u1), vaddq_s32(r1, u0)));
            sh[h] = vmovn_u32(vreinterpretq_u32_s32(vbslq_s32(far, vsubq_s32(k128, fs), fs)));
            th4[h] = vmovn_u32(vreinterpretq_u32_s32(vbslq_s32(far, vsubq_s32(k128, ft), ft)));
        }
        for (int i = 0; i < 8; i++) {
            const uint32_t a = tpx[ia[i]], b = tpx[ib[i]], cc = tpx[ic[i]];
            A[i] = a; B[i] = b; C[i] = cc;
        }
        uint8x8x4_t a = vld4_u8((const uint8_t *) A), b = vld4_u8((const uint8_t *) B), cc = vld4_u8((const uint8_t *) C), r;
        const uint8x8_t s8 = vmovn_u16(vcombine_u16(sh[0], sh[1])), t8 = vmovn_u16(vcombine_u16(th4[0], th4[1]));
        const uint8x8_t w0 = vsub_u8(vsub_u8(vdup_n_u8(128), s8), t8);
        NV_BLEND3(r, a, b, cc, w0, s8, t8);
        vst4_u8((uint8_t *) (out + o), r);
    }
}

/* the same with the texel indices and weights in 16-bit lanes (8 pixels per instruction):
   textures of up to 65536 texels; U >> 16 always fits 16 bits */
static void nv_fetch3(uint32_t *out, int len, int32_t U, int32_t dU, int32_t V, int32_t dV, const TriCtx *c) {
    const int tw = c->tw, th = c->th;
    if (tw * th > 65536) {
        nv_fetch3_32(out, len, U, dU, V, dV, c);
        return;
    }
    uint16_t ia[8] __attribute__((aligned(16))), ib[8] __attribute__((aligned(16))), ic[8] __attribute__((aligned(16)));
    uint32_t A[8] __attribute__((aligned(16))), B[8] __attribute__((aligned(16))), C[8] __attribute__((aligned(16)));
    static const int32_t kLane[4] = { 0, 1, 2, 3 };
    const uint32_t *tpx = c->tpx;
    const int ur = c->uwrap == WR_REPEAT2, vr = c->vwrap == WR_REPEAT2;
    const int32x4_t lane = vld1q_s32(kLane);
    const int16x8_t k127 = vdupq_n_s16(127), k128 = vdupq_n_s16(128), one = vdupq_n_s16(1), zero = vdupq_n_s16(0),
                    umax = vdupq_n_s16((int16_t) (tw - 1)), vmax = vdupq_n_s16((int16_t) (th - 1));
    U -= 32768 + 256;   /* texel centres (and without the 1/256 nudge of the point sampler) */
    V -= 32768 + 256;
    for (int o = 0; o < len; o += 8) {
        const int32_t U0 = (int32_t) ((uint32_t) U + (uint32_t) dU * (uint32_t) o);
        const int32_t V0 = (int32_t) ((uint32_t) V + (uint32_t) dV * (uint32_t) o);
        const int32x4_t ua = vmlaq_n_s32(vdupq_n_s32(U0), lane, dU);
        const int32x4_t ub = vaddq_s32(ua, vdupq_n_s32((int32_t) ((uint32_t) dU * 4)));
        const int32x4_t va = vmlaq_n_s32(vdupq_n_s32(V0), lane, dV);
        const int32x4_t vb = vaddq_s32(va, vdupq_n_s32((int32_t) ((uint32_t) dV * 4)));
        const int16x8_t iu = vcombine_s16(vshrn_n_s32(ua, 16), vshrn_n_s32(ub, 16));
        const int16x8_t iv = vcombine_s16(vshrn_n_s32(va, 16), vshrn_n_s32(vb, 16));
        const int16x8_t fs = vandq_s16(vcombine_s16(vshrn_n_s32(ua, 9), vshrn_n_s32(ub, 9)), k127);
        const int16x8_t ft = vandq_s16(vcombine_s16(vshrn_n_s32(va, 9), vshrn_n_s32(vb, 9)), k127);
        int16x8_t u0, u1, v0, v1;
        if (ur) {
            u0 = vandq_s16(iu, umax);
            u1 = vandq_s16(vaddq_s16(iu, one), umax);
        } else {
            u0 = vmaxq_s16(vminq_s16(iu, umax), zero);
            u1 = vmaxq_s16(vminq_s16(vaddq_s16(iu, one), umax), zero);
        }
        if (vr) {
            v0 = vandq_s16(iv, vmax);
            v1 = vandq_s16(vaddq_s16(iv, one), vmax);
        } else {
            v0 = vmaxq_s16(vminq_s16(iv, vmax), zero);
            v1 = vmaxq_s16(vminq_s16(vaddq_s16(iv, one), vmax), zero);
        }
        const uint16x8_t r0 = vmulq_n_u16(vreinterpretq_u16_s16(v0), (uint16_t) tw);
        const uint16x8_t r1 = vmulq_n_u16(vreinterpretq_u16_s16(v1), (uint16_t) tw);
        const uint16x8_t U0v = vreinterpretq_u16_s16(u0), U1v = vreinterpretq_u16_s16(u1);
        /* far side of the diagonal (s + t > 1): the opposite corner and 1 - s, 1 - t */
        const uint16x8_t far = vcgtq_s16(vaddq_s16(fs, ft), k128);
        vst1q_u16(ia, vbslq_u16(far, vaddq_u16(r1, U1v), vaddq_u16(r0, U0v)));
        vst1q_u16(ib, vbslq_u16(far, vaddq_u16(r1, U0v), vaddq_u16(r0, U1v)));
        vst1q_u16(ic, vbslq_u16(far, vaddq_u16(r0, U1v), vaddq_u16(r1, U0v)));
        const uint8x8_t s8 = vmovn_u16(vbslq_u16(far, vreinterpretq_u16_s16(vsubq_s16(k128, fs)), vreinterpretq_u16_s16(fs)));
        const uint8x8_t t8 = vmovn_u16(vbslq_u16(far, vreinterpretq_u16_s16(vsubq_s16(k128, ft)), vreinterpretq_u16_s16(ft)));
        for (int i = 0; i < 8; i++) {
            const uint32_t a = tpx[ia[i]], b = tpx[ib[i]], cc = tpx[ic[i]];
            A[i] = a; B[i] = b; C[i] = cc;
        }
        uint8x8x4_t a = vld4_u8((const uint8_t *) A), b = vld4_u8((const uint8_t *) B), cc = vld4_u8((const uint8_t *) C), r;
        const uint8x8_t w0 = vsub_u8(vsub_u8(vdup_n_u8(128), s8), t8);
        NV_BLEND3(r, a, b, cc, w0, s8, t8);
        vst4_u8((uint8_t *) (out + o), r);
    }
}

/* ---- texel fetch along a run (no wrap inside it, or power-of-two wrap by mask) ----
 * tex[i] = tpx[((V >> 16) & vm) * tw + ((U >> 16) & um)], U += dU, V += dV, for i < n.
 * Inside nv_span the compiler runs out of registers and reloads its variables from the
 * stack in this loop; written by hand it keeps everything in registers and starts both
 * texel loads before using either (the Cortex-A7 waits 3 cycles for a load). */
typedef struct { int32_t U, V, dU, dV; uint32_t um, vm; int32_t tw; } TexWalk;

static void fetch_texels_c(uint32_t *tex, const uint32_t *tpx, int n, TexWalk *w) {
    uint32_t U = (uint32_t) w->U, V = (uint32_t) w->V;
    for (int i = 0; i < n; i++) {
        tex[i] = tpx[((uint32_t) ((int32_t) V >> 16) & w->vm) * (uint32_t) w->tw + ((uint32_t) ((int32_t) U >> 16) & w->um)];
        U += (uint32_t) w->dU;
        V += (uint32_t) w->dV;
    }
    w->U = (int32_t) U;
    w->V = (int32_t) V;
}

#if defined(__arm__) && !defined(__thumb__) && !defined(GFX_SW_NO_ASM)
#define HAVE_FETCH_ASM 1
/* r0 tex, r1 tpx, r2 n (>= 1), r3 TexWalk */
void sw_fetch_texels(uint32_t *tex, const uint32_t *tpx, int n, TexWalk *w);
__asm__(
    "    .text\n"
    "    .align  2\n"
    "    .global sw_fetch_texels\n"
    "    .type   sw_fetch_texels, %function\n"
    "    .arm\n"
    "sw_fetch_texels:\n"
    "    push    {r4-r11, lr}\n"
    "    ldm     r3, {r4-r10}\n"              /* r4 U, r5 V, r6 dU, r7 dV, r8 um, r9 vm, r10 tw */
    "    subs    r2, r2, #2\n"
    "    blt     2f\n"
    "1:  and     r11, r8, r4, asr #16\n"      /* two texels per pass */
    "    and     r12, r9, r5, asr #16\n"
    "    add     r4, r4, r6\n"
    "    add     r5, r5, r7\n"
    "    and     lr, r8, r4, asr #16\n"
    "    mla     r11, r12, r10, r11\n"
    "    and     r12, r9, r5, asr #16\n"
    "    add     r4, r4, r6\n"
    "    mla     lr, r12, r10, lr\n"
    "    add     r5, r5, r7\n"
    "    ldr     r11, [r1, r11, lsl #2]\n"
    "    subs    r2, r2, #2\n"
    "    ldr     lr, [r1, lr, lsl #2]\n"
    "    str     r11, [r0], #4\n"
    "    str     lr, [r0], #4\n"
    "    bge     1b\n"
    "2:  adds    r2, r2, #2\n"                /* the odd one */
    "    beq     3f\n"
    "    and     r11, r8, r4, asr #16\n"
    "    and     r12, r9, r5, asr #16\n"
    "    add     r4, r4, r6\n"
    "    add     r5, r5, r7\n"
    "    mla     r11, r12, r10, r11\n"
    "    ldr     r11, [r1, r11, lsl #2]\n"
    "    str     r11, [r0]\n"
    "3:  stm     r3, {r4, r5}\n"
    "    pop     {r4-r11, pc}\n"
    "    .size   sw_fetch_texels, .-sw_fetch_texels\n");

/* the hand-written loop is checked against the C one before it is used */
static int sFetchAsm = -1;
static int fetch_asm_selftest(void) {
    static uint32_t tpx[64 * 32], a[40], b[40];
    uint32_t r = 12345;
    for (int i = 0; i < 64 * 32; i++) { r = r * 1103515245u + 12345u; tpx[i] = r; }
    for (int t = 0; t < 4000; t++) {
        r = r * 1103515245u + 12345u;
        const int n = 1 + (int) (r % 37);
        const int tw = (r >> 8) & 1 ? 64 : 61;
        const int wrap = (r >> 9) & 1;
        TexWalk w1, w2;
        w1.um = wrap ? 63u : 0xFFFFFFFFu;
        w1.vm = wrap ? 31u : 0xFFFFFFFFu;
        w1.tw = wrap ? 64 : tw;
        r = r * 1103515245u + 12345u;
        w1.dU = (int32_t) (r % 131072u) - 65536;
        r = r * 1103515245u + 12345u;
        w1.dV = (int32_t) (r % 65536u) - 32768;
        /* starts that stay inside the texture when there is no wrap */
        w1.U = (int32_t) ((wrap ? (r % 5000000u) : 20u << 16) + 0x8000u);
        w1.V = (int32_t) ((wrap ? (r % 3000000u) : 15u << 16) + 0x100u);
        if (!wrap) { w1.dU /= 4; w1.dV /= 8; }
        w2 = w1;
        memset(a, 0, sizeof(a));
        memset(b, 0, sizeof(b));
        fetch_texels_c(a, tpx, n, &w1);
        sw_fetch_texels(b, tpx, n, &w2);
        if (memcmp(a, b, sizeof(a)) || w1.U != w2.U || w1.V != w2.V)
            return 0;
    }
    return 1;
}
static inline void fetch_texels(uint32_t *tex, const uint32_t *tpx, int n, TexWalk *w) {
    if (sFetchAsm > 0)
        sw_fetch_texels(tex, tpx, n, w);
    else
        fetch_texels_c(tex, tpx, n, w);
}
#else
static inline void fetch_texels(uint32_t *tex, const uint32_t *tpx, int n, TexWalk *w) {
    fetch_texels_c(tex, tpx, n, w);
}
#endif

/* 16 fetched texels that all leave the picture as it is: alpha 0 (translucent) or
   alpha <= 0xC0 (punch-through test). The largest texel word has the largest alpha. */
static inline __attribute__((always_inline)) int nv_invisible16(const uint32_t *tex, int pt) {
    const uint32x4_t m = vmaxq_u32(vmaxq_u32(vld1q_u32(tex), vld1q_u32(tex + 4)),
                                   vmaxq_u32(vld1q_u32(tex + 8), vld1q_u32(tex + 12)));
    uint32x2_t h = vpmax_u32(vget_low_u32(m), vget_high_u32(m));
    h = vpmax_u32(h, h);
    return vget_lane_u32(h, 0) < (pt ? 0xC1000000u : 0x01000000u);
}

/* An affine translucent / punch-through span along one texture row (dV = 0): which of its
   n pixels can show a visible texel. The texel index (U + i dU) >> 16 is monotonic in i,
   so the pixels before *first and from *end on only show invisible texels (alpha 0, or
   failing the punch-through test) and leave the picture as it is.
   1: [*first, *end) to draw, 0: nothing, -1: unknown (the row wraps or clamps) */
static int nv_visible(const TriCtx *c, int pt, int32_t U, int32_t dU, int32_t V, int n, int *first, int *end) {
    const int v = V >> 16;
    const int64_t u0 = U, u1 = (int64_t) U + (int64_t) dU * (n - 1);
    if (v < 0 || v >= c->th || u0 < 0 || u1 < 0 || (u0 >> 16) >= c->tw || (u1 >> 16) >= c->tw)
        return -1;
    const int16_t *e = c->ext + 4 * v + (pt ? 2 : 0);
    const int lo = e[0], hi = e[1];
    if (lo > hi)
        return 0;
    int a = 0, b = n;
    if (dU >= 0) {   /* index going up: first with index >= lo, then first with index > hi */
        while (a < b) {
            const int m = (a + b) >> 1;
            if ((((int64_t) U + (int64_t) dU * m) >> 16) >= lo) b = m; else a = m + 1;
        }
        *first = a;
        b = n;
        while (a < b) {
            const int m = (a + b) >> 1;
            if ((((int64_t) U + (int64_t) dU * m) >> 16) > hi) b = m; else a = m + 1;
        }
    } else {         /* going down: first with index <= hi, then first with index < lo */
        while (a < b) {
            const int m = (a + b) >> 1;
            if ((((int64_t) U + (int64_t) dU * m) >> 16) <= hi) b = m; else a = m + 1;
        }
        *first = a;
        b = n;
        while (a < b) {
            const int m = (a + b) >> 1;
            if ((((int64_t) U + (int64_t) dU * m) >> 16) < lo) b = m; else a = m + 1;
        }
    }
    *end = a;
    return *first < *end;
}

/* -filtro, strongly magnified: 8 pixels in two groups of 4, blended in a straight line
   from texel colour a towards b, then from b towards c (rounded) */
static inline __attribute__((always_inline)) void nv_lerp8(uint32_t *out, const uint32_t *a, const uint32_t *b, const uint32_t *c4) {
    static const uint8_t kF4[8] = { 0, 1, 2, 3, 0, 1, 2, 3 }, kHi[8] = { 0, 0, 0, 0, 255, 255, 255, 255 };
    const uint8x8x4_t A = vld4_dup_u8((const uint8_t *) a), B = vld4_dup_u8((const uint8_t *) b), C = vld4_dup_u8((const uint8_t *) c4);
    const uint8x8_t f = vld1_u8(kF4), w = vsub_u8(vdup_n_u8(4), f), hi = vld1_u8(kHi);
    uint8x8x4_t R;
    R.val[0] = vrshrn_n_u16(vmlal_u8(vmull_u8(vbsl_u8(hi, B.val[0], A.val[0]), w), vbsl_u8(hi, C.val[0], B.val[0]), f), 2);
    R.val[1] = vrshrn_n_u16(vmlal_u8(vmull_u8(vbsl_u8(hi, B.val[1], A.val[1]), w), vbsl_u8(hi, C.val[1], B.val[1]), f), 2);
    R.val[2] = vrshrn_n_u16(vmlal_u8(vmull_u8(vbsl_u8(hi, B.val[2], A.val[2]), w), vbsl_u8(hi, C.val[2], B.val[2]), f), 2);
    R.val[3] = vrshrn_n_u16(vmlal_u8(vmull_u8(vbsl_u8(hi, B.val[3], A.val[3]), w), vbsl_u8(hi, C.val[3], B.val[3]), f), 2);
    vst4_u8((uint8_t *) out, R);
}

/* -filtro in the span: the opaque list, and the translucent list's modulated textures (the
   ice of Sherbet Land, see raster_tri); the other translucent spans do not get the code */
/* (-qualidade: every list, see raster_tri) */
#define FILT_SPAN(OUT, TEX) ((OUT) == KIND_OP || (OUT) == KIND_PT || (OUT) == KIND_TR)
static inline __attribute__((always_inline)) uint32_t nv_span(const TriCtx *c, int py, int xs, int xe, const float *a,
                                                               const float *ddx, const int OUT, const int TEX, const int COL) {
    const int W = port_render_w;
    uint16_t *cp = port_color + py * W + xs;
    uint32_t *zp = port_depth + py * W + xs;
    float zf = a[A_Z];
    const float dz = ddx[A_Z];
    const NvSteps *const s = (const NvSteps *) c->nvs;
    const int32_t dzi = s->dzi;
    int32_t X[8];
    const int nch = COL == 0 || COL == 5 ? 0 : (COL == 1 || COL == 4 ? 4 : 8);
    for (int k = 0; k < nch; k++)
        X[k] = (int32_t) (a[kNvAttr[k]] * 65536.0f);
    uint8x8_t flatc[7];
    if (COL == 0 || COL == 5) {
        flatc[0] = vdup_n_u8((uint8_t) c->r); flatc[1] = vdup_n_u8((uint8_t) c->g);
        flatc[2] = vdup_n_u8((uint8_t) c->b); flatc[3] = vdup_n_u8((uint8_t) c->a);
        if (COL == 5) {
            flatc[4] = vdup_n_u8((uint8_t) c->orr); flatc[5] = vdup_n_u8((uint8_t) c->og);
            flatc[6] = vdup_n_u8((uint8_t) c->ob);
        }
    } else if (COL == 4) {
        for (int k = 0; k < 4; k++)
            flatc[k] = vdup_n_u8((uint8_t) clamp8(X[k] >> 16));
    }

    uint32_t tex[16] __attribute__((aligned(16)));
    const uint32_t *const tpx = c->tpx;
    const int tw = c->tw, th = c->th;
    float uz = 0, vz = 0, duz = 0, dvz = 0;
    int32_t U = 0, V = 0, dU = 0, dV = 0;
    const int persp = TEX >= 0 && c->persp;
    if (TEX >= 0) {
        if (!persp) {
            float u = tc_fold(a[A_U] * c->ftw + c->bias, tw, c->uwrap), v = tc_fold(a[A_V] * c->fth + c->bias, th, c->vwrap);
            U = rnd_i(u * 65536.0f) + 256; dU = rnd_i(ddx[A_U] * c->ftw * 65536.0f);
            V = rnd_i(v * 65536.0f) + 256; dV = rnd_i(ddx[A_V] * c->fth * 65536.0f);
        } else {
            uz = a[A_U]; vz = a[A_V];
            duz = ddx[A_U]; dvz = ddx[A_V];
        }
    }

    /* translucent / punch-through span along one texture row: the 16-pixel runs at the start
       and the pixels at the end that only show invisible texels are left out (the runs keep
       their places, so every drawn pixel gets exactly the values it got before) */
    const int n0 = xe - xs;
    int px0 = xs;
    /* (the span may also slant a little across the texture, as long as it stays on one texel row) */
    if ((OUT == KIND_TR || OUT == KIND_PT) && TEX >= 0 && TEX != GFX_TEXENV_DECAL && !persp && c->ext &&
        (V >> 16) == (int32_t) (((int64_t) V + (int64_t) dV * (n0 - 1)) >> 16)) {
        int first = 0, end = n0;
        const int vis = nv_visible(c, OUT == KIND_PT, U, dU, V, n0, &first, &end);
        if (vis == 0)
            return (uint32_t) n0;
        if (vis > 0) {
            const int skip = first >> 4;
            xe = xs + end;
            for (int r = 0; r < skip; r++)
                zf += dz * 16;
            U = (int32_t) ((uint32_t) U + (uint32_t) dU * (uint32_t) (16 * skip));
            V = (int32_t) ((uint32_t) V + (uint32_t) dV * (uint32_t) (16 * skip));
            for (int k = 0; k < nch; k++)
                X[k] = (int32_t) ((uint32_t) X[k] + (uint32_t) s->dX[k] * (uint32_t) (16 * skip));
            cp += 16 * skip;
            zp += 16 * skip;
            px0 = xs + 16 * skip;
        }
    }

    /* perspective: the end of one run is the start of the next (same float operations),
       so its reciprocal and texel coordinates are kept instead of recomputed */
    float pu0 = 0, pv0 = 0, pz = -1.0f;
    /* the exact perspective every 16 pixels, or (at 640x480, unless -exato) every 32 like
       Quake at 320x200: linear in between */
    const int PSTEP = W >= 640 && !port_opt_exact ? 32 : 16;
    int prun_end = xs;
    for (int px = px0; px < xe; px += 16) {
        const int len = xe - px < 16 ? xe - px : 16;
        /* depth of the run's first pixel, made and kept in NEON (zfix's result without a
           move to the integer registers) */
        const uint32x4_t zi = vaddq_u32(vminq_u32(vcvtq_u32_f32(vdupq_n_f32(zf * ZSCALE)), vdupq_n_u32(0xFFFFFFF0u)),
                                        vdupq_n_u32(c->zoff));
        /* the depth and colour rows do not fit in the cache: ask for them ahead */
        __builtin_prefetch(zp + 64);
        __builtin_prefetch(cp + 64);
        __builtin_prefetch(cp + 96);
        if (TEX >= 0) {
            if (persp && px == prun_end) {
                const int plen = xe - px < PSTEP ? xe - px : PSTEP;
                float u0, v0;
                uint32_t pzb, zfb;   /* same bits: the same float, known without a VFP compare */
                memcpy(&pzb, &pz, 4);
                memcpy(&zfb, &zf, 4);
                if (pzb == zfb || pz == zf) {
                    u0 = pu0; v0 = pv0;
                } else {
                    float iz0 = 1.0f / zf;
                    u0 = uz * iz0 * c->ftw + c->bias; v0 = vz * iz0 * c->fth + c->bias;
                }
                /* the run's end, stepped exactly like zf, uz, vz are stepped per 16 pixels */
                float zz1 = zf, uz1 = uz, vz1 = vz;
                for (int q = 0; q < plen; q += 16) {
                    const int cl = plen - q < 16 ? plen - q : 16;
                    zz1 += dz * cl; uz1 += duz * cl; vz1 += dvz * cl;
                }
                float iz1 = 1.0f / zz1;
                float u1 = uz1 * iz1 * c->ftw + c->bias, v1 = vz1 * iz1 * c->fth + c->bias;
                pu0 = u1; pv0 = v1; pz = zz1;
                float fu = tc_fold(u0, tw, c->uwrap), fv = tc_fold(v0, th, c->vwrap);
                u1 += fu - u0;
                v1 += fv - v0;
                const float il = kInvLen[plen];
                U = rnd_i(fu * 65536.0f) + 256; dU = rnd_i((u1 - fu) * il * 65536.0f);
                V = rnd_i(fv * 65536.0f) + 256; dV = rnd_i((v1 - fv) * il * 65536.0f);
                uz = uz1; vz = vz1;
                prun_end = px + plen;
            }
            uint32_t um, vm;
            if (FILT_SPAN(OUT, TEX) && c->filt && !port_opt_exact && !(port_opt_filter_lite && c->pf) && dU > -8192 && dU < 8192 && dV > -8192 && dV < 8192) {
                /* 8 or more pixels per texel (the road right in front): the filter changes
                   slowly and linearly along the run, so it is computed exactly every 4 pixels
                   and the pixels between are blended from those (the filter worked out in
                   advance would show its cells as bands here) */
                uint32_t smp[16] __attribute__((aligned(16)));
                nv_fetch3(smp, 5, U, (int32_t) ((uint32_t) dU * 4), V, (int32_t) ((uint32_t) dV * 4), c);
                nv_lerp8(tex, &smp[0], &smp[1], &smp[2]);
                nv_lerp8(tex + 8, &smp[2], &smp[3], &smp[4]);
                U = (int32_t) ((uint32_t) U + (uint32_t) dU * (uint32_t) len);
                V = (int32_t) ((uint32_t) V + (uint32_t) dV * (uint32_t) len);
            } else if (FILT_SPAN(OUT, TEX) && c->filt && dU > -65536 && dU < 65536 && dV > -65536 && dV < 65536 && c->pf) {
                /* the filter worked out in advance: one cell per pixel (positions relative to
                   the texel centres, as in nv_fetch3; repeat wraps on the cell grid as well) */
                /* which of the two: the finer one only where a texel covers 4 pixels or more
                   (-filtroleve: the coarser one is a quarter of the size, so more of it stays
                   in the cache; see port_opt_pf_level) */
                const int coarse = port_opt_pf_level == 2 ||
                    (port_opt_pf_level == 1 && (dU <= -16384 || dU >= 16384 || dV <= -16384 || dV >= 16384));
                const int ps = c->pfs - coarse;
                const uint32_t *pfx = coarse ? c->pf1 : c->pf;
                /* (cell i covers texture coordinates i / 2^ps up to (i + 1) / 2^ps, like the texels
                   themselves: a clamped run only needs the slow loop where the plain one would) */
                const int32_t QU = (int32_t) (((uint32_t) U - 256u) << ps), QV = (int32_t) (((uint32_t) V - 256u) << ps);
                const int32_t dQU = (int32_t) ((uint32_t) dU << ps), dQV = (int32_t) ((uint32_t) dV << ps);
                const int W2 = tw << ps, H2 = th << ps;
                uint32_t pum, pvm;
                if (axis_mask(QU, dQU, len, W2, c->uwrap, &pum) && axis_mask(QV, dQV, len, H2, c->vwrap, &pvm)) {
                    TexWalk w = { QU, QV, dQU, dQV, pum, pvm, W2 };
                    fetch_texels(tex, pfx, len, &w);
                } else {   /* clamped at an edge */
                    uint32_t qu = (uint32_t) QU, qv = (uint32_t) QV;
                    for (int i = 0; i < len; i++) {
                        int iu = (int32_t) qu >> 16, iv = (int32_t) qv >> 16;
                        iu = c->uwrap == WR_REPEAT2 ? iu & (W2 - 1) : (iu < 0 ? 0 : (iu >= W2 ? W2 - 1 : iu));
                        iv = c->vwrap == WR_REPEAT2 ? iv & (H2 - 1) : (iv < 0 ? 0 : (iv >= H2 ? H2 - 1 : iv));
                        tex[i] = pfx[iv * W2 + iu];
                        qu += (uint32_t) dQU;
                        qv += (uint32_t) dQV;
                    }
                }
                U = (int32_t) ((uint32_t) U + (uint32_t) dU * (uint32_t) len);
                V = (int32_t) ((uint32_t) V + (uint32_t) dV * (uint32_t) len);
            } else if (FILT_SPAN(OUT, TEX) && c->filt && dU > -c->flim && dU < c->flim && dV > -c->flim && dV < c->flim) {
                nv_fetch3(tex, len, U, dU, V, dV, c);
                U = (int32_t) ((uint32_t) U + (uint32_t) dU * (uint32_t) len);
                V = (int32_t) ((uint32_t) V + (uint32_t) dV * (uint32_t) len);
            } else if (axis_mask(U, dU, len, tw, c->uwrap, &um) && axis_mask(V, dV, len, th, c->vwrap, &vm)) {
                TexWalk w = { U, V, dU, dV, um, vm, tw };
                fetch_texels(tex, tpx, len, &w);
                U = w.U;
                V = w.V;
            } else {
                for (int i = 0; i < len; i++) {
                    int iu = tc_wrap(U >> 16, tw, c->umask, c->uwrap);
                    int iv = tc_wrap(V >> 16, th, c->vmask, c->vwrap);
                    tex[i] = tpx[iv * tw + iu];
                    U += dU; V += dV;
                }
            }
        }
        if ((OUT == KIND_TR || OUT == KIND_PT) && TEX >= 0 && TEX != GFX_TEXENV_DECAL && len == 16 &&
            nv_invisible16(tex, OUT == KIND_PT)) {
            /* nothing of these 16 pixels changes */
        } else if (px + 16 <= W) {
            /* 16 pixels of the row are there (the ones past xe only get their own values back) */
            if (len > 8)
                nv_group2(cp, zp, zi, s, X, tex, c, flatc, len, OUT, TEX, COL);
            else
                nv_group(cp, zp, zi, s, X, tex, c, flatc, len, OUT, TEX, COL);
        } else {
            /* end of the row: the pixels after it belong to another row (maybe the other
               core's), so the last group works on a copy */
            for (int o = 0; o < len; o += 8) {
                const int n = len - o < 8 ? len - o : 8;
                int32_t XO[8];
                for (int k = 0; k < nch; k++)
                    XO[k] = (int32_t) ((uint32_t) X[k] + (uint32_t) s->dX8[k] * (uint32_t) (o >> 3));
                const uint32x4_t zo = o ? vaddq_u32(zi, s->dz8v) : zi;
                if (n == 8) {
                    nv_group(cp + o, zp + o, zo, s, XO, tex + o, c, flatc, 8, OUT, TEX, COL);
                } else {
                    uint16_t ct[8];
                    uint32_t zt[8];
                    memcpy(ct, cp + o, (size_t) n * 2);
                    memcpy(zt, zp + o, (size_t) n * 4);
                    nv_group(ct, zt, zo, s, XO, tex + o, c, flatc, n, OUT, TEX, COL);
                    memcpy(cp + o, ct, (size_t) n * 2);
                    memcpy(zp + o, zt, (size_t) n * 4);
                }
            }
        }
        for (int k = 0; k < nch; k++)
            X[k] = (int32_t) ((uint32_t) X[k] + (uint32_t) s->dX[k] * 16);
        cp += len;
        zp += len;
        zf += dz * len;
    }
    return (uint32_t) n0;
}

#define NV_DEF(O, T, C) \
    static uint32_t nv_##O##_##T##_##C(const TriCtx *c, int py, int xs, int xe, const float *a, const float *d) { \
        return nv_span(c, py, xs, xe, a, d, O, T - 1, C); \
    }
#define NV_DEF_T(O, T) NV_DEF(O, T, 0) NV_DEF(O, T, 1) NV_DEF(O, T, 2) NV_DEF(O, T, 3) NV_DEF(O, T, 4) NV_DEF(O, T, 5)
#define NV_DEF_O(O) NV_DEF_T(O, 0) NV_DEF_T(O, 1) NV_DEF_T(O, 2) NV_DEF_T(O, 3) NV_DEF_T(O, 4) NV_DEF_T(O, 5)
NV_DEF_O(0)
NV_DEF_O(1)
NV_DEF_O(2)
#define NV_REF_T(O, T) { nv_##O##_##T##_0, nv_##O##_##T##_1, nv_##O##_##T##_2, nv_##O##_##T##_3, nv_##O##_##T##_4, nv_##O##_##T##_5 }
#define NV_REF_O(O) { NV_REF_T(O, 0), NV_REF_T(O, 1), NV_REF_T(O, 2), NV_REF_T(O, 3), NV_REF_T(O, 4), NV_REF_T(O, 5) }
/* [list kind][texenv + 1 (0 = untextured)][colour mode: 0 flat, 1 gouraud, 2 gouraud + offset,
   3 the same + fog, 4 flat along each row, 5 flat colour and flat offset] */
static const SpanFn sNvFns[3][6][6] = { NV_REF_O(0), NV_REF_O(1), NV_REF_O(2) };
static int sUseNeon = 1;

static int wrap_mode(int n, int clamp, int mirror) {
    int pow2 = (n & (n - 1)) == 0;
    if (clamp) return WR_CLAMP;
    if (mirror) return pow2 ? WR_MIRROR2 : WR_MIRRORN;
    return pow2 ? WR_REPEAT2 : WR_REPEATN;
}

/* rows handled by this call: all, or (for the two-core split) bands of 8 rows by parity */
static __thread int sBandMask = 0, sBandVal = 0;
/* two cores: owner (0 / 1) of each band of 8 rows, rebuilt every frame so both cores
   finish together (core 1 also converts the finished frame for the screen) */
#define MAX_BANDS 128
static uint8_t sBandOwner[MAX_BANDS];
static float sCore0Share = 0.5f;
static void build_band_owners(void) {
    float acc = 0.5f;
    for (int b = 0; b < MAX_BANDS; b++) {
        acc += sCore0Share;
        if (acc >= 1.0f) {
            acc -= 1.0f;
            sBandOwner[b] = 0;
        } else {
            sBandOwner[b] = 1;
        }
    }
}
#define BAND_SKIP(py) (sBandMask && sBandOwner[(py) >> 3] != sBandVal)

/* ---- the sky, drawn first ----
 * The sky (untextured, opaque, farther than every opaque and punch-through vertex) comes
 * in the translucent list, so it was depth-tested over the whole screen after the scene.
 * Drawn first with no depth test it gives the same picture (where the scene drew, the
 * test hid it anyway), costs a plain fill, and stands in for the colour clear of the
 * pixels it covers. sort_tr picks these triangles; raster_tri switches spans by mode. */
#define GFX_SW_SKY_FIRST 1   /* (for the bench tools) */
static __thread int tlSkyMode;   /* 0 normal, 1 record each row's covered span, 2 draw */
static __thread int tlTrNoDepth;  /* this translucent triangle is in front of every depth written */
#define SKY_ROWS 1024
static int16_t sCovLo[SKY_ROWS], sCovHi[SKY_ROWS];   /* -1 nothing, -2 more than one span */

static uint32_t sky_cover_span(const TriCtx *c, int py, int xs, int xe, const float *a, const float *d) {
    (void) c; (void) a; (void) d;
    const int lo = sCovLo[py], hi = sCovHi[py];
    if (lo == -1) {
        sCovLo[py] = (int16_t) xs;
        sCovHi[py] = (int16_t) xe;
    } else if (lo >= 0) {
        if (xs <= hi && xe >= lo) {   /* touching or overlapping: one span */
            if (xs < lo) sCovLo[py] = (int16_t) xs;
            if (xe > hi) sCovHi[py] = (int16_t) xe;
        } else {
            sCovLo[py] = -2;
        }
    }
    return 0;
}

static inline uint16_t sky_565(int r, int g, int b) {   /* same bits as nv_pack565 */
    return (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void sky_fill(uint16_t *p, int n, uint16_t v) {
    const uint16x8_t vv = vdupq_n_u16(v);
    int i = 0;
    for (; i + 8 <= n; i += 8)
        vst1q_u16(p + i, vv);
    for (; i < n; i++)
        p[i] = v;
}

/* flat colour (the opaque span's COL 0) */
static uint32_t sky_fill_flat(const TriCtx *c, int py, int xs, int xe, const float *a, const float *d) {
    (void) a; (void) d;
    sky_fill(port_color + py * port_render_w + xs, xe - xs, sky_565(c->r, c->g, c->b));
    return (uint32_t) (xe - xs);
}

/* one colour per row (the opaque span's COL 4: same rounding) */
static uint32_t sky_fill_row(const TriCtx *c, int py, int xs, int xe, const float *a, const float *d) {
    (void) c; (void) d;
    const int r = clamp8((int32_t) (a[A_R] * 65536.0f) >> 16);
    const int g = clamp8((int32_t) (a[A_G] * 65536.0f) >> 16);
    const int b = clamp8((int32_t) (a[A_B] * 65536.0f) >> 16);
    sky_fill(port_color + py * port_render_w + xs, xe - xs, sky_565(r, g, b));
    return (uint32_t) (xe - xs);
}

#ifndef GFX_SW_NEXTROW
#define GFX_SW_NEXTROW 3   /* cache lines of the next row asked for ahead (3 or 5) */
#endif
/* -filtro on a translucent surface: one that writes depth on the N64 (Z_UPD), with a modulated
   texture. That is the ice of Sherbet Land (bilinear on the N64, like the road); smoke, item
   boxes, water and the kart effects do not write depth and stay unfiltered. */
static inline int tr_ground(const RState *st) {
    return st->depth_write && (st->texenv == GFX_TEXENV_MODULATE || st->texenv == GFX_TEXENV_MODULATEALPHA);
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

    const struct SwTex *tx = st->tex ? &sTexRd[st->tex] : NULL;
    const int textured = tx && tx->px;
    const int persp_geo = textured && !(at[0][A_Z] == at[1][A_Z] && at[0][A_Z] == at[2][A_Z]) &&
                          at[0][A_Z] > 0.0f && at[1][A_Z] > 0.0f && at[2][A_Z] > 0.0f;
    int persp = persp_geo;
    if (persp && !port_opt_exact) {
        /* almost the same depth on the three corners (the kart sprites, the smoke, far
           objects): mapped linearly, as on the PS1. The largest difference from the exact
           mapping is about (zmax - zmin) / zmin * (texels across) / 4; under half a texel
           nobody can see it, and the spans skip the division every 16 pixels (and can
           leave out the transparent ends of the sprites) */
        float zmin = at[0][A_Z], zmax = zmin, u0 = at[0][A_U], u1 = u0, v0 = at[0][A_V], v1 = v0;
        for (int i = 1; i < 3; i++) {
            const float z = at[i][A_Z], u = at[i][A_U], v = at[i][A_V];
            if (z < zmin) zmin = z;
            if (z > zmax) zmax = z;
            if (u < u0) u0 = u;
            if (u > u1) u1 = u;
            if (v < v0) v0 = v;
            if (v > v1) v1 = v;
        }
        const float du = (u1 - u0) * (float) tx->w, dv = (v1 - v0) * (float) tx->h;
        const float span = du > dv ? du : dv;
        if ((zmax - zmin) * span < 2.0f * zmin)   /* (zmax - zmin) / zmin * span / 4 < 0.5 */
            persp = 0;
    }
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
    if (kind == KIND_TR && tlTrNoDepth)
        c.dtest = 0;   /* in front of the whole scene: the test always passes */
    c.dwrite = st->depth_write && kind != KIND_TR;
    c.zoff = st->pane ? PANE_ZOFF : 0;
    c.fog = st->fog;
    const uint32_t fogc = *sFogRd;
    c.fr = (fogc >> 16) & 0xFF; c.fg = (fogc >> 8) & 0xFF; c.fb = fogc & 0xFF;
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
    c.ext = textured && tx->margin ? tx->ext : NULL;
    const uint32_t *tpf = textured ? tx->pf : NULL;
    int tpfwrap = textured ? tx->pfwrap : 0, tpfs = textured ? tx->pfs : PF_SHIFT;
    if (port_opt_filter && !port_opt_exact && ((persp_geo && (kind == KIND_OP || tr_ground(st))) || port_opt_filter_all)) {
        /* a mirrored texture: its doubled copy with the mirror in place, read as a plain
           repeat (the coordinates stay in the original texture's texels) */
        const int axes = (st->mirru && !st->clampu) | ((st->mirrv && !st->clampv) << 1);
        if (axes && tx->mpx && tx->mbake == axes) {
            c.tpx = tx->mpx;
            c.tw = tx->mw; c.th = tx->mh;
            c.umask = tx->mw - 1; c.vmask = tx->mh - 1;
            c.uwrap = wrap_mode(tx->mw, st->clampu, 0);
            c.vwrap = wrap_mode(tx->mh, st->clampv, 0);
            c.ext = NULL;
            tpf = tx->mpf; tpfwrap = tx->mpfwrap; tpfs = tx->mpfs;
        }
    }
    /* -filtro: 3D opaque surfaces in perspective (the track: karts, trees and items are
       sprites in the other lists, the HUD has no perspective) */
    const int wraps_ok = (c.uwrap == WR_REPEAT2 || c.uwrap == WR_CLAMP) && (c.vwrap == WR_REPEAT2 || c.vwrap == WR_CLAMP);
    if (port_opt_filter_all) {
        /* -qualidade: the N64's filter wherever the game asks for it (track, karts, trees,
           items, HUD, menus), magnified or up to 2 texels a pixel, computed exactly */
        c.filt = textured && st->filter && wraps_ok;
        c.flim = 131072;
        if (c.filt)
            c.ext = NULL;   /* (filtered, the invisible texels at a row's ends blend into the visible ones) */
    } else {
        c.filt = port_opt_filter && (kind == KIND_OP || tr_ground(st)) && persp_geo && wraps_ok;
        c.flim = 65536;
    }
    c.pf = c.filt && !port_opt_exact && !port_opt_filter_all && tpf && tpfwrap == (st->clampu | (st->clampv << 1)) ? tpf : NULL;
    c.pfs = tpfs;
    c.pf1 = c.pf ? c.pf + ((size_t) c.tw << tpfs) * ((size_t) c.th << tpfs) : NULL;
    if (port_opt_filter && !port_opt_filter_all && kind == KIND_OP && persp_geo && !c.filt) {
        /* (a 3D opaque texture the filter cannot take: noted once per kind, for the log) */
        static uint32_t seen[16];
        static int nseen;
        const uint32_t key = (uint32_t) c.tw << 20 | (uint32_t) c.th << 8 | (uint32_t) c.uwrap << 4 | (uint32_t) c.vwrap;
        int known = 0;
        for (int i = 0; i < nseen; i++)
            known |= seen[i] == key;
        if (!known && nseen < 16) {
            seen[nseen++] = key;
            printf("filtro: textura %dx%d fora do filtro (repeticao u %d v %d)\n", c.tw, c.th, c.uwrap, c.vwrap);
        }
    }
    SpanFn span = sSpanFns[kind][textured ? 1 : 0][extras ? 2 : (flat ? 0 : 1)];
    int nv_pending = sUseNeon && (kind != KIND_TR || (c.bs == GFX_BLENDF_SRCALPHA && c.bd == GFX_BLENDF_INVSRCALPHA));

    /* attribute gradients */
    const float inv = 1.0f / area;
    const float dy1 = Y[1] - Y[0], dy2 = Y[2] - Y[0], dx1 = X[1] - X[0], dx2 = X[2] - X[0];
    float ddx[NATTR], ddy[NATTR];
    for (int k = 0; k < NATTR; k++) {
        float a1 = at[1][k] - at[0][k], a2 = at[2][k] - at[0][k];
        ddx[k] = (a1 * dy2 - a2 * dy1) * inv;
        ddy[k] = (a2 * dx1 - a1 * dx2) * inv;
    }

    NvSteps nvs;
    c.nvs = NULL;
    if (nv_pending) {
        /* colour mode: offset only matters when textured, fog only when enabled; a gouraud
           triangle whose colour does not change along x (the sky) is flat on every span */
        int col = extras ? (c.fog ? 3 : 2) : (flat ? 0 : 1);
        /* textured, colour and offset the same on the three vertices (the smoke puffs): the
           interpolation would give the vertex values on every pixel (zero gradients) */
        if (col == 2 && v1->color.packed == c0 && v2->color.packed == c0 &&
            ((v0->pad0.vertindex ^ v1->pad0.vertindex) & 0x00FFFFFFu) == 0 &&
            ((v0->pad0.vertindex ^ v2->pad0.vertindex) & 0x00FFFFFFu) == 0) {
            const uint32_t o = v0->pad0.vertindex;
            col = 5;
            c.orr = (o >> 16) & 0xFF; c.og = (o >> 8) & 0xFF; c.ob = o & 0xFF;
        }
        if (col == 1 && (int32_t) (ddx[A_R] * 65536.0f) == 0 && (int32_t) (ddx[A_G] * 65536.0f) == 0 &&
            (int32_t) (ddx[A_B] * 65536.0f) == 0 && (int32_t) (ddx[A_A] * 65536.0f) == 0)
            col = 4;
        /* every pixel opaque: the translucent blend gives the source colour back and the
           punch-through test always passes, so both draw like opaque polygons (TR still
           tested against depth and never writing it) */
        const int va255 = (v0->color.packed >> 24) == 255 && (v1->color.packed >> 24) == 255 && (v2->color.packed >> 24) == 255;
        int opaque;
        if (!textured)
            opaque = va255;
        else if (c.texenv == GFX_TEXENV_DECAL)
            opaque = va255;
        else if (c.texenv == GFX_TEXENV_MODULATEALPHA)
            opaque = va255 && tx->opaque;
        else
            opaque = tx->opaque;
        int env = textured ? c.texenv + 1 : 0;
        if (textured && c.texenv == GFX_TEXENV_MODULATE && !c.fog && (c0 & 0x00FFFFFFu) == 0 &&
            v1->color.packed == c0 && v2->color.packed == c0 &&
            ((v0->pad0.vertindex ^ v1->pad0.vertindex) & 0x00FFFFFFu) == 0 &&
            ((v0->pad0.vertindex ^ v2->pad0.vertindex) & 0x00FFFFFFu) == 0) {
            const uint32_t o = v0->pad0.vertindex;
            env = NV_TEXA + 1;
            col = 0;
            c.r = (o >> 16) & 0xFF; c.g = (o >> 8) & 0xFF; c.b = o & 0xFF;
        }
        int out = kind;
        if (opaque && kind == KIND_TR) {
            out = KIND_OP;
            c.dtest = !tlTrNoDepth;
            c.dwrite = 0;
        } else if (opaque && kind == KIND_PT) {
            out = KIND_OP;
        }
        span = sNvFns[out][env][col];
        if (tlSkyMode) {
            if (tlSkyMode == 1) {
                span = sky_cover_span;
            } else if (!textured && col == 0) {
                span = sky_fill_flat;
            } else if (!textured && col == 4) {
                span = sky_fill_row;
            } else {   /* colour changes along the row: the opaque span, without depth */
                span = sNvFns[KIND_OP][env][col];
                c.dtest = 0;
                c.dwrite = 0;
            }
        }
        nv_prepare(&nvs, ddx, col);
        c.nvs = &nvs;
    }

    /* sort by y for the span walk */
    int i0 = 0, i1 = 1, i2 = 2, t;
    if (Y[i1] < Y[i0]) { t = i0; i0 = i1; i1 = t; }
    if (Y[i2] < Y[i0]) { t = i0; i0 = i2; i2 = t; }
    if (Y[i2] < Y[i1]) { t = i1; i1 = i2; i2 = t; }
    const float xa = X[i0], ya = Y[i0], xb = X[i1], yb = Y[i1], xc = X[i2], yc = Y[i2];

    int ystart = ceil_i(ya - 0.5f), yend = ceil_i(yc - 0.5f);
    if (ystart < 0) ystart = 0;
    if (yend > H) yend = H;
    if (ystart >= yend)
        return;

    const float slope_ac = (yc - ya) != 0.0f ? (xc - xa) / (yc - ya) : 0.0f;
    const float slope_ab = (yb - ya) != 0.0f ? (xb - xa) / (yb - ya) : 0.0f;
    const float slope_bc = (yc - yb) != 0.0f ? (xc - xb) / (yc - yb) : 0.0f;
    uint32_t pixels = 0;
    /* attributes the span reads: depth and texture, + colour (gouraud), + offset and fog */
    const int natt = extras ? NATTR : (flat ? A_R : A_OR);
    /* the row's attributes at its first pixel, 4 at a time in NEON: the same two fused
       multiply-adds per attribute as the scalar loop, so the same values */
    float32x4_t va0[3], vdx[3], vdy[3];
    const int nvec = (natt + 3) >> 2;
    {
        float t0[12] = { 0 }, tx[12] = { 0 }, ty[12] = { 0 };
        for (int k = 0; k < natt; k++) {
            t0[k] = at[0][k];
            tx[k] = ddx[k];
            ty[k] = ddy[k];
        }
        for (int j = 0; j < 3; j++) {
            va0[j] = vld1q_f32(t0 + 4 * j);
            vdx[j] = vld1q_f32(tx + 4 * j);
            vdy[j] = vld1q_f32(ty + 4 * j);
        }
    }
    /* first row whose centre is not above vertex b (fy >= yb), found once so the row loop
       compares integers: on the Cortex-A7 every float compare waits for the VFP to drain */
    int rb = ceil_i(yb - 0.5f);
    while ((float) (rb - 1) + 0.5f >= yb)
        rb--;
    while ((float) rb + 0.5f < yb)
        rb++;

    for (int py = ystart; py < yend; py++) {
        if (BAND_SKIP(py))
            continue;
        const float fy = (float) py + 0.5f;
        float xl = xa + (fy - ya) * slope_ac, xr;
        if (py < rb)
            xr = xa + (fy - ya) * slope_ab;
        else
            xr = xb + (fy - yb) * slope_bc;
        /* ceil of both ends at once in NEON (same result as ceil_i); the smaller is the start */
        int xs, xe;
        const float fxs = ceil2_clamp(xl - 0.5f, xr - 0.5f, W, &xs, &xe);
        if (xs >= xe)
            continue;

        /* the next row's depth and colour, asked for now: a span's first cache lines are
           otherwise waited for at its start (the in-span prefetch only looks ahead along the
           row), and on a small triangle that wait is most of the row's cost */
        if (py + 1 < yend && !BAND_SKIP(py + 1)) {
            const uint32_t *nz = port_depth + (py + 1) * W + xs;
            const uint16_t *nc = port_color + (py + 1) * W + xs;
            __builtin_prefetch(nz);
            __builtin_prefetch(nz + 16);
            __builtin_prefetch(nc);
#if GFX_SW_NEXTROW > 3
            __builtin_prefetch(nz + 32);
            __builtin_prefetch(nc + 32);
#endif
        }
        const float ox = fxs + 0.5f - X[0], oy = fy - Y[0];
        float a[12] __attribute__((aligned(16)));
        {
            const float32x4_t vo = vdupq_n_f32(ox), vp = vdupq_n_f32(oy);
            for (int j = 0; j < nvec; j++)
                vst1q_f32(a + 4 * j, vfmaq_f32(vfmaq_f32(va0[j], vdx[j], vo), vdy[j], vp));
        }
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
        uint8_t b[13] = { st->texenv, st->depth_test, st->depth_write, st->fog, st->blend_src, st->blend_dst,
                          st->clampu, st->clampv, st->mirru, st->mirrv, st->filter, st->pane, st->early };
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
            const int k = (int) (b - sRecB);
            if (k >= 0 && k < 3) {
                sDropBatch[k]++;
                sDropTris[k] += (uint32_t) (n / 3);
            }
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
        const int k = (int) (b - sRecB);
        if (k >= 0 && k < 3)
            sDropTris[k] += (uint32_t) (n / 3);
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
        tex_free(&sTex[i]);
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
    tex_free(&sTex[id]);
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

/* D.K.'s Jungle Parkway's waterfall: a 32x32 texture stretched over a big area, so its
   texels show as blocks (the N64 filtered it). Only that texture, recognised by its
   texels, is replaced on upload by a 4x larger one filtered in advance (bilinear,
   wrapping as it repeats, colours weighted by alpha so the see-through texels do not
   darken their neighbours); the coordinates are fractions of the texture, so it is
   drawn exactly as before, at no cost. -exato keeps the original. */
static uint32_t crc32_texels(const uint32_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *b = (const uint8_t *) p;
    for (uint32_t i = 0; i < n * 4; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void tex_fake_filter(struct SwTex *t) {
    static const uint32_t kFilterCrc[] = { 0x0558cb6du };   /* the waterfall */
    if (port_opt_exact || t->w != 32 || t->h != 32)
        return;
    const uint32_t crc = crc32_texels(t->px, 32 * 32);
    int hit = 0;
    for (unsigned i = 0; i < sizeof kFilterCrc / sizeof kFilterCrc[0]; i++)
        hit |= crc == kFilterCrc[i];
    if (!hit)
        return;
    enum { S = 32, F = 4, D = S * F };
    uint32_t *o = malloc(D * D * 4);
    if (!o)
        return;
    for (int y = 0; y < D; y++) {
        const float sy = (y + 0.5f) / F - 0.5f;
        const int y0 = (int) floorf(sy);
        const float fy = sy - (float) y0;
        for (int x = 0; x < D; x++) {
            const float sx = (x + 0.5f) / F - 0.5f;
            const int x0 = (int) floorf(sx);
            const float fx = sx - (float) x0;
            float acc[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                const int xx = (x0 + (k & 1)) & (S - 1), yy = (y0 + (k >> 1)) & (S - 1);
                const float w = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
                const uint32_t c = t->px[yy * S + xx];
                const float a = (float) (c >> 24) * w;
                acc[0] += a;
                acc[1] += a * (float) ((c >> 16) & 255);
                acc[2] += a * (float) ((c >> 8) & 255);
                acc[3] += a * (float) (c & 255);
            }
            uint32_t r = 0, g = 0, b = 0;
            if (acc[0] > 0.0f) {
                r = (uint32_t) (acc[1] / acc[0] + 0.5f);
                g = (uint32_t) (acc[2] / acc[0] + 0.5f);
                b = (uint32_t) (acc[3] / acc[0] + 0.5f);
            }
            const uint32_t a = (uint32_t) (acc[0] + 0.5f);
            o[y * D + x] = (a > 255 ? 255 : a) << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
    }
    free(t->px);
    t->px = o;
    t->alloc = D * D;
    t->w = D;
    t->h = D;
}

static void convert_texels(uint32_t *d, const uint16_t *s, uint32_t n, uint32_t fmt);

static void sw_upload_texture_now(const uint8_t *buf16, int width, int height, unsigned int type);
static void sw_upload_texture(const uint8_t *buf16, int width, int height, unsigned int type) {
    const double t = port_now_ms();
    sw_upload_texture_now(buf16, width, height, type);
    sFS.up += port_now_ms() - t;
    sFS.upn++;
}
static void sw_upload_texture_now(const uint8_t *buf16, int width, int height, unsigned int type) {
    struct SwTex *t = &sTex[sCurBound];
    if (width <= 0 || height <= 0)
        return;
    /* the same texels again (the front-end re-uploads a texture when its tile settings
       change, e.g. the road of Mario and Royal Raceway every frame): nothing to do, and the
       filter worked out in advance stays (it used to be thrown away, and after 3 uploads
       never made again, so those roads were never filtered) */
    if (t->px && t->w == width && t->h == height) {
        static uint32_t *scratch;
        static uint32_t scratchN;
        const uint32_t n = (uint32_t) width * (uint32_t) height;
        if (scratchN < n) {
            uint32_t *p = realloc(scratch, (size_t) n * 4);
            if (p) {
                scratch = p;
                scratchN = n;
            }
        }
        if (scratchN >= n) {
            convert_texels(scratch, (const uint16_t *) buf16, n, type & (3u << 27));
            if (!memcmp(scratch, t->px, (size_t) n * 4))
                return;
        }
    }
    pf_job_cancel((uint32_t) (t - sTex));   /* (a filter being worked out for the old texels) */
    tex_detach(t);   /* (-paralelo: the frame being drawn keeps the old texels) */
    free(t->pf);     /* (the filter worked out in advance belongs to the old texels) */
    t->pf = NULL;
    free(t->mpx);    /* (and so do the mirrored copy and its filter) */
    free(t->mpf);
    t->mpx = NULL;
    t->mpf = NULL;
    t->mbake = 0;
    if (++t->ups > 3 && !t->pfnever) {
        t->pfnever = 1;
        static int logged;
        if (port_opt_filter && logged++ < 8)
            printf("filtro: textura %dx%d muda a cada pouco, sem o filtro pronto\n", width, height);
    }
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
    convert_texels(t->px, (const uint16_t *) buf16, n, type & (3u << 27));
    t->w = (uint16_t) width;
    t->h = (uint16_t) height;
    tex_fake_filter(t);
    tex_update_opaque(t);
    stTexUp++;
    mark_dirty();
}

static void convert_texels(uint32_t *d, const uint16_t *s, uint32_t n, uint32_t fmt) {
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
    uint16_t front;   /* in front of every depth the scene writes: no depth test needed */
} TrSort;

static TrSort sTrSort[BUCKET_VERTS / 3];
static uint32_t sTrSortN, sTrHud;   /* sTrHud: first one in screen space (z >= 1), measurement */
#define SKY_MAX 64
static TrSort sSky[SKY_MAX];        /* the sky triangles, taken out of the sorted list */
static int sSkyN;

/* can this translucent triangle be drawn before everything (see "the sky, drawn first")?
   box: what the triangles left in the list before it cover, which it must not touch */
static int sky_candidate(const dc_fast_t *v, const RState *st, float zfar, const float *box) {
    if ((st->tex && sTexRd[st->tex].px) || st->fog ||
        st->blend_src != GFX_BLENDF_SRCALPHA || st->blend_dst != GFX_BLENDF_INVSRCALPHA)
        return 0;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 3; i++) {
        const float x = v[i].vert.x, y = v[i].vert.y, z = v[i].vert.z;
        if ((v[i].color.packed >> 24) != 255 || !(x == x) || !(y == y) || !(z < zfar))
            return 0;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    return x1 < box[0] - 2.0f || x0 > box[2] + 2.0f || y1 < box[1] - 2.0f || y0 > box[3] + 2.0f;
}

static void box_add(float *box, const dc_fast_t *v) {
    for (int i = 0; i < 3; i++) {
        const float x = v[i].vert.x, y = v[i].vert.y;
        if (x < box[0]) box[0] = x;
        if (y < box[1]) box[1] = y;
        if (x > box[2]) box[2] = x;
        if (y > box[3]) box[3] = y;
    }
}

static int tr_cmp(const void *a, const void *b) {
    const TrSort *x = a, *y = b;
    if (x->key < y->key) return -1;
    if (x->key > y->key) return 1;
    return (x->seq > y->seq) - (x->seq < y->seq);
}

/* -filtro, worked out in advance: where the road is magnified, the 3-point filter only
   depends on which texel square a pixel falls in and where inside it. Each texel is cut
   into PF_SUB x PF_SUB cells and the filter is computed once for every cell's centre,
   into a texture PF_SUB times larger; the spans then read it like an unfiltered texture
   (one texel per pixel instead of three plus the blend). The filter's position inside a
   texel is rounded to 1/PF_SUB of a texel: steps far below what the 16-bit screen shows. */
/* (8 x 8 cells for small textures under -filtroleve made no difference to the eye and cost
   more: the filter worked out in advance no longer fit the cache) */
static int pf_shift(int w, int h) {
    (void) w; (void) h;
    return PF_SHIFT;
}

/* one grid of cells: cell x covers texture coordinates x / SUB up to (x + 1) / SUB, and holds
   the 3-point filter at its centre (texel centres at i + 0.5, as on the N64) */
static void fill_pf(uint32_t *pf, const uint32_t *px, int tw, int th, int wrap, int shift, int y0, int y1) {
    const int SUB = 1 << shift, W = tw << shift, D = 2 * SUB;
    const int cu = wrap & 1, cv = (wrap >> 1) & 1;
    for (int y = y0; y < y1; y++) {
        /* (the centre, measured from the texel centres, in 1/(2 SUB) of a texel) */
        const int ny = 2 * y + 1 - SUB;
        const int iv = ny >= 0 ? ny / D : -((-ny + D - 1) / D);
        const int ft = (ny - iv * D) * 64 / SUB;
        const int v0 = cv ? (iv < 0 ? 0 : iv) : iv & (th - 1);
        const int v1 = cv ? (iv + 1 < th ? iv + 1 : th - 1) : (iv + 1) & (th - 1);
        const uint32_t *r0 = px + v0 * tw, *r1 = px + v1 * tw;
        for (int x = 0; x < W; x++) {
            const int nx = 2 * x + 1 - SUB;
            const int iu = nx >= 0 ? nx / D : -((-nx + D - 1) / D);
            const int fs = (nx - iu * D) * 64 / SUB;
            const int u0 = cu ? (iu < 0 ? 0 : iu) : iu & (tw - 1);
            const int u1 = cu ? (iu + 1 < tw ? iu + 1 : tw - 1) : (iu + 1) & (tw - 1);
            uint32_t a, b, c;
            int ws, wt;
            if (fs + ft <= 128) {
                a = r0[u0]; b = r0[u1]; c = r1[u0];
                ws = fs; wt = ft;
            } else {
                a = r1[u1]; b = r1[u0]; c = r0[u1];
                ws = 128 - fs; wt = 128 - ft;
            }
            const int w0 = 128 - ws - wt;
            uint32_t o = 0;
            for (int k = 0; k < 32; k += 8) {
                const uint32_t v = (((a >> k) & 255) * w0 + ((b >> k) & 255) * ws + ((c >> k) & 255) * wt + 64) >> 7;
                o |= v << k;
            }
            pf[y * W + x] = o;
        }
    }
}

/* the opaque textures of the frame about to be drawn get their filter worked out in
   advance, a few rows at a time: at most PF_BUDGET_MS of each frame (one 64x64 texture
   took about 11 ms, all at once: D.K.'s Jungle Parkway stuttered when new textures came
   into view). Until a texture's is complete it is drawn as before. */
#define PF_BUDGET_MS 1.5
static struct {
    int active, axes, wrap, shift, w, h, row, rows0, rows;
    uint32_t tex;
    uint32_t *src, *dst;   /* src: the texels, or the mirrored copy made for the job */
} sPfJob;
static void pf_job_cancel(uint32_t id) {
    if (!sPfJob.active || sPfJob.tex != id)
        return;
    if (sPfJob.axes)
        free(sPfJob.src);
    free(sPfJob.dst);
    sPfJob.active = 0;
}
static int pf_job_start(struct SwTex *t, int axes, int wrap) {
    const int w = t->w << (axes & 1), h = t->h << ((axes >> 1) & 1), shift = pf_shift(w, h);
    uint32_t *src = t->px;
    if (axes) {   /* the mirrored copy (bit 0: along u, bit 1: along v) */
        src = malloc((size_t) w * h * 4);
        if (!src)
            return 0;
        for (int y = 0; y < h; y++) {
            const int sy = y < t->h ? y : 2 * t->h - 1 - y;
            for (int x = 0; x < w; x++) {
                const int sx = x < t->w ? x : 2 * t->w - 1 - x;
                src[y * w + x] = t->px[sy * t->w + sx];
            }
        }
    }
    const size_t n0 = ((size_t) w << shift) * ((size_t) h << shift), n1 = ((size_t) w << (shift - 1)) * ((size_t) h << (shift - 1));
    uint32_t *dst = malloc((n0 + n1) * 4);
    if (!dst) {
        if (axes)
            free(src);
        return 0;
    }
    sPfJob.active = 1;
    sPfJob.tex = (uint32_t) (t - sTex);
    sPfJob.axes = axes;
    sPfJob.wrap = wrap;
    sPfJob.shift = shift;
    sPfJob.w = w;
    sPfJob.h = h;
    sPfJob.row = 0;
    sPfJob.rows0 = h << shift;
    sPfJob.rows = sPfJob.rows0 + (h << (shift - 1));
    sPfJob.src = src;
    sPfJob.dst = dst;
    return 1;
}
/* some more rows of the job; 1 when it is complete (and handed to its texture) */
static int pf_job_step(double until) {
    while (sPfJob.row < sPfJob.rows) {
        const int r = sPfJob.row;
        if (r < sPfJob.rows0)
            fill_pf(sPfJob.dst, sPfJob.src, sPfJob.w, sPfJob.h, sPfJob.wrap, sPfJob.shift, r, r + 1);
        else
            fill_pf(sPfJob.dst + ((size_t) sPfJob.w << sPfJob.shift) * ((size_t) sPfJob.h << sPfJob.shift), sPfJob.src,
                    sPfJob.w, sPfJob.h, sPfJob.wrap, sPfJob.shift - 1, r - sPfJob.rows0, r - sPfJob.rows0 + 1);
        sPfJob.row++;
        if (port_now_ms() >= until)
            break;
    }
    if (sPfJob.row < sPfJob.rows)
        return 0;
    struct SwTex *t = &sTex[sPfJob.tex];
    if (sPfJob.axes) {
        t->mpx = sPfJob.src;
        t->mw = (uint16_t) sPfJob.w;
        t->mh = (uint16_t) sPfJob.h;
        t->mbake = (uint8_t) sPfJob.axes;
        t->mpf = sPfJob.dst;
        t->mpfwrap = (uint8_t) sPfJob.wrap;
        t->mpfs = (uint8_t) sPfJob.shift;
    } else {
        t->pf = sPfJob.dst;
        t->pfwrap = (uint8_t) sPfJob.wrap;
        t->pfs = (uint8_t) sPfJob.shift;
    }
    sPfJob.active = 0;
    return 1;
}
static void prefilter_prepare(void) {
    if (!port_opt_filter || port_opt_exact)
        return;
    const double t0 = port_now_ms(), until = t0 + PF_BUDGET_MS;
    int built = 0;
    for (;;) {
        if (!sPfJob.active) {
            /* the next texture of this frame that needs one */
            int started = 0;
            for (int k = 0; k < 3 && !started; k++) {
                static const int kKind[3] = { KIND_OP, KIND_TR, KIND_PT };
                const Bucket *b = &sRenB[kKind[k]];
                for (int i = 0; i < b->nbatch && !started; i++) {
                    const RState *st = &b->batch[i].st;
                    const int axes = (st->mirru && !st->clampu) | ((st->mirrv && !st->clampv) << 1);
                    if (!st->tex || !b->batch[i].count)
                        continue;
                    if (port_opt_filter_all ? !axes || !st->filter : (k == 2 || (k == 1 && !tr_ground(st))))
                        continue;   /* (-qualidade: only the mirrored copies are needed, in every list) */
                    struct SwTex *t = &sTex[st->tex];
                    if (!t->px || (axes ? t->mpx != NULL : t->pf != NULL) || t->pfnever || t->w > 64 || t->h > 64)
                        continue;
                    if ((!st->clampu && (t->w & (t->w - 1))) || (!st->clampv && (t->h & (t->h - 1))))
                        continue;   /* (the filter only runs with power-of-two repeat or clamp) */
                    if (!pf_job_start(t, axes, st->clampu | (st->clampv << 1)))
                        t->pfnever = 1;
                    else
                        started = 1;
                }
            }
            if (!started)
                break;
        }
        if (!pf_job_step(until))
            break;
        built++;
        if (port_now_ms() >= until)
            break;
    }
    sFS.pf += port_now_ms() - t0;
    sFS.pfn += built;
}

static void sort_tr(void) {
    if (!sPipe)
        prefilter_prepare();   /* (-paralelo: in pipe_handover, before the textures are copied) */
    Bucket *b = &sRenB[KIND_TR];
#ifdef HAVE_FETCH_ASM
    if (sFetchAsm < 0) {   /* first frame, before any raster thread runs */
        sFetchAsm = fetch_asm_selftest();
        printf("gfx: busca de texels em assembly: %s\n", sFetchAsm ? "ok" : "FALHOU no teste, usando C");
    }
#endif
    if (!sExp6[63]) {   /* blend tables (first frame, before any raster thread runs) */
        for (int i = 0; i < 32; i++) sExp5[i] = (uint8_t) (i * 255 / 31);
        for (int i = 0; i < 64; i++) sExp6[i] = (uint8_t) (i * 255 / 63);
    }
    uint32_t nt = 0;
    for (int i = 0; i < b->nbatch; i++) {
        Batch *bt = &b->batch[i];
        for (uint32_t t = 0; t + 2 < bt->count; t += 3) {
            const dc_fast_t *v = &b->verts[bt->start + t];
            sTrSort[nt].key = v[0].vert.z + v[1].vert.z + v[2].vert.z + (bt->st.pane ? 192.0f : 0.0f) -
                              (bt->st.early ? 1e6f : 0.0f);
            sTrSort[nt].seq = bt->start + t;
            sTrSort[nt].batch = (uint16_t) i;
            nt++;
        }
    }
    qsort(sTrSort, nt, sizeof(sTrSort[0]), tr_cmp);

    /* the sky: below every opaque / punch-through vertex (with a margin for the depth
       interpolation), and not overlapping a translucent triangle drawn before it. Only
       when all of those write depth: where one did not, the sky used to paint over it. */
    sSkyN = 0;
    /* a growing pane (see gfx_sw_set_pane): its depths only compare right in fixed point,
       so neither the sky-first nor the in-front-of-everything shortcut (both work on the
       float depths) is used in that frame */
    int panes = 0;
    for (int k = 0; k < 3 && !panes; k++)
        for (int i = 0; i < sRenB[k].nbatch; i++)
            if (sRenB[k].batch[i].count && sRenB[k].batch[i].st.pane) {
                panes = 1;
                break;
            }
    int zok = sUseNeon && nt && !panes;
    float zmin = 1e30f, zmax = -1e30f;
    const Bucket *ob[2] = { &sRenB[KIND_OP], &sRenB[KIND_PT] };
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < ob[k]->nbatch; i++)
            if (ob[k]->batch[i].count && !ob[k]->batch[i].st.depth_write)
                zok = 0;
        for (uint32_t i = 0; i < ob[k]->nverts; i++) {
            const float z = ob[k]->verts[i].vert.z;
            if (z < zmin) zmin = z;
            if (z > zmax) zmax = z;
        }
    }
    /* translucent triangles nearer than every opaque / punch-through vertex (with a margin for
       the depth interpolation) always pass the depth test (the HUD, for one) */
    const float zfront = zmax > -1e29f ? zmax + fabsf(zmax) * 0.01f + 1e-6f : -1e30f;
    for (uint32_t i = 0; i < nt; i++) {
        const dc_fast_t *v = &b->verts[sTrSort[i].seq];
        const float z0 = v[0].vert.z, z1 = v[1].vert.z, z2 = v[2].vert.z;
        sTrSort[i].front = sUseNeon && !panes && z0 > zfront && z1 > zfront && z2 > zfront;
    }
    if (zok && zmin > 1e-5f) {
        /* (depth saturates at z = 16: keep well below it) */
        const float zfar = zmin * 0.99f < 15.0f ? zmin * 0.99f : 15.0f;
        float box[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
        uint32_t keep = 0;
        for (uint32_t i = 0; i < nt; i++) {
            const TrSort e = sTrSort[i];
            const dc_fast_t *v = &b->verts[e.seq];
            if (b->batch[e.batch].st.early) {   /* (after the sky in the game's order: not in the box) */
                sTrSort[keep++] = e;
                continue;
            }
            if (sSkyN < SKY_MAX && e.key < 3.0f * zfar && sky_candidate(v, &b->batch[e.batch].st, zfar, box)) {
                sSky[sSkyN++] = e;
                continue;
            }
            box_add(box, v);
            sTrSort[keep++] = e;
        }
        nt = keep;
    }
    sTrSortN = nt;
    sTrHud = nt;
    for (uint32_t i = 0; i < nt; i++)
        if (sTrSort[i].key >= 3.0f) {
            sTrHud = i;
            break;
        }
}

static void raster_tr_sorted(uint32_t from, uint32_t to) {
    Bucket *b = &sRenB[KIND_TR];
    for (uint32_t i = from; i < to; i++) {
        const dc_fast_t *v = &b->verts[sTrSort[i].seq];
        tlTrNoDepth = sTrSort[i].front;
        raster_tri(&v[0], &v[1], &v[2], &b->batch[sTrSort[i].batch].st, KIND_TR);
    }
    tlTrNoDepth = 0;
}

static void raster_sky(void) {
    const Bucket *b = &sRenB[KIND_TR];
    for (int i = 0; i < sSkyN; i++) {
        const dc_fast_t *v = &b->verts[sSky[i].seq];
        raster_tri(&v[0], &v[1], &v[2], &b->batch[sSky[i].batch].st, KIND_TR);
    }
}

/* clears this thread's rows (black, depth 0 = farthest) and draws the three lists on them */
static void raster_frame_band(int mask, int val) {
    const int W = port_render_w, H = port_render_h;
    double t0 = port_now_ms();
    sBandMask = mask;
    sBandVal = val;
    const int sky = sSkyN && H <= SKY_ROWS;
    if (sky) {
        /* what the sky will cover in each row: the colour clear can leave that out */
        for (int y = 0; y < H; y++)
            if (!BAND_SKIP(y))
                sCovLo[y] = sCovHi[y] = -1;
        const uint32_t p = tlPixels, t = tlTris;
        tlSkyMode = 1;
        raster_sky();
        tlSkyMode = 0;
        tlPixels = p;
        tlTris = t;
    }
    for (int y0 = 0; y0 < H; y0 += 8) {
        if (BAND_SKIP(y0))
            continue;
        int rows = H - y0 < 8 ? H - y0 : 8;
        memset(port_depth + y0 * W, 0, (size_t) rows * W * sizeof(uint32_t));
        if (!sky) {
            memset(port_color + y0 * W, 0, (size_t) rows * W * sizeof(uint16_t));
            continue;
        }
        for (int y = y0; y < y0 + rows; y++) {
            uint16_t *row = port_color + y * W;
            const int lo = sCovLo[y], hi = sCovHi[y];
            if (lo >= 0) {
                memset(row, 0, (size_t) lo * sizeof(uint16_t));
                memset(row + hi, 0, (size_t) (W - hi) * sizeof(uint16_t));
            } else {
                memset(row, 0, (size_t) W * sizeof(uint16_t));
            }
        }
    }
    double t1 = port_now_ms();
    if (sky) {
        tlSkyMode = 2;
        raster_sky();
        tlSkyMode = 0;
    } else {
        raster_sky();   /* (more rows than the coverage table: in place, as before) */
    }
    double t2 = port_now_ms();
    flush_bucket(&sRenB[KIND_OP], KIND_OP);
    double t3 = port_now_ms();
    flush_bucket(&sRenB[KIND_PT], KIND_PT);
    double t4 = port_now_ms();
    raster_tr_sorted(0, sTrHud);
    double t5 = port_now_ms();
    raster_tr_sorted(sTrHud, sTrSortN);
    double t6 = port_now_ms();
    double *ph = stPhase[val & 1];   /* each core writes only its own row */
    ph[0] += t1 - t0; ph[1] += t2 - t1; ph[2] += t3 - t2; ph[3] += t4 - t3; ph[4] += t5 - t4; ph[5] += t6 - t5;
}

static void flush_tr_sorted(void) {   /* replay tool: single thread, whole frame */
    sort_tr();
    sBandMask = 0;
    sBandVal = 0;
    raster_sky();   /* first of the translucent list, depth-tested as before */
    raster_tr_sorted(0, sTrSortN);
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
        sWorkerStage = 1;
        raster_frame_band(1, 1);
        sWorkerStage = 0;

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

/* -paralelo: the frame is handed to core 1 at the end of the game's frame and drawn
   while core 0 runs the game and the display list of the next one; core 0 joins in when
   it gets there. The picture is the same, shown one frame later. */
static int sPipeState;   /* 0 nothing, 1 core 1 drawing the last frame, 2 last frame drawn, not shown */
static int sPipeGen;
static double sPipeKickT;
int gfx_sw_presents;     /* gfx_wm_miyoo.c: the frame is shown from here, not after end_frame */

static void pipe_init(void) {
    const size_t nv = (size_t) 3 * BUCKET_VERTS * sizeof(dc_fast_t), nb = (size_t) 3 * BUCKET_BATCH * sizeof(Batch);
    dc_fast_t *v = malloc(nv);
    Batch *bt = malloc(nb);
    if (!v || !bt) {
        free(v);
        free(bt);
        printf("gfx: sem memoria para -paralelo\n");
        return;
    }
    for (int k = 0; k < 3; k++) {
        sBk[1][k].verts = v + (size_t) k * BUCKET_VERTS;
        sBk[1][k].batch = bt + (size_t) k * BUCKET_BATCH;
        sBk[1][k].nverts = 0;
        sBk[1][k].nbatch = 0;
        sBk[1][k].cur = -1;
        sBk[1][k].dirty = 1;
    }
    sTexRd = sTexR;
    sFogRd = &sFogR;
    sCore0Share = 0.3f;   /* core 1 starts alone: it takes the larger part */
    build_band_owners();
    sPipe = 1;
    gfx_sw_presents = 1;
}

/* -perf: the CPU's real clock, timed on a chain of dependent adds (OnionOS can change
   the clock while the game runs: this shows whether it did) */
static int cpu_mhz_now(void) {
    const int N = 200000;   /* 8 adds per pass, about 1.5 ms at 1.2 GHz */
    double best = 1e9;
    for (int k = 0; k < 3; k++) {
        const double t0 = port_now_ms();
#if defined(__arm__)
        int i = N, x = 0;
        __asm__ volatile("1:\n\tadd %1, %1, #1\n\tadd %1, %1, #1\n\tadd %1, %1, #1\n\tadd %1, %1, #1\n\t"
                         "add %1, %1, #1\n\tadd %1, %1, #1\n\tadd %1, %1, #1\n\tadd %1, %1, #1\n\t"
                         "subs %0, %0, #1\n\tbne 1b\n" : "+r"(i), "+r"(x) :: "cc");
#else
        volatile int x = 0;
        for (int i = 0; i < N; i++) { x++; x++; x++; x++; x++; x++; x++; x++; }
#endif
        const double t = port_now_ms() - t0;
        if (t < best) best = t;
    }
    /* (the same loop the bench uses: about 9 cycles per pass, so 1200 MHz reads ~1040) */
    return best > 0 ? (int) (N * 8.0 / (best * 1000.0)) : 0;
}

/* -perf: if no frame is finished for 2 s, says where everyone is (in the log), every 2 s */
extern volatile uint64_t vblticker;
static void *watchdog_proc(void *arg) {
    (void) arg;
    unsigned last = sFramesDone, said = 0, tick = 0;
    double since = port_now_ms();
    for (;;) {
        usleep(500000);
        if ((tick++ & 3) == 0)
            sCpuMhz = cpu_mhz_now();
        const unsigned f = sFramesDone;
        const double now = port_now_ms();
        if (f != last) {
            last = f;
            since = now;
            continue;
        }
        if (now - since >= 2000.0 && said < 20) {
            said++;
            since = now;
            fprintf(stderr, "vigia: nenhum quadro ha 2 s | quadros %u  jogo etapa %d%s  paralelo estado %d  pedidos %d feitos %d esperado %d  core1 %s | tela etapa %d voltas %u | vblank %llu\n",
                    f, sMainStage, port_main_pacing ? " (esperando o ritmo de 30 fps)" : "", sPipeState, sWorkGen, sWorkDone,
                    sPipeGen, sWorkerStage ? "desenhando" : "parado", port_flip_stage, port_flip_loops,
                    (unsigned long long) vblticker);
            fsync(2);
        }
    }
    return NULL;
}

static void start_worker(void) {
    if (port_opt_perf) {
        pthread_t th;
        if (pthread_create(&th, NULL, watchdog_proc, NULL) == 0)
            pthread_detach(th);
        printf("gfx: vigia de travamento ligado (-perf)\n");
    }
    if (pthread_create(&sWorker, NULL, worker_proc, NULL) == 0) {
        pthread_detach(sWorker);
        sWorkerOk = 1;
    }
    if (sWorkerOk && port_opt_parallel)
        pipe_init();
    printf("gfx: desenho em %s\n", sPipe ? "2 nucleos, em linha de montagem (-paralelo)" : sWorkerOk ? "2 nucleos" : "1 nucleo");
}

static void raster_frame(void) {
    tlPixels = tlTris = 0;
    if (!sWorkerOk) {
        raster_frame_band(0, 0);
    } else {
        if (!sBandOwner[1] && !sBandOwner[2])
            build_band_owners();   /* first frame */
        pthread_mutex_lock(&sWMutex);
        int gen = ++sWorkGen;
        pthread_cond_broadcast(&sWCond);
        pthread_mutex_unlock(&sWMutex);

        double t0 = port_now_ms();
        raster_frame_band(1, 0);
        double ms0 = port_now_ms() - t0;
        stCore0Ms += ms0;

        pthread_mutex_lock(&sWMutex);
        while (sWorkDone != gen)
            pthread_cond_wait(&sWCond, &sWMutex);
        stCore1Ms += sWorkerMs;
        /* share for the next frame: each core's time per unit of share this frame */
        if (ms0 > 0.2 && sWorkerMs > 0.2) {
            double r0 = ms0 / sCore0Share, r1 = sWorkerMs / (1.0 - sCore0Share);
            double target = r1 / (r0 + r1);
            double f = 0.75 * sCore0Share + 0.25 * target;
            sCore0Share = (float) (f < 0.3 ? 0.3 : (f > 0.7 ? 0.7 : f));
            build_band_owners();
        }
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

/* -paralelo: core 0 draws its bands of the frame core 1 started on, waits for core 1, and
   lets go of the textures that frame held. The split for the next frame counts the time
   core 1 worked alone (lead): both should finish together. */
static void pipe_finish(int adapt) {
    const double tj = port_now_ms(), lead = tj - sPipeKickT;
    tlPixels = tlTris = 0;
    sMainStage = 2;
    raster_frame_band(1, 0);
    sMainStage = 3;
    const double ms0 = port_now_ms() - tj;
    stCore0Ms += ms0;
    sFS.band += ms0;
    stLeadMs += lead;

    pthread_mutex_lock(&sWMutex);
    while (sWorkDone != sPipeGen)
        pthread_cond_wait(&sWCond, &sWMutex);
    stWaitMs += port_now_ms() - tj - ms0;
    sFS.wait += port_now_ms() - tj - ms0;
    const double ms1 = sWorkerMs;
    tlPixels += sWorkerPixels;
    tlTris += sWorkerTris;
    pthread_mutex_unlock(&sWMutex);
    stCore1Ms += ms1;
    stPixels += tlPixels;

    if (adapt && ms0 > 0.2 && ms1 > 0.2) {
        /* each core's time for the whole frame, at this frame's speed; core 1 had a head
           start of lead: s * r0 + lead = (1 - s) * r1 */
        const double s = sCore0Share, r0 = ms0 / s, r1 = ms1 / (1.0 - s);
        const double target = (r1 - lead) / (r0 + r1);
        const double f = 0.75 * s + 0.25 * target;
        sCore0Share = (float) (f < 0.1 ? 0.1 : (f > 0.7 ? 0.7 : f));
        build_band_owners();
    }

    for (int i = 0; i < sSharedN; i++)
        sTexShared[sSharedIds[i]] = 0;
    sSharedN = 0;
    for (int i = 0; i < sDeferredN; i++)
        free(sDeferred[i]);
    sDeferredN = 0;
    sPipeState = 2;
}

/* -resdin: the size of the next frame. The time the game needed for the last frame, apart
   from waiting for the screen or the 30 fps beat, says whether the drawing keeps up: over
   16.4 ms on average, or one frame over 20 ms (it missed the screen's refresh), the frames
   get smaller one step, at most every 4 frames (it used to wait 15 frames a step: at D.K.'s
   Jungle Parkway, three steps took a quarter of a second at 45 fps, felt as a stutter);
   when the estimate one step up (the drawing grows with the pixels, the rest does not)
   fits in 15 ms they grow back, after 30 frames without a change. */
static const int kResW[4] = { 640, 576, 512, 448 }, kResH[4] = { 480, 432, 384, 336 };
static int sResLevel, sResHold, sResDownHold;
static double sResEma, sResLastT, sResLastPacing, sResWait;
static void resdin_pick(void) {
    if (!port_opt_resdin && sResLevel) {   /* (switched off while smaller: back to full size) */
        sResLevel = 0;
        port_render_w = kResW[0];
        port_render_h = kResH[0];
    }
    if (!port_opt_resdin || !port_video_scales() || sCapF || port_render_w > 640)
        return;
    const double now = port_now_ms();
    double c = 0;
    if (sResLastT > 0) {
        c = now - sResLastT - sResWait - (port_pacing_ms - sResLastPacing);
        if (c < 0)
            c = 0;
        sResEma = sResEma > 0 ? 0.8 * sResEma + 0.2 * c : c;
    }
    sResLastT = now;
    sResLastPacing = port_pacing_ms;
    sResWait = 0;
    if (sResHold > 0)
        sResHold--;
    if (sResDownHold > 0)
        sResDownHold--;
    /* (at 30 fps -- the menus, and the quality mode -- a frame has twice the time) */
    extern int force_30fps;
    const double budget = force_30fps ? 32.8 : 16.4, spike = force_30fps ? 40.0 : 20.0, up_fit = force_30fps ? 30.0 : 15.0;
    if (sResLevel < 3 && !sResDownHold && (sResEma > budget || c > spike)) {
        sResLevel++;
        sResDownHold = 4;
        sResHold = 30;
    } else if (sResLevel > 0 && !sResHold) {
        const double cur = (double) kResW[sResLevel] * kResH[sResLevel], up = (double) kResW[sResLevel - 1] * kResH[sResLevel - 1];
        const double fixed = 2.5;   /* the game and the display list: they do not shrink with the frame */
        if ((sResEma - fixed) * (up / cur) + fixed < up_fit) {
            sResLevel--;
            sResHold = 30;
        }
    }
    sResFrames[sResLevel]++;
    port_render_w = kResW[sResLevel];
    port_render_h = kResH[sResLevel];
}

/* -paralelo: the lists just filled become the frame to draw; the textures it uses are
   copied (entries only, the texels stay shared until it is done) and core 1 starts */
static void pipe_handover(void) {
    resdin_pick();
    Bucket *r = sRecB;
    sRecB = r == sBk[0] ? sBk[1] : sBk[0];   /* the set that was drawn: free again */
    sRenB = r;
    prefilter_prepare();
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < sRenB[k].nbatch; i++) {
            const uint32_t id = sRenB[k].batch[i].st.tex;
            if (id && !sTexShared[id]) {
                sTexShared[id] = 1;
                sSharedIds[sSharedN++] = (uint16_t) id;
                sTexR[id] = sTex[id];
            }
        }
    sFogR = sFogColor;
    sort_tr();
    pthread_mutex_lock(&sWMutex);
    sPipeGen = ++sWorkGen;
    pthread_cond_broadcast(&sWCond);
    pthread_mutex_unlock(&sWMutex);
    sPipeKickT = port_now_ms();
    sPipeState = 1;
}

static void sw_end_frame(void) {
    double t0 = port_now_ms();
    sFrameNo++;
    for (int k = 0; k < 3; k++) {
        if (sRecB[k].nverts > sPeakVerts[k]) sPeakVerts[k] = sRecB[k].nverts;
        if ((uint32_t) sRecB[k].nbatch > sPeakBatch[k]) sPeakBatch[k] = (uint32_t) sRecB[k].nbatch;
    }
    if (sDropTris[0] | sDropTris[1] | sDropTris[2] | sDropBatch[0] | sDropBatch[1] | sDropBatch[2])
        drop_note();
    stWalkMs += t0 - sFrameT0;
    sFS.walk += t0 - sFrameT0;
    count_list(&sOP, &stOpTris, KIND_OP);
    count_list(&sPT, &stPtTris, KIND_PT);
    count_list(&sTR, &stTrTris, KIND_TR);
    stTris += (uint32_t) ((sOP.nverts + sPT.nverts + sTR.nverts) / 3);
    if (sPipe) {
        sMainStage = 1;
        /* the last frame: finished and shown (the screen gets the next colour buffer) */
        if (sPipeState == 1)
            pipe_finish(1);
        if (sPipeState == 2) {
            const double tp = port_now_ms();
            sMainStage = 4;
            port_video_present();
            stPresentMs += port_now_ms() - tp;
            sFS.present += port_now_ms() - tp;
            sResWait += port_now_ms() - tp;
            sPipeState = 0;
        }
        sMainStage = 5;
        const double th = port_now_ms();
        pipe_handover();
        sFS.hand += port_now_ms() - th;
        sFS.res = sResLevel;
        sMainStage = 0;
        sFramesDone++;
        stRasterMs += port_now_ms() - t0;
        if (sCapF) {   /* the capture keeps the finished picture: this frame is drawn now */
            pipe_finish(0);
            cap_end();
        }
        return;
    }
    sort_tr();
    raster_frame();
    sFramesDone++;
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
