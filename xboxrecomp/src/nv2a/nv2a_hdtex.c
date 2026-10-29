/* HD texture replacement -- see nv2a_hdtex.h.
 *
 * Runs on the pump thread only (texture uploads), so no locking. Memory: the
 * decoded file lives only while its texture is built; the texture itself is
 * immutable and keeps no system-memory copy (d3d8_CreateTextureFromLevels).
 * The texture cache in nv2a_pgraph_d3d11.c bounds the GPU memory. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_hdtex.h"

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"     /* BC4/5/6H: not used here */
#include "bcdec/bcdec.h"
#pragma GCC diagnostic pop

typedef struct {
    uint64_t key;
    uint16_t w, h;
    uint16_t kr, ka;            /* colour factors in hundredths: RGB x kr/100, alpha x ka/100 */
    uint8_t  menu;              /* a menu / interface picture */
    uint8_t  hud;               /* a race HUD atlas */
    uint8_t  font;              /* a font rebuilt glyph by glyph (glyph lines) */
    uint8_t  soft;              /* a font: edge softening (fontsoft line), in passes at x2 */
    uint8_t  shipped;           /* a file of the HD HUD the game ships (g_hud_dir, below) */
    const char *file;
} HdEnt;

/* A glyph of a font: its place in the game's atlas (texels) and in the
 * HD atlas (pixels of its HD file). */
typedef struct {
    uint64_t key;
    uint16_t gx, gy, gw, gh, hx, hy;
} HdGlyph;

static HdGlyph *g_glyph;
static int      g_nglyph;

/* fontsoft lines: "fontsoft <font key> <n>" -- the font's coverage is softened
 * by n passes of a [1 2 1] blur at the x2 level (4 n at x4), so that its thin
 * strokes and the black shadow the game draws under them keep the grey, soft
 * look of the game's own 4-bit font at the screen's scale.
 * XBOX_HD_FONT_SOFT=<n> replaces n for the fonts that have a line (0 = off). */
static struct { uint64_t key; int n; } g_soft[8];
static int g_nsoft;

static HdEnt   *g_ent;
static int      g_n;
static char    *g_text;         /* the index's own copy, file names point into it */
static const char *g_builtin;
static size_t   g_builtin_len;
static char     g_dir[MAX_PATH];
static char     g_hud_dir[MAX_PATH];   /* XBOX_HD_HUD: the shipped HD HUD's folder, "" = not used */
static int      g_pack;         /* XBOX_HD_TEXTURES names a pack folder */
static int      g_state = -1;   /* -1 not read yet, 0 off, 1 on */
static int      g_log, g_menus, g_hud, g_sync, g_workers;
static int      g_hud_half;     /* a pack's HUD atlases and fonts at x2, half their own size, unless XBOX_HD_HUD_SCALE=4 */
static int      g_hud_only;     /* XBOX_HD_TEXTURES_HUD=only: the race HUD and its font, nothing else */
static int      g_frame_gate = -1; /* the image's gate bits (HDTEX_GATE_*), -1 = no gate: all HD */

int hdtex_configured(void)
{
    const char *p = getenv("XBOX_HD_TEXTURES"), *h = getenv("XBOX_HD_HUD");
    return (p && p[0] && strcmp(p, "0")) || (h && h[0] && strcmp(h, "0"));
}
void hdtex_set_frame_gate(int on) { g_frame_gate = on ? 1 << HDTEX_GATE_HUD : -1; }
void hdtex_frame_gate(unsigned bits) { if (g_frame_gate >= 0) g_frame_gate = (int)(bits & 3u); }
int hdtex_gate_now(int kind) { return g_frame_gate < 0 || (g_frame_gate >> kind & 1); }
static uint64_t g_budget;

/* Statistics, for the [HDTEX] lines (pump thread, except the worker times). */
static unsigned g_hits, g_misses, g_fail, g_evicted, g_evicted_budget, g_cancelled;
static uint64_t g_bytes_made, g_live, g_live_max;   /* GPU bytes made, alive, most alive */
static double   g_ms_total, g_ms_max;               /* work per texture (a worker's time) */
static double   g_wait_total, g_wait_max;           /* request -> in use (pump's view) */

void hdtex_set_builtin_index(const char *text, size_t len) { g_builtin = text; g_builtin_len = len; }
uint64_t hdtex_budget(void) { return g_budget; }

void hdtex_note_evicted(uint32_t bytes, int over_budget)
{
    g_live -= bytes;
    g_evicted++;
    if (over_budget) g_evicted_budget++;
}

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static uint64_t fnv64(const uint8_t *p, uint32_t n)
{
    /* FNV-1a over 8-byte words, then the tail bytes (the index tool does the same) */
    uint64_t h = 0xcbf29ce484222325ull, v;
    uint32_t i;
    for (i = 0; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); h = (h ^ v) * 0x100000001b3ull; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint64_t hdtex_key(const uint8_t *level0, uint32_t n0) { return fnv64(level0, n0); }

static int ent_cmp(const void *a, const void *b)
{
    uint64_t x = ((const HdEnt *)a)->key, y = ((const HdEnt *)b)->key;
    return x < y ? -1 : x > y;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < (256l << 20) &&
        fseek(f, 0, SEEK_SET) == 0 && (buf = (char *)malloc((size_t)n + 1)) != NULL) {
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
        else { buf[n] = '\0'; if (len) *len = (size_t)n; }
    }
    fclose(f);
    return buf;
}

static int file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* One colour factor, "2" or "1.28" (two decimals at most), in hundredths; 0 = not understood. */
static unsigned parse_factor(const char *s, const char **end)
{
    unsigned v = 0, frac = 0, digits = 0;
    if (*s < '0' || *s > '9') return 0;
    for (; *s >= '0' && *s <= '9'; s++)
        if ((v = v * 10 + (unsigned)(*s - '0')) > 9) return 0;
    if (*s == '.') {
        for (s++; *s >= '0' && *s <= '9'; s++) {
            if (digits == 2) return 0;
            frac = frac * 10 + (unsigned)(*s - '0');
            digits++;
        }
        if (!digits) return 0;
        if (digits == 1) frac *= 10;
    }
    *end = s;
    return v * 100 + frac;
}

