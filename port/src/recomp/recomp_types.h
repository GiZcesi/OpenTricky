/**
 * Xbox Static Recompilation - Runtime Type Definitions
 *
 * Type definitions and helper macros used by mechanically translated
 * x86 -> C code. Each original x86 function is translated to a C
 * function that uses these types and macros.
 *
 * This is a reusable template for ANY Xbox game. Game-specific
 * customization should go in separate headers.
 *
 * Memory model:
 *   Xbox data sections are mapped to their original VAs via
 *   CreateFileMapping + MapViewOfFileEx (see xbox_memory.h).
 *   Recompiled code accesses globals via pointer casts, e.g.:
 *     *(uint32_t*)0x003B2360
 *
 * Register model:
 *   Volatile registers (eax, ecx, edx, esp) are global variables,
 *   matching real x86 behavior where these registers are shared
 *   across all code. This enables correct argument passing via the
 *   simulated stack and return value communication via eax.
 *
 *   Callee-saved registers (ebx, esi, edi) are also global because
 *   callers pass implicit parameters through them (e.g. 'this' via
 *   esi in thiscall). The callee-save contract is enforced by
 *   PUSH32/POP32 instructions in the generated code, not by C local
 *   variable scoping.
 *
 *   ebp is NOT global - it stays local in each function because many
 *   FPO (Frame Pointer Omission) functions use it as scratch without
 *   save/restore. For SEH functions, g_seh_ebp bridges the gap.
 *
 * Calling convention:
 *   All translated functions are void(void). Arguments are passed
 *   on the simulated Xbox stack (via push instructions before call).
 *   Return values are communicated through g_eax.
 *   The call instruction pushes a dummy return address; ret pops it.
 */

#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
/* The env-gated instrumentation in the generated sources calls getenv()
 * and clock(). Without these declarations both are assumed to return
 * `int`, which truncates getenv's pointer on Win64 -- a warning that is
 * a crash waiting for the probe to be enabled, not noise. */
#include <stdlib.h>
#include <time.h>
#include <math.h>   /* x87 transcendentals: the lifter's
                      * fcos/fsin/fpatan/fyl2x/fscale sites translate
                      * into the C library's equivalents. */

/* MSVC's __forceinline -> gcc/clang equivalent on POSIX. */
#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif

/* MSVC's __debugbreak() intrinsic -> gcc/clang equivalent.
 * The auto-generated code emits __debugbreak for x86 INT 3 instructions.
 * These are often original-game assert/"should never happen" traps; a real
 * debugger (or an Xbox retail kernel with no debugger attached) continues
 * past them rather than aborting, so log once per site and fall through
 * instead of hard-crashing the whole process (see recomp_debugbreak_log_once
 * in recomp_manual.c). */
#if !defined(_MSC_VER) && !defined(__debugbreak)
#define RECOMP_STRINGIZE_(x) #x
#define RECOMP_STRINGIZE(x) RECOMP_STRINGIZE_(x)
#define __debugbreak() recomp_debugbreak_log_once(__FILE__ ":" RECOMP_STRINGIZE(__LINE__))
#endif

/* ================================================================
 * Memory offset
 * ================================================================ */

/**
 * Memory offset from Xbox VA to actual mapped address.
 * When Xbox memory is mapped at the original address (0x00010000),
 * this is 0 and the MEM macros are simple identity casts.
 * When mapped elsewhere, this adjusts all memory accesses.
 *
 * Set once during memory initialization, then read-only.
 */
extern ptrdiff_t g_xbox_mem_offset;

/* ================================================================
 * Global registers
 * ================================================================ */

/**
 * Volatile x86 registers (caller-saved):
 *   eax - return values, general accumulator
 *   ecx - 'this' pointer for thiscall, loop counter
 *   edx - high dword of multiply/divide, general
 *   esp - stack pointer (initialized to top of Xbox stack)
 *
 * Callee-saved x86 registers (also global):
 *   ebx, esi, edi - global because callers pass implicit parameters
 *   through them. The callee-save contract is enforced by generated
 *   PUSH32/POP32 instructions.
 *
 * NOT global: ebp - stays local in each function because FPO
 * functions use it as scratch. For SEH, g_seh_ebp bridges the gap.
 */
/* Thread-local: each real Xbox thread (see bridge_PsCreateSystemThreadEx) gets its own
 * independent register state -- see the definition in xbox_memory_layout.c for why. */
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __thread uint32_t g_ebx, g_esi, g_edi;

/**
 * SEH frame pointer bridge.
 *
 * __SEH_prolog sets up ebp for the caller, but since ebp is a local
 * variable in each function, the caller can't see the prolog's change.
 * The prolog writes g_seh_ebp, and the caller reads it after the call.
 * Similarly, __SEH_epilog reads g_seh_ebp at entry and writes it at exit.
 */
extern __thread uint32_t g_seh_ebp;

/*
 * Carry-in across a lifted function boundary.
 *
 * `_cf` is a per-function local, so a carry flag produced in one lifted
 * function and consumed in another is simply lost. That happens where the
 * original compiler laid an inlined string compare's mismatch block at an
 * address the lifter chose to split into its own function: the predecessor
 * `cmp`/`jne` ends up as a tail call, and the `sbb r,r` at the callee's first
 * instruction reads a `_cf` that was just initialised to 0.
 *
 * Callers on such an edge set this before the tail call; those callees
 * initialise their `_cf` from it instead of 0.
 */
extern __thread int g_recomp_cf_in;

/**
 * FPU-stack cross-call bridge, for CRT_ftol_TruncateToInt64 (and any sibling
 * float-conversion CRT helper) specifically.
 *
 * Every translated function's simulated FPU stack (`_fp_stack`/`_fp_top`,
 * declared locally inside each function) is fresh and zero-initialized on
 * entry -- there is no cross-function bridge for it, the same gap `ebp` has
 * (see g_seh_ebp above), except nothing plays g_seh_ebp's role for the FPU
 * stack anywhere in this codebase. This matters for `_ftol`-style CRT
 * helpers specifically: on real x86, the value to convert is left on the
 * caller's real x87 register stack (matching the standard MSVC calling
 * convention for these helpers -- no explicit integer argument is pushed),
 * and the callee's `fld st(0)` reads it directly, since the FPU registers
 * are genuine CPU state that persists across the call boundary automatically.
 * In this simulated environment that isn't true: confirmed live that a
 * caller computing a real value (e.g. `fp_top()=33.333332`) results in the
 * callee seeing `fp_top()=0.0` at entry, silently converting from zero
 * instead of the intended value.
 *
 * This is deliberately a narrow, single-purpose bridge for
 * `CRT_ftol_TruncateToInt64` alone -- not a general fix for every function
 * that happens to use the FPU across a call boundary (that would need
 * promoting the entire `_fp_stack`/`_fp_top` pair to `__thread` globals
 * project-wide, a much larger change with real regression risk for
 * functions whose own FPU use is genuinely self-contained per call). Every
 * call site sets this immediately before calling
 * `CRT_ftol_TruncateToInt64()`; the function itself reads it in place of its
 * own (otherwise-empty) `fp_top()` for the "fld st(0)" duplicate at entry.
 */
extern __thread double g_ftol_arg;

