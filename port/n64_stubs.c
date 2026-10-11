/*
 * n64_stubs.c -- empty versions of N64 functions that the game code still
 * names but never runs on this port (Miyoo Mini).
 *
 * The Dreamcast build linked with --gc-sections, which threw away the dead
 * functions that call these (the RSP audio command path in synthesis.c, the
 * RSP task yield in main.c). Makefile.miyoo does the same; these stubs only
 * make sure the link also works if a dead caller survives.
 *
 * The audio microcode commands (a*Impl) are in mixer_miyoo.c.
 */
#include <stdint.h>
#include <ultra64.h>

/* RSP task yield: there is no RSP; a "yielded" task is reported as finished */
void osSpTaskYield(void) {
}

OSYieldResult osSpTaskYielded(OSTask *task) {
    (void) task;
    return 0;
}
