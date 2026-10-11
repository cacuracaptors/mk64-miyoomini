/* romkit: see romkit.h */
#include <stdlib.h>
#include <string.h>
#include "romkit.h"

uint32_t rk_crc32(uint32_t crc, const void *buf, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            table[i] = c;
        }
    const uint8_t *p = (const uint8_t *) buf;
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

int rk_normalize_rom(uint8_t *rom, size_t size) {
    if (size < 0x1000 || (size & 3))
        return 0;
    if (rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40)
        return 1;                                   /* .z64 */
    if (rom[0] == 0x37 && rom[1] == 0x80 && rom[2] == 0x40 && rom[3] == 0x12) {
        for (size_t i = 0; i < size; i += 2) {      /* .v64: 2-byte swapped */
            uint8_t t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t;
        }
        return 1;
    }
    if (rom[0] == 0x40 && rom[1] == 0x12 && rom[2] == 0x37 && rom[3] == 0x80) {
        for (size_t i = 0; i < size; i += 4) {      /* .n64: 4-byte swapped */
            uint8_t a = rom[i], b = rom[i + 1];
            rom[i] = rom[i + 3]; rom[i + 1] = rom[i + 2]; rom[i + 2] = b; rom[i + 3] = a;
        }
        return 1;
    }
    return 0;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

/* size of the MIO0 block at p (decompressed), 0 if it does not look like a sound one */
static uint32_t mio0_check(const uint8_t *p, size_t avail) {
    if (avail < 16 || memcmp(p, "MIO0", 4))
        return 0;
    const uint32_t dest = be32(p + 4), comp = be32(p + 8), unc = be32(p + 12);
    if (dest == 0 || dest > (8u << 20) || comp < 16 || unc < comp || comp > avail || unc > avail)
        return 0;
    return dest;
}

/* decodes the MIO0 block at in (avail bytes from it) into out (dest bytes); 0 if it
   runs off its input or references before the output's start */
static int mio0_decode(const uint8_t *in, size_t avail, uint8_t *out, uint32_t dest) {
    const uint32_t comp = be32(in + 8), unc = be32(in + 12);
    uint32_t written = 0, bit = 0, ci = 0, ui = 0;
    while (written < dest) {
        const uint32_t byte = 16 + bit / 8;
        if (byte >= avail)
            return 0;
        if (in[byte] & (1 << (7 - (bit % 8)))) {
            if (unc + ui >= avail)
                return 0;
            out[written++] = in[unc + ui++];
        } else {
            if (comp + ci + 1 >= avail)
                return 0;
            const uint8_t *v = &in[comp + ci];
            ci += 2;
            const uint32_t len = ((v[0] & 0xF0u) >> 4) + 3, idx = ((v[0] & 0x0Fu) << 8) + v[1] + 1;
            if (idx > written)
                return 0;
            for (uint32_t i = 0; i < len && written < dest; i++, written++)
                out[written] = out[written - idx];
        }
        bit++;
    }
    return 1;
}

uint8_t *rk_build_pool(const uint8_t *rom, size_t rom_size, size_t *size, void (*progress)(int)) {
    /* first pass: total size */
    size_t total = (rom_size + 3) & ~(size_t) 3;
    for (size_t o = 0; o + 16 <= rom_size; o += 4) {
        const uint32_t d = mio0_check(rom + o, rom_size - o);
        if (d)
            total += (d + 3) & ~3u;
    }
    uint8_t *pool = calloc(1, total);
    if (!pool)
        return NULL;
    memcpy(pool, rom, rom_size);
    size_t at = (rom_size + 3) & ~(size_t) 3;
    for (size_t o = 0; o + 16 <= rom_size; o += 4) {
        if (progress && (o & 0xFFFF) == 0)
            progress((int) (o * 1000 / rom_size));
        const uint32_t d = mio0_check(rom + o, rom_size - o);
        if (!d)
            continue;
        if (!mio0_decode(rom + o, rom_size - o, pool + at, d))
            memset(pool + at, 0, d);   /* (not a real block: zeros keep the layout fixed) */
        at += (d + 3) & ~3u;
    }
    if (progress)
        progress(1000);
    *size = total;
    return pool;
}

/* ---- MIO0 compression: the build's encoder (tools/libmio0.c, from n64split by
   queueRAM), unchanged in what it outputs, so a block compressed here is byte for
   byte the one the build made ---- */
typedef struct { int *indexes; int allocated, count, start; } rk_lookback;
#define RK_LB_COUNT 256

static void lb_push(rk_lookback *lkbk, unsigned char val, int index) {
    rk_lookback *lb = &lkbk[val];
    if (lb->count == lb->allocated) {
        lb->allocated *= 4;
        lb->indexes = realloc(lb->indexes, (size_t) lb->allocated * sizeof(*lb->indexes));
    }
    lb->indexes[lb->count++] = index;
}

static void put_bit(unsigned char *buf, int bit, int val) {
    const unsigned char mask = (unsigned char) (1 << (7 - (bit % 8)));
    const unsigned int offset = (unsigned int) bit / 8;
    buf[offset] = (unsigned char) ((buf[offset] & ~mask) | (val ? mask : 0));
}

static int find_longest(const unsigned char *buf, int start_offset, int max_search, int *found_offset, rk_lookback *lkbk) {
    int best_length = 0, best_offset = 0, cur_length, search_len, farthest, off, i, lb_idx;
    rk_lookback *lb = &lkbk[buf[start_offset]];
    farthest = start_offset - 4096 > 0 ? start_offset - 4096 : 0;
    for (lb_idx = lb->start; lb_idx < lb->count && lb->indexes[lb_idx] < farthest; lb_idx++) {}
    lb->start = lb_idx;
    for (; lb_idx < lb->count && lb->indexes[lb_idx] < start_offset; lb_idx++) {
        off = lb->indexes[lb_idx];
        search_len = max_search < start_offset - off ? max_search : start_offset - off;
        for (i = 0; i < search_len; i++)
            if (buf[start_offset + i] != buf[off + i])
                break;
        cur_length = i;
        if (cur_length == search_len) {
            search_len = max_search - cur_length;
            for (i = 0; i < search_len; i++)
                if (buf[start_offset + cur_length + i] != buf[off + i])
                    break;
            cur_length += i;
        }
        if (cur_length > best_length) {
            best_offset = start_offset - off;
            best_length = cur_length;
        }
    }
    *found_offset = best_offset;
    return best_length;
}

static void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24); p[1] = (uint8_t) (v >> 16); p[2] = (uint8_t) (v >> 8); p[3] = (uint8_t) v;
}

