/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* For g_esp, MEM32 and XBOX_PTR -- the overrides below read guest stack
 * arguments and translate guest addresses. Deliberately without
 * RECOMP_GENERATED_CODE, so the bare register names stay unmapped here and
 * anything touching guest state has to say g_esp explicitly. */
#include "recomp/recomp_types.h"
#include "pass_tags.h"
#include "netplay/np_cmdlog.h"
#include "netplay/np_ghost.h"
#include "fps_cap.h"
#include "aspect.h"
#include "drawdist.h"
#include "ticktrace.h"
#include "../../xboxrecomp/src/kernel/xbox_perf.h"
extern void (*perf_lookup(unsigned int xbox_va))(void);

/* Per-thread SSE / MMX register files (see recomp_types.h). */
__thread recomp_xmm_t g_xmm[8];
__thread uint64_t g_mm[8];





/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * This xboxrecomp checkout already defines the real storage for these in
 * src/kernel/xbox_memory_layout.c (part of the xbox_kernel runtime library) --
 * defining them again here as well caused a "multiple definition" link error,
 * so this file only declares them extern and reads them.
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern __thread uint32_t g_eax;
extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/* ── Video framebuffer discovery ─────────────────────────────
 *
 * The title's video player does not draw through the push buffer: it locks the
 * back buffer through the Xbox D3D8 linked into the XBE and writes decoded MPEG
 * pixels straight into guest video memory. Nothing then moved those pixels to
 * the host swap chain, so the intro video decoded perfectly and the screen
 * stayed black.
 *
 * The addresses were found by hand once (0xF3BA0000 / 0xF3CCC000, double
 * buffered) and lived in XBOX_GUEST_FB / XBOX_GUEST_FB2. Hardcoding them is not
 * a fix -- they are a property of one build's VRAM layout. This wraps the guest
 * LockRect instead and takes the surface the title itself hands back, so the
 * video works with nothing set.
 *
 * sub_0016B1C0(surface, pLockedRect, pRect, flags), ret 16. On entry esp points
 * at the dummy return address, so pLockedRect is at [esp+8], and it must be
 * read BEFORE the call -- the original pops its arguments.
 *
 * Only surfaces in the write-combined VRAM alias with a sane 32-bit pitch are
 * taken; this call also serves plenty of surfaces that are not the framebuffer.
 */
extern void sub_0016B1C0(void);
void d3d8_SetGuestFramebuffer(const void *src, unsigned pitch,
                              unsigned w, unsigned h);
void d3d8_SetGuestFramebufferAlt(const void *alt);
void d3d8_NoteGuestFramebufferLock(void);

