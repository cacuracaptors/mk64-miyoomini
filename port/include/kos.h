/*
 * kos.h -- stand-in for the KallistiOS umbrella header (Miyoo Mini port).
 *
 * Most game files include <kos.h> only for the C library. The few KOS calls
 * that remain outside the Dreamcast-only files are declared here and
 * implemented in port/kos_shim.c. The Dreamcast-only files themselves
 * (gfx_pvr.c, gfx_dc.c, dcaudio/driver.c, vmu.c, dcprofiler.c, aica code)
 * are not built for the Miyoo.
 */
#ifndef PORT_KOS_H
#define PORT_KOS_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- dc/vec3f.h: normalize three float lvalues in place ---- */
#define vec3f_normalize(x, y, z) do {                                       \
        float __vn_inv = 1.0f / sqrtf((x) * (x) + (y) * (y) + (z) * (z));  \
        (x) *= __vn_inv; (y) *= __vn_inv; (z) *= __vn_inv;                 \
    } while (0)

/* ---- monotonic timers ---- */
uint64_t timer_ms_gettime64(void);
uint64_t timer_us_gettime64(void);
uint64_t timer_ns_gettime64(void);

/* ---- threads ---- */
void thd_pass(void);
void thd_sleep(unsigned int ms);

/* ---- genwait: one shared condition variable (the object is ignored) ---- */
int  genwait_wait(void *obj, const char *mesg, int timeout_ms, void (*callback)(void *));
int  genwait_wake_all(void *obj);

/* ---- video / debug output: nothing to do on the Miyoo ---- */
#define vid_border_color(r, g, b) ((void)0)
#define dbgio_enable()            ((void)0)
#define dbgio_disable()           ((void)0)

/* ---- PowerVR texture format tags, still used by the Fast3D front-end as
        plain identifiers for the 16-bit layouts it hands to the backend ---- */
#define PVR_TXRFMT_ARGB1555     (0u << 27)
#define PVR_TXRFMT_RGB565       (1u << 27)
#define PVR_TXRFMT_ARGB4444     (2u << 27)
#define PVR_TXRFMT_NONTWIDDLED  (1u << 26)

#ifdef __cplusplus
}
#endif

#endif /* PORT_KOS_H */