uint32_t rk_mio0_encode(const uint8_t *in, uint32_t length, uint8_t *out) {
    if (length == 0)
        return 0;
    rk_lookback *lookbacks = malloc(RK_LB_COUNT * sizeof(*lookbacks));
    for (int i = 0; i < RK_LB_COUNT; i++) {
        lookbacks[i].allocated = 128;
        lookbacks[i].indexes = malloc(128 * sizeof(int));
        lookbacks[i].count = 0;
        lookbacks[i].start = 0;
    }
    unsigned char *bit_buf = calloc(1, (length + 7) / 8);
    unsigned char *comp_buf = malloc(length);
    unsigned char *uncomp_buf = malloc(length);
    unsigned int bytes_proc = 0;
    int bit_idx = 0, comp_idx = 0, uncomp_idx = 0;

    lb_push(lookbacks, in[0], 0);
    uncomp_buf[uncomp_idx++] = in[0];
    bytes_proc += 1;
    put_bit(bit_buf, bit_idx++, 1);
    while (bytes_proc < length) {
        int offset;
        int max_length = (int) (length - bytes_proc < 18 ? length - bytes_proc : 18);
        int longest_match = find_longest(in, (int) bytes_proc, max_length, &offset, lookbacks);
        lb_push(lookbacks, in[bytes_proc], (int) bytes_proc);
        if (longest_match > 2) {
            int lookahead_offset;
            int lookahead_length = (int) (length - bytes_proc - 1 < 18 ? length - bytes_proc - 1 : 18);
            int lookahead_match = find_longest(in, (int) bytes_proc + 1, lookahead_length, &lookahead_offset, lookbacks);
            if ((longest_match + 1) < lookahead_match) {
                uncomp_buf[uncomp_idx++] = in[bytes_proc];
                put_bit(bit_buf, bit_idx, 1);
                bytes_proc++;
                longest_match = lookahead_match;
                offset = lookahead_offset;
                bit_idx++;
                lb_push(lookbacks, in[bytes_proc], (int) bytes_proc);
            }
            for (int i = 1; i < longest_match; i++)
                lb_push(lookbacks, in[bytes_proc + i], (int) bytes_proc + i);
            comp_buf[comp_idx] = (unsigned char) ((((longest_match - 3) & 0x0F) << 4) | (((offset - 1) >> 8) & 0x0F));
            comp_buf[comp_idx + 1] = (unsigned char) ((offset - 1) & 0xFF);
            comp_idx += 2;
            put_bit(bit_buf, bit_idx, 0);
            bytes_proc += (unsigned int) longest_match;
        } else {
            uncomp_buf[uncomp_idx++] = in[bytes_proc];
            put_bit(bit_buf, bit_idx, 1);
            bytes_proc++;
        }
        bit_idx++;
    }
    const unsigned int bit_length = ((unsigned int) bit_idx + 7) / 8;
    const unsigned int comp_offset = (16 + bit_length + 3) & ~3u;
    const unsigned int uncomp_offset = comp_offset + (unsigned int) comp_idx;
    const unsigned int bytes_written = uncomp_offset + (unsigned int) uncomp_idx;
    memcpy(out, "MIO0", 4);
    wr_be32(out + 4, length);
    wr_be32(out + 8, comp_offset);
    wr_be32(out + 12, uncomp_offset);
    memset(out + 16, 0, 16 + bit_length);
    memcpy(out + 16, bit_buf, bit_length);
    memcpy(out + comp_offset, comp_buf, (size_t) comp_idx);
    memcpy(out + uncomp_offset, uncomp_buf, (size_t) uncomp_idx);
    free(bit_buf);
    free(comp_buf);
    free(uncomp_buf);
    for (int i = 0; i < RK_LB_COUNT; i++)
        free(lookbacks[i].indexes);
    free(lookbacks);
    return bytes_written;
}

