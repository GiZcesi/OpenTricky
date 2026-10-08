/*
 * pumplag -- mesh parts translated after the title rewrote their constant
 * block (diagnostic). See pumplag.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "recomp/recomp_types.h"
#include "pumplag.h"

void sub_000FFDC0(void);    /* MeshRenderer_DrawPartsList (thiscall: first part; ctx; ret 4) */
void sub_000FFEE0(void);    /* Grid record draw (thiscall: record object; ctx; ret 4) */
void sub_001013D0(void);    /* queue one mesh part: takes its block through Lock (thiscall) */
void sub_001017E0(void);    /* queue a matrix record (thiscall, its block from the 2-set ring) */
void sub_0016B920(void);    /* pushbuffer: guarantees wp < limit (stdcall, ret 4) */
void sub_0016B890(void);    /* D3D BlockOnResource (stdcall: resource; ret 4) */
unsigned d3d8_PresentSeq(void);
int d3d8_pump_alt_off(void);
typedef int (*pgraph_plag_fn)(uint32_t q, uint32_t *cb, const uint8_t **snap);
void pgraph_d3d11_set_plag_check(pgraph_plag_fn fn);

#define GPU_CTX_VA      0x001776C0u /* D3D push-buffer context */
#define GPU_PRODUCER    0x2B60u     /* ... frames submitted (Swap 0x1688D0) */
#define GPU_CONSUMER    0x2518u     /* ... frames the GPU finished (the pump) */
#define CTX_SET2        0x196008u   /* render context: ring index mod 2 */
#define CTX_SET4        0x196010u   /* ... ring index mod 4 (mesh part blocks) */
#define CTX_SLOT        0x1A788u    /* ... blocks taken this frame */
#define CTX_BLK2        0x1A790u    /* ... block array: 4 x 0x6A4 blocks of 0xE0 */
#define CTX_HDR         0x1A858u    /* ... each block's D3D vertex buffer header (block + 0xC8) */
#define BLK_SIZE        0xE0u
#define BLK_SET         0x6A4u      /* blocks per set */
#define PART_NEXT       0x04u
#define PART_CONSTS     0x5Cu       /* part: its constant block; +0x80 = transposed WVP */
#define MVP_OFF         0x80u

int g_pumplag;
static int s_fix = -1;              /* XBOX_FIX_CONSTWAIT */
static struct { unsigned long long calls, blocked, skipped, gwait, gtimeout, pcalls; double ms, max_ms, pms; } s_fx;
static uint32_t s_gstamp[4u * 0x6A4u];   /* block -> draw fence after its last Grid draw (0 = none) */

/* Snapshot of each part's block when the title draws it, by sequence number
 * (marker 0x5D000000 | q, q != 0). Written by the game thread before the
 * marker is published, read by the pump thread when it reaches the marker. */
#define SEQ_N 16384u
static struct {
    uint32_t q, cb, producer, consumer, set4, slot, kind;   /* kind 0 mesh part, 1 Grid record */
    uint32_t blk[BLK_SIZE / 4u];
} s_e[SEQ_N];
static uint32_t s_seq = 1;

/* Blocks written through 0x1017E0 (the 2-set ring), most recent last. */
#define WR_N 4096u
static struct { uint32_t va, producer; } s_wr[WR_N];
static volatile uint32_t s_wr_n;

static struct {
    unsigned long long parts, bad, block_only, lag[8], refused, wr;
    unsigned long long bad_lag[8], bad_by_wr, bad_no_wr, grid, grid_bad;
} s_cnt;

static uint32_t producer_now(void)
{
    uint32_t g = MEM32(GPU_CTX_VA);
    return g ? MEM32(g + GPU_PRODUCER) : 0u;
}

