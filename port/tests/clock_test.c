/*
 * clock_test.c -- host test of host_ticks_to_units (xboxrecomp/src/nv2a/
 * host_clock.h), the tick -> ns / us conversion behind the APU XGSCNT
 * register and the NV2A PTIMER.
 *
 *   gcc -std=c11 -O2 -Wall -Wextra -I xboxrecomp/src/nv2a port/tests/clock_test.c \
 *       -o clock_test.exe && ./clock_test.exe
 *
 * A simulated performance counter runs over several days of host uptime
 * (no real clock is read). For each sample the test computes:
 *   - the exact value, floor(count * unit / freq), with a 128-bit product;
 *   - the old formula, count * unit / freq in 64 bits (the product wraps;
 *     computed here with unsigned arithmetic so the test itself has no UB);
 *   - the new helper.
 * It then follows what the game reads: XGSCNT = (uint32_t)(ns / 100), a
 * 10 MHz counter that wraps every 429.5 s, and checks that every step
 * advances by the elapsed time.
 */
#include "host_clock.h"

#include <stdint.h>
#include <stdio.h>

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int64_t exact(int64_t count, int64_t freq, int64_t unit)
{
    return (int64_t)(((unsigned __int128)(uint64_t)count * (uint64_t)unit) /
                     (uint64_t)freq);
}

static int64_t old_formula(int64_t count, int64_t freq, int64_t unit)
{
    /* What `count * unit / freq` does on x86-64 once the product wraps. */
    int64_t wrapped = (int64_t)((uint64_t)count * (uint64_t)unit);
    return wrapped / freq;
}

/* Walk host uptime from 0 to `days`, `step_ms` apart, at counter frequency
 * `freq`, for `unit` (1e9: ns, read as XGSCNT; 1e6: us). Returns the number
 * of steps whose value did not advance by the elapsed time. */
static long walk(int64_t freq, int64_t unit, double days, int64_t step_ms,
                 int use_old, double *first_bad_s, double *worst_jump_s)
{
    const int64_t step_ticks = freq / 1000 * step_ms;
    const int64_t end = (int64_t)(days * 86400.0) * freq;
    int64_t count;
    long bad = 0;
    int have_prev = 0;
    uint64_t prev = 0;

    *first_bad_s = -1.0;
    *worst_jump_s = 0.0;
    for (count = 0; count <= end; count += step_ticks) {
        int64_t v = use_old ? old_formula(count, freq, unit)
                            : host_ticks_to_units(count, freq, unit);
        int64_t want_step, got_step;
        uint64_t cur;

        if (!use_old) {
            s_checks++;
            if (v != exact(count, freq, unit)) {
                if (s_fail++ < 5)
                    printf("FAIL exact: count %lld freq %lld unit %lld: %lld != %lld\n",
                           (long long)count, (long long)freq, (long long)unit,
                           (long long)v, (long long)exact(count, freq, unit));
            }
        }
        if (unit == 1000000000LL) {
            /* XGSCNT: 32-bit register, 100 ns per count */
            cur = (uint32_t)(v / 100);
            want_step = step_ms * 10000;
            got_step = have_prev ? (int64_t)(uint32_t)(cur - prev) : want_step;
        } else {
            cur = (uint64_t)v;
            want_step = step_ms * (unit / 1000);
            got_step = have_prev ? (int64_t)(cur - prev) : want_step;
        }
        /* one unit of rounding either way is the normal floor() jitter */
        if (got_step < want_step - 1 || got_step > want_step + 1) {
            double t = (double)count / (double)freq;
            double jump;
            if (unit == 1000000000LL) {
                int64_t signed_step = (int64_t)(int32_t)(uint32_t)(cur - prev);
                jump = (double)(signed_step - want_step) / 1e7;
            } else {
                jump = (double)((int64_t)(cur - prev) - want_step) / (double)unit;
            }
            if (bad == 0) *first_bad_s = t;
            if (jump < 0 ? -jump > (*worst_jump_s < 0 ? -*worst_jump_s : *worst_jump_s)
                         : jump > (*worst_jump_s < 0 ? -*worst_jump_s : *worst_jump_s))
                *worst_jump_s = jump;
            bad++;
        }
        prev = cur;
        have_prev = 1;
    }
    return bad;
}

static void report(const char *what, int64_t freq, int64_t unit, double days,
                   int64_t step_ms, int use_old, long *out_bad)
{
    double first, worst;
    long bad = walk(freq, unit, days, step_ms, use_old, &first, &worst);
    printf("%-34s freq %10lld Hz, %4.0f days, step %lld ms: %ld bad step(s)",
           what, (long long)freq, days, (long long)step_ms, bad);
    if (bad)
        printf(", first at %.1f s uptime (%.2f h), largest jump %+.1f s",
               first, first / 3600.0, worst);
    printf("\n");
    *out_bad = bad;
}

int main(void)
{
    static const int64_t freqs[] = { 10000000LL, 24000000LL, 3000000000LL };
    long bad;
    unsigned i;

    /* Exactness at the edges for the frequencies Windows and Wine use. */
    for (i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        int64_t f = freqs[i];
        int64_t c;
        for (c = 0; c < 3 * f; c += f / 7 + 1)
            CHECK(host_ticks_to_units(c, f, 1000000000LL) == exact(c, f, 1000000000LL));
        CHECK(host_ticks_to_units(INT64_MAX, f, 1000000LL) == exact(INT64_MAX, f, 1000000LL));
        CHECK(host_ticks_to_units(f * 86400LL * 365LL, f, 1000000000LL) ==
              exact(f * 86400LL * 365LL, f, 1000000000LL));
    }
    /* The case named in the review: count = 1e10 at 10 MHz (1000 s). */
    CHECK(host_ticks_to_units(10000000000LL, 10000000LL, 1000000000LL) == 1000000000000LL);
    printf("old: count 1e10 @ 10 MHz -> %lld ns (want 1000000000000)\n",
           (long long)old_formula(10000000000LL, 10000000LL, 1000000000LL));

    printf("\n-- XGSCNT (ns / 100, 32 bits) as the game reads it --\n");
    report("old formula (before)", 10000000LL, 1000000000LL, 3.0, 100, 1, &bad);
    CHECK(bad > 0);   /* documents the defect */
    for (i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        report("host_ticks_to_units (after)", freqs[i], 1000000000LL, 3.0, 100, 0, &bad);
        CHECK(bad == 0);
    }

    printf("\n-- qemu_clock_get_us (APU pacing) --\n");
    report("old formula (before)", 10000000LL, 1000000LL, 30.0, 1000, 1, &bad);
    CHECK(bad > 0);
    report("host_ticks_to_units (after)", 10000000LL, 1000000LL, 30.0, 1000, 0, &bad);
    CHECK(bad == 0);

    printf("\n%d check(s), %d failure(s)\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
