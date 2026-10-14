/*
 * Miyoo Mini (Plus) video: the software renderer draws RGB565 into port_color
 * (320x240 by default, 640x480 with -hires). On present the finished picture
 * is handed to a flip thread pinned to the second core, which converts it to
 * XRGB8888, scales it to the 640x480 panel, rotates it by 180 degrees (the
 * panel is mounted upside down), copies it into the hidden page of /dev/fb0
 * and shows that page at the next vsync (FBIOPAN_DISPLAY waits for it).
 *
 * Three colour buffers rotate: one being drawn, one being shown by the flip
 * thread, and the previous one. port_fb_pixel565() reads the last presented
 * frame (the N64 "copy framebuffer" effects, e.g. the Luigi Raceway screen).
 *
 * Without a usable /dev/fb0 everything still runs (nothing is shown).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/fb.h>
#include <dlfcn.h>
#if defined(TARGET_MIYOO) && __has_include(<mi_gfx.h>)
#include <mi_sys.h>
#include <mi_gfx.h>
#define HAVE_MI_GFX 1
#endif

#include "port_miyoo.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#else
#include "neon_emu.h"
#endif

#define PANEL_W 640
#define PANEL_H 480

int port_render_w = 320, port_render_h = 240;
uint16_t *port_color;
uint32_t *port_depth;

static uint16_t *cbuf[3];
static int cur_idx;          /* buffer being drawn */
static int last_idx = -1;    /* last presented */
static int last_w, last_h;   /* its size (-resdin) */

static int fb_fd = -1;
static struct fb_var_screeninfo fb_var;
static uint8_t *fb_mem;
static size_t fb_len;
static int fb_pages;         /* 2 = page flipping, 1 = draw straight into the shown page */
static unsigned long fb_phys;  /* physical address of the screen pages (for the 2D engine) */
static int fb_page;

static pthread_t flip_thread;
static pthread_mutex_t flip_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flip_cond = PTHREAD_COND_INITIALIZER;
static int flip_started;
static int job_pending;
static int snap_free = 1;
static const uint16_t *snap;
static int snap_w, snap_h;   /* the queued frame's size (it can change from frame to frame: -resdin) */
static int snap_ge = -1;     /* its 2D pieces for the 2D engine (their part), -1 none */

static inline uint32_t rgb565_to_x8888(uint32_t c) {
    return ((c & 0xF800u) << 8) | ((c & 0xE000u) << 3) |
           ((c & 0x07E0u) << 5) | ((c & 0x0600u) >> 1) |
           ((c & 0x001Fu) << 3) | ((c & 0x001Cu) >> 2);
}

/* src (w x h, RGB565) -> dst (PANEL_W x PANEL_H, XRGB8888), scaled and rotated 180 degrees.
 * dst is the hidden screen page itself: each row is built in a small cached line buffer
 * and written to the page in one forward copy. The screen memory is fast to write
 * (write-combined) but ~30x slower to read, so it is never read back. */
static uint32_t line_buf[PANEL_W] __attribute__((aligned(16)));

/* 8 RGB565 pixels -> b g r planes, expanded by bit replication (same as rgb565_to_x8888) */
static inline void conv8(uint16x8_t v, uint8x8_t *r, uint8x8_t *g, uint8x8_t *b) {
    uint8x8_t tr = vshrn_n_u16(v, 8), tg = vshrn_n_u16(v, 3), tb = vmovn_u16(vshlq_n_u16(v, 3));
    *r = vsri_n_u8(tr, tr, 5);
    *g = vsri_n_u8(tg, tg, 6);
    *b = vsri_n_u8(tb, tb, 5);
}
/* 8 pixels ending at s[7], in reverse order (the panel is upside down) */
static inline uint16x8_t load8_rev(const uint16_t *s) {
    uint16x8_t v = vrev64q_u16(vld1q_u16(s));
    return vcombine_u16(vget_high_u16(v), vget_low_u16(v));
}

/* one source row (w pixels) -> line_buf, mirrored; doubled horizontally when twice = 1 */
static void conv_row(const uint16_t *s, int w, int twice) {
    if (w % 8) {
        uint32_t *d = line_buf;
        for (int x = w - 1; x >= 0; x--) {
            uint32_t c = rgb565_to_x8888(s[x]);
            *d++ = c;
            if (twice)
                *d++ = c;
        }
        return;
    }
    uint8_t *d = (uint8_t *) line_buf;
    const uint8x8_t zero = vdup_n_u8(0);
    for (int x = w - 8; x >= 0; x -= 8) {
        uint8x8x4_t o;
        conv8(load8_rev(s + x), &o.val[2], &o.val[1], &o.val[0]);
        o.val[3] = zero;
        if (!twice) {
            vst4_u8(d, o);
            d += 32;
        } else {
            uint8x8x2_t zb = vzip_u8(o.val[0], o.val[0]), zg = vzip_u8(o.val[1], o.val[1]), zr = vzip_u8(o.val[2], o.val[2]);
            uint8x8x4_t a = { { zb.val[0], zg.val[0], zr.val[0], zero } }, b = { { zb.val[1], zg.val[1], zr.val[1], zero } };
            vst4_u8(d, a);
            vst4_u8(d + 32, b);
            d += 64;
        }
    }
}

