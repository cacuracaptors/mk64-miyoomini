/*
 * savestate_miyoo.c -- save states, OnionOS's Game Switcher and the save at power-off.
 *
 * A state is the game's writable memory: the .data and .bss of the game's own objects, which
 * port/ld/savestate.ld gathers between __mk_state_start and __mk_state_end. Left out: the
 * port layer and the renderer (screen, sound output, buttons, textures, threads: they carry
 * on as they are), the copies of libc's variables, and the ROM data rebuilt the same at every
 * start (textures, staff ghosts, the sound "ROM"). The executable is not position
 * independent, so the game's memory sits at the same addresses in every run of the same
 * build; a state records a checksum of the code and is refused by any other build.
 *
 * Saved and loaded only between two frames (port_state_boundary, from the game loop), never
 * in the middle of one. Pages of zeros are not written (a bitmap says which pages are in the
 * file) and the rest is compressed with zlib when the device has it (port/zdyn_miyoo.c).
 * After a load: the renderer forgets its textures (same addresses, other contents), the
 * mirror-mode course vertices are set back, and the game's copy of the save data (records,
 * cups won) is taken again from the EEPROM file, so a state never rolls the progress back.
 *
 * Buttons (input_miyoo.c), as in OnionOS's RetroArch:
 *   MENU            what Tweaks > Shortcuts > "In-game: Menu button" sets for a single, a long
 *                   and a double press (config/keymap.json, read as keymon reads it; by
 *                   default the Game Switcher, back to the menu, the previous game). Each
 *                   saves the state (state_auto.mks) and the picture the Game Switcher shows
 *                   first, as keymon has RetroArch do.
 *   MENU+R2 / L2    save / load state_quick.mks, at any time
 *   SELECT+START    closes the game (saving state_auto.mks too, for the Game Switcher)
 *
 * Started from the Game Switcher (OnionOS leaves "switcher" in /tmp/prev_state), the game
 * goes on from state_auto.mks; started from the game list, it starts from the beginning.
 *
 * Power-off (OnionOS creates /tmp/.offOrder and sends SIGTERM; the signal does not always
 * arrive, so the file is checked 4 times a second) and the low battery shutdown
 * (battery_miyoo.c): the state is saved, saves/resume_boot is left, and cmd_to_run.sh (which
 * keymon removes) is put back, so OnionOS opens the game again at the next start and the
 * game goes on from where it was. A script in .tmp_update/checkoff makes OnionOS wait for
 * the save (20 s at most). saves/state_loading guards against a state that crashes the game:
 * while it exists the next start does not resume.
 *
 * The Game Switcher's picture: /mnt/SDCARD/Saves/CurrentProfile/romScreens/<hash>.png, the
 * hash being OnionOS's FNV1A_Pippip_Yurii of the game's path in the recent list (the .port
 * shortcut), as screenshot_system() writes it for RetroArch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "port_miyoo.h"
#include "zdyn_miyoo.h"
#include "setup_font.h"

extern char __mk_state_start[], __mk_state_end[];
extern char __rom_data_start[], __rom_data_end[], __rom_bss_start[], __rom_bss_end[];
extern char __executable_start[], etext[];

/* the renderer (src/gfx/gfx_retro_dc.c, port/gfx_sw.c) and the saves (saves_miyoo.c) */
void gfx_mirror_state_get(uint32_t out[2]);
void gfx_mirror_state_set(const uint32_t in[2]);
void gfx_pvr_request_cache_flush(void);
void port_saves_reapply(void);

#define SAVE_DIR "saves"
#define AUTO_STATE SAVE_DIR "/state_auto.mks"
#define QUICK_STATE SAVE_DIR "/state_quick.mks"
#define BOOT_RESUME SAVE_DIR "/resume_boot"
#define LOADING_FLAG SAVE_DIR "/state_loading"
#define ONION_DIR "/mnt/SDCARD/.tmp_update"
#define CMD_TO_RUN ONION_DIR "/cmd_to_run.sh"
#define RUN_SWITCHER ONION_DIR "/.runGameSwitcher"
#define CHECKOFF_DIR ONION_DIR "/checkoff"
#define CHECKOFF_SCRIPT CHECKOFF_DIR "/mk64_save.sh"
#define ROMSCREEN_DIR "/mnt/SDCARD/Saves/CurrentProfile/romScreens"
#define RECENT_LIST "/mnt/SDCARD/Roms/recentlist.json"
#define RECENT_HIDDEN "/mnt/SDCARD/Roms/recentlist-hidden.json"
#define KEYMAP ONION_DIR "/config/keymap.json"

#define PAGE 4096u
#define BOOT_LOAD_FRAME 3      /* the game has set itself up and drawn its first frames */
#define LOADED_OK_FRAMES 300   /* 5 s after a resume: it did not crash the game */

