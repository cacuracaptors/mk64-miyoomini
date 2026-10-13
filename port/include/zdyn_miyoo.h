/* zlib loaded at run time (port/zdyn_miyoo.c) */
#ifndef ZDYN_MIYOO_H
#define ZDYN_MIYOO_H

/* zlib's z_stream, field for field (32-bit: uInt and uLong are both 32 bits) */
typedef struct {
    const unsigned char *next_in;
    unsigned avail_in;
    unsigned long total_in;
    unsigned char *next_out;
    unsigned avail_out;
    unsigned long total_out;
    const char *msg;
    void *state;
    void *zalloc, *zfree, *opaque;
    int data_type;
    unsigned long adler;
    unsigned long reserved;
} zd_stream;

#define ZD_OK 0
#define ZD_STREAM_END 1
#define ZD_NO_FLUSH 0
#define ZD_FINISH 4

int zd_available(void);   /* 1 = zlib found (loads it the first time) */
int zd_deflate_init(zd_stream *s, int level);
int zd_deflate(zd_stream *s, int flush);
int zd_deflate_end(zd_stream *s);
int zd_inflate_init(zd_stream *s);
int zd_inflate(zd_stream *s, int flush);
int zd_inflate_end(zd_stream *s);

#endif