static void convert_frame(const uint16_t *src, int w, int h, uint32_t *dst) {
    if (w * 2 == PANEL_W && h * 2 == PANEL_H) {
        for (int y = 0; y < h; y++) {
            conv_row(src + y * w, w, 1);
            uint32_t *row = dst + (PANEL_H - 1 - 2 * y) * PANEL_W;
            memcpy(row, line_buf, PANEL_W * 4);
            memcpy(row - PANEL_W, line_buf, PANEL_W * 4);
        }
    } else if (w == PANEL_W && h == PANEL_H) {
        for (int y = 0; y < h; y++) {
            conv_row(src + y * w, w, 0);
            memcpy(dst + (PANEL_H - 1 - y) * PANEL_W, line_buf, PANEL_W * 4);
        }
    } else {
        for (int y = 0; y < PANEL_H; y++) {
            const uint16_t *s = src + (y * h / PANEL_H) * w;
            for (int x = 0; x < PANEL_W; x++)
                line_buf[PANEL_W - 1 - x] = rgb565_to_x8888(s[x * w / PANEL_W]);
            memcpy(dst + (PANEL_H - 1 - y) * PANEL_W, line_buf, PANEL_W * 4);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* The SigmaStar 2D engine (MI_GFX) does the RGB565 -> XRGB8888 conversion, the 180    */
/* degree turn and (at 320x240) the 2x scale into the screen page by itself, so the    */
/* CPU does not spend ~3 ms per frame on it. The colour buffers are then physically     */
/* contiguous (MMA) memory, mapped cached: drawing into them is as fast as before.     */
/* The libraries are opened at run time; any failure, or a wrong result in the test    */
/* blit at startup, keeps the NEON copy (convert_frame). -semgfx turns it off.         */
/* ------------------------------------------------------------------------- */
int port_opt_nogfx = 0;
static int hw_ok;
#ifdef HAVE_MI_GFX
static struct {
    MI_S32 (*SysInit)(void);
    MI_S32 (*MmaAlloc)(MI_U8 *, MI_U32, MI_PHY *);
    MI_S32 (*MmaFree)(MI_PHY);
    MI_S32 (*Mmap)(MI_U64, MI_U32, void **, MI_BOOL);
    MI_S32 (*Munmap)(void *, MI_U32);
    MI_S32 (*Flush)(void *, MI_U32);
    MI_S32 (*GfxOpen)(void);
    MI_S32 (*GfxClose)(void);
    MI_S32 (*Blit)(MI_GFX_Surface_t *, MI_GFX_Rect_t *, MI_GFX_Surface_t *, MI_GFX_Rect_t *, MI_GFX_Opt_t *, MI_U16 *);
    MI_S32 (*Wait)(MI_BOOL, MI_U16);
} mi;
static MI_PHY hw_phys[3];
static size_t hw_size;
static int hw_rotate = E_MI_GFX_ROTATE_180, hw_mirror = E_MI_GFX_MIRROR_NONE;

static void *open_lib(const char *name) {
    static const char *dirs[] = { "", "/customer/lib/", "/config/lib/", "/lib/", "/usr/lib/" };
    char path[96];
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(path, sizeof path, "%s%s", dirs[i], name);
        void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (h)
            return h;
    }
    return NULL;
}

static int hw_blit_q(MI_PHY src, int sw, int sh, MI_PHY dst, int dw, int dh, int rotate, int mirror, MI_U16 *fq);
/* (two threads queue work for the engine: the flip thread, and the cores' 2D under-layer in
   the menus: one call into its driver at a time) */
static pthread_mutex_t ge_mx = PTHREAD_MUTEX_INITIALIZER;
static MI_S32 mi_blit(MI_GFX_Surface_t *s, MI_GFX_Rect_t *sr, MI_GFX_Surface_t *d, MI_GFX_Rect_t *dr, MI_GFX_Opt_t *o,
                      MI_U16 *f) {
    pthread_mutex_lock(&ge_mx);
    const MI_S32 r = mi.Blit(s, sr, d, dr, o, f);
    pthread_mutex_unlock(&ge_mx);
    return r;
}
static int hw_blit(MI_PHY src, int sw, int sh, MI_PHY dst, int dw, int dh, int rotate, int mirror) {
    return hw_blit_q(src, sw, sh, dst, dw, dh, rotate, mirror, NULL);
}
/* fq: queue it without waiting (its fence there); NULL: wait for it */
static int hw_blit_q(MI_PHY src, int sw, int sh, MI_PHY dst, int dw, int dh, int rotate, int mirror, MI_U16 *fq) {
    MI_GFX_Surface_t s = { src, E_MI_GFX_FMT_RGB565, (MI_U32) sw, (MI_U32) sh, (MI_U32) sw * 2 };
    MI_GFX_Surface_t d = { dst, E_MI_GFX_FMT_ARGB8888, (MI_U32) dw, (MI_U32) dh, (MI_U32) dw * 4 };
    MI_GFX_Rect_t sr = { 0, 0, (MI_U32) sw, (MI_U32) sh }, dr = { 0, 0, (MI_U32) dw, (MI_U32) dh };
    MI_GFX_Opt_t opt;
    memset(&opt, 0, sizeof opt);
    opt.stClipRect = dr;
    opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
    opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
    opt.eMirror = (MI_GFX_Mirror_e) mirror;
    opt.eRotate = (MI_GFX_Rotate_e) rotate;
    opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
    MI_U16 fence = 0;
    if (mi_blit(&s, &sr, &d, &dr, &opt, &fence) != 0)
        return -1;
    if (fq)
        *fq = fence;
    else
        mi.Wait(TRUE, fence);
    return 0;
}

/* test blit (scaled like the real one) into a scratch buffer: right orientation and
   colours (within the rounding of the expansion), or no hardware blit */
static int hw_selftest(int w, int h) {
    const int sw = 16, sh = 8, dw = w * 2 == PANEL_W ? 32 : 16, dh = w * 2 == PANEL_W ? 16 : 8;
    MI_PHY ps = 0, pd = 0;
    void *vs = NULL, *vd = NULL;
    int ok = 0;
    if (mi.MmaAlloc(NULL, 4096, &ps) || mi.MmaAlloc(NULL, 4096, &pd))
        goto out;
    if (mi.Mmap(ps, 4096, &vs, TRUE) || mi.Mmap(pd, 4096, &vd, TRUE))
        goto out;
    uint16_t *src = vs;
    for (int i = 0; i < sw * sh; i++)
        src[i] = (uint16_t) (((i * 7) & 31) << 11 | ((i * 13) & 63) << 5 | ((i * 3 + 5) & 31));
    const int tries[2][2] = { { E_MI_GFX_ROTATE_180, E_MI_GFX_MIRROR_NONE }, { E_MI_GFX_ROTATE_0, E_MI_GFX_MIRROR_BOTH } };
    for (int t = 0; t < 2 && !ok; t++) {
        memset(vd, 0, 4096);
        mi.Flush(vs, 4096);
        mi.Flush(vd, 4096);
        if (hw_blit(ps, sw, sh, pd, dw, dh, tries[t][0], tries[t][1]))
            continue;
        mi.Flush(vd, 4096);
        const uint32_t *dst = vd;
        int bad = 0;
        for (int y = 0; y < dh && !bad; y++)
            for (int x = 0; x < dw; x++) {
                /* screen pixel (x, y) shows source pixel (sw-1-x/sx, sh-1-y/sy) */
                int sx = sw - 1 - x * sw / dw, sy = sh - 1 - y * sh / dh;
                uint32_t want = rgb565_to_x8888(src[sy * sw + sx]), got = dst[y * dw + x];
                for (int k = 0; k < 24; k += 8) {
                    int a = (int) ((want >> k) & 255), b = (int) ((got >> k) & 255);
                    if (a - b > 8 || b - a > 8)
                        bad = 1;
                }
            }
        if (!bad) {
            hw_rotate = tries[t][0];
            hw_mirror = tries[t][1];
            ok = 1;
        }
    }
out:
    if (vs) mi.Munmap(vs, 4096);
    if (vd) mi.Munmap(vd, 4096);
    if (ps) mi.MmaFree(ps);
    if (pd) mi.MmaFree(pd);
    return ok;
}
#endif

/* (measurement, for drawing the HUD with the 2D engine) does its scaling blend the pixels
   (bilinear) or repeat them? A black and a white pixel stretched to 16: the log says which */
#ifdef HAVE_MI_GFX
static void hw_scale_probe(void) {
    MI_PHY ps = 0, pd = 0;
    void *vs = NULL, *vd = NULL;
    if (mi.MmaAlloc(NULL, 4096, &ps) || mi.MmaAlloc(NULL, 4096, &pd) || mi.Mmap(ps, 4096, &vs, TRUE) ||
        mi.Mmap(pd, 4096, &vd, TRUE))
        goto out;
    uint16_t *src = vs;
    src[0] = 0x0000;
    src[1] = 0xFFFF;
    memset(vd, 0, 4096);
    mi.Flush(vs, 4096);
    mi.Flush(vd, 4096);
    if (hw_blit(ps, 2, 1, pd, 16, 1, E_MI_GFX_ROTATE_0, E_MI_GFX_MIRROR_NONE) == 0) {
        mi.Flush(vd, 4096);
        const uint32_t *d = vd;
        int mid = 0;
        char line[16 * 4 + 1];
        for (int x = 0; x < 16; x++) {
            const int g = (int) ((d[x] >> 8) & 255);
            mid += g > 24 && g < 232;
            snprintf(line + x * 4, 5, "%3d ", g);
        }
        printf("video: escala do MI_GFX %s: %s\n", mid ? "suave (mistura os pixels)" : "pixelada (repete os pixels)", line);
    } else {
        printf("video: escala do MI_GFX: o teste falhou\n");
    }
out:
    if (vs) mi.Munmap(vs, 4096);
    if (vd) mi.Munmap(vd, 4096);
    if (ps) mi.MmaFree(ps);
    if (pd) mi.MmaFree(pd);
}
#endif

/* colour buffers in MMA memory when the 2D engine works; 0 = keep the malloc ones */
/* ---- the 2D of a frame by the 2D engine (gfx_sw.c, hud_ge_build) ----------------------
   The renderer hands over the 2D pieces of its frame (the HUD, the menus' pictures and
   letters) as rectangles; the engine draws them over the finished picture in the screen
   page, at 640x480, after the frame's own copy (flip thread), scaled with its smoothing.
   Their texels are copied (with the piece's colour multiplied in, and the colour
   premultiplied by the alpha, so the smoothing blends the edges right) into memory the
   engine reads: one part per colour buffer, so the copies of the frame being drawn never
   touch those of a frame still being shown. Each copy has a 1-texel border repeating its
   edge (the smoothing at the edges reads no neighbour). A test at startup checks where the
   engine puts a rectangle when it turns the picture and how it blends; any doubt, and the
   cores draw all of it as before. */
#ifdef HAVE_MI_GFX
#define GE_MAX 1024
typedef struct {
    uint32_t off;              /* the copy, in this colour buffer's part */
    uint16_t pw, ph, sw, sh;   /* the copy's size (border included) and the picture in it (at 1,1) */
    int16_t dx, dy, dw, dh;    /* where on the 640x480 screen (as the game sees it) */
    uint8_t on;
    uint8_t layer;             /* 0 over the finished picture (flip thread), 1 under the cores' 2D */
} GeItem;
typedef struct {
    GeItem it[GE_MAX];
    int n, nover;
    uint32_t used;
    /* the copies made into this part, in order (what its memory holds): a frame that makes
       the same copies at the same places as the last one here (three frames before: a
       screen that does not change) leaves them as they are */
    GeArgs ca[2][GE_MAX];
    uint32_t coff[2][GE_MAX];
    int cn[2], cur;
    uint32_t dmin, dmax;   /* what was written this frame (flushed for the engine) */
} GeOvl;
static GeOvl sGeOvl[3];
static MI_PHY ge_phys[3];
static uint8_t *ge_virt[3];
static uint32_t ge_cap;      /* bytes in each part */
static int ge_ok;            /* the test passed */
static int ge_place;         /* 0: the rectangle where it is asked for (turned inside it); 1: the engine turns its place too */
static int ge_under;         /* the engine blends right into an RGB565 colour buffer (tested) */
static int ge_sec = -1;      /* the part being filled (the colour buffer being drawn) */
static double ge_copy_ms;    /* (-perf: the copies, game thread) */
static int ge_under_n;       /* (-perf: pieces drawn under the cores' 2D) */
#define GE_HASH 2048
static struct {
    uint32_t stamp, off;
    GeArgs a;   /* (dx..dh not compared) */
} sGeHash[GE_HASH];
static uint32_t sGeStamp;

static int ge_blit_f(MI_PHY src, int pw, int ph, const MI_GFX_Rect_t *sr, MI_PHY dst, int dw, int dh,
                     const MI_GFX_Rect_t *dr, int rotate, MI_U16 *fence, int rgb565);
static int ge_blit(MI_PHY src, int pw, int ph, const MI_GFX_Rect_t *sr, MI_PHY dst, int dw, int dh, const MI_GFX_Rect_t *dr,
                   int rotate, MI_U16 *fence) {
    return ge_blit_f(src, pw, ph, sr, dst, dw, dh, dr, rotate, fence, 0);
}
/* rgb565: the destination is a colour buffer (the cores' RGB565) instead of a screen page */
static int ge_blit_f(MI_PHY src, int pw, int ph, const MI_GFX_Rect_t *sr, MI_PHY dst, int dw, int dh,
                     const MI_GFX_Rect_t *dr, int rotate, MI_U16 *fence, int rgb565) {
    MI_GFX_Surface_t s = { src, E_MI_GFX_FMT_ARGB8888, (MI_U32) pw, (MI_U32) ph, (MI_U32) pw * 4 };
    MI_GFX_Surface_t d = { dst, rgb565 ? E_MI_GFX_FMT_RGB565 : E_MI_GFX_FMT_ARGB8888, (MI_U32) dw, (MI_U32) dh,
                           (MI_U32) dw * (rgb565 ? 2 : 4) };
    MI_GFX_Rect_t srr = *sr, drr = *dr;
    MI_GFX_Opt_t opt;
    memset(&opt, 0, sizeof opt);
    opt.stClipRect.s32Xpos = 0;
    opt.stClipRect.s32Ypos = 0;
    opt.stClipRect.u32Width = (MI_U32) dw;
    opt.stClipRect.u32Height = (MI_U32) dh;
    opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;           /* (premultiplied colour) */
    opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_INVSRCALPHA;
    opt.eMirror = E_MI_GFX_MIRROR_NONE;
    opt.eRotate = (MI_GFX_Rotate_e) rotate;
    opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_ALPHACHANNEL;
    return mi_blit(&s, &srr, &d, &drr, &opt, fence) != 0;
}

static int near_px(uint32_t got, uint32_t want) {
    for (int k = 0; k < 24; k += 8) {
        const int a = (int) ((got >> k) & 255), b = (int) ((want >> k) & 255);
        if (a - b > 6 || b - a > 6)
            return 0;
    }
    return 1;
}

/* where does a rectangle land, turned by 180 degrees, and is the blend right? */
static int ge_selftest(void) {
    MI_PHY ps = 0, pd = 0;
    void *vs = NULL, *vd = NULL;
    int ok = 0;
    char why[160] = "sem memoria";
    if (mi.MmaAlloc(NULL, 4096, &ps) || mi.MmaAlloc(NULL, 4096, &pd) || mi.Mmap(ps, 4096, &vs, TRUE) ||
        mi.Mmap(pd, 4096, &vd, TRUE))
        goto out;
    uint32_t *s = vs, *d = vd;
    /* 1: place. A 4x3 source, all blue but the picture at (1,1): red, green */
    for (int i = 0; i < 12; i++)
        s[i] = 0xFF0000FFu;
    s[5] = 0xFFFF0000u;
    s[6] = 0xFF00FF00u;
    memset(vd, 0, 4096);
    mi.Flush(vs, 4096);
    mi.Flush(vd, 4096);
    MI_GFX_Rect_t sr = { 1, 1, 2, 1 }, dr = { 1, 2, 2, 1 };
    MI_U16 fence = 0;
    if (ge_blit(ps, 4, 3, &sr, pd, 8, 8, &dr, E_MI_GFX_ROTATE_180, &fence)) {
        snprintf(why, sizeof why, "a copia com giro falhou");
        goto out;
    }
    mi.Wait(TRUE, fence);
    mi.Flush(vd, 4096);
    int nz = 0;
    for (int i = 0; i < 64; i++)
        nz += (d[i] & 0xFFFFFFu) != 0;
    if (nz == 2 && (d[2 * 8 + 1] & 0xFFFFFF) == 0x00FF00 && (d[2 * 8 + 2] & 0xFFFFFF) == 0xFF0000)
        ge_place = 0;
    else if (nz == 2 && (d[5 * 8 + 5] & 0xFFFFFF) == 0x00FF00 && (d[5 * 8 + 6] & 0xFFFFFF) == 0xFF0000)
        ge_place = 1;
    else {
        int n = snprintf(why, sizeof why, "lugar inesperado:");
        for (int i = 0; i < 64 && n < (int) sizeof why - 16; i++)
            if (d[i] & 0xFFFFFFu)
                n += snprintf(why + n, sizeof why - n, " (%d,%d)=%06x", i % 8, i / 8, (unsigned) (d[i] & 0xFFFFFF));
        goto out;
    }
    /* 2: blend, scaled 2x and turned. Half-transparent (premultiplied) over grey */
    for (int i = 0; i < 16; i++)
        s[i] = 0x80804000u;
    for (int i = 0; i < 64; i++)
        d[i] = 0xFF404040u;
    mi.Flush(vs, 4096);
    mi.Flush(vd, 4096);
    MI_GFX_Rect_t sr2 = { 1, 1, 2, 2 }, dr2 = { 1, 2, 4, 4 };
    if (ge_place) {
        dr2.s32Xpos = 8 - 1 - 4;
        dr2.s32Ypos = 8 - 2 - 4;
    }
    if (ge_blit(ps, 4, 4, &sr2, pd, 8, 8, &dr2, E_MI_GFX_ROTATE_180, &fence)) {
        snprintf(why, sizeof why, "a mistura falhou");
        goto out;
    }
    mi.Wait(TRUE, fence);
    mi.Flush(vd, 4096);
    const uint32_t want = 0xA06020u, bg = 0x404040u;
    int bad = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            /* the blit's rectangle on the page (turned when the engine moves it) */
            const int gx = ge_place ? 7 - x : x, gy = ge_place ? 7 - y : y;
            const int in = gx >= 1 && gx < 5 && gy >= 2 && gy < 6;
            if (!near_px(d[y * 8 + x], in ? want : bg))
                bad++;
        }
    if (bad) {
        snprintf(why, sizeof why, "mistura errada em %d pixels (ex. %06x, esperado %06x)", bad,
                 (unsigned) (d[(ge_place ? 4 : 3) * 8 + (ge_place ? 4 : 2)] & 0xFFFFFF), (unsigned) want);
        goto out;
    }
    /* 3: the same blend into an RGB565 colour buffer, not turned (the 2D under-layer of the
       menus, see port_ge_underlay_run): over 66,65,66 it must give about 161,96,33 */
    {
        uint16_t *d16 = vd;
        for (int i = 0; i < 64; i++)
            d16[i] = 0x4208;
        mi.Flush(vs, 4096);
        mi.Flush(vd, 4096);
        MI_GFX_Rect_t dr3 = { 2, 1, 4, 4 };
        ge_under = 0;
        if (ge_blit_f(ps, 4, 4, &sr2, pd, 8, 8, &dr3, E_MI_GFX_ROTATE_0, &fence, 1) == 0) {
            mi.Wait(TRUE, fence);
            mi.Flush(vd, 4096);
            int bad3 = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    const uint32_t c = d16[y * 8 + x];
                    const uint32_t c8 = rgb565_to_x8888(c) & 0xFFFFFF;
                    const int in = x >= 2 && x < 6 && y >= 1 && y < 5;
                    const uint32_t w8 = in ? 0xA16021u : (rgb565_to_x8888(0x4208) & 0xFFFFFF);
                    for (int k = 0; k < 24; k += 8) {
                        const int a = (int) ((c8 >> k) & 255), b = (int) ((w8 >> k) & 255);
                        if (a - b > 10 || b - a > 10)
                            bad3++;
                    }
                }
            ge_under = !bad3;
            if (bad3)
                printf("video: MI_GFX nao mistura certo na cor de 16 bits (%d erros, ex. %04x): 2D por baixo desligado\n",
                       bad3, (unsigned) d16[2 * 8 + 3]);
        }
    }
    ok = 1;
out:
    if (vs) mi.Munmap(vs, 4096);
    if (vd) mi.Munmap(vd, 4096);
    if (ps) mi.MmaFree(ps);
    if (pd) mi.MmaFree(pd);
    if (!ok)
        printf("video: 2D pelo MI_GFX desligado (teste: %s), os nucleos desenham\n", why);
    return ok;
}

