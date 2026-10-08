#include "xbox_perf.h"
#include <stdlib.h>
/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

#include "xbox_memory_layout.h"
#include "kernel.h"
#include <stdio.h>
#include <string.h>

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120

/* XBE section header layout (56 bytes each) */
#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;  /* actual_base - XBOX_BASE_ADDRESS */
static void *g_gpu_mmio_base = NULL;   /* fake NV2A MMIO aperture, Xbox VA 0xFD000000 */

/*
 * PFIFO "kick and spin" pump.
 *
 * The D3D driver's low-level command-pusher code spins waiting for a real
 * PFIFO command processor to do its job -- one that doesn't exist here
 * (PFIFO is an acknowledged, deliberately unfinished "Phase 1" stub in
 * this tool). Confirmed live via gdb
 * across two distinct spin shapes so far, both hanging off the same GPU
 * channel-context global at Xbox VA 0x1776C0:
 *
 *   1. Two near-identical leaf functions ("kick the pusher") set bit
 *      0x10000 at a fixed hardware register, Xbox VA 0xFD100410, then
 *      spin reading it until real hardware would clear it.
 *   2. A "wait for ring-buffer space" loop compares a PUT value (a plain
 *      field in the context struct, +0x1C) against a GET value read
 *      through a pointer stored at +0x3F0 -- itself a Xbox-heap-allocated
 *      software bookkeeping cell, not hardware, and nothing ever advances
 *      it because there's no real consumer draining the ring.
 *   3. A "wait for GPU fence" loop: the driver increments a target fence
 *      value (context+0x10) and pushes a "set fence" command, then polls
 *      a completion readback -- a pointer to a small status struct at
 *      context+0x2304, whose +0x44 field is where real hardware would
 *      write the fence it has actually reached -- until it matches.
 *      Confirmed the context here is the very same one: the caller passes
 *      the literal constant 0x174B30, which is exactly what gets stored
 *      at Xbox VA 0x1776C0 during device init (see sub_0016B0E0).
 *   4. A per-frame "don't get more than 2 frames ahead of the GPU" throttle
 *      (sub_001688D0): compares a producer count (context+0x2B60, bumped
 *      once per frame submitted) against a consumer count (context+0x2518,
 *      what real hardware would advance as it finishes each frame) and
 *      spins (via a plain ~400-cycle busy-wait stall, sub_0016B210) while
 *      producer-consumer >= 2. Confirmed live via gdb: the main thread
 *      hangs here indefinitely the first time `Application_RunMainLoop`
 *      reaches real rendering (`SceneRenderer_RenderFrame`) -- the frame
 *      counter above +0x2518 never advances because, same as every other
 *      wait here, there's no real GPU behind it counting anything.
 *
 * All three are the same underlying gap wearing different clothes: no
 * command ever actually gets consumed or completed. Modeling "the software
 * GPU processes everything instantly" fixes all three at once -- clear the
 * kick bit, make GET track PUT, and make the fence readback track the
 * fence target, every tick, so any wait built on them resolves within one
 * poll interval.
 *
 * Fixing this at the generated-code call sites would work today but
 * silently stop working the moment this codebase gets regenerated from a
 * fresh seed round (a real, recurring event in this project) or a new call
 * site appears in code a future round translates. Fixing it here instead
 * -- by walking the same context chain the driver itself resolves, not by
 * patching any particular caller -- survives both. This thread only ever
 * touches plain Xbox memory cells (never any of the shared g_eax/g_ebx/
 * g_esi/g_edi/g_esp CPU-register globals, and never calls into any
 * recompiled register-based code), so it's safe to run concurrently with
 * the single synchronous Xbox "thread" the rest of this runtime assumes --
 * there's nothing here to race with it.
 */
#define XBOX_GPU_CONTEXT_PTR_VA   0x001776C0u
#define XBOX_GPU_CTX_PUT_OFF      0x1Cu
#define XBOX_GPU_CTX_GET_PTR_OFF  0x3F0u
#define XBOX_GPU_CTX_FENCE_TARGET_OFF   0x10u
#define XBOX_GPU_CTX_FENCE_STRUCT_OFF   0x2304u
#define XBOX_GPU_FENCE_CURRENT_OFF      0x44u
#define XBOX_GPU_CTX_FRAME_CONSUMER_OFF 0x2518u
#define XBOX_GPU_CTX_FRAME_PRODUCER_OFF 0x2B60u
/* MCPX ACI (AC'97) register file. xemu's hw/xbox/mcpx/aci.c builds a 0x1000
 * MMIO region and aliases the AC'97 Native Audio Bus Master block in at +0x100;
 * each of the four bus-master channels is 16 bytes with its Control Register at
 * +0x0B and Status at +0x06. */
#define XBOX_ACI_BASE_VA          0xFEC00000u
#define XBOX_ACI_NABM_OFF         0x00000100u
#define XBOX_ACI_NABM_CHANNELS    4
#define XBOX_ACI_CHANNEL_STRIDE   0x10u
#define XBOX_ACI_CR_RR            0x02u  /* Reset Registers -- hardware self-clears */
#define XBOX_ACI_CR_DONT_CLEAR    0x1Cu  /* IOCE | FEIE | LVBIE survive a reset */
#define XBOX_ACI_SR_DCH           0x01u  /* DMA Controller Halted */

#define XBOX_PFIFO_KICK_VA        0xFD100410u
#define XBOX_PFIFO_BUSY_BIT       0x00010000u
/* GPU-MMIO-base+0x100, bit 0x1000000: a real busy/ready flag two different
 * call sites poll with *opposite* polarity -- sub_00170385 waits for it to
 * become SET before proceeding, while sub_0016F6B0 (three frames deeper in
 * the same device-init chain) writes a "kick" to +0x600100 and then waits
 * for this same bit to CLEAR (a textbook busy-until-hardware-finishes
 * protocol). Confirmed live via gdb both times that the base resolves to
 * the genuine mapped GPU MMIO aperture (0xFD000000), not a bug in address
 * resolution -- just, as always, no real hardware behind it to ever flip
 * either transition on its own. A first attempt forced the bit permanently
 * SET, which satisfied sub_00170385 but left sub_0016F6B0 blocked forever;
 * toggling it instead (same technique as the ACPI GPIO field-pin bit in
 * xbox_io_port_read) satisfies whichever polarity is actually being waited
 * on within a tick or two, without needing to know the exact real protocol. */
#define XBOX_PGRAPH_READY_VA      0xFD000100u
#define XBOX_PGRAPH_READY_BIT     0x01000000u

/* D3DDevice_BlockUntilVerticalBlank (0x00169070) does
 * KeWaitForSingleObject(D3D_g_pDevice + 0x24F0, ..., Timeout=0/infinite) --
 * a real VBlank event a genuine display interrupt would signal ~60 times a
 * second. D3D_g_pDevice is a fixed global for this title (confirmed
 * throughout this session: Direct3D_CreateDevice sets it to literal
 * 0x174B30), so the event's Xbox VA is a stable constant, not something that
 * needs to be read out of a pointer at runtime. Nothing else in this
 * emulation ever signals it, so without this the game hangs forever the
 * first time it waits for a vblank -- same root cause as every other wait
 * this pump thread already covers, just surfaced through the KE event path
 * instead of a raw PFIFO/PGRAPH memory poll. */
#define XBOX_D3D_DEVICE_VA        0x00174B30u
#define XBOX_VBLANK_EVENT_OFF     0x24F0u
#define XBOX_VBLANK_EVENT_VA      (XBOX_D3D_DEVICE_VA + XBOX_VBLANK_EVENT_OFF)
#define XBOX_VBLANK_PERIOD_TICKS  16  /* ~16ms per tick below -> ~60Hz */

/* Fake per-thread TLS structure backing fs:[0x28]. File scope because the
 * PFIFO pump guards this slot -- see the [TIB] check in the pump loop. */
#define XBOX_FAKE_TLS_VA 0x00760000u
#define XBOX_FAKE_PRCB_VA 0x00770000u

/* ── Per-thread TIB allocation ─────────────────────────────────────────────
 * See the pool comment in xbox_memory_layout.h for why this exists.
 * g_xbox_tib_va is read by xbox_resolve_uncached_alias on every guest memory
 * access below 0x100, so it lives here beside the other per-thread register
 * state and is declared in recomp_types.h. */
__thread uint32_t g_xbox_tib_va = 0;

static volatile LONG g_tib_next_slot = 0;

uint32_t xbox_tib_alloc_for_thread(uint32_t stack_base, uint32_t stack_limit)
{
    LONG slot = InterlockedIncrement(&g_tib_next_slot) - 1;
    uint32_t base_va, kpcr_va, kthread_va, tls_va;
    uint8_t *base;

    if (slot >= XBOX_TIB_MAX) {
        fprintf(stderr, "  [TIB] pool exhausted (%d slots); thread falls back to the "
                        "shared block at VA 0\n", XBOX_TIB_MAX);
        fflush(stderr);
        return 0;
    }

    base_va    = XBOX_TIB_POOL_VA + (uint32_t)slot * XBOX_TIB_SIZE;
    kpcr_va    = base_va + XBOX_TIB_KPCR_OFF;
    kthread_va = base_va + XBOX_TIB_KTHREAD_OFF;
    tls_va     = base_va + XBOX_TIB_TLSDATA_OFF;

    /* Use the same guest->host conversion the MEM* macros use. This used to
     * subtract XBOX_BASE_ADDRESS (0x10000) while the mapping actually starts at
     * XBOX_MAP_START (0), so every TIB was initialised 64 KB below where the
     * guest reads it. The guest therefore saw an all-zero KPCR: fs:[0x28]
     * (PrcbData.CurrentThread) read 0, the CRT thread-start routine computed a
     * TLS destination of Xbox VA 4, and its rep movsd smeared data across the
     * real TIB -- which then broke the NULL circular-list terminator in
     * sub_000A3890 and hung UI_BuildButtonGroup. */
    base = (uint8_t *)g_memory_base + (base_va - XBOX_MAP_START);
    memset(base, 0, XBOX_TIB_SIZE);

    /* ── KPCR ─────────────────────────────────────────────────────────── */
    {
        uint8_t *k = base + XBOX_TIB_KPCR_OFF;
        *(uint32_t *)(k + XBOX_KPCR_SEH_LIST)    = 0xFFFFFFFFu; /* end of chain */
        *(uint32_t *)(k + XBOX_KPCR_TLS_ARRAY)   = tls_va;
        *(uint32_t *)(k + XBOX_KPCR_STACK_LIMIT) = stack_limit;
        *(uint32_t *)(k + XBOX_KPCR_SELF)        = kpcr_va;
        *(uint32_t *)(k + XBOX_KPCR_SELF_PCR)    = kpcr_va;
        /* Prcb points at PrcbData, which is embedded at KPCR+0x28. */
        *(uint32_t *)(k + XBOX_KPCR_PRCB)        = kpcr_va + XBOX_KPCR_PRCB_DATA;
        /* PASSIVE_LEVEL. The CRT reads this directly (`movzx eax, fs:[0x24]`)
         * and bugchecks at >= 2, so it has to be a real value, not a hole
         * that happens to be zero -- see XBOX_NULL_PAGE_VA. */
        *(uint8_t *)(k + XBOX_KPCR_IRQL)         = 0;
        /* PrcbData.CurrentThread -- this is what `mov eax, fs:0x28` reads. */
        *(uint32_t *)(k + XBOX_KPCR_PRCB_DATA)   = kthread_va;
        /* The field SSX's entry-point logic tests; see the note in the header. */
        *(uint32_t *)(k + XBOX_KPCR_PRCB_DATA + XBOX_PRCB_D3D_CACHE_OFF) = 1;
    }

    /* ── KTHREAD ──────────────────────────────────────────────────────── */
    {
        uint8_t *t = base + XBOX_TIB_KTHREAD_OFF;
        *(uint32_t *)(t + XBOX_KTHREAD_STACK_BASE)  = stack_base;
        *(uint32_t *)(t + XBOX_KTHREAD_STACK_LIMIT) = stack_limit;
        *(uint32_t *)(t + XBOX_KTHREAD_TLS_DATA)    = tls_va;
    }

    g_xbox_tib_va = kpcr_va;
    return kpcr_va;
}

static HANDLE           g_pfifo_pump_thread = NULL;
static volatile LONG    g_pfifo_pump_running = 0;
/* Turns of the pump loop; the freeze report (port crashreport.c) says
 * whether the pump still runs. One increment per turn. */
volatile uint32_t       g_pump_beats = 0;