/**
 * Shared FPU stack for the `CRT_ftol_TruncateToInt64` fragment chain
 * (`CRT_ftol_TruncateToInt64`, `sub_0015CA8B`, `sub_0015CAAF`, `sub_0015CAC7`,
 * `sub_0015CADB`).
 *
 * These are all lifted fragments of a single original x86 function (branch
 * targets and fall-throughs within 0x0015CA68-0x0015CADD), exactly the same
 * situation `g_seh_ebp` exists to handle for `ebp` -- the lifter split one
 * function into several C functions, each of which normally declares its
 * own fresh local `_fp_stack`/`_fp_top`, so a value one fragment pushes is
 * invisible to the next fragment it tail-jumps into. On real hardware this
 * doesn't matter, since the FPU stack is genuine CPU state that persists
 * across a jump within the same function. These 5 functions are rewired to
 * share this pair (via their `fp_push`/`fp_pop`/`fp_top`/`fp_st1` macros)
 * instead of each declaring their own, so a push in one fragment is
 * correctly visible in the next. `CRT_ftol_TruncateToInt64` itself resets
 * `g_ftol_fp_top` to 0 and seeds the stack from `g_ftol_arg` at its own
 * entry (the one point this chain is reached from outside itself); no other
 * fragment resets it. Scoped to just this one function chain rather than a
 * project-wide change, for the same risk-management reason as `g_ftol_arg`
 * above.
 */
extern __thread double g_ftol_fp_stack[8];
extern __thread int g_ftol_fp_top;

/**
 * Shadow of the real x87 ST(0) register, mirrored by every function-local
 * `fp_push` in the generated code (see the sed-generated `fp_push` macros
 * throughout recomp_gen/*.c -- each one now assigns through `g_x87_st0` as
 * well as its own local `_fp_stack`). On real hardware, a function that ends
 * with "load a value, then return" (MSVC cdecl float/double return
 * convention: value left in ST(0), no explicit push at the call site) leaves
 * that value sitting in a register the *caller* can read. Our per-function
 * local `_fp_stack`/`_fp_top` arrays are torn down on return, silently
 * dropping this value. A handful of call sites (found while wiring the
 * CRT_ftol_TruncateToInt64 bridge -- see g_ftol_arg above) consume a
 * "returned" float this way with no FPU pushes of their own before the
 * consuming CRT_ftol_TruncateToInt64() call, so they read g_x87_st0 instead
 * of a local fp_top() that doesn't exist for them. The mirror-write is
 * purely additive (every existing fp_push still does exactly what it did
 * before, plus this one extra assignment), so it's safe to apply project-wide
 * without auditing every one of the ~2000 call sites that already push
 * correctly through their own local stack -- only the orphan sites that
 * had no local stack at all needed to change what they read.
 */
extern __thread double g_x87_st0;

/* x87 comparison result, as the status word's C3/C2/C0 bits would encode it.
 *
 * `fcomp` / `fcom` set the FPU status word and the following `fnstsw ax` copies
 * it into AH, which the code then tests with `test ah, 0x41`. The generator
 * emitted the fnstsw as a *comment only*, so every one of those branches read a
 * stale AH -- 2,074 sites, i.e. every floating-point comparison in the build.
 *
 * It is a global rather than a local because the status word is CPU state that
 * outlives a function: several lifted fragments run the `fnstsw` for a compare
 * their caller performed. -1 = ST(0) < operand, 0 = equal, 1 = ST(0) > operand. */
extern __thread int g_fpu_cmp;

/* The x87 control word, for its rounding-control field (bits 10-11): MSVC's
 * _ftol switches to truncation with fldcw before a fistp, while code that
 * issues fistp directly rounds to nearest. fnstcw/fldcw read and write this;
 * fistp and frndint round through x87_rint. 0x027F is the power-on value. */
extern __thread uint16_t g_x87_cw;
/* 80-bit extended values (fld/fstp tbyte): 64-bit mantissa with an explicit
 * integer bit, 15-bit exponent biased by 16383, sign in bit 79. The simulated
 * register file holds doubles, so these convert at the boundary. */
