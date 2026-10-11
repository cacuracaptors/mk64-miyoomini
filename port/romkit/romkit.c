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
