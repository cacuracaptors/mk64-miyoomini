/*
 * Miyoo Mini (Plus) "window manager" for the Fast3D front-end. Keeps the DC
 * symbol name (gfx_dc) so src/main.c needs no change. The front-end keeps
 * working in 640x480 screen coordinates; the software renderer scales them
 * to its own buffer size.
 */
#include <stdint.h>
#include <kos.h>
#include "gfx/gfx_window_manager_api.h"
#include "port_miyoo.h"

volatile int port_main_pacing;   /* -perf watchdog: waiting for the 30 fps beat */
double port_pacing_ms;           /* time spent waiting for the 30 fps beat (gfx_sw.c -resdin) */
int force_30fps = 1;   /* 1 = vblank-locked 30 fps (set by game code per mode), 0 = uncapped */

extern volatile uint64_t vblticker;
static uint64_t last_ticker;

static void wm_init(const char *game_name, uint8_t start_in_fullscreen) {
    (void) game_name;
    (void) start_in_fullscreen;
}

static void wm_set_keyboard_callbacks(uint8_t (*on_key_down)(int), uint8_t (*on_key_up)(int), void (*on_all_keys_up)(void)) {
    (void) on_key_down;
    (void) on_key_up;
    (void) on_all_keys_up;
}

static void wm_set_fullscreen_changed_callback(void (*cb)(uint8_t)) {
    (void) cb;
}

static void wm_set_fullscreen(uint8_t enable) {
    (void) enable;
}

static void wm_main_loop(void (*run_one_game_iter)(void)) {
    for (;;)
        run_one_game_iter();
}

static void wm_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = 640;
    *height = 480;
}

static void wm_handle_events(void) {
}

static uint8_t wm_start_frame(void) {
    return 1;
}

static double perf_last, perf_work;
static int perf_frames;

static void wm_swap_buffers_begin(void) {
    double t0 = port_now_ms();
    if (!gfx_sw_presents)   /* (-paralelo: shown from gfx_sw's end_frame) */
        port_video_present();
    if (port_opt_perf) {
        extern void gfx_sw_perf_report(int frames, double ms);
        double now = port_now_ms();
        perf_frames++;
        if (perf_last == 0)
            perf_last = now;
        else if (now - perf_last >= 2000.0) {
            gfx_sw_perf_report(perf_frames, now - perf_last);
            extern double port_audio_ms;
            fprintf(stderr, "perf main: %.1f fps  wait-for-flip %.2f ms/frame  som %.2f ms/frame\n",
                    perf_frames * 1000.0 / (now - perf_last), perf_work / perf_frames, port_audio_ms / perf_frames);
            port_audio_ms = 0;
            perf_frames = 0;
            perf_work = 0;
            perf_last = now;
        }
        perf_work += now - t0;
    }
}

/* 30 fps pacing on the 60 Hz tick, exactly like the DC build: hold each frame
 * to a constant 2-tick beat; a slow frame resyncs instead of running a debt. */
static void wm_swap_buffers_end(void) {
    if (force_30fps) {
        const uint64_t target = last_ticker + 2;
        port_main_pacing = 1;
        const double tw = port_now_ms();
        while (vblticker < target)
            genwait_wait((void *) &vblticker, NULL, 100, NULL);   /* timeout: never hang on a dead clock */
        port_pacing_ms += port_now_ms() - tw;
        port_main_pacing = 0;
        last_ticker = (vblticker > target) ? vblticker : target;
    }
}

static double wm_get_time(void) {
    return port_now_ms() / 1000.0;
}

struct GfxWindowManagerAPI gfx_dc = {
    wm_init,
    wm_set_keyboard_callbacks,
    wm_set_fullscreen_changed_callback,
    wm_set_fullscreen,
    wm_main_loop,
    wm_get_dimensions,
    wm_handle_events,
    wm_start_frame,
    wm_swap_buffers_begin,
    wm_swap_buffers_end,
    wm_get_time,
};
