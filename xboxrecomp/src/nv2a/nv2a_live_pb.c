/*
 * nv2a_live_pb.c -- consume the title's *live* push buffer.
 *
 * Until now the two halves of the display path have never been joined:
 *
 *   producer  the title's own statically-linked D3D8 writes NV2A methods into
 *             a ring buffer in Xbox memory (once the twenty SetRenderState
 *             functions that had never been
 *             translated, so nothing was written at all);
 *   consumer  nv2a_pgraph_d3d11.c translates NV2A methods into draw calls, but
 *             was reachable only from nv2a_pb_replay.c, an offline tool that
 *             replays *captured* buffers.
 *
 * This file is the join. It runs on the existing PFIFO pump thread (see
 * xbox_memory_layout.c), which already models "the software GPU consumes
 * everything instantly" by making GET track PUT -- it just threw the commands
 * away. Now they are parsed first.
 *
 * ── The ring ────────────────────────────────────────────────────────────────
 * The channel context pointer lives at Xbox VA 0x001776C0. Within that
 * context:
 *     +0x00  current write pointer (an Xbox VA), advanced by every method the
 *            title emits -- this is what sub_0016B920 hands out and what the
 *            SetRenderState family bumps
 *     +0x04  limit; sub_0016B920 compares against it and wraps via sub_0016B680
 *
 * Progress is tracked against +0x00 directly rather than against the PUT/GET
 * pair the pump uses, because +0x00 is what the producer actually moves; PUT is
 * only refreshed when the driver kicks. The buffer base is learned from the
 * first write pointer observed, which is correct because we start watching
 * before the title has emitted anything (init runs long before any draw).
 *
 * ── Threading ───────────────────────────────────────────────────────────────
 * Everything here happens on the pump thread, including D3D device creation and
 * every draw, so the D3D11 device is only ever touched by one thread. Like the
 * rest of the pump this reads plain Xbox memory cells and never touches the
 * g_eax/g_esp CPU-register globals, so it cannot race the recompiled code.
 *
 * A torn read is possible in principle -- the title could be mid-write when we
 * sample the pointer -- but the pointer is only published *after* the method
 * and its data are stored (the setters write [eax], [eax+4] and only then
 * `mov [esi],eax`), so anything below the sampled pointer is complete.
 *
 * ── Gate ────────────────────────────────────────────────────────────────────
 * On by default; set XBOX_LIVE_PB=0 to disable and get the previous
 * headless behaviour back.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "nv2a_pgraph_d3d11.h"
#include "../d3d/d3d8_xbox.h"
/* XBOX_TOTAL_RAM: the size of the guest window, which the translator needs
 * in order to bounds-check vertex stream offsets. Header-only use -- it
 * adds no link dependency from nv2a back onto the kernel library. */
#include "../kernel/xbox_memory_layout.h"
#define XBOX_GUEST_RAM_BYTES ((uint32_t)XBOX_TOTAL_RAM)

/* Push-buffer command header encoding (same as nv2a_pb_replay.c). */
#define PB_INC_MASK      0xE0030003u
#define PB_INC_MATCH     0x00000000u
#define PB_NONINC_MASK   0xE0030003u
#define PB_NONINC_MATCH  0x40000000u

#define GPU_CONTEXT_PTR_VA  0x001776C0u
#define CTX_WRITE_PTR_OFF   0x00u
#define CTX_LIMIT_OFF       0x04u
/*
 * The real push-buffer extent, confirmed live against the title's own
 * allocations: +0x10 is the buffer base and +0x14 the end, and they match
 * xbox_HeapAlloc block #12 exactly (0x0108B000..0x0128D000, 2 MB). +0x04 is
 * only the current segment limit, so it must not be used as the wrap point.
 */
#define CTX_BUF_BASE_OFF    0x10u
#define CTX_BUF_END_OFF     0x14u

/* The title's own surface flip drives Present; this is only the fallback for
 * when it stops flipping, so the window keeps pumping messages. */
/* Safety net only: fires when the title has genuinely stopped presenting, so
 * a stall cannot freeze the window. The frame boundary drives the real one. */