uint32_t rk_mio0_decode(const uint8_t *in, size_t avail, uint8_t **out, uint32_t *raw_len) {
    const uint32_t d = mio0_check(in, avail);
    if (!d)
        return 0;
    uint8_t *o = malloc(d);
    if (!o || !mio0_decode(in, avail, o, d)) {
        free(o);
        return 0;
    }
    /* the stream's end: the last literal byte used */
    const uint32_t comp = be32(in + 8), unc = be32(in + 12);
    uint32_t written = 0, bit = 0, ui = 0;
    (void) comp;
    while (written < d) {
        if (in[16 + bit / 8] & (1 << (7 - (bit % 8)))) {
            ui++;
            written++;
        } else {
            const uint8_t *v = &in[comp + 2 * (bit - ui)];
            written += ((v[0] & 0xF0u) >> 4) + 3;
        }
        bit++;
    }
    *out = o;
    *raw_len = d;
    return unc + ui;
}

/* ---- recipes ---- */
static int get32(const uint8_t **p, const uint8_t *end, uint32_t *v) {
    if (end - *p < 4)
        return 0;
    memcpy(v, *p, 4);
    *p += 4;
    return 1;
}

int rk_recipe_open(rk_recipe *r, const uint8_t *buf, size_t n) {
    if (n < 28 || memcmp(buf, "MK64RCP1", 8))
        return 0;
    r->p = buf + 8;
    r->end = buf + n;
    return get32(&r->p, r->end, &r->rom_size) && get32(&r->p, r->end, &r->rom_crc) &&
           get32(&r->p, r->end, &r->pool_size) && get32(&r->p, r->end, &r->nfiles);
}

