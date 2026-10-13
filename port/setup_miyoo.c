/*
 * First-run setup: the published package carries no game data. The first time
 * the game starts, it rebuilds what it needs (dc_data/) out of the player's own
 * Mario Kart 64 (USA) ROM, following mk64.rcp (see port/romkit/romkit.h), then
 * deletes the ROM so it doesn't take room on the SD card. Every start after that
 * only fills the executable's game data back in from dc_data/romdata.bin.
 *
 * The screen is drawn straight into /dev/fb0 (before the game's own video is up),
 * upside down like the rest of the port, the way the Tomb Raider (OpenLara) port
 * does its first run.
 *
 * A folder without mk64.rcp (a development build) skips all of this.
 * SETUP_TEST (PC test build): the screen is a 640x480 buffer saved as .ppm files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifndef SETUP_TEST
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#endif

#include "romkit/romkit.c"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare" /* tinf, as published */
#include "romkit/tinflate.c"
#pragma GCC diagnostic pop
#include "setup_font.h"

#define RECIPE_FILE  "mk64.rcp"
#define STAMP_FILE   "dc_data/setup.ok"
#define ROMDATA_FILE "dc_data/romdata.bin"

void port_setup(void);

/* ---- the screen -------------------------------------------------------------- */

static struct {
    uint8_t *mem;
    int w, h, stride, bpp, pages;
    size_t size;
#ifdef SETUP_TEST
    uint32_t *shadow;
#endif
} screen;

static void screen_open(void) {
    if (screen.mem)
        return;
#ifdef SETUP_TEST
    screen.w = 640; screen.h = 480; screen.bpp = 4; screen.stride = 640 * 4; screen.pages = 1;
    screen.size = (size_t) screen.stride * screen.h;
    screen.mem = calloc(1, screen.size);
    screen.shadow = calloc(640 * 480, 4);
#else
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0)
        return;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0 &&
        (var.bits_per_pixel == 16 || var.bits_per_pixel == 32)) {
        void *mem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mem != MAP_FAILED) {
            screen.mem = mem;
            screen.size = fix.smem_len;
            screen.w = (int) var.xres;
            screen.h = (int) var.yres;
            screen.stride = (int) fix.line_length;
            screen.bpp = (int) var.bits_per_pixel / 8;
            screen.pages = var.yres ? (int) (var.yres_virtual / var.yres) : 1;
            if (screen.pages < 1)
                screen.pages = 1;
            if ((size_t) screen.pages * screen.h * screen.stride > screen.size)
                screen.pages = 1;
        }
    }
    close(fd); /* the mapping stays valid */
#endif
}