/*
 * Sync one channel's fence readback to whatever it's being asked to reach.
 *
 * `context` isn't always the persistent GPU device object at Xbox VA
 * 0x174B30 -- confirmed live via gdb that sub_0016ED57 (device miniport
 * init) also runs this exact wait shape for context value literal `1`, a
 * small hardcoded channel/device index (from Renderer_InitializeD3DDevice's
 * own `esi = 1;`, not corruption -- traced all the way up before concluding
 * that). For that channel, `context + 0x2304` resolves through a real
 * lookup table at low Xbox memory to a genuine address inside the mapped
 * NV2A MMIO aperture (confirmed: 0xFD800000, well within the
 * 0xFD000000-0xFFFFFFFF range mapped in xbox_MemoryLayoutInit) -- i.e. the
 * *address resolution* here is completely correct; there's just still no
 * real hardware behind it to advance the fence, same root cause as every
 * other wait this pump already handles. Both known channels are synced
 * below; a third would need adding here the same way if one ever turns up.
 */
static void xbox_pfifo_sync_fence(uint8_t *mem_base, uint32_t context)
{
    uint32_t fence_target0  = *(volatile uint32_t *)(mem_base + context);
    uint32_t fence_target10 = *(volatile uint32_t *)(mem_base + context + XBOX_GPU_CTX_FENCE_TARGET_OFF);
    uint32_t fence_struct   = *(volatile uint32_t *)(mem_base + context + XBOX_GPU_CTX_FENCE_STRUCT_OFF);
    if (fence_struct) {
        /* Two call sites wait on this same fence-struct readback against
         * two different target fields -- context+0x10 (sub_0016F220) and
         * context+0x0 (sub_0016ED57). Sync both; the 1ms poll interval
         * means whichever is actually being waited on at any instant sees
         * its own target satisfied on the next tick regardless of which
         * write happened last. */
        volatile uint32_t *current =
            (volatile uint32_t *)(mem_base + fence_struct + XBOX_GPU_FENCE_CURRENT_OFF);
        *current = fence_target10;
        *current = fence_target0;
    }
}

/* Live push-buffer consumer, implemented in src/nv2a/nv2a_live_pb.c.
 * Declared here rather than pulling in the nv2a headers, which drag in the
 * whole D3D8 vtable surface this file has no other use for. */
extern void nv2a_live_pb_tick(uint8_t *mem_base);

/* ── MCPX ACI: AC'97 bus-master reset ─────────────────────
 *
 * DSOUND resets an audio channel the standard AC'97 way: set CR.RR, then spin
 * until the hardware clears it. The title's loop is a tight three-instruction
 * spin (VA 0x0017E9AC, inside sub_0017E97A) that reads the register *once*
 * before looping on the cached byte, so it exits only if RR is already clear
 * when it looks. Our MMIO aperture is ordinary RAM, so the 2 DSOUND wrote
 * stayed 2 and the main game thread never left audio init -- confirmed live by
 * attaching to the idle process and finding thread 1 parked there with the
 * whole DSOUND call chain above it.
 *
 * Model what the hardware does on an RR write, matching QEMU's ac97
 * reset_bm_regs(): clear the bus-master registers, raise DCH in the status
 * word, and keep only the interrupt-enable bits in CR -- which drops RR and
 * lets the spin fall through.
 */
static void xbox_aci_tick(uint8_t *mem_base)
{
    int ch;
    for (ch = 0; ch < XBOX_ACI_NABM_CHANNELS; ch++) {
        uint32_t chan = XBOX_ACI_BASE_VA + XBOX_ACI_NABM_OFF +
                        (uint32_t)ch * XBOX_ACI_CHANNEL_STRIDE;
        volatile uint8_t *cr = (volatile uint8_t *)(mem_base + chan + 0x0B);

        if (!(*cr & XBOX_ACI_CR_RR))
            continue;

        *(volatile uint32_t *)(mem_base + chan + 0x00) = 0;              /* BDBAR */
        *(volatile uint8_t  *)(mem_base + chan + 0x04) = 0;              /* CIV   */
        *(volatile uint8_t  *)(mem_base + chan + 0x05) = 0;              /* LVI   */
        *(volatile uint16_t *)(mem_base + chan + 0x08) = 0;              /* PICB  */
        *(volatile uint8_t  *)(mem_base + chan + 0x0A) = 0;              /* PIV   */
        *(volatile uint8_t  *)(mem_base + chan + 0x06) = XBOX_ACI_SR_DCH;
        *cr = (uint8_t)(*cr & XBOX_ACI_CR_DONT_CLEAR);
    }
}

/* Is a guest address one the pump may dereference?
 *
 * The pump follows pointers the title publishes in guest memory -- the GPU
 * context block, and the GET pointer inside it. Those are guest data, so they
 * can be stale, uninitialised, or (on a code path this port has not brought up
 * yet) simply wrong. Dereferencing one unchecked reads at `mem_base + value`,
 * and the mapping has a real hole in it: the RAM mirror cannot back
 * 0xEC400000-0xFD000000, because the GPU MMIO aperture is fixed at 0xFD000000.
 * A bad pointer there kills the host process outright, in a thread with no
 * relation to the guest code that produced it -- which is exactly how this
 * presented: a SIGSEGV in xbox_pfifo_pump_thread with the real defect three
 * subsystems away.
 *
 * Real hardware cannot fault the host either; the GPU would read whatever that
 * address decodes to. Bounds-checking here models that and, more usefully,
 * names the bad pointer instead of losing it in a crash. */
static int pfifo_addr_ok(uint32_t va, size_t need)
{
    if (!g_memory_size)
        return 0;
    if (va == 0 || va >= g_memory_size)
        return 0;
    return (size_t)va + need <= g_memory_size;
}

static void pfifo_bad_ptr(const char *what, uint32_t va)
{
    static uint32_t seen[8];
    static unsigned n;
    unsigned i;
    for (i = 0; i < n && i < 8; i++)
        if (seen[i] == va)
            return;
    if (n < 8)
        seen[n++] = va;
    fprintf(stderr, "  [PFIFO] ignoring out-of-range %s 0x%08X "
                    "(guest RAM is 0x0-0x%08X)\n",
            what, va, (uint32_t)g_memory_size);
    fflush(stderr);
}

/*
 * Vertical blank at a real 59.94 Hz.
 *
 * The title paces its frames on the vertical-blank event, as a console does.
 * It used to be raised by the pump loop below every 16 iterations -- each a
 * Sleep(1) plus however long that iteration's rendering took -- so the more
 * a scene drew, the slower "60 Hz" ran: in a race the game thread spent half
 * its time waiting for a vblank that arrived at well under 60 Hz, and the
 * frame rate followed. This thread raises it on an absolute schedule from the
 * performance counter instead, independent of the render load. If it ever
 * falls several periods behind (a debugger, a suspended process) it resyncs
 * rather than firing a burst. XBOX_VBLANK_PUMP=1 restores the old pacing.
 */
static int g_vblank_from_pump = 0;

static DWORD WINAPI xbox_vblank_thread(LPVOID param)
{
    LARGE_INTEGER f, t0, now, due;
    HANDLE timer;
    double period;
    uint64_t n = 0;
    (void)param;

    QueryPerformanceFrequency(&f);
    period = (double)f.QuadPart * 1001.0 / 60000.0;         /* NTSC 59.94 Hz */
    /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (0x2): sub-millisecond waits
     * without spinning; older systems fall back to a plain timer. */
    timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    QueryPerformanceCounter(&t0);

    while (g_pfifo_pump_running) {
        double target;
        n++;
        target = (double)t0.QuadPart + period * (double)n;
        QueryPerformanceCounter(&now);
        if ((double)now.QuadPart < target) {
            double wait100ns = (target - (double)now.QuadPart) * 1e7 / (double)f.QuadPart;
            if (timer) {
                due.QuadPart = -(LONGLONG)wait100ns;           /* relative */
                if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                    WaitForSingleObject(timer, INFINITE);
                else
                    Sleep((DWORD)(wait100ns / 1e4));
            } else {
                Sleep((DWORD)(wait100ns / 1e4));
            }
        } else if ((double)now.QuadPart - target > 4.0 * period) {
            t0 = now;                                          /* fell behind: resync */
            n = 0;
        }
        xbox_signal_dispatcher_event(XBOX_VBLANK_EVENT_VA);
    }
    if (timer) CloseHandle(timer);
    return 0;
}

/* Pushbuffer translation time (XBOX_PERF=1). */
static void perf_pb_tick(uint8_t *mem_base)
{
    if (g_perf_on) {
        double t0 = perf_now();
        nv2a_live_pb_tick(mem_base);
        perf_add(PZ_PB, perf_now() - t0);
    } else
        nv2a_live_pb_tick(mem_base);
}

extern int pgraph_d3d11_reports_pending(void);
extern int d3d8_pump_alt_off(void);
static int occ_wait_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_PUMP_OCCWAIT"); on = !(e && e[0] == '0'); }
    return on && !d3d8_pump_alt_off();     /* XBOX_FIX_PUMP_ALT=1: every other frame (identity test) */
}

static DWORD WINAPI xbox_pfifo_pump_thread(LPVOID param)
{
    uint8_t *mem_base = (uint8_t *)param;
    int vblank_tick = 0;
    uint32_t last_put = 0, seen_put = 0;
    int idle = 0;

    while (g_pfifo_pump_running) {
        g_pump_beats++;
        /* Guest .text must not change after load. A jump table at Xbox VA
         * 0x000FB030 (GfxContext_ApplyRenderStateDelta's switch) reads back
         * correct at load and zero by the time the title uses it, and a
         * page write-watch on that address catches nothing -- so the write
         * arrives through some other view of the same file mapping. This
         * pump thread already runs continuously; checking here names the
         * moment it happens instead of only the consequence. */
        {
            static uint32_t jt_last = 0xFFFFFFFFu;
            uint32_t jt_now = *(volatile uint32_t *)(mem_base + 0x000FB030u);
            if (jt_last == 0xFFFFFFFFu) jt_last = jt_now;
            if (jt_now != jt_last) {
                fprintf(stderr, "  [TEXT] .text[0x000FB030] changed 0x%08X -> 0x%08X\n", jt_last, jt_now);
                jt_last = jt_now;
                xbox_VerifyViewIntegrity("after .text change");
                fflush(stderr);
            }
        }
        if (g_vblank_from_pump && ++vblank_tick >= XBOX_VBLANK_PERIOD_TICKS) {
            vblank_tick = 0;
            xbox_signal_dispatcher_event(XBOX_VBLANK_EVENT_VA);
        }

        xbox_aci_tick(mem_base);

        volatile uint32_t *kick = (volatile uint32_t *)(mem_base + XBOX_PFIFO_KICK_VA);
        if (*kick & XBOX_PFIFO_BUSY_BIT) {
            *kick &= ~XBOX_PFIFO_BUSY_BIT;
        }

        {
            volatile uint32_t *ready = (volatile uint32_t *)(mem_base + XBOX_PGRAPH_READY_VA);
            *ready ^= XBOX_PGRAPH_READY_BIT;
        }

        /*
         * Acknowledge only work the translator has actually done.
         *
         * This used to report everything as consumed first -- GET = PUT, every
         * fence reached, frame consumer = producer -- and only then parse the
         * commands. The title's D3D runtime reuses per-draw memory (the
         * stride-0 vertex attributes that carry each object's matrix, dynamic
         * vertex rings, push-buffer segments) as soon as a fence says the GPU
         * is past it, so by the time a draw was translated its matrix slot
         * could already hold another object's: identical draw lists rendered
         * different frames, and character select flickered between the
         * platform and a screen-filling copy of the Select Mode cave.
         *
         * So: sample what the title has published, translate everything up to
         * the write pointer (nv2a_live_pb_tick drains it per tick), then
         * publish the sampled values. Default because,
         * measured over three character-select runs each, acknowledging first
         * left 8-14 corrupt frames per run (shards, a missing cave) and
         * acknowledging last left none. (The first measurement, which found
         * no difference, was masked by the vertex-ring wrap bug in
         * d3d8_device.c.) It costs frame rate -- the title now waits for the
         * CPU-side vertex programs as it would for the GPU -- and
         * XBOX_PFIFO_ACK_LATE=0 restores the old order.
         */
        {
            static int ack_early = -1;
            uint32_t context, put = 0, get_va = 0, producer = 0;
            uint32_t ft0 = 0, ft10 = 0, fstruct = 0;
            if (ack_early < 0) {
                const char *e = getenv("XBOX_PFIFO_ACK_LATE");
                ack_early = (e && e[0] == '0');
            }
            context = *(volatile uint32_t *)(mem_base + XBOX_GPU_CONTEXT_PTR_VA);
            if (context && !pfifo_addr_ok(context, XBOX_GPU_CTX_FRAME_CONSUMER_OFF + 4)) {
                pfifo_bad_ptr("GPU context pointer", context);
                context = 0;
            }
            if (context) {
                put      = *(volatile uint32_t *)(mem_base + context + XBOX_GPU_CTX_PUT_OFF);
                seen_put = put;
                get_va   = *(volatile uint32_t *)(mem_base + context + XBOX_GPU_CTX_GET_PTR_OFF);
                producer = *(volatile uint32_t *)(mem_base + context + XBOX_GPU_CTX_FRAME_PRODUCER_OFF);
                ft0      = *(volatile uint32_t *)(mem_base + context);
                ft10     = *(volatile uint32_t *)(mem_base + context + XBOX_GPU_CTX_FENCE_TARGET_OFF);
                fstruct  = *(volatile uint32_t *)(mem_base + context + XBOX_GPU_CTX_FENCE_STRUCT_OFF);
                if (get_va && !pfifo_addr_ok(get_va, 4)) {
                    pfifo_bad_ptr("GPU GET pointer", get_va);
                    get_va = 0;
                }
            }

            /* Translate first: nv2a_live_pb_tick walks the dwords the title
             * has published since the last tick and dispatches them through
             * the PGRAPH->D3D11 translator. XBOX_LIVE_PB=0 skips it. */
            if (!ack_early)
                perf_pb_tick(mem_base);

            if (context) {
                if (get_va)
                    *(volatile uint32_t *)(mem_base + get_va) = put;
                if (fstruct) {
                    /* Same two targets xbox_pfifo_sync_fence syncs, but the
                     * values sampled before translating. */
                    volatile uint32_t *current =
                        (volatile uint32_t *)(mem_base + fstruct + XBOX_GPU_FENCE_CURRENT_OFF);
                    *current = ft10;
                    *current = ft0;
                }
                /* Fourth channel: per-frame GPU-ahead throttle (see comment
                 * block above, item 4). */
                *(volatile uint32_t *)(mem_base + context + XBOX_GPU_CTX_FRAME_CONSUMER_OFF) = producer;
            }

            /* Second known channel: a small literal index, not a heap/global
             * pointer -- see xbox_pfifo_sync_fence's comment. */
            xbox_pfifo_sync_fence(mem_base, 1);

            if (ack_early)
                perf_pb_tick(mem_base);
        }

        /* While the title is publishing commands, come straight
         * back for more instead of sleeping a millisecond. The title waits
         * on fences the pump acknowledges (D3D_BlockOnTime), and a race
         * frame has several: each paid up to 1-2 ms of Sleep(1) latency,
         * about a tenth of the main thread. Sleep only once idle. */
        if (seen_put != last_put) {
            static int klog = -1;
            if (klog < 0) { const char *e = getenv("XBOX_FLIP_LOG"); klog = e && e[0] == '1'; }
            if (klog) {     /* XBOX_FLIP_LOG=1: every kick, timestamped */
                extern unsigned pgraph_d3d11_draws_pending(void);
                LARGE_INTEGER q, f;
                QueryPerformanceCounter(&q);
                QueryPerformanceFrequency(&f);
                fprintf(stderr, "[KICK] t=%.1f put %08X pending draws %u\n",
                        (double)q.QuadPart * 1000.0 / (double)f.QuadPart, seen_put,
                        pgraph_d3d11_draws_pending());
            }
            last_put = seen_put;
            idle = 0;
            SwitchToThread();
        } else if (++idle < 8) {
            SwitchToThread();
        } else if (occ_wait_on() && pgraph_d3d11_reports_pending()) {
            /* Fork: the title spins on a visibility-test report (LensFX's
             * sun, Render_ReadVisibilityTestResult) until the pump writes it;
             * a Sleep(1) here added ~1-2 ms to each such wait. Poll without
             * sleeping until the report is written -- the same value, only
             * sooner. XBOX_FIX_PUMP_OCCWAIT=0: the old Sleep(1). */
            SwitchToThread();
        } else {
            Sleep(1);
        }
    }
    return 0;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};

