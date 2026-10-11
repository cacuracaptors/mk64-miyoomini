/*
 * romrecipe -- PC tool (runs in WSL) for the release: writes the recipe that
 * rebuilds this port's game data out of the player's ROM, checks it, and finds
 * game data inside the executable. See romkit.h for the recipe format.
 *
 *   romrecipe make  ROM recipe.bin name=file [name=file ...]
 *       the recipe for these files (stored under these names); prints how much of
 *       each comes from the ROM and the largest pieces that do not
 *   romrecipe apply ROM recipe.bin outdir
 *       rebuilds the files into outdir and checks them against the recipe's CRCs
 *   romrecipe scan  ROM sections.bin
 *       how much of each section (written by romdata.py) is ROM data
 *
 * Build: gcc -O2 -o romrecipe romrecipe.c romkit.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "romkit.h"

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t) n : 1);
    if (b && fread(b, 1, (size_t) n, f) != (size_t) n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *size = (size_t) n;
    return b;
}

static uint8_t *pool;
static uint32_t pool_size;
static uint8_t *rom;
static size_t rom_size;

static int load_rom(const char *path) {
    rom = read_file(path, &rom_size);
    if (!rom) {
        fprintf(stderr, "nao consegui ler a ROM %s\n", path);
        return 0;
    }
    if (!rk_normalize_rom(rom, rom_size)) {
        fprintf(stderr, "%s nao parece uma ROM de N64\n", path);
        return 0;
    }
    size_t ps;
    pool = rk_build_pool(rom, rom_size, &ps, NULL);
    if (!pool)
        return 0;
    pool_size = (uint32_t) ps;
    fprintf(stderr, "ROM: %zu bytes, crc %08x; reserva (ROM + blocos MIO0 abertos): %u bytes\n", rom_size,
            rk_crc32(0, rom, rom_size), pool_size);
    return 1;
}

/* ---- index of the pool: 16-byte windows at every 4th byte, in the three views ---- */
#define HBITS 24
#define WIN 16
static uint32_t *head, *ent, *nxt;
/* 8-byte windows too: data mixing byte orders inside each record (vertices: 12 bytes of
   halfwords, then 4 single bytes) has no 16 bytes in one order */
#define WIN8 8
static uint32_t *head8, *ent8, *nxt8;

static uint32_t hash8(const uint8_t *b) {
    uint64_t x = 0;
    for (int k = 0; k < 8; k++) x = x << 8 | b[k];
    return (uint32_t) ((x * 0x9E3779B97F4A7C15ull) >> (64 - HBITS));
}

static uint32_t hash_bytes(const uint8_t *b) {
    uint64_t x = 0, y = 0;
    for (int k = 0; k < 8; k++) x = x << 8 | b[k];
    for (int k = 8; k < 16; k++) y = y << 8 | b[k];
    return (uint32_t) ((x * 0x9E3779B97F4A7C15ull ^ y * 0xC2B2AE3D27D4EB4Full) >> (64 - HBITS));
}

static void build_index(void) {
    const uint32_t per = pool_size / 4;
    head = calloc((size_t) 1 << HBITS, 4);
    ent = malloc((size_t) per * 3 * 4);
    nxt = malloc((size_t) per * 3 * 4);
    uint32_t n = 0;
    for (int v = 0; v < 3; v++)
        for (uint32_t p = 0; p + WIN <= pool_size; p += 4) {
            uint8_t w[WIN];
            int same = 1;
            for (int k = 0; k < WIN; k++) {
                w[k] = rk_view(pool, p + k, v);
                same &= w[k] == w[0];
            }
            if (same)
                continue;   /* (runs of one value: left out, they would make endless chains) */
            const uint32_t h = hash_bytes(w);
            ent[n] = (uint32_t) v << 30 | p;
            nxt[n] = head[h];
            head[h] = n + 1;
            n++;
        }
    head8 = calloc((size_t) 1 << HBITS, 4);
    ent8 = malloc((size_t) per * 3 * 4);
    nxt8 = malloc((size_t) per * 3 * 4);
    uint32_t n8 = 0;
    for (int v = 0; v < 3; v++)
        for (uint32_t p = 0; p + WIN8 <= pool_size; p += 4) {
            uint8_t w[WIN8];
            int same = 1;
            for (int k = 0; k < WIN8; k++) {
                w[k] = rk_view(pool, p + k, v);
                same &= w[k] == w[0];
            }
            if (same)
                continue;
            const uint32_t h = hash8(w);
            ent8[n8] = (uint32_t) v << 30 | p;
            nxt8[n8] = head8[h];
            head8[h] = n8 + 1;
            n8++;
        }
    fprintf(stderr, "indice: %u + %u posicoes\n", n, n8);
}