#define PRESENT_FALLBACK_MS 500

static int      g_enabled       = -1;   /* -1 = not yet resolved */
static int      g_ready         = 0;    /* device + translator live */
static int      g_failed        = 0;    /* init failed; do not retry */
static uint32_t g_last_wp       = 0;
/* A wrap can only happen within one maximum-length command run of the limit
 * (count is 11 bits, so 2047 dwords). Anything further back is a reset. */
#define WRAP_SLACK  0x2000u
static uint32_t g_resets;
static int g_wp_wild;
static uint32_t g_pb_end;
static uint32_t g_pb_base       = 0;
static uint32_t g_pb_limit      = 0;
static DWORD    g_last_present  = 0;
static uint64_t g_dwords_seen   = 0;
static uint32_t g_methods       = 0;
static uint32_t g_batches       = 0;

/* Ring extent, published by the contiguous allocator (kernel_bridge.c).
 *
 * The title writes its one-time texture, vertex-format and surface state into
 * the push buffer during D3D init and then recycles it, all before this
 * consumer can attach -- attaching needs a D3D11 device, which cannot exist
 * until that same init has finished. So the capture has to start before the
 * parse can, and the channel context is useless that early: during init the
 * title publishes neither its write pointer (it reads as base throughout) nor
 * the base/limit fields. The allocation is the only place the extent is known
 * in time. */
static uint32_t  g_ring_hint_base;
static uint32_t  g_ring_hint_size;
static uint32_t *g_init_capture;
static uint32_t  g_init_capture_dwords;
static uint32_t  g_init_capture_fill;

void nv2a_live_pb_note_ring(uint32_t base, uint32_t size)
{
    if (!g_ring_hint_base) {
        g_ring_hint_base = base;
        g_ring_hint_size = size;
    }
}

/* Append-only capture of the push buffer, sampled from the guest thread.
 *
 * The 1 ms pump cannot see D3D init: the title emits its one-time texture,
 * vertex-format and surface state and recycles the ring inside a single tick,
 * and the translator cannot be brought up that early anyway (it needs a D3D11
 * device, which needs the very init that is being missed). Sampling from the
 * kernel dispatcher instead runs on the guest's own thread, interleaved with
 * that init at kernel-call granularity, so no window is missed. Only the delta
 * since the last sample is copied, so the cost is a couple of loads per call. */
#define INIT_CAPTURE_MAX  (512u * 1024u)
static uint32_t  g_cap_wp;

void nv2a_live_pb_capture(uint8_t *mem_base)
{
    uint32_t ctx, wp;
    if (g_ready || !mem_base || !g_ring_hint_base) return;
    ctx = *(volatile uint32_t *)(mem_base + GPU_CONTEXT_PTR_VA);
    if (!ctx) return;
    wp = *(volatile uint32_t *)(mem_base + ctx + CTX_WRITE_PTR_OFF);
    if (!g_cap_wp) g_cap_wp = g_ring_hint_base;
    if (wp <= g_cap_wp) { if (wp && wp < g_cap_wp) g_cap_wp = wp; return; }
    {
        uint32_t n = (wp - g_cap_wp) / 4u;
        if (!g_init_capture) {
            g_init_capture = (uint32_t *)malloc(INIT_CAPTURE_MAX);
            g_init_capture_dwords = g_init_capture ? INIT_CAPTURE_MAX / 4u : 0;
        }
        if (g_init_capture && g_init_capture_fill + n <= g_init_capture_dwords) {
            memcpy(g_init_capture + g_init_capture_fill,
                   mem_base + g_cap_wp, n * 4u);
            g_init_capture_fill += n;
        }
        g_cap_wp = wp;
    }
}

int nv2a_live_pb_enabled(void)
{
    if (g_enabled < 0) {
        const char *e = getenv("XBOX_LIVE_PB");
        g_enabled = (e && e[0] == '0') ? 0 : 1;
    }
    return g_enabled;
}