/*
 * A mirror slot that something else in the process has already claimed
 * part of cannot take a full view. Rather than leave the whole slot
 * (g_memory_size, e.g. 140 MB) as a hole, the free parts of it get smaller
 * views of the matching part of the section, so only the foreign bytes
 * themselves stay unmapped. Up to this many pieces per slot.
 */
#define XBOX_MIRROR_MAX_PARTS 8
typedef struct {
    void     *view;
    uintptr_t slot_off;   /* offset inside the slot == offset inside the section */
    size_t    len;
} xbox_mirror_part;
static xbox_mirror_part g_mirror_parts[XBOX_NUM_MIRRORS][XBOX_MIRROR_MAX_PARTS];

/* Number of mirror slots below the GPU MMIO aperture (set by the mapping loop). */
static int g_mirror_slots = 0;

/* Address-space reservations held from base selection until each slot is
 * mapped (see xbox_reserve_layout). */
static BOOL g_mirror_reserved[XBOX_NUM_MIRRORS];
static BOOL g_gap_reserved = FALSE;

/*
 * Test hook (XBOX_TEST_MIRROR_BLOCK=<n>[t]): plant a foreign 64 KB
 * allocation in the middle of RAM mirror slot <n> (1-based) right after the
 * base view is chosen, standing in for whatever else in the process
 * occasionally lands in a mirror slot between the layout probe and the
 * mirror mapping (seen live: "Mirror 14: FAILED", roughly one start in ten).
 * A trailing 't' makes the block transient: it is released after the first
 * failed attempt to map that mirror, like a short-lived allocation would be.
 * Off unless the variable is set; never set by the launcher or the tools.
 */
static int   g_test_block_slot = 0;          /* 1-based mirror number, 0 = off */
static int   g_test_block_transient = 0;
static void *g_test_block = NULL;

static void xbox_test_plant_mirror_block(void)
{
    const char *e = getenv("XBOX_TEST_MIRROR_BLOCK");
    int n;
    uintptr_t addr;
    if (!e || !e[0]) return;
    n = atoi(e);
    if (n < 1 || n > XBOX_NUM_MIRRORS) return;
    g_test_block_slot = n;
    g_test_block_transient = (strchr(e, 't') != NULL);
    addr = (uintptr_t)g_memory_base + (uintptr_t)n * g_memory_size
         + ((g_memory_size / 2) & ~(uintptr_t)0xFFFF);
    g_test_block = VirtualAlloc((void *)addr, 0x10000,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    fprintf(stderr, "  TEST: planted %s 64 KB block in mirror slot %d at %p (%s)\n",
            g_test_block_transient ? "transient" : "permanent", n, (void *)addr,
            g_test_block ? "ok" : "FAILED");
}

/*
 * Layout report (XBOX_MEMLAYOUT_LOG=1): every region of native address
 * space from the base view up to the end of the GPU MMIO aperture, as
 * offsets from g_memory_base, plus a read-only check that each mirror slot
 * really aliases the base view. Used to compare layouts between builds.
 */
static int xbox_memlayout_log_on(void)
{
    const char *e = getenv("XBOX_MEMLAYOUT_LOG");
    return e && e[0] == '1';
}

static void xbox_memlayout_check_aliases(uintptr_t gpu_mmio_offset)
{
    int m;
    for (m = 0; m < XBOX_NUM_MIRRORS; m++) {
        uintptr_t slot = (uintptr_t)(m + 1) * g_memory_size;
        uintptr_t off;
        int same = 0, differ = 0, hole = 0;
        if (slot + g_memory_size > gpu_mmio_offset) break;
        /* One 4 KB page every 1 MB of the slot. */
        for (off = 0; off < g_memory_size; off += 0x100000) {
            const uint8_t *b = (const uint8_t *)g_memory_base + off;
            const uint8_t *a = (const uint8_t *)g_memory_base + slot + off;
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(a, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
                mbi.Type != MEM_MAPPED) {
                hole++;
                continue;
            }
            if (memcmp(a, b, 0x1000) == 0) same++; else differ++;
        }
        fprintf(stderr, "  MEMLAYOUT alias: mirror %2d %s (pages same %d, differ %d, not a view %d)\n",
                m + 1, (differ == 0 && hole == 0) ? "OK  " : "BAD ", same, differ, hole);
    }
}

static void xbox_memlayout_dump_regions(void)
{
    uintptr_t base = (uintptr_t)g_memory_base;
    uintptr_t p = base, end = base + 0xFD000000u + 0x03000000u;
    fprintf(stderr, "  MEMLAYOUT base %p\n", g_memory_base);
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t rend;
        if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == 0) break;
        rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (rend > end) rend = end;
        fprintf(stderr, "  MEMLAYOUT +0x%09llX..+0x%09llX %-7s %-7s alloc %s0x%09llX prot 0x%lX\n",
                (unsigned long long)(p - base), (unsigned long long)(rend - base),
                mbi.State == MEM_FREE ? "free" : mbi.State == MEM_RESERVE ? "reserve" : "commit",
                mbi.State == MEM_FREE ? "-" : mbi.Type == MEM_MAPPED ? "mapped" :
                mbi.Type == MEM_IMAGE ? "image" : "private",
                (uintptr_t)mbi.AllocationBase >= base ? "+" : "abs ",
                (unsigned long long)((uintptr_t)mbi.AllocationBase >= base
                    ? (uintptr_t)mbi.AllocationBase - base : (uintptr_t)mbi.AllocationBase),
                (unsigned long)mbi.Protect);
        p = rend;
    }
}

/* Number of mirror slots that fit below the GPU MMIO aperture: slot m
 * (0-based) spans [(m+1)*size, (m+2)*size) from the base view. */
static int xbox_mirror_slot_limit(size_t memory_size, uintptr_t gpu_mmio_offset)
{
    int m;
    for (m = 0; m < XBOX_NUM_MIRRORS; m++)
        if ((uintptr_t)(m + 2) * memory_size > gpu_mmio_offset) break;
    return m;
}

/* Name whatever occupies [start, end), so a failed mirror says what took
 * its place (a module, a mapped file, a heap or thread-stack block...). */
static void xbox_describe_occupants(uintptr_t start, uintptr_t end)
{
    uintptr_t p = start;
    int shown = 0;
    while (p < end && shown < 6) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t rend;
        if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == 0) break;
        rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (rend > end) rend = end;
        if (mbi.State != MEM_FREE) {
            char name[MAX_PATH] = "";
            if (mbi.Type == MEM_IMAGE)
                GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof(name));
            fprintf(stderr, "    occupied: Xbox VA 0x%08llX-0x%08llX by %s %s memory "
                            "(allocation base %p)%s%s\n",
                    (unsigned long long)(p - (uintptr_t)g_memory_offset),
                    (unsigned long long)(rend - (uintptr_t)g_memory_offset),
                    mbi.State == MEM_RESERVE ? "reserved" : "committed",
                    mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : "private",
                    mbi.AllocationBase, name[0] ? " " : "", name);
            shown++;
        }
        p = rend;
    }
}

/* Collect the free, 64 KB-aligned stretches of [start, end) (allocation
 * granularity: views and VirtualAlloc ranges must start on it). */
static int xbox_free_subranges(uintptr_t start, uintptr_t end,
                               uintptr_t *out_start, size_t *out_len, int max)
{
    uintptr_t p = start;
    int n = 0;
    while (p < end && n < max) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t rend;
        if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == 0) break;
        rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (rend > end) rend = end;
        if (mbi.State == MEM_FREE) {
            uintptr_t s = (p + 0xFFFFu) & ~(uintptr_t)0xFFFFu;
            uintptr_t e = rend & ~(uintptr_t)0xFFFFu;
            if (e > s) {
                out_start[n] = s;
                out_len[n] = (size_t)(e - s);
                n++;
            }
        }
        p = rend;
    }
    return n;
}

/*
 * Reserve every mirror slot and the gap above them as soon as the base view
 * is placed. xbox_probe_layout_free only checks that they are free at that
 * moment; the views themselves are mapped much later in
 * xbox_MemoryLayoutInit, after the XBE sections are loaded, and anything
 * else in the process that allocates in between can land inside a slot
 * (seen live: mirror 14 failing about one start in ten, leaving a 140 MB
 * hole). Holding the ranges as plain reservations closes that window; each
 * one is released immediately before its view is mapped onto it. When all
 * of them succeed, the final layout is exactly what it was without them.
 */
static void xbox_reserve_layout(uintptr_t gpu_mmio_offset)
{
    uintptr_t base = (uintptr_t)g_memory_base;
    int m, slots = xbox_mirror_slot_limit(g_memory_size, gpu_mmio_offset);
    uintptr_t gap_start = (uintptr_t)(slots + 1) * g_memory_size;

    for (m = 0; m < slots; m++) {
        void *want = (void *)(base + (uintptr_t)(m + 1) * g_memory_size);
        void *got = VirtualAlloc(want, g_memory_size, MEM_RESERVE, PAGE_NOACCESS);
        if (got && got != want) {
            VirtualFree(got, 0, MEM_RELEASE);
            got = NULL;
        }
        g_mirror_reserved[m] = (got != NULL);
    }
    if (gap_start < gpu_mmio_offset) {
        void *want = (void *)(base + gap_start);
        void *got = VirtualAlloc(want, gpu_mmio_offset - gap_start, MEM_RESERVE, PAGE_READWRITE);
        if (got && got != want) {
            VirtualFree(got, 0, MEM_RELEASE);
            got = NULL;
        }
        g_gap_reserved = (got != NULL);
    }
}

