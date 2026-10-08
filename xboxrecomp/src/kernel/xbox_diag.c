/*
 * xbox_diag -- in-process live diagnostics. See xbox_diag.h for the rationale.
 */

#include "xbox_diag.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>

/* ---- things we borrow from the rest of the runtime ---- */

extern ptrdiff_t g_xbox_mem_offset;

typedef void (*diag_func_t)(void);
extern diag_func_t recomp_lookup(uint32_t xbox_va);

/* Implemented in ssx_recomp/src/recomp_manual.c. Copies the deduped list of
 * unresolved indirect-call targets, in first-seen order. */
extern int recomp_icall_miss_list(uint32_t *out, int max);

/* Runtime probe control, implemented in ssx_recomp/src/recomp/recomp_probe.c.
 * Weak so this generic runtime still links for a title that does not build the
 * probe facility; the `probe` command reports that rather than failing to link. */
extern int  recomp_probe_arm(const char *spec)        __attribute__((weak));
extern void recomp_probe_clear(void)                  __attribute__((weak));
extern void recomp_probe_list(char *out, size_t cap)  __attribute__((weak));

/* xbox_heap_owner_of() is declared in xbox_memory_layout.h. */

/* Per-file I/O ledger, implemented in kernel_bridge.c. */
extern int xbox_file_ledger_get(int idx, const char **path,
                                unsigned long long *bytes, unsigned int *reads);

/* ---- guest memory access -------------------------------- */

/*
 * The pool descriptor table the CRT allocator indexes with (id & 0xF); each
 * slot holds a descriptor VA. A descriptor's free-list sentinel sits at +0x10.
 *
 * Block header, 16 bytes:  magic@0 ('MB' allocated / 'FB' free / 'BS' end),
 * flags@2, size@4, next@8, prev@0xC. A *free* block additionally stores its
 * free-list links in the first 8 bytes of its payload, i.e. at header+0x10
 * (next) and header+0x14 (prev), which is why a free-list node address is the
 * block header address.
 */
#define DIAG_POOL_TABLE_VA   0x00203BE0u
#define DIAG_POOL_SLOTS      16
#define DIAG_SENTINEL_OFF    0x10u
#define DIAG_FL_NEXT_OFF     0x10u
#define DIAG_FL_PREV_OFF     0x14u

#define DIAG_MAGIC_ALLOC     0x424Du   /* 'MB' */
#define DIAG_MAGIC_FREE      0x4246u   /* 'FB' */
#define DIAG_MAGIC_END       0x4253u   /* 'BS' */

static int diag_va_ok(uint32_t va, uint32_t len)
{
    if (!g_xbox_mem_offset) return 0;
    if (len == 0) return 0;
    va = xbox_fold_ram_alias(va);   /* heap pointers are 0x8xxxxxxx */
    if (va > 0xFFFFFFFFu - len) return 0;
    /* RAM, plus the MMIO apertures we map above it. */
    if ((uint64_t)va + len <= (uint64_t)XBOX_TOTAL_RAM) return 1;
    if (va >= 0xFD000000u) return 1;
    return 0;
}

static uint8_t  g8 (uint32_t va) { return *(volatile uint8_t  *)((uintptr_t)xbox_fold_ram_alias(va) + g_xbox_mem_offset); }
static uint16_t g16(uint32_t va) { return *(volatile uint16_t *)((uintptr_t)xbox_fold_ram_alias(va) + g_xbox_mem_offset); }
static uint32_t g32(uint32_t va) { return *(volatile uint32_t *)((uintptr_t)xbox_fold_ram_alias(va) + g_xbox_mem_offset); }


/* ---- write watches -------------------------------------- */
/*
 * "Which code wrote this guest address?" has been the most-asked question in
 * this port, and gdb's hardware watchpoints answered it wrongly more than once
 * (it blamed ntdll's heap for a write that a VirtualQuery sweep
 * proved could not have come from there). This does it in-process instead.
 *
 * Write-protect the page holding the address; the VEH catches the access
 * violation, reports the guest register state and the native return addresses
 * (feed them to addr2line to get the lifted function), then unprotects and sets
 * the trap flag so the instruction can complete. The following single-step
 * exception re-arms the protection.
 *
 * Page-granular, so expect hits from neighbouring addresses -- the report
 * prints the exact faulting VA so they are easy to tell apart.
 */
#define DIAG_MAX_WATCH 8

/*
 * Protection is page-granular, but the interesting address almost never is.
 * Watching one field of a heap object meant wading through every write to the
 * other 4,088 bytes of its page -- a memset walking the page in 0x20 steps
 * buried the one write that mattered under hundreds of reports. So the watch
 * records the exact range it was asked about and reports only faults that
 * touch it; the rest are let through silently.
 */
typedef struct {
    uint32_t page_va; DWORD old_prot; int active;
    uint32_t va;      /* exact address asked for */
    uint32_t len;     /* bytes of interest at that address */
} diag_watch;
static diag_watch g_watch[DIAG_MAX_WATCH];
static int g_watch_n = 0;
static volatile LONG g_watch_hits = 0;
static diag_watch *g_watch_pending = NULL;   /* re-arm after the single step */
static void *g_watch_pending_alias = NULL;    /* the exact view that faulted */
/* The report is built at the fault but printed after the single step, so it
 * can say what was stored -- and `watchnan` can drop every write that did
 * not store a NaN (finding which of thousands of matrix writes
 * first put a NaN into the race camera's view). */
static char     g_watch_rep[2048];
static uint32_t g_watch_rep_va = 0;
static int      g_watch_nan_only = 0;

static void *diag_native(uint32_t va) { return (void *)((uintptr_t)xbox_fold_ram_alias(va) + g_xbox_mem_offset); }

int xbox_diag_watch_add(uint32_t va) { return xbox_diag_watch_add_ex(va, 4); }