/* ── one-time bring-up of the D3D8 HLE device ──────────────────────────────
 * The title never calls our CreateDevice: its D3D8 is statically linked and
 * talks to the GPU through the push buffer instead. So the host-side device
 * that the translator draws into has to be created here. */
static int live_pb_bring_up(void)
{
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;

    d3d = xbox_Direct3DCreate8(0);
    if (!d3d) {
        fprintf(stderr, "[LIVE-PB] Direct3DCreate8 failed; live push buffer disabled\n");
        return 0;
    }

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth  = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount  = 1;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow    = NULL;   /* the layer creates its own output window */
    pp.Windowed         = TRUE;

    hr = d3d->lpVtbl->CreateDevice(d3d, 0, 1 /* HAL */, NULL, 0, &pp, &dev);
    if (FAILED(hr) || !dev) {
        fprintf(stderr, "[LIVE-PB] CreateDevice failed: 0x%08lX; live push buffer disabled\n",
                (unsigned long)hr);
        return 0;
    }

    pgraph_d3d11_init();
    fprintf(stderr, "[LIVE-PB] live push buffer connected to the D3D11 translator\n");
    return 1;
}

/*
 * Record which NV2A methods the translator does not implement.
 *
 * pgraph_d3d11_method returns 0 for anything it does not handle, and during
 * device init the title emits a lot of them. Logging each distinct method once
 * turns "272 ignored" into an actionable list of exactly what to implement
 * next, without flooding the log the way per-call logging would.
 */
#define UNHANDLED_CAP 256
static uint32_t g_unhandled[UNHANDLED_CAP];
static int      g_unhandled_count = 0;

static void note_unhandled(uint32_t method)
{
    int i;
    for (i = 0; i < g_unhandled_count; i++)
        if (g_unhandled[i] == method) return;
    if (g_unhandled_count < UNHANDLED_CAP) {
        g_unhandled[g_unhandled_count++] = method;
        fprintf(stderr, "[LIVE-PB] unhandled NV2A method 0x%04X (distinct #%d)\n",
                method, g_unhandled_count);
    }
}

/* ── parse one contiguous run of push-buffer dwords ────────────────────── */
/*
 * Dword accounting and a per-method histogram.
 *
 * "524766 dwords -> 1238 methods" is not a number anyone can act on: it does
 * not say whether the parser is losing sync, whether the ring is mostly
 * untouched, or whether the title is simply not emitting draws yet. These
 * three counters separate those cases, and the histogram reports the
 * draw-critical registers by name whether or not they were ever seen -- an
 * absent DRAW_ARRAYS and a mis-parsed one look identical in a total.
 */
static unsigned long long g_dw_zero, g_dw_cmd, g_dw_unknown;
#define METHOD_HIST_SLOTS 2048          /* method is 14 bits, dword-aligned */
static unsigned int g_method_hist[METHOD_HIST_SLOTS];

static void hist_note(unsigned int method)
{
    unsigned int slot = method >> 2;
    if (slot < METHOD_HIST_SLOTS) g_method_hist[slot]++;
}

