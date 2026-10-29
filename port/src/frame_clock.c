/*
 * frame_clock -- the title's 60 Hz frame timer on an exact grid.
 *
 * The title paces itself with a one-shot timeSetEvent re-armed at the end of
 * every Application_FrameTimerCallback (0xB26B0, through its stdcall thunk
 * 0xB26A0): it reads its millisecond clock (KeTickCount), adds 16.667 minus
 * the measured interval to an accumulator, and asks for trunc(acc + 16.667)
 * whole milliseconds (gen/recomp_0004.c, 0xB26B0-0xB2745). The accumulator
 * aims at an absolute 60.000 Hz grid; the whole milliseconds and the clock's
 * error scatter the actual expiries 14 to 19 ms apart (sd 0.95 ms).
 * The callback is where the title reads the controller (Application_TickFrame
 * -> Input_CatchUpPollAndTick) and signals the main loop's frame event.
 *
 * Here the expiry the title asks for is replaced by the exact point of that
 * grid: one per 1000/60 ms, on the performance counter. Nothing in the title
 * changes -- the same accumulator, the same delays asked, the same ticks; only
 * the instant the timer fires moves by the fraction of a millisecond the API
 * could not express. The grid starts again from the title's own schedule when
 * the delay asked strays from it by more than REANCHOR_MS (a load, a stall,
 * the title's catch-up). The timer is armed EARLY_MS ahead and the callback's
 * entry waits for the exact instant (a timer wake is ~0.1-0.5 ms late).
 *
 * XBOX_FRAME_CLOCK=0: off, the title's own expiries (diagnostic comparison).
 *
 * The grid point of each callback is also the time its tick was produced:
 * the interpolation above 60 fps measures every image from it
 * (frame_clock_tick_ref), rather than from the moment the main loop saw the
 * frame event, which comes late whenever the event fires during a render.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "recomp/recomp_types.h"
#include "frame_clock.h"
#include "../../xboxrecomp/src/kernel/xbox_logstats.h"

void sub_000B26A0(void);                       /* Application_FrameTimerCallback_StdcallThunk (stdcall, ret 20) */
extern LONGLONG (*g_xbox_ktimer_due_filter)(uint32_t timer_va, LONGLONG rel_100ns);
LONGLONG xbox_kt_qpf(void);
void d3d8_sync_set_frame_clock(void);                                         /* d3d8_sync.h */

#define FRAME_CB        0x000B26A0u
#define SLOT_PROC       0x28u                  /* XAPI timeSetEvent entry: lpTimeProc */
#define SLOT_USER       0x2Cu                  /* dwUser = XBoxExecutionMan */
#define XEM_ACC         0x10u                  /* XBoxExecutionMan: accumulator (float, ms) */
#define PERIOD_MS       (1000.0 / 60.0)
#define EARLY_MS        2.0                    /* timer armed this early; the callback's entry waits */
#define REANCHOR_MS     4.0

static struct {
    int on;
    double qf;
    double next;                /* planned instant of the next callback (ms, QPC) */
    uint32_t user;
    /* The tick of the last callback (frame_clock_tick_ref): its grid point, the
     * callbacks entered so far and the one that set it. */
    volatile double tick_ref;
    volatile LONG fires, ref_fire;
} F;

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / F.qf;
}

/* Kernel side, with the bridge's timer lock held: the frame timer's expiry. */
static LONGLONG due_filter(uint32_t timer_va, LONGLONG rel_100ns)
{
    double natural, grid;
    if (MEM32(timer_va + SLOT_PROC) != FRAME_CB) return 0;
    F.user = MEM32(timer_va + SLOT_USER);
    natural = now_ms() + (double)rel_100ns / 1e4;
    grid = F.next;
    if (F.next <= 0.0 || fabs(natural - grid) > REANCHOR_MS) F.next = natural;
    return (LONGLONG)((F.next - EARLY_MS) * F.qf / 1000.0);
}


void hook_fclock_000B26A0(void)
{
    double t, plan = F.next, fire, ref = 0.0;
    uint32_t user = MEM32(g_esp + 0xCu);       /* dwUser, as the thunk reads it */
    LONG n;
    if (F.on && plan > 0.0 && user == F.user) {
        t = now_ms();
        if (t < plan && plan - t <= EARLY_MS + 2.0) {      /* armed early: wait for the exact instant */
            static __thread HANDLE wt;
            if (!wt) wt = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
            if (wt && plan - t > 1.4) {                    /* most of it asleep, the last ~1.2 ms yielding (a wake is up to ~1 ms late) */
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((plan - 1.2 - t) * 1e4);
                if (due.QuadPart < 0 && SetWaitableTimer(wt, &due, 0, NULL, NULL, FALSE))
                    WaitForSingleObject(wt, 5);
            }
            while ((t = now_ms()) < plan) {
                YieldProcessor();
                if (plan - t > 0.3) Sleep(0);
            }
        }
        fire = t;
        /* this tick's time: its point of the grid (the wait above ends a few
         * microseconds after it), or the actual instant after a stall, from
         * which the grid starts again */
        ref = fire - plan > 0.5 * PERIOD_MS ? fire : plan;
        /* the next point of the grid; far behind (a stall): from now */
        F.next = fire - plan > 0.5 * PERIOD_MS ? fire + PERIOD_MS : plan + PERIOD_MS;
    }
    /* The callback produces its tick now (pad sample, frame event): counted
     * first, then its time published (frame_clock_tick_ref reads in the
     * opposite order). A callback outside the grid publishes none. */
    n = InterlockedIncrement(&F.fires);
    if (ref > 0.0) {
        F.tick_ref = ref;
        MemoryBarrier();
        InterlockedExchange(&F.ref_fire, n);
    }
    sub_000B26A0();
}

int frame_clock_on(void) { return F.on; }

/* Any thread: the time (QPC ms) the tick of the last callback was produced,
 * its point of the grid; 0 when unknown (clock off, the last callback outside
 * the grid, or one in progress). Read in the opposite order of the writes: a
 * callback that counts itself between the reads makes them differ. */
double frame_clock_tick_ref(void)
{
    LONG a, b;
    double r;
    if (!F.on) return 0.0;
    a = F.ref_fire;
    MemoryBarrier();
    r = F.tick_ref;
    MemoryBarrier();
    b = F.fires;
    return a == b ? r : 0.0;
}

void frame_clock_init(void)
{
    const char *e = getenv("XBOX_FRAME_CLOCK");
    F.on = !(e && e[0] == '0');
    F.qf = (double)xbox_kt_qpf();
    if (F.on) {
        g_xbox_ktimer_due_filter = due_filter;
        d3d8_sync_set_frame_clock();
    }
    fprintf(stderr, "[FCLOCK] XBOX_FRAME_CLOCK=%d (%s)\n", F.on,
            F.on ? "the title's frame timer on an exact 60 Hz grid" : "the title's whole-millisecond expiries");
}

void (*frame_clock_lookup(unsigned int xbox_va))(void)
{
    if (F.on && xbox_va == 0x000B26A0u) return hook_fclock_000B26A0;
    return 0;
}
