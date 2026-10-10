/*
 * Miyoo Mini (Plus) port: startup, 60 Hz vblank ticker, quit handling.
 *
 * Options (command line):
 *   -hires   render at 640x480 instead of 320x240 (slower)
 *   -perf    print timing lines to stderr every 2 s
 *   -60fps   single-player races run uncapped (up to 60 fps; experimental)
 *   -timer   game clock from a 60 Hz timer instead of the panel's vsync (comparison)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include "port_miyoo.h"

int port_opt_perf = 0;
int port_opt_60fps = 0;
static int opt_timer = 0;   /* -timer: old free-running 60 Hz clock, for comparison */

double port_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

void port_quit(int code) {
    port_video_shutdown();
    fflush(stdout);
    fflush(stderr);
    sync();
    _exit(code);
}

static void on_signal(int sig) {
    port_video_shutdown();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* run from the executable's folder, whatever the launcher's current directory is */
void port_chdir_to_exe(void) {
    char path[512];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0)
        return;
    path[n] = 0;
    char *slash = strrchr(path, '/');
    if (!slash)
        return;
    *slash = 0;
    if (path[0] && chdir(path) != 0)
        perror("chdir");
}

void port_init(int argc, char **argv) {
    int hires = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-hires"))
            hires = 1;
        else if (!strcmp(argv[i], "-perf"))
            port_opt_perf = 1;
        else if (!strcmp(argv[i], "-60fps"))
            port_opt_60fps = 1;
        else if (!strcmp(argv[i], "-timer"))
            opt_timer = 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    mkdir("saves", 0777);

    port_video_init(hires);
    port_input_init();

    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);
    signal(SIGABRT, on_signal);
    signal(SIGFPE, on_signal);
    signal(SIGILL, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
}

/* ---- 60 Hz "vblank" ---------------------------------------------------------
 * The game's timing (gVBlankTimer, sNumVBlanks, the 30 fps pacer, the audio
 * frame clock) runs off this tick, as it ran off the real vblank IRQ on the DC.
 * It follows the panel's own vsync (FBIO_WAITFORVSYNC, ~16.75 ms on the Miyoo):
 * a separate 60 Hz timer slowly drifts against the panel, and every few seconds
 * a frame would then stay on screen for 1 or 3 refreshes instead of 2 (a hitch).
 * If the panel can't be waited on, a 60 Hz timer is used instead. */
static void (*s_vbl_fn)(uint32_t, void *);

static int vsync_usable(void) {
    double t0 = port_now_ms();
    for (int i = 0; i < 6; i++)
        if (port_video_wait_vsync() != 0)
            return 0;
    double per = (port_now_ms() - t0) / 6.0;
    printf("vblank: vsync do painel a cada %.2f ms\n", per);
    return per > 12.0 && per < 25.0;
}

static void *vbl_proc(void *arg) {
    (void) arg;
    if (!opt_timer && vsync_usable()) {
        printf("vblank: seguindo o vsync do painel\n");
        for (;;) {
            if (port_video_wait_vsync() != 0)
                break;
            s_vbl_fn(0, NULL);
        }
    }
    printf("vblank: relogio de 60 Hz\n");
    const long period = 16666667L; /* ns */
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += period;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        /* if we fell far behind (suspend, debugger), resync instead of bursting */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long late = (long long) (now.tv_sec - next.tv_sec) * 1000000000LL + (now.tv_nsec - next.tv_nsec);
        if (late > 4LL * period)
            next = now;
        s_vbl_fn(0, NULL);
    }
    return NULL;
}

void port_start_vblank(void (*fn)(uint32_t, void *)) {
    pthread_t th;
    s_vbl_fn = fn;
    if (pthread_create(&th, NULL, vbl_proc, NULL) != 0) {
        fprintf(stderr, "mk64: nao consegui criar a thread de vblank\n");
        port_quit(1);
    }
    pthread_detach(th);
}

void port_poll_quit(void) {
    if (port_input_quit_requested()) {
        printf("mk64: saindo\n");
        port_quit(0);
    }
}