enum { EXIT_QUIT, EXIT_SWITCHER, EXIT_POWEROFF, EXIT_BATTERY, EXIT_QUICK_SWITCH };
/* OnionOS's in-game MENU actions (keymon's activate_RA_action) */
enum { MENU_OFF, MENU_SWITCHER, MENU_EXIT, MENU_QUICK_SWITCH, MENU_RA_MENU };

static volatile sig_atomic_t sLoopRunning, sOffSignal;
static int sRequest[2];        /* PORT_REQ_SAVE, PORT_REQ_LOAD */
static int sMenuPress = -1;    /* PORT_MENU_*, to do at the next boundary */
static int sMenuAction[3] = { MENU_SWITCHER, MENU_EXIT, MENU_QUICK_SWITCH };   /* single, long, double */
static int sOnion;             /* running under OnionOS */
static int sDisabled;
static char sDisabledWhy[96];
static char sCmd[4096];        /* cmd_to_run.sh as it was at the start */
static int sCmdLen = -1;
static int sOffAtStart;
static int sCheckoff;
static double sNextOffPoll;
static char sRomscreen[160];
static unsigned sFrames;
static int sBootLoadPending;
static unsigned sLoadedAt;
static uint32_t sRomDataHash, sRomBssHash;
static int sRomBssHashed;

#define slog printf

static int exists(const char *path) {
    return access(path, F_OK) == 0;
}