/* The factor field: "<rgb><alpha>", one digit each, 1 or 2 ("22", "12"), or
 * "<rgb>:<alpha>" with decimals ("1.28:2"), between 0.5 and 4. */
static int parse_factors(const char *s, uint16_t *kr, uint16_t *ka)
{
    unsigned r, a;
    const char *e;
    if (strlen(s) == 2 && s[0] >= '1' && s[0] <= '2' && s[1] >= '1' && s[1] <= '2') {
        *kr = (uint16_t)((s[0] - '0') * 100);
        *ka = (uint16_t)((s[1] - '0') * 100);
        return 1;
    }
    if (!(r = parse_factor(s, &e)) || *e != ':' || !(a = parse_factor(e + 1, &e)) || *e ||
        r < 50 || r > 400 || a < 50 || a > 400)
        return 0;
    *kr = (uint16_t)r;
    *ka = (uint16_t)a;
    return 1;
}

static int g_cap;

static int add_ent(const HdEnt *e)
{
    if (g_n == g_cap) {
        int c = g_cap ? g_cap * 2 : 2048;
        HdEnt *grown = (HdEnt *)realloc(g_ent, (size_t)c * sizeof *g_ent);
        if (!grown) return 0;
        g_ent = grown;
        g_cap = c;
    }
    g_ent[g_n++] = *e;
    return 1;
}

/* Parse "<key> <w>x<h> <factors> <3d|menu|hud> <file>" lines in place; '#' = comment.
 * Without a pack (the shipped HD HUD alone), the glyph and fontsoft lines only. */
