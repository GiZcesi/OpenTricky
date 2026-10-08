/*
 * ticktrace -- per race tick trace. See ticktrace.h.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "recomp/recomp_types.h"
#include "ticktrace.h"

void sub_000AD4A0(void);    /* InGameState vt+0x14: race tick (thiscall, ret) */
void sub_0012A610(void);    /* RNG core, ecx = object (A 0x1FAD70, B 0x1FAD88) */

int g_ticktrace_on;

#define APP_GLOBAL  0x001E3C7Cu
#define RNG_A       0x001FAD70u
#define RNG_B       0x001FAD88u

static struct {
    int      rng;
    FILE    *f;
    CRITICAL_SECTION lock;
    DWORD    tid;                       /* loop thread (the one running the ticks) */
    volatile int in_tick;
    unsigned seq;                       /* race tick number seen by the trace */
    uint32_t extra_va[8], extra_len[8];
    int      n_extra;
    DWORD    last_flush;
} s;

static uint32_t fnv(uint32_t va, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)XBOX_PTR(va);
    uint32_t h = 2166136261u, i;
    for (i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t race_ptr(void)
{
    uint32_t app = MEM32(APP_GLOBAL), lvl;
    if (app < 0x1000u) return 0;
    lvl = MEM32(app + 0x72Cu);
    if (lvl < 0x1000u) return 0;
    return MEM32(lvl + 0x1Cu);
}

/* Race camera: block of view 0 in InGameState, view matrix +0xC0 (64 bytes,
 * passed to SetViewMatrix vt+0x74 by 0xDDF30 at render) and projection
 * parameters +0x100 / +0x104. */
static uint32_t cam_hash(void)
{
    uint32_t app = MEM32(APP_GLOBAL), st = app >= 0x1000u ? MEM32(app + 4u) : 0;
    return st >= 0x1000u ? fnv(st + 0xC0u, 0x48u) : 0;
}

/* Rider: kinematics (position +0x170, velocity +0x180, RiderEvent state
 * +0x458, speed factor +0x15C, gauge +0x1C). */
static uint32_t rider_kin(uint32_t r)
{
    uint32_t h = fnv(r + 0x170u, 0x1Cu) ^ 0x9E3779B9u;
    h = (h ^ MEM32(r + 0x458u)) * 16777619u;
    h = (h ^ MEM32(r + 0x15Cu)) * 16777619u;
    return (h ^ MEM32(r + 0x1Cu)) * 16777619u;
}

/* ── guest callers from the host stack ─────────────────────────────
 * Translated functions call each other directly in C: the host stack IS the
 * guest call chain (the guest return slot is 0). Reverse table (host pointer
 * -> guest VA) built once with recomp_lookup. */
typedef struct { uintptr_t host; uint32_t va; } Rev;
static Rev   *s_rev;
static size_t s_nrev;

static int rev_cmp(const void *a, const void *b)
{
    uintptr_t x = ((const Rev *)a)->host, y = ((const Rev *)b)->host;
    return x < y ? -1 : x > y;
}

static void rev_build(void)
{
    uint32_t va;
    size_t cap = 16384;
    s_rev = (Rev *)malloc(cap * sizeof *s_rev);
    for (va = 0x00010000u; va < 0x00190000u && s_rev; va++) {
        recomp_func_t fn = recomp_lookup(va);
        if (!fn) continue;
        if (s_nrev == cap) { cap *= 2; s_rev = (Rev *)realloc(s_rev, cap * sizeof *s_rev); if (!s_rev) break; }
        s_rev[s_nrev].host = (uintptr_t)fn;
        s_rev[s_nrev].va = va;
        s_nrev++;
    }
    if (s_rev) qsort(s_rev, s_nrev, sizeof *s_rev, rev_cmp);
}

static uint32_t rev_va(uintptr_t host)
{
    size_t lo = 0, hi = s_nrev;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s_rev[mid].host <= host) lo = mid + 1; else hi = mid;
    }
    return lo ? s_rev[lo - 1].va : 0;
}

/* ── watch: who writes what (XBOX_TICKTRACE_WATCH) ─────────────────
 * Regions relative to a base: st (InGameState [app+4]), r0..r7 (riders
 * [race+0xC4+4i]), race, app, or an absolute VA (0x...). For each word:
 * number of ticks where it changed DURING the tick, and of renders where it
 * changed DURING the render (0xAB610). Summary at exit, per contiguous range. */