/* Map one full mirror view, retrying a couple of times: the reservation
 * covers the slot only if it was still entirely free at base selection, and
 * a short-lived foreign allocation may be gone a moment later. */
static void *xbox_map_mirror(int m, uintptr_t addr)
{
    int attempt;
    for (attempt = 1; attempt <= 3; attempt++) {
        void *v;
        DWORD err;
        if (g_mirror_reserved[m]) {
            VirtualFree((void *)addr, 0, MEM_RELEASE);
            g_mirror_reserved[m] = FALSE;
        }
        v = MapViewOfFileEx(g_mapping_handle, FILE_MAP_ALL_ACCESS, 0, 0,
                            g_memory_size, (LPVOID)addr);
        if (v) {
            if (attempt > 1)
                fprintf(stderr, "  Mirror %d: mapped at %p on attempt %d\n",
                        m + 1, (void *)addr, attempt);
            return v;
        }
        err = GetLastError();
        fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu), attempt %d of 3\n",
                m + 1, (void *)addr, err, attempt);
        if (attempt == 1)
            xbox_describe_occupants(addr, addr + g_memory_size);
        if (g_test_block && g_test_block_transient && m + 1 == g_test_block_slot) {
            VirtualFree(g_test_block, 0, MEM_RELEASE);
            g_test_block = NULL;
            fprintf(stderr, "  TEST: transient block in mirror slot %d released\n", m + 1);
        }
        Sleep(10u * (DWORD)attempt);
    }
    return NULL;
}

/* Last resort for a slot that cannot take a full view: map the matching
 * part of the section onto each free stretch of it, so only the foreign
 * bytes stay unmapped instead of the whole slot. Returns bytes covered. */
