/*
 * kos_shim.c -- the few KallistiOS services the game code still calls
 * (Miyoo Mini port). See port/include/kos.h.
 */
#include <kos.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <string.h>

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

uint64_t timer_ns_gettime64(void) { return mono_ns(); }
uint64_t timer_us_gettime64(void) { return mono_ns() / 1000ull; }
uint64_t timer_ms_gettime64(void) { return mono_ns() / 1000000ull; }

void thd_pass(void) {
    sched_yield();
}

void thd_sleep(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long) (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static pthread_mutex_t gw_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gw_cond = PTHREAD_COND_INITIALIZER;
static unsigned int gw_seq;

int genwait_wait(void *obj, const char *mesg, int timeout_ms, void (*callback)(void *)) {
    unsigned int seq;
    (void) obj; (void) mesg; (void) callback;
    pthread_mutex_lock(&gw_mutex);
    seq = gw_seq;
    if (timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (seq == gw_seq)
            if (pthread_cond_timedwait(&gw_cond, &gw_mutex, &ts) != 0)
                break;
    } else {
        while (seq == gw_seq)
            pthread_cond_wait(&gw_cond, &gw_mutex);
    }
    pthread_mutex_unlock(&gw_mutex);
    return 0;
}

int genwait_wake_all(void *obj) {
    (void) obj;
    pthread_mutex_lock(&gw_mutex);
    gw_seq++;
    pthread_cond_broadcast(&gw_cond);
    pthread_mutex_unlock(&gw_mutex);
    return 0;
}

/* memcpy32.S (SH4 store-queue copy) */
void *memcpy32(void *restrict dst, const void *restrict src, size_t bytes) {
    return memcpy(dst, src, bytes);
}