/* bytes of t[i..i+64) that a copy from pool position p reproduces, switching view
   wherever another one carries on */
static uint32_t follow_score(const uint8_t *t, uint32_t i, uint32_t n, uint32_t p) {
    uint32_t got = 0, k = 0;
    while (k < 64 && i + k < n && p + k < pool_size) {
        int ok = 0;
        for (int v = 0; v < 3 && !ok; v++)
            ok = t[i + k] == rk_view(pool, p + k, v);
        got += ok;
        k++;
    }
    return got;
}

static uint32_t match_len(const uint8_t *t, uint32_t i, uint32_t n, int v, uint32_t p) {
    uint32_t k = 0;
    while (i + k < n && p + k < pool_size && t[i + k] == rk_view(pool, p + k, v))
        k++;
    return k;
}

/* ---- the recipe of one file ---- */
struct Plan;
typedef struct {
    uint8_t kind;          /* 0 literal, 1..3 copy (view + 1), 4 MIO0 of sub, 5 run of one byte */
    uint32_t a, len;       /* copy: pool offset, length; MIO0: raw length, stream length */
    const uint8_t *lit;    /* literal bytes */
    struct Plan *sub;      /* MIO0: the ops making the raw data */
} Op;
typedef struct Plan { Op *ops; uint32_t n, cap; uint64_t lit; } Plan;

static void add_op(Plan *pl, Op op) {
    if (pl->n) {
        Op *o = &pl->ops[pl->n - 1];
        if (op.kind == 0 && o->kind == 0 && o->lit + o->len == op.lit) { o->len += op.len; pl->lit += op.len; return; }
        if (op.kind >= 1 && op.kind <= 3 && o->kind == op.kind && o->a + o->len == op.a) { o->len += op.len; return; }
    }
    if (pl->n == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 1024;
        pl->ops = realloc(pl->ops, pl->cap * sizeof(Op));
    }
    pl->ops[pl->n++] = op;
    if (op.kind == 0)
        pl->lit += op.len;
    if (op.kind == 4)
        pl->lit += op.sub->lit;
}

static void lit_op(Plan *pl, const uint8_t *p, uint32_t len) { add_op(pl, (Op) { 0, 0, len, p, NULL }); }
static void copy_op(Plan *pl, int v, uint32_t at, uint32_t len) { add_op(pl, (Op) { (uint8_t) (v + 1), at, len, NULL, NULL }); }

