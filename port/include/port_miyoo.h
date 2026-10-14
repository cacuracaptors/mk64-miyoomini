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
extern int port_opt_filter_lite; /* -filtroleve: only the filter worked out in advance (cheaper) */
extern int port_opt_pf_level;    /* -filtroleve: which grid of cells (see the span) */
extern int port_opt_near_elim;   /* -filtroleve: the exact filter right in front of the camera (step, 16.16) */
extern int port_opt_nogfx;   /* -semgfx: frame copy by the CPU instead of the 2D engine */
extern int port_opt_parallel; /* on unless -semparalelo: core 1 draws a frame while the game prepares the next (shown 1 frame later) */
extern int port_opt_mute;     /* -mudo: no sound */
extern int port_opt_exact;    /* -exato: no shortcuts that change the picture slightly */
extern int port_opt_nohud;    /* -semhud: (measurement) races without the HUD */
extern int port_opt_hudcpu;   /* -hudcpu: the 2D drawn by the cores, not by the 2D engine */
extern int gfx_sw_presents;   /* port/gfx_sw.c: with -paralelo it shows the frames itself */
extern volatile int port_flip_stage, port_main_pacing;   /* -perf watchdog (port/gfx_sw.c) */
extern volatile unsigned port_flip_loops;
extern double port_pacing_ms;        /* gfx_wm_miyoo.c: time waited for the 30 fps beat */
int port_video_scales(void);         /* video_miyoo.c: any frame size can be shown (2D engine) */
/* video_miyoo.c: the 2D of a frame (HUD, menus) drawn by the 2D engine over the finished
   picture, at 640x480 (see hud_ge_build in gfx_sw.c). mode: 0 the texture, 1 its colour
   times the vertex colour, 2 colour and alpha times the vertex colour, 3 the vertex colour
   alone (no texture). gen: which upload of the texels (the same gen, the same texels). */
#define PORT_GE_TEX 0
#define PORT_GE_MOD 1
#define PORT_GE_MODA 2
#define PORT_GE_FILL 3
int port_ge_hud_ok(void);
void port_ge_overlay_begin(void);
int port_ge_overlay_add(const uint32_t *px, int tw, uint32_t gen, int sx, int sy, int sw, int sh, int dx, int dy, int dw,
                        int dh, uint32_t color, int mode);   /* index, or -1: no room */
void port_ge_overlay_cancel(int idx);
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
int port_video_last_frame(const uint16_t **px, int *w, int *h);   /* the last frame shown (0: none yet) */

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

/* battery_miyoo.c: OnionOS's low battery warning and shutdown, done by the game */
void port_battery_init(void);
void port_battery_release(void);   /* (async-signal-safe) */
void port_battery_draw(uint16_t *buf, int w, int h);
extern volatile int port_battery_exit;

/* savestate_miyoo.c: save states, OnionOS's Game Switcher, the save at power-off */
enum { PORT_REQ_SAVE, PORT_REQ_LOAD };
enum { PORT_MENU_SINGLE, PORT_MENU_LONG, PORT_MENU_DOUBLE };
void port_state_init(const char *argv0);
void port_state_boundary(void);       /* between two frames: loads, saves, the ways out */
void port_state_request(int what);    /* PORT_REQ_*: done at the next boundary */
void port_state_menu(int press);      /* PORT_MENU_*: what OnionOS's Tweaks set for it */
int port_state_menu_double_used(void);   /* a double press does something (the single press waits for it) */
int port_state_take_sigterm(void);    /* (signal handler) 1 = saved and closed at the next frame */
void port_state_release(void);        /* (async-signal-safe) */
void port_state_disable(const char *why);
void port_osd_draw(uint16_t *buf, int w, int h);   /* the state messages, on the frame shown */

#endif