static void parse_index(char *text)
{
    int skipped = 0;
    char *line = text, *next;
    for (; line && *line; line = next) {
        unsigned long long k;
        unsigned w, h;
        char fac[16], cat[8];
        int used = 0;
        HdEnt e;
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (line[0] == '#' || line[0] == '\r' || !line[0]) continue;
        if (!strncmp(line, "fontsoft ", 9)) {
            unsigned n;
            if (sscanf(line + 9, "%llx %u", &k, &n) != 2 || n > 8 || g_nsoft == 8) { skipped++; continue; }
            g_soft[g_nsoft].key = k;
            g_soft[g_nsoft].n = (int)n;
            g_nsoft++;
            continue;
        }
        if (!strncmp(line, "glyph ", 6)) {
            unsigned v[6];
            if (sscanf(line + 6, "%llx %u %u %u %u %u %u", &k, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 7 ||
                !v[2] || !v[3] || v[0] + v[2] > 4096 || v[1] + v[3] > 4096 || v[4] > 8192 || v[5] > 8192) {
                skipped++;
                continue;
            }
            if (g_nglyph % 256 == 0) {
                HdGlyph *grown = (HdGlyph *)realloc(g_glyph, (size_t)(g_nglyph + 256) * sizeof *g_glyph);
                if (!grown) continue;
                g_glyph = grown;
            }
            g_glyph[g_nglyph].key = k;
            g_glyph[g_nglyph].gx = (uint16_t)v[0]; g_glyph[g_nglyph].gy = (uint16_t)v[1];
            g_glyph[g_nglyph].gw = (uint16_t)v[2]; g_glyph[g_nglyph].gh = (uint16_t)v[3];
            g_glyph[g_nglyph].hx = (uint16_t)v[4]; g_glyph[g_nglyph].hy = (uint16_t)v[5];
            g_nglyph++;
            continue;
        }
        if (sscanf(line, "%llx %ux%u %15s %7s %n", &k, &w, &h, fac, cat, &used) != 5 || !used ||
            w == 0 || h == 0 || w > 4096 || h > 4096 || !parse_factors(fac, &e.kr, &e.ka)) {
            skipped++;
            continue;
        }
        {
            char *name = line + used, *end = name + strlen(name);
            while (end > name && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
            if (!*name) { skipped++; continue; }
            e.file = name;
        }
        e.key = k;
        e.w = (uint16_t)w;
        e.h = (uint16_t)h;
        e.menu = strcmp(cat, "menu") == 0;
        e.hud = strcmp(cat, "hud") == 0;
        e.font = strcmp(cat, "font") == 0;
        e.soft = 0;
        e.shipped = 0;
        if (!g_pack) continue;
        if (e.menu && !g_menus) continue;
        if ((e.hud || e.font) && (!g_hud || g_hud_dir[0])) continue;   /* the shipped HUD has them */
        if (g_hud_only && !e.hud && !e.font) continue;
        if (!add_ent(&e)) break;
    }
    if (skipped) fprintf(stderr, "[HDTEX] %d index lines not understood\n", skipped);
}

/* Sorted by key; the fonts' softening (fontsoft lines) set. */
static void finish_index(void)
{
    qsort(g_ent, (size_t)g_n, sizeof *g_ent, ent_cmp);
    {
        const char *v = getenv("XBOX_HD_FONT_SOFT");
        int i;
        for (i = 0; i < g_nsoft; i++) {
            HdEnt key, *e;
            key.key = g_soft[i].key;
            e = g_n ? (HdEnt *)bsearch(&key, g_ent, (size_t)g_n, sizeof *g_ent, ent_cmp) : NULL;
            if (e && e->font)
                e->soft = (uint8_t)(v && v[0] >= '0' && v[0] <= '8' ? v[0] - '0' : g_soft[i].n);
        }
    }
}

/* The folder that holds the pack's .dds files: the one given, or one of the
 * folders the pack's archive puts above them. */
static int find_pack_dir(const char *given)
{
    static const char *const below[] = {
        "", "/replacements", "/SLUS-20326/replacements", "/SSX Tricky/SLUS-20326/replacements",
    };
    int i, j, probe = g_n < 8 ? g_n : 8;
    char path[MAX_PATH * 2];
    for (i = 0; i < (int)(sizeof below / sizeof below[0]); i++) {
        for (j = 0; j < probe; j++) {
            snprintf(path, sizeof path, "%s%s/%s", given, below[i], g_ent[(size_t)j * g_n / probe].file);
            if (file_exists(path)) {
                snprintf(g_dir, sizeof g_dir, "%s%s", given, below[i]);
                return 1;
            }
        }
    }
    return 0;
}


/* ── The shipped HD HUD ────────────────────────────────────────────────
 * The game ships an HD race HUD (the atlases map1 and map4 of hudgame.xsh /
 * hudtrick.xsh, the fonts title.ffn and menu.ffn) in a folder beside its
 * executable, hd-hud (XBOX_HD_HUD=1, or another folder): at x2, colours as
 * they are (no factor), 32-bit BGRA without compression. A font's file holds
 * its letters in the game's own layout (the index's glyph lines place them),
 * white, alpha = coverage. It is used with or without a pack; with a pack
 * too, the pack's own HUD files are not read. Each file is checked against
 * its FNV-1a 64 at start: a missing or different file leaves its pictures
 * the game's own (logged). Below: per game texture (key, size, font or
 * not), the file that replaces it and that file's FNV-1a 64. */
typedef struct { uint64_t key; uint16_t w, h; uint8_t font; const char *file; uint64_t fnv; } HudFile;
static const HudFile k_hud_files[] = {
    { 0x45fe446c0807cf2cull, 256, 256, 0, "map1.dds",  0xb272ce1d6ea2fa09ull },   /* swizzled, linear */
    { 0x822e3378d331eb34ull, 256, 256, 0, "map1.dds",  0xb272ce1d6ea2fa09ull },
    { 0x9a7403ebcdb5948eull, 256, 256, 0, "map4.dds",  0x1f766b66d1371a9dull },
    { 0x13d0d53887040446ull, 256, 256, 0, "map4.dds",  0x1f766b66d1371a9dull },
    { 0x198cecc8c4fcd9a5ull, 256, 256, 1, "title.dds", 0xf86d35c42d0cd6f1ull },
    { 0xe3c2303845c51fa5ull, 128, 128, 1, "menu.dds",  0x0f7651de3b7295b5ull },
};
#define N_HUD_FILES ((int)(sizeof k_hud_files / sizeof k_hud_files[0]))

/* Whether the shipped file `name` is there and is the one shipped. */
static int hud_file_ok(const char *name, uint64_t fnv, const char **why)
{
    char path[MAX_PATH * 2];
    size_t n = 0;
    char *d;
    snprintf(path, sizeof path, "%s/%s", g_hud_dir, name);
    if (!(d = read_file(path, &n))) { *why = "missing"; return 0; }
    if (fnv64((const uint8_t *)d, (uint32_t)n) != fnv) {
        *why = "not the shipped file (damaged or replaced)";
        free(d);
        return 0;
    }
    free(d);
    return 1;
}

/* The shipped HUD's entries whose file is right; returns the files found. */
static int add_shipped(int *files_total)
{
    const char *seen[N_HUD_FILES];
    int ok[N_HUD_FILES], nseen = 0, nok = 0, i, j;
    for (i = 0; i < N_HUD_FILES; i++) {
        const HudFile *f = &k_hud_files[i];
        int good = -1;
        HdEnt e;
        for (j = 0; j < nseen; j++)
            if (!strcmp(seen[j], f->file)) good = ok[j];
        if (good < 0) {
            const char *why = "";
            good = hud_file_ok(f->file, f->fnv, &why);
            if (!good)
                fprintf(stderr, "[HDTEX] HD HUD: %s/%s %s: its part of the HUD stays the game's own\n",
                        g_hud_dir, f->file, why);
            seen[nseen] = f->file;
            ok[nseen++] = good;
            nok += good;
        }
        if (!good) continue;
        if (f->font) {                    /* its letters' places: the glyph lines */
            for (j = 0; j < g_nglyph && g_glyph[j].key != f->key; j++) {}
            if (j == g_nglyph) {
                fprintf(stderr, "[HDTEX] HD HUD: no glyph lines for %s in the index: that font stays the game's own\n",
                        f->file);
                continue;
            }
        }
        memset(&e, 0, sizeof e);
        e.key = f->key;
        e.w = f->w;
        e.h = f->h;
        e.kr = e.ka = 100;
        e.hud = !f->font;
        e.font = f->font;
        e.shipped = 1;
        e.file = f->file;
        if (!add_ent(&e)) break;
    }
    *files_total = nseen;
    return nok;
}

static int start_workers(void);

/* XBOX_HD_HUD: "1" = hd-hud beside the executable, a folder, or "0" / empty = off. */
static int hud_dir_from(const char *v)
{
    if (!v || !v[0] || !strcmp(v, "0")) return 0;
    if (!strcmp(v, "1")) {
        char exe[MAX_PATH], *slash;
        DWORD k = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
        if (!k || k >= sizeof exe || !(slash = strrchr(exe, '\\')) || (size_t)(slash - exe) + 8 > sizeof g_hud_dir)
            return 0;
        memcpy(g_hud_dir, exe, (size_t)(slash - exe));
        memcpy(g_hud_dir + (slash - exe), "/hd-hud", 8);
    } else {
        snprintf(g_hud_dir, sizeof g_hud_dir, "%s", v);
    }
    return 1;
}

int hdtex_on(void)
{
    const char *dir, *v;
    char path[MAX_PATH * 2];
    const char *from = NULL;
    int hud_files = 0, hud_found = 0, pack_entries;
    if (g_state >= 0) return g_state;
    g_state = 0;
    dir = getenv("XBOX_HD_TEXTURES");
    g_pack = dir && dir[0] && strcmp(dir, "0");
    hud_dir_from(getenv("XBOX_HD_HUD"));
    if (!g_pack && !g_hud_dir[0]) return 0;
    v = getenv("XBOX_HD_LOG");             g_log = v ? atoi(v) : 0;      /* 1 hits, 2 misses too */
    v = getenv("XBOX_HD_TEXTURES_MENUS");  g_menus = v && v[0] == '1';
    v = getenv("XBOX_HD_TEXTURES_HUD");    g_hud = !(v && v[0] == '0');
    g_hud_only = v && (v[0] == 'o' || v[0] == 'O');     /* "only" */
    v = getenv("XBOX_HD_HUD_SCALE");       g_hud_half = !(v && v[0] == '4');   /* x2 unless 4 */
    v = getenv("XBOX_HD_TEXTURES_MB");
    g_budget = (uint64_t)((v && atoi(v) >= 64) ? atoi(v) : 1024) << 20;
    v = getenv("XBOX_HD_TEXTURES_SYNC");   g_sync = v && v[0] == '1';

    v = getenv("XBOX_HD_TEXTURES_INDEX");
    if (v && v[0]) {
        g_text = read_file(v, NULL);
        from = v;
    } else {
        if (g_pack) {
            snprintf(path, sizeof path, "%s/hdtex_index.txt", dir);
            if ((g_text = read_file(path, NULL)) != NULL) from = path;
        }
        if (!g_text && g_builtin && g_builtin_len && (g_text = (char *)malloc(g_builtin_len + 1)) != NULL) {
            memcpy(g_text, g_builtin, g_builtin_len);
            g_text[g_builtin_len] = '\0';
            from = "(built in)";
        }
    }
    if (!g_text) {
        fprintf(stderr, "[HDTEX] off: no index (%s)\n", v && v[0] ? v : "none built in");
        return 0;
    }
    parse_index(g_text);                 /* the pack's entries (none without a pack) */
    pack_entries = g_n;
    if (g_pack && g_n == 0) {
        fprintf(stderr, "[HDTEX] pack off: the index %s lists nothing for it\n", from);
        g_pack = 0;
    } else if (g_pack && !find_pack_dir(dir)) {
        fprintf(stderr, "[HDTEX] pack off: none of the index's files are in %s (the pack's folder, "
                        "or the folder above \"replacements\")\n", dir);
        g_pack = 0;
        g_n = pack_entries = 0;
    }
    if (g_hud_dir[0]) hud_found = add_shipped(&hud_files);
    if (g_n == 0) {
        fprintf(stderr, "[HDTEX] off: %s\n", g_hud_dir[0] ? "no pack, and the HD HUD's files are not usable"
                                                           : "no pack");
        return 0;
    }
    finish_index();
    if (!g_sync && !start_workers()) g_sync = 1;
    if (g_pack)
        fprintf(stderr, "[HDTEX] on: %d textures listed (index %s, menus %s, hud %s%s x%d, %d glyphs), pack %s, "
                        "budget %u MB, %s\n",
                pack_entries, from, g_menus ? "on" : "off",
                g_hud_dir[0] ? "shipped" : g_hud ? "on" : "off", g_hud_only ? " only" : "",
                g_hud_half ? 2 : 4, g_nglyph, g_dir,
                (unsigned)(g_budget >> 20),
                g_sync ? "loaded at first use (synchronous)" : "loaded by worker threads");
    if (g_hud_dir[0])
        fprintf(stderr, "[HDTEX] HD HUD: %d of %d files from %s (%d textures, x2)%s\n",
                hud_found, hud_files, g_hud_dir, g_n - pack_entries,
                g_pack ? "; the pack's own HUD files are not read" : "");
    g_state = 1;
    return 1;
}

/* ── DDS ───────────────────────────────────────────────────────────── */

enum { DDS_BC1 = 1, DDS_BC2, DDS_BC3, DDS_BC7, DDS_RGBA8, DDS_BGRA8 };

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Header of a DDS in memory: kind, size, levels present, offset of the data. */
static int dds_header(const uint8_t *d, size_t n, int *kind, uint32_t *w, uint32_t *h,
                      uint32_t *levels, size_t *data)
{
    uint32_t pf_flags, fourcc;
    if (n < 128 || memcmp(d, "DDS ", 4) || rd32(d + 4) != 124) return 0;
    *h = rd32(d + 12);
    *w = rd32(d + 16);
    *levels = rd32(d + 28);            /* some writers leave DDSD_MIPMAPCOUNT out */
    if (!*levels) *levels = 1;
    pf_flags = rd32(d + 80);
    fourcc = rd32(d + 84);
    *data = 128;
    *kind = 0;
    if ((pf_flags & 4u) && fourcc == 0x30315844u) {              /* "DX10" */
        if (n < 148) return 0;
        *data = 148;
        switch (rd32(d + 128)) {
        case 71: case 72: *kind = DDS_BC1; break;
        case 74: case 75: *kind = DDS_BC2; break;
        case 77: case 78: *kind = DDS_BC3; break;
        case 98: case 99: *kind = DDS_BC7; break;
        case 28: case 29: *kind = DDS_RGBA8; break;
        case 87: case 91: *kind = DDS_BGRA8; break;
        }
    } else if (pf_flags & 4u) {
        if (fourcc == 0x31545844u) *kind = DDS_BC1;              /* DXT1 */
        else if (fourcc == 0x33545844u) *kind = DDS_BC2;         /* DXT3 */
        else if (fourcc == 0x35545844u) *kind = DDS_BC3;         /* DXT5 */
    } else if ((pf_flags & 0x40u) && rd32(d + 88) == 32) {        /* uncompressed RGB, 32 bits */
        *kind = rd32(d + 92) == 0x00FF0000u ? DDS_BGRA8 : DDS_RGBA8;
    }
    return *kind && *w && *h && *w <= 8192 && *h <= 8192;
}

static size_t dds_level_bytes(int kind, uint32_t w, uint32_t h)
{
    size_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    switch (kind) {
    case DDS_BC1: return bw * bh * 8;
    case DDS_BC2: case DDS_BC3: case DDS_BC7: return bw * bh * 16;
    default: return (size_t)w * h * 4;
    }
}

/* One level to RGBA8 (top-down rows of w * 4 bytes). */
static void dds_decode(int kind, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *out)
{
    uint32_t bx, by, x, y;
    uint8_t blk[64];
    if (kind == DDS_RGBA8 || kind == DDS_BGRA8) {
        memcpy(out, src, (size_t)w * h * 4);
        if (kind == DDS_BGRA8)
            for (x = 0; x < w * h; x++) { uint8_t t = out[x * 4]; out[x * 4] = out[x * 4 + 2]; out[x * 4 + 2] = t; }
        return;
    }
    for (by = 0; by < (h + 3) / 4; by++)
        for (bx = 0; bx < (w + 3) / 4; bx++) {
            int full = bx * 4 + 4 <= w && by * 4 + 4 <= h;
            uint8_t *dst = full ? out + ((size_t)by * 4 * w + bx * 4) * 4 : blk;
            int pitch = full ? (int)(w * 4) : 16;
            switch (kind) {
            case DDS_BC1: bcdec_bc1(src, dst, pitch); src += 8; break;
            case DDS_BC2: bcdec_bc2(src, dst, pitch); src += 16; break;
            case DDS_BC3: bcdec_bc3(src, dst, pitch); src += 16; break;
            default:      bcdec_bc7(src, dst, pitch); src += 16; break;
            }
            if (!full)
                for (y = 0; y < 4 && by * 4 + y < h; y++)
                    for (x = 0; x < 4 && bx * 4 + x < w; x++)
                        memcpy(out + ((size_t)(by * 4 + y) * w + bx * 4 + x) * 4, blk + (y * 4 + x) * 4, 4);
        }
}

/* RGBA -> BGRA in place, colours times kr/100, alpha times ka/100 (rounded,
 * clamped; whole factors give exactly p * k). */
static void to_bgra_scaled(uint8_t *p, size_t texels, unsigned kr, unsigned ka)
{
    size_t i;
    for (i = 0; i < texels; i++, p += 4) {
        unsigned r = (p[0] * kr + 50) / 100, g = (p[1] * kr + 50) / 100, b = (p[2] * kr + 50) / 100,
                 a = (p[3] * ka + 50) / 100;
        p[0] = (uint8_t)(b > 255 ? 255 : b);
        p[1] = (uint8_t)(g > 255 ? 255 : g);
        p[2] = (uint8_t)(r > 255 ? 255 : r);
        p[3] = (uint8_t)(a > 255 ? 255 : a);
    }
}

/* Box-filtered half of a BGRA level (for an HD file without its full chain). */
static void half_level(const uint8_t *s, uint32_t w, uint32_t h, uint8_t *d)
{
    uint32_t W = w > 1 ? w / 2 : 1, H = h > 1 ? h / 2 : 1, x, y, c;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint32_t x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1;
            uint32_t y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
            for (c = 0; c < 4; c++)
                d[((size_t)y * W + x) * 4 + c] = (uint8_t)((s[((size_t)y0 * w + x0) * 4 + c] + s[((size_t)y0 * w + x1) * 4 + c] +
                                                         s[((size_t)y1 * w + x0) * 4 + c] + s[((size_t)y1 * w + x1) * 4 + c] + 2) / 4);
        }
}

/* Texel (x, y) of a swizzled w x h texture (powers of two): the bits of x and
 * y interleaved, x first, as long as each has bits left (the index tool's
 * morton order). */
static uint32_t swz_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t r = 0, ob = 0;
    for (; w > 1 || h > 1; w >>= 1, h >>= 1) {
        if (w > 1) { r |= (x & 1u) << ob++; x >>= 1; }
        if (h > 1) { r |= (y & 1u) << ob++; y >>= 1; }
    }
    return r;
}