static uint32_t hash_words(const void *start, const void *end) {
    const uint32_t *p = start, *e = (const uint32_t *) ((uintptr_t) end & ~(uintptr_t) 3);
    uint32_t h = 2166136261u;
    for (; p < e; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

static uint32_t code_hash(void) {
    static uint32_t h;
    if (!h) {
        h = hash_words(__executable_start, etext);
        if (!h)
            h = 1;
    }
    return h;
}

/* OnionOS's FNV1A_Pippip_Yurii (src/common/utils/hash.h), reading through memcpy */
static uint32_t onion_hash(const char *str, size_t len) {
    const uint32_t prime = 591798841u;
    uint64_t h = 14695981039346656037ULL;
    if (len > 8) {
        size_t cycles = ((len - 1) >> 4) + 1;
        const size_t ndhead = len - (cycles << 3);
        for (; cycles--; str += 8) {
            uint64_t a, b;
            memcpy(&a, str, 8);
            memcpy(&b, str + ndhead, 8);
            h = (h ^ a) * prime;
            h = (h ^ b) * prime;
        }
    } else {
        uint64_t a = 0;
        memcpy(&a, str, len);
        h = (h ^ a) * prime;
    }
    const uint32_t h32 = (uint32_t) (h ^ (h >> 32));
    return h32 ^ (h32 >> 16);
}

/* ---- small file helpers --------------------------------------------------------------- */

static void fsync_path(const char *path) {
    const int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

/* the whole file at once: beside the old one, then renamed over it */
static int write_file(const char *path, const void *data, size_t len) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return 0;
    const uint8_t *p = data;
    size_t left = len;
    while (left) {
        const ssize_t w = write(fd, p, left);
        if (w <= 0) {
            close(fd);
            remove(tmp);
            return 0;
        }
        p += w;
        left -= (size_t) w;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
    fsync_path(path);
    return 1;
}

static void touch(const char *path) {
    write_file(path, "", 0);
}

/* ---- on-screen messages ------------------------------------------------------------------ */

static char sOsd[48];
static double sOsdUntil;

static void osd_show(const char *msg) {
    snprintf(sOsd, sizeof(sOsd), "%s", msg);
    sOsdUntil = port_now_ms() + 2000.0;
}

/* (video_miyoo.c, on every frame shown) bottom left, white on a darkened band */
void port_osd_draw(uint16_t *buf, int w, int h) {
    if (!sOsd[0] || !buf)
        return;
    if (port_now_ms() > sOsdUntil) {
        sOsd[0] = 0;
        return;
    }
    const int n = (int) strlen(sOsd);
    const int x0 = w / 40, y0 = h - FONT_H - h / 30, pad = 4;
    if (x0 + n * FONT_W + pad > w || y0 - pad < 0)
        return;
    for (int y = y0 - pad; y < y0 + FONT_H + pad && y < h; y++) {
        uint16_t *row = buf + y * w;
        for (int x = x0 - pad; x < x0 + n * FONT_W + pad; x++)
            row[x] = (uint16_t) ((row[x] >> 1) & 0x7BEF);
    }
    for (int i = 0; i < n; i++) {
        int ch = (unsigned char) sOsd[i];
        if (ch < 32 || ch > 126)
            ch = '?';
        const uint16_t *glyph = font + (ch - 32) * FONT_H;
        for (int gy = 0; gy < FONT_H; gy++) {
            const uint16_t bits = glyph[gy];
            uint16_t *row = buf + (y0 + gy) * w + x0 + i * FONT_W;
            for (int gx = 0; bits && gx < FONT_W; gx++)
                if (bits & (1 << (FONT_W - 1 - gx)))
                    row[gx] = 0xFFFF;
        }
    }
}

/* ---- the Game Switcher's picture (PNG) ---------------------------------------------------- */

static uint32_t crc_table[256];

static uint32_t crc32_of(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint8_t *put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
    return p + 4;
}

/* raw (filter byte + RGB rows) -> zlib stream: deflate when zlib is there, stored blocks if not */
static size_t png_zlib(const uint8_t *raw, size_t n, uint8_t *out, size_t cap) {
    if (zd_available()) {
        zd_stream z;
        if (zd_deflate_init(&z, 1) != ZD_OK)
            return 0;
        z.next_in = raw;
        z.avail_in = (unsigned) n;
        z.next_out = out;
        z.avail_out = (unsigned) cap;
        const int r = zd_deflate(&z, ZD_FINISH);
        const size_t len = z.total_out;
        zd_deflate_end(&z);
        return r == ZD_STREAM_END ? len : 0;
    }
    uint8_t *p = out;
    *p++ = 0x78;
    *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    for (size_t off = 0; off < n;) {
        const size_t len = n - off > 65535 ? 65535 : n - off;
        if ((size_t) (p - out) + 5 + len + 4 > cap)
            return 0;
        *p++ = off + len >= n ? 1 : 0;
        *p++ = (uint8_t) len;
        *p++ = (uint8_t) (len >> 8);
        *p++ = (uint8_t) ~len;
        *p++ = (uint8_t) (~len >> 8);
        memcpy(p, raw + off, len);
        p += len;
        off += len;
    }
    return (size_t) (put32(p, (b << 16) | a) - out);
}

static int write_png565(const char *path, const uint16_t *px, int w, int h) {
    const size_t stride = (size_t) w * 3 + 1, n = stride * (size_t) h;
    const size_t cap = n + n / 8 + 1024;
    uint8_t *raw = malloc(n), *z = malloc(cap), *file = malloc(cap + 64);
    int ok = 0;
    if (raw && z && file) {
        for (int y = 0; y < h; y++) {
            uint8_t *o = raw + (size_t) y * stride;
            *o++ = 0;   /* filter: none */
            const uint16_t *s = px + (size_t) y * w;
            for (int x = 0; x < w; x++) {
                const uint32_t c = s[x], r = c >> 11, g = (c >> 5) & 63, b = c & 31;
                *o++ = (uint8_t) ((r << 3) | (r >> 2));
                *o++ = (uint8_t) ((g << 2) | (g >> 4));
                *o++ = (uint8_t) ((b << 3) | (b >> 2));
            }
        }
        const size_t zlen = png_zlib(raw, n, z, cap);
        if (zlen) {
            static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
            uint8_t *p = file;
            memcpy(p, sig, 8);
            p += 8;
            /* IHDR: w, h, 8 bits, RGB */
            uint8_t *c = p;
            p = put32(p, 13);
            memcpy(p, "IHDR", 4);
            p += 4;
            p = put32(p, (uint32_t) w);
            p = put32(p, (uint32_t) h);
            *p++ = 8;
            *p++ = 2;
            *p++ = 0;
            *p++ = 0;
            *p++ = 0;
            p = put32(p, crc32_of(0, c + 4, 17));
            c = p;
            p = put32(p, (uint32_t) zlen);
            memcpy(p, "IDAT", 4);
            p += 4;
            memcpy(p, z, zlen);
            p += zlen;
            p = put32(p, crc32_of(0, c + 4, zlen + 4));
            c = p;
            p = put32(p, 0);
            memcpy(p, "IEND", 4);
            p += 4;
            p = put32(p, crc32_of(0, c + 4, 4));
            ok = write_file(path, file, (size_t) (p - file));
        }
    }
    free(raw);
    free(z);
    free(file);
    return ok;
}

static void write_romscreen(void) {
    const uint16_t *px;
    int w, h;
    if (!sRomscreen[0] || !port_video_last_frame(&px, &w, &h))
        return;
    const double t0 = port_now_ms();
    mkdir(ROMSCREEN_DIR, 0777);
    const int ok = write_png565(sRomscreen, px, w, h);
    slog("estado: imagem do Game Switcher %s (%dx%d, %.0f ms)\n", ok ? "gravada" : "FALHOU", w, h, port_now_ms() - t0);
}

/* the game's line in OnionOS's recent list: the path the Game Switcher hashes */
static void find_romscreen(void) {
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd)))
        return;
    const char *dir = strrchr(cwd, '/');
    dir = dir ? dir + 1 : cwd;   /* the game's folder: MarioKart64 */
    FILE *f = fopen(exists(RECENT_HIDDEN) ? RECENT_HIDDEN : RECENT_LIST, "r");
    if (!f)
        return;
    static char line[8192];
    char path[512];
    while (fgets(line, sizeof(line), f)) {
        const char *t = strstr(line, "\"type\":");
        const char *r = strstr(line, "\"rompath\":\"");
        if (!t || !r)
            continue;
        const int type = atoi(t + 7);
        if (type != 5 && type != 17)
            continue;
        r += 11;
        const char *e = strchr(r, '"');
        if (!e || e - r >= (int) sizeof(path))
            continue;
        memcpy(path, r, (size_t) (e - r));
        path[e - r] = 0;
        const char *colon = strchr(path, ':');   /* "launch:rompath" (the search panel) */
        const char *rom = colon ? colon + 1 : path;
        int ours = strstr(rom, dir) != NULL;
        if (!ours) {   /* the .port shortcut names the folder (GameDir="MarioKart64") */
            FILE *s = fopen(rom, "r");
            if (s) {
                char buf[4096];
                const size_t n = fread(buf, 1, sizeof(buf) - 1, s);
                buf[n] = 0;
                fclose(s);
                ours = strstr(buf, dir) != NULL;
            }
        }
        if (ours) {
            snprintf(sRomscreen, sizeof(sRomscreen), ROMSCREEN_DIR "/%u.png", (unsigned) onion_hash(rom, strlen(rom)));
            slog("estado: na lista de recentes como %s -> %s\n", rom, sRomscreen);
            break;
        }
    }
    fclose(f);
}

