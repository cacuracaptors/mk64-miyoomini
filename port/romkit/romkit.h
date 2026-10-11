/*
 * romkit -- rebuilds the game data this port needs out of the player's own
 * Mario Kart 64 (USA) ROM, following a "recipe" that holds no game data: only
 * where each piece comes from in the ROM (or in a block of the ROM once
 * decompressed), with which byte order, plus the few bytes this port changed
 * (pointers, fixes), which are the port's own.
 *
 * Shared by the PC tool that writes the recipe (romrecipe.c) and the first-run
 * setup on the device (port/setup_miyoo.c).
 *
 * The "pool" a recipe copies from is the ROM in big-endian (.z64) order,
 * followed by every MIO0 block found in it, decompressed, each padded to 4
 * bytes. A copy reads the pool directly or with each 2 or 4 bytes reversed
 * (byte p of the view is pool[p ^ 1] or pool[p ^ 3]): the port keeps most data
 * in the CPU's little-endian order.
 *
 * Recipe file (little-endian):
 *   "MK64RCP1", u32 rom_size, u32 rom_crc (of the .z64 bytes), u32 pool_size,
 *   u32 file count; per file: char name[64], u32 size, u32 crc, u32 op count,
 *   ops: u8 0 + u32 len + bytes (literal), or u8 1..3 + u32 pool offset + u32
 *   len (copy through view 0 = as is, 1 = 2-byte swap, 2 = 4-byte swap).
 */
#ifndef ROMKIT_H
#define ROMKIT_H

#include <stdint.h>
#include <stddef.h>

#define RK_ROM_SIZE_US 0x00C00000u   /* 12 MB */

uint32_t rk_crc32(uint32_t crc, const void *buf, size_t n);

/* any of .z64 / .v64 / .n64 to .z64 order in place; 0 if it is not an N64 ROM */
int rk_normalize_rom(uint8_t *rom, size_t size);

/* the pool for a ROM already in .z64 order (malloc'ed; *size set); NULL on failure.
   progress (optional) gets 0..1000 */
uint8_t *rk_build_pool(const uint8_t *rom, size_t rom_size, size_t *size, void (*progress)(int));

static inline uint8_t rk_view(const uint8_t *pool, uint32_t p, int view) {
    static const uint32_t mask[3] = { 0, 1, 3 };
    return pool[p ^ mask[view]];
}

#endif