static size_t xbox_map_mirror_parts(int m, uintptr_t addr)
{
    uintptr_t s[XBOX_MIRROR_MAX_PARTS];
    size_t l[XBOX_MIRROR_MAX_PARTS];
    size_t covered = 0;
    int i, k = 0;
    int n = xbox_free_subranges(addr, addr + g_memory_size, s, l, XBOX_MIRROR_MAX_PARTS);

    for (i = 0; i < n; i++) {
        uintptr_t off = s[i] - addr;
        void *v = MapViewOfFileEx(g_mapping_handle, FILE_MAP_ALL_ACCESS, 0, (DWORD)off,
                                  l[i], (LPVOID)s[i]);
        if (v == (void *)s[i]) {
            g_mirror_parts[m][k].view = v;
            g_mirror_parts[m][k].slot_off = off;
            g_mirror_parts[m][k].len = l[i];
            k++;
            covered += l[i];
        } else if (v) {
            UnmapViewOfFile(v);
        }
    }
    fprintf(stderr, "  Mirror %d: partial -- %d view(s) cover %u of %u KB of the slot "
                    "(Xbox VA 0x%08llX-0x%08llX); the rest stays unmapped\n",
            m + 1, k, (unsigned)(covered / 1024), (unsigned)(g_memory_size / 1024),
            (unsigned long long)(addr - (uintptr_t)g_memory_offset),
            (unsigned long long)(addr + g_memory_size - (uintptr_t)g_memory_offset));
    return covered;
}

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Global registers for recompiled code (via recomp_types.h).
 * Thread-local: with real concurrent Xbox threads (see bridge_PsCreateSystemThreadEx
 * in kernel_bridge.c), each genuine OS thread needs its own independent x86 register
 * state and stack pointer, exactly like each real Xbox hardware thread would. Declaring
 * these __thread gives every CreateThread-spawned thread its own copy
 * automatically, with zero changes needed to the ~300K lines of generated recomp code
 * that reference them by plain name -- the whole point of them being globals in the
 * first place. Safe for threads CreateThread'd after this EXE's own TLS is initialized
 * (the historical DLL-TLS caveat with __thread doesn't apply to a statically
 * linked EXE's own threads). */
__thread uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
__thread int g_recomp_cf_in = 0;  /* carry across a lifted function boundary */
__thread uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;

/* SEH frame pointer bridge (see recomp_types.h for explanation) -- thread-local for the
 * same reason as the registers above. */
__thread uint32_t g_seh_ebp = 0;

/* CRT_ftol_TruncateToInt64's FPU-stack cross-call bridge (see recomp_types.h for the
 * full explanation) -- thread-local for the same reason as the registers above. */
__thread double g_ftol_arg = 0.0;

/* Shared FPU stack for the CRT_ftol_TruncateToInt64 fragment chain (see recomp_types.h
 * for the full explanation) -- thread-local for the same reason as the registers above. */
__thread double g_ftol_fp_stack[8] = {0};
__thread int g_ftol_fp_top = 0;

/* Shadow of real x87 ST(0), mirrored by every generated fp_push (see recomp_types.h). */
__thread double g_x87_st0 = 0.0;
__thread int g_fpu_cmp = 0;
__thread int g_str_ne = 0;
__thread uint16_t g_x87_cw = 0x027F;

/* XBOX_X87_COMPAT (diagnostic): bit 0 restores the old `fnstsw ax`
 * (AH only), bit 1 the old no-op `fxam`. Read once, lazily. */
int g_x87_compat = -1;
int g_x87_status_lo = 0;
int x87_compat_init(void)
{
    const char *e = getenv("XBOX_X87_COMPAT");
    g_x87_compat = e ? atoi(e) : 0;
    e = getenv("XBOX_X87_STATUS_LO");
    /* PE (0x20) is sticky on hardware and set by almost every inexact result;
     * the frontend depends on it: AL = 0 blanked the title art and 3D menus. */
    g_x87_status_lo = e ? (int)strtol(e, NULL, 0) : 0x20;
    return g_x87_compat;
}

__thread double g_fp_stack[8];
__thread int g_fp_top = 0;

/* XBOX_X87_RET: how a float-returning call hands ST(0) to its
 * caller -- see x87_ret in recomp_types.h. */
int g_x87_ret_mode = -1;
static uint32_t g_x87_ret_lo, g_x87_ret_hi = 0xFFFFFFFFu;
int x87_ret_init(void)
{
    const char *e = getenv("XBOX_X87_RET");
    /* Default 2: the callee's real ST(0). 0 is the old hand-off
     * that passed pi out of every angle wrap and inverted the race camera. */
    g_x87_ret_mode = e ? (int)strtol(e, NULL, 0) : 2;
    e = getenv("XBOX_X87_RET_SITES");
    if (e) {
        char *end;
        g_x87_ret_lo = (uint32_t)strtoul(e, &end, 0);
        g_x87_ret_hi = *end == '-' ? (uint32_t)strtoul(end + 1, NULL, 0) : g_x87_ret_lo;
        fprintf(stderr, "[X87RET] mode 0x%X only at sites 0x%08X-0x%08X\n",
                g_x87_ret_mode, g_x87_ret_lo, g_x87_ret_hi);
    }
    return g_x87_ret_mode;
}

static void x87_ret_note(uint32_t site, int left, double top, double st0);

/* Slow path of x87_ret (recomp_types.h): logging and site bisection. */
int x87_ret_diag(uint32_t site, int left, int mode)
{
    if (mode & 0x100)
        x87_ret_note(site, left, g_fp_stack[g_fp_top & 7], g_x87_st0);
    if ((mode & 0x200) && (site < g_x87_ret_lo || site > g_x87_ret_hi))
        return 0;
    return mode & 0xFF;
}

/* One line per (site, values left, top == last push) the first time it is
 * seen: which callees leave their result on the stack, and which do not. */
static void x87_ret_note(uint32_t site, int left, double top, double st0)
{
    static struct { uint32_t site; uint8_t seen; } tab[2048];
    static int logged;
    int b = left < 0 ? 0 : left > 2 ? 3 : left + 1;   /* <0, 0, 1, >1 */
    int same = (top == st0) || (top != top && st0 != st0);
    uint8_t bit = (uint8_t)(1u << (b * 2 + same));
    uint32_t h = (site * 2654435761u) >> 21;
    for (int i = 0; i < 2048; i++, h = (h + 1) & 2047) {
        if (tab[h].site == site || tab[h].site == 0) {
            tab[h].site = site;
            if (tab[h].seen & bit) return;
            tab[h].seen |= bit;
            break;
        }
    }
    if (logged++ < 4000)
        fprintf(stderr, "[X87RET] site=0x%08X left=%d top=%g last_push=%g%s\n",
                site, left, top, st0, same ? "" : " DIFF");
}

/* ICALL trace ring buffer */
volatile uint32_t g_icall_trace[16] = {0};
volatile uint32_t g_icall_trace_idx = 0;
volatile uint64_t g_icall_count = 0;

/* Forward declaration: definition is with the rest of the heap allocator further
 * down, but it needs to be initialized from xbox_MemoryLayoutInit below, while
 * still single-threaded -- see xbox_HeapAlloc for why this needs a lock at all. */
static CRITICAL_SECTION g_heap_cs;

/*
 * Check whether every mirror-view slot and the GPU MMIO aperture would be
 * free at this candidate base, before we commit to it.
 *
 * The base-address selection below used to validate only the base view's
 * own address, then map mirrors and the GPU MMIO aperture at fixed offsets
 * from it (base + (m+1)*g_memory_size, base + 0xFD000000) without checking
 * first whether those specific native addresses were actually available.
 * That's fine until something else in the process already occupies one of
 * them -- confirmed live: mirror 14 consistently fails to map at native
 * address 0x7A810000 (error 487, ERROR_INVALID_ADDRESS), and this binary
 * has no ASLR, so the same collision happens on every run. The mapping
 * loop already tolerates a failed mirror and continues, but that just
 * turns the failure into a silent hole in Xbox VA space -- exactly the
 * gap the game later read through and segfaulted on (Xbox VA 0x7F800000,
 * squarely inside mirror 14's unmapped range). Probing every slot up
 * front lets us reject a bad base candidate and fall through to the next
 * one in try_bases[], instead of discovering the hole only when the game
 * happens to read through it at runtime.
 */
static BOOL xbox_probe_layout_free(uintptr_t base, size_t memory_size)
{
    const uintptr_t gpu_mmio_offset = 0xFD000000u;
    const uintptr_t gpu_mmio_size = 0x03000000u;
    MEMORY_BASIC_INFORMATION mbi;

    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        uintptr_t off = (uintptr_t)(m + 1) * memory_size;
        if (off + memory_size > gpu_mmio_offset) break;
        uintptr_t addr = base + off;
        if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0) return FALSE;
        if (mbi.State != MEM_FREE || mbi.RegionSize < memory_size) return FALSE;
    }

    uintptr_t gpu_addr = base + gpu_mmio_offset;
    if (VirtualQuery((LPCVOID)gpu_addr, &mbi, sizeof(mbi)) == 0) return FALSE;
    if (mbi.State != MEM_FREE || mbi.RegionSize < gpu_mmio_size) return FALSE;

    return TRUE;
}

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }

    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Map the full 64MB Xbox address space (covers all sections + stack + heap) */
    g_memory_size = XBOX_TOTAL_RAM;

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_memory_size,   /* low DWORD of size (64 MB) */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                GetLastError());
        return FALSE;
    }

    /*
     * Map the base view at the desired virtual address.
     * Try the original Xbox base address first. If that fails (common on
     * Windows 11 where low addresses are often reserved), try page-aligned
     * addresses upward until we find a free region.
     */
    {
        static const uintptr_t try_bases[] = {
            XBOX_BASE_ADDRESS,      /* 0x00010000 - original Xbox address */
            0x00800000,             /* 8 MB - above typical PEB/TEB region */
            0x01000000,             /* 16 MB */
            0x02000000,             /* 32 MB */
            0x10000000,             /* 256 MB */
        };
        const int num_try_bases = (int)(sizeof(try_bases) / sizeof(try_bases[0]));

        /* The fixed candidates above are all within 256 MB of each other,
         * which is far smaller than a single mirror slot (g_memory_size,
         * e.g. 140 MB) -- so if something else in the process occupies a
         * given mirror's native address for one candidate, it very likely
         * still does for the others too (confirmed live: all 5 rejected
         * together by xbox_probe_layout_free once XBOX_TOTAL_RAM grew
         * large enough to reach into a permanently-occupied region, since
         * this binary has no ASLR). Once the fixed list is exhausted,
         * search further out in much bigger strides -- a full mirror-slot
         * width -- so each attempt actually lands somewhere new instead of
         * re-probing the same occupied neighborhood. */
        int found = 0;

        /* Test hook (XBOX_TEST_BASE=<hex address>): try this base first,
         * through the same probe as the candidates below. Lets a layout test
         * pick a base whose mirror slots are all free (e.g. one above the
         * fixed system pages at native 0x7FFE0000, which every lower
         * candidate's slot range covers). Off unless the variable is set. */
        {
            const char *e = getenv("XBOX_TEST_BASE");
            uintptr_t hint_addr = e ? (uintptr_t)strtoull(e, NULL, 16) : 0;
            if (hint_addr) {
                if (xbox_probe_layout_free(hint_addr, g_memory_size)) {
                    g_memory_base = MapViewOfFileEx(g_mapping_handle, FILE_MAP_ALL_ACCESS,
                                                    0, 0, g_memory_size, (LPVOID)hint_addr);
                    if (g_memory_base && (uintptr_t)g_memory_base != hint_addr) {
                        UnmapViewOfFile(g_memory_base);
                        g_memory_base = NULL;
                    }
                    found = (g_memory_base != NULL);
                }
                fprintf(stderr, "  TEST: base candidate 0x%p %s\n", (void *)hint_addr,
                        found ? "used" : "rejected");
            }
        }

        for (int i = 0; i < num_try_bases && !found; i++) {
            uintptr_t hint_addr = try_bases[i];
            if (!xbox_probe_layout_free(hint_addr, g_memory_size)) {
                fprintf(stderr, "  Base candidate 0x%p: skipped -- a mirror slot or the "
                        "GPU MMIO aperture would collide with something already in this "
                        "process's address space\n", (void *)hint_addr);
                continue;
            }
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,           /* offset into mapping */
                g_memory_size,  /* size */
                (LPVOID)hint_addr
            );
            if (g_memory_base) {
                if ((uintptr_t)g_memory_base != hint_addr) {
                    /* OS gave us a different address, retry */
                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                found = 1;
                break;
            }
        }

        if (!found) {
            for (uintptr_t hint_addr = 0x20000000u; hint_addr < 0x60000000u;
                 hint_addr += g_memory_size) {
                if (!xbox_probe_layout_free(hint_addr, g_memory_size)) continue;
                g_memory_base = MapViewOfFileEx(
                    g_mapping_handle, FILE_MAP_ALL_ACCESS, 0, 0,
                    g_memory_size, (LPVOID)hint_addr
                );
                if (g_memory_base) {
                    if ((uintptr_t)g_memory_base != hint_addr) {
                        UnmapViewOfFile(g_memory_base);
                        g_memory_base = NULL;
                        continue;
                    }
                    fprintf(stderr, "  Base candidate 0x%p: found via extended search\n",
                            (void *)hint_addr);
                    found = 1;
                    break;
                }
            }
        }

        if (!found) {
            /* Last resort: let the OS choose. We can't pre-validate the
             * downstream mirror/GPU-MMIO layout in this case, so a later
             * mirror or the GPU MMIO aperture may still fail to map --
             * but an unvalidated attempt beats refusing to start at all. */
            fprintf(stderr, "  No validated base candidate found -- letting the OS "
                    "choose (mirror/GPU-MMIO layout may still collide)\n");
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle, FILE_MAP_ALL_ACCESS, 0, 0, g_memory_size, NULL
            );
        }
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

    xbox_test_plant_mirror_block();

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;

    xbox_reserve_layout(0xFD000000u);

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /*
     * Copy XBE header to base address.
     * The Xbox kernel maps the XBE image header at 0x00010000.
     * Game code reads kernel thunk table, certificate data, and
     * section info from this region.
     */
    {
        /* XBE header size is at file offset 0x0108 (SizeOfImageHeader) */
        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;  /* fallback: 4KB */
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    /*
     * Dynamically load ALL XBE sections by parsing the section headers.
     *
     * This replaces the old approach of hardcoding section addresses for
     * a specific game (Burnout 3). By reading the section table from the
     * XBE header, any game's sections are loaded automatically.
     *
     * Every section is copied to its original Xbox VA:
     * - .text: needed because memory walkers may scan code pages
     * - .rdata: constants, vtables, kernel thunk table
     * - .data: global variables (initialized portion from XBE, BSS zeroed)
     * - XDK library sections (D3D, DSOUND, WMADEC, XPP, etc.)
     * - DOLBY, BINK, XTIMAGE, etc.
     */
    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;
        size_t total_bytes = 0;

        if (num_sections > 64) num_sections = 64;  /* sanity cap */

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            /* Read section name from XBE header */
            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            /* Validate: section must fit within our 64MB mapped region */
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            /* Determine copy size (raw_size may exceed vsize due to alignment) */
            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            /* Zero the full virtual size first (handles BSS) */
            memset(XBOX_VA(sec_va), 0, sec_vsize);

            /* Copy initialized data from XBE */
            if (copy_size > 0 && sec_raw_off + copy_size <= xbe_size) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            }

            sections_loaded++;
            total_bytes += copy_size;

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);

        /* Read back a known .text dword. GfxContext_ApplyRenderStateDelta
         * switches through a jump table at Xbox VA 0x000FB030 whose first
         * entry is 0x000FAC08 in the file; when that reads back as 0 the
         * switch tail-jumps to a null target -- a silently dropped call
         * rather than a fault. This separates "never loaded" from "loaded
         * and then overwritten". */
        fprintf(stderr, "  .text readback: [0x000FB030] = 0x%08X (expect 0x000FAC08)"
                        " at native %p\n",
                *(uint32_t *)XBOX_VA(0x000FB030u), (void *)XBOX_VA(0x000FB030u));
    }

    /*
     * Parse the kernel thunk table address from the XBE header.
     * The XBE stores KernelImageThunkAddress at offset 0x0158, XOR-encrypted.
     * The key differs between retail and debug XBEs, and there is no flag
     * saying which was used -- decode with both and keep whichever lands in
     * the mapped address range (this is what tools/xbe_parser does).
     *
     * Debug XBEs are not an edge case here: they are the builds most worth
     * recompiling, since they still carry assert strings and symbols. Halo's
     * cachebeta.xbe is one, and assuming the retail key decoded its thunk
     * table to 0xB4F98174 instead of 0x00253090, which silently fell back to
     * the compile-time default and resolved 0 of 378 kernel imports.
     */
    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;  /* retail XOR key */
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;  /* debug XOR key  */
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        /* Validate: thunk VA should be within our mapped region */
        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {
            /* Count thunk entries by scanning until we hit 0 */
            uint32_t thunk_count = 0;
            /* XBOX_KERNEL_THUNK_TABLE_SIZE, not 366: the kernel exports 378
             * slots, and kernel.h notes 366 is short by 12. A title importing
             * a high ordinal would have had its table truncated here. */
            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA

    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    xbox_VerifyViewIntegrity("after base map");

    /* Still single-threaded here -- safe, race-free place to initialize the
     * heap allocator's critical section (see xbox_HeapAlloc). */
    InitializeCriticalSection(&g_heap_cs);

    /*
     * Initialize the Xbox stack for recompiled code.
     * The stack area lives at XBOX_STACK_BASE in Xbox address space.
     * g_esp is the global stack pointer shared by all translated functions.
     */
    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    /*
     * Populate the fake Thread Information Block (TIB) at Xbox VA 0x0.
     *
     * The original Xbox code uses fs:[offset] to read per-thread data,
     * but the recompiler drops the fs: segment prefix and generates
     * MEM32(offset) instead. Since we mapped low memory (0x0-0xFFFF),
     * we populate the TIB fields that game code accesses:
     *
     *   fs:[0x00] = SEH exception list (-1 = end of chain)
     *   fs:[0x04] = stack base (top of stack)
     *   fs:[0x08] = stack limit (bottom of stack)
     *   fs:[0x18] = self pointer (TIB address)
     *   fs:[0x20] = KPCR Prcb pointer (→ fake structure)
     *   fs:[0x28] = TLS / RW engine context pointer
     *
     * We use free space in the BSS area for the fake structures.
     */
    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        /* Fake TIB at address 0x0 */
        MEM32_INIT(0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        MEM32_INIT(0x04, XBOX_STACK_TOP);   /* Stack base (high address) */
        MEM32_INIT(0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        MEM32_INIT(0x18, 0x00000000);       /* Self pointer (TIB at VA 0) */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure.
         *
         * This used to be hardcoded to 0 unconditionally, forcing every game's
         * read of [fs:[0x20]+0x250] to see 0 and take whatever "cache doesn't
         * exist yet" branch follows. That's fine for Burnout 3 (its code
         * tolerates/expects the skip), but traced on SSX Tricky this exact
         * zero forces its real entry-point logic (sub_001541A9) down a
         * near-empty branch (calls a function that does nothing but `ecx=0;
         * return`) instead of the substantial branch that leads directly into
         * its actual application/game initialization
         * (Application_ConstructAndInitInput). On real Xbox hardware the Prcb
         * always exists and this D3D-cache-style field is non-zero once a
         * device is up -- 0 was never really "correct" for every game, just
         * convenient for the one game this runtime was originally written
         * against.
         *
         * Fix: give fs:[0x20] a real (fake, but non-null) Prcb-shaped buffer
         * with a non-zero placeholder at +0x250, so games whose real logic
         * depends on this field being populated take their real "cache
         * exists" path instead of a bootstrap-only skip branch. Burnout 3's
         * own code was never shown to depend on this field being exactly 0
         * (only tested against 0 vs. non-0), so this should be strictly
         * more correct for both games, not a trade-off between them.
         */
        #define FAKE_PRCB_VA XBOX_FAKE_PRCB_VA
        MEM32_INIT(0x20, FAKE_PRCB_VA);
        MEM32_INIT(FAKE_PRCB_VA + 0x250, 0x00000001);  /* non-zero D3D-cache-style field */
        #undef FAKE_PRCB_VA

        /*
         * fs:[0x28] - Thread local storage / RW engine context.
         * The RW engine reads [fs:[0x28] + 0x28] to get a pointer
         * to its data area. We allocate a fake structure at 0x00760000
         * (in the BSS area) and a data buffer at 0x00700000.
         */
        #define FAKE_TLS_VA     XBOX_FAKE_TLS_VA
        #define FAKE_RWDATA_VA  0x00700000  /* RW engine data area (in BSS) */

        MEM32_INIT(0x28, FAKE_TLS_VA);
        /* TLS[0x28] = pointer to RW data area */
        MEM32_INIT(FAKE_TLS_VA + 0x28, FAKE_RWDATA_VA);

        fprintf(stderr, "  TIB: fake TIB at VA 0x0, TLS at 0x%08X, RW data at 0x%08X\n",
                FAKE_TLS_VA, FAKE_RWDATA_VA);

        /* The block above at VA 0 stays as a fallback for any thread that
         * never claims a slot. The main thread claims slot 0 here, so from
         * this point on `fs:[n]` accesses resolve to per-thread storage. */
        {
            uint32_t tib = xbox_tib_alloc_for_thread(XBOX_STACK_TOP, XBOX_STACK_BASE);
            fprintf(stderr, "  TIB: per-thread pool at 0x%08X (%d slots); main thread -> 0x%08X\n",
                    XBOX_TIB_POOL_VA, XBOX_TIB_MAX, tib);
        }

        #undef FAKE_TLS_VA
        #undef FAKE_RWDATA_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * Write the synthetic Xbox-kernel PE header at XBOX_FAKE_KERNEL_HEADER_VA.
     *
     * RenderWare's Xbox driver code (xbcache.c) reads MEM32(0x8001003C)
     * to parse the Xbox kernel's PE header and find the INIT section for
     * CPU cache line sizing. On PC, we provide a minimal fake PE header
     * with 0 sections so the function gracefully skips the cache init.
     *
     * recomp_types.h's xbox_resolve_uncached_alias redirects Xbox VA
     * 0x80010000-0x80010FFF to XBOX_FAKE_KERNEL_HEADER_VA, which is
     * ordinary Xbox address space already backed by the base RAM mapping
     * above -- no separate native allocation needed, so this can't collide
     * with a RAM-wrap mirror view the way a fixed-native-address
     * VirtualAlloc at base+0x80010000 used to (confirmed live: that
     * collided with the mirror covering that same relative offset once
     * XBOX_TOTAL_RAM grew large enough to reach it).
     */
    {
        uint8_t *fake_header = (uint8_t *)g_memory_base + XBOX_FAKE_KERNEL_HEADER_VA;
        /* Zero-fill then set e_lfanew = 0x80 (offset to PE header).
         * With the rest zeroed, NumberOfSections = 0 and the INIT
         * section search finds nothing, which is the safe path. */
        memset(fake_header, 0, XBOX_FAKE_KERNEL_HEADER_SIZE);
        *(uint32_t *)(fake_header + 0x3C) = 0x80;  /* e_lfanew */
        fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x80010000 (redirected to VA 0x%08X)\n",
                XBOX_FAKE_KERNEL_HEADER_VA);
    }

    /*
     * Null-pointer absorption page. Everything below Xbox VA 0x100 is
     * redirected here rather than faulting; see XBOX_NULL_PAGE_VA in the
     * header for why it is a page of its own and not, as it was until part
     * 146, the calling thread's own KPCR.
     */
    {
        uint8_t *null_page = (uint8_t *)g_memory_base + XBOX_NULL_PAGE_VA;
        memset(null_page, 0, XBOX_NULL_PAGE_SIZE);
        fprintf(stderr, "  Kernel: null-pointer page at Xbox VA 0x%08X "
                        "(absorbs accesses below 0x100)\n",
                XBOX_NULL_PAGE_VA);
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            XBOX_HEAP_SIZE / (1024 * 1024), XBOX_HEAP_BASE,
            XBOX_HEAP_BASE + XBOX_HEAP_SIZE);

    /*
     * Map mirror views of the 64 MB region.
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        /* The GPU MMIO aperture below is mapped at a *fixed* native address,
         * g_memory_base + 0xFD000000 (48 MB) -- it has to be exactly there
         * since recompiled code dereferences physical GPU register addresses
         * directly and the offset from Xbox VA to native pointer must be 0
         * for that range. Each mirror view claims [(m+1)*g_memory_size,
         * (m+2)*g_memory_size) of address space relative to g_memory_base;
         * once XBOX_TOTAL_RAM grows large enough, the high mirrors start
         * reaching into that fixed 0xFD000000 range and MapViewOfFileEx
         * silently claims part of it, so the later GPU MMIO VirtualAlloc at
         * the same native address fails (confirmed live: raising
         * XBOX_TOTAL_RAM from 128 to 140 MB made mirror 28's tail overlap
         * the GPU MMIO region's first ~12 MB, breaking the GPU register
         * aperture and turning the game's own direct register probe at
         * Xbox VA 0xFED00000 into a genuine, unmapped-memory segfault).
         * Stop
         * mapping mirrors before they'd reach that boundary so this stays
         * safe for any heap size, instead of hardcoding a mirror count that
         * happens to work for one specific XBOX_TOTAL_RAM. */
        const uintptr_t gpu_mmio_offset = 0xFD000000u;
        int mirrors_ok = 0, mirrors_partial = 0;
        char incomplete[128] = "";
        int m;
        for (m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_offset = (uintptr_t)(m + 1) * g_memory_size;
            if (mirror_offset + g_memory_size > gpu_mmio_offset) {
                fprintf(stderr, "  RAM mirror: stopping at %d/%d views -- further mirrors "
                        "would overlap the fixed GPU MMIO aperture at Xbox VA 0x%08X\n",
                        m, XBOX_NUM_MIRRORS, (unsigned)gpu_mmio_offset);
                break;
            }
            uintptr_t mirror_base = (uintptr_t)g_memory_base + mirror_offset;
            g_mirror_views[m] = xbox_map_mirror(m, mirror_base);
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                size_t len = strlen(incomplete);
                if (xbox_map_mirror_parts(m, mirror_base)) mirrors_partial++;
                if (len + 4 < sizeof(incomplete))
                    snprintf(incomplete + len, sizeof(incomplete) - len, " %d", m + 1);
            }
        }
        g_mirror_slots = m;
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, m,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
        if (mirrors_ok != m)
            fprintf(stderr, "  RAM mirror: INCOMPLETE -- mirror(s)%s not fully mapped "
                    "(%d of them partially); see the lines above\n",
                    incomplete, mirrors_partial);

        /*
         * Back the leftover gap between where mirrors had to stop (to avoid
         * the GPU MMIO collision above) and the GPU MMIO aperture itself
         * with real, zeroed memory, the same way the aperture below is
         * backed -- otherwise it's a real hole in Xbox VA space. Confirmed
         * live this gap isn't just theoretical: RenderWare's memory-wrap
         * walker (see the mirror comment above -- it deliberately reads
         * past the nominal RAM size during its extended walk) read from
         * Xbox VA 0xF5000000, exactly the first byte of this gap at
         * XBOX_TOTAL_RAM=140 MB, and segfaulted on it in both the main
         * thread and a worker thread simultaneously. This gap's size depends on XBOX_TOTAL_RAM (a
         * larger heap leaves a smaller gap here, since mirrors reach
         * further before hitting the cap), so it's computed, not hardcoded.
         *
         * The gap starts right after the last mirror slot the loop above
         * walked through (m slots), whether or not each of those slots got
         * its view. It used to start after "mirrors_ok" slots instead, which
         * is the same number only when every mirror maps: with one mirror
         * missing in the middle, the gap started one slot too low, collided
         * with the last mirror's view, failed to back anything, and left the
         * whole real gap (0xF5000000-0xFD000000 at 140 MB) unmapped on top of
         * the missing mirror's own 140 MB hole.
         */
        {
            uintptr_t gap_start = (uintptr_t)(m + 1) * g_memory_size;
            if (gap_start < gpu_mmio_offset) {
                uintptr_t gap_size = gpu_mmio_offset - gap_start;
                void *gap_native = (void *)((uintptr_t)g_memory_base + gap_start);
                void *gap_mapped = VirtualAlloc(gap_native, gap_size,
                                                 g_gap_reserved ? MEM_COMMIT
                                                                : (MEM_COMMIT | MEM_RESERVE),
                                                 PAGE_READWRITE);
                if (gap_mapped && gap_mapped == gap_native) {
                    fprintf(stderr, "  RAM mirror gap: backed %d MB at Xbox VA 0x%08X-0x%08X "
                            "with zeroed memory (absorbs reads/writes past the last mirror)\n",
                            (int)(gap_size / (1024 * 1024)), (unsigned)gap_start, (unsigned)gpu_mmio_offset);
                } else {
                    /* Something else already holds part of the gap: back
                     * every free stretch of it instead of none of it. */
                    uintptr_t s[XBOX_MIRROR_MAX_PARTS];
                    size_t l[XBOX_MIRROR_MAX_PARTS], backed = 0;
                    int i, n;
                    fprintf(stderr, "  RAM mirror gap: FAILED to back 0x%08X-0x%08X (error %lu) "
                            "in one piece\n",
                            (unsigned)gap_start, (unsigned)gpu_mmio_offset, GetLastError());
                    xbox_describe_occupants((uintptr_t)gap_native, (uintptr_t)gap_native + gap_size);
                    n = xbox_free_subranges((uintptr_t)gap_native, (uintptr_t)gap_native + gap_size,
                                            s, l, XBOX_MIRROR_MAX_PARTS);
                    for (i = 0; i < n; i++)
                        if (VirtualAlloc((void *)s[i], l[i], MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE) == (void *)s[i])
                            backed += l[i];
                    fprintf(stderr, "  RAM mirror gap: backed %u of %u KB in %d piece(s); "
                            "reads/writes in the rest will still fault\n",
                            (unsigned)(backed / 1024), (unsigned)(gap_size / 1024), n);
                }
            }
        }
        if (xbox_memlayout_log_on())
            xbox_memlayout_check_aliases(gpu_mmio_offset);
    }

    /*
     * The Xbox "uncached" RAM alias at 0x80000000 (real hardware maps the
     * same physical RAM there a second time, cache-bypassed, for
     * GPU-visible driver writes -- confirmed genuine by disassembling real
     * D3D8 driver bytes doing `mov dword ptr [0x80000000], edx` / `wbinvd`)
     * is handled at the XBOX_PTR macro level in recomp_types.h instead of
     * with a second OS-level memory mapping here. A fixed native address
     * of base+0x80000000 is fragile -- it can collide with whatever else
     * Windows/ASLR already placed in the process's address space and then
     * fails unpredictably per-machine (observed directly: it collided on
     * this machine). Masking the top bit off in XBOX_PTR before
     * translation achieves the identical result (Xbox VA 0x80000000+k
     * reads/writes the same memory as Xbox VA k) with no dependency on OS
     * memory layout at all.
     */

    /*
     * Back the Xbox VA range 0xFD000000-0xFFFFFFFF (48 MB) with real,
     * zeroed memory: this is the NV2A GPU's MMIO register aperture
     * (0xFD000000-0xFE000000) plus the rest of the top-of-address-space
     * hardware/chipset MMIO region some drivers probe past it (confirmed
     * live: a probe at 0xFED00000 faulted here after the first 16 MB of
     * this range alone had already been mapped). Some games probe these
     * registers directly (see the EXCEPTION_ACCESS_VIOLATION handler in
     * main.c, which has a special case for this exact range). That
     * handler's range check compares against the *native* fault address,
     * which is xbox_va + g_memory_offset -- for this range that's always
     * well past 4 GB (0xFD000000 + a few hundred MB), i.e. never equal to
     * the raw 0xFD000000-0xFE000000 constants it's checking against, so
     * it can never actually match. Also, even if it did match, that
     * handler just returns EXCEPTION_CONTINUE_SEARCH with no actual
     * instruction-skipping implemented (an explicit TODO there), which
     * would not resume execution either. Mapping this range so it simply
     * doesn't fault at all sidesteps both problems: reads see zeros (a
     * software GPU has nothing meaningful to report anyway) and writes are
     * silently absorbed, exactly like the existing GPU-probe handler's
     * intent, but reliably.
     */
    {
        void *gpu_mmio_base = (void *)((uintptr_t)g_memory_base + 0xFD000000u);
        void *gpu_mmio = VirtualAlloc(gpu_mmio_base, 0x03000000u,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (gpu_mmio && gpu_mmio == gpu_mmio_base) {
            uint8_t *gpu_mmio_bytes = (uint8_t *)gpu_mmio;
            g_gpu_mmio_base = gpu_mmio;
            fprintf(stderr, "  GPU MMIO probe range: mapped 48 MB at Xbox VA 0xFD000000\n");

            /*
             * sub_00170385 (deep in the D3D device-init chain) is a loop
             * that can *only* return via a specific combination of status
             * bits at three fixed registers: bit 0x10 SET at both
             * 0xFD003214 and 0xFD002400, then bit 0x10 CLEAR at 0xFD003220
             * -- confirmed by reading the function's complete control flow,
             * not assumed. The third condition is already satisfied by our
             * zero-initialized mapping, but the first two are what gate
             * *reaching* that check at all: with them also zero, the
             * function takes an unconditional "skip ahead" branch before
             * ever evaluating the real exit condition, making the loop run
             * forever regardless of anything else. Unlike the busy/kick
             * bits elsewhere in this file, these read as static
             * capability/presence flags (real hardware would report them
             * once, at device-detection time, not toggle them), so this is
             * a one-time init here rather than something the pump thread
             * needs to service continuously.
             */
            gpu_mmio_bytes[0x3214] |= 0x10;
            gpu_mmio_bytes[0x2400] |= 0x10;

            g_pfifo_pump_running = 1;
            g_pfifo_pump_thread = CreateThread(
                NULL, 0, xbox_pfifo_pump_thread,
                (LPVOID)g_memory_base,
                0, NULL);
            {
                const char *e = getenv("XBOX_VBLANK_PUMP");
                g_vblank_from_pump = e && e[0] == '1';
                if (!g_vblank_from_pump && g_pfifo_pump_thread) {
                    HANDLE vt = CreateThread(NULL, 0, xbox_vblank_thread, NULL, 0, NULL);
                    if (vt) CloseHandle(vt);
                    else g_vblank_from_pump = 1;
                }
            }
            if (g_pfifo_pump_thread) {
                fprintf(stderr, "  PFIFO kick pump: running (clears kick bit at 0x%08X, "
                        "drains GPU context ring + fence at 0x%08X)\n",
                        XBOX_PFIFO_KICK_VA, XBOX_GPU_CONTEXT_PTR_VA);
            } else {
                g_pfifo_pump_running = 0;
                fprintf(stderr, "  PFIFO kick pump: FAILED to start (error %lu) -- "
                        "driver command-pusher spins will hang\n", GetLastError());
            }
        } else {
            fprintf(stderr, "  GPU MMIO probe range: FAILED to map at %p (error %lu) -- "
                    "direct register probes in this range will still fault\n",
                    gpu_mmio_base, GetLastError());
        }
    }

    if (xbox_memlayout_log_on())
        xbox_memlayout_dump_regions();

    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

void xbox_MemoryLayoutShutdown(void)
{
    if (g_pfifo_pump_thread) {
        g_pfifo_pump_running = 0;
        WaitForSingleObject(g_pfifo_pump_thread, 1000);
        CloseHandle(g_pfifo_pump_thread);
        g_pfifo_pump_thread = NULL;
    }
    if (g_gpu_mmio_base) {
        VirtualFree(g_gpu_mmio_base, 0, MEM_RELEASE);
        g_gpu_mmio_base = NULL;
    }
    /* Unmap mirror views first */
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            UnmapViewOfFile(g_mirror_views[m]);
            g_mirror_views[m] = NULL;
        }
        for (int k = 0; k < XBOX_MIRROR_MAX_PARTS; k++) {
            if (g_mirror_parts[m][k].view) {
                UnmapViewOfFile(g_mirror_parts[m][k].view);
                g_mirror_parts[m][k].view = NULL;
            }
        }
    }
    g_mirror_slots = 0;
    /* Unmap base view */
    if (g_memory_base) {
        UnmapViewOfFile(g_memory_base);
        g_memory_base = NULL;
        g_memory_size = 0;
    }
    /* Close file mapping handle */
    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

