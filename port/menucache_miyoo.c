/* The menus' pictures, decompressed once (MIO0 and TKMK00, src/menu_items.c).
 *
 * At every change of menu screen the game decompresses that screen's pictures again,
 * in one frame: 75 to 105 ms on the Miyoo (the hitch log, "resto"), longer than the sound's
 * reserve (the sound ran dry at every change of screen). The pictures come from the game's
 * data, which never changes: each one is kept, decompressed, the first time, and copied
 * from there the next times. Only for pictures read straight from the game's data (by
 * address); what the game decompresses from its own buffers (gMenuCompressedBuffer, reused
 * for different pictures) is left as it was. -perf: the time spent here, in the hitch log.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "port_miyoo.h"

int mio0decode(const unsigned char *in, unsigned char *out);
void tkmk00decode(uint32_t *_tkmk, uint8_t *tmp_buf, uint16_t *_rgba16, uint32_t alpha_color);
void gfx_texture_cache_invalidate(void *addr);

double port_decode_ms;   /* (the hitch log: time in the decoders and here) */

#define DC_MAX 768
#define DC_BYTES (24u << 20)

typedef struct {
    const void *src;
    uint32_t kind;     /* 0 MIO0, 0x100 | alpha colour TKMK00 */
    uint32_t sum;      /* the first 64 bytes of the compressed data (a check, the address is the key) */
    uint32_t size;     /* bytes kept: the picture (and for TKMK00 its 1-byte-a-pixel buffer after it) */
    uint8_t *data;
} DcEnt;

static DcEnt sDc[DC_MAX];
static int sDcN;
static size_t sDcBytes;

static uint32_t be32(const uint8_t *p) {
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static uint32_t sum64(const uint8_t *p) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 64; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static DcEnt *dc_find(const void *src, uint32_t kind, uint32_t sum, uint32_t size) {
    for (int i = 0; i < sDcN; i++)
        if (sDc[i].src == src && sDc[i].kind == kind && sDc[i].sum == sum && sDc[i].size == size)
            return &sDc[i];
    return NULL;
}

static void dc_keep(const void *src, uint32_t kind, uint32_t sum, uint32_t size, const void *a, uint32_t na, const void *b,
                    uint32_t nb) {
    if (sDcN >= DC_MAX || sDcBytes + size > DC_BYTES)
        return;
    uint8_t *d = malloc(size);
    if (!d)
        return;
    memcpy(d, a, na);
    if (nb)
        memcpy(d + na, b, nb);
    DcEnt *e = &sDc[sDcN++];
    e->src = src;
    e->kind = kind;
    e->sum = sum;
    e->size = size;
    e->data = d;
    sDcBytes += size;
}

void port_mio0decode_cached(const void *in, void *out) {
    const double t0 = port_now_ms();
    const uint8_t *p = in;
    const uint32_t size = be32(p + 4);   /* (the decompressed size, in the MIO0 header) */
    const uint32_t sum = sum64(p);
    const DcEnt *e = size && size <= (4u << 20) ? dc_find(in, 0, sum, size) : NULL;
    if (e) {
        gfx_texture_cache_invalidate(out);   /* (as mio0decode does: the renderer reloads it) */
        memcpy(out, e->data, size);
    } else {
        mio0decode(in, out);
        if (size && size <= (4u << 20))
            dc_keep(in, 0, sum, size, out, size, NULL, 0);
    }
    port_decode_ms += port_now_ms() - t0;
}

void port_tkmk00decode_cached(void *in, void *tmp, void *out, uint32_t alpha) {
    const double t0 = port_now_ms();
    const uint8_t *p = in;
    const uint32_t w = (uint32_t) p[8] << 8 | p[9], h = (uint32_t) p[10] << 8 | p[11];
    const uint32_t px = w * h, size = px * 3;   /* the picture (2 bytes a pixel), then tmp (1) */
    const uint32_t kind = 0x100u | (alpha & 0xFF), sum = sum64(p);
    const DcEnt *e = px && px <= (1u << 20) ? dc_find(in, kind, sum, size) : NULL;
    if (e) {
        memcpy(out, e->data, px * 2);
        memcpy(tmp, e->data + px * 2, px);
    } else {
        tkmk00decode(in, tmp, out, alpha);
        if (px && px <= (1u << 20))
            dc_keep(in, kind, sum, size, out, px * 2, tmp, px);
    }
    port_decode_ms += port_now_ms() - t0;
}