/* OnionOS's settings for MENU in a game (keymon: _settings_load_keymap and the old flags) */
static int json_int(const char *json, const char *key, int fallback) {
    char k[64];
    snprintf(k, sizeof(k), "\"%s\"", key);
    const char *p = strstr(json, k);
    if (!p || !(p = strchr(p + strlen(k), ':')))
        return fallback;
    return atoi(p + 1);
}

static void read_menu_settings(void) {
    FILE *f = fopen(KEYMAP, "r");
    if (f) {
        char buf[2048];
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        sMenuAction[PORT_MENU_SINGLE] = json_int(buf, "ingame_single_press", sMenuAction[PORT_MENU_SINGLE]);
        sMenuAction[PORT_MENU_LONG] = json_int(buf, "ingame_long_press", sMenuAction[PORT_MENU_LONG]);
        sMenuAction[PORT_MENU_DOUBLE] = json_int(buf, "ingame_double_press", sMenuAction[PORT_MENU_DOUBLE]);
    }
    if (exists(ONION_DIR "/config/.menuInverted")) {
        sMenuAction[PORT_MENU_SINGLE] = MENU_EXIT;
        sMenuAction[PORT_MENU_LONG] = MENU_SWITCHER;
    }
    if (exists(ONION_DIR "/config/.noGameSwitcher")) {
        sMenuAction[PORT_MENU_SINGLE] = MENU_EXIT;
        sMenuAction[PORT_MENU_LONG] = MENU_OFF;
    }
    slog("estado: MENU (Tweaks): toque %d, segurar %d, dois toques %d (1 Game Switcher, 2 menu, 3 jogo anterior)\n",
           sMenuAction[PORT_MENU_SINGLE], sMenuAction[PORT_MENU_LONG], sMenuAction[PORT_MENU_DOUBLE]);
}

/* the previous game, as keymon's quick switch does it (resumeGame(1), state.h): the second game
   of the recent list moves to the top, its command goes into cmd_to_run.sh, and /tmp/quick_switch
   has runtime.sh start it when this game ends. 0 = there is no previous game. */
static int quick_switch_prepare(void) {
    const char *list = exists(RECENT_HIDDEN) ? RECENT_HIDDEN : RECENT_LIST;
    FILE *f = fopen(list, "r");
    if (!f)
        return 0;
    static char lines[64][2048];
    int n = 0, pick = -1, valid = 0;
    char cmd[2200] = "";
    while (n < 64 && fgets(lines[n], sizeof(lines[n]), f)) {
        const char *line = lines[n++];
        if (pick >= 0)
            continue;
        const char *t = strstr(line, "\"type\":");
        const char *r = strstr(line, "\"rompath\":\"");
        if (!t || !r)
            continue;
        const int type = atoi(t + 7);
        if (type != 5 && type != 17)
            continue;
        char rom[1024], launch[1024] = "";
        r += 11;
        const char *e = strchr(r, '"');
        if (!e || e - r >= (int) sizeof(rom))
            continue;
        memcpy(rom, r, (size_t) (e - r));
        rom[e - r] = 0;
        char *colon = strchr(rom, ':');
        if (colon) {   /* "launch:rompath" */
            *colon = 0;
            snprintf(launch, sizeof(launch), "%s", rom);
            memmove(rom, colon + 1, strlen(colon + 1) + 1);
        } else {
            const char *l = strstr(line, "\"launch\":\"");
            const char *le = l ? strchr(l + 10, '"') : NULL;
            if (le && le - (l + 10) < (int) sizeof(launch)) {
                memcpy(launch, l + 10, (size_t) (le - (l + 10)));
                launch[le - (l + 10)] = 0;
            }
        }
        if (!exists(rom) || !launch[0] || !exists(launch))
            continue;
        if (valid++ == 1) {
            pick = n - 1;
            snprintf(cmd, sizeof(cmd), "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"%s\" \"%s\"", launch, rom);
        }
    }
    const int complete = feof(f);
    fclose(f);
    if (pick < 0)
        return 0;
    if (pick > 0 && complete) {   /* (a list longer than read here is left as it is) */
        static char out[64 * 2048];
        size_t len = 0;
        for (int i = -1; i < n; i++) {
            const int k = i < 0 ? pick : i;
            if (i == pick)
                continue;
            const size_t l = strlen(lines[k]);
            memcpy(out + len, lines[k], l);
            len += l;
        }
        write_file(list, out, len);
    }
    if (!write_file(CMD_TO_RUN, cmd, strlen(cmd)))
        return 0;
    touch("/tmp/quick_switch");
    touch("/tmp/force_auto_load_state");
    slog("estado: jogo anterior: %s\n", cmd);
    return 1;
}