/* ── Dynamic heap allocator ────────────────────────────────
 *
 * Simple bump allocator for MmAllocateContiguousMemory and similar.
 * Returns Xbox VAs within the mapped region so MEM32() works correctly.
 * No free support (bump-only for now).
 */
static uint32_t g_heap_next = XBOX_HEAP_BASE;

/*
 * Allocation ledger. "Which subsystem owns this guest address?" is the
 * question that decides whether a stray write is a bad pointer or a genuine
 * double-allocation, and it cannot be answered from the address alone. Record
 * every block with the native return addresses of whoever asked for it; the
 * diag server's `owner <va>` command looks the address up and prints them for
 * addr2line. Ring buffer, so a long run costs a fixed amount.
 */
#define XBOX_ALLOC_LEDGER 4096
typedef struct {
    uint32_t va, size;
    uint32_t idx;
    void    *frames[6];
    USHORT   nframes;
} xbox_alloc_rec;
static xbox_alloc_rec g_ledger[XBOX_ALLOC_LEDGER];
static int g_ledger_n = 0;   /* number written, saturating at the cap */

static int g_heap_alloc_count = 0;

/* g_heap_cs protects g_heap_next/g_heap_alloc_count below -- now that Xbox
 * worker threads run as genuine concurrent OS threads (see
 * bridge_PsCreateSystemThreadEx in kernel_bridge.c), more than one can call
 * this bump allocator at once. Declared near the top of this file and
 * initialized once, eagerly, from xbox_MemoryLayoutInit while still
 * single-threaded (simpler and race-free vs. a lazy double-checked init). */