static inline double x87_load80(const void *p)
{
    const unsigned char *b = (const unsigned char *)p;
    uint64_t mant; uint16_t se; int e; double v;
    memcpy(&mant, b, 8); memcpy(&se, b + 8, 2);
    e = se & 0x7FFF;
    if (e == 0 && mant == 0) v = 0.0;
    else if (e == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, e - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
static inline void x87_store80(void *p, double v)
{
    unsigned char *b = (unsigned char *)p;
    uint64_t mant = 0; uint16_t se = signbit(v) ? 0x8000 : 0; int e;
    double a = fabs(v);
    if (isnan(v)) { mant = 0xC000000000000000ull; se |= 0x7FFF; }
    else if (isinf(v)) { mant = 0x8000000000000000ull; se |= 0x7FFF; }
    else if (a != 0.0) {
        double f = frexp(a, &e);              /* a = f * 2^e, 0.5 <= f < 1 */
        mant = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(b, &mant, 8); memcpy(b + 8, &se, 2);
}

static inline double x87_rint(double v)
{
    switch ((g_x87_cw >> 10) & 3) {
    case 1:  return floor(v);
    case 2:  return ceil(v);
    case 3:  return trunc(v);
    default: return nearbyint(v);
    }
}

/* Zero-flag result of the last operation whose comparison and branch are
 * emitted as separate statements: `repe cmps`, `repne scas`, and `cmpxchg`.
 * 0 means the operation ended equal (ZF set), non-zero means it did not.
 * The branch that reads it can be several lines later, so it has to survive
 * between the two. */
extern __thread int g_str_ne;

/* The x87 control word.
 *
 * `fnstcw` and `fldcw` were emitted as bare comments, so the memory a function
 * stores the control word into was never written -- and the CRT's standard
 * idiom is to save it, modify a copy, load that, work, then load the saved one
 * back. Reading an uninitialised word as the control word made the title
 * believe an exception was pending and call `RtlRaiseException` with
 * STATUS_FLOAT_INEXACT_RESULT 7,848 times a run.
 *
 * Modelled as storage only. The arithmetic here is C doubles, so the rounding
 * and precision fields have nothing to act on, and the exception mask has no
 * real exceptions to mask -- what matters is that a value stored is the value
 * read back. 0x027F is the x87's own initial value: all six exceptions masked,
 * round to nearest, 64-bit precision. */
extern __thread uint16_t g_x87_cw;

/* The condition bits in AH (C0 0x01, C1 0x02, C2 0x04, C3 0x40) as `fnstsw`
 * reads them. g_fpu_cmp normally holds a comparison (-1/0/1: C0 for less,
 * C3 for equal, nothing for greater); `fxam` stores 0x1000 | AH instead,
 * because its answer -- the operand's class -- does not fit a comparison.
 * It used to be a no-op, so the CRT's atan2 classified every argument from
 * the previous compare, took its special-value path and returned NaN: that
 * NaN became the race camera's pitch and hid the whole course. */
#define FPU_AH()  ((g_fpu_cmp >= 0x1000) ? (uint32_t)(g_fpu_cmp & 0xFF) \
                   : (g_fpu_cmp < 0) ? 0x01u : (g_fpu_cmp == 0) ? 0x40u : 0x00u)

/* `fnstsw ax` writes the whole status word into AX: the condition bits in
 * AH and the sticky exception flags in AL. Only AH used to be written, so AL
 * kept whatever eax held. The CRT's sin/cos/atan2 exit path tests AL for
 * pending exceptions whenever the control word is not 0x027F -- and read
 * garbage as 'invalid operation': it raised STATUS_FLOAT_INVALID_OPERATION and
 * returned the indefinite NaN, which became the race camera's rotation and
 * hid the course. No exception flags are modelled, so AL is 0. */
extern int g_x87_compat;
int x87_compat_init(void);
#define X87_COMPAT() (g_x87_compat >= 0 ? g_x87_compat : x87_compat_init())
extern int g_x87_status_lo;   /* AL after fnstsw ax: XBOX_X87_STATUS_LO, default 0 */
#define FNSTSW_AX(reg) ((X87_COMPAT() & 1) ? (void)SET_HI8((reg), FPU_AH()) \
    : (void)((reg) = ((reg) & 0xFFFF0000u) | ((uint32_t)FPU_AH() << 8) | (uint32_t)(g_x87_status_lo & 0xFF)))

/* fxam: C3/C2/C0 name the class, C1 is the sign. */
static inline int x87_fxam(double v)
{
    int ah = signbit(v) ? 0x02 : 0;
    if (X87_COMPAT() & 2) return g_fpu_cmp;     /* old no-op */
    if (isnan(v))                          ah |= 0x01;   /* NaN: C0 */
    else if (isinf(v))                     ah |= 0x05;   /* infinity: C2 C0 */
    else if (v == 0.0)                     ah |= 0x40;   /* zero: C3 */
    else if (fpclassify(v) == FP_SUBNORMAL) ah |= 0x44;  /* denormal: C3 C2 */
    else                                   ah |= 0x04;   /* normal: C2 */
    return 0x1000 | ah;
}

/* Rotates at the operand's width: ROL32 on a byte moved bit 7 to bit 8,
 * where the byte write dropped it instead of wrapping it round. */
static inline uint32_t ROL8(uint32_t v, int n)  { v &= 0xFFu;   n &= 7;  return n ? ((v << n) | (v >> (8 - n))) & 0xFFu : v; }
static inline uint32_t ROR8(uint32_t v, int n)  { v &= 0xFFu;   n &= 7;  return n ? ((v >> n) | (v << (8 - n))) & 0xFFu : v; }
static inline uint32_t ROL16(uint32_t v, int n) { v &= 0xFFFFu; n &= 15; return n ? ((v << n) | (v >> (16 - n))) & 0xFFFFu : v; }
static inline uint32_t ROR16(uint32_t v, int n) { v &= 0xFFFFu; n &= 15; return n ? ((v >> n) | (v << (16 - n))) & 0xFFFFu : v; }

/* PF after `test ah, mask`.
 *
 * x86 sets PF when the low byte of the result has an *even* number of set bits,
 * which is why compilers use `test ah,0x05` / `jp` to mean "not less than" and
 * `test ah,0x44` / `jp` to mean "not equal": the mask selects C0/C2/C3 and the
 * parity of what survives encodes the comparison.
 *
 * The recompiler dropped the whole idiom -- `fnstsw` became a comment, the
 * `test` vanished, and the branch was emitted as `if (1)`, always taken, in both
 * directions.  fixfpbranch.py recovers each mask from the XBE and rewrites the
 * branch through this macro. */
#define FPU_PARITY(mask)  (!__builtin_parity(FPU_AH() & (unsigned)(mask)))


/**
 * The simulated x87 register stack, shared by every translated function.
 *
 * This used to be a `double _fp_stack[8]; int _fp_top = 0;` pair declared
 * locally inside each of the ~2000 generated functions, which meant a value
 * pushed by one lifted fragment was invisible to the next fragment it
 * tail-jumps into -- exactly the gap `g_seh_ebp` closes for `ebp`. On real
 * hardware the x87 register file is CPU state that survives calls and jumps
 * alike. The narrow bridges above (g_ftol_arg, g_ftol_fp_stack) were built for
 * the one chain where that was proven to bite; the render path was the second,
 * where a mid-loop fragment inherits its operands from its caller and cannot be
 * recovered correctly at all while every fragment starts from a fresh zeroed
 * array.
 *
 * The access macros already mask with `& 7`, i.e. they model x87's circular
 * eight-register file, so sharing the pair is strictly more faithful than
 * resetting it per call -- a fragment reading st(0) now sees what the previous
 * one left there rather than 0.0. Thread-local because each guest thread has
 * its own FPU state.
 */
extern __thread double g_fp_stack[8];
extern __thread int g_fp_top;

/**
 * Float return across a call (tools/audit/fpretcheck.py). fixfpret.py's
 * `fp_push(g_x87_st0)` hand-off predates the shared stack above: a callee's
 * ST(0) is now already on it, and g_x87_st0 is only its *last push* (the angle
 * wrapper sub_0001E600 ends `fld 2pi; fmulp; fsubr`, so callers got pi). Each
 * site records the depth before the call in `_fpc`; `before - g_fp_top` is how
 * many values the callee left. XBOX_X87_RET: 0 = old unconditional hand-off,
 * 1 = hand off only when the callee left nothing, 2 = like 1 and also drop
 * whatever extra the callee leaked, so the caller's depth is exactly
 * before + 1 as the ABI guarantees (default). Diagnostics (x87_ret_diag, out of line so
 * they change without a full rebuild): | 0x100 logs site outcomes,
 * | 0x200 applies the mode only to sites in XBOX_X87_RET_SITES=lo-hi.
 */
extern int g_x87_ret_mode;
int x87_ret_init(void);
int x87_ret_diag(uint32_t site, int left, int mode);
static inline void x87_ret(int before, uint32_t site)
{
    int mode = g_x87_ret_mode >= 0 ? g_x87_ret_mode : x87_ret_init();
    int left = before - g_fp_top;
    if (mode & 0x300)
        mode = x87_ret_diag(site, left, mode);
    if ((mode & 0xFF) == 0) {
        g_fp_stack[--g_fp_top & 7] = g_x87_st0;
    } else if ((mode & 0xFF) == 1) {
        if (left <= 0)
            g_fp_stack[--g_fp_top & 7] = g_x87_st0;
    } else {
        double v = left >= 1 ? g_fp_stack[g_fp_top & 7] : g_x87_st0;
        g_fp_top = before - 1;
        g_fp_stack[g_fp_top & 7] = g_x87_st0 = v;
    }
}
#define X87_RET(before, site) x87_ret((before), (site))

/* ================================================================
 * ICALL trace ring buffer (for debugging indirect calls)
 * ================================================================ */

/** Size of the ring buffer (must be power of 2). */
#define ICALL_TRACE_SIZE 16

/** Ring buffer of recent indirect call target VAs. */
extern volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];

/** Current write index into the ring buffer. */
extern volatile uint32_t g_icall_trace_idx;

/** Total count of indirect calls executed. */
extern volatile uint64_t g_icall_count;

/**
 * Called when an indirect call target cannot be resolved.
 * Implement this in your game-specific code to log diagnostics.
 * The va parameter is the Xbox VA that failed to resolve.
 */
void recomp_icall_fail_log(uint32_t va);
/* Prints each unique unresolved ICALL target VA exactly once (deduped) --
 * used to find what to seed next without drowning in repeat-call spam.
 * Defined in recomp_manual.c. */
void recomp_icall_miss_log_once(uint32_t va);
/* Same, but records the generated call site, so a miss names the exact
 * RECOMP_ICALL_SAFE that produced it instead of just the containing file. */
void recomp_icall_miss_log_at(uint32_t va, const char *file, int line);
/* Prints each unique original-INT3 debug-break site exactly once (deduped
 * by __FILE__:__LINE__). Defined in recomp_manual.c. */
void recomp_debugbreak_log_once(const char *site);

/* ================================================================
 * Memory access helpers
 * ================================================================ */