/* The HUD atlases draw some pieces from strips one texel wide (the gauge's
 * rail, its graduations, single dots): an opaque texel whose two neighbours
 * across are fully transparent. The game samples such a strip at the centre
 * of its texel, where an HD drawing at x4 has the bright core of a blurred
 * line rather than the strip's own colour (the rail turns white, its cyan
 * goes). Those texels keep the game's colour: their s x s block of the HD
 * level 0 takes the original BGRA texel; everything else is the HD drawing.
 * `orig` is the game's level 0, linear BGRA, ow x oh. */
static void keep_thin_strips(uint8_t *hd, uint32_t W, uint32_t H, const uint8_t *orig, uint32_t ow, uint32_t oh)
{
    uint32_t s = W / ow, x, y, i, j;
    if (!ow || !oh || s < 2 || W != ow * s || H != oh * s) return;
    for (y = 0; y < oh; y++)
        for (x = 0; x < ow; x++) {
            const uint8_t *t = orig + ((size_t)y * ow + x) * 4;
#define ALPHA0(xx, yy) ((xx) >= ow || (yy) >= oh || orig[((size_t)(yy) * ow + (xx)) * 4 + 3] == 0)
            int across_x = ALPHA0(x - 1, y) && ALPHA0(x + 1, y);     /* x - 1 wraps above ow at 0 */
            int across_y = ALPHA0(x, y - 1) && ALPHA0(x, y + 1);
#undef ALPHA0
            if (!t[3] || !(across_x || across_y)) continue;
            for (j = 0; j < s; j++)
                for (i = 0; i < s; i++)
                    memcpy(hd + (((size_t)y * s + j) * W + (size_t)x * s + i) * 4, t, 4);
        }
}

