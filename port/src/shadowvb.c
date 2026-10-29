/*
 * shadowvb -- a rider part whose dynamic vertex buffers cannot be allocated
 * is not drawn (its writer is skipped). See shadowvb.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
#include "recomp/recomp_types.h"
#include "shadowvb.h"

void sub_000FE100(void);   /* ctx::AllocVBSlot(size, dynamic, 4): thiscall, ret 0xC, returns the slot */
void sub_000FE260(void);   /* ctx::FreeVBSlot(slot): thiscall, ret 4 */
void sub_00103900(void);   /* ctx: create the part's dynamic vertex buffers (part): thiscall, ret 4 */
void sub_001033D0(void);   /* ctx: per-frame writer + draw of the part's vertices (part, x): thiscall, ret 8 */
void sub_00150D70(void);   /* arena alloc(name, size, flags): cdecl, returns 0 when full */
void sub_00150950(void);   /* arena free(p): cdecl */

#define VB_SLOTS        0x200u
#define VB_DATA(ctx, slot, k) MEM32((ctx) + 0x18FE44u + 48u * (slot) + 16u * (k))
#define PART_SLOT       0x60u      /* the part's vertex buffer slot */
#define PART_EDGES      0x184u     /* the part's edge count (s16): buffers of edges * 128 bytes */
#define NAME_VB         0x001A2A40u  /* the game's own "VertexBuffer" allocation name */
#define BALLAST_MAX     4096

static int s_fix = -1;             /* XBOX_FIX_SHADOWVB: 1 (default) fix, 0 original; -1 not read yet */
static uint32_t s_test_every;      /* XBOX_TEST_SHADOWVB_FAIL: 0 off, N = every Nth part VB refused */
static uint32_t s_ntest;
static uint32_t s_creating;        /* part whose buffers 0x103900 is creating (until its slot is taken) */
static int s_refused;              /* that part's buffers were refused and it was given a size-0 slot */
static uint32_t s_refused_edges;
static uint32_t s_shadowless[VB_SLOTS];   /* slot -> part not drawn, writer skipped (0: none) */
static uint32_t s_nshadowless;
static uint32_t s_nlogged;
static uint32_t s_ballast[BALLAST_MAX];
static int s_nballast;

static void shadowvb_init(void)
{
    const char *x = getenv("XBOX_FIX_SHADOWVB");
    s_fix = (x && x[0] == '0') ? 0 : 1;
    s_test_every = 0u;
    if (!s_fix || s_test_every)
        fprintf(stderr, "[MEM] XBOX_FIX_SHADOWVB=%d, XBOX_TEST_SHADOWVB_FAIL=%u (test: every %uth rider part vertex buffer refused)\n",
                s_fix, s_test_every, s_test_every);
}

/* ---- test only: fill the arena so that the next allocation of `need` bytes fails ---- */
static uint32_t arena_alloc(uint32_t size, uint32_t ebp0)
{
    uint32_t esp0 = g_esp, p;
    PUSH32(g_esp, 0x40u); PUSH32(g_esp, size); PUSH32(g_esp, NAME_VB); PUSH32(g_esp, 0);
    g_seh_ebp = ebp0;
    sub_00150D70();
    p = g_eax;
    g_esp = esp0;
    return p;
}

static void arena_free(uint32_t p, uint32_t ebp0)
{
    uint32_t esp0 = g_esp;
    PUSH32(g_esp, p); PUSH32(g_esp, 0);
    g_seh_ebp = ebp0;
    sub_00150950();
    g_esp = esp0;
}

static void arena_fill(uint32_t need, uint32_t ebp0)
{
    uint32_t sz = 0x100000u, p;
    for (;;) {
        if (sz < need) sz = need;
        while (s_nballast < BALLAST_MAX && (p = arena_alloc(sz, ebp0)) != 0) s_ballast[s_nballast++] = p;
        if (sz == need || s_nballast >= BALLAST_MAX) break;
        sz >>= 1;
    }
}

static void arena_release(uint32_t ebp0)
{
    while (s_nballast > 0) arena_free(s_ballast[--s_nballast], ebp0);
}