/* ---- the state file ----------------------------------------------------------------------- */

typedef struct {
    char magic[8];           /* "MK64STAT" */
    uint32_t version;        /* 1 */
    uint32_t code_hash;      /* the build that wrote it */
    uint32_t region_start, region_size, page;
    uint32_t zlib;           /* 1: one deflate stream; 0: the pages as they are */
    uint32_t raw_bytes;      /* the pages in the file, uncompressed */
    uint32_t file_bytes;     /* what follows the bitmap */
    uint32_t extra_bytes;    /* sizeof(StateExtra) */
    uint32_t frames;
} StateHeader;

typedef struct {
    uint32_t mirror[2];      /* the mirror-mode course vertices (gfx_retro_dc.c) */
} StateExtra;

static int page_zero(const uint32_t *p) {
    for (unsigned i = 0; i < PAGE / 4; i += 8)
        if (p[i] | p[i + 1] | p[i + 2] | p[i + 3] | p[i + 4] | p[i + 5] | p[i + 6] | p[i + 7])
            return 0;
    return 1;
}

static void check_rom_hashes(void) {
    if (hash_words(__rom_data_start, __rom_data_end) != sRomDataHash)
        slog("estado: AVISO: os dados da ROM no executavel mudaram durante o jogo\n");
    if (sRomBssHashed && hash_words(__rom_bss_start, __rom_bss_end) != sRomBssHash)
        slog("estado: AVISO: a ROM de som mudou durante o jogo\n");
}

static int save_state(const char *path) {
    if (sDisabled) {
        slog("estado: desligado (%s)\n", sDisabledWhy);
        return 0;
    }
    const double t0 = port_now_ms();
    char *const base = __mk_state_start;
    const size_t npages = (size_t) (__mk_state_end - __mk_state_start) / PAGE, nbm = (npages + 7) / 8;
    uint8_t *bm = calloc(nbm, 1);
    if (!bm)
        return 0;
    size_t raw = 0;
    for (size_t p = 0; p < npages; p++)
        if (!page_zero((const uint32_t *) (base + p * PAGE))) {
            bm[p >> 3] |= (uint8_t) (1u << (p & 7));
            raw += PAGE;
        }
    const double t_scan = port_now_ms();
    check_rom_hashes();

    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    StateHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "MK64STAT", 8);
    h.version = 1;
    h.code_hash = code_hash();
    h.region_start = (uint32_t) (uintptr_t) base;
    h.region_size = (uint32_t) (npages * PAGE);
    h.page = PAGE;
    h.zlib = (uint32_t) zd_available();
    h.raw_bytes = (uint32_t) raw;
    h.extra_bytes = sizeof(StateExtra);
    h.frames = sFrames;
    StateExtra x;
    memset(&x, 0, sizeof(x));
    gfx_mirror_state_get(x.mirror);

    int ok = 0;
    const char *why = "abrir";
    uint8_t *zbuf = NULL;
    FILE *f = fopen(tmp, "wb");
    if (f && fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(&x, sizeof(x), 1, f) == 1 && fwrite(bm, 1, nbm, f) == nbm) {
        why = "gravar";
        const long start = ftell(f);
        ok = 1;
        if (h.zlib) {
            const size_t zcap = 256 * 1024;
            zd_stream z;
            zbuf = malloc(zcap);
            if (!zbuf || zd_deflate_init(&z, 1) != ZD_OK) {
                ok = 0;
            } else {
                for (size_t p = 0; p <= npages && ok; p++) {
                    const int last = p == npages;
                    if (!last && !(bm[p >> 3] & (1u << (p & 7))))
                        continue;
                    if (!last) {
                        z.next_in = (const unsigned char *) (base + p * PAGE);
                        z.avail_in = PAGE;
                    }
                    int r;
                    do {
                        z.next_out = zbuf;
                        z.avail_out = (unsigned) zcap;
                        r = zd_deflate(&z, last ? ZD_FINISH : ZD_NO_FLUSH);
                        const size_t got = zcap - z.avail_out;
                        if (r < 0 || (got && fwrite(zbuf, 1, got, f) != got))
                            ok = 0;
                    } while (ok && (last ? r != ZD_STREAM_END : z.avail_in != 0));
                }
                zd_deflate_end(&z);
            }
        } else {
            for (size_t p = 0; p < npages && ok;) {
                if (!(bm[p >> 3] & (1u << (p & 7)))) {
                    p++;
                    continue;
                }
                size_t q = p;
                while (q < npages && (bm[q >> 3] & (1u << (q & 7))))
                    q++;
                const size_t len = (q - p) * PAGE;
                if (fwrite(base + p * PAGE, 1, len, f) != len)
                    ok = 0;
                p = q;
            }
        }
        const long end = ftell(f);
        h.file_bytes = (uint32_t) (end - start);
        if (ok && (fseek(f, 0, SEEK_SET) != 0 || fwrite(&h, sizeof(h), 1, f) != 1))
            ok = 0;
    }
    const double t_write = port_now_ms();
    if (f) {
        if (ok) {
            why = "fsync";
            if (fflush(f) != 0 || fsync(fileno(f)) != 0)
                ok = 0;
        }
        if (fclose(f) != 0)
            ok = 0;
    }
    const double t_sync = port_now_ms();
    if (ok) {
        why = "renomear";
        ok = rename(tmp, path) == 0;
    }
    if (ok) {
        /* the renamed entry onto the card at once (vfat), then the folder */
        fsync_path(path);
        fsync_path(SAVE_DIR);
    } else {
        remove(tmp);
    }
    free(zbuf);
    free(bm);
    const double t_end = port_now_ms();
    if (ok)
        slog("estado: salvo %s: %u de %u paginas (%u KB) -> %u KB %s | varredura %.0f ms, %s+escrita %.0f, fsync %.0f, "
               "fim %.0f, total %.0f\n",
               path, (unsigned) (raw / PAGE), (unsigned) npages, (unsigned) (raw / 1024), (unsigned) (h.file_bytes / 1024),
               h.zlib ? "(zlib)" : "(sem compressao)", t_scan - t0, h.zlib ? "compressao" : "", t_write - t_scan,
               t_sync - t_write, t_end - t_sync, t_end - t0);
    else
        slog("estado: FALHOU ao salvar %s (%s: %s); o anterior ficou como estava\n", path, why, strerror(errno));
    return ok;
}