/* The game's atlas `orig` (ow x oh, BGRA) enlarged s times into out
 * (ow s x oh s): bilinear, as the game would sample it. */
static void enlarge(const uint8_t *orig, uint32_t ow, uint32_t oh, uint32_t s, uint8_t *out)
{
    uint32_t W = ow * s, H = oh * s, x, y;
    for (y = 0; y < H; y++) {
        float fy = ((float)y + 0.5f) / (float)s - 0.5f;
        int y0 = (int)(fy < 0 ? -1 : fy), y1;
        float ty = fy - (float)y0;
        if (y0 < 0) { y0 = 0; ty = 0; }
        y1 = y0 + 1 < (int)oh ? y0 + 1 : y0;
        for (x = 0; x < W; x++) {
            float fx = ((float)x + 0.5f) / (float)s - 0.5f;
            int x0 = (int)(fx < 0 ? -1 : fx), x1, c;
            float tx = fx - (float)x0;
            if (x0 < 0) { x0 = 0; tx = 0; }
            x1 = x0 + 1 < (int)ow ? x0 + 1 : x0;
            for (c = 0; c < 4; c++) {
                float a = orig[((size_t)y0 * ow + x0) * 4 + c] * (1 - tx) + orig[((size_t)y0 * ow + x1) * 4 + c] * tx;
                float b = orig[((size_t)y1 * ow + x0) * 4 + c] * (1 - tx) + orig[((size_t)y1 * ow + x1) * 4 + c] * tx;
                out[((size_t)y * W + x) * 4 + c] = (uint8_t)(a * (1 - ty) + b * ty + 0.5f);
            }
        }
    }
}

