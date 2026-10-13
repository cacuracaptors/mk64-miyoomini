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

/* Hitch log (always on): a stretch of slow frames that lost more than 40 ms against
   60 fps (one frame over 50 ms, or several of 2 screen refreshes in a row), with when it
   happened (t, the same clock as the sound's gap lines) and where its time went. The
   game and the sound run on this thread. "resto" is the game itself plus anything not
   measured; "cpu" is the time this thread really ran (much less than the stretch: it was
   kept waiting), "faltas" the pages the system had to set up for it (from the SD card:
   "do cartao"). */
#include <sys/resource.h>
#include <time.h>
extern int32_t gGamestate;
extern int16_t gCurrentCourseId;
static int sHitches, sHitchLines;
typedef struct {
    int n, pfn, upn, res;
    double t0, total, max, walk, band, wait, present, hand, pf, up, audio, cpu;
    long minf, majf;
} HitchRun;
static HitchRun sRun;
static void hitch_print(const HitchRun *r) {
    sHitches++;
    if (sHitchLines >= 300)
        return;
    sHitchLines++;
    const double known = r->walk + r->band + r->wait + r->present + r->hand + r->audio;   /* (uploads are inside the list) */
    fprintf(stderr, "engasgo: t=%.0f ms, %d quadro(s) em %.0f ms (o maior %.0f, perdeu %.0f) estado %d pista %d res %d | "
            "lista %.1f (texturas %d: %.1f) nucleo0 %.1f espera nucleo1 %.1f mostrar %.1f entrega %.1f (filtro feito %d: %.1f) "
            "som %.1f resto %.1f | cpu %.0f ms faltas %ld do cartao %ld\n",
            r->t0, r->n, r->total, r->max, r->total - r->n * 16.68, (int) gGamestate, (int) gCurrentCourseId, r->res,
            r->walk, r->upn, r->up, r->band, r->wait, r->present, r->hand, r->pfn, r->pf, r->audio, r->total - known,
            r->cpu, r->minf, r->majf);
}
static void hitch_check(double now) {
    static double last, last_audio, last_cpu;
    static long last_min, last_maj;
    struct rusage ru;
    struct timespec ts;
    getrusage(RUSAGE_SELF, &ru);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    const double cpu = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
    SwFrameStats fs;
    gfx_sw_frame_stats(&fs);
    const double dt = now - last;
    if (last > 0) {
        if (dt > 20.0) {   /* a slow frame: part of the stretch */
            HitchRun *r = &sRun;
            if (!r->n)
                r->t0 = last;
            r->n++;
            r->total += dt;
            if (dt > r->max) r->max = dt;
            r->walk += fs.walk; r->band += fs.band; r->wait += fs.wait; r->present += fs.present;
            r->hand += fs.hand; r->pf += fs.pf; r->pfn += fs.pfn; r->up += fs.up; r->upn += fs.upn;
            r->res = fs.res;
            r->audio += port_audio_total_ms - last_audio;
            r->cpu += cpu - last_cpu;
            r->minf += ru.ru_minflt - last_min;
            r->majf += ru.ru_majflt - last_maj;
        } else if (sRun.n) {   /* the stretch is over */
            if (sRun.total - sRun.n * 16.68 > 40.0)
                hitch_print(&sRun);
            memset(&sRun, 0, sizeof(sRun));
        }
    }
    last = now;
    last_audio = port_audio_total_ms;
    last_cpu = cpu;
    last_min = ru.ru_minflt;
    last_maj = ru.ru_majflt;
}

static void wm_swap_buffers_begin(void) {
    double t0 = port_now_ms();
    hitch_check(t0);
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
            fprintf(stderr, "perf main: %.1f fps  wait-for-flip %.2f ms/frame  som %.2f ms/frame  engasgos %d\n",
                    perf_frames * 1000.0 / (now - perf_last), perf_work / perf_frames, port_audio_ms / perf_frames, sHitches);
            sHitches = 0;
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
