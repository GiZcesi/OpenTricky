/* mmio_trap.c -- see mmio_trap.h. */
#include "mmio_trap.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

/* ===================================================================== */
/* AArch64 load/store decoder (host independent)                          */
/* ===================================================================== */

bool ot_a64_decode(uint32_t insn, ot_a64_access *o)
{
    /* Load/store register classes: bits[29:27] == 111, V (bit 26) == 0 (GPR). */
    if (((insn >> 27) & 7) != 7 || ((insn >> 26) & 1)) return false;

    unsigned size = (insn >> 30) & 3;
    unsigned opc  = (insn >> 22) & 3;
    unsigned cls  = (insn >> 24) & 3;
    memset(o, 0, sizeof *o);
    o->rt = insn & 31;
    o->rn = (insn >> 5) & 31;

    if (cls == 1) {
        /* unsigned immediate offset: no writeback, address comes from FAR anyway */
    } else if (cls == 0) {
        if ((insn >> 21) & 1) {
            /* register offset (bits 11:10 == 10); other values here are atomics/PAC */
            if (((insn >> 10) & 3) != 2) return false;
        } else {
            unsigned idx = (insn >> 10) & 3;
            if (idx == 2) return false;               /* LDTR/STTR: unprivileged, not emitted */
            if (idx == 1 || idx == 3) {
                o->writeback = (idx == 3) ? 1 : 2;    /* 11 pre-index, 01 post-index */
                o->imm = ((int64_t)(int32_t)(insn << 11)) >> 23;   /* sign-extend imm9 */
            }
        }
    } else {
        return false;
    }

    o->size = 1u << size;
    switch (opc) {
    case 0: o->is_store = true; break;                              /* STR* */
    case 1: break;                                                  /* LDR*, zero-extended */
    case 2:                                                         /* LDRS* to Xt (PRFM at size 3) */
        if (size == 3) return false;
        o->sign_extend = true; o->dest64 = true; break;
    case 3:                                                         /* LDRS* to Wt (reserved at size 2/3) */
        if (size >= 2) return false;
        o->sign_extend = true; break;
    }
    if (!o->is_store && !o->sign_extend && size == 3) o->dest64 = true;   /* LDR Xt */
    return true;
}

/* ===================================================================== */
/* Registry of trapped windows and pages                                  */
/* ===================================================================== */

#define MAX_WINDOWS 8
#define MAX_PAGES   512

typedef struct {
    uintptr_t lo, hi;                 /* device window [lo, hi) */
    ot_mmio_read_fn  rd;
    ot_mmio_write_fn wr;
    void *user;
    const char *name;
} window_t;

typedef struct { uintptr_t base; uint8_t *shadow; } page_t;

static window_t s_win[MAX_WINDOWS];
static page_t   s_page[MAX_PAGES];
static int      s_nwin, s_npage;
static size_t   s_pagesz;
static volatile uint64_t s_count;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t ot_mmio_trap_count(void) { return s_count; }

static page_t *find_page(uintptr_t a)
{
    for (int i = 0; i < s_npage; i++)
        if (a >= s_page[i].base && a < s_page[i].base + s_pagesz) return &s_page[i];
    return NULL;
}

static window_t *find_window(uintptr_t a)
{
    for (int i = 0; i < s_nwin; i++)
        if (a >= s_win[i].lo && a < s_win[i].hi) return &s_win[i];
    return NULL;
}

/* ===================================================================== */
/* Signal handling (macOS arm64)                                          */
/* ===================================================================== */

#if defined(__APPLE__) && defined(__aarch64__)

#include <sys/ucontext.h>

static struct sigaction s_old_segv, s_old_bus;
static int s_installed;

static uint64_t get_x(_STRUCT_MCONTEXT64 *mc, unsigned r)
{
    if (r < 29) return mc->__ss.__x[r];
    if (r == 29) return mc->__ss.__fp;
    if (r == 30) return mc->__ss.__lr;
    return 0;   /* 31: ZR when used as a data register */
}
static void set_x(_STRUCT_MCONTEXT64 *mc, unsigned r, uint64_t v)
{
    if (r < 29) mc->__ss.__x[r] = v;
    else if (r == 29) mc->__ss.__fp = v;
    else if (r == 30) mc->__ss.__lr = v;
    /* 31: ZR, writes discarded */
}

static void fatal(const char *why, uintptr_t pc, uint32_t insn, uintptr_t far)
{
    char b[256];
    int n = snprintf(b, sizeof b,
        "[MMIO] %s: pc=%p insn=%08X fault addr=%p\n", why, (void *)pc, insn, (void *)far);
    if (n > 0) (void)!write(2, b, (size_t)n);
}

static void chain(int sig, siginfo_t *si, void *uc)
{
    struct sigaction *old = (sig == SIGBUS) ? &s_old_bus : &s_old_segv;
    if (old->sa_flags & SA_SIGINFO) { old->sa_sigaction(sig, si, uc); return; }
    if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) { old->sa_handler(sig); return; }
    /* Default action: put it back and return, so the instruction faults again
     * and the process dies with the usual report. */
    signal(sig, SIG_DFL);
}