/* the header and the size checked, nothing loaded: 1 = a state of this build */
static int state_usable(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    StateHeader h;
    const int ok = fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, "MK64STAT", 8) && h.version == 1 &&
                   h.code_hash == code_hash() && h.region_start == (uint32_t) (uintptr_t) __mk_state_start &&
                   h.region_size == (uint32_t) (__mk_state_end - __mk_state_start) && h.page == PAGE;
    fclose(f);
    return ok;
}

/* 1 loaded; 0 no file; -1 not loaded (another build, a damaged file): the game is untouched */
static int load_state(const char *path) {
    const double t0 = port_now_ms();
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    char *const base = __mk_state_start;
    const size_t npages = (size_t) (__mk_state_end - __mk_state_start) / PAGE, nbm = (npages + 7) / 8;
    StateHeader h;
    StateExtra x;
    uint8_t *bm = calloc(nbm, 1), *raw = NULL, *cz = NULL;
    const char *why = NULL;
    if (!bm)
        why = "sem memoria";
    else if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, "MK64STAT", 8) || h.version != 1)
        why = "nao e um estado";
    else if (h.code_hash != code_hash() || h.region_start != (uint32_t) (uintptr_t) base ||
             h.region_size != npages * PAGE || h.page != PAGE || h.extra_bytes != sizeof(StateExtra))
        why = "de outra versao do jogo";
    else if (fread(&x, sizeof(x), 1, f) != 1 || fread(bm, 1, nbm, f) != nbm)
        why = "cortado";
    if (!why) {
        size_t pages = 0;
        for (size_t p = 0; p < npages; p++)
            pages += (bm[p >> 3] >> (p & 7)) & 1;
        const long pos = ftell(f);
        fseek(f, 0, SEEK_END);
        if (pages * PAGE != h.raw_bytes || ftell(f) != pos + (long) h.file_bytes)
            why = "cortado";
        fseek(f, pos, SEEK_SET);
    }
    if (!why && h.zlib && !zd_available())
        why = "comprimido, e sem zlib";
    if (!why && !(raw = malloc(h.raw_bytes ? h.raw_bytes : 1)))
        why = "sem memoria";
    if (!why) {
        if (h.zlib) {
            cz = malloc(h.file_bytes ? h.file_bytes : 1);
            zd_stream z;
            if (!cz || fread(cz, 1, h.file_bytes, f) != h.file_bytes)
                why = cz ? "erro de leitura" : "sem memoria";
            else if (zd_inflate_init(&z) != ZD_OK)
                why = "zlib";
            else {
                z.next_in = cz;
                z.avail_in = h.file_bytes;
                z.next_out = raw;
                z.avail_out = h.raw_bytes;
                const int r = zd_inflate(&z, ZD_FINISH);
                if (r != ZD_STREAM_END || z.total_out != h.raw_bytes)
                    why = "dados danificados";
                zd_inflate_end(&z);
            }
        } else if (fread(raw, 1, h.raw_bytes, f) != h.raw_bytes) {
            why = "erro de leitura";
        }
    }
    fclose(f);
    free(cz);
    const double t_read = port_now_ms();
    if (why) {
        slog("estado: %s nao carregado (%s)\n", path, why);
        free(bm);
        free(raw);
        return -1;
    }

    /* from here on the game's memory changes */
    const uint8_t *src = raw;
    for (size_t p = 0; p < npages; p++) {
        uint32_t *pg = (uint32_t *) (base + p * PAGE);
        if (bm[p >> 3] & (1u << (p & 7))) {
            memcpy(pg, src, PAGE);
            src += PAGE;
        } else if (!page_zero(pg)) {   /* (a page never touched stays the kernel's zero page) */
            memset(pg, 0, PAGE);
        }
    }
    free(bm);
    free(raw);
    gfx_mirror_state_set(x.mirror);
    gfx_pvr_request_cache_flush();   /* same addresses, other textures */
    port_saves_reapply();            /* the progress is the EEPROM file's, not the state's */
    slog("estado: carregado %s (salvo no quadro %u): leitura %.0f ms, copia %.0f ms\n", path, h.frames, t_read - t0,
           port_now_ms() - t_read);
    return 1;
}

