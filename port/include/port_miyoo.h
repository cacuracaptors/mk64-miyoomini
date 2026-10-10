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

/* src/main.c (TARGET_MIYOO) */
void port_audio_pump(void);

/* port/video_miyoo.c: the software renderer draws into port_color / port_depth */
extern int port_render_w, port_render_h;
extern uint16_t *port_color;   /* RGB565, port_render_w x port_render_h */
extern float *port_depth;      /* 1/w, larger = nearer, cleared to 0 */
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
