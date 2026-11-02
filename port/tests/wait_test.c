/*
 * wait_test.c -- host test of the kernel's waits on NT timeouts, from 100 ns
 * to the longest relative interval (xboxrecomp/src/kernel/kernel_sync.c and
 * kernel_thread.c).
 *
 *   gcc -std=gnu11 -O3 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
 *       -I xboxrecomp/src/kernel -I xboxrecomp/src port/tests/wait_test.c \
 *       -o wait_test.exe && ./wait_test.exe
 *
 * (-O3 as the game is built: the old code's undefined behaviour depends on it.)
 *
 * The real kernel_sync.c and kernel_thread.c are built into the test, with the
 * Win32 calls they wait with redirected to a simulated clock: a Win32 wait of
 * N ms moves the clock N ms on and times out, unless the test has scheduled
 * the object's signal, or an APC, within it -- the clock then stops there.
 * Nothing really waits, so a wait of centuries runs in a moment.
 *
 * Each wait (NtWaitForSingleObject, NtWaitForSingleObjectEx,
 * NtWaitForMultipleObjectsEx, KeWaitForSingleObject, KeWaitForMultipleObjects,
 * KeDelayExecutionThread) is checked with relative and absolute timeouts:
 *   - it lasts as long as the timeout says, in whole milliseconds (a relative
 *     wait under 1 ms: 1 ms, or a yield for KeDelayExecutionThread), and never
 *     passes INFINITE for a finite timeout;
 *   - a timeout that fits in one Win32 wait (up to 0xFFFFFFFE ms) is a single
 *     Win32 call, with that count;
 *   - a signal, or an APC, after the first 49.7 days still ends the wait, with
 *     the status NT gives it.
 * Built against kernel sources that cast the timeout to a 32-bit count, the
 * long cases fail and the short ones pass.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

/* ---- Simulated clock, and the Win32 calls the waits make ---------------- */

#define NEVER UINT64_MAX
#define FT_EPOCH 133000000000000000ull  /* a system time (100 ns since 1601) */

static uint64_t s_clock_ms;     /* time since the call under test began */
static uint64_t s_event_ms;     /* the object is signalled, or the APC comes, then */
static int      s_event_apc;    /* the event is an APC (alertable waits only) */
static unsigned s_calls;        /* Win32 waits the call made */
static DWORD    s_first_ms;     /* the count the first one was given */
static int      s_infinite;     /* one of them was given INFINITE */
static unsigned s_yields;

static DWORD sim_wait(DWORD ms, BOOL alertable, DWORD signalled)
{
    uint64_t end = ms == INFINITE ? NEVER : s_clock_ms + ms;

    if (++s_calls == 1)
        s_first_ms = ms;
    if (ms == INFINITE)
        s_infinite = 1;
    if (s_event_ms != NEVER && s_event_ms <= end && (alertable || !s_event_apc)) {
        if (s_event_ms > s_clock_ms)
            s_clock_ms = s_event_ms;
        return s_event_apc ? WAIT_IO_COMPLETION : signalled;
    }
    if (end == NEVER)
        return WAIT_FAILED;     /* nothing would ever end this wait */
    s_clock_ms = end;
    return WAIT_TIMEOUT;
}

static DWORD t_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    (void)h;
    return sim_wait(ms, alertable, WAIT_OBJECT_0);
}

static DWORD t_WaitForMultipleObjectsEx(DWORD n, const HANDLE *h, BOOL all,
                                        DWORD ms, BOOL alertable)
{
    (void)n; (void)h; (void)all;
    return sim_wait(ms, alertable, WAIT_OBJECT_0);
}

static DWORD t_SleepEx(DWORD ms, BOOL alertable)
{
    return sim_wait(ms, alertable, 0) == WAIT_IO_COMPLETION ? WAIT_IO_COMPLETION : 0;
}

static void t_Sleep(DWORD ms)
{
    (void)sim_wait(ms, FALSE, 0);
}

static BOOL t_SwitchToThread(void)
{
    s_yields++;
    return TRUE;
}

static void t_GetSystemTimeAsFileTime(LPFILETIME ft)
{
    uint64_t t = FT_EPOCH + s_clock_ms * 10000;
    ft->dwLowDateTime  = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

#define WaitForSingleObjectEx    t_WaitForSingleObjectEx
#define WaitForMultipleObjectsEx t_WaitForMultipleObjectsEx
#define SleepEx                  t_SleepEx
#define Sleep                    t_Sleep
#define SwitchToThread           t_SwitchToThread
#define GetSystemTimeAsFileTime  t_GetSystemTimeAsFileTime

#include "kernel_sync.c"
#include "kernel_thread.c"

#undef WaitForSingleObjectEx
#undef WaitForMultipleObjectsEx
#undef SleepEx
#undef Sleep
#undef SwitchToThread
#undef GetSystemTimeAsFileTime

void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level; (void)subsystem; (void)fmt;
}