/* ---- OnionOS ------------------------------------------------------------------------------ */

static void put_back_cmd(void) {
    if (sCmdLen > 0 && !exists(CMD_TO_RUN)) {
        const int ok = write_file(CMD_TO_RUN, sCmd, (size_t) sCmdLen);
        slog("estado: cmd_to_run.sh devolvido (%s)\n", ok ? "ok" : "FALHOU");
    }
}

static void make_checkoff_script(const char *argv0) {
    const char *exe = strrchr(argv0, '/');
    exe = exe ? exe + 1 : argv0;
    mkdir(CHECKOFF_DIR, 0777);
    char s[512];
    const int n = snprintf(s, sizeof(s),
                           "#!/bin/sh\n"
                           "# Mario Kart 64: OnionOS waits here (20 s at most) while the game saves at power-off\n"
                           "i=0\n"
                           "while grep -q '%s' /proc/%d/cmdline 2>/dev/null && [ $i -lt 200 ]; do sleep 0.1; i=$((i+1)); done\n",
                           exe, (int) getpid());
    sCheckoff = write_file(CHECKOFF_SCRIPT, s, (size_t) n);
}

/* (async-signal-safe: port_quit and the crash handler) */
void port_state_release(void) {
    if (sCheckoff)
        unlink(CHECKOFF_SCRIPT);
}

void port_state_disable(const char *why) {
    sDisabled = 1;
    snprintf(sDisabledWhy, sizeof(sDisabledWhy), "%s", why);
    slog("estado: save states desligados (%s)\n", why);
}

void port_state_init(const char *argv0) {
    sOnion = exists(ONION_DIR);
    mkdir(SAVE_DIR, 0777);
    remove(AUTO_STATE ".tmp");
    remove(QUICK_STATE ".tmp");
    sRomDataHash = hash_words(__rom_data_start, __rom_data_end);
    slog("estado: memoria do jogo %u KB em %p; fora do estado: dados da ROM %u KB, ROM de som %u KB\n",
           (unsigned) ((__mk_state_end - __mk_state_start) / 1024), (void *) __mk_state_start,
           (unsigned) ((__rom_data_end - __rom_data_start) / 1024), (unsigned) ((__rom_bss_end - __rom_bss_start) / 1024));
    if (sOnion) {
        FILE *f = fopen(CMD_TO_RUN, "rb");
        if (f) {
            const size_t n = fread(sCmd, 1, sizeof(sCmd) - 1, f);
            if (n > 0 && n < sizeof(sCmd) - 1)
                sCmdLen = (int) n;
            fclose(f);
        }
        sOffAtStart = exists("/tmp/.offOrder");
        make_checkoff_script(argv0);
        find_romscreen();
        read_menu_settings();
    }

    /* where the game starts */
    char prev[32] = "";
    FILE *f = fopen("/tmp/prev_state", "r");
    if (f) {
        if (fgets(prev, sizeof(prev), f))
            prev[strcspn(prev, "\r\n")] = 0;
        fclose(f);
    }
    if (exists(LOADING_FLAG)) {
        slog("estado: a ultima retomada nao chegou a rodar (travou?): comecando do inicio\n");
        remove(LOADING_FLAG);
        remove(BOOT_RESUME);
        return;
    }
    const int boot = exists(BOOT_RESUME);
    const int switcher = !strcmp(prev, "switcher") || !strcmp(prev, "game");
    if ((boot || switcher) && !sDisabled) {
        if (state_usable(AUTO_STATE)) {
            sBootLoadPending = 1;
        } else if (exists(AUTO_STATE)) {
            slog("estado: %s e de outra versao do jogo: apagado\n", AUTO_STATE);
            remove(AUTO_STATE);
        }
    }
    if (!sBootLoadPending)
        remove(BOOT_RESUME);
    slog("estado: aberto por '%s'%s: %s\n", prev[0] ? prev : "?", boot ? " (desligou no jogo)" : "",
           sBootLoadPending ? "continuando de onde parou" : "comecando do inicio");
}

