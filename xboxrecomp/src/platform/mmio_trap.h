/*
 * mmio_trap.h -- memory-mapped device registers on hosts without VEH (macOS arm64).
 *
 * The recompiled code reads and writes device registers (the APU, the AC'97
 * codec, the DAC palette) with ordinary loads and stores to guest addresses.
 * On Windows a vectored exception handler decodes the faulting x86-64
 * instruction. Here the register page is made inaccessible, the access faults
 * (SIGBUS or SIGSEGV), and the handler decodes the faulting AArch64 load/store,
 * performs it against the device's read/write functions, writes the result
 * into the saved register state and resumes after the instruction.
 *
 * Only the access forms a C compiler emits for a volatile scalar are decoded:
 * LDR/STR (immediate, unsigned offset), LDUR/STUR, pre/post-indexed, register
 * offset, and the sign-extending loads, in 8/16/32/64-bit sizes. Anything else
 * touching a trapped page (pairs, SIMD, atomics, exclusives) is reported and
 * left to fault, never skipped silently.
 *
 * Host pages are 16 KB on Apple Silicon, so a device window is usually smaller
 * than the page that must be protected. Accesses to the rest of such a page are
 * served from a shadow copy taken when the trap was installed, so those bytes
 * keep behaving like plain memory.
 */
#ifndef OT_MMIO_TRAP_H
#define OT_MMIO_TRAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t (*ot_mmio_read_fn)(void *user, uint32_t offset, unsigned size);
typedef void     (*ot_mmio_write_fn)(void *user, uint32_t offset, uint64_t value, unsigned size);

/* Trap [native, native + size): accesses call read/write with the offset from
 * `native`. Installs the signal handlers on first use. Returns false if the
 * range cannot be protected. */
bool ot_mmio_trap(void *native, size_t size, const char *name,
                  ot_mmio_read_fn read, ot_mmio_write_fn write, void *user);

/* Accesses handled so far (diagnostics and tests). */
uint64_t ot_mmio_trap_count(void);

/* Test hook: the decoder alone. `insn` is one AArch64 instruction word.
 * Fills the decoded fields and returns true when it is a form this module
 * handles. */
typedef struct {
    unsigned size;        /* bytes: 1, 2, 4 or 8 */
    bool     is_store;
    unsigned rt;          /* 0..31 (31 = ZR) */
    unsigned rn;          /* base register, 0..31 (31 = SP) */
    bool     sign_extend; /* LDRS* */
    bool     dest64;      /* result written to the full X register */
    int      writeback;   /* 0 none, 1 pre-index, 2 post-index */
    int64_t  imm;         /* index offset when writeback != 0 */
} ot_a64_access;
bool ot_a64_decode(uint32_t insn, ot_a64_access *out);

#ifdef __cplusplus
}
#endif
#endif
