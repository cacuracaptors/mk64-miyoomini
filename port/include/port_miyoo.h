/* Miyoo Mini (Plus) port layer: shared declarations */
#ifndef PORT_MIYOO_H
#define PORT_MIYOO_H

#include <stdint.h>

/* port/port_main.c */
void port_chdir_to_exe(void);
/* first run: dc_data/ from the player's ROM (mk64.rcp); every run: the executable's game data */
void port_setup(void);
void port_init(int argc, char **argv);
void port_start_vblank(void (*fn)(uint32_t, void *));
void port_poll_quit(void);
void port_quit(int code);
double port_now_ms(void);
extern int port_opt_perf;
extern int port_opt_60fps;   /* -60fps: 1P races not capped at 30 (experimental) */
extern int port_opt_filter;  /* -filtro: 3-point filter on magnified 3D opaque surfaces */
extern int port_opt_filter_test; /* -filtroteste: the filter modes take turns (measurement) */
extern int port_opt_filter_lite;
extern int port_opt_pf_level;
extern int port_opt_filter_all;
extern int port_opt_near_elim;  /* -filtroperto: the exact filter right in front of the camera (step, 16.16) */ /* -qualidade: the N64's filter on every texture that asks for it */
extern int port_opt_quality;    /* quality mode: 30 fps, 640x480, the N64's filter everywhere */
void port_apply_mode(int quality);   /* port_main.c: swap between performance and quality */  /* -filtroleve: which grid of cells (see the span) */ /* -filtroleve: only the filter worked out in advance (cheaper) */
extern int port_opt_nogfx;   /* -semgfx: frame copy by the CPU instead of the 2D engine */
extern int port_opt_parallel; /* on unless -semparalelo: core 1 draws a frame while the game prepares the next (shown 1 frame later) */
extern int port_opt_mute;     /* -mudo: no sound */
extern int port_opt_exact;    /* -exato: no shortcuts that change the picture slightly */
extern int gfx_sw_presents;   /* port/gfx_sw.c: with -paralelo it shows the frames itself */
extern volatile int port_flip_stage, port_main_pacing;   /* -perf watchdog (port/gfx_sw.c) */
extern volatile unsigned port_flip_loops;
extern double port_pacing_ms;        /* gfx_wm_miyoo.c: time waited for the 30 fps beat */
int port_video_scales(void);         /* video_miyoo.c: any frame size can be shown (2D engine) */
extern int port_opt_resdin;          /* -resdin: dynamic resolution */

/* src/main.c (TARGET_MIYOO) */
void port_audio_pump(void);

/* port/video_miyoo.c: the software renderer draws into port_color / port_depth */
extern int port_render_w, port_render_h;
extern uint16_t *port_color;   /* RGB565, port_render_w x port_render_h */
extern uint32_t *port_depth;   /* 1/w x 2^28, larger = nearer, cleared to 0 */
void port_video_init(int hires);
void port_video_present(void);
void port_video_shutdown(void);
void port_video_membench(void);
int port_video_set_vsync_clock(void (*fn)(void), void (*fallback)(void));   /* 1 = fn runs at every real vsync */
uint16_t port_fb_pixel565(int x, int y);   /* last presented frame, N64 320x240 coords */

/* port/input_miyoo.c */
void port_input_init(void);
void port_input_poll(void);
int port_input_quit_requested(void);

/* one frame's times (ms) in the renderer, for the hitch log (gfx_sw.c, gfx_wm_miyoo.c) */
typedef struct {
    double walk, band, wait, present, hand, pf, up;   /* display list, core 0's bands, waiting for core 1, showing, handing over (pf: filter built in advance, up: texture uploads) */
    int pfn, upn, res;
} SwFrameStats;
void gfx_sw_frame_stats(SwFrameStats *out);
extern double port_audio_total_ms;   /* all the time spent mixing the sound (src/main.c) */

#endif