/* ---- the hooks ---- */
void hook_shadowvb_00103900(void);
void hook_shadowvb_00103900(void)
{
    uint32_t part = MEM32(g_esp + 4u);
    if (s_fix < 0) shadowvb_init();
    if (!s_fix && !s_test_every) { sub_00103900(); return; }
    s_creating = part; s_refused = 0;
    sub_00103900();
    s_creating = 0;
    if (s_refused) {
        uint32_t keep_eax = g_eax, slot = MEM32(part + PART_SLOT);
        MEM16(part + PART_EDGES) = (uint16_t)s_refused_edges;    /* the callers use it; the writer is skipped */
        if (slot < VB_SLOTS) {
            if (!s_shadowless[slot]) s_nshadowless++;
            s_shadowless[slot] = part;
        }
        s_refused = 0;
        g_eax = keep_eax;
    }
}

void hook_shadowvb_000FE100(void);
void hook_shadowvb_000FE100(void)
{
    uint32_t esp0 = g_esp, ctx = g_ecx, ebp0 = g_seh_ebp, part = s_creating, size, dyn, a3, slot, slot0, edges;
    int filled = 0;
    if (!part) { sub_000FE100(); return; }
    s_creating = 0;                          /* 0x103900 takes one slot: this one */
    size = MEM32(esp0 + 4u); dyn = MEM8(esp0 + 8u); a3 = MEM32(esp0 + 12u);
    if (s_test_every && size && ++s_ntest % s_test_every == 0u) {
        arena_fill(size, ebp0);
        filled = s_nballast;
        g_esp = esp0; g_ecx = ctx;
    }
    g_seh_ebp = ebp0;
    sub_000FE100();
    slot = g_eax;
    if (filled) { arena_release(ebp0); g_eax = slot; }
    if (!s_fix || !size || slot >= VB_SLOTS
        || (VB_DATA(ctx, slot, 0) && (!dyn || (VB_DATA(ctx, slot, 1) && VB_DATA(ctx, slot, 2)))))
        return;                              /* allocated (or not ours to fix): the original result */

    /* Refused: release the slot (and any buffer that did get allocated), take a slot of size 0 with the
     * edge count at 0 while 0x103900 fills it (that loop is guarded), and skip the writer for the part. */
    edges = (uint32_t)(int32_t)(int16_t)MEM16(part + PART_EDGES);
    slot0 = slot;
    PUSH32(g_esp, slot0); PUSH32(g_esp, 0);
    g_ecx = ctx; g_seh_ebp = ebp0;
    sub_000FE260();
    MEM16(part + PART_EDGES) = 0;
    PUSH32(g_esp, a3); PUSH32(g_esp, dyn); PUSH32(g_esp, 0u); PUSH32(g_esp, 0);
    g_ecx = ctx; g_seh_ebp = ebp0;
    sub_000FE100();
    slot = g_eax;
    s_refused = 1; s_refused_edges = edges;
    if (s_nlogged++ == 0u || s_test_every)
        fprintf(stderr, "[MEM] rider part %08X: vertex buffers of 0x%X bytes (%u edges) refused, game arena full%s; "
                "this rider part is not drawn (slot %u -> size-0 slot %u)%s\n",
                part, size, edges, filled ? " (test filler)" : "", slot0, slot,
                s_test_every ? "" : " -- logged once");
    g_esp = esp0 + 16u;                      /* as the original returns: fake return address + 3 arguments */
    g_eax = slot;
}

void hook_shadowvb_000FE260(void);
void hook_shadowvb_000FE260(void)
{
    if (s_nshadowless) {
        uint32_t slot = MEM32(g_esp + 4u);
        if (slot < VB_SLOTS && s_shadowless[slot]) { s_shadowless[slot] = 0; s_nshadowless--; }
    }
    sub_000FE260();
}

void hook_shadowvb_001033D0(void);
void hook_shadowvb_001033D0(void)
{
    if (s_nshadowless) {
        uint32_t part = MEM32(g_esp + 4u), slot = MEM32(part + PART_SLOT);
        if (slot < VB_SLOTS && s_shadowless[slot] == part) {
            g_esp += 12u;                    /* skipped: ret 8 + fake return address */
            return;
        }
    }
    sub_001033D0();
}

void (*shadowvb_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x00103900u) return hook_shadowvb_00103900;
    if (xbox_va == 0x000FE100u) return hook_shadowvb_000FE100;
    if (xbox_va == 0x000FE260u) return hook_shadowvb_000FE260;
    if (xbox_va == 0x001033D0u) return hook_shadowvb_001033D0;
    return 0;
}
