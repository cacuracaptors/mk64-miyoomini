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

static int hw_blit(MI_PHY src, int sw, int sh, MI_PHY dst, int dw, int dh, int rotate, int mirror) {
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
    if (mi.Blit(&s, &sr, &d, &dr, &opt, &fence) != 0)
        return -1;
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

/* colour buffers in MMA memory when the 2D engine works; 0 = keep the malloc ones */
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
    return 1;
#else
    (void) bytes;
    return 0;
#endif
}

/* frame index -> show it in screen page `back` with the 2D engine; 0 = done */
static int hw_present(const uint16_t *src, int back) {
#ifdef HAVE_MI_GFX
    int idx = src == cbuf[0] ? 0 : (src == cbuf[1] ? 1 : (src == cbuf[2] ? 2 : -1));
    if (!hw_ok || idx < 0)
        return -1;
    const size_t bytes = (size_t) port_render_w * port_render_h * 2;
    mi.Flush((void *) src, (MI_U32) bytes);   /* the drawing is in the CPU cache */
    MI_PHY dst = (MI_PHY) fb_phys + (MI_PHY) back * PANEL_W * PANEL_H * 4;
    if (hw_blit(hw_phys[idx], port_render_w, port_render_h, dst, PANEL_W, PANEL_H, hw_rotate, hw_mirror)) {
        printf("video: MI_GFX falhou no jogo, voltando para a copia pela CPU\n");
        hw_ok = 0;
        return -1;
    }
    return 0;
#else
    (void) src; (void) back;
    return -1;
#endif
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

static void *flip_proc(void *arg) {
    (void) arg;
    double t_cpu = 0;
    double t_conv = 0, t_show = 0, last_print = 0, last_tick = 0, tmin = 1e9, tmax = 0;
    int frames = 0, ticks = 0, fast = 0;
    pin_cpu(1);
    for (;;) {
        pthread_mutex_lock(&flip_mutex);
        if (!vsync_cb || fb_pages != 2) {   /* not the clock: just wait for frames */
            while (!job_pending)
                pthread_cond_wait(&flip_cond, &flip_mutex);
        }
        int job = job_pending;
        job_pending = 0;
        const uint16_t *src = snap;
        pthread_mutex_unlock(&flip_mutex);

        double t0 = port_now_ms();
        if (fb_pages == 2) {
            if (job) {
                int back = fb_page ^ 1;
                const double c0 = port_opt_perf ? thread_cpu_ms() : 0;
                if (hw_present(src, back))
                    convert_frame(src, port_render_w, port_render_h, (uint32_t *) (fb_mem + (size_t) back * PANEL_W * PANEL_H * 4));
                release_snap();   /* copied: the game may queue the next frame during the vsync wait */
                if (port_opt_perf)
                    t_cpu += thread_cpu_ms() - c0;
                double t1 = port_now_ms();
                t_conv += t1 - t0;
                fb_var.yoffset = back * PANEL_H;
                ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
                fb_page = back;
                t_show += port_now_ms() - t1;
            } else {
                fb_var.yoffset = fb_page * PANEL_H;   /* same page: just wait for the vsync */
                ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
            }
        } else if (fb_pages == 1 && job) {
            convert_frame(src, port_render_w, port_render_h, (uint32_t *) fb_mem);
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
            if (last_tick != 0) {
                double d = now - last_tick;
                if (d < tmin) tmin = d;
                if (d > tmax) tmax = d;
            }
            last_tick = now;
            ticks++;
            vsync_cb();
        }

        if (port_opt_perf) {
            double now = port_now_ms();
            if (last_print == 0)
                last_print = now;
            else if (now - last_print >= 2000.0) {
                fprintf(stderr, "perf flip: convert+copy %.2f (cpu %.2f)  pan(vsync) %.2f  ms/frame (%d frames) | vsync %d ticks, %.2f..%.2f ms\n",
                        frames ? t_conv / frames : 0, frames ? t_cpu / frames : 0, frames ? t_show / frames : 0, frames, ticks,
                        ticks ? tmin : 0, tmax);
                t_conv = t_show = t_cpu = 0;
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
    pthread_mutex_lock(&flip_mutex);
    while (!snap_free)
        pthread_cond_wait(&flip_cond, &flip_mutex);
    snap = cbuf[cur_idx];
    snap_free = 0;
    job_pending = 1;
    pthread_cond_broadcast(&flip_cond);
    pthread_mutex_unlock(&flip_mutex);

    last_idx = cur_idx;
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