/**
 * Real Xbox hardware maps physical RAM twice: once at 0x00000000
 * (normal, cached) and again at 0x80000000 (uncached -- the top address
 * bit disables the CPU cache for that access). Drivers use the uncached
 * alias for writes that must be immediately visible to the GPU (which
 * reads RAM directly, bypassing the CPU cache), typically followed by a
 * wbinvd to flush anything still dirty in cache back to the cached view.
 * Confirmed as genuine, deliberate Xbox driver behavior (not a bug) by
 * disassembling real D3D8 driver bytes: `mov dword ptr [0x80000000], edx`
 * immediately followed by `wbinvd`.
 *
 * We don't have a real CPU cache to bypass, and modeling this via actual
 * OS-level address-space aliasing (a second MapViewOfFileEx at native
 * address base+0x80000000) is fragile -- that fixed native address can
 * collide with whatever else Windows/ASLR has already placed in the
 * process's address space, and then fails unpredictably per-machine.
 * Resolving the alias here instead, by masking off the top bit before
 * translating, works unconditionally: Xbox VA 0x80000000+k always reads
 * and writes the exact same memory as Xbox VA k, with no dependency on
 * OS memory layout at all. Restricted to the real hardware's 64 MB
 * window (0x80000000-0x83FFFFFF) so it can't accidentally swallow
 * unrelated high addresses (e.g. the GPU MMIO aperture at 0xFD000000).
 */
/*
 * Per-thread Thread Information Block, Xbox VA.
 *
 * The lifter drops `fs:` segment prefixes, so `mov eax, fs:0x28` arrives here
 * as a plain absolute access below 0x100. Those used to land on one fake TIB
 * shared by every guest thread; the title genuinely relies on `fs:` being
 * per-thread (SEH chain head at +0x00, TLS block at +0x28), and sharing them
 * scrambled both. Each thread now owns a block out of the TIB pool -- see
 * xbox_memory_layout.h -- and the redirect below sends its low accesses there.
 *
 * 0 means "no slot claimed": the access falls through to the shared block at
 * VA 0, which is the old behaviour.
 */
extern __thread uint32_t g_xbox_tib_va;

/* Where the synthetic kernel PE header actually lives. This mirrors
 * xbox_memory_layout.h's XBOX_FAKE_KERNEL_HEADER_VA, which this header cannot
 * include, and it is a variable rather than a literal for a reason: it *was*
 * a literal 0x00741000, and when the kernel data area moved during the 64 MB
 * layout work the literal stayed behind and pointed into the title's arena.
 * Every RenderWare cache-line read then got garbage, and three separate
 * memory layouts "rendered nothing" for what looked like a memory-map reason.
 * Set once by xbox_MemoryLayoutInit. */
extern uint32_t g_xbox_fake_hdr_va;

/*
 * A comparison that has to survive a function split.
 *
 * The disassembler cuts one machine function into several C functions at
 * branch targets. When the flag-setting instruction lands in one piece and the
 * `jcc` that reads it lands in the next, the condition has nowhere to live:
 * the lifter emits `int _flags = 0;` in the consumer and `(void)0; /* ...
 * flags set for next jcc *\/` in the producer, so the branch tests a constant
 * zero and is simply never taken. 29 branches across 23 functions were dead
 * this way.
 *
 * One of them is why the frontend never loads: FILE's pack-directory search
 * compares an entry name with `test eax,eax` in sub_0014FC8B and branches on
 * it in sub_0014FC9A, so no name ever matched, every `|pack` entry open
 * returned NULL, and the caller wrote a quarter-million words from Xbox VA 0.
 *
 * These carry the operands instead of the flags, which is enough to rebuild
 * every condition except the overflow and parity ones.
 */
extern __thread uint32_t g_cmp_a;
extern __thread uint32_t g_cmp_b;
extern __thread int      g_cmp_test;   /* 1 = `test` (CF/OF clear), 0 = `cmp` */

#define SPLIT_CMP(a_, b_, test_)     (g_cmp_a = (uint32_t)(a_), g_cmp_b = (uint32_t)(b_), g_cmp_test = (test_))

#define SPLIT_AND   ((uint32_t)(g_cmp_a & g_cmp_b))
#define SPLIT_JE    (g_cmp_test ? (SPLIT_AND == 0u) : (g_cmp_a == g_cmp_b))
#define SPLIT_JNE   (!SPLIT_JE)
#define SPLIT_JB    (g_cmp_test ? 0 : (g_cmp_a <  g_cmp_b))
#define SPLIT_JAE   (!SPLIT_JB)
#define SPLIT_JBE   (g_cmp_test ? (SPLIT_AND == 0u) : (g_cmp_a <= g_cmp_b))
#define SPLIT_JA    (!SPLIT_JBE)
#define SPLIT_JL    (g_cmp_test ? ((int32_t)SPLIT_AND <  0)                                 : ((int32_t)g_cmp_a <  (int32_t)g_cmp_b))
#define SPLIT_JGE   (!SPLIT_JL)
#define SPLIT_JLE   (g_cmp_test ? ((int32_t)SPLIT_AND <= 0)                                 : ((int32_t)g_cmp_a <= (int32_t)g_cmp_b))
#define SPLIT_JG    (!SPLIT_JLE)

static inline uint32_t xbox_resolve_uncached_alias(uint32_t va) {
    /* fs:-relative access -- see g_xbox_tib_va above. First test because it is
     * the cheapest and, for ordinary addresses, always false. */
    /*
     * Kept as a safety net -- removing it outright unmasks latent null-pointer
     * writes elsewhere in the title (sub_00172202 faults at once) -- but with
     * offsets 8 and 0xC excluded.
     *
     * Every real fs: access now goes through the FS8/FS16/FS32 macros above,
     * emitted by the lifter from Capstone's mem.segment, so nothing that
     * genuinely wants TIB data arrives here any more. Offsets 8 and 0xC are the
     * ones the NULL circular-list terminator sub_000A3890 reads (it tests
     * MEM32(ecx + 8) == ecx with ecx == 0); KPCR+8 is StackLimit, legitimately
     * non-zero, so while the redirect covered them the terminator could never
     * report end-of-list -- hanging UI_BuildButtonGroup and the frontend behind
     * it.
     */
    if ((va == 8u || va == 0xCu) && g_xbox_tib_va) {
        extern void xbox_lowaccess_log(unsigned va, void *ra);
        extern int g_lowacc_enabled;
        if (g_lowacc_enabled) xbox_lowaccess_log(va, __builtin_return_address(0));
    }
    /*
     * Absorb null-pointer accesses on a page of their own.
     *
     * This used to return `g_xbox_tib_va + va`, i.e. it put the landing zone
     * for every stray null dereference directly on top of the calling thread's
     * live KPCR. It landed: a null-based `[ecx+0x24]` store wrote 0xBC over
     * KPCR+0x24 (Irql), after which the CRT's _getptd read an IRQL of 188,
     * concluded it was at raised IRQL and called KeBugCheck(0x0A) -- ending
     * every run at exit 10 as soon as the video played.
     *
     * The redirect itself stays: removing it makes sub_00172202 fault at once,
     * so the latent null writes are still there to be found one at a time
     * (XBOX_PROTECT_LOWPAGE=1 turns this back into a fault). What changes is
     * that they can no longer corrupt per-thread state.
     */
    if (va < 0x100u) {
        return 0x04102000u /* XBOX_NULL_PAGE_VA */ + va;
    }
    /* Synthetic Xbox-kernel PE header page (xbox_memory_layout.h's
     * XBOX_FAKE_KERNEL_HEADER_VA / xbox_MemoryLayoutInit) -- redirected
     * here, ahead of the generic uncached-alias mask below, so reads of
     * the real kernel's PE-header-detection page (0x80010000, read by
     * RenderWare's xbcache.c) land on synthetic all-zero-except-e_lfanew
     * data instead of falling through to the mask and aliasing the real
     * XBE header at 0x00010000 (wrong data) or, in an earlier version of
     * this fix, a separate fixed-native-address mapping that could
     * collide with a RAM-wrap mirror view at the same relative offset. */
    if (va >= 0x80010000u && va < 0x80011000u) {
        return g_xbox_fake_hdr_va + (va - 0x80010000u);
    }
    if (va >= 0x80000000u && va < 0x84000000u) {
        return va & 0x7FFFFFFFu;
    }
    /*
     * Write-combined alias: on Xbox, physical RAM is visible three times --
     * cached at 0x00000000, uncached at 0x80000000 (above), and
     * write-combined at 0xF0000000. Only the first two were handled, so a
     * 0xF0000000-based pointer fell through unchanged and XBOX_PTR resolved
     * it into whichever RAM mirror view happened to cover that offset, i.e.
     * a real page but the *wrong* one.
     *
     * D3D8 hands the title exactly such a pointer: LockRect on the back
     * buffer returns `physical | 0xF0000000` (0xF3BA0000 for the buffer at
     * 0x03BA0000), because a surface being written by the CPU wants
     * write-combining. Every video frame was therefore blitted into memory
     * about 62 MB away from the back buffer, on top of the MPEG decoder's
     * own buffers -- which is what made it abort with "Invalid motion_vector
     * code" and bugcheck.
     */
    if (va >= 0xF0000000u && va < 0xF4000000u) {
        return va & 0x03FFFFFFu;
    }
    return va;
}

