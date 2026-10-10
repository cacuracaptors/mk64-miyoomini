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
float *port_depth;

static uint16_t *cbuf[3];
static int cur_idx;          /* buffer being drawn */
static int last_idx = -1;    /* last presented */

static int fb_fd = -1;
static struct fb_var_screeninfo fb_var;
static uint8_t *fb_mem;
static size_t fb_len;
static int fb_pages;         /* 2 = page flipping, 1 = draw straight into the shown page */
static int fb_page;
static uint32_t *outbuf;     /* PANEL_W x PANEL_H, flip thread only */

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

/* src (w x h, RGB565) -> dst (PANEL_W x PANEL_H, XRGB8888), scaled and rotated 180 degrees */
static void convert_frame(const uint16_t *src, int w, int h, uint32_t *dst) {
    if (w * 2 == PANEL_W && h * 2 == PANEL_H) {
        for (int y = 0; y < h; y++) {
            const uint16_t *s = src + y * w;
            uint32_t *d0 = dst + (PANEL_H - 1 - 2 * y) * PANEL_W;
            uint32_t *d = d0;
            for (int x = w - 1; x >= 0; x--) {
                uint32_t c = rgb565_to_x8888(s[x]);
                d[0] = c;
                d[1] = c;
                d += 2;
            }
            memcpy(d0 - PANEL_W, d0, PANEL_W * 4);
        }
    } else {
        for (int y = 0; y < PANEL_H; y++) {
            const uint16_t *s = src + (y * h / PANEL_H) * w;
            uint32_t *d = dst + (PANEL_H - 1 - y) * PANEL_W;
            for (int x = 0; x < PANEL_W; x++)
                d[PANEL_W - 1 - x] = rgb565_to_x8888(s[x * w / PANEL_W]);
        }
    }
}

static void pin_cpu(int cpu) {
    unsigned long mask = 1UL << cpu;
    syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask);
}

static void *flip_proc(void *arg) {
    (void) arg;
    double t_conv = 0, t_show = 0, last_print = 0;
    int frames = 0;
    pin_cpu(1);
    for (;;) {
        pthread_mutex_lock(&flip_mutex);
        while (!job_pending)
            pthread_cond_wait(&flip_cond, &flip_mutex);
        job_pending = 0;
        const uint16_t *src = snap;
        pthread_mutex_unlock(&flip_mutex);

        double t0 = port_now_ms();
        if (fb_pages == 2) {
            convert_frame(src, port_render_w, port_render_h, outbuf);
            int back = fb_page ^ 1;
            memcpy(fb_mem + (size_t) back * PANEL_W * PANEL_H * 4, outbuf, (size_t) PANEL_W * PANEL_H * 4);
            double t1 = port_now_ms();
            t_conv += t1 - t0;
            fb_var.yoffset = back * PANEL_H;
            ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
            fb_page = back;
            t_show += port_now_ms() - t1;
        } else if (fb_pages == 1) {
            convert_frame(src, port_render_w, port_render_h, (uint32_t *) fb_mem);
            t_conv += port_now_ms() - t0;
        }

        pthread_mutex_lock(&flip_mutex);
        snap_free = 1;
        pthread_cond_broadcast(&flip_cond);
        pthread_mutex_unlock(&flip_mutex);

        if (port_opt_perf) {
            double now = port_now_ms();
            frames++;
            if (last_print == 0)
                last_print = now;
            else if (now - last_print >= 2000.0) {
                fprintf(stderr, "perf flip: convert+copy %.2f  pan(vsync) %.2f  ms/frame (%d frames)\n",
                        t_conv / frames, t_show / frames, frames);
                t_conv = t_show = 0;
                frames = 0;
                last_print = now;
            }
        }
    }
    return NULL;
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
    port_depth = calloc(n, sizeof(float));
    outbuf = malloc((size_t) PANEL_W * PANEL_H * 4);
    if (!port_depth || !outbuf) {
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

int port_video_wait_vsync(void) {
    uint32_t arg = 0;
    if (fb_fd < 0)
        return -1;
    return ioctl(fb_fd, FBIO_WAITFORVSYNC, &arg) == 0 ? 0 : -1;
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
