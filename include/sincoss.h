#ifndef SINCOSS_H
#define SINCOSS_H

#include <PR/ultratypes.h>

/*
 * sin/cos for a u16 binary angle (full circle = 0x10000), centralized here
 * (these were duplicated as `static inline` across ~10 translation units).
 *
 * Change the trig implementation in ONE place via SINCOSS_USE_FSCA:
 *   1 = SH4 `fsca` hardware instruction (current Dreamcast behavior)
 *   0 = N64-faithful sine table, routed through sins()/coss() (gSineTable).
 *       Same angle convention, so this is an apples-to-apples A/B test of
 *       whether `fsca`'s values are behind the speed deficit.
 */
#ifndef SINCOSS_USE_FSCA
#define SINCOSS_USE_FSCA 1
#endif

#if SINCOSS_USE_FSCA

static inline void sincoss(u16 arg0, f32* s, f32* c) {
    register float __s __asm__("fr2");
    register float __c __asm__("fr3");

    arg0 &= 0xFFF0;

    asm("lds    %2,fpul\n\t"
        "fsca    fpul,dr2\n\t"
        : "=f"(__s), "=f"(__c)
        : "r"(arg0)
        : "fpul");

    *s = __s;
    *c = __c;
}

static inline void scaled_sincoss(u16 arg0, f32* s, f32* c, f32 scale) {
    register float __s __asm__("fr2");
    register float __c __asm__("fr3");

    arg0 &= 0xFFF0;

    asm("lds    %2,fpul\n\t"
        "fsca    fpul,dr2\n\t"
        : "=f"(__s), "=f"(__c)
        : "r"(arg0)
        : "fpul");

    *s = __s * scale;
    *c = __c * scale;
}

#else /* N64-faithful sine table */

f32 sins(u16);
f32 coss(u16);

/* The angle is taken as s32: callers pass floats (e.g. render_player.c), and a
   negative float converts to u16 by wrapping on N64/SH4 but saturates to 0 on ARM. */
static inline void sincoss(s32 arg0, f32* s, f32* c) {
    *s = sins((u16) arg0);
    *c = coss((u16) arg0);
}

static inline void scaled_sincoss(s32 arg0, f32* s, f32* c, f32 scale) {
    *s = sins((u16) arg0) * scale;
    *c = coss((u16) arg0) * scale;
}

#endif /* SINCOSS_USE_FSCA */

#endif /* SINCOSS_H */