/* ---- The waits under test ----------------------------------------------- */

enum { W_NT_SINGLE, W_NT_SINGLE_EX, W_NT_MULTIPLE_EX, W_KE_SINGLE,
       W_KE_MULTIPLE, W_DELAY, W_DELAY_ALERTABLE, W_COUNT };

static const char *const s_wait_name[W_COUNT] = {
    "NtWaitForSingleObject", "NtWaitForSingleObjectEx",
    "NtWaitForMultipleObjectsEx", "KeWaitForSingleObject",
    "KeWaitForMultipleObjects", "KeDelayExecutionThread",
    "KeDelayExecutionThread alertable",
};

static NTSTATUS call_wait(int w, PLARGE_INTEGER timeout, BOOLEAN alertable)
{
    HANDLE h = (HANDLE)(ULONG_PTR)0x1234;
    HANDLE hs[2] = { h, h };
    PVOID objs[2] = { h, h };

    switch (w) {
    case W_NT_SINGLE:      return xbox_NtWaitForSingleObject(h, alertable, timeout);
    case W_NT_SINGLE_EX:   return xbox_NtWaitForSingleObjectEx(h, 1, alertable, timeout);
    case W_NT_MULTIPLE_EX: return xbox_NtWaitForMultipleObjectsEx(2, hs, 1, alertable, timeout);
    case W_KE_SINGLE:      return xbox_KeWaitForSingleObject(h, 0, 0, alertable, timeout);
    case W_KE_MULTIPLE:    return xbox_KeWaitForMultipleObjects(2, objs, 1, 0, 0, alertable,
                                                                timeout, NULL);
    case W_DELAY:          return xbox_KeDelayExecutionThread(1, FALSE, timeout);
    default:               return xbox_KeDelayExecutionThread(1, TRUE, timeout);
    }
}

static int s_checks, s_fail;

static void check(int ok, const char *wait, const char *what, const char *fmt,
                  unsigned long long want, unsigned long long got)
{
    s_checks++;
    if (!ok) {
        s_fail++;
        printf("FAIL %-34s %-34s ", wait, what);
        printf(fmt, want, got);
        printf("\n");
    }
}

/* One wait: `timeout` (NULL: none) with the object signalled, or an APC, at
 * `event_ms` (NEVER: no event). Checks the status, how long it lasted, the
 * Win32 calls and the yields. want_first is the count of the first Win32
 * wait (~0ull: not checked). */
static void run(int w, const char *what, PLARGE_INTEGER timeout, BOOLEAN alertable,
                uint64_t event_ms, int event_apc, NTSTATUS want_status,
                uint64_t want_ms, unsigned long long want_first,
                unsigned want_yields)
{
    NTSTATUS st;

    s_clock_ms = 0;
    s_event_ms = event_ms;
    s_event_apc = event_apc;
    s_calls = 0;
    s_first_ms = 0;
    s_infinite = 0;
    s_yields = 0;
    st = call_wait(w, timeout, alertable);

    check(st == want_status, s_wait_name[w], what, "status want 0x%08llX got 0x%08llX",
          (unsigned long long)(uint32_t)want_status, (unsigned long long)(uint32_t)st);
    check(s_clock_ms == want_ms, s_wait_name[w], what, "lasted want %llu ms got %llu ms",
          want_ms, s_clock_ms);
    if (timeout)
        check(!s_infinite, s_wait_name[w], what, "INFINITE passed: want %llu got %llu",
              0, (unsigned long long)s_infinite);
    if (want_first != ~0ull)
        check(s_calls >= 1 && s_first_ms == want_first, s_wait_name[w], what,
              "first Win32 wait want %llu ms got %llu ms", want_first,
              s_calls ? (unsigned long long)s_first_ms : ~0ull);
    check(s_yields == want_yields, s_wait_name[w], what, "yields want %llu got %llu",
          want_yields, s_yields);
}