/* Contiguous allocation, honouring the physical-address range the caller asks
 * for.
 *
 * `MmAllocateContiguousMemoryEx` takes a lowest and highest acceptable physical
 * address, and that is how a title guarantees memory the GPU can reach: this
 * one asks for `high = 0x03FFB000` -- inside the console's 64 MB -- and then
 * hands the GPU `va & 0x03FFFFFF`. The bridge ignored the range and returned
 * whatever the bump allocator was up to, so once the heap passed 64 MB the
 * masked address pointed somewhere else entirely and the texture was read from
 * empty memory. The blank splash screen was exactly this.
 *
 * The fix has two halves. This one lets both allocators share the GPU-visible
 * low region from opposite ends -- ordinary allocations up from
 * XBOX_HEAP_BASE, range-constrained ones down from the ceiling the caller
 * names -- instead of reserving a fixed 16 MB pool to serve ~7 MB. The other
 * half is in the header: the kernel data area, TIB pool and stack moved above
 * the 64 MB line, since the GPU never reads them, leaving
 * 0x00220000..0x04000000 unbroken for the arena and the surfaces. */
static uint32_t g_contig_next = 0;   /* 0 until first use; then the low water mark */
static uint32_t g_heap_spill = 0;    /* 0 until the low region fills */
static uint32_t g_virt_next = 0;     /* xbox_HeapAllocVirtual's low water mark, from the top */

/* Reclaimed blocks.
 *
 * The bump allocator never reused anything, which was fine while the heap was
 * 140 MB and nothing cared where an allocation landed. It stopped being fine
 * once every allocation had to fit below 64 MB: the title creates the same
 * ~521 KB group of surfaces three times during init and frees the earlier
 * ones, and those frees were dropped, so the low region ran out ~64 KB short
 * and the overflow spilled above the line -- where the title's own uncached
 * aliases fold back onto guest .text.
 *
 * Exact-fit first, because the title reallocates the same sizes. */
#define XBOX_FREE_LIST_CAP 256
static struct { uint32_t va, size; } g_free_list[XBOX_FREE_LIST_CAP];
static int g_free_list_n = 0;

static uint32_t heap_take_free(uint32_t size, uint32_t alignment)
{
    int i, best = -1;
    /* Reuse is ON. Handing a freed block back out is the correct behaviour,
     * and the title frees ~40 times a run.
     *
     * It does change the failure mode of a latent use-after-free: a bump
     * allocator leaves a freed block's contents intact, so a stale pointer
     * reads plausible data, while reuse lets the next owner overwrite it. But
     * measured over three runs each -- not the one run each that first
     * suggested the free list was to blame -- it is not the cause:
     *
     *     reuse ON    3/3 crash in sub_001423C0
     *     reuse OFF   2/3 crash, 1/3 hang
     *
     * So the crash is there either way and reuse only makes it deterministic,
     * which is worth having while it is being chased. XBOX_HEAP_NO_REUSE=1
     * turns it off to compare. */
    { static int off = -1;
      if (off < 0) off = getenv("XBOX_HEAP_NO_REUSE") ? 1 : 0;
      if (off) return 0; }
    for (i = 0; i < g_free_list_n; i++) {
        uint32_t va = g_free_list[i].va;
        if (g_free_list[i].size < size) continue;
        if (va & (alignment - 1)) continue;          /* keep it simple: exact alignment */
        if (best < 0 || g_free_list[i].size < g_free_list[best].size) best = i;
        if (g_free_list[i].size == size) { best = i; break; }
    }
    if (best < 0) return 0;
    {
        uint32_t va = g_free_list[best].va;
        uint32_t sz = g_free_list[best].size;
        g_free_list[best] = g_free_list[--g_free_list_n];
        if (sz >= size + 4096u && g_free_list_n < XBOX_FREE_LIST_CAP) {
            g_free_list[g_free_list_n].va   = va + size;
            g_free_list[g_free_list_n].size = sz - size;
            g_free_list_n++;
        }
        return va;
    }
}

/* Low reserve for contiguous allocations capped below 16 MB (fork).
 *
 * Contiguous memory is carved from the top of the 64 MB window downwards while
 * the heap grows from 0x00214000 upwards, so by the time the title's D3D asks
 * MmAllocateContiguousMemoryEx(0x1000, 0, 0xFFFFFF, ..) for a page of
 * visibility-test reports the whole range below 16 MB belongs to the heap:
 * "no room", EndVisibilityTest fails, GET_REPORT is never sent and every
 * result reads 0 from the null page -- the lens flare never showed.
 *
 * Physical memory between the low page and the XBE image, [0x8000, 0x10000),
 * is used by nothing (checked: all zero at the end of a menus + race run
 * without this). It serves only requests that would otherwise fail with "no
 * room" and whose ceiling is below 16 MB; the heap is not touched. When it
 * runs out the request fails as before (counted). XBOX_FIX_OCCLUSION=0 turns
 * it off together with the translator's occlusion queries. */
#define LOW_RESERVE_LO 0x00008000u
#define LOW_RESERVE_HI 0x00010000u
static uint32_t g_low_reserve_next = LOW_RESERVE_LO;
static unsigned g_low_reserve_served, g_low_reserve_exhausted;
static int low_reserve_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_OCCLUSION"); on = !(e && e[0] == '0'); }
    return on;
}

uint32_t xbox_HeapAllocRange(uint32_t size, uint32_t alignment,
                             uint32_t low, uint32_t high)
{
    uint32_t ceiling, result;

    if (high == 0 || high >= XBOX_GPU_VISIBLE_END - 1)
        return xbox_HeapAlloc(size, alignment);

    EnterCriticalSection(&g_heap_cs);
    if (alignment < 4) alignment = 4;
    if (size < 4096) size = 4096;

    if (g_contig_next == 0) g_contig_next = XBOX_GPU_VISIBLE_END;

    {   /* A freed block inside the caller's range is better than growing. */
        uint32_t reuse = heap_take_free(size, alignment);
        if (reuse && reuse >= low && reuse + size - 1 <= high) {
            memset((void *)((uintptr_t)reuse + g_memory_offset), 0, size);
            g_heap_alloc_count++;
            LeaveCriticalSection(&g_heap_cs);
            return reuse;
        }
        if (reuse && g_free_list_n < XBOX_FREE_LIST_CAP) {   /* put it back */
            g_free_list[g_free_list_n].va = reuse;
            g_free_list[g_free_list_n].size = size;
            g_free_list_n++;
        }
    }

    ceiling = high + 1;
    if (ceiling > g_contig_next) ceiling = g_contig_next;

    result = (ceiling - size) & ~(alignment - 1);

    if ((size > ceiling || result < low || result < g_heap_next)
        && high <= 0x00FFFFFFu && high >= LOW_RESERVE_LO && low_reserve_on()) {
        uint32_t r = (g_low_reserve_next + alignment - 1) & ~(alignment - 1);
        void *cf[4]; USHORT n = CaptureStackBackTrace(1, 4, cf, NULL);
        unsigned long long lk = n ? (unsigned long long)((uintptr_t)cf[n - 1]
            - (uintptr_t)GetModuleHandleA(NULL) + 0x140000000ull) : 0ull;
        if (r >= low && r + size <= LOW_RESERVE_HI && r + size - 1 <= high) {
            g_low_reserve_next = r + size;
            g_low_reserve_served++;
            memset((void *)((uintptr_t)r + g_memory_offset), 0, size);
            g_heap_alloc_count++;
            fprintf(stderr, "  [HEAP] contiguous #%d: size=%u range[0x%08X,0x%08X] -> 0x%08X"
                            " (low reserve, %u served) caller=0x%llX\n",
                    g_heap_alloc_count, size, low, high, r, g_low_reserve_served, lk);
            fflush(stderr);
            LeaveCriticalSection(&g_heap_cs);
            return r;
        }
        g_low_reserve_exhausted++;
        fprintf(stderr, "  [HEAP] low reserve exhausted (%u served, %u refused): falling back\n",
                g_low_reserve_served, g_low_reserve_exhausted);
    }
    if (size > ceiling || result < low || result < g_heap_next) {
        LeaveCriticalSection(&g_heap_cs);
        {   /* name the caller, to inventory what still fails */
            void *cf[4]; USHORT n = CaptureStackBackTrace(1, 4, cf, NULL);
            unsigned long long lk = n ? (unsigned long long)((uintptr_t)cf[n - 1]
                - (uintptr_t)GetModuleHandleA(NULL) + 0x140000000ull) : 0ull;
            fprintf(stderr, "  [HEAP] contiguous %u bytes in [0x%08X,0x%08X]: no room"
                            " (down to 0x%08X, heap up to 0x%08X) caller=0x%llX\n",
                    size, low, high, g_contig_next, g_heap_next, lk);
        }
        fflush(stderr);
        return 0;
    }
    g_contig_next = result;

    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
    g_heap_alloc_count++;
    fprintf(stderr, "  [HEAP] contiguous #%d: size=%u align=%u range[0x%08X,0x%08X]"
                    " -> 0x%08X (%u KB between the two ends)\n",
            g_heap_alloc_count, size, alignment, low, high, result,
            (g_contig_next - g_heap_next) / 1024u);
    fflush(stderr);
    LeaveCriticalSection(&g_heap_cs);
    return result;
}

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    EnterCriticalSection(&g_heap_cs);

    if (alignment < 4) alignment = 4;

    /* Every allocation gets its own page. This looks wasteful -- ExAllocatePool
     * is the kernel's small-object pool and the title calls it ~1028 times a
     * run, so this costs ~4.2 MB for a few hundred KB of objects -- but it is
     * load-bearing: lowering the floor to the real size, or even to 512 bytes,
     * collapses rendering (draws 128,257 -> 2,562, no textures installed at
     * all) within 43 allocations. Both variants still share pages between
     * objects, so what the title depends on is a *page* per pool block, not
     * slack after one. Do not shave this without finding out what reads a
     * neighbouring object; the space it costs is reclaimed by the spill
     * region below instead. */
    if (size == 0) size = alignment;
    size = (size + alignment - 1) & ~(alignment - 1);

    /* Everything the title can see must live below 64 MB, not just the
     * memory the GPU reads.
     *
     * The title forms uncached aliases as `0x80000000 | (addr & 0x03FFFFFF)`
     * and writes through them -- it did exactly that to a mesh remap table in
     * sub_001423C0. An allocation above the line masks down onto whatever
     * shares its low 26 bits, and in this layout that was guest .text: 1026
     * writes walked across the page holding GfxContext_ApplyRenderStateDelta"s
     * jump table at 0x000FB030, zeroing it, after which that switch
     * tail-jumped to a null target on every render-state change.
     *
     * So the low region serves everything, and the spill above the line is a
     * last resort that should never be reached -- it exists so an overflow
     * fails loudly later rather than silently corrupting the image now. It
     * only became reachable at all because every allocation used to be
     * rounded up to a 4 KB page, which cost 4.2 MB of a 64 MB budget for
     * ~1000 ExAllocatePool blocks of 96-400 bytes. */
    result = heap_take_free(size, alignment);
    if (result) {
        memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
        g_heap_alloc_count++;
        LeaveCriticalSection(&g_heap_cs);
        return result;
    }

    result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > (g_contig_next ? g_contig_next : XBOX_GPU_VISIBLE_END)) {
        if (!g_heap_spill) {
            g_heap_spill = XBOX_HEAP_SPILL_BASE;
            fprintf(stderr, "  [HEAP] WARNING: low region exhausted at 0x%08X;"
                            " spilling above 64 MB, where the title's own"
                            " uncached aliases will fold back onto the image\n",
                    g_heap_next);
        }
        result = (g_heap_spill + alignment - 1) & ~(alignment - 1);
        if (result + size > (g_virt_next ? g_virt_next : XBOX_HEAP_BASE + XBOX_HEAP_SIZE)) {
            fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u,"
                            " spill at 0x%08X)\n", size, g_heap_spill);
            LeaveCriticalSection(&g_heap_cs);
            return 0;
        }
        g_heap_spill = result + size;
    } else {
        g_heap_next = result + size;
    }

    /* Zero-fill the allocated block (Xbox memory is always zeroed) */
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    if (g_ledger_n < XBOX_ALLOC_LEDGER) {
        xbox_alloc_rec *r = &g_ledger[g_ledger_n++];
        r->va = result; r->size = size; r->idx = (uint32_t)g_heap_alloc_count + 1;
        r->nframes = CaptureStackBackTrace(1, 6, r->frames, NULL);
    }
    g_heap_alloc_count++;
    { void *_cf[2]; USHORT _n = CaptureStackBackTrace(1, 2, _cf, NULL);
      unsigned long long _lk = _n ? (unsigned long long)((uintptr_t)_cf[0]
          - (uintptr_t)GetModuleHandleA(NULL) + 0x140000000ull) : 0ull;
      fprintf(stderr, "  [HEAP] #%d: size=%u align=%u → 0x%08X..0x%08X (used %u/%u) caller=0x%llX\n",
            g_heap_alloc_count, size, alignment, result, result + size,
            g_heap_next - XBOX_HEAP_BASE, XBOX_HEAP_SIZE, _lk); }
    fflush(stderr);

    LeaveCriticalSection(&g_heap_cs);
    return result;
}