static void ge_init(void) {
    if (!ge_selftest())
        return;
    static const uint32_t sizes[] = { 2u << 20, 1u << 20, 512u << 10 };
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int i;
        for (i = 0; i < 3; i++) {
            void *v = NULL;
            if (mi.MmaAlloc(NULL, sizes[k], &ge_phys[i]))
                break;
            if (mi.Mmap(ge_phys[i], sizes[k], &v, TRUE)) {
                mi.MmaFree(ge_phys[i]);
                break;
            }
            ge_virt[i] = v;
        }
        if (i == 3) {
            ge_cap = sizes[k];
            ge_ok = 1;
            printf("video: 2D (HUD, menus) pelo MI_GFX, %u KB por quadro%s\n", (unsigned) (ge_cap >> 10),
                   ge_place ? " (o MI_GFX gira o lugar)" : "");
            return;
        }
        while (i-- > 0) {
            mi.Munmap(ge_virt[i], sizes[k]);
            mi.MmaFree(ge_phys[i]);
            ge_virt[i] = NULL;
        }
    }
    printf("video: 2D pelo MI_GFX desligado (sem memoria MMA)\n");
}

static inline uint32_t mul255(uint32_t a, uint32_t b) {
    const uint32_t x = a * b + 128;
    return (x + (x >> 8)) >> 8;
}