static int emit_marker(uint32_t tag)
{
    uint32_t ctx = MEM32(GPU_CTX_VA), wp, esp0, sv_eax = g_eax, sv_ecx = g_ecx, sv_edx = g_edx;
    if (!ctx || (MEM32(ctx + 0x0Cu) & 4u)) { s_cnt.refused++; return 0; }
    esp0 = g_esp;
    PUSH32(g_esp, ctx);
    PUSH32(g_esp, 0);
    sub_0016B920();
    g_esp = esp0;
    wp = g_eax;
    MEM32(wp) = 0x00040100u;            /* NV097_NO_OPERATION, 1 parameter */
    MEM32(wp + 4u) = tag;
    MEM32(ctx) = wp + 8u;
    g_eax = sv_eax; g_ecx = sv_ecx; g_edx = sv_edx;
    return 1;
}

/* Runs the original on part p alone (its link cut for the call, then put back). */
static void draw_part(uint32_t p, uint32_t ctx)
{
    uint32_t next = MEM32(p + PART_NEXT);
    MEM32(p + PART_NEXT) = 0;
    PUSH32(g_esp, ctx);
    PUSH32(g_esp, 0);
    g_ecx = p;
    sub_000FFDC0();                     /* ret 4: pops both */
    MEM32(p + PART_NEXT) = next;
}

/* 0xFFDC0, thiscall (first part; ctx), ret 4. Diagnostic on: each part is
 * drawn alone, after its block is copied and its marker pushed. */
/* Copies the block cb and returns its sequence number (game thread). */
static uint32_t snap(uint32_t cb, uint32_t ctx, uint32_t kind)
{
    uint32_t q = s_seq++ & 0xFFFFFFu, k, g = MEM32(GPU_CTX_VA);
    if (!q) q = s_seq++ & 0xFFFFFFu;
    k = q & (SEQ_N - 1u);
    s_e[k].cb = cb;
    s_e[k].producer = producer_now();
    s_e[k].consumer = g ? MEM32(g + GPU_CONSUMER) : 0u;
    s_e[k].set4 = MEM32(ctx + CTX_SET4);
    s_e[k].slot = (cb - (ctx + CTX_BLK2)) / BLK_SIZE;
    s_e[k].kind = kind;
    memcpy(s_e[k].blk, (const void *)XBOX_PTR(cb), BLK_SIZE);
    s_e[k].q = q;
    return q;
}

void hook_plag_000FFDC0(void)
{
    uint32_t part = g_ecx, esp0 = g_esp, ctx = MEM32(esp0 + 4u), p;
    if (!g_pumplag) { sub_000FFDC0(); return; }
    g_esp = esp0 + 8u;                  /* the caller's frame: dummy return + ctx popped */
    for (p = part; p >= 0x1000u; ) {
        uint32_t next = MEM32(p + PART_NEXT), cb = MEM32(p + PART_CONSTS);
        if (cb >= 0x1000u) {
            s_cnt.parts++;
            emit_marker(0x5D000000u | snap(cb, ctx, 0));
        }
        draw_part(p, ctx);
        p = next;
    }
    emit_marker(0x5D000000u);
    g_esp = esp0 + 8u;
}

/* Draw fences for the Grid blocks (XBOX_FIX_CONSTWAIT): after each Grid
 * record's draw, a NOP 0x5C000000 | n is pushed and the block remembers n;
 * the translator publishes the last n it has passed
 * (pgraph_d3d11_fork_fence). The block may be rewritten once that is >= n. */
extern volatile uint32_t pgraph_d3d11_fork_fence;
static uint32_t s_fseq;

/* 0xFFEE0 (vtable 0x1A2AC4 slot 0), thiscall (record object; ctx), ret 4:
 * draws a record queued by 0x1017E0, its block ([obj+0x18]) bound as stream 1
 * through the block's vertex buffer header (block + 0xC8). The draw comes
 * after the next Swap: a block taken on frame N is drawn on frame N+1 and,
 * through the mod-2 index, taken again on frame N+2. Diagnostic on: the
 * block is copied and marked like a mesh part's. */