void hook_lockrect_0016B1C0(void)
{
    uint32_t plr = (uint32_t)MEM32(g_esp + 8);
    uint32_t pitch, pbits;

    sub_0016B1C0();

    if (!plr) return;
    pitch = (uint32_t)MEM32(plr);
    pbits = (uint32_t)MEM32(plr + 4);

    if (pbits < 0xF0000000u || pbits >= 0xF4000000u) return;
    if (pitch < 0x400u || pitch > 0x4000u) return;

    {
        static uint32_t seen_a = 0, seen_b = 0;
        unsigned w = pitch / 4;
        unsigned h = 480;                    /* the title's video mode */
        /* Every lock of a known framebuffer, not just the first: the present
         * shows the guest buffer only while these keep arriving. */
        if (pbits == seen_a || pbits == seen_b) {
            d3d8_NoteGuestFramebufferLock();
            return;
        }
        d3d8_NoteGuestFramebufferLock();
        if (!seen_a) {
            seen_a = pbits;
            d3d8_SetGuestFramebuffer(XBOX_PTR(pbits), pitch, w, h);
            fprintf(stderr, "  [FB] guest framebuffer discovered: 0x%08X "
                    "pitch %u (%ux%u)\n", pbits, pitch, w, h);
            fflush(stderr);
        } else if (!seen_b) {
            seen_b = pbits;
            d3d8_SetGuestFramebufferAlt(XBOX_PTR(pbits));
            fprintf(stderr, "  [FB] second framebuffer discovered: 0x%08X "
                    "(double buffered)\n", pbits);
            fflush(stderr);
        }
    }
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /* Xbox D3D8 IDirect3DSurface8::LockRect -- see the note above. */
    if (xbox_va == 0x0016B1C0u) return hook_lockrect_0016B1C0;

    /* Render-pass tag hooks, only when XBOX_PASS_TAGS is on --
     * off, the original path is untouched (see pass_tags.h). */
    if (g_pass_tags_mode) {
        recomp_func_t fn = pass_tags_lookup(xbox_va);
        if (fn) return fn;
    }

    /* Display shapes wider than 16:9 -- only then is the hook on
     * Renderer_SetScreenMode handed out (see aspect.h). */
    if (g_aspect_hook_on) {
        recomp_func_t fn = aspect_lookup(xbox_va);
        if (fn) return fn;
    }

    /* Draw distance Far / Max (XBOX_DRAW_DISTANCE) -- in Original
     * (default) none is handed out (see drawdist.h). */
    {
        recomp_func_t fn = drawdist_lookup(xbox_va);
        if (fn) return fn;
    }

    /* Rider command log, only when XBOX_NETLOG=1 -- off, the
     * original path is untouched (see netplay/np_cmdlog.h). */
    if (g_np_cmdlog_on || g_np_ghost_on) {
        recomp_func_t fn = np_cmdlog_lookup(xbox_va);
        if (fn) return fn;
    }

    /* Frame profile by zones (XBOX_PERF=1) -- first: its hooks
     * time the call and chain to ticktrace / fps_cap themselves. Off, none
     * is handed out. */
    if (g_perf_on) {
        recomp_func_t fn = perf_lookup(xbox_va);
        if (fn) return fn;
    }

    /* Per-tick trace (XBOX_TICKTRACE=1) -- before fps_cap: its
     * tick hook does not overlap the cap's hooks. Off, none is handed out
     * (the RNG hook routed from direct calls by pass 13 is a pass-through). */
    if (g_ticktrace_on) {
        recomp_func_t fn = ticktrace_lookup(xbox_va);
        if (fn) return fn;
    }

    /* Host frame cap (XBOX_FPS_CAP != 60) -- renders without a
     * tick while the main loop waits for its frame event. At the default
     * (60) the hook is not installed: the original path is untouched. */
    if (g_fps_cap_on) {
        recomp_func_t fn = fps_cap_lookup(xbox_va);
        if (fn) return fn;
    }

    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/*
 * Deduped unresolved-ICALL-target logger. Prints each unique VA exactly
 * once, so a real run's output stays readable (RECOMP_ICALL_SAFE calls this
 * on every lookup miss, which can be very frequent for hot code paths).
 * Used to find what to seed next without needing a native stack-trace
 * diagnostic every time --
 */
#define ICALL_MISS_LOG_CAP 512
static uint32_t g_icall_miss_seen[ICALL_MISS_LOG_CAP];
static uint32_t g_icall_miss_hits[ICALL_MISS_LOG_CAP];
static uint32_t g_icall_miss_prev[ICALL_MISS_LOG_CAP];
static int g_icall_miss_count = 0;

/*
 * Report unresolved targets by hit rate since the previous report.
 *
 * The rate is what separates the two very different things this list holds: a
 * target hit once during init is a gap worth seeding eventually, while one hit
 * at the frame rate is the reason nothing renders. A single deduped line per
 * target makes them look identical.
 */
/*
 * Per-call-site accounting.
 *
 * The deduped per-target list answers "which addresses are unresolved" but not
 * "who is calling them", and for a hot miss that is the only question that
 * matters: target 0x00000000 was reported at 212 million misses a run, all
 * folded into one line whose "from" attribution was whichever site happened to
 * reach it first. A NULL vtable slot called four million times a second is a
 * different defect from one called once at init, and they were indistinguishable.
 */
#define ICALL_SITE_CAP 256
static struct { uint32_t va; const char *file; int line; unsigned long long hits; }
    g_icall_sites[ICALL_SITE_CAP];
static int g_icall_site_count = 0;

static void icall_site_count(uint32_t va, const char *file, int line)
{
    int i;
    for (i = 0; i < g_icall_site_count; i++)
        if (g_icall_sites[i].va == va && g_icall_sites[i].line == line
            && g_icall_sites[i].file == file) {
            g_icall_sites[i].hits++;
            return;
        }
    if (g_icall_site_count < ICALL_SITE_CAP) {
        g_icall_sites[g_icall_site_count].va = va;
        g_icall_sites[g_icall_site_count].file = file;
        g_icall_sites[g_icall_site_count].line = line;
        g_icall_sites[g_icall_site_count].hits = 1;
        g_icall_site_count++;
    }
}

static void icall_site_report(void)
{
    int i, j, best;
    int printed = 0;
    char used[ICALL_SITE_CAP];
    if (g_icall_site_count == 0) return;
    for (i = 0; i < g_icall_site_count; i++) used[i] = 0;
    fprintf(stderr, "[ICALL-MISS] busiest call sites:\n");
    for (j = 0; j < 6; j++) {
        best = -1;
        for (i = 0; i < g_icall_site_count; i++)
            if (!used[i] && (best < 0
                             || g_icall_sites[i].hits > g_icall_sites[best].hits))
                best = i;
        if (best < 0 || g_icall_sites[best].hits == 0) break;
        used[best] = 1;
        {
            const char *f = g_icall_sites[best].file, *b = f;
            const char *q;
            if (f) for (q = f; *q; q++) if (*q == 47 || *q == 92) b = q + 1;
            fprintf(stderr, "[ICALL-MISS]   0x%08X  %llu from %s:%d\n",
                    g_icall_sites[best].va, g_icall_sites[best].hits,
                    b ? b : "?", g_icall_sites[best].line);
        }
        printed++;
    }
    if (printed == 0)
        fprintf(stderr, "[ICALL-MISS]   (no sites recorded)\n");
}

void recomp_icall_miss_report(void)
{
    int i;
    unsigned long long total = 0;
    for (i = 0; i < g_icall_miss_count; i++)
        total += g_icall_miss_hits[i] - g_icall_miss_prev[i];
    if (total == 0)
        return;
    fprintf(stderr, "[ICALL-MISS] %llu misses since last report:\n", total);
    for (i = 0; i < g_icall_miss_count; i++) {
        uint32_t d = g_icall_miss_hits[i] - g_icall_miss_prev[i];
        g_icall_miss_prev[i] = g_icall_miss_hits[i];
        if (d)
            fprintf(stderr, "[ICALL-MISS]   0x%08X  %u since last (%u total)\n",
                    g_icall_miss_seen[i], d, g_icall_miss_hits[i]);
    }
    icall_site_report();
    fflush(stderr);
}


/*
 * Copy the deduped unresolved-target list, in first-seen order. Used by the
 * live diagnostics server (xbox_diag) so the list can be read while the title
 * is still running, instead of having to scrape stderr afterwards.
 */
int recomp_icall_miss_list(uint32_t *out, int max)
{
    int n = g_icall_miss_count;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = g_icall_miss_seen[i];
    return n;
}

/* Kept so anything still calling the old name compiles; the site-aware
 * form below is what the RECOMP_ICALL macros now use. */
void recomp_icall_miss_log_once(uint32_t va)
{
    recomp_icall_miss_log_at(va, NULL, 0);
}

void recomp_icall_miss_log_at(uint32_t va, const char *file, int line)
{
    icall_site_count(va, file, line);
    for (int i = 0; i < g_icall_miss_count; i++) {
        if (g_icall_miss_seen[i] == va) {
            g_icall_miss_hits[i]++;
            {   /* Periodic rate report, driven by the misses themselves. */
                static DWORD last = 0;
                DWORD now = GetTickCount();
                if (last == 0) last = now;
                if (now - last >= 3000) { last = now; recomp_icall_miss_report(); }
            }
            return;                      /* already logged */
        }
    }
    if (g_icall_miss_count < ICALL_MISS_LOG_CAP) {
        g_icall_miss_hits[g_icall_miss_count] = 1;
        g_icall_miss_seen[g_icall_miss_count++] = va;
    }
    {
        /* Only the basename is useful -- the generated files all sit in one
         * directory, and the full build path swamps the line. */
        const char *base = file;
        if (base) { const char *p;
            for (p = file; *p; p++) if (*p == 47 || *p == 92) base = p + 1; }
        fprintf(stderr, "[ICALL-MISS] unresolved target 0x%08X (new, #%d) from %s:%d\n",
                va, g_icall_miss_count, base ? base : "?", line);
    }
#ifdef _WIN32
    /*
     * A miss whose target is obvious garbage (not a plausible Xbox VA) means
     * the *function pointer itself* was read from the wrong place -- the
     * interesting question is then which translated function performed the
     * call, not which address it landed on. Print a native backtrace for
     * those only, so the ordinary "not seeded yet" misses stay one line each.
     */
    /*
     * XBOX_ICALL_TRACE=0xVA also forces a backtrace for one specific target.
     * A target inside the code range looks "plausible" and so never got a
     * trace, but a plausible-looking address can still be garbage -- 0x0002108A
     * sits one byte inside an fmul -- and then the only useful question is
     * which translated function performed the call.
     */
    static uint32_t trace_va = 0xFFFFFFFFu;
    if (trace_va == 0xFFFFFFFFu) {
        const char *e = getenv("XBOX_ICALL_TRACE");
        trace_va = e ? (uint32_t)strtoul(e, NULL, 0) : 0u;
    }
    if (va < 0x00010000u || va >= 0x00400000u || (trace_va && va == trace_va)) {
        void *frames[24];
        USHORT n = CaptureStackBackTrace(0, 24, frames, NULL);
        uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
        fprintf(stderr, "  [ICALL-MISS] implausible target -- native frames (link addrs):\n");
        for (USHORT i = 0; i < n; i++) {
            uintptr_t f = (uintptr_t)frames[i];
            if (f >= base && f < base + 0x08000000ULL) {
                fprintf(stderr, "    [%u] 0x%llX\n", (unsigned)i,
                        (unsigned long long)(0x140000000ULL + (f - base)));
            }
        }
    }
#endif
    fflush(stderr);

}

/*
 * Original x86 INT 3 instructions get lifted to __debugbreak(), which by
 * default lowers to __builtin_trap() (a hard SIGILL) -- fine for a debug
 * build attached to a debugger, but fatal for us since nothing catches it.
 * On real Xbox hardware with no debugger attached, INT 3 also raises a
 * machine exception; retail titles that ship with them left in generally
 * rely on the exception handler treating it as a no-op/continue rather
 * than a fatal fault. Log once per site and continue, matching this
 * codebase's existing "degrade gracefully, don't hard-crash" convention
 * (see RECOMP_ICALL_SAFE) instead of aborting the whole process.
 */
#define DEBUGBREAK_LOG_CAP 256
static const char *g_debugbreak_seen[DEBUGBREAK_LOG_CAP];
static int g_debugbreak_count = 0;

void recomp_debugbreak_log_once(const char *site)
{
    for (int i = 0; i < g_debugbreak_count; i++) {
        if (g_debugbreak_seen[i] == site) return; /* already logged (same __FILE__:__LINE__ literal) */
    }
    if (g_debugbreak_count < DEBUGBREAK_LOG_CAP) {
        g_debugbreak_seen[g_debugbreak_count++] = site;
    }
    fprintf(stderr, "[INT3] hit original debug-break at %s (new, #%d) -- continuing\n", site, g_debugbreak_count);
    fflush(stderr);
}