static void half_level(const uint8_t *s, uint32_t w, uint32_t h, uint8_t *d);

/* A font's HD atlas in the game's layout. The pack has the same letters in
 * HD, laid out in another order: `hd` (W x H, its level 0, alpha already
 * scaled) becomes the game's atlas `orig` (ow x oh, BGRA) enlarged s times
 * (bilinear, as the game would sample it), then every glyph the index places
 * in the pack (glyph lines of this key) is copied from the pack's HD atlas to
 * its place in the game's layout: white, alpha = the pack's coverage, as the
 * game's own font (white, alpha = coverage). A glyph the index does not list
 * stays the game's own. Returns 0 when the sizes do not fit (no texture).
 *
 * A shipped font (`shipped`: the HD HUD's file, x2, its letters already in the
 * game's layout): the game's atlas is enlarged at x4 and halved (box), then
 * each glyph takes the file's letter at its own place (the same place as in
 * the game's atlas). */
static int font_compose(uint8_t *hd, uint32_t W, uint32_t H, const uint8_t *orig, uint32_t ow, uint32_t oh,
                        uint64_t key, int shipped)
{
    uint32_t s = ow ? W / ow : 0, x, y, i, n = 0;
    uint8_t *src, *big = NULL;
    if (s < 2 || W != ow * s || H != oh * s || !(src = (uint8_t *)malloc((size_t)W * H * 4))) return 0;
    memcpy(src, hd, (size_t)W * H * 4);
    if (!shipped) {
        enlarge(orig, ow, oh, s, hd);
    } else {
        if (!(big = (uint8_t *)malloc((size_t)W * H * 16))) { free(src); return 0; }
        enlarge(orig, ow, oh, s * 2, big);
        half_level(big, W * 2, H * 2, hd);
        free(big);
    }
    for (i = 0; i < (uint32_t)g_nglyph; i++) {
        const HdGlyph *g = &g_glyph[i];
        uint32_t gw = g->gw * s, gh = g->gh * s, X = g->gx * s, Y = g->gy * s;
        uint32_t hx = shipped ? X : g->hx, hy = shipped ? Y : g->hy;
        if (g->key != key || g->gx + g->gw > ow || g->gy + g->gh > oh || hx + gw > W || hy + gh > H)
            continue;
        for (y = 0; y < gh; y++)
            for (x = 0; x < gw; x++) {
                uint8_t *d = hd + ((size_t)(Y + y) * W + X + x) * 4;
                d[0] = d[1] = d[2] = 255;
                d[3] = src[((size_t)(hy + y) * W + hx + x) * 4 + 3];
            }
        n++;
    }
    free(src);
    if (g_log) fprintf(stderr, "[HDTEX] font %016llx: %u HD glyphs, x%u\n", (unsigned long long)key, n, s);
    return 1;
}

/* n passes of a separable [1 2 1] / 4 blur on the alpha of a BGRA level
 * (edges clamped). */
static void soften_alpha(uint8_t *p, uint32_t w, uint32_t h, int n)
{
    uint16_t *a = (uint16_t *)malloc((size_t)w * h * 2), *t = (uint16_t *)malloc((size_t)w * h * 2);
    uint32_t x, y;
    if (!a || !t || w < 2 || h < 2) { free(a); free(t); return; }
    for (x = 0; x < w * h; x++) a[x] = (uint16_t)(p[x * 4 + 3] << 4);     /* 12.4 fixed point */
    while (n-- > 0) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                const uint16_t *r = a + (size_t)y * w;
                t[(size_t)y * w + x] = (uint16_t)((r[x ? x - 1 : 0] + 2u * r[x] + r[x + 1 < w ? x + 1 : x] + 2) / 4);
            }
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                a[(size_t)y * w + x] = (uint16_t)((t[(size_t)(y ? y - 1 : 0) * w + x] + 2u * t[(size_t)y * w + x] +
                                                   t[(size_t)(y + 1 < h ? y + 1 : y) * w + x] + 2) / 4);
    }
    for (x = 0; x < w * h; x++) p[x * 4 + 3] = (uint8_t)((a[x] + 8) >> 4);
    free(a);
    free(t);
}

/* The HD texture has the levels of its file (or made by halving), down to the
 * size of the game's own smallest level (glevels: the guest texture's mip
 * levels), never below: the game samples its texture down to that level only.
 * The race HUD's atlases have one level and the HUD draws its pieces from
 * them a few texels from a transparent gap or another piece (a trick's points
 * banner: its body is 3 texels wide, next to the gap before its end cap). A
 * level under the game's own merges a piece with its neighbours in the atlas,
 * and a piece drawn small reads it: the banner's body went dark at its left
 * end, a sharp line where its end cap starts. */