/* Memory the title can only reach by virtual address: NtAllocateVirtualMemory
 * reservations and thread stacks.
 *
 * On the console neither costs the GPU-visible budget. A reserve takes no
 * physical memory until it is committed, and virtual pages are never
 * physically contiguous, so no correct title forms the `0x80000000 | (addr &
 * 0x03FFFFFF)` aliases of them that pin everything else below 64 MB. Serving
 * them from the low region spent 1.1 MB of it -- the XAPI heap's 1 MB
 * reserve at boot and two 64 KB thread stacks -- and SSX Tricky's budget is
 * exact: its hard-coded 53.1 MB arena plus ~7 MB of surfaces left nothing,
 * so from the first 3D frame every 256-byte surface request the D3D runtime
 * made failed (1,057 times in 24 s) and the character-select meshes were
 * built on null allocations.
 *
 * Served downward from the top of the mapped region, so the upward overflow
 * spill (xbox_HeapAlloc) keeps its own space and its warning. Freed blocks
 * are not recycled (see xbox_HeapFree): the free list feeds the low region. */
uint32_t xbox_HeapAllocVirtual(uint32_t size, uint32_t alignment)
{
    uint32_t result, floor;

    EnterCriticalSection(&g_heap_cs);
    if (alignment < 4) alignment = 4;
    if (size == 0) size = alignment;
    size = (size + alignment - 1) & ~(alignment - 1);
    if (!g_virt_next) g_virt_next = XBOX_HEAP_BASE + XBOX_HEAP_SIZE;
    floor = g_heap_spill ? g_heap_spill : XBOX_HEAP_SPILL_BASE;
    if (size > g_virt_next || ((g_virt_next - size) & ~(alignment - 1)) < floor) {
        LeaveCriticalSection(&g_heap_cs);
        fprintf(stderr, "xbox_HeapAllocVirtual: out of memory (requested %u)\n", size);
        return xbox_HeapAlloc(size, alignment);
    }
    result = (g_virt_next - size) & ~(alignment - 1);
    g_virt_next = result;
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
    if (g_ledger_n < XBOX_ALLOC_LEDGER) {
        xbox_alloc_rec *r = &g_ledger[g_ledger_n++];
        r->va = result; r->size = size; r->idx = (uint32_t)g_heap_alloc_count + 1;
        r->nframes = CaptureStackBackTrace(1, 6, r->frames, NULL);
    }
    g_heap_alloc_count++;
    fprintf(stderr, "  [HEAP] #%d: virtual size=%u align=%u -> 0x%08X..0x%08X (above the GPU line)\n",
            g_heap_alloc_count, size, alignment, result, result + size);
    fflush(stderr);
    LeaveCriticalSection(&g_heap_cs);
    return result;
}

void xbox_HeapFree(uint32_t xbox_va)
{
    uint32_t base = 0, size = 0;
    if (!xbox_va) return;
    /* Titles free contiguous memory through its uncached (0x80000000) or
     * write-combined (0xF0000000) alias -- on an Xbox that is the address
     * MmAllocateContiguousMemory returned. Ours hands out the physical form,
     * so fold before the lookup, which otherwise misses and leaks the block. */
    {
        static int fold = -1;
        if (fold < 0) { const char *e = getenv("XBOX_HEAPFREE_FOLD"); fold = !(e && e[0] == '0'); }
        if (fold && xbox_va >= 0x80000000u && xbox_va < 0x84000000u)
            xbox_va &= 0x7FFFFFFFu;
        else if (fold && xbox_va >= 0xF0000000u && xbox_va < 0xF4000000u)
            xbox_va &= 0x03FFFFFFu;
    }
    if (!xbox_heap_owner_of(xbox_va, &base, &size, NULL, NULL, 0, NULL) || !size)
        return;
    /* Only low-region blocks are recycled: a block from above the GPU line,
     * handed back out by xbox_HeapAlloc, would sit where the title's own
     * uncached aliases fold onto the image. */
    if (base >= XBOX_GPU_VISIBLE_END)
        return;
    EnterCriticalSection(&g_heap_cs);
    if (g_free_list_n < XBOX_FREE_LIST_CAP) {
        g_free_list[g_free_list_n].va   = base;
        g_free_list[g_free_list_n].size = size;
        g_free_list_n++;
    }
    LeaveCriticalSection(&g_heap_cs);
}

/* Real, live heap accounting for anything that needs to report memory
 * statistics to the game (MmQueryStatistics and friends) -- see the bug
 * this replaced: xbox_MmQueryStatistics (kernel_memory.c) used to report a
 * hardcoded 64 MB total (not this port's actual, larger XBOX_TOTAL_RAM) and
 * an "available" figure derived from the *host PC's* real free RAM via
 * GlobalMemoryStatusEx, clamped to a static 32 MB -- completely disconnected
 * from this heap's actual, changing consumption. A title that sizes a bulk
 * allocation off that report is sizing it against a number with no
 * relationship to what's really free, which is exactly how a legitimate,
 * in-budget-on-real-hardware request can still blow this port's own,
 * differently-loaded heap. */
void xbox_HeapGetStats(uint32_t* out_used, uint32_t* out_total)
{
    EnterCriticalSection(&g_heap_cs);
    if (out_used)  *out_used  = g_heap_next - XBOX_HEAP_BASE;
    if (out_total) *out_total = XBOX_HEAP_SIZE;
    LeaveCriticalSection(&g_heap_cs);
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}

size_t xbox_GetMemorySize(void)
{
    return g_memory_size;
}

/* Number of RAM mirror views actually mapped. Each aliases the same physical
 * pages as the base view at g_memory_base + (m+1)*g_memory_size, and each has
 * its own page protections -- so a write watch that protects only the base
 * view is blind to any write that arrives through a mirror. That is not
 * hypothetical: guest .text at 0x000FB030 was being zeroed while a
 * PAGE_READONLY watch on it reported nothing at all. */
int xbox_GetMirrorCount(void)
{
    int m, n = 0;
    for (m = 0; m < XBOX_NUM_MIRRORS; m++)
        if (g_mirror_views[m]) n++;
    return n;
}

/* Number of mirror slots below the GPU MMIO aperture. Slot m (0-based)
 * sits at g_memory_base + (m+1)*g_memory_size. Not the same as
 * xbox_GetMirrorCount when a slot in the middle could not take a view:
 * walk the slots and ask xbox_IsMirrorAddress before touching one. */
int xbox_GetMirrorSlotCount(void)
{
    return g_mirror_slots;
}

/* TRUE when p lies inside one of our mirror views (a full view or a part
 * of a partially mapped slot), i.e. memory that aliases the base view --
 * never something else that happens to occupy a mirror slot. */
BOOL xbox_IsMirrorAddress(const void *p)
{
    uintptr_t a = (uintptr_t)p, base = (uintptr_t)g_memory_base, rel, slot_off;
    int m, k;
    if (!g_memory_base || g_memory_size == 0 || a < base + g_memory_size) return FALSE;
    rel = a - base;
    m = (int)(rel / g_memory_size) - 1;
    if (m < 0 || m >= g_mirror_slots) return FALSE;
    if (g_mirror_views[m]) return TRUE;
    slot_off = rel % g_memory_size;
    for (k = 0; k < XBOX_MIRROR_MAX_PARTS; k++)
        if (g_mirror_parts[m][k].view && slot_off >= g_mirror_parts[m][k].slot_off &&
            slot_off < g_mirror_parts[m][k].slot_off + g_mirror_parts[m][k].len)
            return TRUE;
    return FALSE;
}

int xbox_VerifyViewIntegrity(const char *tag)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *p, *end;
    int foreign = 0;

    if (!g_memory_base) return 0;

    p   = (unsigned char *)g_memory_base;
    end = p + g_memory_size;

    while (p < end) {
        if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.Type != MEM_MAPPED || mbi.AllocationBase != g_memory_base) {
            if (foreign < 8) {
                fprintf(stderr,
                    "  [VIEW] %s: FOREIGN region in guest view at native %p "
                    "(Xbox VA 0x%08zX) size=%zu state=0x%lX type=0x%lX allocbase=%p (ours=%p)\n",
                    tag ? tag : "?", (void *)p,
                    (size_t)((uintptr_t)p - (uintptr_t)g_memory_base),
                    (size_t)mbi.RegionSize, (unsigned long)mbi.State,
                    (unsigned long)mbi.Type, mbi.AllocationBase, g_memory_base);
            }
            foreign++;
        }
        if (mbi.RegionSize == 0) break;
        p += mbi.RegionSize;
    }

    if (foreign)
        fprintf(stderr, "  [VIEW] %s: %d foreign region(s) inside the guest view\n",
                tag ? tag : "?", foreign);

    /* The view being intact does not mean memory is healthy: a corrupted
     * HOST heap makes RtlAllocateHeap hand out (and write through) wild
     * pointers, which can land anywhere -- including inside this view.
     * That is what a stray ntdll write into guest RAM actually means, so
     * validate the process heap here too. */
    if (!HeapValidate(GetProcessHeap(), 0, NULL)) {
        fprintf(stderr, "  [VIEW] %s: PROCESS HEAP IS CORRUPT\n", tag ? tag : "?");
        foreign++;
    }

    return foreign;
}

/*
 * Find the allocation covering `va`. Returns 0 if none. On success fills the
 * out-params; `frames` receives up to `max_frames` native return addresses.
 */
int xbox_heap_owner_of(uint32_t va, uint32_t *out_base, uint32_t *out_size,
                       uint32_t *out_index, void **frames, int max_frames,
                       int *out_nframes)
{
    int i, k;
    va = xbox_fold_ram_alias(va);   /* contiguous blocks are handed out aliased */
    for (i = 0; i < g_ledger_n; i++) {
        if (va < g_ledger[i].va || va >= g_ledger[i].va + g_ledger[i].size) continue;
        if (out_base)  *out_base  = g_ledger[i].va;
        if (out_size)  *out_size  = g_ledger[i].size;
        if (out_index) *out_index = g_ledger[i].idx;
        k = frames ? g_ledger[i].nframes : 0;
        if (k > max_frames) k = max_frames;
        if (k < 0) k = 0;
        for (int j = 0; j < k; j++) frames[j] = g_ledger[i].frames[j];
        if (out_nframes) *out_nframes = k;
        return 1;
    }
    return 0;
}