int xbox_diag_watch_add_ex(uint32_t va, uint32_t len)
{
    uint32_t page = va & ~0xFFFu;
    DWORD old = 0;
    int i;
    if (len == 0) len = 4;
    if (g_watch_n >= DIAG_MAX_WATCH) return -1;
    if (!diag_va_ok(va, 1)) return -2;
    for (i = 0; i < g_watch_n; i++) if (g_watch[i].page_va == page) return 1; /* already */
    if (!VirtualProtect(diag_native(page), 0x1000, PAGE_READONLY, &old)) return -3;
    /* Protect the same physical page in every RAM mirror too. The mirrors are
     * independent views of one file mapping, so they carry their own page
     * protections: a write through a mirror address hits the same memory and
     * never faults the base view. Watching only the base view silently missed
     * whatever was zeroing guest .text at 0x000FB030 -- the value changed
     * while the watch reported no writes at all. */
    {
        /* Walk the slots, not the count of mapped views: a slot in the
         * middle may have no view (or only part of one), and whatever else
         * occupies it must not have its protection changed. */
        int m, mirrors = xbox_GetMirrorSlotCount();
        size_t stride = xbox_GetMemorySize();
        for (m = 0; m < mirrors; m++) {
            DWORD mo = 0;
            void *alias = (void *)((uintptr_t)diag_native(page)
                                   + (uintptr_t)(m + 1) * stride);
            if (xbox_IsMirrorAddress(alias))
                VirtualProtect(alias, 0x1000, PAGE_READONLY, &mo);
        }
    }
    g_watch[g_watch_n].page_va = page;
    g_watch[g_watch_n].old_prot = old;
    g_watch[g_watch_n].active = 1;
    g_watch[g_watch_n].va = va;
    g_watch[g_watch_n].len = len;
    g_watch_n++;
    return 0;
}

/* Host code writing guest memory -- the file bridge's ReadFile -- fails on a
 * watched page instead of faulting into the handler (ReadFile returns
 * ERROR_NOACCESS), and a title whose file read fails can hang at boot: a
 * startup watch on a page that a load later reads into stalled the game at
 * its first frame. Bracket such writes: begin lifts protection on
 * every watched page the range overlaps, and says so when it covers the
 * watched field itself; end restores it. Returns whether anything was lifted. */
int xbox_diag_watch_host_write(uint32_t va, uint32_t len, int begin)
{
    int i, lifted = 0;
    if (!g_watch_n || !len) return 0;
    for (i = 0; i < g_watch_n; i++) {
        uint32_t pg = g_watch[i].page_va;
        DWORD old;
        if (!g_watch[i].active || va >= pg + 0x1000u || va + len <= pg) continue;
        VirtualProtect(diag_native(pg), 0x1000, begin ? g_watch[i].old_prot : PAGE_READONLY, &old);
        lifted = 1;
        if (begin && va < g_watch[i].va + g_watch[i].len && va + len > g_watch[i].va) {
            extern volatile unsigned g_last_loc;
            fprintf(stderr, "  [WATCH] host file read writes Xbox VA 0x%08X (buffer 0x%08X+%u, after loc_%08X)\n",
                    g_watch[i].va, va, len, (unsigned)g_last_loc);
            fflush(stderr);
        }
    }
    return lifted;
}

void xbox_diag_watch_clear(void)
{
    DWORD old;
    int i;
    for (i = 0; i < g_watch_n; i++)
        if (g_watch[i].active) {
            int m, mirrors = xbox_GetMirrorSlotCount();
            size_t stride = xbox_GetMemorySize();
            VirtualProtect(diag_native(g_watch[i].page_va), 0x1000,
                           g_watch[i].old_prot, &old);
            for (m = 0; m < mirrors; m++) {
                DWORD mo = 0;
                void *alias = (void *)((uintptr_t)diag_native(g_watch[i].page_va)
                                       + (uintptr_t)(m + 1) * stride);
                if (xbox_IsMirrorAddress(alias))
                    VirtualProtect(alias, 0x1000, g_watch[i].old_prot, &mo);
            }
        }
    g_watch_n = 0;
    g_watch_pending = NULL;
}

extern __thread uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp;
extern volatile unsigned g_main_loc;   /* stamped in Application_RunMainLoop */
extern volatile unsigned g_last_loc;   /* stamped across the generated code */