enum { WB_ABS, WB_ST, WB_RACE, WB_APP, WB_CAM, WB_R0 };
typedef struct {
    int      base;      /* WB_*, WB_R0 + i */
    uint32_t off, len, abs_va;
    uint32_t *before, *n_tick, *n_rend;
    char     name[32];
} Watch;
static Watch s_w[8];
static int   s_nw;
static unsigned long long s_w_ticks, s_w_rends;

static uint32_t watch_va(const Watch *w)
{
    uint32_t app = MEM32(APP_GLOBAL), race = race_ptr(), b = 0;
    switch (w->base) {
    case WB_ABS:  return w->abs_va;
    case WB_ST:   b = app >= 0x1000u ? MEM32(app + 4u) : 0; break;
    case WB_RACE: b = race; break;
    case WB_APP:  b = app; break;
    case WB_CAM:  b = app >= 0x1000u && MEM32(app + 4u) >= 0x1000u ? MEM32(MEM32(app + 4u) + 0x2Cu) : 0; break;
    default:
        if (race && (uint32_t)(w->base - WB_R0) < MEM32(race + 0x88u)) b = MEM32(race + 0xC4u + 4u * (w->base - WB_R0));
    }
    return b >= 0x1000u ? b + w->off : 0;
}

static void watch_parse(const char *e)
{
    while (e && *e && s_nw < 8) {
        Watch *w = &s_w[s_nw];
        const char *t = e;
        memset(w, 0, sizeof *w);
        if (!strncmp(e, "st", 2)) { w->base = WB_ST; e += 2; }
        else if (!strncmp(e, "race", 4)) { w->base = WB_RACE; e += 4; }
        else if (!strncmp(e, "app", 3)) { w->base = WB_APP; e += 3; }
        else if (!strncmp(e, "cam", 3)) { w->base = WB_CAM; e += 3; }
        else if (e[0] == 'r' && e[1] >= '0' && e[1] <= '7') { w->base = WB_R0 + (e[1] - '0'); e += 2; }
        else { w->base = WB_ABS; w->abs_va = (uint32_t)strtoul(e, (char **)&e, 0); }
        if (*e == '+') w->off = (uint32_t)strtoul(e + 1, (char **)&e, 0);
        if (*e == ':') w->len = (uint32_t)strtoul(e + 1, (char **)&e, 0) & ~3u;
        snprintf(w->name, sizeof w->name, "%.*s", (int)(e - t), t);
        if (w->len && w->len <= 0x10000u) {
            w->before = (uint32_t *)calloc(w->len / 4, 4);
            w->n_tick = (uint32_t *)calloc(w->len / 4, 4);
            w->n_rend = (uint32_t *)calloc(w->len / 4, 4);
            s_nw++;
        }
        while (*e && *e != ',') e++;
        if (*e == ',') e++;
    }
}

static void watch_snap(uint32_t *va)
{
    int i;
    for (i = 0; i < s_nw; i++) {
        va[i] = watch_va(&s_w[i]);
        if (va[i]) memcpy(s_w[i].before, (const void *)XBOX_PTR(va[i]), s_w[i].len);
    }
}

static void watch_diff(const uint32_t *va, int rend)
{
    int i;
    for (i = 0; i < s_nw; i++) {
        uint32_t k, *now, *cnt = rend ? s_w[i].n_rend : s_w[i].n_tick;
        if (!va[i] || watch_va(&s_w[i]) != va[i]) continue;
        now = (uint32_t *)XBOX_PTR(va[i]);
        for (k = 0; k < s_w[i].len / 4; k++) if (now[k] != s_w[i].before[k]) cnt[k]++;
    }
}