/* every page, so it shows whichever one the display is scanning out */
static void fill_rect(int x0, int y0, int w, int h, uint32_t rgb) {
    if (!screen.mem)
        return;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > screen.w) w = screen.w - x0;
    if (y0 + h > screen.h) h = screen.h - y0;
    if (w <= 0 || h <= 0)
        return;
    const uint16_t c16 = (uint16_t) (((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
    const uint32_t c32 = 0xFF000000u | rgb;
    for (int page = 0; page < screen.pages; page++) {
        uint8_t *base = screen.mem + (size_t) page * screen.h * screen.stride;
        for (int y = y0; y < y0 + h; y++) {
            /* the panel is mounted rotated by 180 degrees: (x, y) is (w-1-x, h-1-y) */
            uint8_t *row = base + (size_t) (screen.h - 1 - y) * screen.stride;
            for (int x = x0; x < x0 + w; x++) {
                const int px = screen.w - 1 - x;
                if (screen.bpp == 4)
                    ((uint32_t *) row)[px] = c32;
                else
                    ((uint16_t *) row)[px] = c16;
            }
        }
    }
#ifdef SETUP_TEST
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            screen.shadow[y * 640 + x] = rgb;
#endif
}

static void draw_text(int x, int y, const char *s, int scale, uint32_t rgb) {
    for (; *s; s++, x += FONT_W * scale) {
        int ch = (unsigned char) *s;
        if (ch < 32 || ch > 126)
            ch = '?';
        const uint16_t *glyph = font + (ch - 32) * FONT_H;
        for (int gy = 0; gy < FONT_H; gy++) {
            const uint16_t bits = glyph[gy];
            for (int gx = 0; bits && gx < FONT_W; gx++)
                if (bits & (1 << (FONT_W - 1 - gx)))
                    fill_rect(x + gx * scale, y + gy * scale, scale, scale, rgb);
        }
    }
}

static void draw_centered(int y, const char *s, int scale, uint32_t rgb) {
    draw_text((screen.w - (int) strlen(s) * FONT_W * scale) / 2, y, s, scale, rgb);
}

#define COLOR_BG    0x101418
#define COLOR_TITLE 0xE0B040
#define COLOR_TEXT  0xE8E8E8
#define COLOR_DIM   0x9098A0
#define COLOR_BAR   0xE0B040
#define COLOR_FRAME 0x505860
#define COLOR_ERROR 0xF06050
enum { BAR_X = 120, BAR_Y = 300, BAR_W = 400, BAR_H = 26 };

#ifdef SETUP_TEST
static void dump_screen(const char *name) {
    FILE *f = fopen(name, "wb");
    fprintf(f, "P6 640 480 255\n");
    for (int i = 0; i < 640 * 480; i++) {
        const uint8_t px[3] = { (uint8_t) (screen.shadow[i] >> 16), (uint8_t) (screen.shadow[i] >> 8),
                                (uint8_t) screen.shadow[i] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}
#endif

/* title and the two lines under it; the lower part changes as it goes */
static void draw_screen(const char *line1, const char *line2, uint32_t color) {
    fill_rect(0, 0, screen.w, screen.h, COLOR_BG);
    draw_centered(90, "MARIO KART 64", 2, COLOR_TITLE);
    draw_centered(160, "First-time setup", 1, COLOR_DIM);
    draw_centered(215, line1, 1, color);
    draw_centered(245, line2, 1, color);
}

static void draw_status(const char *line1, const char *line2, uint32_t color) {
    fill_rect(0, BAR_Y - 4, screen.w, screen.h - (BAR_Y - 4), COLOR_BG);
    draw_centered(BAR_Y + 10, line1, 1, color);
    draw_centered(BAR_Y + 40, line2, 1, color);
}

static uint64_t work_total, work_base, work_done;
static int shown_percent = -1;

static void draw_progress(void) {
    int percent = work_total ? (int) (work_done * 100 / work_total) : 0;
    if (percent > 100)
        percent = 100;
    if (percent == shown_percent)
        return;
    shown_percent = percent;
    fill_rect(BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, COLOR_FRAME);
    fill_rect(BAR_X, BAR_Y, BAR_W, BAR_H, COLOR_BG);
    fill_rect(BAR_X, BAR_Y, BAR_W * percent / 100, BAR_H, COLOR_BAR);
    char text[16];
    snprintf(text, sizeof(text), "%d%%", percent);
    fill_rect(0, BAR_Y + BAR_H + 10, screen.w, FONT_H + 4, COLOR_BG);
    draw_centered(BAR_Y + BAR_H + 12, text, 1, COLOR_TEXT);
#ifdef SETUP_TEST
    static int dumped;
    if (percent >= 42 && !dumped++)
        dump_screen("setup_progress.ppm");
#endif
}

static uint32_t pool_weight;
static void on_pool(int permille) {
    work_done = work_base + (uint64_t) pool_weight * (uint32_t) permille / 1000;
    draw_progress();
}
static void on_file(uint32_t done) {
    work_done = work_base + done;
    draw_progress();
}

/* back to the OnionOS menu (quit) or on to the game; the screen goes black either way */
static void finish(int wait_seconds, int quit) {
#ifdef SETUP_TEST
    (void) wait_seconds;
    dump_screen(quit ? "setup_error.ppm" : "setup_done.ppm");
#else
    sleep(wait_seconds);
#endif
    fill_rect(0, 0, screen.w, screen.h, 0);
#ifndef SETUP_TEST
    if (screen.mem)
        munmap(screen.mem, screen.size);
    screen.mem = NULL;
#endif
    if (quit)
        exit(0);
}

static void fail_screen(const char *l1, const char *l2, const char *s1, const char *s2) {
    screen_open();
    draw_screen(l1, l2, COLOR_ERROR);
    draw_status(s1, s2, COLOR_TEXT);
    finish(10, 1);
}

/* a name short enough for one line of the screen */
static const char *short_name(const char *name, char *buf, size_t n) {
    const size_t len = strlen(name), max = 36;
    if (len <= max)
        return name;
    snprintf(buf, n, "%.*s...%s", (int) (max - 7), name, name + len - 4);
    return buf;
}

/* ---- files ----------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t max, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long n = ftell(f);
        if (n > 0 && (size_t) n <= max && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t) n))) {
            if (fread(buf, 1, (size_t) n, f) == (size_t) n) {
                *size = (size_t) n;
            } else {
                free(buf);
                buf = NULL;
            }
        }
    }
    fclose(f);
    return buf;
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    int ok = fwrite(data, 1, size, f) == size;
    ok &= fflush(f) == 0;
    ok &= fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
    return ok;
}

static int make_parents(const char *path) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *s = dir + 1; *s; s++) {
        if (*s != '/')
            continue;
        *s = 0;
        if (mkdir(dir, 0777) != 0 && errno != EEXIST)
            return 0;
        *s = '/';
    }
    return 1;
}

static int has_suffix(const char *name, const char *suffix) {
    const size_t l = strlen(name), s = strlen(suffix);
    return l > s && !strcasecmp(name + l - s, suffix);
}

static int is_rom_name(const char *name) {
    return has_suffix(name, ".z64") || has_suffix(name, ".n64") || has_suffix(name, ".v64");
}

/* ---- the ROM ------------------------------------------------------------------- */

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }

/* the ROM inside a .zip (stored or deflated); NULL with why set */
static uint8_t *rom_from_zip(const uint8_t *zip, size_t n, uint32_t want, const char **why) {
    *why = "has no N64 ROM inside (.z64, .n64 or .v64)";
    size_t eocd = 0;
    int found = 0;
    for (size_t i = n >= 22 ? n - 22 : 0; n >= 22; i--) {
        if (rd32(zip + i) == 0x06054b50) {
            eocd = i;
            found = 1;
            break;
        }
        if (i == 0 || n - i > 65557)
            break;
    }
    if (!found) {
        *why = "is not a readable .zip file";
        return NULL;
    }
    const uint32_t count = rd16(zip + eocd + 10);
    size_t p = rd32(zip + eocd + 16);
    for (uint32_t k = 0; k < count; k++) {
        if (p + 46 > n || rd32(zip + p) != 0x02014b50)
            break;
        const uint32_t method = rd16(zip + p + 10), csize = rd32(zip + p + 20), usize = rd32(zip + p + 24);
        const uint32_t nlen = rd16(zip + p + 28), elen = rd16(zip + p + 30), clen = rd16(zip + p + 32);
        const uint32_t lho = rd32(zip + p + 42);
        char name[256];
        if (p + 46 + nlen > n)
            break;
        snprintf(name, sizeof(name), "%.*s", (int) nlen, (const char *) zip + p + 46);
        p += 46 + (size_t) nlen + elen + clen;
        if (!is_rom_name(name))
            continue;
        if (usize != want) {
            *why = "holds a ROM of a different size";
            continue;
        }
        if ((size_t) lho + 30 > n || rd32(zip + lho) != 0x04034b50)
            continue;
        const size_t data = (size_t) lho + 30 + rd16(zip + lho + 26) + rd16(zip + lho + 28);
        if (data > n || csize > n - data)
            continue;
        uint8_t *rom = malloc(want);
        if (!rom) {
            *why = "is too big for the memory left";
            return NULL;
        }
        unsigned int got = want;
        if (method == 0 && csize == want) {
            memcpy(rom, zip + data, want);
            return rom;
        }
        if (method == 8 && tinf_uncompress(rom, &got, zip + data, csize) == TINF_OK && got == want)
            return rom;
        free(rom);
        *why = "could not be unpacked (damaged .zip?)";
    }
    return NULL;
}

/* the ROM in .z64 order, size and CRC checked against the recipe; NULL with why set */
static uint8_t *load_rom(const char *path, const rk_recipe *r, const char **why) {
    struct stat st;
    const int zipped = has_suffix(path, ".zip");
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        *why = "could not be read";
        return NULL;
    }
    if (!zipped && (uint64_t) st.st_size != r->rom_size) {
        *why = "has the wrong size for this game";
        return NULL;
    }
    if (zipped && (uint64_t) st.st_size > 64u * 1024 * 1024) {
        *why = "is too big to be this game";
        return NULL;
    }
    uint8_t *buf = malloc((size_t) st.st_size);
    FILE *f = fopen(path, "rb");
    if (!buf || !f) {
        free(buf);
        if (f)
            fclose(f);
        *why = buf ? "could not be read" : "is too big for the memory left";
        return NULL;
    }
    /* reading counts as r->rom_size of the work, whatever the file's size */
    size_t got = 0;
    while (got < (size_t) st.st_size) {
        const size_t chunk = (size_t) st.st_size - got < (1u << 20) ? (size_t) st.st_size - got : (1u << 20);
        if (fread(buf + got, 1, chunk, f) != chunk)
            break;
        got += chunk;
        work_done = work_base + (uint64_t) r->rom_size * got / (size_t) st.st_size;
        draw_progress();
    }
    fclose(f);
    if (got != (size_t) st.st_size) {
        free(buf);
        *why = "could not be read";
        return NULL;
    }
    uint8_t *rom = buf;
    if (zipped) {
        rom = rom_from_zip(buf, got, r->rom_size, why);
        free(buf);
        if (!rom)
            return NULL;
    }
    if (!rk_normalize_rom(rom, r->rom_size)) {
        free(rom);
        *why = "is not an N64 ROM";
        return NULL;
    }
    const uint32_t crc = rk_crc32(0, rom, r->rom_size);
    if (crc != r->rom_crc) {
        fprintf(stderr, "setup: %s: CRC32 %08x, expected %08x\n", path, crc, r->rom_crc);
        free(rom);
        *why = "is not the USA version, or was modified";
        return NULL;
    }
    return rom;
}

/* ---- building dc_data/ ---------------------------------------------------------- */

static int safe_name(const char *name) {
    return name[0] && name[0] != '/' && !strstr(name, "..") && strlen(name) < 200;
}

static void run_setup(const uint8_t *recipe, size_t recipe_size, uint32_t recipe_crc) {
    rk_recipe r;
    rk_file file;
    if (!rk_recipe_open(&r, recipe, recipe_size))
        fail_screen("The file " RECIPE_FILE " is damaged.", "Please install the game again.", "", "");

    /* the work: reading the ROM, the pool, then every file */
    uint64_t files_total = 0;
    rk_recipe scan = r;
    for (uint32_t i = 0; i < r.nfiles; i++) {
        if (!rk_recipe_next(&scan, &file) || !safe_name(file.name) || !rk_recipe_skip(&scan, &file))
            fail_screen("The file " RECIPE_FILE " is damaged.", "Please install the game again.", "", "");
        files_total += file.size;
    }
    pool_weight = r.pool_size;
    work_total = (uint64_t) r.rom_size + r.pool_size + files_total;

    /* the ROM: any .z64/.n64/.v64/.zip in this folder, the first one that is the right one */
    DIR *d = opendir(".");
    char rom_path[256] = "", tried[256] = "";
    const char *why = NULL;
    uint8_t *rom = NULL;
    struct dirent *de;
    while (!rom && d && (de = readdir(d)) != NULL) {
        struct stat st;
        if (!is_rom_name(de->d_name) && !has_suffix(de->d_name, ".zip"))
            continue;
        if (stat(de->d_name, &st) != 0 || !S_ISREG(st.st_mode) || strlen(de->d_name) >= sizeof(rom_path))
            continue;
        screen_open();
        draw_screen("Building the game data from your ROM.", "This takes about five minutes and only happens once.",
                    COLOR_TEXT);
        work_base = work_done = 0;
        shown_percent = -1;
        draw_progress();
        fprintf(stderr, "setup: reading %s\n", de->d_name);
        rom = load_rom(de->d_name, &r, &why);
        if (rom)
            snprintf(rom_path, sizeof(rom_path), "%s", de->d_name);
        else {
            fprintf(stderr, "setup: %s %s\n", de->d_name, why);
            snprintf(tried, sizeof(tried), "%s", de->d_name);
        }
    }
    if (d)
        closedir(d);
    if (!rom) {
        if (!tried[0])
            fail_screen("Mario Kart 64 (USA) ROM not found.", "Copy it (.z64, .n64, .v64 or .zip) into",
                        "Roms/PORTS/Games/MarioKart64", "and start the game again.");
        char shortn[48], l1[96], l2[96];
        snprintf(l1, sizeof(l1), "%s", short_name(tried, shortn, sizeof(shortn)));
        snprintf(l2, sizeof(l2), "%s.", why);
        fail_screen(l1, l2, "This port needs Mario Kart 64 (USA).", "Copy it here and start the game again.");
    }
    struct stat rom_stat;
    const int rom_mb = stat(rom_path, &rom_stat) == 0 ? (int) ((rom_stat.st_size + (1 << 19)) >> 20) : 0;

    /* enough room? (plus a margin for saves and logs) */
    const uint64_t need = files_total + 8ull * 1024 * 1024;
    struct statvfs vfs;
    if (statvfs(".", &vfs) == 0) {
        const uint64_t free_bytes = (uint64_t) vfs.f_bavail * vfs.f_frsize;
        if (free_bytes < need) {
            char l2[96];
            fprintf(stderr, "setup: not enough space: %llu MB needed, %llu MB free\n",
                    (unsigned long long) (need >> 20), (unsigned long long) (free_bytes >> 20));
            snprintf(l2, sizeof(l2), "%llu MB needed, %llu MB free.", (unsigned long long) (need >> 20),
                     (unsigned long long) (free_bytes >> 20));
            fail_screen("Not enough free space on the SD card:", l2, "Free some space and start the game again.", "");
        }
    }

    /* the pool: the ROM and every block of it unpacked */
    work_base = r.rom_size;
    size_t pool_size = 0;
    uint8_t *pool = rk_build_pool(rom, r.rom_size, &pool_size, on_pool);
    free(rom);
    if (!pool || pool_size != r.pool_size) {
        fprintf(stderr, "setup: pool %zu bytes, expected %u\n", pool_size, r.pool_size);
        draw_status("Setup failed: not enough memory?", "The ROM was kept. Start the game again.", COLOR_ERROR);
        finish(10, 1);
    }
    work_base += r.pool_size;

    /* every file: to name.tmp, checked, then renamed */
    remove(STAMP_FILE);
    int ok = 1;
    for (uint32_t i = 0; i < r.nfiles && ok; i++) {
        char tmp[256];
        ok = rk_recipe_next(&r, &file);
        uint8_t *out = ok ? malloc(file.size ? file.size : 1) : NULL;
        snprintf(tmp, sizeof(tmp), "%s.tmp", file.name);
        if (!out) {
            fprintf(stderr, "setup: %s: out of memory\n", file.name);
            ok = 0;
        } else if (!rk_recipe_build(&r, &file, pool, pool_size, out, on_file)) {
            fprintf(stderr, "setup: %s: rebuilt bytes do not match\n", file.name);
            ok = 0;
        } else if (!make_parents(file.name) || !write_file(tmp, out, file.size) || rename(tmp, file.name) != 0) {
            fprintf(stderr, "setup: %s: %s\n", file.name, strerror(errno));
            remove(tmp);
            ok = 0;
        }
        free(out);
        work_base += file.size;
    }
    free(pool);

    char stamp[16];
    snprintf(stamp, sizeof(stamp), "%08x\n", recipe_crc);
    if (ok)
        ok = write_file(STAMP_FILE ".tmp", stamp, strlen(stamp)) && rename(STAMP_FILE ".tmp", STAMP_FILE) == 0;
    if (!ok) {
        draw_status("Setup failed. The reason is in log.txt,", "in this game's folder.", COLOR_ERROR);
        finish(10, 1);
    }
    sync();

    /* everything the game needs is there: the ROM is no longer used */
    char shortn[48], l1[96], l2[96];
    if (unlink(rom_path) == 0) {
        fprintf(stderr, "setup: done, %s removed (%d MB freed)\n", rom_path, rom_mb);
        snprintf(l1, sizeof(l1), "Done! %s was removed", short_name(rom_path, shortn, sizeof(shortn)));
    } else {
        fprintf(stderr, "setup: done, could not remove %s: %s\n", rom_path, strerror(errno));
        snprintf(l1, sizeof(l1), "Done! You can now delete %s", short_name(rom_path, shortn, sizeof(shortn)));
    }
    snprintf(l2, sizeof(l2), "to free %d MB. Starting the game...", rom_mb);
    work_done = work_total;
    draw_progress();
    draw_status(l1, l2, COLOR_TEXT);
    sync();
    finish(4, 0);
}

/* ---- every start: the executable's game data -------------------------------------- */

#ifdef SETUP_TEST
char test_data[4096];
#endif

/* the writable mappings of this process (romdata.bin only ever names the
   executable's .data) */
static unsigned long wr_lo[256], wr_hi[256];
static int wr_count = -1;

static int writable(uintptr_t a, uintptr_t size) {
    if (wr_count < 0) {
        wr_count = 0;
        FILE *f = fopen("/proc/self/maps", "r");
        char line[512];
        while (f && wr_count < 256 && fgets(line, sizeof(line), f)) {
            char perms[8];
            if (sscanf(line, "%lx-%lx %7s", &wr_lo[wr_count], &wr_hi[wr_count], perms) == 3 && perms[1] == 'w')
                wr_count++;
        }
        if (f)
            fclose(f);
    }
    for (int i = 0; i < wr_count; i++)
        if (a >= wr_lo[i] && a < wr_hi[i])
            return size <= wr_hi[i] - a;
    return 0;
}

/* 1: in place (or the executable already had it), 0: romdata.bin missing or wrong */
static int load_romdata(void) {
    FILE *f = fopen(ROMDATA_FILE, "rb");
    if (!f)
        return 0;
    uint8_t head[12];
    uint32_t n = 0, *table = NULL;
    int ok = fread(head, 1, 12, f) == 12 && !memcmp(head, "MK64ROMD", 8) && (n = rd32(head + 8)) > 0 && n < 100000 &&
             (table = malloc(n * 8)) && fread(table, 8, n, f) == n;
    int stripped = 1;
    for (uint32_t i = 0; ok && i < n; i++) {
        const uintptr_t a = table[i * 2], s = table[i * 2 + 1];
        ok = writable(a, s);
        for (uintptr_t k = 0; ok && stripped && k < s; k++)
            stripped = ((const uint8_t *) a)[k] == 0;
    }
    if (ok && !stripped) {
        /* a development build: its data is already in it */
        fprintf(stderr, "setup: the executable already has its game data; %s not used\n", ROMDATA_FILE);
    } else {
        for (uint32_t i = 0; ok && i < n; i++)
            ok = fread((void *) (uintptr_t) table[i * 2], 1, table[i * 2 + 1], f) == table[i * 2 + 1];
    }
    free(table);
    fclose(f);
    if (!ok)
        fprintf(stderr, "setup: %s missing, damaged or from another version\n", ROMDATA_FILE);
    return ok;
}

/* A development build (port/build_miyoo.sh) carries the game data in the executable; the
   release (port/release_miyoo.sh) has it zeroed, to be filled in from romdata.bin. Asked
   of the data itself, since romdata.bin's table of addresses belongs to the build it was
   made for: a development build sent over a release install moved the addresses, did not
   match it, and stopped at "The game data is damaged" (and dropped setup.ok). */
static int has_own_data(void) {
    extern uint8_t _data_segment2SegmentRomStart[], _data_segment2SegmentRomEnd[];
    const uint8_t *p = _data_segment2SegmentRomStart, *e = _data_segment2SegmentRomEnd;
    size_t samples = 0, set = 0;
    for (; p < e; p += 61, samples++)
        set += *p != 0;
    return samples > 1000 && set * 4 > samples;
}

void port_setup(void) {
    if (has_own_data()) {
        fprintf(stderr, "setup: the executable already has its game data (development build)\n");
        return;
    }
    size_t recipe_size = 0;
    uint8_t *recipe = read_file(RECIPE_FILE, 64u << 20, &recipe_size);
    if (!recipe)
        return; /* a development build: dc_data/ comes with it */
    const uint32_t recipe_crc = rk_crc32(0, recipe, recipe_size);

    size_t stamp_size = 0;
    uint8_t *stamp = read_file(STAMP_FILE, 64, &stamp_size);
    unsigned int stamped = 0;
    const int done = stamp && sscanf((const char *) stamp, "%8x", &stamped) == 1 && stamped == recipe_crc;
    free(stamp);
    if (!done) {
        fprintf(stderr, "setup: first run, building dc_data/ from the ROM\n");
        tinf_init();
        run_setup(recipe, recipe_size, recipe_crc);
    }
    free(recipe);

    if (!load_romdata()) {
        remove(STAMP_FILE); /* so the next start rebuilds it */
        fail_screen("The game data is damaged.", "Copy your Mario Kart 64 (USA) ROM into",
                    "Roms/PORTS/Games/MarioKart64", "and start the game again.");
    }
}