/* (from the SIGTERM handler) 1 = the game saves and closes at the next frame */
int port_state_take_sigterm(void) {
    if (!sLoopRunning)
        return 0;
    sOffSignal = 1;
    return 1;
}

void port_state_request(int what) {
    if (what >= 0 && what < 2)
        sRequest[what] = 1;
}

void port_state_menu(int press) {
    if (press >= 0 && press < 3)
        sMenuPress = press;
}

int port_state_menu_double_used(void) {
    return sOnion && sMenuAction[PORT_MENU_DOUBLE] != MENU_OFF;
}

static void exit_saving(int kind) {
    static const char *names[] = { "fechar", "Game Switcher", "desligar", "bateria baixa", "jogo anterior" };
    const int off = kind == EXIT_POWEROFF || kind == EXIT_BATTERY;
    slog("estado: saindo (%s)\n", names[kind]);
    if (sBootLoadPending) {
        /* the resume has not happened yet: the state on the card is still the one to go on from */
        if (off) {
            touch(BOOT_RESUME);
            put_back_cmd();
        }
        port_quit(0);
    }
    if (sOnion)
        write_romscreen();
    const int ok = save_state(AUTO_STATE);
    if (ok)
        remove(LOADING_FLAG);
    if (off) {
        if (ok)
            touch(BOOT_RESUME);
        put_back_cmd();
    }
    if (kind == EXIT_SWITCHER)
        touch(RUN_SWITCHER);
    if (kind == EXIT_QUICK_SWITCH && !quick_switch_prepare())
        slog("estado: nenhum jogo anterior na lista: de volta ao menu\n");
    port_quit(0);
}

/* once a frame, between frames (src/main.c, through port_poll_quit) */
void port_state_boundary(void) {
    sFrames++;
    sLoopRunning = 1;
    if (!sRomBssHashed) {   /* the sound "ROM" is in by now (setup_audio_data) */
        sRomBssHash = hash_words(__rom_bss_start, __rom_bss_end);
        sRomBssHashed = 1;
    }

    if (sBootLoadPending && sFrames >= BOOT_LOAD_FRAME) {
        touch(LOADING_FLAG);
        remove(BOOT_RESUME);
        sBootLoadPending = 0;
        if (load_state(AUTO_STATE) == 1)
            sLoadedAt = sFrames;
        else
            remove(LOADING_FLAG);
    }
    if (sLoadedAt && sFrames - sLoadedAt >= LOADED_OK_FRAMES) {
        remove(LOADING_FLAG);
        sLoadedAt = 0;
    }

    /* power-off: the signal, or OnionOS's file (an old one, there before the game, is ignored) */
    int off = sOffSignal;
    if (!off && sOnion && !sOffAtStart) {
        const double now = port_now_ms();
        if (now >= sNextOffPoll) {
            sNextOffPoll = now + 250.0;
            off = exists("/tmp/.offOrder");
        }
    }
    if (off)
        exit_saving(EXIT_POWEROFF);
    if (port_battery_exit)
        exit_saving(EXIT_BATTERY);

    if (sRequest[PORT_REQ_SAVE]) {
        sRequest[PORT_REQ_SAVE] = 0;
        osd_show(save_state(QUICK_STATE) ? "State saved" : "Save failed");
    }
    if (sRequest[PORT_REQ_LOAD]) {
        sRequest[PORT_REQ_LOAD] = 0;
        const int r = load_state(QUICK_STATE);
        osd_show(r == 1 ? "State loaded" : r == 0 ? "No saved state" : "Load failed");
    }
    if (sMenuPress >= 0) {
        const int press = sMenuPress;
        sMenuPress = -1;
        if (!sOnion) {   /* (outside OnionOS: MENU closes the game, as before) */
            if (press == PORT_MENU_SINGLE)
                exit_saving(EXIT_QUIT);
        } else {
            switch (sMenuAction[press]) {
                case MENU_SWITCHER: exit_saving(EXIT_SWITCHER); break;
                case MENU_EXIT: exit_saving(EXIT_QUIT); break;
                case MENU_QUICK_SWITCH: exit_saving(EXIT_QUICK_SWITCH); break;
                default: break;   /* (off, or RetroArch's own menu: nothing here) */
            }
        }
    }
    if (port_input_quit_requested())
        exit_saving(EXIT_QUIT);
}