void nv2a_live_pb_report(void)
{
    static const struct { unsigned int m; const char *name; } watch[] = {
        { 0x17FC, "SET_BEGIN_END"               },
        { 0x1810, "DRAW_ARRAYS"                 },
        { 0x1818, "INLINE_ARRAY"                },
        { 0x1800, "ARRAY_ELEMENT16"             },
        { 0x1808, "ARRAY_ELEMENT32"             },
        { 0x1720, "VERTEX_DATA_ARRAY_OFFSET[0]" },
        { 0x1760, "VERTEX_DATA_ARRAY_FORMAT[0]" },
        { 0x01D0, "CLEAR_SURFACE"               },
    };
    unsigned long long total = g_dw_zero + g_dw_cmd + g_dw_unknown;
    unsigned int i, top[8], seen = 0;

    for (i = 0; i < 8; i++) top[i] = 0;

    fprintf(stderr, "[LIVE-PB] dwords: %llu zero, %llu command, %llu unrecognised (of %llu); %u segment resets\n",
            g_dw_zero, g_dw_cmd, g_dw_unknown, total, g_resets);

    for (i = 0; i < sizeof(watch) / sizeof(watch[0]); i++)
        fprintf(stderr, "[LIVE-PB]   %-28s 0x%04X  x%u\n",
                watch[i].name, watch[i].m, g_method_hist[watch[i].m >> 2]);

    for (i = 0; i < METHOD_HIST_SLOTS; i++) {
        unsigned int j, k;
        if (!g_method_hist[i]) continue;
        seen++;
        for (j = 0; j < 8; j++) {
            if (g_method_hist[i] > g_method_hist[top[j]]) {
                for (k = 7; k > j; k--) top[k] = top[k - 1];
                top[j] = i;
                break;
            }
        }
    }
    /* The vertex-array block in full: which attribute slots the title binds
     * and how it draws is exactly what the translator needs, and a top-8 list
     * never shows it. */
    fprintf(stderr, "[LIVE-PB] vertex-array block:\n");
    for (i = 0x1700; i < 0x1830; i += 4) {
        unsigned int c = g_method_hist[i >> 2];
        if (c) fprintf(stderr, "[LIVE-PB]   0x%04X  x%u\n", i, c);
    }
    fprintf(stderr, "[LIVE-PB] %u distinct methods seen; busiest:\n", seen);
    for (i = 0; i < 8 && g_method_hist[top[i]]; i++)
        fprintf(stderr, "[LIVE-PB]   0x%04X  x%u\n", top[i] << 2, g_method_hist[top[i]]);
    fflush(stderr);
}

/*
 * How much of the ring's tail the title wrote before it wrapped.
 *
 * At a wrap the producer writes a jump back to the ring's base where it
 * stopped, then carries on from the base -- and moves the context's limit
 * field to the new segment in the same breath. By the time the pump samples
 * the context, the limit is the new segment's, below the old write pointer,
 * so the tail length came out as zero and everything from the last sample to
 * the jump was dropped: part of a frame's draws and, often, its render-surface
 * switch. Losing one switch made the next frame look like the same surface
 * too, so two frames went unpresented -- a 50 ms hitch several times a second
 * in a race. Walk the commands from the last sample up to the jump instead.
 * The old limit is not a bound either: the title's last command before a
 * wrap can run past it (a jump 200 bytes beyond it was seen), so `max` is the
 * ring's end and only the jump -- or a dword that is no command -- ends the
 * walk. Returns the dword count to parse; *jump_out gets the jump header, or
 * 0 if none was found.
 */
static uint32_t live_pb_tail_len(const uint32_t *data, uint32_t max, uint32_t *jump_out)
{
    uint32_t pos = 0;
    *jump_out = 0;
    while (pos < max) {
        uint32_t h = data[pos];
        if (h == 0) { pos++; continue; }
        if ((h & 0xE0000003u) == 0x20000000u || (h & 3u) == 1u) {  /* old / new jump */
            *jump_out = h;
            return pos;
        }
        if ((h & PB_INC_MASK) == PB_INC_MATCH || (h & PB_NONINC_MASK) == PB_NONINC_MATCH) {
            uint32_t count = (h >> 18) & 0x7FF;
            if (pos + 1 + count > max) break;
            pos += 1 + count;           /* count 0 (0x40001800 between draws) is a no-op */
            continue;
        }
        break;                          /* not a command: stale bytes past the end */
    }
    return pos;
}

