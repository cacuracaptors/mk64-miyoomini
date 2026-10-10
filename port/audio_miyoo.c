/*
 * Miyoo Mini (Plus) audio, stage 1: muted. The sound engine still runs (one
 * audio frame per 60 Hz tick, see port_audio_pump in src/main.c) so music and
 * effects advance exactly as on the DC; nothing is played yet.
 * Keeps the DC symbol name audio_dc so src/main.c needs no change.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "dcaudio/audio_api.h"

static bool am_init(void) {
    return true;
}

static int am_buffered(void) {
    return 0;
}

static int am_get_desired_buffered(void) {
    return 0;
}

static void am_play(uint8_t *buf, size_t len) {
    (void) buf;
    (void) len;
}

struct AudioAPI audio_dc = { am_init, am_buffered, am_get_desired_buffered, am_play };

void mute_stream(void) {
}

void unmute_stream(void) {
}