/* one texel as the cores would blend it, premultiplied: c the vertex colour, o the offset */
static inline uint32_t ge_texel(uint32_t t, int mode, uint32_t c, uint32_t o) {
    uint32_t a = t >> 24, r = (t >> 16) & 255, g = (t >> 8) & 255, b = t & 255;
    const uint32_t cr = (c >> 16) & 255, cg = (c >> 8) & 255, cb = c & 255, ca = c >> 24;
    switch (mode) {
        case PORT_GE_FILL:
            r = cr; g = cg; b = cb; a = ca;
            break;
        case PORT_GE_MOD:
        case PORT_GE_MODA:
            r = mul255(r, cr);
            g = mul255(g, cg);
            b = mul255(b, cb);
            if (mode == PORT_GE_MODA)
                a = mul255(a, ca);
            break;
        case PORT_GE_DECAL:
            r = mul255(cr, 255 - a) + mul255(r, a);
            g = mul255(cg, 255 - a) + mul255(g, a);
            b = mul255(cb, 255 - a) + mul255(b, a);
            a = ca;
            break;
        default:
            break;
    }
    if (o && mode != PORT_GE_FILL) {
        r += (o >> 16) & 255;
        g += (o >> 8) & 255;
        b += o & 255;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
    }
    if (a != 255) {
        r = mul255(r, a);
        g = mul255(g, a);
        b = mul255(b, a);
    }
    return a << 24 | r << 16 | g << 8 | b;
}

/* a texel coordinate outside the texture, as the cores read it */
static inline int ge_wrap(int i, int n, int mode) {
    if (i >= 0 && i < n)
        return i;
    if (mode == PORT_GE_CLAMP)
        return i < 0 ? 0 : n - 1;
    if (mode == PORT_GE_MIRROR) {
        int k = i % (2 * n);
        if (k < 0)
            k += 2 * n;
        return k < n ? k : 2 * n - 1 - k;
    }
    int k = i % n;
    return k < 0 ? k + n : k;
}

int port_ge_hud_ok(void) {
    return hw_ok && ge_ok;
}

void port_ge_overlay_begin(void) {
    ge_sec = cur_idx;
    GeOvl *o = &sGeOvl[ge_sec];
    o->n = 0;
    o->nover = 0;
    o->used = 0;
    o->cur ^= 1;
    o->cn[o->cur] = 0;
    o->dmin = ~0u;
    o->dmax = 0;
    sGeStamp++;
}

/* the vertex colour at step k of n (the gradient's rows or columns, at their centres) */
static inline uint32_t ge_lerp(uint32_t c0, uint32_t c1, int k, int n) {
    if (c0 == c1 || n <= 1)
        return c0;
    const uint32_t f = (uint32_t) ((2 * k + 1) * 256 / (2 * n)), g = 256 - f;
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8)
        r |= ((((c0 >> s) & 255) * g + ((c1 >> s) & 255) * f + 128) >> 8) << s;
    return r;
}

/* (pieces partly off the screen stay on the cores: with the engine cutting them at the
   edges, patch 79, the whole system froze at the start of a race) */
int port_ge_clip_ok(void) {
    return 0;
}

static int ge_same(const GeArgs *x, const GeArgs *y) {
    return x->px == y->px && x->gen == y->gen && x->tw == y->tw && x->th == y->th && x->sx == y->sx &&
           x->sy == y->sy && x->sw == y->sw && x->sh == y->sh && x->wu == y->wu && x->wv == y->wv &&
           x->grad == y->grad && x->mode == y->mode && x->c0 == y->c0 && x->c1 == y->c1 && x->o0 == y->o0 &&
           x->o1 == y->o1;
}