static void watch_report(void)
{
    int i;
    for (i = 0; i < s_nw; i++) {
        uint32_t k, n = s_w[i].len / 4, start = 0;
        int open = 0, kind = 0;
        fprintf(s.f, "# watch %s (%llu ticks, %llu renders): ranges +off..+end [t=changed during the tick, r=during the render, max count]\n",
                s_w[i].name, s_w_ticks, s_w_rends);
        for (k = 0; k <= n; k++) {
            int kk = k < n ? ((s_w[i].n_tick[k] != 0) | (s_w[i].n_rend[k] != 0) << 1) : 0;
            if (open && kk != kind) {
                uint32_t j, mt = 0, mr = 0;
                for (j = start; j < k; j++) { if (s_w[i].n_tick[j] > mt) mt = s_w[i].n_tick[j]; if (s_w[i].n_rend[j] > mr) mr = s_w[i].n_rend[j]; }
                fprintf(s.f, "#   +%04X..+%04X %s%s t=%u r=%u\n", start * 4, k * 4 - 1,
                        kind & 1 ? "t" : "-", kind & 2 ? "r" : "-", mt, mr);
                open = 0;
            }
            if (!open && kk) { open = 1; kind = kk; start = k; }
        }
    }
}

/* ── render reads (XBOX_TICKTRACE_READS) ───────────────────────────
 * One region (same syntax as WATCH). One render in XBOX_TICKTRACE_READS_EVERY
 * (default 30): its host pages become PAGE_NOACCESS; each access raises an
 * exception, recorded (word read / written in the region, guest function from
 * RIP), then the page is opened for one instruction (single step) and
 * protected again. Slow: investigation tool only. */
static Watch     s_rd;
static int       s_rd_on, s_rd_every = 30;
static uint32_t *s_rd_nr, *s_rd_nw;           /* per word: renders where read / written */
static uint32_t *s_rd_seen;                   /* "seen in this render" mark */
static uint32_t  s_rd_gen;
static volatile LONG s_rd_armed;
static uintptr_t s_rd_lo, s_rd_hi, s_rd_va_host;   /* protected pages; host base of the region */
static unsigned long long s_rd_renders, s_rd_faults;
typedef struct { uint32_t va, lo, hi; unsigned long long n; } RdFn;
static RdFn s_rd_fn[48], s_wr_fn[48];           /* readers, writers */
static int  s_rd_tick;                           /* XBOX_TICKTRACE_READS_PHASE=tick: trace the ticks */
static DWORD s_rd_prot = PAGE_READWRITE;         /* original protection of the pages */
static __thread uintptr_t t_rd_page;

static void rd_note_fn(RdFn *t, uint32_t va, uint32_t off)
{
    int i;
    for (i = 0; i < 48; i++)
        if (t[i].va == va || !t[i].va) {
            if (!t[i].n || off < t[i].lo) t[i].lo = off;
            if (off > t[i].hi) t[i].hi = off;
            t[i].va = va; t[i].n++;
            return;
        }
}

