/*
 * Miyoo Mini (Plus) port: startup, 60 Hz vblank ticker, quit handling.
 *
 * Options (command line):
 *   -hires   render at 640x480 instead of 320x240 (slower)
 *   -perf    print timing lines to stderr every 2 s
 *   -60fps   single-player races run uncapped (up to 60 fps; experimental)
 *   -timer   game clock from a 60 Hz timer instead of the panel's vsync (comparison)
 *   -filtro  N64-style 3-point texture filter on magnified 3D opaque surfaces (the road)
 *   -semgfx  copy frames to the screen with the CPU instead of the 2D engine (MI_GFX)
 *   -semparalelo  draw each frame start to end before the next one (the default draws a
 *            frame on core 1 while the game prepares the next, shown one frame later)
 *   -resdin  dynamic resolution: with -hires, frames drop to 576x432, 512x384 or 448x336
 *            while the drawing does not keep up with 60 fps (the 2D engine scales them)
 *   -cerimonia  (testing) after the title screen and the menus, the podium ceremony
 *            instead of the race (the upstream debug path)
 *   -exato   texture mapping exactly as before: perspective every 16 pixels, on every
 *            surface (the default maps nearly flat-on surfaces linearly, every 32 at 640x480)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <ucontext.h>

#include "port_miyoo.h"

int port_opt_perf = 0;
int port_opt_60fps = 0;
int port_opt_filter = 0;
int port_opt_parallel = 1;   /* -semparalelo turns it off */
int port_opt_resdin = 0;     /* -resdin: smaller frames while the drawing does not keep up */
int port_opt_mute = 0;       /* -mudo: no sound */
int port_opt_ceremony = 0;   /* -cerimonia: A on the title screen leads to the podium ceremony (testing) */
int port_opt_exact = 0;      /* -exato: no drawing shortcuts that change the picture slightly */
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

/* ---- crash report -------------------------------------------------------------
 * A crash writes crash_log.txt (appended) in the game's folder: the signal, the
 * thread, the faulting instruction (pc), its caller (lr), the address it touched,
 * and every word on the stack that points into the code (the callers further up,
 * plus some stale ones). Resolve them against the unstripped build/mario-kart.elf:
 *   arm-linux-gnueabihf-addr2line -f -C -e build/mario-kart.elf 0x... 0x...
 * Async-signal-safe: write() only, no stdio. */
extern char __executable_start[], etext[];

static void crash_str(int fd, const char *str) {
    if (write(fd, str, strlen(str)) < 0)
        return;
}

static void crash_hex(int fd, const char *label, unsigned long v) {
    static const char digits[] = "0123456789abcdef";
    char out[16];
    int n = 0;
    out[n++] = '0';
    out[n++] = 'x';
    for (int sh = 28; sh >= 0; sh -= 4)
        out[n++] = digits[(v >> sh) & 15];
    out[n++] = '\n';
    crash_str(fd, label);
    if (write(fd, out, (size_t) n) < 0)
        return;
}

static void on_crash(int sig, siginfo_t *info, void *context) {
    int fd = open("crash_log.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGABRT ? "SIGABRT"
                         : sig == SIGFPE ? "SIGFPE" : sig == SIGILL ? "SIGILL" : "signal";
        crash_str(fd, "\n--- crash: ");
        crash_str(fd, name);
        crash_str(fd, " ---\n");
        crash_hex(fd, "thread: ", (unsigned long) syscall(SYS_gettid));
        crash_hex(fd, "process: ", (unsigned long) getpid());
        if (info)
            crash_hex(fd, "fault addr: ", (unsigned long) info->si_addr);
#if defined(__arm__)
        if (context) {
            const ucontext_t *uc = context;
            crash_hex(fd, "pc: ", uc->uc_mcontext.arm_pc);
            crash_hex(fd, "lr: ", uc->uc_mcontext.arm_lr);
            crash_hex(fd, "sp: ", uc->uc_mcontext.arm_sp);
            /* code addresses on the stack (the thread's stack is mapped well past this) */
            const unsigned long lo = (unsigned long) __executable_start, hi = (unsigned long) etext;
            const unsigned long *sp = (const unsigned long *) uc->uc_mcontext.arm_sp;
            int found = 0;
            for (int i = 0; i < 512 && found < 48; i++) {
                const unsigned long w = sp[i];
                if (w >= lo && w < hi) {
                    crash_hex(fd, "stack: ", w);
                    found++;
                }
            }
        }
#else
        (void) context;
#endif
        close(fd);
    }
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
        else if (!strcmp(argv[i], "-filtro"))
            port_opt_filter = 1;
        else if (!strcmp(argv[i], "-semgfx"))
            port_opt_nogfx = 1;
        else if (!strcmp(argv[i], "-paralelo"))
            port_opt_parallel = 1;   /* (the default; kept so old shortcuts still work) */
        else if (!strcmp(argv[i], "-semparalelo"))
            port_opt_parallel = 0;
        else if (!strcmp(argv[i], "-exato"))
            port_opt_exact = 1;
        else if (!strcmp(argv[i], "-resdin"))
            port_opt_resdin = 1;
        else if (!strcmp(argv[i], "-mudo"))
            port_opt_mute = 1;
        else if (!strcmp(argv[i], "-cerimonia"))
            port_opt_ceremony = 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    mkdir("saves", 0777);

    port_video_init(hires);
    if (port_opt_perf)
        port_video_membench();
    port_input_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    static const int crash_signals[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
    for (int i = 0; i < 5; i++)
        sigaction(crash_signals[i], &sa, NULL);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
}

/* ---- 60 Hz "vblank" ---------------------------------------------------------
 * The game's timing (gVBlankTimer, sNumVBlanks, the 30 fps pacer, the audio
 * frame clock) runs off this tick, as it ran off the real vblank IRQ on the DC.
 * Normally the tick comes from the panel's vsync, through the flip thread
 * (port/video_miyoo.c); the timer below is only the fallback. */
static void (*s_vbl_fn)(uint32_t, void *);

static void vsync_tick(void) {
    s_vbl_fn(0, NULL);
}

static void *vbl_proc(void *arg) {
    (void) arg;
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

static void start_timer_clock(void);

void port_start_vblank(void (*fn)(uint32_t, void *)) {
    s_vbl_fn = fn;
    if (!opt_timer && port_video_set_vsync_clock(vsync_tick, start_timer_clock)) {
        printf("vblank: relogio do jogo = vsync do painel\n");
        return;
    }
    start_timer_clock();
}

static void start_timer_clock(void) {
    pthread_t th;
    printf("vblank: relogio de 60 Hz\n");
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