int port_ge_overlay_add(const GeArgs *in) {
    if (ge_sec < 0 || !ge_ok)
        return -1;
    GeOvl *o = &sGeOvl[ge_sec];
    GeArgs a = *in;
    if (a.grad == PORT_GE_FLAT) {
        a.c1 = a.c0;
        a.o1 = a.o0;
    }
    if (a.mode == PORT_GE_TEX) {
        a.c0 = a.c1 = 0;
    } else if (a.mode == PORT_GE_MOD) {
        a.c0 |= 0xFF000000u;   /* (its alpha is not used) */
        a.c1 |= 0xFF000000u;
    } else if (a.mode == PORT_GE_FILL) {
        a.o0 = a.o1 = 0;
    }
    if (a.c0 == a.c1 && a.o0 == a.o1)
        a.grad = PORT_GE_FLAT;
    if (!a.px) {   /* a plain colour; a gradient: a strip of it, smoothed by the scaling */
        a.tw = a.th = 0;
        a.sx = a.sy = 0;
        a.sw = a.sh = 1;
        if (a.grad != PORT_GE_FLAT) {
            const int len = (a.grad == PORT_GE_ROWS ? a.dh : a.dw) / 2;
            const int n = len < 2 ? 2 : (len > 128 ? 128 : len);
            a.sw = a.grad == PORT_GE_ROWS ? 1 : n;
            a.sh = a.grad == PORT_GE_ROWS ? n : 1;
        }
    }
    if (o->n >= GE_MAX || a.sw < 1 || a.sh < 1 || a.sw > 1022 || a.sh > 1022)
        return -1;
    const int sw = a.sw, sh = a.sh;
    const int pw = (sw + 2 + 3) & ~3, ph = sh + 2;   /* (rows of a multiple of 16 bytes) */
    /* the same texels, colours and part already copied in this frame: shared */
    uint32_t h = a.gen * 2654435761u ^ a.c0 * 40503u ^ a.c1 * 2246822519u ^ a.o0 * 3266489917u ^ a.o1 * 668265263u ^
                 (uint32_t) ((a.sx & 0xFFFF) | a.sy << 16) * 97u ^ (uint32_t) (sw | sh << 10) * 31u ^
                 (uint32_t) (a.mode | a.grad << 4 | a.wu << 8 | a.wv << 10) ^ (uint32_t) (uintptr_t) a.px;
    h = (h ^ (h >> 15)) & (GE_HASH - 1);
    int slot = -1;
    for (int k = 0; k < 8; k++) {
        const uint32_t j = (h + k) & (GE_HASH - 1);
        if (sGeHash[j].stamp != sGeStamp) {
            slot = (int) j;
            break;
        }
        if (ge_same(&sGeHash[j].a, &a)) {
            slot = -2 - (int) j;
            break;
        }
    }
    uint32_t off;
    if (slot <= -2) {
        off = sGeHash[-2 - slot].off;
    } else {
        const uint32_t bytes = ((uint32_t) pw * ph * 4 + 63) & ~63u;
        if (o->used + bytes > ge_cap)
            return -1;
        off = o->used;
        o->used += bytes;
        const int c = o->cn[o->cur], pv = o->cur ^ 1;
        const int same = c < o->cn[pv] && o->coff[pv][c] == off && ge_same(&o->ca[pv][c], &a);
        if (c < GE_MAX) {
            o->ca[o->cur][c] = a;
            o->coff[o->cur][c] = off;
            o->cn[o->cur] = c + 1;
        }
        if (!same) {
        const double t0 = port_opt_perf ? port_now_ms() : 0;
        if (off < o->dmin)
            o->dmin = off;
        if (off + bytes > o->dmax)
            o->dmax = off + bytes;
        uint32_t *dst = (uint32_t *) (ge_virt[ge_sec] + off);
        static uint32_t colc[1024], colo[1024];   /* (left to right: the colours of each column) */
        static int colx[1024];                    /* (and its texel column) */
        for (int x = 0; x < sw; x++) {
            if (a.grad == PORT_GE_COLS) {
                colc[x] = ge_lerp(a.c0, a.c1, x, sw);
                colo[x] = ge_lerp(a.o0, a.o1, x, sw);
            }
            if (a.px)
                colx[x] = ge_wrap(a.sx + x, a.tw, a.wu);
        }
        /* (each row is made in ordinary memory and copied over in one go: written a texel at
           a time, the engine's memory took about 45 us a piece) */
        static uint32_t rowb[1024 + 4] __attribute__((aligned(16)));
        for (int y = 1; y <= sh; y++) {
            uint32_t *d = rowb;
            const uint32_t rc = a.grad == PORT_GE_ROWS ? ge_lerp(a.c0, a.c1, y - 1, sh) : a.c0;
            const uint32_t ro = a.grad == PORT_GE_ROWS ? ge_lerp(a.o0, a.o1, y - 1, sh) : a.o0;
            if (!a.px) {
                for (int x = 0; x < sw; x++)
                    d[x + 1] = ge_texel(0, PORT_GE_FILL, a.grad == PORT_GE_COLS ? colc[x] : rc, 0);
            } else {
                const uint32_t *s = a.px + (size_t) ge_wrap(a.sy + y - 1, a.th, a.wv) * a.tw;
                if (a.grad == PORT_GE_COLS)
                    for (int x = 0; x < sw; x++)
                        d[x + 1] = ge_texel(s[colx[x]], a.mode, colc[x], colo[x]);
                else
                    for (int x = 0; x < sw; x++)
                        d[x + 1] = ge_texel(s[colx[x]], a.mode, rc, ro);
            }
            d[0] = d[1];
            for (int x = sw + 1; x < pw; x++)
                d[x] = d[sw];
            memcpy(dst + (size_t) y * pw, rowb, (size_t) pw * 4);
            if (y == 1)   /* (the border rows: the first and last row again) */
                memcpy(dst, rowb, (size_t) pw * 4);
            if (y == sh)
                memcpy(dst + (size_t) (sh + 1) * pw, rowb, (size_t) pw * 4);
        }
        if (port_opt_perf)
            ge_copy_ms += port_now_ms() - t0;
        }
        if (slot >= 0) {
            sGeHash[slot].stamp = sGeStamp;
            sGeHash[slot].a = a;
            sGeHash[slot].off = off;
        }
    }
    const int dx = a.dx, dy = a.dy, dw = a.dw, dh = a.dh;
    GeItem *it = &o->it[o->n];
    it->off = off;
    it->pw = (uint16_t) pw;
    it->ph = (uint16_t) ph;
    it->sw = (uint16_t) sw;
    it->sh = (uint16_t) sh;
    it->dx = (int16_t) dx;
    it->dy = (int16_t) dy;
    it->dw = (int16_t) dw;
    it->dh = (int16_t) dh;
    it->on = 1;
    it->layer = (uint8_t) (in->layer != 0);
    if (!it->layer)
        o->nover++;
    return o->n++;
}

void port_ge_overlay_cancel(int idx) {
    if (ge_sec >= 0 && idx >= 0 && idx < sGeOvl[ge_sec].n)
        sGeOvl[ge_sec].it[idx].on = 0;
}

int port_ge_under_ok(void) {
    return hw_ok && ge_ok && ge_under;
}

/* the cores' 2D under-layer (menus): the engine's pieces of layer 1 into the colour buffer
   being drawn, at its size, after the 3D and before the cores' own 2D (gfx_sw.c,
   underlay_sync: both cores wait for it). The buffer is cached memory: written back before
   (the engine reads and blends into it) and dropped from the cache after. */