/**
 * Translate an Xbox VA to an actual pointer.
 * Mask to 32-bit first: Xbox addresses are 32-bit and arithmetic
 * in the recompiled code can overflow. Without the mask, a 64-bit
 * uintptr_t cast preserves the overflow bits, landing us 4GB+ past
 * our mapping and causing access violations.
 */
#define XBOX_PTR(addr) \
    ((uintptr_t)xbox_resolve_uncached_alias((uint32_t)(addr)) + g_xbox_mem_offset)

/*
 * Explicit fs:-relative (TIB) access.
 *
 * The lifter now emits these whenever Capstone reports an fs: segment override
 * on a memory operand, so a real `mov eax, fs:0x28` is no longer spelled the
 * same as a genuine null dereference at offset 0x28. That ambiguity is what the
 * low-address TIB redirect below exists to work around, and why the circular-
 * list terminator sub_000A3890 -- which tests MEM32(ecx+8) == ecx with ecx == 0
 * -- could never report end-of-list.
 */
#define FS8(off)   (*(volatile uint8_t  *)((uintptr_t)(g_xbox_tib_va + (off)) + g_xbox_mem_offset))
#define FS16(off)  (*(volatile uint16_t *)((uintptr_t)(g_xbox_tib_va + (off)) + g_xbox_mem_offset))
#define FS32(off)  (*(volatile uint32_t *)((uintptr_t)(g_xbox_tib_va + (off)) + g_xbox_mem_offset))

/** Read/write N bytes at a flat Xbox memory address. */
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))

/** Signed memory reads. */
#define SMEM8(addr)  (*(volatile int8_t   *)XBOX_PTR(addr))
#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))
#define SMEM32(addr) (*(volatile int32_t  *)XBOX_PTR(addr))

/** Float/double memory access. */
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))

/**
 * 128-bit SSE memory access.
 *
 * MEMF is a 4-byte float and MEMD an 8-byte double: correct for movss and
 * movlps respectively, but WRONG for the full-width moves. movaps / movups /
 * movdqa / movdqu each transfer 16 bytes, so lifting them through MEMF copies
 * only the first quarter and silently leaves the rest of the destination
 * holding whatever was there before (confirmed live: the CRT's unaligned
 * memcpy fixup copied 4 of 16 bytes, so a filename came out as "data" followed
 * by stale heap free-list links).
 *
 * Use a plain struct rather than a vector type so the copy has no alignment
 * requirement -- movups and the unaligned leg of movaps fixups both land on
 * addresses that are not 16-byte aligned.
 */
typedef struct { unsigned char b[16]; } recomp_x128_t;
#define MEMX(addr)   (*(recomp_x128_t *)XBOX_PTR(addr))

/**
 * An XMM register.
 *
 * The lifter used to declare these as a bare `float`, which is only correct
 * for the scalar single forms (movss, addss, ...). Anything full-width -- a
 * movaps of a matrix row, a packed add -- then silently operated on one lane
 * and left the other three holding whatever was there before. Real 16-byte
 * storage with a member per access width fixes that while keeping the scalar
 * spelling cheap:
 *
 *   .f   low lane as float      (movss, addss, cvtsi2ss, ...)
 *   .d   low 8 bytes as double  (movlps, movsd)
 *   .l[] four packed lanes      (addps, mulps, shufps, ...)
 *   .x   all 16 bytes           (movaps, movups)
 *
 * .f aliases .l[0] on every little-endian target, which is exactly the
 * relationship the hardware has between the scalar and packed views.
 */
typedef union recomp_xmm_u {
    float          f;
    double         d;
    float          l[4];
    uint32_t       u[4];
    recomp_x128_t  x;
} recomp_xmm_t;

/* The SSE and MMX register files, per thread. They were locals
 * in each generated function, so a value live across a call, a tail jump or
 * a fall-through into a split fragment was lost -- the fragment read an
 * uninitialised local (27 functions, the terrain/physics maths among them:
 * sub_00030CA0 took rcpps of garbage and wrote NaN into the rider). Same
 * cure as the x87 stack in part 179: one register file, as on the CPU.
 * Defined in recomp_manual.c. */
extern __thread recomp_xmm_t g_xmm[8];
extern __thread uint64_t g_mm[8];
#define xmm0 g_xmm[0]
#define xmm1 g_xmm[1]
#define xmm2 g_xmm[2]
#define xmm3 g_xmm[3]
#define xmm4 g_xmm[4]
#define xmm5 g_xmm[5]
#define xmm6 g_xmm[6]
#define xmm7 g_xmm[7]
#define mm0 g_mm[0]
#define mm1 g_mm[1]
#define mm2 g_mm[2]
#define mm3 g_mm[3]
#define mm4 g_mm[4]
#define mm5 g_mm[5]
#define mm6 g_mm[6]
#define mm7 g_mm[7]

/** A 128-bit memory operand read as four packed lanes. */
#define XMMM(addr)   (*(const recomp_xmm_t *)XBOX_PTR(addr))

/*
 * Packed SSE operations.
 *
 * The lifter emits these as bare comments -- `addps` and friends had no effect
 * at all, which left every vector and matrix routine in the title computing
 * nothing. Each macro copies its source first so a memory operand that
 * overlaps the destination still behaves like the hardware, which reads both
 * operands before writing.
 */
#define XMM_BINOP(d, s, op)     do { recomp_xmm_t _s_ = (s); int _i_;          for (_i_ = 0; _i_ < 4; _i_++) (d).l[_i_] op _s_.l[_i_]; } while (0)

#define XMM_MINPS(d, s)     do { recomp_xmm_t _s_ = (s); int _i_;          for (_i_ = 0; _i_ < 4; _i_++) if (_s_.l[_i_] < (d).l[_i_]) (d).l[_i_] = _s_.l[_i_]; } while (0)

#define XMM_MAXPS(d, s)     do { recomp_xmm_t _s_ = (s); int _i_;          for (_i_ = 0; _i_ < 4; _i_++) if (_s_.l[_i_] > (d).l[_i_]) (d).l[_i_] = _s_.l[_i_]; } while (0)

/* Packed compares write an all-ones or all-zero mask per lane. */
#define XMM_CMPPS(d, s, cmp)     do { recomp_xmm_t _s_ = (s), _d_ = (d); int _i_;          for (_i_ = 0; _i_ < 4; _i_++)              (d).u[_i_] = (_d_.l[_i_] cmp _s_.l[_i_]) ? 0xFFFFFFFFu : 0u; } while (0)

#define XMM_SHUFPS(d, s, imm)     do { recomp_xmm_t _a_ = (d), _b_ = (s);          (d).l[0] = _a_.l[((imm) >> 0) & 3]; (d).l[1] = _a_.l[((imm) >> 2) & 3];          (d).l[2] = _b_.l[((imm) >> 4) & 3]; (d).l[3] = _b_.l[((imm) >> 6) & 3]; } while (0)