static LONG CALLBACK rd_veh(EXCEPTION_POINTERS *x)
{
    DWORD code = x->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && s_rd_armed) {
        uintptr_t a = (uintptr_t)x->ExceptionRecord->ExceptionInformation[1], pg;
        DWORD old;
        if (a < s_rd_lo || a >= s_rd_hi) return EXCEPTION_CONTINUE_SEARCH;
        s_rd_faults++;
        if (a >= s_rd_va_host && a < s_rd_va_host + s_rd.len) {
            uint32_t w = (uint32_t)((a - s_rd_va_host) >> 2);
            if (s_rd_seen[w] != s_rd_gen) {
                s_rd_seen[w] = s_rd_gen;
                if (x->ExceptionRecord->ExceptionInformation[0] == 1) s_rd_nw[w]++; else s_rd_nr[w]++;
            }
            rd_note_fn(x->ExceptionRecord->ExceptionInformation[0] == 1 ? s_wr_fn : s_rd_fn,
                       rev_va((uintptr_t)x->ContextRecord->Rip), w * 4u);
        }
        pg = a & ~(uintptr_t)0xFFF;
        VirtualProtect((void *)pg, 0x1000, s_rd_prot, &old);
        t_rd_page = pg;
        x->ContextRecord->EFlags |= 0x100;          /* one instruction, then protect again */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && t_rd_page) {
        DWORD old;
        if (s_rd_armed) VirtualProtect((void *)t_rd_page, 0x1000, PAGE_NOACCESS, &old);
        t_rd_page = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int rd_arm(void)
{
    uint32_t va = watch_va(&s_rd);
    DWORD old;
    if (!va) return 0;
    s_rd_va_host = XBOX_PTR(va);
    s_rd_lo = s_rd_va_host & ~(uintptr_t)0xFFF;
    s_rd_hi = (s_rd_va_host + s_rd.len + 0xFFF) & ~(uintptr_t)0xFFF;
    s_rd_gen++;
    InterlockedExchange(&s_rd_armed, 1);
    if (!VirtualProtect((void *)s_rd_lo, s_rd_hi - s_rd_lo, PAGE_NOACCESS, &old)) { InterlockedExchange(&s_rd_armed, 0); return 0; }
    s_rd_prot = old;
    return 1;
}

static void rd_disarm(void)
{
    DWORD old;
    InterlockedExchange(&s_rd_armed, 0);
    VirtualProtect((void *)s_rd_lo, s_rd_hi - s_rd_lo, s_rd_prot, &old);
    s_rd_renders++;
}

static void rd_report(void)
{
    uint32_t k, n = s_rd.len / 4, start = 0;
    int open = 0, kind = 0, i;
    if (!s_rd_on) return;
    fprintf(s.f, "# reads %s (%llu %s traced, %llu faults): ranges read (R) / written (W) DURING the %s, max count\n",
            s_rd.name, s_rd_renders, s_rd_tick ? "ticks" : "renders", s_rd_faults, s_rd_tick ? "tick" : "render");
    for (k = 0; k <= n; k++) {
        int kk = k < n ? ((s_rd_nr[k] != 0) | (s_rd_nw[k] != 0) << 1) : 0;
        if (open && kk != kind) {
            uint32_t j, mr = 0, mw = 0;
            for (j = start; j < k; j++) { if (s_rd_nr[j] > mr) mr = s_rd_nr[j]; if (s_rd_nw[j] > mw) mw = s_rd_nw[j]; }
            fprintf(s.f, "#   +%04X..+%04X %s%s R=%u W=%u\n", start * 4, k * 4 - 1, kind & 1 ? "R" : "-", kind & 2 ? "W" : "-", mr, mw);
            open = 0;
        }
        if (!open && kk) { open = 1; kind = kk; start = k; }
    }
    fprintf(s.f, "# reads %s: reading functions (va:count[+first..+last offset read])", s_rd.name);
    for (i = 0; i < 48 && s_rd_fn[i].va; i++)
        fprintf(s.f, " %06X:%llu[+%X..+%X]", s_rd_fn[i].va, s_rd_fn[i].n, s_rd_fn[i].lo, s_rd_fn[i].hi);
    fprintf(s.f, "\n# reads %s: writing functions (va:count[+first..+last offset written])", s_rd.name);
    for (i = 0; i < 48 && s_wr_fn[i].va; i++)
        fprintf(s.f, " %06X:%llu[+%X..+%X]", s_wr_fn[i].va, s_wr_fn[i].n, s_wr_fn[i].lo, s_wr_fn[i].hi);
    fputc('\n', s.f);
}

/* ── hooks ─────────────────────────────────────────────────────── */

void sub_000AB610(void);
extern void (*fps_cap_lookup(unsigned int xbox_va))(void);

/* InGameState render (only with XBOX_TICKTRACE_WATCH); chains to the fps_cap
 * hook if there is one (DUP mode). */
static void hook_tt_AB610(void)
{
    uint32_t va[8];
    recomp_func_t next = fps_cap_lookup(0x000AB610u);
    int on = s_nw && race_ptr(), rd = 0;
    static unsigned n_rend;
    if (on) watch_snap(va);
    if (s_rd_on && !s_rd_tick && race_ptr() && GetCurrentThreadId() == s.tid && (n_rend++ % (unsigned)s_rd_every) == 0u) rd = rd_arm();
    if (next) next(); else sub_000AB610();
    if (rd) rd_disarm();
    if (on) { watch_diff(va, 1); s_w_rends++; }
}

static void maybe_flush(void)
{
    DWORD t = GetTickCount();
    if (t - s.last_flush > 2000u) { fflush(s.f); s.last_flush = t; }
}

/* Not static: pass 13 would route a direct call here (none today). */
void hook_tt_AD4A0(void)
{
    uint32_t a0 = MEM32(RNG_A + 0x14u), b0 = MEM32(RNG_B + 0x14u), race, i, n, va[8];
    int w;
    recomp_func_t nx = fps_cap_lookup(0x000AD4A0u);   /* fps_cap: interpolation */
    if (!g_ticktrace_on) { if (nx) nx(); else sub_000AD4A0(); return; }
    if (!s.tid) s.tid = GetCurrentThreadId();
    w = s_nw && race_ptr();
    if (w) watch_snap(va);
    {
        static unsigned n_tick;
        int rd = s_rd_on && s_rd_tick && race_ptr() && (n_tick++ % (unsigned)s_rd_every) == 0u && rd_arm();
        s.in_tick = 1;
        if (nx) nx(); else sub_000AD4A0();
        s.in_tick = 0;
        if (rd) rd_disarm();
    }
    if (w) { watch_diff(va, 0); s_w_ticks++; }
    race = race_ptr();
    /* periodic cumulative summary: a killed process does not go through atexit */
    if ((s_nw || s_rd_on) && s.seq % 1800u == 0u) { EnterCriticalSection(&s.lock); watch_report(); rd_report(); LeaveCriticalSection(&s.lock); }
    EnterCriticalSection(&s.lock);
    s.seq++;
    fprintf(s.f, "T %u %u %u %u %u %u %u %08X %08X", s.seq,
            race ? MEM32(race + 0x18u) : 0, race ? MEM32(race + 0x1Cu) : 0,
            MEM32(RNG_A + 0x14u), MEM32(RNG_B + 0x14u),
            MEM32(RNG_A + 0x14u) - a0, MEM32(RNG_B + 0x14u) - b0,
            fnv(RNG_A, 0x18u), fnv(RNG_B, 0x18u));
    fprintf(s.f, " %08X", race ? fnv(race, 0x400u) : 0);
    n = race ? MEM32(race + 0x88u) : 0;
    if (n > 8) n = 8;
    fprintf(s.f, " %08X %u", cam_hash(), n);
    for (i = 0; i < n; i++) {
        uint32_t r = MEM32(race + 0xC4u + 4u * i);
        fprintf(s.f, " %08X", r >= 0x1000u ? rider_kin(r) : 0);
    }
    for (i = 0; i < (uint32_t)s.n_extra; i++) fprintf(s.f, " x%08X", fnv(s.extra_va[i], s.extra_len[i]));
    fputc('\n', s.f);
    maybe_flush();
    LeaveCriticalSection(&s.lock);
}

/* Also routed from direct calls (pass 13): without XBOX_TICKTRACE_RNG, calls
 * the original and does nothing else. */
void hook_tt_12A610(void)
{
    uint32_t obj = g_ecx;
    if (!s.rng || (obj != RNG_A && obj != RNG_B)) { sub_0012A610(); return; }
    {
        void *fr[12];
        uint32_t calls[3] = { 0, 0, 0 }, n0 = MEM32(obj + 0x14u);
        unsigned short c = CaptureStackBackTrace(1, 12, fr, NULL), j, k = 0;
        char ph = GetCurrentThreadId() != s.tid ? 'o' : (s.in_tick ? 't' : 'h');
        for (j = 0; j < c && k < 3; j++) {
            uint32_t va = rev_va((uintptr_t)fr[j]);
            if (va >= 0x0012A4B0u && va < 0x0012A700u) continue;   /* RNG wrappers */
            if (k && calls[k - 1] == va) continue;
            calls[k++] = va;
        }
        EnterCriticalSection(&s.lock);
        fprintf(s.f, "R %u %c %u %c %06X %06X %06X\n", s.seq, obj == RNG_A ? 'A' : 'B', n0, ph,
                calls[0], calls[1], calls[2]);
        LeaveCriticalSection(&s.lock);
    }
    sub_0012A610();
}

/* ── init ──────────────────────────────────────────────────────── */

/* The highest "_local" above the exe's folder, + \ticktrace (np_cmdlog's rule). */
static void default_dir(char *out, size_t cap)
{
    char dir[MAX_PATH], probe[MAX_PATH], *p;
    DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
    snprintf(out, cap, ".");
    if (!n || n >= MAX_PATH) return;
    if ((p = strrchr(dir, '\\'))) *p = 0;
    snprintf(out, cap, "%s", dir);
    for (;;) {
        DWORD a;
        if (snprintf(probe, sizeof probe, "%s\\_local", dir) >= (int)sizeof probe) return;
        a = GetFileAttributesA(probe);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
            snprintf(out, cap, "%s\\ticktrace", probe);
        if (!(p = strrchr(dir, '\\'))) return;
        *p = 0;
    }
}

static void ticktrace_atexit(void)
{
    if (s.f) { EnterCriticalSection(&s.lock); watch_report(); rd_report(); fflush(s.f); LeaveCriticalSection(&s.lock); }
}

void ticktrace_init(void)
{
    const char *e = getenv("XBOX_TICKTRACE");
    char dir[MAX_PATH], path[MAX_PATH + 64];
    SYSTEMTIME t;
    if (!e || strcmp(e, "1")) return;
    InitializeCriticalSection(&s.lock);
    e = getenv("XBOX_TICKTRACE_DIR");
    if (e && *e) snprintf(dir, sizeof dir, "%s", e); else default_dir(dir, sizeof dir);
    CreateDirectoryA(dir, NULL);
    GetLocalTime(&t);
    snprintf(path, sizeof path, "%s\\ticktrace_%04u%02u%02u_%02u%02u%02u_%lu.txt", dir,
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    s.f = fopen(path, "w");
    if (!s.f) { fprintf(stderr, "[TICKTRACE] cannot open %s: off\n", path); return; }
    setvbuf(s.f, NULL, _IOFBF, 1 << 20);
    e = getenv("XBOX_TICKTRACE_EXTRA");
    while (e && *e && s.n_extra < 8) {
        unsigned long va = strtoul(e, (char **)&e, 0), len = 0;
        if (*e == ':') len = strtoul(e + 1, (char **)&e, 0);
        if (va && len) { s.extra_va[s.n_extra] = (uint32_t)va; s.extra_len[s.n_extra] = (uint32_t)len; s.n_extra++; }
        while (*e && *e != ',') e++;
        if (*e == ',') e++;
    }
    watch_parse(getenv("XBOX_TICKTRACE_WATCH"));
    {   /* XBOX_TICKTRACE_READS: one region, same syntax; read through the last watch slot */
        int n0 = s_nw;
        watch_parse(getenv("XBOX_TICKTRACE_READS"));
        if (s_nw > n0) {
            s_rd = s_w[n0]; s_nw = n0;
            s_rd_nr = (uint32_t *)calloc(s_rd.len / 4, 4);
            s_rd_nw = (uint32_t *)calloc(s_rd.len / 4, 4);
            s_rd_seen = (uint32_t *)calloc(s_rd.len / 4, 4);
            e = getenv("XBOX_TICKTRACE_READS_EVERY");
            if (e && atoi(e) > 0) s_rd_every = atoi(e);
            e = getenv("XBOX_TICKTRACE_READS_PHASE");
            s_rd_tick = e && !strcmp(e, "tick");
            s_rd_on = s_rd_nr && s_rd_nw && s_rd_seen;
            if (s_rd_on) AddVectoredExceptionHandler(1, rd_veh);
            if (!s_rev) rev_build();
        }
    }
    e = getenv("XBOX_TICKTRACE_RNG");
    s.rng = e && e[0] == '1';
    if (s.rng && !s_rev) rev_build();
    fprintf(s.f, "# ticktrace v1: T seq race_tick state nA nB dA dB rngA rngB race cam nriders riders(kinematics)... [xEXTRA]%s\n",
            s.rng ? "; R seq A|B n phase(t/h/o) caller1 caller2 caller3" : "");
    g_ticktrace_on = 1;
    atexit(ticktrace_atexit);
    fprintf(stderr, "[TICKTRACE] per tick trace -> %s%s (%zu functions in the reverse table)\n",
            path, s.rng ? "; RNG draws" : "", s_nrev);
}

void (*ticktrace_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x000AD4A0u) return hook_tt_AD4A0;
    if (xbox_va == 0x0012A610u) return hook_tt_12A610;
    if (xbox_va == 0x000AB610u && (s_nw || s_rd_on)) return hook_tt_AB610;
    return 0;
}