static IDirect3DTexture8 *build(const HdEnt *e, const uint8_t *orig, uint32_t ow, uint32_t oh, uint32_t glevels,
                                uint32_t *gpu_bytes)
{
    char path[MAX_PATH * 2];
    size_t n = 0, data, off, total = 0, sz;
    uint8_t *file, *pix = NULL;
    const void *bits[16];
    uint32_t W, H, have, full = 1, levels, i, d;
    int kind;
    IDirect3DTexture8 *tex = NULL;

    if (e->font && !orig) return NULL;   /* a font needs the game's atlas: no texture without it */
    snprintf(path, sizeof path, "%s/%s", e->shipped ? g_hud_dir : g_dir, e->file);
    file = (uint8_t *)read_file(path, &n);
    if (!file) return NULL;
    if (!dds_header(file, n, &kind, &W, &H, &have, &data) || W > 4096 || H > 4096) goto done;
    for (d = W > H ? W : H; d > 1; d >>= 1) full++;
    levels = full > 16 ? 16 : full;
    if (glevels >= 1 && glevels <= 16) {     /* none smaller than the game's smallest level */
        uint32_t gw = e->w >> (glevels - 1), gh = e->h >> (glevels - 1);
        if (!gw) gw = 1;
        if (!gh) gh = 1;
        for (i = 1; i < levels && (W >> i) >= gw && (H >> i) >= gh; i++)
            ;
        levels = i;
    }
    for (i = 0; i < levels; i++) {
        uint32_t lw = W >> i ? W >> i : 1, lh = H >> i ? H >> i : 1;
        total += (size_t)lw * lh * 4;
    }
    if (!(pix = (uint8_t *)malloc(total))) goto done;
    for (i = 0, off = data, sz = 0; i < levels; i++) {
        uint32_t lw = W >> i ? W >> i : 1, lh = H >> i ? H >> i : 1;
        uint8_t *dst = pix + sz;
        size_t need = dds_level_bytes(kind, lw, lh);
        if (i < have && off + need <= n && !(orig && i > 0)) {
            dds_decode(kind, file + off, lw, lh, dst);
            to_bgra_scaled(dst, (size_t)lw * lh, e->kr, e->ka);
            if (orig && e->font) {           /* font: its HD letters in the game's layout */
                if (!font_compose(dst, lw, lh, orig, ow, oh, e->key, e->shipped)) goto done;
                if (e->soft) soften_alpha(dst, lw, lh, (int)e->soft * (int)(lw / ow / 2) * (int)(lw / ow / 2));
            } else if (orig)                 /* HUD atlas: thin strips keep the game's colour, */
                keep_thin_strips(dst, lw, lh, orig, ow, oh);   /* the mips are made from it */
            off += need;
        } else if (i > 0) {
            uint32_t pw = W >> (i - 1) ? W >> (i - 1) : 1, ph = H >> (i - 1) ? H >> (i - 1) : 1;
            half_level((const uint8_t *)bits[i - 1], pw, ph, dst);
        } else {
            goto done;                       /* not even level 0 */
        }
        bits[i] = dst;
        sz += (size_t)lw * lh * 4;
    }
    if ((e->hud || e->font) && g_hud_half && !e->shipped && levels > 1) {
        /* XBOX_HD_HUD_SCALE=2: start at the box-filtered half (level 1); the shipped HUD is at x2 already. */
        if (SUCCEEDED(d3d8_CreateTextureFromLevels(W > 1 ? W / 2 : 1, H > 1 ? H / 2 : 1, levels - 1, bits + 1, &tex)) && tex)
            *gpu_bytes = (uint32_t)(total - (size_t)W * H * 4);
    } else if (SUCCEEDED(d3d8_CreateTextureFromLevels(W, H, levels, bits, &tex)) && tex)
        *gpu_bytes = (uint32_t)total;
done:
    free(pix);
    free(file);
    return tex;
}

/* ── Jobs ──────────────────────────────────────────────────────────────
 * The pump hashes the guest bytes and queues a job; a worker reads, decodes
 * and makes the texture (the D3D11 device is free-threaded); the pump takes
 * it at the texture's next bind (hdtex_collect) -- until then the game's own
 * texture is drawn, so a load never stalls the frame. A job belongs to the
 * pump until it is collected or cancelled; `refs` counts the pump and the
 * worker holding it, the last to let go frees it. */
struct HdJob {
    const HdEnt *e;
    struct HdJob *next;                  /* queue */
    volatile LONG done;                  /* 1 = tex / bytes / ms are final */
    volatile LONG cancelled;
    volatile LONG refs;
    IDirect3DTexture8 *tex;
    uint32_t bytes;
    double ms, t_req;
    uint8_t *orig;                       /* HUD atlas: the game's level 0, linear BGRA */
    uint32_t ow, oh;
    uint32_t glevels;                    /* the guest texture's mip levels */
};

static CRITICAL_SECTION   g_qlock;
static CONDITION_VARIABLE g_qwake;
static HdJob *g_qhead, *g_qtail;

static void job_release(HdJob *j)
{
    if (InterlockedDecrement(&j->refs) == 0) {
        if (j->tex) j->tex->lpVtbl->Release(j->tex);   /* cancelled after it was made */
        free(j->orig);
        free(j);
    }
}

static void job_run(HdJob *j)
{
    double t0 = now_ms();
    if (!j->cancelled) j->tex = build(j->e, j->orig, j->ow, j->oh, j->glevels, &j->bytes);
    j->ms = now_ms() - t0;
    MemoryBarrier();
    InterlockedExchange(&j->done, 1);
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        HdJob *j;
        EnterCriticalSection(&g_qlock);
        while (!g_qhead) SleepConditionVariableCS(&g_qwake, &g_qlock, INFINITE);
        j = g_qhead;
        g_qhead = j->next;
        if (!g_qhead) g_qtail = NULL;
        LeaveCriticalSection(&g_qlock);
        job_run(j);
        job_release(j);
    }
    return 0;
}