#define XMM_UNPCKLPS(d, s)     do { recomp_xmm_t _a_ = (d), _b_ = (s);          (d).l[0] = _a_.l[0]; (d).l[1] = _b_.l[0];          (d).l[2] = _a_.l[1]; (d).l[3] = _b_.l[1]; } while (0)

#define XMM_UNPCKHPS(d, s)     do { recomp_xmm_t _a_ = (d), _b_ = (s);          (d).l[0] = _a_.l[2]; (d).l[1] = _b_.l[2];          (d).l[2] = _a_.l[3]; (d).l[3] = _b_.l[3]; } while (0)

/* Half-register moves. Both read the whole source before writing, so a
 * destination that overlaps the source behaves like the hardware. */
#define XMM_MOVLHPS(d, s)     do { recomp_xmm_t _s_ = (s); (d).l[2] = _s_.l[0]; (d).l[3] = _s_.l[1]; } while (0)

#define XMM_MOVHLPS(d, s)     do { recomp_xmm_t _s_ = (s); (d).l[0] = _s_.l[2]; (d).l[1] = _s_.l[3]; } while (0)

/* ANDNPS is (NOT dst) AND src -- the destination is the inverted operand. */
#define XMM_ANDNPS(d, s)     do { recomp_xmm_t _s_ = (s); int _i_;          for (_i_ = 0; _i_ < 4; _i_++) (d).u[_i_] = (~(d).u[_i_]) & _s_.u[_i_]; } while (0)

/** A 64-bit memory operand, for the MMX loads and stores. */
#define MEM64(addr)  (*(volatile uint64_t *)XBOX_PTR(addr))

/*
 * MMX.
 *
 * The generated code already declares mm0-mm7 as plain `uint64_t`, so the
 * register file exists; what was missing is the arithmetic. Every one of these
 * was emitted as a bare `TODO` comment, which means the packed adds, shifts,
 * pack/unpack and multiply-add did nothing while the surrounding scalar code
 * carried on as if they had.
 *
 * Written as typed inline functions rather than macros: these have real lane
 * semantics -- saturation, signedness, adjacent-pair accumulation -- and a
 * macro that gets one of those subtly wrong is far harder to see than a
 * function that states its types. Lane 0 is the low bits, as on hardware.
 */
static inline uint64_t mm_lane_b(uint64_t v, int i) { return (v >> (i * 8)) & 0xFFu; }
static inline uint64_t mm_lane_w(uint64_t v, int i) { return (v >> (i * 16)) & 0xFFFFu; }
static inline uint64_t mm_lane_d(uint64_t v, int i) { return (v >> (i * 32)) & 0xFFFFFFFFu; }

static inline uint64_t mm_paddb(uint64_t d, uint64_t s) {
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++)
        r |= ((mm_lane_b(d, i) + mm_lane_b(s, i)) & 0xFFu) << (i * 8);
    return r;
}
static inline uint64_t mm_paddw(uint64_t d, uint64_t s) {
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++)
        r |= ((mm_lane_w(d, i) + mm_lane_w(s, i)) & 0xFFFFu) << (i * 16);
    return r;
}
static inline uint64_t mm_paddd(uint64_t d, uint64_t s) {
    uint64_t r = 0; int i;
    for (i = 0; i < 2; i++)
        r |= ((mm_lane_d(d, i) + mm_lane_d(s, i)) & 0xFFFFFFFFu) << (i * 32);
    return r;
}
/* Unsigned saturating byte add: clamps at 255 rather than wrapping. */
static inline uint64_t mm_paddusb(uint64_t d, uint64_t s) {
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++) {
        uint64_t t = mm_lane_b(d, i) + mm_lane_b(s, i);
        if (t > 0xFFu) t = 0xFFu;
        r |= t << (i * 8);
    }
    return r;
}
/* Shift counts of 32 (or 64) and above flush the lane to zero on hardware,
 * where C's shift of a full-width operand would be undefined. */
static inline uint64_t mm_psrld(uint64_t d, unsigned c) {
    uint64_t r = 0; int i;
    if (c >= 32) return 0;
    for (i = 0; i < 2; i++)
        r |= (mm_lane_d(d, i) >> c) << (i * 32);
    return r;
}
static inline uint64_t mm_pslld(uint64_t d, unsigned c) {
    uint64_t r = 0; int i;
    if (c >= 32) return 0;
    for (i = 0; i < 2; i++)
        r |= ((mm_lane_d(d, i) << c) & 0xFFFFFFFFu) << (i * 32);
    return r;
}
static inline uint64_t mm_psrlq(uint64_t d, unsigned c) { return c >= 64 ? 0 : d >> c; }
static inline uint64_t mm_psllq(uint64_t d, unsigned c) { return c >= 64 ? 0 : d << c; }

static inline uint64_t mm_punpcklwd(uint64_t d, uint64_t s) {
    return mm_lane_w(d, 0) | (mm_lane_w(s, 0) << 16)
         | (mm_lane_w(d, 1) << 32) | (mm_lane_w(s, 1) << 48);
}
static inline uint64_t mm_punpckhwd(uint64_t d, uint64_t s) {
    return mm_lane_w(d, 2) | (mm_lane_w(s, 2) << 16)
         | (mm_lane_w(d, 3) << 32) | (mm_lane_w(s, 3) << 48);
}
/* Signed words in, unsigned bytes out, saturated at both ends. */
static inline uint64_t mm_packuswb(uint64_t d, uint64_t s) {
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++) {
        int32_t w = (int32_t)(int16_t)(uint16_t)mm_lane_w(i < 4 ? d : s, i & 3);
        if (w < 0) w = 0; else if (w > 255) w = 255;
        r |= (uint64_t)(uint32_t)w << (i * 8);
    }
    return r;
}
/* Four signed 16x16 products, summed in adjacent pairs into two dwords. */
static inline uint64_t mm_pmaddwd(uint64_t d, uint64_t s) {
    uint64_t r = 0; int i;
    for (i = 0; i < 2; i++) {
        int32_t a = (int32_t)(int16_t)(uint16_t)mm_lane_w(d, i * 2 + 0)
                  * (int32_t)(int16_t)(uint16_t)mm_lane_w(s, i * 2 + 0);
        int32_t b = (int32_t)(int16_t)(uint16_t)mm_lane_w(d, i * 2 + 1)
                  * (int32_t)(int16_t)(uint16_t)mm_lane_w(s, i * 2 + 1);
        r |= (uint64_t)(uint32_t)(a + b) << (i * 32);
    }
    return r;
}
/* Two signed dwords to the low two float lanes; the upper two are preserved. */
#define MM_CVTPI2PS(x, q)     do { uint64_t _q_ = (q);          (x).l[0] = (float)(int32_t)(uint32_t)mm_lane_d(_q_, 0);          (x).l[1] = (float)(int32_t)(uint32_t)mm_lane_d(_q_, 1); } while (0)

/**
 * x86 port I/O (in/out instructions).
 *
 * Unlike MEM8/MEM32, ports have no address-space representation to map --
 * each one is real hardware behavior that has to be modeled explicitly.
 * Dispatched to xbox_io_port_read/write (kernel_bridge.c), which know the
 * handful of ports actually hit in practice (e.g. 0x80C0, the Xbox ACPI
 * GPIO block's TV-encoder field-pin bit) and return 0 / no-op for anything
 * else -- the same "nothing here" behavior real unpopulated I/O space has.
 */