void hook_plag_000FFEE0(void)
{
    uint32_t obj = g_ecx, ctx = MEM32(g_esp + 4u), cb = MEM32(obj + 0x18u);
    uint32_t r = (cb - (ctx + CTX_BLK2)) / BLK_SIZE;
    if (g_pumplag && cb >= 0x1000u) {
        s_cnt.grid++;
        emit_marker(0x5D000000u | snap(cb, ctx, 1));
        sub_000FFEE0();
        emit_marker(0x5D000000u);
    } else
        sub_000FFEE0();
    if (s_fix > 0 && cb >= 0x1000u && r < 4u * BLK_SET) {
        uint32_t n = ++s_fseq & 0xFFFFFFu;
        if (!n) n = ++s_fseq & 0xFFFFFFu;
        if (emit_marker(0x5C000000u | n)) s_gstamp[r] = n;
        else s_gstamp[r] = 0;
    }
}

/* Waits until the draw fence of the Grid record last drawn with block r has
 * been passed by the translator (at most 250 ms). */
static void grid_fence_wait(uint32_t r)
{
    uint32_t stamp = s_gstamp[r];
    LARGE_INTEGER t0, t, fq;
    unsigned spins = 0;
    if (!stamp || (int32_t)((pgraph_d3d11_fork_fence << 8) - (stamp << 8)) >= 0) return;
    s_fx.gwait++;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&fq);
    for (;;) {
        if ((int32_t)((pgraph_d3d11_fork_fence << 8) - (stamp << 8)) >= 0) break;
        QueryPerformanceCounter(&t);
        if ((double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart > 250.0) { s_fx.gtimeout++; break; }
        if (++spins < 64u) SwitchToThread(); else Sleep(0);
    }
    QueryPerformanceCounter(&t);
    {
        double ms = (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
        s_fx.pms += ms;
        if (ms > s_fx.max_ms) s_fx.max_ms = ms;
    }
}

/* 0x1013D0, thiscall, one direct caller (0x10416E): queues a mesh part and
 * takes block r = [ctx+0x196010] * 0x6A4 + [ctx+0x1A788] through Lock. Lock's
 * BlockOnResource relies on the header's stamp, which for a Grid record can
 * precede its draw (see 0x1017E0 below): XBOX_FIX_CONSTWAIT=1 first waits
 * for the Grid draw fence of that block. */
void hook_plag_001013D0(void)
{
    uint32_t ctx = g_ecx;
    if (s_fix > 0 && !d3d8_pump_alt_off()) {
        uint32_t r = MEM32(ctx + CTX_SET4) * BLK_SET + MEM32(ctx + CTX_SLOT);
        if (r < 4u * BLK_SET && s_gstamp[r]) { s_fx.pcalls++; grid_fence_wait(r); }
    }
    sub_001013D0();
}

/* 0x1017E0, thiscall: queues a Grid record; takes block
 * r = [ctx+0x196008] * 0x6A4 + [ctx+0x1A788] of the block array -- the
 * array the mesh parts take theirs from with [ctx+0x196010] (mod 4) and the
 * same per-frame counter -- and writes its matrix there, without D3D Lock.
 * With a mod-2 index it lands on blocks that earlier parts and Grid records
 * were drawn with, possibly not yet drawn by the pump.
 *
 * XBOX_FIX_CONSTWAIT=1: before the write, wait until the GPU (the pump) is
 * done with the block:
 *  1. BlockOnResource (0x16B890) on the block's vertex buffer header, as the
 *     parts' Lock (0x16B070) does: covers the mesh parts.
 *  2. A Grid record's header is only stamped when stream 1 is unbound, with
 *     the D3D's last kicked PUT ([gpu+0x1C], 0x16A3FB), which can precede the
 *     draw, so step 1 can return early for it: wait for the draw fence pushed
 *     after its last draw (see above).
 * XBOX_FIX_PUMP_ALT=1 (pump_ident): only on every other pump frame. */
void hook_plag_001017E0(void)
{
    uint32_t ctx = g_ecx;
    uint32_t r = MEM32(ctx + CTX_SET2) * BLK_SET + MEM32(ctx + CTX_SLOT);
    if (s_fix > 0 && r < 4u * BLK_SET && !d3d8_pump_alt_off()) {
        uint32_t hdr = ctx + CTX_HDR + r * BLK_SIZE, data = ctx + CTX_BLK2 + r * BLK_SIZE;
        uint32_t stamp = s_gstamp[r];
        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceCounter(&t0);
        QueryPerformanceFrequency(&fq);
        s_fx.calls++;
        if (((MEM32(hdr + 4u) ^ data) & 0x0FFFFFFFu) == 0) {
            uint32_t esp0 = g_esp, sv_eax = g_eax, sv_ecx = g_ecx, sv_edx = g_edx;
            PUSH32(g_esp, hdr);
            PUSH32(g_esp, 0);           /* dummy return */
            sub_0016B890();             /* stdcall, ret 4 */
            g_esp = esp0;
            g_eax = sv_eax; g_ecx = sv_ecx; g_edx = sv_edx;
        } else s_fx.skipped++;
        if (stamp) grid_fence_wait(r);  /* 24-bit fences, compared in the top bits */
        QueryPerformanceCounter(&t1);
        {
            double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
            s_fx.ms += ms;
            if (ms > s_fx.max_ms) s_fx.max_ms = ms;
            if (ms > 0.05) s_fx.blocked++;
        }
        {
            static unsigned last;
            unsigned f = d3d8_PresentSeq();
            if (f - last >= 3000u) {
                last = f;
                fprintf(stderr, "[CONSTWAIT] f%u calls %llu waited > 0.05 ms %llu (total %.1f ms, max %.2f ms), "
                        "grid fence waits %llu (timeouts %llu; parts on a Grid block %llu, %.1f ms in fence waits), "
                        "header mismatch %llu\n", f, s_fx.calls, s_fx.blocked, s_fx.ms, s_fx.max_ms, s_fx.gwait,
                        s_fx.gtimeout, s_fx.pcalls, s_fx.pms, s_fx.skipped);
            }
        }
    }
    sub_001017E0();
    if (g_pumplag) {
        uint32_t n = s_wr_n;
        s_wr[n & (WR_N - 1u)].va = ctx + CTX_BLK2 + r * BLK_SIZE;
        s_wr[n & (WR_N - 1u)].producer = producer_now();
        s_wr_n = n + 1u;
        s_cnt.wr++;
    }
}

/* Pump thread, at marker q: 1 if the part's matrix changed since the title
 * drew the part (the draws that follow use another part's matrix), 0 if not,
 * -1 if unknown. */
static int plag_check(uint32_t q, uint32_t *cb_out, const uint8_t **snap_out)
{
    static unsigned printed, last_report;
    uint32_t k = q & (SEQ_N - 1u), cb, now, lag, i, mvp_diff = 0, blk_diff = 0, first = 99, last = 0;
    unsigned f = d3d8_PresentSeq();
    if (s_e[k].q != q) return -1;
    cb = s_e[k].cb;
    now = producer_now();
    lag = now - s_e[k].producer;
    s_cnt.lag[lag > 7u ? 7u : lag]++;
    if (lag >= 3u) {
        static unsigned lp;
        uint32_t g = MEM32(GPU_CTX_VA);
        if (lp++ < 60u)
            fprintf(stderr, "[PLAG] LAG%u f%u q%u %s block %08X: Swap count then %u (GPU done %u), now %u (GPU done %u)\n",
                    lag, f, q, s_e[k].kind ? "grid" : "part", s_e[k].cb, s_e[k].producer, s_e[k].consumer,
                    now, g ? MEM32(g + GPU_CONSUMER) : 0u);
    }
    for (i = 0; i < BLK_SIZE / 4u; i++)
        if (MEM32(cb + 4u * i) != s_e[k].blk[i]) {
            blk_diff++;
            if (i >= MVP_OFF / 4u && i < MVP_OFF / 4u + 16u) {
                mvp_diff++;
                if (first == 99) first = i - MVP_OFF / 4u;
                last = i - MVP_OFF / 4u;
            }
        }
    if (f - last_report >= 600u) {
        last_report = f;
        fprintf(stderr, "[PLAG] f%u parts %llu rewritten %llu (block only %llu; by 0x1017E0 %llu, other %llu) "
                "lag 0..7+ %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu; 0x1017E0 blocks %llu; refused %llu; "
                "grid draws %llu rewritten %llu\n",
                f, s_cnt.parts, s_cnt.bad, s_cnt.block_only, s_cnt.bad_by_wr, s_cnt.bad_no_wr,
                s_cnt.lag[0], s_cnt.lag[1], s_cnt.lag[2], s_cnt.lag[3], s_cnt.lag[4], s_cnt.lag[5],
                s_cnt.lag[6], s_cnt.lag[7], s_cnt.wr, s_cnt.refused, s_cnt.grid, s_cnt.grid_bad);
    }
    if (!mvp_diff) {
        if (blk_diff) s_cnt.block_only++;
        return 0;
    }
    if (s_e[k].kind) s_cnt.grid_bad++;
    else s_cnt.bad++;
    s_cnt.bad_lag[lag > 7u ? 7u : lag]++;
    {   /* who wrote over the matrix: a 0x1017E0 block overlapping it, since the snapshot */
        uint32_t n = s_wr_n, j, hit_va = 0, hit_prod = 0, lo = cb + MVP_OFF, hi = cb + MVP_OFF + 0x40u;
        for (j = 0; j < WR_N && j < n; j++) {
            uint32_t e = (n - 1u - j) & (WR_N - 1u), m0 = s_wr[e].va + MVP_OFF, m1 = m0 + 0x40u;
            if ((int32_t)(s_wr[e].producer - s_e[k].producer) < 0) break;
            if (m0 < hi && lo < m1) { hit_va = s_wr[e].va; hit_prod = s_wr[e].producer; break; }
        }
        if (hit_va) s_cnt.bad_by_wr++; else s_cnt.bad_no_wr++;
        if (printed++ < 400u)
            fprintf(stderr, "[PLAG] REWRITTEN f%u %s q%u block %08X set4 %u slot %u: title %u frame(s) ahead "
                    "(Swap count then %u, now %u, GPU done %u); matrix dwords %u..%u of 16 differ, block %u of 56; "
                    "writer %s %08X (Swap count %u)\n",
                    f, s_e[k].kind ? "grid" : "part", q, cb, s_e[k].set4, s_e[k].slot, lag, s_e[k].producer, now,
                    MEM32(MEM32(GPU_CTX_VA) + GPU_CONSUMER), first, last, blk_diff,
                    hit_va ? "0x1017E0 block" : "unknown", hit_va, hit_prod);
    }
    *cb_out = cb;
    *snap_out = (const uint8_t *)s_e[k].blk;
    return 1;
}

void pumplag_init(void)
{
    const char *e = getenv("XBOX_PUMPLAG");
    const char *x = getenv("XBOX_FIX_CONSTWAIT");
    s_fix = !(x && x[0] == '0');
    fprintf(stderr, "[CONSTWAIT] XBOX_FIX_CONSTWAIT=%d\n", s_fix);
    g_pumplag = e && (e[0] == '1' || e[0] == '2') ? e[0] - '0' : 0;
    if (g_pumplag) {
        pgraph_d3d11_set_plag_check(plag_check);
        fprintf(stderr, "[PLAG] on: every mesh part's constant block checked when the pump draws it\n");
    }
}

void (*pumplag_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x000FFEE0u && (g_pumplag || s_fix > 0)) return hook_plag_000FFEE0;
    if (xbox_va == 0x001013D0u) return hook_plag_001013D0;   /* direct call: always routed */
    if (!g_pumplag) return 0;
    if (xbox_va == 0x000FFDC0u) return hook_plag_000FFDC0;
    if (xbox_va == 0x001017E0u) return hook_plag_001017E0;
    return 0;
}
