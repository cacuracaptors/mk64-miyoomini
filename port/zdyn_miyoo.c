/*
 * zdyn_miyoo.c -- zlib, taken from the device at run time (dlopen): the save states and the
 * Game Switcher's picture are compressed with it. OnionOS ships it
 * (/mnt/SDCARD/miyoo/lib/libz.so.1); without it the files are written uncompressed.
 *
 * Only the stable part of zlib's interface is used (the z_stream layout, deflateInit_ /
 * inflateInit_, unchanged since zlib 1.0), so no zlib header is needed to build.
 */
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include "zdyn_miyoo.h"

static int (*p_deflateInit_)(zd_stream *, int, const char *, int);
static int (*p_deflate)(zd_stream *, int);
static int (*p_deflateEnd)(zd_stream *);
static int (*p_inflateInit_)(zd_stream *, const char *, int);
static int (*p_inflate)(zd_stream *, int);
static int (*p_inflateEnd)(zd_stream *);
static int sState;   /* 0 not tried, 1 loaded, -1 missing */

int zd_available(void) {
    if (sState)
        return sState > 0;
    static const char *names[] = {
        "libz.so.1", "/mnt/SDCARD/miyoo/lib/libz.so.1", "/mnt/SDCARD/.tmp_update/lib/parasyte/libz.so.1",
        "/customer/lib/libz.so.1", "/lib/libz.so.1", "/usr/lib/libz.so.1", "libz.so",
    };
    void *h = NULL;
    const char *used = NULL;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]) && !h; i++) {
        h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        used = names[i];
    }
    sState = -1;
    if (!h) {
        printf("estado: zlib nao encontrada (arquivos sem compressao)\n");
        return 0;
    }
    p_deflateInit_ = (int (*)(zd_stream *, int, const char *, int)) dlsym(h, "deflateInit_");
    p_deflate = (int (*)(zd_stream *, int)) dlsym(h, "deflate");
    p_deflateEnd = (int (*)(zd_stream *)) dlsym(h, "deflateEnd");
    p_inflateInit_ = (int (*)(zd_stream *, const char *, int)) dlsym(h, "inflateInit_");
    p_inflate = (int (*)(zd_stream *, int)) dlsym(h, "inflate");
    p_inflateEnd = (int (*)(zd_stream *)) dlsym(h, "inflateEnd");
    const char *(*ver)(void) = (const char *(*)(void)) dlsym(h, "zlibVersion");
    if (!p_deflateInit_ || !p_deflate || !p_deflateEnd || !p_inflateInit_ || !p_inflate || !p_inflateEnd || !ver ||
        ver()[0] != '1') {
        printf("estado: zlib incompleta em %s (arquivos sem compressao)\n", used);
        return 0;
    }
    printf("estado: zlib %s (%s)\n", ver(), used);
    sState = 1;
    return 1;
}

int zd_deflate_init(zd_stream *s, int level) {
    memset(s, 0, sizeof(*s));
    return p_deflateInit_(s, level, "1.2.11", (int) sizeof(*s));
}
int zd_deflate(zd_stream *s, int flush) { return p_deflate(s, flush); }
int zd_deflate_end(zd_stream *s) { return p_deflateEnd(s); }

int zd_inflate_init(zd_stream *s) {
    memset(s, 0, sizeof(*s));
    return p_inflateInit_(s, "1.2.11", (int) sizeof(*s));
}
int zd_inflate(zd_stream *s, int flush) { return p_inflate(s, flush); }
int zd_inflate_end(zd_stream *s) { return p_inflateEnd(s); }