int xbox_diag_handle_fault(void *vep)
{
    EXCEPTION_POINTERS *ep = (EXCEPTION_POINTERS *)vep;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    /* Second half: the instruction has retired, put the guard back. */
    if (code == EXCEPTION_SINGLE_STEP && g_watch_pending) {
        if (g_watch_rep_va) {
            uint32_t v = *(volatile uint32_t *)diag_native(g_watch_rep_va);
            int nan = ((v & 0x7F800000u) == 0x7F800000u) && (v & 0x007FFFFFu);
            if (!g_watch_nan_only || nan) {
                InterlockedIncrement(&g_watch_hits);
                fputs(g_watch_rep, stderr);
                fprintf(stderr, "    stored: 0x%08X%s\n", v, nan ? " (NaN)" : "");
                fflush(stderr);
            }
            g_watch_rep_va = 0;
        }
        /* Re-arm the exact alias that faulted, not just the base view: the
         * write may have arrived through a RAM mirror, and putting the guard
         * back on the base page would leave that mirror writable. */
        VirtualProtect(g_watch_pending_alias ? g_watch_pending_alias
                                             : diag_native(g_watch_pending->page_va),
                       0x1000, PAGE_READONLY, &old);
        g_watch_pending = NULL;
        g_watch_pending_alias = NULL;
        return 1;
    }

    if (code != EXCEPTION_ACCESS_VIOLATION || g_watch_n == 0) return 0;
    {
        uintptr_t fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        /* Fold RAM mirrors back onto the base view. A mirror alias sits
         * (m+1)*g_memory_size above the base mapping of the same physical
         * page, which overflows 32 bits well before the last mirror -- so
         * truncating to uint32_t first made every mirror fault match no watch
         * at all, and an unhandled one takes the process down. Divide before
         * narrowing. */
        uintptr_t fdelta = fault - (uintptr_t)g_xbox_mem_offset;
        size_t    fstride = xbox_GetMemorySize();
        unsigned  fmirror = fstride ? (unsigned)(fdelta / fstride) : 0u;
        uint32_t  fva   = (uint32_t)(fstride ? (fdelta % fstride) : fdelta);
        uint32_t  page  = fva & ~0xFFFu;
        int is_write = (int)ep->ExceptionRecord->ExceptionInformation[0];
        int i;
        for (i = 0; i < g_watch_n; i++) {
            if (!g_watch[i].active || g_watch[i].page_va != page) continue;

            /* Same page, different field: let it through without a word.
             * Reporting the whole page is what made this tool unusable on a
             * heap object. The access still has to be single-stepped, so this
             * only suppresses the report, not the re-arm below. */
            if (fva + 4u <= g_watch[i].va || fva >= g_watch[i].va + g_watch[i].len)
                goto let_through;

            /*
             * Build the whole report first and emit it with one write. Two
             * guest threads can fault on the same page at once, and separate
             * fprintf calls interleave line by line -- which produced a
             * capture with two headers and one merged frame list, i.e. a
             * backtrace attributed to the wrong thread.
             */
            {
                char rep[2048];
                int p = 0;
                void *fr[20]; USHORT n, k;
                uintptr_t base = (uintptr_t)GetModuleHandle(NULL);

                n = CaptureStackBackTrace(0, 20, fr, NULL);

                p += _snprintf(rep + p, sizeof(rep) - p,
                        "  [WATCH] %s Xbox VA 0x%08X (hit #%ld, tid %lu)\n",
                        is_write ? "WRITE to" : "read of", fva, g_watch_hits,
                        GetCurrentThreadId());
                p += _snprintf(rep + p, sizeof(rep) - p,
                        "    guest: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n",
                        g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
                /* The label watchdogs, which usually name the writer outright
                 * and save resolving a frame by hand. g_main_loc is stamped
                 * only inside the main loop, so it stays meaningful when the
                 * write comes from another thread; g_last_loc is shared. */
                p += _snprintf(rep + p, sizeof(rep) - p,
                        "    loc:   main=loc_%08X  last=loc_%08X\n",
                        g_main_loc, g_last_loc);
                /* A write through a RAM mirror view is a wild pointer that
                 * happened to wrap onto the watched field (a
                 * 0x8D8AE800 store landed on a mesh part record at
                 * 0x018AE800), so say so. */
                if (fmirror)
                    p += _snprintf(rep + p, sizeof(rep) - p,
                            "    via RAM mirror %u: the guest pointer was 0x%08X + %u x %zu MB\n",
                            fmirror, fva, fmirror, fstride >> 20);
                /* The faulting thread's own frames. CaptureStackBackTrace
                 * inside the handler stops at the exception dispatcher, so
                 * unwind from the fault context instead: the first frame is
                 * the generated line doing the write. */
                {
                    CONTEXT uc = *ep->ContextRecord;
                    int f;
                    p += _snprintf(rep + p, sizeof(rep) - p, "    faulting frames:");
                    for (f = 0; f < 10 && uc.Rip && p < (int)sizeof(rep) - 32; f++) {
                        DWORD64 img = 0;
                        PRUNTIME_FUNCTION fe;
                        p += _snprintf(rep + p, sizeof(rep) - p, " 0x%llX",
                                (unsigned long long)(0x140000000ULL + (uc.Rip - base)));
                        fe = RtlLookupFunctionEntry(uc.Rip, &img, NULL);
                        if (!fe) {
                            uc.Rip = *(DWORD64 *)(uintptr_t)uc.Rsp;
                            uc.Rsp += 8;
                        } else {
                            PVOID hd = NULL;
                            DWORD64 ef = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, uc.Rip, fe, &uc, &hd, &ef, NULL);
                        }
                        if (uc.Rip < base || uc.Rip >= base + 0x4000000u) break;
                    }
                    p += _snprintf(rep + p, sizeof(rep) - p, "\n");
                }
                p += _snprintf(rep + p, sizeof(rep) - p,
                        "    native frames (link addrs for addr2line):\n");
                for (k = 0; k < n && p < (int)sizeof(rep) - 32; k++)
                    p += _snprintf(rep + p, sizeof(rep) - p, "      0x%llX\n",
                            (unsigned long long)(0x140000000ULL + ((uintptr_t)fr[k] - base)));
                if (p < 0 || p > (int)sizeof(rep)) p = (int)sizeof(rep) - 1;
                rep[p] = 0;
                if (is_write) {         /* printed after the step, with the value */
                    memcpy(g_watch_rep, rep, sizeof g_watch_rep);
                    g_watch_rep_va = fva & ~3u;
                } else if (!g_watch_nan_only) {
                    InterlockedIncrement(&g_watch_hits);
                    fputs(rep, stderr);
                    fflush(stderr);
                }
            }

            /* Let the access through, then re-arm on the single step. */
let_through:
            g_watch_pending_alias =
                (void *)((uintptr_t)diag_native(page)
                         + (uintptr_t)fmirror * fstride);
            VirtualProtect(g_watch_pending_alias, 0x1000, g_watch[i].old_prot, &old);
            g_watch_pending = &g_watch[i];
            ep->ContextRecord->EFlags |= 0x100;   /* TF */
            return 1;
        }
    }
    return 0;
}

/* ---- client plumbing ------------------------------------ */

typedef struct { SOCKET s; char buf[8192]; int len; } diag_client;

static void cflush(diag_client *c)
{
    if (c->len > 0) { send(c->s, c->buf, c->len, 0); c->len = 0; }
}

