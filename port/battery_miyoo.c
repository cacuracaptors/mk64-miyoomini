/*
 * battery_miyoo.c -- low battery on OnionOS, as the system does it for its emulators.
 *
 * The warning: OnionOS's batmon draws a red battery icon straight into the screen
 * memory every 16 ms once the battery is under the "warn at" level. The game flips
 * between two pages and rewrites the whole picture every frame, so the icon showed on
 * one page and not the other: it flickered. An app that shows the battery itself tells
 * batmon so with the flag /tmp/hasBatteryDisplay (as Onion's own themed apps do); the
 * game sets it while it runs and draws the same icon into every frame instead.
 *
 * The shutdown: under the "exit at" level, keymon closes RetroArch and DraStic and
 * leaves /tmp/.lowBat, and runtime.sh then shows the low battery screen and turns the
 * device off once the game has ended. A port is not closed by keymon, so the game kept
 * running until the battery died. The game now does the same on its own: it leaves
 * /tmp/.lowBat and quits (the races' results are already saved as they happen).
 *
 * Levels and switches come from OnionOS's settings, as batmon and keymon read them:
 * /tmp/percBat (500 = charging), config/battery/warnAt, config/battery/exitAt,
 * config/.noBatteryWarning, config/.noLowBatteryAutoSave.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "port_miyoo.h"

#define ONION_CONFIG "/mnt/SDCARD/.tmp_update/config/"
#define FLAG_OWN_DISPLAY "/tmp/hasBatteryDisplay"

volatile int port_battery_low;      /* draw the warning icon */
volatile int port_battery_exit;     /* quit now (port_poll_quit) */
static int sOwnFlag;

static int read_int(const char *path, int fallback) {
    FILE *f = fopen(path, "r");
    int v = fallback;
    if (!f)
        return fallback;
    if (fscanf(f, "%d", &v) != 1)
        v = fallback;
    fclose(f);
    return v;
}

static int exists(const char *path) {
    return access(path, F_OK) == 0;
}

static void *battery_proc(void *arg) {
    (void) arg;
    int logged = 0;
    for (;;) {
        const int perc = read_int("/tmp/percBat", -1);
        int warn_at = exists(ONION_CONFIG ".noBatteryWarning") ? 0 : read_int(ONION_CONFIG "battery/warnAt", 15);
        int exit_at = exists(ONION_CONFIG ".noLowBatteryAutoSave") ? 0 : read_int(ONION_CONFIG "battery/exitAt", 4);
        const int valid = perc >= 0 && perc <= 100;   /* (500: charging; over 100: a bad reading) */
        port_battery_low = valid && perc < warn_at;
        if (valid && exit_at > 0 && perc <= exit_at && !port_battery_exit) {
            FILE *f = fopen("/tmp/.lowBat", "w");   /* runtime.sh: low battery screen, then off */
            if (f)
                fclose(f);
            printf("bateria: %d%% (sair em %d%%): fechando o jogo para o OnionOS desligar\n", perc, exit_at);
            port_battery_exit = 1;
        }
        if (valid && port_battery_low && !logged) {
            printf("bateria: %d%% (aviso em %d%%)\n", perc, warn_at);
            logged = 1;
        }
        sleep(2);
    }
    return NULL;
}

void port_battery_init(void) {
    if (!exists("/tmp/percBat") && !exists(ONION_CONFIG))
        return;   /* (not OnionOS) */
    FILE *f = fopen(FLAG_OWN_DISPLAY, "w");
    if (f) {
        fclose(f);
        sOwnFlag = 1;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, battery_proc, NULL) == 0)
        pthread_detach(th);
}

/* (async-signal-safe: called from the crash and signal handlers too) */
void port_battery_release(void) {
    if (sOwnFlag)
        unlink(FLAG_OWN_DISPLAY);
}

/* batmon's icon (red outline, nearly empty), 15 pixels from the top and right edges of
   the 640x480 screen, scaled to the frame being drawn (-resdin) */
void port_battery_draw(uint16_t *buf, int w, int h) {
    if (!port_battery_low || !buf)
        return;
    const uint16_t red = 0xF800;
    const float s = (float) w / 640.0f;
#define PX(v) ((int) ((v) * s + 0.5f))
    const int x0 = w - PX(15 + 30 + 4), y0 = PX(15);
    const int bw = PX(30), bh = PX(15), hw = PX(4), t = PX(1) > 0 ? PX(1) : 1;
    if (x0 < 0 || y0 + bh > h)
        return;
    for (int y = 0; y < bh; y++) {
        uint16_t *row = buf + (y0 + y) * w + x0;
        const int edge_y = y < t || y >= bh - t;
        for (int x = 0; x < bw; x++)
            if (edge_y || x < t || x >= bw - t)
                row[x] = red;
        /* what is left of the charge: a sliver at the empty end */
        if (y >= PX(3) && y < bh - PX(3))
            for (int x = PX(3); x < PX(3) + (PX(2) > 0 ? PX(2) : 1); x++)
                row[x] = red;
        /* the terminal, on the right */
        if (y >= PX(4) && y < bh - PX(4))
            for (int x = bw; x < bw + hw; x++)
                row[x] = red;
    }
#undef PX
}