uint32_t xbox_io_port_read(uint16_t port, int width);
void     xbox_io_port_write(uint16_t port, int width, uint32_t value);
#define XBOX_IO_READ8(port)         ((uint8_t)xbox_io_port_read((uint16_t)(port), 1))
#define XBOX_IO_READ16(port)        ((uint16_t)xbox_io_port_read((uint16_t)(port), 2))
#define XBOX_IO_READ32(port)        (xbox_io_port_read((uint16_t)(port), 4))
#define XBOX_IO_WRITE8(port, val)   xbox_io_port_write((uint16_t)(port), 1, (uint8_t)(val))
#define XBOX_IO_WRITE16(port, val)  xbox_io_port_write((uint16_t)(port), 2, (uint16_t)(val))
#define XBOX_IO_WRITE32(port, val)  xbox_io_port_write((uint16_t)(port), 4, (uint32_t)(val))

/* ================================================================
 * Flag computation helpers
 *
 * These macros compute x86 flags for conditional branches.
 * Used by the lifter's pattern-matching output:
 *   cmp a, b; jcc target  ->  if (COND(a, b)) goto target;
 * ================================================================ */

/* Unsigned comparison conditions (from CMP a, b -> a - b) */
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b)  ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))   /* below (CF=1) */
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))   /* above or equal */
#define CMP_BE(a, b)  ((uint32_t)(a) <= (uint32_t)(b))   /* below or equal */
#define CMP_A(a, b)   ((uint32_t)(a) >  (uint32_t)(b))   /* above */

/*
 * Signed conditions have to be evaluated at the *operand's* width.
 *
 * `LO8(r)` and `HI8(r)` yield a uint8_t, so `test al, al ; js` arrived here as
 * TEST_S(LO8(edx), LO8(edx)) -- and the old definition widened that to 32 bits
 * before checking the sign, where a byte is always positive. The branch could
 * therefore never be taken. That is not a corner case: it is why the RefPack
 * decoder never terminated. Opcode 0xE3 is a literal run, recognised by bit 7,
 * and with the sign test dead every such opcode fell into the short-form
 * back-reference branch instead. The decoder then ran for billions of
 * iterations, writing ~140 MB into a 512 KB buffer and off the end of guest
 * RAM, where the RAM mirror folded it back onto .text.
 *
 * 49 TEST_S sites and 83 signed compares use byte operands.
 *
 * sizeof does not apply integer promotion, so it reads the width the lifter
 * already encoded at the call site: 1 for LO8/HI8, 4 for a whole register.
 */
#define RECOMP_SEXT(v)                                       (sizeof(v) == 1u ? (int32_t)(int8_t)(uint8_t)(v)        : sizeof(v) == 2u ? (int32_t)(int16_t)(uint16_t)(v)                        : (int32_t)(uint32_t)(v))

#define RECOMP_SIGNBIT(v)                                    (sizeof(v) == 1u ? 0x80u                                : sizeof(v) == 2u ? 0x8000u : 0x80000000u)

/* Signed comparison conditions */
#define CMP_L(a, b)   (RECOMP_SEXT(a) <  RECOMP_SEXT(b))  /* less (SF!=OF) */
#define CMP_GE(a, b)  (RECOMP_SEXT(a) >= RECOMP_SEXT(b))  /* greater or equal */
#define CMP_LE(a, b)  (RECOMP_SEXT(a) <= RECOMP_SEXT(b))  /* less or equal */
#define CMP_G(a, b)   (RECOMP_SEXT(a) >  RECOMP_SEXT(b))  /* greater */

/* TEST-based conditions (AND without storing result) */
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)  /* ZF=1 */
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0)  /* ZF=0 */
#define TEST_S(a, b)  ((((uint32_t)(a) & (uint32_t)(b))                                         & RECOMP_SIGNBIT(a)) != 0u)               /* SF=1 */

/* ================================================================
 * Arithmetic with carry/overflow detection
 * ================================================================ */

/** Add with carry flag. Returns result, sets *cf. */
static inline uint32_t ADD32_CF(uint32_t a, uint32_t b, int *cf) {
    uint32_t r = a + b;
    *cf = (r < a);
    return r;
}

/** Sub with carry (borrow) flag. Returns result, sets *cf. */
static inline uint32_t SUB32_CF(uint32_t a, uint32_t b, int *cf) {
    *cf = (a < b);
    return a - b;
}

/* ================================================================
 * Rotation / shift helpers
 * ================================================================ */

static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31;
    return (val << n) | (val >> (32 - n));
}

static inline uint32_t ROR32(uint32_t val, int n) {
    n &= 31;
    return (val >> n) | (val << (32 - n));
}

/* ================================================================
 * Sign/zero extension
 * ================================================================ */

#define ZX8(v)   ((uint32_t)(uint8_t)(v))
#define ZX16(v)  ((uint32_t)(uint16_t)(v))
#define SX8(v)   ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)  ((uint32_t)(int32_t)(int16_t)(v))

/* ================================================================
 * Byte/word register access
 *
 * These macros extract or set partial registers, matching x86
 * behavior where writing AL doesn't affect bits 8-31 of EAX.
 * ================================================================ */

/** Extract low byte (al, bl, cl, dl). */
#define LO8(r)  ((uint8_t)((r) & 0xFF))
/** Extract high byte of low word (ah, bh, ch, dh). */
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
/** Extract low word (ax, bx, cx, dx). */
#define LO16(r) ((uint16_t)((r) & 0xFFFF))

/** Set low byte, preserving upper 24 bits. */
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
/** Set high byte of low word, preserving other bits. */
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
/** Set low word, preserving upper 16 bits. */
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))

/* ================================================================
 * Stack simulation
 *
 * For push/pop heavy prologues in the generated code.
 * ================================================================ */

/**
 * Push a 32-bit value onto the simulated stack.
 * Evaluates val BEFORE decrementing sp, matching x86 semantics
 * where push [esp+N] reads the operand before adjusting ESP.
 */
#define PUSH32(sp, val) do { \
    uint32_t _pv = (uint32_t)(val); \
    (sp) -= 4; \
    MEM32(sp) = _pv; \
} while(0)

/** Pop a 32-bit value from the simulated stack. */
#define POP32(sp, dst) do { \
    (dst) = MEM32(sp); \
    (sp) += 4; \
} while(0)

/* ================================================================
 * Byte swap (for endian conversion if needed)
 *
 * Xbox is little-endian like x86, so these are rarely needed,
 * but some games use bswap for network byte order or data parsing.
 * ================================================================ */

static inline uint32_t BSWAP32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

static inline uint16_t BSWAP16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* ================================================================
 * Indirect call dispatch
 *
 * The dispatch system resolves Xbox virtual addresses to native
 * function pointers at runtime. Three lookup sources are checked:
 *   1. Manual overrides (hand-written reimplementations)
 *   2. Generated dispatch table (auto-recompiled functions)
 *   3. Kernel thunk bridge (Xbox kernel function replacements)
 * ================================================================ */

/**
 * Generic function pointer type for all recompiled functions.
 * All translated functions are void(void) - arguments and return
 * values are passed through global registers and the simulated stack.
 */
#ifndef RECOMP_DISPATCH_H  /* avoid conflict with recomp_dispatch.h */
typedef void (*recomp_func_t)(void);

/**
 * Look up a recompiled function by its Xbox VA.
 * Returns NULL if the VA is not in the generated dispatch table.
 */
/* Sort the dispatch table so recomp_lookup's binary search is valid.
 * Must be called by the host before any guest code runs. Returns the number
 * of entries that were out of order, which is worth printing: a non-zero
 * count means that many functions had been silently unreachable. */
size_t recomp_dispatch_init(void);

recomp_func_t recomp_lookup(uint32_t xbox_va);