static void cprintf(diag_client *c, const char *fmt, ...)
{
    char line[1024];
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    if (c->len + n > (int)sizeof(c->buf)) cflush(c);
    if (n > (int)sizeof(c->buf)) { send(c->s, line, n, 0); return; }
    memcpy(c->buf + c->len, line, (size_t)n);
    c->len += n;
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return 0;
    v = strtoul(s, &end, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 16);
    if (end == s) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int parse_dec(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return 0;
    v = strtoul(s, &end, 10);
    if (end == s) return 0;
    *out = (uint32_t)v;
    return 1;
}

/* ---- commands ------------------------------------------- */

static const char *magic_name(uint16_t m)
{
    switch (m) {
    case DIAG_MAGIC_ALLOC: return "MB/alloc";
    case DIAG_MAGIC_FREE:  return "FB/free ";
    case DIAG_MAGIC_END:   return "BS/end  ";
    default:               return "??      ";
    }
}

static void cmd_info(diag_client *c)
{
    cprintf(c, "mem_offset  0x%p\n", (void *)g_xbox_mem_offset);
    cprintf(c, "total_ram   %u bytes (0x%X)\n",
            (unsigned)XBOX_TOTAL_RAM, (unsigned)XBOX_TOTAL_RAM);
    cprintf(c, "heap_base   0x%08X  size 0x%X\n",
            (unsigned)XBOX_HEAP_BASE, (unsigned)XBOX_HEAP_SIZE);
    cprintf(c, "pool_table  0x%08X\n", DIAG_POOL_TABLE_VA);
}

static void cmd_mem(diag_client *c, uint32_t va, uint32_t len)
{
    uint32_t i;
    if (!diag_va_ok(va, len)) { cprintf(c, "ERR address 0x%08X..+%u not mapped\n", va, len); return; }
    for (i = 0; i < len; i += 16) {
        char asc[17];
        uint32_t j, n = (len - i < 16) ? len - i : 16;
        cprintf(c, "%08X ", va + i);
        for (j = 0; j < 16; j++) {
            if (j < n) { uint8_t b = g8(va + i + j); cprintf(c, "%02X ", b);
                         asc[j] = (b >= 32 && b < 127) ? (char)b : '.'; }
            else       { cprintf(c, "   "); asc[j] = ' '; }
        }
        asc[16] = 0;
        cprintf(c, " |%s|\n", asc);
    }
    cprintf(c, "OK\n");
}

static void cmd_d32(diag_client *c, uint32_t va, uint32_t count)
{
    uint32_t i;
    if (!diag_va_ok(va, count * 4)) { cprintf(c, "ERR range not mapped\n"); return; }
    for (i = 0; i < count; i += 4) {
        uint32_t j, n = (count - i < 4) ? count - i : 4;
        cprintf(c, "%08X ", va + i * 4);
        for (j = 0; j < n; j++) cprintf(c, " %08X", g32(va + (i + j) * 4));
        cprintf(c, "\n");
    }
    cprintf(c, "OK\n");
}

static void cmd_str(diag_client *c, uint32_t va, uint32_t max)
{
    char out[513];
    uint32_t i = 0;
    if (max > 512) max = 512;
    if (!diag_va_ok(va, 1)) { cprintf(c, "ERR address not mapped\n"); return; }
    while (i < max && diag_va_ok(va + i, 1)) {
        uint8_t b = g8(va + i);
        if (!b) break;
        out[i++] = (b >= 32 && b < 127) ? (char)b : '.';
    }
    out[i] = 0;
    cprintf(c, "0x%08X \"%s\" (len %u)\nOK\n", va, out, i);
}

static void cmd_pools(diag_client *c)
{
    int i;
    if (!diag_va_ok(DIAG_POOL_TABLE_VA, DIAG_POOL_SLOTS * 4)) {
        cprintf(c, "ERR pool table not mapped yet\n"); return;
    }
    cprintf(c, "slot descriptor  sentinel   first-free\n");
    for (i = 0; i < DIAG_POOL_SLOTS; i++) {
        uint32_t desc = g32(DIAG_POOL_TABLE_VA + (uint32_t)i * 4);
        if (!desc) continue;
        if (!diag_va_ok(desc, 0x40)) { cprintf(c, "%3d  0x%08X  <unmapped>\n", i, desc); continue; }
        {
            uint32_t sent = desc + DIAG_SENTINEL_OFF;
            cprintf(c, "%3d  0x%08X  0x%08X 0x%08X\n", i, desc, sent, g32(sent + DIAG_FL_NEXT_OFF));
        }
    }
    cprintf(c, "OK\n");
}

/*
 * Walk the free list of one pool, reporting any node that is not 'FB'.
 *
 * The sentinel is checked first, and that distinction matters: SSX builds a
 * CRT pool inside a 53 MB MmAllocateContiguousMemoryEx block during init, uses
 * it briefly, then RETIRES it -- D3D later reuses the same memory for a vertex
 * pointer table (confirmed with `owner` and `watch`). The stale pool-table
 * entry at 0x00203BE0 still points there, so a naive walk reports a "broken
 * list" for the rest of the run and looks exactly like heap corruption. It is
 * not. A live pool's sentinel carries size 0x7FFFFFFF; if that is gone, the
 * whole descriptor is gone and there is nothing meaningful to walk.
 */
#define DIAG_SENTINEL_SIZE 0x7FFFFFFFu

static int walk_freelist(diag_client *c, int slot, uint32_t max, int quiet)
{
    uint32_t desc, sent, n;
    uint32_t k = 0;
    int bad = 0;

    desc = g32(DIAG_POOL_TABLE_VA + (uint32_t)slot * 4);
    if (!desc || !diag_va_ok(desc, 0x40)) return 0;
    sent = desc + DIAG_SENTINEL_OFF;

    if (g32(sent + 4) != DIAG_SENTINEL_SIZE) {
        cprintf(c, "pool %d: descriptor at 0x%08X is no longer a descriptor "
                   "(sentinel size 0x%08X, expected 0x7FFFFFFF)\n",
                slot, desc, g32(sent + 4));
        cprintf(c, "  the pool looks retired and its memory reused -- "
                   "not corruption. Use `owner 0x%08X` to see who holds it.\n", desc);
        return 0;
    }

    n = g32(sent + DIAG_FL_NEXT_OFF);
    if (!quiet) cprintf(c, "pool %d free list (sentinel 0x%08X)\n", slot, sent);
    while (k < max && n && n != sent) {
        uint16_t m;
        if (!diag_va_ok(n, 0x18)) {
            cprintf(c, "  [%u] 0x%08X  <unmapped>  *** BROKEN ***\n", k, n);
            bad++; break;
        }
        m = g16(n);
        if (!quiet || m != DIAG_MAGIC_FREE)
            cprintf(c, "  [%u] 0x%08X  %s size=0x%08X next=0x%08X prev=0x%08X%s\n",
                    k, n, magic_name(m), g32(n + 4),
                    g32(n + DIAG_FL_NEXT_OFF), g32(n + DIAG_FL_PREV_OFF),
                    (m != DIAG_MAGIC_FREE) ? "  *** NOT FREE ***" : "");
        if (m != DIAG_MAGIC_FREE) { bad++; break; }
        n = g32(n + DIAG_FL_NEXT_OFF);
        k++;
    }
    if (!quiet) cprintf(c, "  %u node(s)%s\n", k, bad ? ", LIST IS BROKEN" : "");
    return bad;
}

/* Walk the address-ordered block chain from an explicit header VA. */
static void cmd_blocks(diag_client *c, uint32_t va, uint32_t max)
{
    uint32_t k = 0;
    if (!diag_va_ok(va, 0x10)) { cprintf(c, "ERR start address not mapped\n"); return; }
    cprintf(c, "hdr        magic     flags size       next       prev\n");
    while (k < max && diag_va_ok(va, 0x10)) {
        uint16_t m = g16(va);
        uint32_t next = g32(va + 8);
        cprintf(c, "0x%08X %s %04X  0x%08X 0x%08X 0x%08X%s\n",
                va, magic_name(m), g16(va + 2), g32(va + 4), next, g32(va + 0x0C),
                (m != DIAG_MAGIC_ALLOC && m != DIAG_MAGIC_FREE && m != DIAG_MAGIC_END)
                    ? "  *** BAD MAGIC ***" : "");
        if (m == DIAG_MAGIC_END || !next || next == va) break;
        va = next;
        k++;
    }
    cprintf(c, "OK\n");
}

static void cmd_heapcheck(diag_client *c)
{
    int slot, bad = 0, pools = 0;
    if (!diag_va_ok(DIAG_POOL_TABLE_VA, DIAG_POOL_SLOTS * 4)) {
        cprintf(c, "ERR pool table not mapped yet\n"); return;
    }
    for (slot = 0; slot < DIAG_POOL_SLOTS; slot++) {
        if (!g32(DIAG_POOL_TABLE_VA + (uint32_t)slot * 4)) continue;
        pools++;
        bad += walk_freelist(c, slot, 4096, 1 /* quiet: only report violations */);
    }
    cprintf(c, "%d pool(s) checked, %d violation(s)\nOK\n", pools, bad);
}

static void cmd_icall(diag_client *c)
{
    uint32_t list[512];
    int n = recomp_icall_miss_list(list, 512), i;
    if (n <= 0) { cprintf(c, "no unresolved indirect-call targets\nOK\n"); return; }
    cprintf(c, "%d unresolved indirect-call target(s), first-seen order:\n", n);
    for (i = 0; i < n; i++) {
        uint32_t va = list[i];
        int plausible = (va >= 0x00010000u && va < 0x00400000u);
        cprintf(c, "  #%-3d 0x%08X %s\n", i + 1, va,
                plausible ? "" : "  (implausible -- the pointer itself was read from the wrong place)");
    }
    cprintf(c, "OK\n");
}

static void cmd_func(diag_client *c, uint32_t va)
{
    diag_func_t f = recomp_lookup(va);
    cprintf(c, "0x%08X %s\nOK\n", va, f ? "is dispatched" : "is NOT dispatched (an indirect call here would miss)");
}

static void cmd_owner(diag_client *c, uint32_t va)
{
    uint32_t base = 0, size = 0, idx = 0;
    void *fr[6];
    int nf = 0, i;
    if (!xbox_heap_owner_of(va, &base, &size, &idx, fr, 6, &nf)) {
        cprintf(c, "0x%08X is not inside any xbox_HeapAlloc block\nOK\n", va);
        return;
    }
    cprintf(c, "0x%08X is inside allocation #%u: 0x%08X..0x%08X (%u bytes, +0x%X in)\n",
            va, idx, base, base + size, size, va - base);
    cprintf(c, "  requested by (link addrs for addr2line):\n");
    {
        uintptr_t mb = (uintptr_t)GetModuleHandle(NULL);
        for (i = 0; i < nf; i++)
            cprintf(c, "    0x%llX\n",
                    (unsigned long long)(0x140000000ULL + ((uintptr_t)fr[i] - mb)));
    }
    cprintf(c, "OK\n");
}

static void cmd_files(diag_client *c)
{
    const char *path; unsigned long long bytes; unsigned int reads;
    int i = 0, shown = 0;
    cprintf(c, "%-12s %-8s  path\n", "bytes read", "reads");
    while (xbox_file_ledger_get(i, &path, &bytes, &reads)) {
        cprintf(c, "%-12llu %-8u  %s%s\n", bytes, reads, path,
                (reads == 0) ? "   <-- opened but never read" : "");
        shown++; i++;
    }
    if (!shown) cprintf(c, "no files opened yet\n");
    cprintf(c, "OK\n");
}

/* Scan a guest range for a 32-bit value. Answers questions like "is this
 * NV2A method header anywhere in the push buffer" without a rebuild. */
static void cmd_find(diag_client *c, uint32_t va, uint32_t len, uint32_t val)
{
    uint32_t i, hits = 0;
    if (!diag_va_ok(va, len)) { cprintf(c, "ERR range not mapped\n"); return; }
    for (i = 0; i + 4 <= len; i += 4) {
        if (g32(va + i) != val) continue;
        if (hits < 32) cprintf(c, "  0x%08X\n", va + i);
        hits++;
    }
    cprintf(c, "%u match(es) for 0x%08X in 0x%08X..0x%08X%s\nOK\n",
            hits, val, va, va + len,
            (hits > 32) ? " (first 32 shown)" : "");
}


/* ---- thread sampling ------------------------------------ *
 * "Where is the main thread right now?" -- answered by suspending each thread
 * and reading its instruction pointer, rather than inferring it from the
 * label watchdog.
 *
 * The watchdog stamps a global at each generated label, which works only while
 * the thread stays inside stamped code: once it leaves, the global keeps its
 * last value and reads as a hot spot that is really just the last stamp before
 * a long unstamped stretch, so this samples the truth instead.
 *
 * Reported as a module RVA because ASLR moves the image every run; feed it to
 * addr2line as 0x140000000 + rva.
 */
static void cmd_threads(diag_client *c)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    uintptr_t base = (uintptr_t)GetModuleHandle(NULL);
    int n = 0;

    if (snap == INVALID_HANDLE_VALUE) { cprintf(c, "ERR snapshot failed\n"); return; }
    te.dwSize = sizeof(te);
    cprintf(c, "tid       rva         (module base %p)\n", (void *)base);
    if (Thread32First(snap, &te)) {
        do {
            HANDLE th;
            CONTEXT ctx;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                            te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th) != (DWORD)-1) {
                memset(&ctx, 0, sizeof(ctx));
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(th, &ctx)) {
                    uintptr_t ip = (uintptr_t)ctx.Rip;
                    if (ip >= base && ip - base < 0x08000000u)
                        cprintf(c, "%-9lu 0x%08llX\n", (unsigned long)te.th32ThreadID,
                                (unsigned long long)(ip - base));
                    else
                        cprintf(c, "%-9lu (outside the image: %p)\n",
                                (unsigned long)te.th32ThreadID, (void *)ip);
                    /*
                     * A rough call chain. The generated code uses ordinary C
                     * calls, so return addresses really are on the native
                     * stack -- but the optimiser omits frame pointers, so a
                     * proper unwind would need RtlVirtualUnwind. Scanning the
                     * stack for in-image values gets the same answer for the
                     * question that matters here ("is FEInit_Boot still on the
                     * stack?") and costs nothing. Expect stale values mixed in:
                     * this is a lead, not a proof.
                     */
                    {
                        uintptr_t *sp = (uintptr_t *)(uintptr_t)ctx.Rsp;
                        int k, shown = 0;
                        for (k = 0; k < 512 && shown < 24; k++) {
                            uintptr_t v;
                            if (IsBadReadPtr(sp + k, sizeof(*sp))) break;
                            v = sp[k];
                            if (v >= base && v - base < 0x08000000u) {
                                cprintf(c, "    ^ 0x%08llX\n",
                                        (unsigned long long)(v - base));
                                shown++;
                            }
                        }
                    }
                    n++;
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    cprintf(c, "%d thread(s)\nOK\n", n);
}

static void cmd_help(diag_client *c)
{
    cprintf(c,
        "info                  runtime layout\n"
        "mem <va> [len]        hex+ascii dump (len decimal, default 64)\n"
        "d32 <va> [count]      dword dump (count decimal, default 8)\n"
        "w32 <va> <value>      write one guest dword (hex); diagnostics only\n"
        "save <va> <len> <path> write guest memory to a host file (hex va and len)\n"
        "probe [SPEC|clear]    arm generated-code probes at runtime, no rebuild\n"
        "                        ADDR[|WHEN][|SHOW,SHOW][|LIMIT];...  no spaces\n"
        "                        e.g. probe 0xAA24C|esi==0|esi,[esi+4]|3\n"
        "str <va> [max]        read a C string\n"
        "pools                 CRT pool descriptor table\n"
        "freelist <slot> [max] walk one pool's free list\n"
        "blocks <va> [max]     walk the block chain from a header VA\n"
        "heapcheck             validate every pool's free list, report only breaks\n"
        "icall                 unresolved indirect-call targets\n"
        "files                 every file opened, with bytes actually read\n"
        "func <va>             is this VA in the dispatch table?\n"
        "owner <va>            which allocation owns this VA, and who asked for it\n"
        "find <va> <len> <val> scan a range for a 32-bit value (all hex)\n"
        "watch <va>            report who writes this page (to stderr)\n"
        "unwatch               clear all watches\n"
        "shot <path.png>       save the next presented frame\n"
        "press <button> [ms]   hold a pad-0 button (A B X Y BLACK WHITE START BACK\n"
        "                      UP DOWN LEFT RIGHT) for ms, default 150\n"
        "quit                  close the connection\n"
        "OK\n");
}

/* ---- dispatch ------------------------------------------- */

/* Set by the host (main.c) to d3d8_RequestScreenshot; used by `shot`. */
void (*g_diag_shot_hook)(const wchar_t *path) = NULL;
/* Set by the host to its input HLE; used by `press`. Returns 0 for an
 * unknown button name. */
int (*g_diag_press_hook)(const char *name, int ms) = NULL;
/* Set by the host to nv2a_drawlog_arm; used by `drawlog`. */
void (*g_diag_drawlog_hook)(int frames) = NULL;
/* Set by the host to nv2a_skipprog_set; used by `skipprog`. */
void (*g_diag_skipprog_hook)(uint32_t hash) = NULL;
/* Set by the host to nv2a_ignored_dump; used by `ignored`. */
void (*g_diag_ignored_hook)(void) = NULL;

static int handle(diag_client *c, char *line)
{
    char *cmd = strtok(line, " \t\r\n");
    char *a1  = strtok(NULL, " \t\r\n");
    char *a2  = strtok(NULL, " \t\r\n");
    uint32_t va = 0, n = 0;

    if (!cmd) { cprintf(c, "OK\n"); return 1; }

    if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) { cprintf(c, "OK\n"); return 0; }
    if (!strcmp(cmd, "help")) { cmd_help(c); return 1; }
    if (!strcmp(cmd, "info")) { cmd_info(c); cprintf(c, "OK\n"); return 1; }
    /*
     * `loc` samples the label watchdogs repeatedly and reports whether they
     * are moving. Bisecting a hang by adding probes and rebuilding costs
     * minutes a step; this names the stuck label in one query with no rebuild.
     * Two samples apart in time distinguish "blocked" from "spinning", which
     * is the first thing worth knowing about any hang.
     */
    if (!strcmp(cmd, "loc")) {
        unsigned l0 = g_last_loc, i, changes = 0;
        unsigned lmin = l0, lmax = l0;
        for (i = 0; i < 40; i++) {
            unsigned l;
            Sleep(1);
            l = g_last_loc;
            if (l != l0) changes++;
            if (l < lmin) lmin = l;
            if (l > lmax) lmax = l;
            l0 = l;
        }
        cprintf(c, "main_loc  loc_%08X\n", g_main_loc);
        cprintf(c, "last_loc  loc_%08X   range loc_%08X..loc_%08X\n",
                g_last_loc, lmin, lmax);
        cprintf(c, "%u of 40 samples moved -- %s\n", changes,
                changes ? "running" : "BLOCKED (same label throughout)");
        cprintf(c, "OK\n");
        return 1;
    }
    if (!strcmp(cmd, "shot")) {
        /* shot <path.png>: save the next presented frame, as F12 does. The
         * host sets the hook (the kernel library does not link against the
         * renderer). */
        wchar_t w[MAX_PATH];
        if (!a1) { cprintf(c, "ERR usage: shot <path.png>\n"); return 1; }
        if (!g_diag_shot_hook) { cprintf(c, "ERR no screenshot hook\n"); return 1; }
        MultiByteToWideChar(CP_UTF8, 0, a1, -1, w, MAX_PATH);
        g_diag_shot_hook(w);
        cprintf(c, "OK\n");
        return 1;
    }
    if (!strcmp(cmd, "press")) {
        /* press <button> [ms]: hold a pad-0 button for ms (default 150), so a
         * test can walk the menus one step at a time and look at each screen
         * (`shot`) before choosing the next press. Names as
         * XBOX_INPUT_AUTOPRESS: A B X Y BLACK WHITE START BACK UP DOWN LEFT
         * RIGHT. */
        if (!a1) { cprintf(c, "ERR usage: press <button> [ms]\n"); return 1; }
        if (!g_diag_press_hook) { cprintf(c, "ERR no input hook\n"); return 1; }
        if (!g_diag_press_hook(a1, a2 ? atoi(a2) : 150)) {
            cprintf(c, "ERR unknown button '%s'\n", a1);
            return 1;
        }
        cprintf(c, "OK\n");
        return 1;
    }
    if (!strcmp(cmd, "drawlog")) {
        /* drawlog [frames]: describe every program draw of the next frames
         * (XBOX_NV2A_DRAWLOG output, to stderr), starting two presents from
         * now -- for screens reached by hand, whose frame number is not
         * known in advance. XBOX_NV2A_DRAWLOG_VERTS / _MODE still apply. */
        if (!g_diag_drawlog_hook) { cprintf(c, "ERR no drawlog hook\n"); return 1; }
        g_diag_drawlog_hook(a1 ? atoi(a1) : 1);
        cprintf(c, "OK\n");
        return 1;
    }
    if (!strcmp(cmd, "ignored")) {
        /* ignored: print (to stderr) and reset the dropped NV2A method
         * histogram; call once to reset, again after the scene of interest. */
        if (!g_diag_ignored_hook) { cprintf(c, "ERR no ignored hook\n"); return 1; }
        g_diag_ignored_hook();
        cprintf(c, "OK\n");
        return 1;
    }
    if (!strcmp(cmd, "stall")) {
        /* stall <ms>: suspend every other thread for ms -- a deliberate
         * hitch, to test whether some behaviour depends on frame time. */
        DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        HANDLE held[256];
        int nh = 0, ms = a1 ? atoi(a1) : 500;
        THREADENTRY32 te;
        if (snap == INVALID_HANDLE_VALUE) { cprintf(c, "ERR snapshot failed\n"); return 1; }
        te.dwSize = sizeof te;
        if (Thread32First(snap, &te)) {
            do {
                HANDLE th;
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || nh >= 256) continue;
                th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (!th) continue;
                if (SuspendThread(th) != (DWORD)-1) held[nh++] = th;
                else CloseHandle(th);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        Sleep((DWORD)ms);
        while (nh > 0) { nh--; ResumeThread(held[nh]); CloseHandle(held[nh]); }
        cprintf(c, "OK\n");
        return 1;
    }
    if (!strcmp(cmd, "skipprog")) {
        /* skipprog <hex hash>|0: drop draws of the vertex program with that
         * listing hash (as XBOX_NV2A_DRAWLOG prints it) until cleared. */
        if (!g_diag_skipprog_hook) { cprintf(c, "ERR no skipprog hook\n"); return 1; }
        g_diag_skipprog_hook(a1 ? (uint32_t)strtoul(a1, NULL, 16) : 0u);
        cprintf(c, "OK\n");
        return 1;
    }
    if (!strcmp(cmd, "pools")) { cmd_pools(c); return 1; }
    if (!strcmp(cmd, "heapcheck")) { cmd_heapcheck(c); return 1; }
    if (!strcmp(cmd, "icall")) { cmd_icall(c); return 1; }
    if (!strcmp(cmd, "files")) { cmd_files(c); return 1; }

    if (!strcmp(cmd, "mem")) {
        if (!parse_u32(a1, &va)) { cprintf(c, "ERR usage: mem <va> [len]\n"); return 1; }
        if (!parse_dec(a2, &n) || n == 0) n = 64;
        if (n > 4096) n = 4096;
        cmd_mem(c, va, n); return 1;
    }
    /* Arm generated-code probes without restarting, let alone rebuilding.
     *
     * Every generated label carries a RECOMP_LOC() check, so a probe is pure
     * runtime state: `probe 0xAA24C|esi==0|esi,[esi+4]|3` starts reporting on
     * the next pass through that label.  The spec must contain no spaces --
     * this dispatcher tokenises on whitespace. */
    if (!strcmp(cmd, "probe")) {
        if (!recomp_probe_arm || !recomp_probe_clear || !recomp_probe_list) {
            cprintf(c, "ERR probe support not linked into this build\n");
            return 1;
        }
        if (!a1) {
            char buf[2048];
            buf[0] = 0;
            recomp_probe_list(buf, sizeof(buf));
            cprintf(c, "%s", buf);
            cprintf(c, "OK\n");
            return 1;
        }
        if (!strcmp(a1, "clear")) {
            recomp_probe_clear();
            cprintf(c, "cleared\nOK\n");
            return 1;
        }
        {
            int armed = recomp_probe_arm(a1);
            if (armed < 0) {
                cprintf(c, "ERR bad spec; want ADDR[|WHEN][|SHOW,SHOW][|LIMIT] "
                           "separated by ';', no spaces\n");
                return 1;
            }
            cprintf(c, "armed %d\nOK\n", armed);
            return 1;
        }
    }
    if (!strcmp(cmd, "d32")) {
        if (!parse_u32(a1, &va)) { cprintf(c, "ERR usage: d32 <va> [count]\n"); return 1; }
        if (!parse_dec(a2, &n) || n == 0) n = 8;
        if (n > 1024) n = 1024;
        cmd_d32(c, va, n); return 1;
    }
    /* `w32 <va> <value>` pokes one guest dword: test fixtures only (e.g. writing
     * the profile's unlock masks, then lets the game save them itself). */
    if (!strcmp(cmd, "w32")) {
        uint32_t v;
        if (!parse_u32(a1, &va) || !parse_u32(a2, &v)) { cprintf(c, "ERR usage: w32 <va> <value> (hex)\n"); return 1; }
        if (!diag_va_ok(va, 4)) { cprintf(c, "ERR range not mapped\n"); return 1; }
        *(volatile uint32_t *)((uintptr_t)xbox_fold_ram_alias(va) + g_xbox_mem_offset) = v;
        cprintf(c, "%08X <- %08X\nOK\n", va, v);
        return 1;
    }
    /* `save 0 4000000 <path>` writes the 64 MB of guest RAM to a file in the
     * same layout as a physical dump of xemu's RAM, so the two can
     * be compared structure by structure. Not atomic: the game keeps running
     * while it is copied. */
    if (!strcmp(cmd, "save")) {
        char *path = strtok(NULL, " \t\r\n");
        FILE *f;
        if (!parse_u32(a1, &va) || !parse_u32(a2, &n) || !path) {
            cprintf(c, "ERR usage: save <va> <len> <path> (hex va and len)\n");
            return 1;
        }
        if (!diag_va_ok(va, n)) { cprintf(c, "ERR range not mapped\n"); return 1; }
        f = fopen(path, "wb");
        if (!f) { cprintf(c, "ERR cannot open %s\n", path); return 1; }
        fwrite((const void *)((uintptr_t)xbox_fold_ram_alias(va) + g_xbox_mem_offset), 1, n, f);
        fclose(f);
        cprintf(c, "saved %u bytes\nOK\n", n);
        return 1;
    }
    if (!strcmp(cmd, "str")) {
        if (!parse_u32(a1, &va)) { cprintf(c, "ERR usage: str <va> [max]\n"); return 1; }
        if (!parse_dec(a2, &n) || n == 0) n = 128;
        cmd_str(c, va, n); return 1;
    }
    if (!strcmp(cmd, "freelist")) {
        if (!parse_dec(a1, &va) || va >= DIAG_POOL_SLOTS) { cprintf(c, "ERR usage: freelist <slot 0-15> [max]\n"); return 1; }
        if (!parse_dec(a2, &n) || n == 0) n = 64;
        walk_freelist(c, (int)va, n, 0); cprintf(c, "OK\n"); return 1;
    }
    if (!strcmp(cmd, "blocks")) {
        if (!parse_u32(a1, &va)) { cprintf(c, "ERR usage: blocks <va> [max]\n"); return 1; }
        if (!parse_dec(a2, &n) || n == 0) n = 32;
        if (n > 4096) n = 4096;
        cmd_blocks(c, va, n); return 1;
    }
    if (!strcmp(cmd, "threads")) { cmd_threads(c); return 1; }
    if (!strcmp(cmd, "watch")) {
        if (!parse_u32(a1, &va)) { cprintf(c, "ERR usage: watch <va>\n"); return 1; }
        {
            uint32_t wl = 4;
            /* optional byte length, within the page: a moving target such as the
             * top of a matrix stack needs a range, not one dword */
            if (a2 && parse_u32(a2, &wl) && wl) {
                if (wl > 0x1000u - (va & 0xFFFu)) wl = 0x1000u - (va & 0xFFFu);
            } else wl = 4;
            int r = xbox_diag_watch_add_ex(va, wl);
            if (r == 0)      cprintf(c, "watching page 0x%08X (writes reported on stderr)\nOK\n", va & ~0xFFFu);
            else if (r == 1) cprintf(c, "page 0x%08X already watched\nOK\n", va & ~0xFFFu);
            else             cprintf(c, "ERR could not watch 0x%08X (code %d)\n", va, r);
        }
        return 1;
    }
    if (!strcmp(cmd, "watchnan")) {
        if (!parse_u32(a1, &va)) { cprintf(c, "ERR usage: watchnan <va>\n"); return 1; }
        g_watch_nan_only = 1;
        {
            uint32_t wl = 4;
            /* optional byte length, within the page: a moving target such as the
             * top of a matrix stack needs a range, not one dword */
            if (a2 && parse_u32(a2, &wl) && wl) {
                if (wl > 0x1000u - (va & 0xFFFu)) wl = 0x1000u - (va & 0xFFFu);
            } else wl = 4;
            int r = xbox_diag_watch_add_ex(va, wl);
            if (r == 0 || r == 1) cprintf(c, "watching 0x%08X for NaN stores\nOK\n", va);
            else                  cprintf(c, "ERR could not watch 0x%08X (code %d)\n", va, r);
        }
        return 1;
    }
    if (!strcmp(cmd, "unwatch")) { xbox_diag_watch_clear(); cprintf(c, "all watches cleared\nOK\n"); return 1; }

    if (!strcmp(cmd, "find")) {
        char *a3 = strtok(NULL, " \t\r\n");
        uint32_t val = 0;
        if (!parse_u32(a1, &va) || !parse_u32(a2, &n) || !parse_u32(a3, &val)) {
            cprintf(c, "ERR usage: find <va> <len-hex> <value-hex>\n"); return 1;
        }
        if (n > 0x400000u) n = 0x400000u;
        cmd_find(c, va, n, val); return 1;
    }

    if (!strcmp(cmd, "owner")) {
        if (!parse_u32(a1, &va)) { cprintf(c, "ERR usage: owner <va>\n"); return 1; }
        cmd_owner(c, va); return 1;
    }

    if (!strcmp(cmd, "func")) {
        if (!parse_u32(a1, &va)) { cprintf(c, "ERR usage: func <va>\n"); return 1; }
        cmd_func(c, va); return 1;
    }

    cprintf(c, "ERR unknown command '%s' (try help)\n", cmd);
    return 1;
}

/* ---- server --------------------------------------------- */

static SOCKET   g_listen = INVALID_SOCKET;
static HANDLE   g_thread = NULL;
static volatile LONG g_running = 0;

static DWORD WINAPI diag_thread(LPVOID param)
{
    (void)param;
    while (g_running) {
        struct sockaddr_in ca;
        int calen = (int)sizeof(ca);
        SOCKET cs = accept(g_listen, (struct sockaddr *)&ca, &calen);
        diag_client c;
        char line[512];
        int pos = 0;

        if (cs == INVALID_SOCKET) { if (!g_running) break; Sleep(50); continue; }

        c.s = cs; c.len = 0;
        cprintf(&c, "xbox_diag ready -- 'help' for commands\nOK\n");
        cflush(&c);

        for (;;) {
            char ch;
            int r = recv(cs, &ch, 1, 0);
            if (r <= 0) break;
            if (ch == '\n') {
                line[pos] = 0; pos = 0;
                if (!handle(&c, line)) { cflush(&c); break; }
                cflush(&c);
            } else if (pos < (int)sizeof(line) - 1 && ch != '\r') {
                line[pos++] = ch;
            }
        }
        closesocket(cs);
    }
    return 0;
}

void xbox_diag_start(void)
{
    const char *env = getenv("XBOX_DIAG_PORT");

    /* XBOX_DIAG_WATCH=<hex-va>[,<hex-va>...] arms write watches before the
     * title runs. The interactive `watch` command can only be issued once the
     * server is up, which is already past initialisation -- and the fields
     * worth watching (a scene view's depth scale, for one) are written exactly
     * once, during init, and never again. Arming from the environment is the
     * only way to see those writes at all. */
    {
        const char *w = getenv("XBOX_DIAG_WATCH");
        if (w && *w) {
            while (*w) {
                char *end = NULL;
                unsigned long va = strtoul(w, &end, 16);
                unsigned long len = 4;
                if (end == w) break;
                /* `va:len` narrows the report to that many bytes; bare `va`
                 * means the one dword there. */
                if (*end == ':') {
                    char *lend = NULL;
                    unsigned long n = strtoul(end + 1, &lend, 16);
                    if (lend != end + 1) { len = n ? n : 4; end = lend; }
                }
                if (va) {
                    int r = xbox_diag_watch_add_ex((uint32_t)va, (uint32_t)len);
                    fprintf(stderr, "  [DIAG] watch 0x%08X+%lu armed at startup: %s\n",
                            (uint32_t)va, len,
                            r == 0 ? "ok" : r == 1 ? "already" : "failed");
                }
                w = end;
                while (*w == ',' || *w == ' ') w++;
            }
            fflush(stderr);
        }
    }

    WSADATA wsa;
    struct sockaddr_in sa;
    unsigned long port;
    BOOL yes = TRUE;

    if (g_running) return;
    if (!env || !*env) return;

    port = strtoul(env, NULL, 10);
    if (port == 0 || port > 65535) {
        fprintf(stderr, "  [DIAG] XBOX_DIAG_PORT='%s' is not a usable port\n", env);
        return;
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "  [DIAG] WSAStartup failed\n"); return;
    }
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { fprintf(stderr, "  [DIAG] socket() failed\n"); return; }
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* loopback only */

    if (bind(g_listen, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(g_listen, 1) != 0) {
        fprintf(stderr, "  [DIAG] could not listen on 127.0.0.1:%lu (error %d)\n",
                port, WSAGetLastError());
        closesocket(g_listen); g_listen = INVALID_SOCKET;
        return;
    }

    g_running = 1;
    g_thread = CreateThread(NULL, 0, diag_thread, NULL, 0, NULL);
    fprintf(stderr, "  [DIAG] live diagnostics on 127.0.0.1:%lu -- 'help' for commands\n", port);
    fflush(stderr);
}

void xbox_diag_stop(void)
{
    if (!g_running) return;
    g_running = 0;
    if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
    if (g_thread) { WaitForSingleObject(g_thread, 500); CloseHandle(g_thread); g_thread = NULL; }
}

#else  /* !_WIN32 */
void xbox_diag_start(void) {}
void xbox_diag_stop(void) {}
#endif