void port_ge_underlay_run(uint16_t *color, int w, int h) {
    if (ge_sec < 0 || !hw_ok || color != cbuf[ge_sec])
        return;
    GeOvl *o = &sGeOvl[ge_sec];
    if (o->dmax > o->dmin) {
        mi.Flush(ge_virt[ge_sec] + o->dmin, o->dmax - o->dmin);
        o->dmin = ~0u;
        o->dmax = 0;
    }
    const MI_U32 bytes = (MI_U32) w * h * 2;
    mi.Flush(color, bytes);
    MI_U16 fence = 0;
    int n = 0;
    for (int i = 0; i < o->n; i++) {
        const GeItem *it = &o->it[i];
        if (!it->on || !it->layer)
            continue;
        const int x0 = (it->dx * w + 320) / 640, x1 = ((it->dx + it->dw) * w + 320) / 640;
        const int y0 = (it->dy * h + 240) / 480, y1 = ((it->dy + it->dh) * h + 240) / 480;
        if (x1 <= x0 || y1 <= y0)
            continue;
        MI_GFX_Rect_t sr = { 1, 1, it->sw, it->sh };
        MI_GFX_Rect_t dr = { x0, y0, (MI_U32) (x1 - x0), (MI_U32) (y1 - y0) };
        if (ge_blit_f(ge_phys[ge_sec] + it->off, it->pw, it->ph, &sr, hw_phys[ge_sec], w, h, &dr, E_MI_GFX_ROTATE_0,
                      &fence, 1) == 0)
            n++;
    }
    if (n)
        mi.Wait(TRUE, fence);
    mi.Flush(color, bytes);
    ge_under_n += n;
}

/* flip thread: the pieces of part `sec` over screen page `back` (after the frame's copy) */
static int ge_draw(int sec, int back, MI_U16 *fq) {
    const GeOvl *o = &sGeOvl[sec];
    const MI_PHY page = (MI_PHY) fb_phys + (MI_PHY) back * PANEL_W * PANEL_H * 4;
    MI_U16 fence = 0;
    int n = 0;
    for (int i = 0; i < o->n; i++) {
        const GeItem *it = &o->it[i];
        if (!it->on || it->layer)
            continue;
        MI_GFX_Rect_t sr = { 1, 1, it->sw, it->sh };
        MI_GFX_Rect_t dr = { it->dx, it->dy, (MI_U32) it->dw, (MI_U32) it->dh };
        if (!ge_place) {   /* (the panel is upside down: the place turned here) */
            dr.s32Xpos = PANEL_W - it->dx - it->dw;
            dr.s32Ypos = PANEL_H - it->dy - it->dh;
        }
        if (ge_blit(ge_phys[sec] + it->off, it->pw, it->ph, &sr, page, PANEL_W, PANEL_H, &dr, hw_rotate, &fence) == 0)
            n++;
    }
    if (n)
        *fq = fence;   /* (waited for with the frame's copy, in the flip thread) */
    return n;
}

/* game thread, at present: the pieces go with this colour buffer (-1: none) */
static int ge_take(int idx) {
    if (ge_sec != idx || !hw_ok) {
        ge_sec = -1;
        return -1;
    }
    ge_sec = -1;
    GeOvl *o = &sGeOvl[idx];
    if (!o->nover)
        return -1;
    if (o->dmax > o->dmin)   /* (the new copies are in the CPU cache) */
        mi.Flush(ge_virt[idx] + o->dmin, o->dmax - o->dmin);
    return idx;
}
#else
int port_ge_hud_ok(void) {
    return 0;
}
void port_ge_overlay_begin(void) {
}
int port_ge_clip_ok(void) {
    return 0;
}
int port_ge_overlay_add(const GeArgs *a) {
    return -1;
}
int port_ge_under_ok(void) {
    return 0;
}
void port_ge_underlay_run(uint16_t *color, int w, int h) {
}
void port_ge_overlay_cancel(int idx) {
}
#endif

static int hw_init(size_t bytes) {
#ifdef HAVE_MI_GFX
    if (port_opt_nogfx || !fb_phys || fb_pages != 2)
        return 0;
    void *hs = open_lib("libmi_sys.so"), *hg = open_lib("libmi_gfx.so");
    if (!hs || !hg) {
        printf("video: sem libmi_sys/libmi_gfx, copia pela CPU\n");
        return 0;
    }
    mi.SysInit = dlsym(hs, "MI_SYS_Init");
    mi.MmaAlloc = dlsym(hs, "MI_SYS_MMA_Alloc");
    mi.MmaFree = dlsym(hs, "MI_SYS_MMA_Free");
    mi.Mmap = dlsym(hs, "MI_SYS_Mmap");
    mi.Munmap = dlsym(hs, "MI_SYS_Munmap");
    mi.Flush = dlsym(hs, "MI_SYS_FlushInvCache");
    mi.GfxOpen = dlsym(hg, "MI_GFX_Open");
    mi.GfxClose = dlsym(hg, "MI_GFX_Close");
    mi.Blit = dlsym(hg, "MI_GFX_BitBlit");
    mi.Wait = dlsym(hg, "MI_GFX_WaitAllDone");
    if (!mi.SysInit || !mi.MmaAlloc || !mi.MmaFree || !mi.Mmap || !mi.Munmap || !mi.Flush || !mi.GfxOpen ||
        !mi.GfxClose || !mi.Blit || !mi.Wait) {
        printf("video: MI_GFX incompleto, copia pela CPU\n");
        return 0;
    }
    if (mi.SysInit() != 0 || mi.GfxOpen() != 0) {
        printf("video: MI_GFX nao abriu, copia pela CPU\n");
        return 0;
    }
    if (!hw_selftest(port_render_w, port_render_h)) {
        printf("video: teste do MI_GFX falhou, copia pela CPU\n");
        mi.GfxClose();
        return 0;
    }
    hw_scale_probe();
    hw_size = (bytes + 4095) & ~(size_t) 4095;
    for (int i = 0; i < 3; i++) {
        void *v = NULL;
        if (mi.MmaAlloc(NULL, (MI_U32) hw_size, &hw_phys[i]) || mi.Mmap(hw_phys[i], (MI_U32) hw_size, &v, TRUE)) {
            printf("video: sem memoria MMA, copia pela CPU\n");
            for (int k = 0; k <= i; k++)
                if (hw_phys[k]) mi.MmaFree(hw_phys[k]);
            mi.GfxClose();
            return 0;
        }
        memset(v, 0, hw_size);
        free(cbuf[i]);
        cbuf[i] = v;
    }
    hw_ok = 1;
    printf("video: copia para a tela pelo MI_GFX (%s)\n", hw_rotate == E_MI_GFX_ROTATE_180 ? "giro 180" : "espelho");
    if (hw_rotate == E_MI_GFX_ROTATE_180 && hw_mirror == E_MI_GFX_MIRROR_NONE)
        ge_init();
    else
        printf("video: 2D pelo MI_GFX desligado (a tela so sai espelhada, e espelho nao escala)\n");
    return 1;
#else
    (void) bytes;
    return 0;
#endif
}

/* frame index -> show it in screen page `back` with the 2D engine; 0 = done */
static int hw_present(const uint16_t *src, int back, int w, int h, uint16_t *fq) {
#ifdef HAVE_MI_GFX
    int idx = src == cbuf[0] ? 0 : (src == cbuf[1] ? 1 : (src == cbuf[2] ? 2 : -1));
    if (!hw_ok || idx < 0)
        return -1;
    const size_t bytes = (size_t) w * h * 2;
    mi.Flush((void *) src, (MI_U32) bytes);   /* the drawing is in the CPU cache */
    MI_PHY dst = (MI_PHY) fb_phys + (MI_PHY) back * PANEL_W * PANEL_H * 4;
    if (hw_blit_q(hw_phys[idx], w, h, dst, PANEL_W, PANEL_H, hw_rotate, hw_mirror, (MI_U16 *) fq)) {
        printf("video: MI_GFX falhou no jogo, voltando para a copia pela CPU\n");
        hw_ok = 0;
        return -1;
    }
    return 0;
#else
    (void) src; (void) back; (void) w; (void) h; (void) fq;
    return -1;
#endif
}

/* -resdin: frames smaller than the buffers can only be shown through the 2D engine (it
   scales any size to the panel; the CPU copy only does 1x and 2x) */