static void live_pb_parse(const uint32_t *data, uint32_t num_dwords)
{
    uint32_t pos = 0;

    while (pos < num_dwords) {
        uint32_t header = data[pos];

        if (header == 0) { g_dw_zero++; pos++; continue; }

        if ((header & PB_INC_MASK) == PB_INC_MATCH) {
            uint32_t count      = (header >> 18) & 0x7FF;
            uint32_t method     = header & 0x1FFC;
            uint32_t subchannel = (header >> 13) & 7;
            uint32_t i;

            if (count == 0 || pos + 1 + count > num_dwords) { g_dw_unknown++; pos++; continue; }

            /* Decisive trace: the bound vertex-stream offset never changes and
             * the memory it points at reads as an identity matrix, which is not
             * what a position stream looks like. Dump the real command context
             * around the first few draws so the actual method/param sequence
             * settles what the title is doing, rather than inference. */
            if (method <= 0x1810 && method + count * 4 > 0x1810) {
                static int traced = 0;
                if (traced < 2) {
                    uint32_t s = (pos >= 48) ? pos - 48 : 0, k;
                    traced++;
                    fprintf(stderr, "[LIVE-PB] --- draw context, dwords %u..%u ---\n", s, pos + count);
                    for (k = s; k <= pos + count && k < num_dwords; k++)
                        fprintf(stderr, "[LIVE-PB]   +%-4u 0x%08X%s\n", k, data[k],
                                (k == pos) ? "   <== this header" : "");
                    fflush(stderr);
                }
            }
            for (i = 0; i < count; i++) {
                hist_note(method + i * 4);
                if (!pgraph_d3d11_method((int)subchannel, method + i * 4, data[pos + 1 + i]))
                    note_unhandled(method + i * 4);
                g_methods++;
            }
            g_dw_cmd += 1 + count;
            pos += 1 + count;
        }
        else if ((header & PB_NONINC_MASK) == PB_NONINC_MATCH) {
            uint32_t count      = (header >> 18) & 0x7FF;
            uint32_t method     = header & 0x1FFC;
            uint32_t subchannel = (header >> 13) & 7;
            uint32_t i;

            if (count == 0 || pos + 1 + count > num_dwords) { g_dw_unknown++; pos++; continue; }

            /* Same trace on the non-incrementing path: DRAW_ARRAYS repeats one
             * method over many parameters, so this is where it actually lands. */
            if (method == 0x1810) {
                static int traced = 0;
                if (traced < 2) {
                    uint32_t s = (pos >= 64) ? pos - 64 : 0, k;
                    traced++;
                    fprintf(stderr, "[LIVE-PB] --- draw context (non-inc), dwords %u..%u ---\n", s, pos + count);
                    for (k = s; k <= pos + count && k < num_dwords; k++)
                        fprintf(stderr, "[LIVE-PB]   +%-4u 0x%08X%s\n", k, data[k],
                                (k == pos) ? "   <== this header" : "");
                    fflush(stderr);
                }
            }
            for (i = 0; i < count; i++) {
                hist_note(method);
                if (!pgraph_d3d11_method((int)subchannel, method, data[pos + 1 + i]))
                    note_unhandled(method);
                g_methods++;
            }
            g_dw_cmd += 1 + count;
            pos += 1 + count;
        }
        else {
            /* Jump/call/return headers and anything else we do not model yet.
             * Skipping one dword keeps the parser in step with the ring rather
             * than aborting the whole batch. */
            {
                /* Report where a run of commands first stops making sense. A
                 * total of "122880 unrecognised" says nothing about whether the
                 * producer wrote data into the ring, the parser lost sync, or
                 * the pointer crossed a region that was never filled. The
                 * address and the offending dword separate those. */
                static int told = 0;
                if (told < 6) {
                    told++;
                    fprintf(stderr, "[LIVE-PB] not a command at +%u dwords into the batch: "
                                    "0x%08X (next 0x%08X 0x%08X)\n",
                            pos, header,
                            (pos + 1 < num_dwords) ? data[pos + 1] : 0,
                            (pos + 2 < num_dwords) ? data[pos + 2] : 0);
                    if (pos >= 4)
                        fprintf(stderr, "[LIVE-PB]   preceding: 0x%08X 0x%08X 0x%08X 0x%08X\n",
                                data[pos - 4], data[pos - 3], data[pos - 2], data[pos - 1]);
                    fflush(stderr);
                }
            }
            g_dw_unknown++;
            pos++;
        }
    }
}

