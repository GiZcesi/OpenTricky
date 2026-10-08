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
    uint8_t  kr, ka;            /* colour factors: RGB x kr, alpha x ka */
    uint8_t  menu;              /* a menu / interface picture */
    const char *file;
} HdEnt;

static HdEnt   *g_ent;
static int      g_n;
static char    *g_text;         /* the index's own copy, file names point into it */
static const char *g_builtin;
static size_t   g_builtin_len;
static char     g_dir[MAX_PATH];
static int      g_state = -1;   /* -1 not read yet, 0 off, 1 on */
static int      g_log, g_menus, g_sync, g_workers;
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

/* Parse "<key> <w>x<h> <rgb><alpha> <3d|menu> <file>" lines in place; '#' = comment. */
static void parse_index(char *text)
{
    int cap = 0, skipped = 0;
    char *line = text, *next;
    for (; line && *line; line = next) {
        unsigned long long k;
        unsigned w, h, f;
        char cat[8];
        int used = 0;
        HdEnt e;
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (line[0] == '#' || line[0] == '\r' || !line[0]) continue;
        if (sscanf(line, "%llx %ux%u %u %7s %n", &k, &w, &h, &f, cat, &used) != 5 || !used ||
            w == 0 || h == 0 || w > 4096 || h > 4096 || f / 10 < 1 || f / 10 > 2 || f % 10 < 1 || f % 10 > 2) {
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
        e.kr = (uint8_t)(f / 10);
        e.ka = (uint8_t)(f % 10);
        e.menu = strcmp(cat, "menu") == 0;
        if (e.menu && !g_menus) continue;
        if (g_n == cap) {
            HdEnt *grown;
            cap = cap ? cap * 2 : 2048;
            grown = (HdEnt *)realloc(g_ent, (size_t)cap * sizeof *g_ent);
            if (!grown) break;
            g_ent = grown;
        }
        g_ent[g_n++] = e;
    }
    if (skipped) fprintf(stderr, "[HDTEX] %d index lines not understood\n", skipped);
    qsort(g_ent, (size_t)g_n, sizeof *g_ent, ent_cmp);
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

static int start_workers(void);

int hdtex_on(void)
{
    const char *dir, *v;
    char path[MAX_PATH * 2];
    const char *from = NULL;
    if (g_state >= 0) return g_state;
    g_state = 0;
    dir = getenv("XBOX_HD_TEXTURES");
    if (!dir || !dir[0] || !strcmp(dir, "0")) return 0;
    v = getenv("XBOX_HD_LOG");             g_log = v ? atoi(v) : 0;      /* 1 hits, 2 misses too */
    v = getenv("XBOX_HD_TEXTURES_MENUS");  g_menus = v && v[0] == '1';
    v = getenv("XBOX_HD_TEXTURES_MB");
    g_budget = (uint64_t)((v && atoi(v) >= 64) ? atoi(v) : 1024) << 20;
    v = getenv("XBOX_HD_TEXTURES_SYNC");   g_sync = v && v[0] == '1';

    v = getenv("XBOX_HD_TEXTURES_INDEX");
    if (v && v[0]) {
        g_text = read_file(v, NULL);
        from = v;
    } else {
        snprintf(path, sizeof path, "%s/hdtex_index.txt", dir);
        if ((g_text = read_file(path, NULL)) != NULL) from = path;
        else if (g_builtin && g_builtin_len && (g_text = (char *)malloc(g_builtin_len + 1)) != NULL) {
            memcpy(g_text, g_builtin, g_builtin_len);
            g_text[g_builtin_len] = '\0';
            from = "(built in)";
        }
    }
    if (!g_text) {
        fprintf(stderr, "[HDTEX] off: no index (%s)\n", v && v[0] ? v : "none built in");
        return 0;
    }
    parse_index(g_text);
    if (g_n == 0) {
        fprintf(stderr, "[HDTEX] off: the index %s lists nothing\n", from);
        return 0;
    }
    if (!find_pack_dir(dir)) {
        fprintf(stderr, "[HDTEX] off: none of the index's files are in %s (the pack's folder, "
                        "or the folder above \"replacements\")\n", dir);
        return 0;
    }
    if (!g_sync && !start_workers()) g_sync = 1;
    fprintf(stderr, "[HDTEX] on: %d textures listed (index %s, menus %s), pack %s, budget %u MB, %s\n",
            g_n, from, g_menus ? "on" : "off", g_dir, (unsigned)(g_budget >> 20),
            g_sync ? "loaded at first use (synchronous)" : "loaded by worker threads");
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

/* RGBA -> BGRA in place, colours times kr, alpha times ka (clamped). */
static void to_bgra_scaled(uint8_t *p, size_t texels, int kr, int ka)
{
    size_t i;
    for (i = 0; i < texels; i++, p += 4) {
        unsigned r = p[0] * (unsigned)kr, g = p[1] * (unsigned)kr, b = p[2] * (unsigned)kr, a = p[3] * (unsigned)ka;
        p[0] = (uint8_t)(b > 255 ? 255 : b);
        p[1] = (uint8_t)(g > 255 ? 255 : g);
        p[2] = (uint8_t)(r > 255 ? 255 : r);
        p[3] = (uint8_t)(a > 255 ? 255 : a);
    }
}

/* Box-filtered half of a BGRA level (for a pack file without its full chain). */
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

static IDirect3DTexture8 *build(const HdEnt *e, uint32_t *gpu_bytes)
{
    char path[MAX_PATH * 2];
    size_t n = 0, data, off, total = 0, sz;
    uint8_t *file, *pix = NULL;
    const void *bits[16];
    uint32_t W, H, have, full = 1, levels, i, d;
    int kind;
    IDirect3DTexture8 *tex = NULL;

    snprintf(path, sizeof path, "%s/%s", g_dir, e->file);
    file = (uint8_t *)read_file(path, &n);
    if (!file) return NULL;
    if (!dds_header(file, n, &kind, &W, &H, &have, &data) || W > 4096 || H > 4096) goto done;
    for (d = W > H ? W : H; d > 1; d >>= 1) full++;
    levels = full > 16 ? 16 : full;
    for (i = 0; i < levels; i++) {
        uint32_t lw = W >> i ? W >> i : 1, lh = H >> i ? H >> i : 1;
        total += (size_t)lw * lh * 4;
    }
    if (!(pix = (uint8_t *)malloc(total))) goto done;
    for (i = 0, off = data, sz = 0; i < levels; i++) {
        uint32_t lw = W >> i ? W >> i : 1, lh = H >> i ? H >> i : 1;
        uint8_t *dst = pix + sz;
        size_t need = dds_level_bytes(kind, lw, lh);
        if (i < have && off + need <= n) {
            dds_decode(kind, file + off, lw, lh, dst);
            to_bgra_scaled(dst, (size_t)lw * lh, e->kr, e->ka);
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
    if (SUCCEEDED(d3d8_CreateTextureFromLevels(W, H, levels, bits, &tex)) && tex)
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
};

static CRITICAL_SECTION   g_qlock;
static CONDITION_VARIABLE g_qwake;
static HdJob *g_qhead, *g_qtail;

static void job_release(HdJob *j)
{
    if (InterlockedDecrement(&j->refs) == 0) {
        if (j->tex) j->tex->lpVtbl->Release(j->tex);   /* cancelled after it was made */
        free(j);
    }
}

static void job_run(HdJob *j)
{
    double t0 = now_ms();
    if (!j->cancelled) j->tex = build(j->e, &j->bytes);
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

HdJob *hdtex_request(const uint8_t *level0, uint32_t n0, uint32_t w, uint32_t h)
{
    HdEnt key, *e;
    HdJob *j;
    if (!hdtex_on() || !level0 || !n0) return NULL;
    key.key = fnv64(level0, n0);
    e = (HdEnt *)bsearch(&key, g_ent, (size_t)g_n, sizeof *g_ent, ent_cmp);
    if (!e || e->w != w || e->h != h) {
        g_misses++;
        if (g_log >= 2) fprintf(stderr, "[HDTEX] miss %016llx %ux%u\n", (unsigned long long)key.key, w, h);
        return NULL;
    }
    if (!(j = (HdJob *)calloc(1, sizeof *j))) return NULL;
    j->e = e;
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