/* t[0..n) from the pool, as long copies as possible; what is not found is literal */
static void plan_plain(Plan *pl, const uint8_t *t, uint32_t n) {
    uint32_t i = 0, lit_start = 0, lit_len = 0;
    int have_cur = 0;
    uint32_t cur_p = 0;
#define FLUSH_LIT() do { if (lit_len) lit_op(pl, t + lit_start, lit_len); lit_len = 0; } while (0)
    while (i < n) {
        /* 1. carry on from where the last copy ended (in any view) */
        uint32_t lc = 0;
        int vc = 0;
        if (have_cur)
            for (int v = 0; v < 3; v++) {
                const uint32_t l = match_len(t, i, n, v, cur_p);
                if (l > lc) { lc = l; vc = v; }
            }
        /* 2. a new place in the pool */
        uint32_t lh = 0, ph = 0, back = 0;
        int vh = 0;
        if (lc < 8 && i + WIN <= n) {
            const uint32_t h = hash_bytes(t + i);
            int tries = 0;
            for (uint32_t e = head[h]; e && tries < 48; e = nxt[e - 1], tries++) {
                const int v = (int) (ent[e - 1] >> 30);
                const uint32_t p = ent[e - 1] & 0x3FFFFFFFu;
                const uint32_t l = match_len(t, i, n, v, p);
                if (l < WIN)
                    continue;
                uint32_t b = 0;
                while (b < lit_len && b < p && t[i - b - 1] == rk_view(pool, p - b - 1, v))
                    b++;
                if (l + b > lh + back) { lh = l; ph = p; vh = v; back = b; }
            }
        }
        if (lh < WIN && lc < 4 && i + WIN8 <= n) {
            const uint32_t h = hash8(t + i);
            uint32_t best = 0, bp = 0;
            int tries = 0;
            for (uint32_t e = head8[h]; e && tries < 48; e = nxt8[e - 1], tries++) {
                const int v = (int) (ent8[e - 1] >> 30);
                const uint32_t p = ent8[e - 1] & 0x3FFFFFFFu;
                if (match_len(t, i, n, v, p) < WIN8)
                    continue;
                const uint32_t sc = follow_score(t, i, n, p);
                if (sc > best) { best = sc; bp = p; }
            }
            if (best >= 48 && !(have_cur && bp == cur_p)) {   /* a place that keeps reproducing the data */
                have_cur = 1;
                cur_p = bp;
                continue;
            }
        }
        if (lh >= WIN && lh + back > lc) {
            lit_len -= back;
            FLUSH_LIT();
            copy_op(pl, vh, ph - back, lh + back);
            i += lh;
            have_cur = 1; cur_p = ph + lh;
        } else if (lc) {
            FLUSH_LIT();
            copy_op(pl, vc, cur_p, lc);
            i += lc;
            cur_p += lc;
        } else {
            if (!lit_len) lit_start = i;
            lit_len++;
            i++;
            if (have_cur) cur_p++;
        }
    }
    FLUSH_LIT();
#undef FLUSH_LIT
}

/* the same, but a MIO0 block that the build's compressor makes from data found in the
   pool is kept as "compress these ops": the compressed bytes themselves are not stored */