int rk_recipe_next(rk_recipe *r, rk_file *f) {
    if (r->end - r->p < 64)
        return 0;
    memcpy(f->name, r->p, 64);
    f->name[64] = 0;
    r->p += 64;
    return get32(&r->p, r->end, &f->size) && get32(&r->p, r->end, &f->crc) && get32(&r->p, r->end, &f->nops);
}

/* runs nops ops into o (cap bytes; o NULL: only steps over them); the bytes made, or -1 */
static long run_ops(const uint8_t **pp, const uint8_t *end, uint32_t nops, const uint8_t *pool, uint32_t pool_size,
                    uint8_t *o, uint32_t cap, uint32_t base, void (*progress)(uint32_t)) {
    const uint8_t *p = *pp;
    uint32_t at = 0;
    static const uint32_t mask[4] = { 0, 0, 1, 3 };
    for (uint32_t k = 0; k < nops; k++) {
        if (p >= end)
            return -1;
        const uint8_t kind = *p++;
        uint32_t a, len;
        if (kind == 0) {
            if (!get32(&p, end, &len) || (uint32_t) (end - p) < len || at + len > cap)
                return -1;
            if (o) memcpy(o + at, p, len);
            p += len;
        } else if (kind <= 3) {
            if (!get32(&p, end, &a) || !get32(&p, end, &len) || at + len > cap || a > pool_size || len > pool_size - a)
                return -1;
            if (o) {
                const uint32_t m = mask[kind];
                for (uint32_t q = 0; q < len; q++)
                    o[at + q] = pool[(a + q) ^ m];
            }
        } else if (kind == 4) {
            uint32_t rawlen, sub;
            if (!get32(&p, end, &rawlen) || !get32(&p, end, &sub) || rawlen > (64u << 20))
                return -1;
            if (o) {
                uint8_t *raw = malloc(rawlen ? rawlen : 1), *enc = malloc((size_t) rawlen * 2 + 64);
                long got = raw && enc ? run_ops(&p, end, sub, pool, pool_size, raw, rawlen, 0, NULL) : -1;
                len = got == (long) rawlen ? rk_mio0_encode(raw, rawlen, enc) : 0;
                if (got != (long) rawlen || at + len > cap) {
                    free(raw);
                    free(enc);
                    return -1;
                }
                memcpy(o + at, enc, len);
                free(raw);
                free(enc);
            } else {
                if (run_ops(&p, end, sub, pool, pool_size, NULL, rawlen, 0, NULL) < 0)
                    return -1;
                len = 0;   /* (unknown without compressing: only used to step over) */
            }
        } else if (kind == 5) {
            if (p >= end)
                return -1;
            const uint8_t v = *p++;
            if (!get32(&p, end, &len) || at + len > cap)
                return -1;
            if (o) memset(o + at, v, len);
        } else {
            return -1;
        }
        at += len;
        if (progress && (k & 255) == 255)
            progress(base + at);
    }
    *pp = p;
    return (long) at;
}

int rk_recipe_skip(rk_recipe *r, const rk_file *f) {
    return run_ops(&r->p, r->end, f->nops, NULL, 0xFFFFFFFFu, NULL, 0xFFFFFFFFu, 0, NULL) >= 0;
}

int rk_recipe_build(rk_recipe *r, const rk_file *f, const uint8_t *pool, uint32_t pool_size, uint8_t *out,
                    void (*progress)(uint32_t done)) {
    const long got = run_ops(&r->p, r->end, f->nops, pool, pool_size, out, f->size, 0, progress);
    return got == (long) f->size && rk_crc32(0, out, f->size) == f->crc;
}
