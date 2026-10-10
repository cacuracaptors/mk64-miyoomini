/*
 * n64_stubs.c -- empty versions of N64 functions that the game code still
 * names but never runs on this port (Miyoo Mini).
 *
 * The Dreamcast build linked with --gc-sections, which threw away the dead
 * functions that call these (the RSP audio command path in synthesis.c, the
 * RSP task yield in main.c). Makefile.miyoo does the same; these stubs only
 * make sure the link also works if a dead caller survives.
 *
 * Stage 1 of the port has no sound mixer. When the software mixer arrives,
 * the a*Impl functions get real bodies (mixer.c from the PC ports).
 */
#include <stdint.h>
#include <ultra64.h>
#include "audio/mixer.h"

void aClearBufferImpl(uint16_t addr, int nbytes) { (void) addr; (void) nbytes; }
void aLoadBufferImpl(const void *source_addr, uint16_t dest_addr, uint16_t nbytes) {
    (void) source_addr; (void) dest_addr; (void) nbytes;
}
void aSaveBufferImpl(uint16_t source_addr, int16_t *dest_addr, uint16_t nbytes) {
    (void) source_addr; (void) dest_addr; (void) nbytes;
}
void aLoadADPCMImpl(int num_entries_times_16, const int16_t *book_source_addr) {
    (void) num_entries_times_16; (void) book_source_addr;
}
void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
    (void) flags; (void) in; (void) out; (void) nbytes;
}
void aInterleaveImpl(uint16_t left, uint16_t right) { (void) left; (void) right; }
void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) {
    (void) in_addr; (void) out_addr; (void) nbytes;
}
void aSetLoopImpl(ADPCM_STATE *adpcm_loop_state) { (void) adpcm_loop_state; }
void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state) { (void) flags; (void) state; }
void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) {
    (void) flags; (void) pitch; (void) state;
}
void aEnvSetup1Impl(uint8_t initial_vol_wet, uint16_t rate_wet, uint16_t rate_left, uint16_t rate_right) {
    (void) initial_vol_wet; (void) rate_wet; (void) rate_left; (void) rate_right;
}
void aEnvSetup2Impl(uint16_t initial_vol_left, uint16_t initial_vol_right) {
    (void) initial_vol_left; (void) initial_vol_right;
}
void aEnvMixerImpl(uint16_t in_addr, uint16_t n_samples, int swap_reverb, int neg_left, int neg_right,
                   uint16_t dry_left_addr, uint16_t dry_right_addr, uint16_t wet_left_addr, uint16_t wet_right_addr) {
    (void) in_addr; (void) n_samples; (void) swap_reverb; (void) neg_left; (void) neg_right;
    (void) dry_left_addr; (void) dry_right_addr; (void) wet_left_addr; (void) wet_right_addr;
}
void aSMixImpl(uint16_t in_addr, uint16_t out_addr, uint16_t count) {
    (void) in_addr; (void) out_addr; (void) count;
}
void aMixImpl(int16_t gain, uint16_t in_addr, uint16_t out_addr, uint16_t count) {
    (void) gain; (void) in_addr; (void) out_addr; (void) count;
}
void aDMEMMove2Impl(uint8_t t, uint16_t in_addr, uint16_t out_addr, uint16_t count) {
    (void) t; (void) in_addr; (void) out_addr; (void) count;
}
void aDownsampleHalfImpl(uint16_t n_samples, uint16_t in_addr, uint16_t out_addr) {
    (void) n_samples; (void) in_addr; (void) out_addr;
}

/* RSP task yield: there is no RSP; a "yielded" task is reported as finished */
void osSpTaskYield(void) {
}

OSYieldResult osSpTaskYielded(OSTask *task) {
    (void) task;
    return 0;
}
