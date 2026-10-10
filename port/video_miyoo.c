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

#include "port_miyoo.h"

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
static uint32_t line_buf[PANEL_W];

static void convert_frame(const uint16_t *src, int w, int h, uint32_t *dst) {
    if (w * 2 == PANEL_W && h * 2 == PANEL_H) {
        for (int y = 0; y < h; y++) {
            const uint16_t *s = src + y * w;
            uint32_t *d = line_buf;
            for (int x = w - 1; x >= 0; x--) {
                uint32_t c = rgb565_to_x8888(s[x]);
                d[0] = c;
                d[1] = c;
                d += 2;
            }
            uint32_t *row = dst + (PANEL_H - 1 - 2 * y) * PANEL_W;
            memcpy(row, line_buf, PANEL_W * 4);
            memcpy(row - PANEL_W, line_buf, PANEL_W * 4);
        }
    } else if (w == PANEL_W && h == PANEL_H) {
        for (int y = 0; y < h; y++) {
            const uint16_t *s = src + y * w;
            uint32_t *d = line_buf;
            for (int x = w - 1; x >= 0; x--)
                *d++ = rgb565_to_x8888(s[x]);
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

static void *flip_proc(void *arg) {
    (void) arg;
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
                convert_frame(src, port_render_w, port_render_h, (uint32_t *) (fb_mem + (size_t) back * PANEL_W * PANEL_H * 4));
                release_snap();   /* copied: the game may queue the next frame during the vsync wait */
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
                fprintf(stderr, "perf flip: convert+copy %.2f  pan(vsync) %.2f  ms/frame (%d frames) | vsync %d ticks, %.2f..%.2f ms\n",
                        frames ? t_conv / frames : 0, frames ? t_show / frames : 0, frames, ticks, ticks ? tmin : 0, tmax);
                t_conv = t_show = 0;
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