static void on_fault(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    _STRUCT_MCONTEXT64 *mc = uc->uc_mcontext;
    uintptr_t far = (uintptr_t)mc->__es.__far;
    if (!far) far = (uintptr_t)si->si_addr;

    page_t *pg = find_page(far);
    if (!pg) { chain(sig, si, ucv); return; }

    uintptr_t pc = (uintptr_t)mc->__ss.__pc;
    uint32_t insn = *(const uint32_t *)pc;
    ot_a64_access a;
    if (!ot_a64_decode(insn, &a)) {
        fatal("undecodable access to a trapped page", pc, insn, far);
        chain(sig, si, ucv);
        return;
    }

    /* Cross-check against the hardware syndrome when it describes the access
     * (ISV set): size, direction and register must agree with our decode. */
    uint32_t esr = mc->__es.__esr;
    unsigned ec = esr >> 26;
    if ((ec == 0x24 || ec == 0x25) && ((esr >> 24) & 1)) {
        unsigned sas = (esr >> 22) & 3, srt = (esr >> 16) & 31, wnr = (esr >> 6) & 1;
        if ((1u << sas) != a.size || (bool)wnr != a.is_store || srt != a.rt) {
            fatal("decoder disagrees with ESR", pc, insn, far);
            chain(sig, si, ucv);
            return;
        }
    }

    if (far + a.size > pg->base + s_pagesz) {
        fatal("access straddles a trapped page boundary", pc, insn, far);
        chain(sig, si, ucv);
        return;
    }

    window_t *w = find_window(far);
    if (w && far + a.size > w->hi) w = NULL;     /* not wholly inside: treat as plain memory */

    if (a.is_store) {
        uint64_t v = get_x(mc, a.rt);
        if (a.size < 8) v &= (1ull << (8 * a.size)) - 1;
        if (w) w->wr(w->user, (uint32_t)(far - w->lo), v, a.size);
        else   memcpy(pg->shadow + (far - pg->base), &v, a.size);
    } else {
        uint64_t v = 0;
        if (w) v = w->rd(w->user, (uint32_t)(far - w->lo), a.size);
        else   memcpy(&v, pg->shadow + (far - pg->base), a.size);
        if (a.size < 8) v &= (1ull << (8 * a.size)) - 1;
        if (a.sign_extend) {
            unsigned bits = 8 * a.size;
            v = (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
            if (!a.dest64) v &= 0xFFFFFFFFull;
        }
        set_x(mc, a.rt, v);     /* a W destination zero-extends into the X register */
    }

    if (a.writeback) {
        uint64_t nb = (a.writeback == 1) ? (uint64_t)far
                    : (a.rn == 31 ? mc->__ss.__sp : get_x(mc, a.rn)) + (uint64_t)a.imm;
        if (a.rn == 31) mc->__ss.__sp = nb; else set_x(mc, a.rn, nb);
    }

    mc->__ss.__pc = pc + 4;
    __atomic_add_fetch(&s_count, 1, __ATOMIC_RELAXED);
}

static bool install_handlers(void)
{
    if (s_installed) return true;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;       /* the handler never longjmps; nested faults still reach us */
    sigemptyset(&sa.sa_mask);
    /* A PROT_NONE page raises SIGBUS on macOS; SIGSEGV is installed too. */
    if (sigaction(SIGBUS, &sa, &s_old_bus) != 0) return false;
    if (sigaction(SIGSEGV, &sa, &s_old_segv) != 0) return false;
    s_installed = 1;
    return true;
}

bool ot_mmio_trap(void *native, size_t size, const char *name,
                  ot_mmio_read_fn read, ot_mmio_write_fn write, void *user)
{
    bool ok = false;
    pthread_mutex_lock(&s_lock);
    if (!s_pagesz) s_pagesz = (size_t)sysconf(_SC_PAGESIZE);
    if (s_nwin >= MAX_WINDOWS || !install_handlers()) goto out;

    uintptr_t lo = (uintptr_t)native, hi = lo + size;
    uintptr_t plo = lo & ~(uintptr_t)(s_pagesz - 1);
    uintptr_t phi = (hi + s_pagesz - 1) & ~(uintptr_t)(s_pagesz - 1);

    /* Shadow the pages first (their current content), then protect them. A page
     * already trapped for another window is shared, not copied again (it would fault). */
    int first_new = s_npage;
    for (uintptr_t p = plo; p < phi; p += s_pagesz) {
        if (find_page(p)) continue;
        if (s_npage >= MAX_PAGES) goto rollback;
        uint8_t *sh = (uint8_t *)malloc(s_pagesz);
        if (!sh) goto rollback;
        memcpy(sh, (void *)p, s_pagesz);
        s_page[s_npage].base = p;
        s_page[s_npage].shadow = sh;
        s_npage++;
    }
    /* the window is registered before its pages are protected: a fault the instant mprotect lands must find it */
    if (s_nwin >= (int)(sizeof s_win / sizeof s_win[0])) goto rollback;
    s_win[s_nwin++] = (window_t){ lo, hi, read, write, user, name };
    for (int i = first_new; i < s_npage; i++)
        if (mprotect((void *)s_page[i].base, s_pagesz, PROT_NONE) != 0) { s_nwin--; goto rollback; }
    ok = true;
    goto out;

rollback:
    for (int i = first_new; i < s_npage; i++) {
        mprotect((void *)s_page[i].base, s_pagesz, PROT_READ | PROT_WRITE);
        free(s_page[i].shadow);
    }
    s_npage = first_new;
out:
    pthread_mutex_unlock(&s_lock);
    return ok;
}

#else /* not macOS arm64 */

bool ot_mmio_trap(void *native, size_t size, const char *name,
                  ot_mmio_read_fn read, ot_mmio_write_fn write, void *user)
{
    (void)native; (void)size; (void)name; (void)read; (void)write; (void)user;
    fprintf(stderr, "[MMIO] trapping device registers is only implemented for macOS arm64\n");
    return false;
}

#endif
