/* Miyoo Mini (Plus) port layer: shared declarations */
#ifndef PORT_MIYOO_H
#define PORT_MIYOO_H

#include <stdint.h>

/* port/port_main.c */
void port_chdir_to_exe(void);
void port_init(int argc, char **argv);
void port_start_vblank(void (*fn)(uint32_t, void *));
void port_poll_quit(void);
void port_quit(int code);
double port_now_ms(void);
extern int port_opt_perf;
extern int port_opt_60fps;   /* -60fps: 1P races not capped at 30 (experimental) */
extern int port_opt_filter;  /* -filtro: 3-point filter on magnified 3D opaque surfaces */
extern int port_opt_nogfx;   /* -semgfx: frame copy by the CPU instead of the 2D engine */
extern int port_opt_parallel; /* -paralelo: core 1 draws a frame while the game prepares the next (shown 1 frame later) */
extern int gfx_sw_presents;   /* port/gfx_sw.c: with -paralelo it shows the frames itself */

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

#endif