static uint32_t n_mio0, n_mio0_bytes;
static Plan plan_file(const uint8_t *t, uint32_t n) {
    Plan pl = { 0 };
    uint32_t seg = 0;
    for (uint32_t off = 0; off + 16 <= n; off += 4) {
        if (memcmp(t + off, "MIO0", 4))
            continue;
        uint8_t *raw = NULL;
        uint32_t rawlen = 0;
        const uint32_t slen = rk_mio0_decode(t + off, n - off, &raw, &rawlen);
        if (!slen || off + slen > n) { free(raw); continue; }
        uint8_t *enc = malloc((size_t) rawlen * 2 + 64);
        const uint32_t elen = rk_mio0_encode(raw, rawlen, enc);
        const int same = elen == slen && !memcmp(enc, t + off, slen);
        free(enc);
        if (!same) { free(raw); continue; }
        if (off > seg)
            plan_plain(&pl, t + seg, off - seg);
        Plan *sub = calloc(1, sizeof(Plan));
        plan_plain(sub, raw, rawlen);   /* (raw stays allocated: the literal ops point into it) */
        add_op(&pl, (Op) { 4, rawlen, slen, NULL, sub });
        n_mio0++;
        n_mio0_bytes += slen;
        seg = off + slen;
        off = ((seg + 3) & ~3u) - 4;
    }
    if (n > seg)
        plan_plain(&pl, t + seg, n - seg);
    return pl;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

/* literal bytes written as runs where one value repeats (fill, padding) */
static uint64_t fill_bytes;
static void write_literal(FILE *f, const uint8_t *p, uint32_t len, uint32_t *nops) {
    uint32_t i = 0, start = 0;
    while (i < len) {
        uint32_t r = 1;
        while (i + r < len && p[i + r] == p[i]) r++;
        if (r >= 16) {
            if (i > start) { fputc(0, f); put32(f, i - start); fwrite(p + start, 1, i - start, f); (*nops)++; }
            fputc(5, f); fputc(p[i], f); put32(f, r); (*nops)++;
            fill_bytes += r;
            i += r;
            start = i;
        } else {
            i += r;
        }
    }
    if (len > start) { fputc(0, f); put32(f, len - start); fwrite(p + start, 1, len - start, f); (*nops)++; }
}

/* writes the ops (the count first, fixed up afterwards); returns the op count */
static uint32_t write_plan(FILE *f, const Plan *pl) {
    const long at = ftell(f);
    put32(f, 0);
    uint32_t nops = 0;
    for (uint32_t k = 0; k < pl->n; k++) {
        const Op *o = &pl->ops[k];
        if (o->kind == 0) {
            write_literal(f, o->lit, o->len, &nops);
        } else if (o->kind == 4) {
            fputc(4, f);
            put32(f, o->a);
            write_plan(f, o->sub);
            nops++;
        } else {
            fputc(o->kind, f);
            put32(f, o->a);
            put32(f, o->len);
            nops++;
        }
    }
    const long end = ftell(f);
    fseek(f, at, SEEK_SET);
    put32(f, nops);
    fseek(f, end, SEEK_SET);
    return nops;
}

/* the largest literal pieces (not runs of one value), to see what they are */
static void show_literals(const Plan *pl, const uint8_t *base) {
    for (int shown = 0; shown < 3; shown++) {
        const Op *best = NULL;
        for (uint32_t k = 0; k < pl->n; k++) {
            const Op *o = &pl->ops[k];
            if (o->kind != 0 || o->len < 256)
                continue;
            int run = 1;
            for (uint32_t q = 1; q < o->len && run; q++) run = o->lit[q] == o->lit[0];
            if (run || (best && o->len <= best->len))
                continue;
            best = o;
        }
        if (!best)
            break;
        printf("      proprio: %u bytes%s:", best->len, base && best->lit >= base ? "" : " (dentro de um bloco MIO0)");
        if (base && best->lit >= base) printf(" posicao 0x%x:", (unsigned) (best->lit - base));
        for (uint32_t q = 0; q < 16 && q < best->len; q++)
            printf(" %02x", best->lit[q]);
        printf("\n");
        ((Op *) best)->kind = 6;   /* (shown: the next pass takes the next largest) */
    }
    for (uint32_t k = 0; k < pl->n; k++)
        if (pl->ops[k].kind == 6) pl->ops[k].kind = 0;
}

static int cmd_make(int argc, char **argv) {
    if (!load_rom(argv[2]))
        return 1;
    build_index();
    FILE *out = fopen(argv[3], "wb");
    if (!out) { perror(argv[3]); return 1; }
    const int nf = argc - 4;
    fwrite("MK64RCP1", 8, 1, out);
    put32(out, (uint32_t) rom_size);
    put32(out, rk_crc32(0, rom, rom_size));
    put32(out, pool_size);
    put32(out, (uint32_t) nf);
    uint64_t all = 0, all_lit = 0, all_fill = 0;
    for (int a = 4; a < argc; a++) {
        char name[64] = { 0 };
        const char *eq = strchr(argv[a], '=');
        if (!eq || eq - argv[a] >= 64) { fprintf(stderr, "use nome=arquivo: %s\n", argv[a]); return 1; }
        memcpy(name, argv[a], (size_t) (eq - argv[a]));
        size_t n;
        uint8_t *t = read_file(eq + 1, &n);
        if (!t) { fprintf(stderr, "nao li %s\n", eq + 1); return 1; }
        n_mio0 = n_mio0_bytes = 0;
        Plan pl = plan_file(t, (uint32_t) n);
        fwrite(name, 64, 1, out);
        put32(out, (uint32_t) n);
        put32(out, rk_crc32(0, t, n));
        const uint64_t f0 = fill_bytes;
        write_plan(out, &pl);
        const uint64_t fills = fill_bytes - f0, own = pl.lit - fills;
        printf("%-36s %9zu bytes  da ROM %6.2f%%  proprios %7llu  (+ %llu de preenchimento)%s\n", name, n,
               n ? 100.0 * (double) (n - pl.lit) / (double) n : 100.0, (unsigned long long) own,
               (unsigned long long) fills, n_mio0 ? "  [MIO0 recomprimido]" : "");
        show_literals(&pl, t);
        all += n;
        all_lit += own;
        all_fill += fills;
    }
    long rsize = ftell(out);
    fclose(out);
    printf("TOTAL %llu bytes: proprios %llu, preenchimento %llu; receita %ld bytes\n", (unsigned long long) all,
           (unsigned long long) all_lit, (unsigned long long) all_fill, rsize);
    return 0;
}

static uint32_t get32(const uint8_t **p) { uint32_t v; memcpy(&v, *p, 4); *p += 4; return v; }

static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *s = tmp + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(tmp, 0777); *s = '/'; }
}