/**
 * Look up a kernel thunk function by its synthetic VA.
 * Kernel thunks live at 0xFE000000+ (synthetic addresses assigned
 * during kernel bridge initialization).
 * Returns NULL if the VA is not a kernel thunk.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/**
 * Look up a manually overridden function by its Xbox VA.
 * Manual overrides take priority over generated code.
 * Returns NULL if no manual override exists for this VA.
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
#endif

/**
 * RECOMP_ICALL - Indirect call through the dispatch table.
 *
 * Looks up the Xbox VA and calls the translated function.
 * Falls back to kernel bridge for kernel thunk synthetic VAs.
 * The caller must PUSH32 a dummy return address before this macro.
 * If not found, pops the dummy return address to keep the stack balanced.
 *
 * The range check (0x00400000 to 0xFE000000) skips garbage VAs that
 * come from uninitialized vtable pointers. Adjust this range based
 * on your game's .text section boundaries. Kernel thunks at
 * 0xFE000000+ must NOT be blocked.
 *
 * CUSTOMIZE: Change the VA range check to match your game's code range.
 * Your .text section typically spans 0x00010000 to ~0x003XXXXX.
 * Any VA outside .text and below 0xFE000000 is likely garbage.
 */
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    /* Skip garbage VAs outside code section + kernel thunk range */ \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        g_esp += 4; eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) _fn(); \
    else { g_esp += 4; eax = 0; } \
} while(0)

/**
 * RECOMP_ICALL_SAFE - Stack-safe indirect call.
 *
 * Restores g_esp to saved_esp (pre-argument value) on lookup failure,
 * preventing stdcall argument leaks on failed vtable calls.
 * Use this when the caller pushes arguments that the callee would
 * normally clean up (stdcall convention).
 */
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        g_esp = (saved_esp); eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) _fn(); \
    else { g_esp = (saved_esp); eax = 0; recomp_icall_miss_log_at(_va, __FILE__, __LINE__); } \
} while(0)

/**
 * RECOMP_ITAIL - Indirect tail call (jmp through function pointer).
 *
 * No return address is pushed - reuses the current frame's return addr.
 * Used for tail-call optimization where the original code uses
 * jmp [reg] instead of call [reg].
 *
 * Mirrors RECOMP_ICALL_SAFE's miss-log fallback (a genuinely missing
 * jump-table arm here silently fell through with
 * eax left holding whatever it was before the failed lookup -- easy to
 * mistake for a real return value, since nothing logged the miss at all.
 * That one instance (Application_StateMachineTick's own state-0 handler)
 * silently broke the entire game state machine for this whole project's
 * history. eax=0 on miss matches RECOMP_ICALL_SAFE's own "missing function
 * returns 0" convention, so callers that do check a return value get a
 * consistent, visible-in-the-log failure instead of stale register noise.
 */
#define RECOMP_ITAIL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) _fn(); \
    else { eax = 0; recomp_icall_miss_log_at(_va, __FILE__, __LINE__); } \
} while(0)

/* ================================================================
 * Register name aliases for generated code
 *
 * Map x86 volatile register names to global variables.
 * These #defines allow the generated code to use natural register
 * names (eax, ecx, edx, esp) which the preprocessor maps to the
 * corresponding globals (g_eax, g_ecx, g_edx, g_esp).
 *
 * Only active when RECOMP_GENERATED_CODE is defined (in generated
 * .c files) to avoid polluting hand-written code.
 * ================================================================ */

#ifdef RECOMP_GENERATED_CODE
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
/* ebp is NOT global - it's local in each function.
 * For __SEH_prolog/epilog, use g_seh_ebp to bridge. */
/* The x87 register file is CPU state: one per thread, surviving calls, tail
 * calls and the lifter's function splits. Every function used to declare its
 * own `double _fp_stack[8]; int _fp_top = 0;`, so a value pushed before a split
 * was gone after it -- 104 fragments start by reading a stack their
 * predecessor filled and read zeros instead. The declarations are gone; these
 * point the fp_* macros at the per-thread pair (an earlier attempt
 * that seemed to crash was a player pressing A during the test run). */
#define _fp_stack g_fp_stack
#define _fp_top   g_fp_top
#endif


/* ================================================================
 * Forward declarations for translated functions
 *
 * These are generated by the recompiler and included per-file.
 * The recomp_funcs.h header (generated) declares all translated
 * function prototypes.
 * ================================================================ */

#endif /* RECOMP_TYPES_H */

/* ---------------------------------------------------------------------------
 * MMX packed integer helpers.
 *
 * The mm registers are plain uint64_t in generated code, so these are ordinary
 * integer work. They were emitted as `TODO` comments, which is silent data
 * loss: in the video's YUV-to-RGB row converter (sub_001493E0) the two dropped
 * `paddw`s meant the chroma was never added to the luma, and the dropped
 * `packuswb` meant the 16-bit table entries were stored without being packed
 * down to bytes. The result was a luminance-only image with the chroma bytes
 * left at zero -- a magenta EA logo.
 * ------------------------------------------------------------------------ */
static inline uint64_t mmx_paddb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++)
        r |= (uint64_t)(uint8_t)((uint8_t)(a >> (i*8)) + (uint8_t)(b >> (i*8))) << (i*8);
    return r;
}
static inline uint64_t mmx_paddw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++)
        r |= (uint64_t)(uint16_t)((uint16_t)(a >> (i*16)) + (uint16_t)(b >> (i*16))) << (i*16);
    return r;
}
static inline uint64_t mmx_paddd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; i++)
        r |= (uint64_t)(uint32_t)((uint32_t)(a >> (i*32)) + (uint32_t)(b >> (i*32))) << (i*32);
    return r;
}
static inline uint64_t mmx_psubb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; i++)
        r |= (uint64_t)(uint8_t)((uint8_t)(a >> (i*8)) - (uint8_t)(b >> (i*8))) << (i*8);
    return r;
}
static inline uint64_t mmx_psubw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++)
        r |= (uint64_t)(uint16_t)((uint16_t)(a >> (i*16)) - (uint16_t)(b >> (i*16))) << (i*16);
    return r;
}
static inline uint64_t mmx_psubd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; i++)
        r |= (uint64_t)(uint32_t)((uint32_t)(a >> (i*32)) - (uint32_t)(b >> (i*32))) << (i*32);
    return r;
}
static inline uint8_t mmx_satub(int32_t v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}
/* packuswb: 4 signed words from `a` become the low 4 bytes, 4 from `b` the
 * high 4, each saturated to unsigned byte range. */
static inline uint64_t mmx_packuswb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++)
        r |= (uint64_t)mmx_satub((int16_t)(a >> (i*16))) << (i*8);
    for (i = 0; i < 4; i++)
        r |= (uint64_t)mmx_satub((int16_t)(b >> (i*16))) << ((i+4)*8);
    return r;
}
static inline int8_t mmx_satsb(int32_t v)
{
    return (int8_t)(v < -128 ? -128 : (v > 127 ? 127 : v));
}
static inline uint64_t mmx_packsswb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; i++)
        r |= (uint64_t)(uint8_t)mmx_satsb((int16_t)(a >> (i*16))) << (i*8);
    for (i = 0; i < 4; i++)
        r |= (uint64_t)(uint8_t)mmx_satsb((int16_t)(b >> (i*16))) << ((i+4)*8);
    return r;
}
static inline int16_t mmx_satsw(int64_t v)
{
    return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}
static inline uint64_t mmx_packssdw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; i++)
        r |= (uint64_t)(uint16_t)mmx_satsw((int32_t)(a >> (i*32))) << (i*16);
    for (i = 0; i < 2; i++)
        r |= (uint64_t)(uint16_t)mmx_satsw((int32_t)(b >> (i*32))) << ((i+2)*16);
    return r;
}

#include "recomp_mmx.h"