int port_video_scales(void) {
    return hw_ok;
}

static void pin_cpu(int cpu) {
    unsigned long mask = 1UL << cpu;
    syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask);
}

/* The flip thread is also the game's 60 Hz clock: every pass of its loop ends at a
 * real vsync (FBIOPAN_DISPLAY waits for it), and then calls the vsync callback.
 * With a frame queued it shows that frame; otherwise it re-shows the current page,
 * which changes nothing on screen but still waits for the next vsync. So the game
 * ticks exactly with the panel (~16.75 ms on the Miyoo) and every 30 fps frame stays
 * on screen for exactly two refreshes. (A sleeping 60 Hz timer only wakes in 10 ms
 * steps on the Miyoo and drifts against the panel: frames lasted 1 to 3 refreshes.)
 * Everything touching /dev/fb0 stays in this one thread. */
static void (*vsync_cb)(void);
static void (*vsync_fallback)(void);

static void release_snap(void) {
    pthread_mutex_lock(&flip_mutex);
    snap_free = 1;
    pthread_cond_broadcast(&flip_cond);
    pthread_mutex_unlock(&flip_mutex);
}

/* CPU time this thread really used (the 2D engine works while the thread sleeps) */
static double thread_cpu_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* -perf watchdog (gfx_sw.c): where the flip thread is and how many passes it made */
volatile int port_flip_stage;
volatile unsigned port_flip_loops;

static void *flip_proc(void *arg) {
    (void) arg;
    double t_cpu = 0;
    double t_conv = 0, t_show = 0, last_print = 0, last_tick = 0, tmin = 1e9, tmax = 0, t_ge = 0, t_gen = 0, t_gecpu = 0;
    double p_prev = 0;           /* -perf, the rhythm: when the last frame was flipped */
    int p_hist[4] = { 0 }, p_late = 0;
    int frames = 0, ticks = 0, fast = 0;
    pin_cpu(1);
    for (;;) {
        port_flip_stage = 1;   /* waiting for the lock or a frame */
        port_flip_loops++;
        pthread_mutex_lock(&flip_mutex);
        /* not the clock: just wait for frames. Once it becomes the clock it must leave this
           wait even with no frame queued: with -paralelo the first frame is only shown after
           the game's next frame, and that one waits for the clock (it used to stay here
           until a frame came, so both waited for each other at startup) */
        while (!job_pending && !(vsync_cb && fb_pages == 2))
            pthread_cond_wait(&flip_cond, &flip_mutex);
        int job = job_pending;
        job_pending = 0;
        const uint16_t *src = snap;
        const int sw = snap_w, sh = snap_h, ge = snap_ge;
        pthread_mutex_unlock(&flip_mutex);

        double t0 = port_now_ms();
        if (fb_pages == 2) {
            if (job) {
                int back = fb_page ^ 1;
                const double c0 = port_opt_perf ? thread_cpu_ms() : 0;
                port_flip_stage = 2;   /* copying a frame */
                /* the next refresh, the one this frame should make (-perf: did it?) */
                double due = 0;
                if (last_tick != 0)
                    due = last_tick + 16.68 * (double) (int) ((t0 - last_tick) / 16.68 + 1.0);
                uint16_t fence = 0;
                const int hw = hw_present(src, back, sw, sh, &fence) == 0;   /* (queued: the engine works on it) */
                if (!hw)
                    convert_frame(src, sw, sh, (uint32_t *) (fb_mem + (size_t) back * PANEL_W * PANEL_H * 4));
#ifdef HAVE_MI_GFX
                /* the 2D over it, queued right behind the frame's copy: the calls into the
                   driver (about 35 us each) are made while the engine is still busy with the
                   copy, and one wait covers both (waiting for the copy first, then making the
                   calls, added ~1 ms between the frame being done and its flip; a frame that
                   came late enough then missed its refresh). Its part is only reused three
                   frames later, after this pass. */
                if (hw && ge >= 0) {
                    const double g0 = port_opt_perf ? port_now_ms() : 0, gc = port_opt_perf ? thread_cpu_ms() : 0;
                    MI_U16 gf = 0;
                    const int n = ge_draw(ge, back, &gf);
                    if (n)
                        fence = gf;
                    t_gen += n;
                    if (port_opt_perf) {
                        t_ge += port_now_ms() - g0;
                        t_gecpu += thread_cpu_ms() - gc;
                    }
                }
                if (hw)
                    mi.Wait(TRUE, fence);
#else
                (void) ge;
#endif
                release_snap();   /* copied: the game may queue the next frame during the vsync wait */
                if (port_opt_perf && due != 0 && port_now_ms() > due - 0.3)
                    p_late++;   /* (ready after the refresh it was meant for) */
                if (port_opt_perf)
                    t_cpu += thread_cpu_ms() - c0;
                double t1 = port_now_ms();
                t_conv += t1 - t0;
                fb_var.yoffset = back * PANEL_H;
                port_flip_stage = 3;   /* showing it (vsync) */
                ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
                fb_page = back;
                const double tp = port_now_ms();
                t_show += tp - t1;
                if (port_opt_perf) {   /* how many refreshes the frame before stayed on screen */
                    if (p_prev != 0) {
                        const int r = (int) ((tp - p_prev) / 16.68 + 0.5);
                        p_hist[r <= 1 ? 0 : (r >= 4 ? 3 : r - 1)]++;
                    }
                    p_prev = tp;
                }
            } else {
                fb_var.yoffset = fb_page * PANEL_H;   /* same page: just wait for the vsync */
                port_flip_stage = 4;   /* no frame: vsync */
                ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
            }
        } else if (fb_pages == 1 && job) {
            convert_frame(src, sw, sh, (uint32_t *) fb_mem);
            release_snap();
            t_conv += port_now_ms() - t0;
        } else if (job) {
            release_snap();   /* no screen: nothing to show */
        }
        if (job)
            frames++;

        if (vsync_cb && fb_pages == 2) {
            double now = port_now_ms();
            /* safety: if a flip did not wait for the vsync, this is no clock. Ignore the
               early pass; if it keeps happening, hand the clock back to a timer. */
            if (last_tick != 0 && now - last_tick < 8.0) {
                if (++fast > 30) {
                    printf("video: a troca de pagina nao espera o vsync; usando relogio de 60 Hz\n");
                    vsync_cb = NULL;
                    if (vsync_fallback)
                        vsync_fallback();
                }
                continue;
            }
            fast = 0;
            int missed = 0;
            if (last_tick != 0) {
                double d = now - last_tick;
                if (d < tmin) tmin = d;
                if (d > tmax) tmax = d;
                /* a pass that took more than one refresh (a long copy and 2D before the
                   flip, the menus' big pictures) went past a vsync without ticking: the
                   game's clock (its 30 fps beat, the sound) gets those ticks now instead of
                   losing them (it stuttered and the sound ran dry) */
                missed = (int) (d / 16.68 + 0.5) - 1;
                if (missed > 3)
                    missed = 3;
            }
            last_tick = now;
            port_flip_stage = 5;   /* game clock tick */
            for (; missed > 0; missed--) {
                ticks++;
                vsync_cb();
            }
            ticks++;
            vsync_cb();
            port_flip_stage = 6;
        }

        if (port_opt_perf) {
            double now = port_now_ms();
            if (last_print == 0)
                last_print = now;
            else if (now - last_print >= 2000.0) {
                fprintf(stderr, "perf flip: convert+copy %.2f (cpu %.2f)  pan(vsync) %.2f  ms/frame (%d frames) | vsync %d ticks, %.2f..%.2f ms\n",
                        frames ? t_conv / frames : 0, frames ? t_cpu / frames : 0, frames ? t_show / frames : 0, frames, ticks,
                        ticks ? tmin : 0, tmax);
#ifdef HAVE_MI_GFX
                if (t_gen > 0 || ge_copy_ms > 0 || ge_under_n > 0)
                    fprintf(stderr, "perf 2d MI_GFX: %.1f pecas/quadro por cima, %.1f por baixo do 2D dos nucleos, desenho %.2f ms/quadro (nucleo 1, cpu %.2f), copias %.2f ms/quadro (jogo)\n",
                            frames ? t_gen / frames : 0, frames ? (double) ge_under_n / frames : 0, frames ? t_ge / frames : 0,
                            frames ? t_gecpu / frames : 0, frames ? ge_copy_ms / frames : 0);
                ge_under_n = 0;
                ge_copy_ms = 0;
#endif
                fprintf(stderr, "perf ritmo: quadros na tela por 1 atualizacao %d, 2 %d, 3 %d, 4+ %d | prontos depois da sua atualizacao %d\n",
                        p_hist[0], p_hist[1], p_hist[2], p_hist[3], p_late);
                memset(p_hist, 0, sizeof p_hist);
                p_late = 0;
                t_conv = t_show = t_cpu = t_ge = t_gen = t_gecpu = 0;
                frames = ticks = 0;
                tmin = 1e9; tmax = 0;
                last_print = now;
            }
        }
    }
    return NULL;
}