static int cmd_apply(char **argv) {
    if (!load_rom(argv[2]))
        return 1;
    size_t rn;
    const uint8_t *r = read_file(argv[3], &rn);
    rk_recipe rc;
    if (!r || !rk_recipe_open(&rc, r, rn)) { fprintf(stderr, "receita invalida\n"); return 1; }
    if (rc.rom_size != rom_size || rc.rom_crc != rk_crc32(0, rom, rom_size) || rc.pool_size != pool_size) {
        fprintf(stderr, "esta ROM nao e a da receita\n");
        return 1;
    }
    int bad = 0;
    rk_file f;
    for (uint32_t k = 0; k < rc.nfiles && rk_recipe_next(&rc, &f); k++) {
        uint8_t *o = malloc(f.size ? f.size : 1);
        const int ok = rk_recipe_build(&rc, &f, pool, pool_size, o, NULL);
        char path[512];
        snprintf(path, sizeof path, "%s/%s", argv[4], f.name);
        mkdirs(path);
        FILE *w = fopen(path, "wb");
        if (w) { fwrite(o, 1, f.size, w); fclose(w); }
        printf("%-36s %s\n", f.name, ok ? "ok" : "ERRADO");
        bad |= !ok;
        free(o);
        if (!ok) break;
    }
    return bad;
}

static int cmd_scan(char **argv) {
    if (!load_rom(argv[2]))
        return 1;
    build_index();
    size_t sn;
    const uint8_t *s = read_file(argv[3], &sn), *p = s;
    if (!s) { fprintf(stderr, "nao li %s\n", argv[3]); return 1; }
    uint64_t total_rom = 0;
    while (p + 8 <= s + sn) {
        const uint32_t nl = get32(&p);
        char name[256] = { 0 };
        memcpy(name, p, nl < 255 ? nl : 255); p += nl;
        const uint32_t size = get32(&p);
        Plan pl = plan_file(p, size);
        const uint32_t from_rom = size - (uint32_t) pl.lit;
        if (from_rom >= 512 && from_rom * 4 >= size) {
            printf("%9u bytes, %6.2f%% da ROM: %s\n", size, 100.0 * from_rom / size, name);
            total_rom += from_rom;
        }
        free(pl.ops);
        p += size;
    }
    printf("TOTAL de dados da ROM nas secoes acima: %llu bytes\n", (unsigned long long) total_rom);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1], "make")) return cmd_make(argc, argv);
    if (argc == 5 && !strcmp(argv[1], "apply")) return cmd_apply(argv);
    if (argc == 4 && !strcmp(argv[1], "scan")) return cmd_scan(argv);
    fprintf(stderr, "uso: romrecipe make ROM receita nome=arquivo...\n"
                    "     romrecipe apply ROM receita pasta\n"
                    "     romrecipe scan ROM secoes.bin\n");
    return 2;
}