static int start_workers(void)
{
    SYSTEM_INFO si;
    int i, n;
    const char *v = getenv("XBOX_HD_TEXTURES_THREADS");
    GetSystemInfo(&si);
    n = v && atoi(v) > 0 ? atoi(v) : (int)si.dwNumberOfProcessors / 4;   /* the game and the pump keep theirs */
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    InitializeCriticalSection(&g_qlock);
    InitializeConditionVariable(&g_qwake);
    for (i = 0; i < n; i++) {
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (!t) break;
        CloseHandle(t);
        g_workers++;
    }
    return g_workers > 0;
}

HdJob *hdtex_request(const uint8_t *level0, uint32_t n0, uint32_t w, uint32_t h, uint32_t levels, int *gate)
{
    HdEnt key, *e;
    HdJob *j;
    if (gate) *gate = -1;
    if (!hdtex_on() || !level0 || !n0) return NULL;
    key.key = fnv64(level0, n0);
    e = (HdEnt *)bsearch(&key, g_ent, (size_t)g_n, sizeof *g_ent, ent_cmp);
    if (!e || e->w != w || e->h != h) {
        g_misses++;
        if (g_log >= 2) fprintf(stderr, "[HDTEX] miss %016llx %ux%u\n", (unsigned long long)key.key, w, h);
        return NULL;
    }
    if ((e->font || e->hud) && g_frame_gate >= 0 && gate)   /* both kept; the image's gate picks */
        *gate = e->font ? HDTEX_GATE_FONT : HDTEX_GATE_HUD;
    if (!(j = (HdJob *)calloc(1, sizeof *j))) return NULL;
    j->e = e;
    j->glevels = levels;
    /* A HUD atlas (32-bit, swizzled in guest memory: linear images never get
     * here) hands its own texels over, unswizzled, for keep_thin_strips. */
    if (e->hud && n0 == w * h * 4 && w && h && !(w & (w - 1)) && !(h & (h - 1)) &&
        (j->orig = (uint8_t *)malloc(n0)) != NULL) {
        uint32_t x, y;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                memcpy(j->orig + ((size_t)y * w + x) * 4, level0 + (size_t)swz_index(x, y, w, h) * 4, 4);
        j->ow = w;
        j->oh = h;
    }
    /* A font (A4R4G4B4, swizzled): its texels as BGRA8 for font_compose. */
    if (e->font && n0 == w * h * 2 && w && h && !(w & (w - 1)) && !(h & (h - 1)) &&
        (j->orig = (uint8_t *)malloc((size_t)w * h * 4)) != NULL) {
        uint32_t x, y;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                const uint8_t *t = level0 + (size_t)swz_index(x, y, w, h) * 2;
                uint8_t *o = j->orig + ((size_t)y * w + x) * 4;
                unsigned v = t[0] | (unsigned)t[1] << 8;
                o[0] = (uint8_t)((v & 15) * 17);
                o[1] = (uint8_t)((v >> 4 & 15) * 17);
                o[2] = (uint8_t)((v >> 8 & 15) * 17);
                o[3] = (uint8_t)((v >> 12 & 15) * 17);
            }
        j->ow = w;
        j->oh = h;
    }
    j->t_req = now_ms();
    if (g_sync) {
        j->refs = 1;
        job_run(j);
        return j;
    }
    j->refs = 2;
    EnterCriticalSection(&g_qlock);
    if (g_qtail) g_qtail->next = j; else g_qhead = j;
    g_qtail = j;
    LeaveCriticalSection(&g_qlock);
    WakeConditionVariable(&g_qwake);
    return j;
}

int hdtex_collect(HdJob *j, IDirect3DTexture8 **tex, uint32_t *gpu_bytes)
{
    double wait;
    *tex = NULL;
    *gpu_bytes = 0;
    if (!j->done) return 0;
    MemoryBarrier();
    wait = now_ms() - j->t_req;
    if (!j->tex) {
        g_fail++;
        fprintf(stderr, "[HDTEX] could not load %s/%s\n", g_dir, j->e->file);
    } else {
        *tex = j->tex;
        *gpu_bytes = j->bytes;
        j->tex = NULL;                    /* the caller owns it now */
        g_hits++;
        g_bytes_made += *gpu_bytes;
        g_live += *gpu_bytes;
        if (g_live > g_live_max) g_live_max = g_live;
        g_ms_total += j->ms;
        if (j->ms > g_ms_max) g_ms_max = j->ms;
        g_wait_total += wait;
        if (wait > g_wait_max) g_wait_max = wait;
        if (g_log)
            fprintf(stderr, "[HDTEX] hit %016llx %ux%u -> %s (%.1f ms work, %.1f ms until used, %u KB)\n",
                    (unsigned long long)j->e->key, j->e->w, j->e->h, j->e->file, j->ms, wait, *gpu_bytes >> 10);
        if (g_log || g_hits % 100 == 1)
            fprintf(stderr, "[HDTEX] %u loaded (%.0f MB made, %.0f MB alive, %.0f MB most), %u misses, %u failed, "
                            "%u dropped (%u over budget), %u cancelled; work %.1f ms mean, %.1f ms max, %.0f ms total; "
                            "until used %.0f ms mean, %.0f ms max (%d workers)\n",
                    g_hits, (double)g_bytes_made / 1048576.0, (double)g_live / 1048576.0, (double)g_live_max / 1048576.0,
                    g_misses, g_fail, g_evicted, g_evicted_budget, g_cancelled, g_ms_total / g_hits, g_ms_max,
                    g_ms_total, g_wait_total / g_hits, g_wait_max, g_sync ? 0 : g_workers);
    }
    job_release(j);
    return 1;
}

void hdtex_cancel(HdJob *j)
{
    g_cancelled++;
    InterlockedExchange(&j->cancelled, 1);
    job_release(j);
}