int main(void)
{
    /* Timeouts in whole ms: under, at and past the longest Win32 wait, past
     * 2^32 ms (where the low 32 bits were all that was left), a year, and the
     * longest relative interval, 0x8000000000000000 (XAPI's Sleep(INFINITE)). */
    static const struct { const char *what; uint64_t ms; } lengths[] = {
        { "1 ms",                    1 },
        { "16 ms",                   16 },
        { "0xFFFFFFFE ms (49.7 d)",  0xFFFFFFFEull },
        { "0xFFFFFFFF ms",           0xFFFFFFFFull },
        { "2^32 ms",                 0x100000000ull },
        { "2^32 + 5 ms",             0x100000005ull },
        { "10 x 2^32 + 123 ms",      0xA0000007Bull },
        { "365 days",                31536000000ull },
        { "0x8000000000000000",      0 },   /* INT64_MIN: 922337203685477 ms */
    };
    const uint64_t longest = 922337203685477ull;    /* 2^63 x 100 ns, in ms */
    int w;
    size_t i;

    for (w = 0; w < W_COUNT; w++) {
        const int delay = w == W_DELAY || w == W_DELAY_ALERTABLE;
        const BOOLEAN alertable = w == W_DELAY_ALERTABLE;
        const NTSTATUS timed_out = delay ? STATUS_SUCCESS : STATUS_TIMEOUT;
        LARGE_INTEGER t;
        char what[96];

        for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
            uint64_t ms = lengths[i].ms ? lengths[i].ms : longest;
            unsigned long long first = ms < 0xFFFFFFFEull ? ms : 0xFFFFFFFEull;

            /* Relative */
            t.QuadPart = lengths[i].ms ? -(LONGLONG)(ms * 10000) : INT64_MIN;
            snprintf(what, sizeof(what), "relative %s", lengths[i].what);
            run(w, what, &t, alertable, NEVER, 0, timed_out, ms, first, 0);

            /* Absolute: the system time `ms` from now (the longest: the
             * largest absolute time there is) */
            if (lengths[i].ms) {
                t.QuadPart = (LONGLONG)(FT_EPOCH + ms * 10000);
            } else {
                t.QuadPart = INT64_MAX;
                ms = (uint64_t)(INT64_MAX - (LONGLONG)FT_EPOCH) / 10000;
                first = 0xFFFFFFFEull;
            }
            snprintf(what, sizeof(what), "absolute %s", lengths[i].what);
            run(w, what, &t, alertable, NEVER, 0, timed_out, ms, first, 0);
        }

        /* Under 1 ms: a relative wait takes 1 ms, a delay yields; an
         * absolute one takes what is left in whole ms, here none. */
        t.QuadPart = -1;
        run(w, "relative 100 ns", &t, alertable, NEVER, 0, timed_out,
            delay ? 0 : 1, delay ? ~0ull : 1, delay ? 1 : 0);
        t.QuadPart = (LONGLONG)FT_EPOCH + 1;
        run(w, "absolute 100 ns", &t, alertable, NEVER, 0, timed_out, 0, 0, 0);

        /* Zero: a wait polls once, a delay yields */
        t.QuadPart = 0;
        run(w, "zero", &t, alertable, NEVER, 0, timed_out, 0, delay ? ~0ull : 0,
            delay ? 1 : 0);

        /* An absolute time already past */
        t.QuadPart = (LONGLONG)FT_EPOCH - 10000;
        run(w, "absolute, past", &t, alertable, NEVER, 0, timed_out, 0,
            delay ? ~0ull : 0, 0);

        if (!delay) {
            /* No timeout: one INFINITE wait, ended by the object */
            run(w, "none, signalled at 5 ms", NULL, FALSE, 5, 0, STATUS_SUCCESS, 5,
                INFINITE, 0);

            /* Signalled after the first 49.7 days of a 2^33 ms timeout */
            t.QuadPart = -(LONGLONG)(0x200000000ull * 10000);
            run(w, "2^33 ms, signalled at 2^32 + 100 ms", &t, FALSE,
                0x100000064ull, 0, STATUS_SUCCESS, 0x100000064ull, 0xFFFFFFFEull, 0);

            /* An APC after the first 49.7 days of an alertable wait */
            t.QuadPart = INT64_MIN;
            run(w, "longest, alertable, APC at 60 days", &t, TRUE,
                5184000000ull, 1, STATUS_ALERTED, 5184000000ull, 0xFFFFFFFEull, 0);
        } else if (alertable) {
            /* Sleep(INFINITE) on a thread that gets an APC after 60 days */
            t.QuadPart = INT64_MIN;
            run(w, "longest, APC at 60 days", &t, TRUE, 5184000000ull, 1,
                STATUS_ALERTED, 5184000000ull, 0xFFFFFFFEull, 0);
        } else {
            /* NULL: an invalid parameter, as before */
            run(w, "none", NULL, FALSE, NEVER, 0, STATUS_INVALID_PARAMETER, 0, ~0ull, 0);
        }
    }

    printf("%d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