void nv2a_live_pb_tick(uint8_t *mem_base)
{
    uint32_t ctx, wp, limit;

    /* Occlusion reports whose queries completed since the last tick. */
    if (g_ready) pgraph_d3d11_poll_reports();
    {   /* new round = new texture signatures */
        extern void pgraph_d3d11_tick_begin(void);
        pgraph_d3d11_tick_begin();
    }

    if (!nv2a_live_pb_enabled() || g_failed || !mem_base)
        return;

    /* The translator needs the same window to dereference vertex streams. */
    pgraph_d3d11_set_mem_base(mem_base, XBOX_GUEST_RAM_BYTES);

    ctx = *(volatile uint32_t *)(mem_base + GPU_CONTEXT_PTR_VA);
    if (!ctx)
        return;

    wp    = *(volatile uint32_t *)(mem_base + ctx + CTX_WRITE_PTR_OFF);
    limit = *(volatile uint32_t *)(mem_base + ctx + CTX_LIMIT_OFF);
    if (!wp || !limit || limit <= wp)
        return;

    if (!g_ready) {
        uint32_t base = *(volatile uint32_t *)(mem_base + ctx + CTX_BUF_BASE_OFF);
        uint32_t end  = *(volatile uint32_t *)(mem_base + ctx + CTX_BUF_END_OFF);

        if (!live_pb_bring_up()) { g_failed = 1; return; }
        g_ready = 1;

        /*
         * Start from the buffer's real base, not from wherever the write
         * pointer happens to be. The title fills the push buffer during
         * device init and its first frame, which is *before* this consumer
         * comes up -- starting at the current write pointer silently discarded
         * all of it, which is why draws never appeared even though the title
         * really does emit NV097_SET_BEGIN_END (it writes the 0x000417FC
         * header in sub_00169A00).
         */
        if (base && end > base && wp >= base && wp <= end) {
            g_pb_base  = base;
            g_pb_end   = end;
            g_pb_limit = limit;
            g_last_wp  = base;    /* replay what is already buffered */
        } else {
            /* Extent did not look sane -- fall back to the old behaviour
             * rather than parse from a wild pointer. */
            g_pb_base  = wp;
            g_pb_end   = limit;
            g_pb_limit = limit;
            g_last_wp  = wp;
        }
        /* Replay the init-time capture before following the write pointer.
         * Everything the title programmed once -- texture stages, vertex
         * formats, surface state -- lives only here; the live ring has been
         * recycled since. */
        if (g_init_capture && g_init_capture_fill) {
            fprintf(stderr, "[LIVE-PB] replaying %u captured init dwords\n",
                    g_init_capture_fill);
            fflush(stderr);
            live_pb_parse(g_init_capture, g_init_capture_fill);
            g_dwords_seen += g_init_capture_fill;
            g_batches++;
        }

        g_last_present = GetTickCount();
        fprintf(stderr,
                "[LIVE-PB] ring: base=0x%08X end=0x%08X limit=0x%08X wp=0x%08X"
                " (%u KB extent, %u KB in use)\n",
                g_pb_base, g_pb_end, g_pb_limit, wp,
                (g_pb_end - g_pb_base) / 1024u,
                (g_pb_limit > g_pb_base) ? (g_pb_limit - g_pb_base) / 1024u : 0u);
        fflush(stderr);
        return;
    }

    /*
     * Refuse a write pointer that is not inside the ring.
     *
     * The pointer is guest memory like any other, so a stray write can leave
     * garbage in it -- observed as 0xB3B8B3B7, which the parser then happily
     * walked 749 million dwords from. Nothing downstream can be right once the
     * pointer is wild, and reading 1.4 GB of unrelated address space is how a
     * diagnostic turns into a crash of its own. Report once and stop tracking
     * rather than guess.
     */
    if (wp < g_pb_base || wp > g_pb_end) {
        if (!g_wp_wild) {
            g_wp_wild = 1;
            fprintf(stderr, "[LIVE-PB] write pointer left the ring: 0x%08X not in "
                            "0x%08X..0x%08X -- the channel context has been "
                            "overwritten; stopping\n", wp, g_pb_base, g_pb_end);
            fflush(stderr);
        }
        return;
    }

    if (wp != g_last_wp) {
        uint32_t prev_wp = g_last_wp;
        uint32_t n = 0, wrapped = 0;

        /*
         * The wrap point is the context's own limit field, which the producer
         * moves as it hands out segments -- NOT the end of the allocation.
         * Using the extent end made a single legitimate wrap replay the whole
         * two-megabyte ring, of which only the first few hundred dwords had
         * ever been written; the other half-million dwords were stale heap
         * bytes that happened to decode as 312 distinct "methods". That is
         * where "524766 dwords -> 1238 methods" came from, and it is why the
         * unhandled-method list was full of registers the title never touched.
         */
        uint32_t old_limit = g_pb_limit;
        if (limit > g_pb_base && limit <= g_pb_end)
            g_pb_limit = limit;

        if (wp > g_last_wp) {
            n = (wp - g_last_wp) / 4;
            live_pb_parse((const uint32_t *)(mem_base + g_last_wp), n);
        } else if (g_last_wp + WRAP_SLACK >= g_pb_limit) {
            /* A genuine wrap: the producer ran out of room before the limit
             * and continued at the base. Finish the tail, then the head. */
            uint32_t bound = g_pb_end;
            uint32_t tail = 0, jump = 0;
            uint32_t head = (wp > g_pb_base) ? (wp - g_pb_base) / 4 : 0;
            if (bound > g_last_wp)
                tail = live_pb_tail_len((const uint32_t *)(mem_base + g_last_wp),
                                        (bound - g_last_wp) / 4, &jump);
            {
                static int told = 0;
                if (told < 2 || (!jump && told < 20)) {
                    told++;
                    fprintf(stderr, "[LIVE-PB] wrap at 0x%08X: %u tail dwords up to %s 0x%08X "
                                    "(limit was 0x%08X, now 0x%08X)\n",
                            g_last_wp, tail, jump ? "jump" : "stop", jump ? jump :
                            ((const uint32_t *)(mem_base + g_last_wp))[tail], old_limit, g_pb_limit);
                }
            }
            /* No jump means this was not a wrap the walk could follow: parse
             * nothing rather than replay stale commands from an older pass. */
            if (!jump) tail = 0;
            if (tail) live_pb_parse((const uint32_t *)(mem_base + g_last_wp), tail);
            if (head) live_pb_parse((const uint32_t *)(mem_base + g_pb_base), head);
            n = tail + head;
            wrapped = 1;
        } else {
            /*
             * A segment reset, which looks identical to a wrap but is not one.
             * The driver rewinds the write pointer once the GPU has drained
             * the segment, and our PFIFO pump reports everything consumed
             * instantly, so this title rewinds constantly -- from five KB into
             * a 127 KB segment, nowhere near the limit. Treating it as a wrap
             * replayed the whole unwritten remainder of the segment as
             * commands. Everything below the new pointer has already been
             * parsed, so there is nothing to read here.
             */
            n = 0;
            g_resets++;
        }
        g_dwords_seen += n;
        g_last_wp = wp;
        g_batches++;
        {   /* XBOX_FLIP_LOG=1: every batch, timestamped */
            static int blog = -1;
            if (blog < 0) { const char *e = getenv("XBOX_FLIP_LOG"); blog = e && e[0] == '1'; }
            if (blog) {
                LARGE_INTEGER q, f;
                QueryPerformanceCounter(&q);
                QueryPerformanceFrequency(&f);
                fprintf(stderr, "[BATCH] t=%.1f %08X -> %08X %s%u dwords limit %08X\n",
                        (double)q.QuadPart * 1000.0 / (double)f.QuadPart, prev_wp, wp,
                        wrapped ? "wrap " : (n == 0 ? "RESET " : ""), n, g_pb_limit);
            }
        }

        if (g_batches <= 12 || (g_batches % 200) == 0)
            fprintf(stderr, "[LIVE-PB] batch %u: 0x%08X -> 0x%08X %s%u dwords"
                            " (limit 0x%08X)\n",
                    g_batches, prev_wp, wp,
                    wrapped ? "wrapped, " : (n == 0 ? "segment reset, " : ""),
                    n, g_pb_limit);
    }

    {
        DWORD now = GetTickCount();
        /* Present when the title flips its render surface. The 16 ms timer stays
         * only as a fallback: without it a title that never flips (or a stall)
         * would freeze the window and stop pumping messages. Driving purely off
         * the timer put Present between a frame's clear and its draw, so a
         * cleared black buffer is what reached the screen. */
        /* Keep the window responsive on every tick, but never present here:
         * the frame boundary (a CLEAR_SURFACE starting the next frame) is what
         * drives Present, in pgraph_d3d11_method. Presenting on a timer showed
         * the buffer mid-frame -- cleared but not yet drawn -- so the screen
         * was black while correct geometry was being submitted. */
        d3d8_PumpMessages();

        /* Stats reporting runs on its own timer, not inside the present
         * path. Presenting moved to the title's frame boundary, which made
         * that block rare, and the counters silently stopped being reported
         * -- which reads as a regression when only the metric moved. */
          {
                static DWORD last_report = 0;
                if (now - last_report >= 5000) {
                    PgraphD3D11Stats st;
                    last_report = now;
                    pgraph_d3d11_get_stats(&st);
                    fprintf(stderr,
                            "[LIVE-PB] %llu dwords in %u batches -> %u methods; "
                            "draws=%u verts=%u clears=%u ignored=%u\n",
                            (unsigned long long)g_dwords_seen, g_batches, g_methods,
                            st.draw_calls, st.vertices_submitted, st.clears,
                            st.methods_ignored);
                    nv2a_live_pb_report();
                    pgraph_d3d11_report_nops();
                    pgraph_d3d11_report_occlusion();
                    fflush(stderr);
                }
            }
        /* While the title is playing a video it stops issuing push-buffer
         * flips and writes pixels straight into guest VRAM instead, so the
         * only thing pacing the window is this fallback. At 500 ms that shows
         * about 7 of the intro's 60 frames -- the video decodes at a correct
         * ~30 fps (measured: 60 blits, mean interval 32.8 ms) and almost all of
         * it was being thrown away before it reached the screen. Present at
         * video rate while a guest framebuffer is registered. */
        {
            unsigned fallback_ms = d3d8_HasGuestFramebuffer()
                                 ? 16u : (unsigned)PRESENT_FALLBACK_MS;
            /* While the title flips surfaces, presents happen at the flips
             * (pgraph_d3d11_flipping); a timer present lands mid-frame. */
            if (d3d8_MsSincePresent() >= fallback_ms && !pgraph_d3d11_flipping()) {
            g_last_present = now;
            pgraph_d3d11_flush();

            /* XBOX_TEST_QUAD=1 draws a known-good magenta quad through the exact
             * same device the translator uses, immediately before Present. The
             * translator's own geometry, state and pipeline bindings have all
             * been verified correct while the back buffer still reads pure
             * black, so this separates 'the data is wrong' from 'nothing this
             * device draws ever lands'. Diagnostic only; off by default. */
            {
                static int want = -1;
                if (want < 0) {
                    const char *e = getenv("XBOX_TEST_QUAD");
                    want = (e && e[0] == '1') ? 1 : 0;
                }
                if (want)
                    pgraph_d3d11_test_quad();
            }

            /* Lift the guest framebuffer into the frame if asked. The video
             * player writes decoded pixels straight into guest VRAM, bypassing
             * the push buffer entirely, so without this they never reach the
             * host swap chain. */
            {
                static int checked = 0;
                static uint32_t fb_va = 0;
                if (!checked) {
                    const char *e = getenv("XBOX_GUEST_FB");
                    const char *e2 = getenv("XBOX_GUEST_FB2");
                    checked = 1;
                    if (e && e[0])
                        fb_va = (uint32_t)strtoul(e, NULL, 0);
                    if (e2 && e2[0]) {
                        extern uint32_t g_guest_fb_alt_va;
                        g_guest_fb_alt_va = (uint32_t)strtoul(e2, NULL, 0);
                    }
                }
                if (fb_va)
                    pgraph_d3d11_present_guest_fb(fb_va, 0xA00, 640, 480);
            }

            d3d8_PresentFrame();

            }
        }
    }
}