/* 1 = the flip thread will call fn at every vsync; 0 = no page flipping, use a timer */
int port_video_set_vsync_clock(void (*fn)(void), void (*fallback)(void)) {
    if (fb_pages != 2)
        return 0;
    pthread_mutex_lock(&flip_mutex);
    vsync_fallback = fallback;
    vsync_cb = fn;
    pthread_cond_broadcast(&flip_cond);   /* wake it out of the frame-only wait */
    pthread_mutex_unlock(&flip_mutex);
    return 1;
}

static void fb_init(void) {
    struct fb_fix_screeninfo fix;
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        printf("video: sem /dev/fb0 (nada sera mostrado)\n");
        return;
    }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) ||
        fb_var.xres != PANEL_W || fb_var.yres != PANEL_H || fb_var.bits_per_pixel != 32 ||
        fix.line_length != PANEL_W * 4) {
        printf("video: framebuffer inesperado %ux%u %ubpp (nada sera mostrado)\n", fb_var.xres, fb_var.yres,
               fb_var.bits_per_pixel);
        close(fb_fd);
        fb_fd = -1;
        return;
    }
    fb_len = fix.smem_len;
    fb_phys = fix.smem_start;
    fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        printf("video: mmap falhou\n");
        fb_mem = NULL;
        close(fb_fd);
        fb_fd = -1;
        return;
    }
    if (fb_var.yres_virtual >= PANEL_H * 2 && fb_len >= (size_t) PANEL_W * PANEL_H * 4 * 2) {
        fb_pages = 2;
        fb_page = (fb_var.yoffset >= (unsigned) PANEL_H) ? 1 : 0;
        printf("video: vsync ligado (2 paginas)\n");
    } else {
        fb_pages = 1;
        printf("video: vsync desligado (framebuffer virtual %ux%u)\n", fb_var.xres_virtual, fb_var.yres_virtual);
    }
}

void port_video_init(int hires) {
    if (hires) {
        port_render_w = 640;
        port_render_h = 480;
    }
    size_t n = (size_t) port_render_w * port_render_h;
    for (int i = 0; i < 3; i++) {
        cbuf[i] = calloc(n, sizeof(uint16_t));
        if (!cbuf[i]) {
            fprintf(stderr, "video: sem memoria\n");
            exit(1);
        }
    }
    port_depth = calloc(n, sizeof(uint32_t));
    if (!port_depth) {
        fprintf(stderr, "video: sem memoria\n");
        exit(1);
    }
    cur_idx = 0;
    port_color = cbuf[0];
    printf("video: renderizando em %dx%d\n", port_render_w, port_render_h);

    fb_init();
    if (hw_init(n * sizeof(uint16_t)))
        port_color = cbuf[0];
    if (pthread_create(&flip_thread, NULL, flip_proc, NULL) == 0) {
        pthread_detach(flip_thread);
        flip_started = 1;
    }
}

void port_video_present(void) {
    if (!flip_started)
        return;
    port_battery_draw(cbuf[cur_idx], port_render_w, port_render_h);   /* (low battery: OnionOS's icon) */
    port_osd_draw(cbuf[cur_idx], port_render_w, port_render_h);       /* ("State saved"...) */
#ifdef HAVE_MI_GFX
    const int ge = ge_take(cur_idx);
#else
    const int ge = -1;
#endif
    pthread_mutex_lock(&flip_mutex);
    while (!snap_free)
        pthread_cond_wait(&flip_cond, &flip_mutex);
    snap = cbuf[cur_idx];
    snap_w = port_render_w;
    snap_h = port_render_h;
    snap_ge = ge;
    snap_free = 0;
    job_pending = 1;
    pthread_cond_broadcast(&flip_cond);
    pthread_mutex_unlock(&flip_mutex);

    last_idx = cur_idx;
    last_w = port_render_w;
    last_h = port_render_h;
    cur_idx = (cur_idx + 1) % 3;
    port_color = cbuf[cur_idx];
}

/* -perf: memory speed at startup (MB/s), to know how much the frame buffers cost */
void port_video_membench(void) {
    const size_t n = 1u << 20;   /* 1 MB */
    uint8_t *a = malloc(n), *b = malloc(n);
    if (!a || !b) { free(a); free(b); return; }
    memset(a, 1, n); memset(b, 2, n);
    double t0 = port_now_ms();
    for (int i = 0; i < 8; i++) memset(a, i, n);
    double t1 = port_now_ms();
    volatile uint32_t sink = 0;
    for (int i = 0; i < 8; i++) { const uint32_t *p = (const uint32_t *) b; uint32_t s = 0; for (size_t k = 0; k < n / 4; k += 8) s += p[k]; sink += s; }
    double t2 = port_now_ms();
    for (int i = 0; i < 8; i++) memcpy(a, b, n);
    double t3 = port_now_ms();
    double t4 = t3;
    if (fb_mem && fb_pages == 2) {
        uint8_t *page = fb_mem + (size_t) (fb_page ^ 1) * PANEL_W * PANEL_H * 4;
        for (int i = 0; i < 8; i++) memcpy(page, a, n);
        t4 = port_now_ms();
    }
    fprintf(stderr, "perf memoria (MB/s): memset %.0f  leitura %.0f  memcpy %.0f  copia p/ tela %.0f\n",
            8.0 / ((t1 - t0) / 1000.0), 8.0 / ((t2 - t1) / 1000.0), 8.0 / ((t3 - t2) / 1000.0),
            t4 > t3 ? 8.0 / ((t4 - t3) / 1000.0) : 0.0);
    (void) sink;
    free(a); free(b);
}

void port_video_shutdown(void) {
#ifdef HAVE_MI_GFX
    /* MMA memory is the kernel's: give it back */
    if (hw_ok) {
        hw_ok = 0;
        mi.Wait(TRUE, 0);
        /* the mappings stay until the process ends (another thread may still look at them) */
        for (int i = 0; i < 3; i++)
            mi.MmaFree(hw_phys[i]);
        mi.GfxClose();
    }
#endif
    /* back to page 0, where OnionOS draws */
    if (fb_fd >= 0 && fb_pages == 2 && fb_var.yoffset != 0) {
        fb_var.yoffset = 0;
        ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
    }
}

/* the last frame shown, for the Game Switcher's picture (savestate_miyoo.c) */
int port_video_last_frame(const uint16_t **px, int *w, int *h) {
    if (last_idx < 0)
        return 0;
    *px = cbuf[last_idx];
    *w = last_w;
    *h = last_h;
    return 1;
}

uint16_t port_fb_pixel565(int x, int y) {
    if (last_idx < 0)
        return 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > 319) x = 319;
    if (y > 239) y = 239;
    x = x * port_render_w / 320;
    y = y * port_render_h / 240;
    return cbuf[last_idx][y * port_render_w + x];
}
