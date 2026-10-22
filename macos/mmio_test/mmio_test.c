/* MMIO fault-handler test (MACOS_PORT_PLAN.md 0.4). Built at -O0, -O1 and -O3 so the
 * compiler picks different AArch64 addressing forms for the same C.
 *
 * A 32 KB mapping holds a 4 KB "device" at +0x5000 inside the 16 KB page
 * [0x4000, 0x8000), which is trapped. The device is 4 KB of registers whose reads
 * and writes are logged. Accesses in the same page but outside the window must
 * behave like plain memory and keep their pre-trap content (the shadow copy). */
#include "platform/mmio_trap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>

#define PAGE   0x4000u
#define WIN_OFF 0x5000u
#define WIN_LEN 0x1000u

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the device: a register file, plus the last access it saw */
/* volatile: the device callbacks run from the signal handler, which the optimiser cannot see */
static volatile uint8_t  g_regs[WIN_LEN];
static volatile uint32_t g_last_off; static volatile unsigned g_last_size; static volatile int g_last_write;
static volatile int g_reads, g_writes;

static uint64_t peek(uint32_t off, unsigned size) { uint64_t v = 0; for (unsigned i = 0; i < size; i++) v |= (uint64_t)g_regs[off + i] << (8 * i); return v; }
static void     poke(uint32_t off, uint64_t v, unsigned size) { for (unsigned i = 0; i < size; i++) g_regs[off + i] = (uint8_t)(v >> (8 * i)); }

static uint64_t dev_read(void *u, uint32_t off, unsigned size)
{
    (void)u; uint64_t v = peek(off, size);
    g_last_off = off; g_last_size = size; g_last_write = 0; g_reads++;
    return v;
}
static void dev_write(void *u, uint32_t off, uint64_t v, unsigned size)
{
    (void)u; poke(off, v, size);
    g_last_off = off; g_last_size = size; g_last_write = 1; g_writes++;
}

/* Variable offsets reach the compiler as uint32_t indexes, like the translated code's
 * MEM32(va): that is the register-offset (uxtw) form. noinline so -O3 keeps real accesses. */
#define NI __attribute__((noinline))
static NI uint8_t  ld8 (volatile uint8_t  *b, uint32_t i) { return *(volatile uint8_t  *)(b + i); }
static NI uint16_t ld16(volatile uint8_t  *b, uint32_t i) { return *(volatile uint16_t *)(b + i); }
static NI uint32_t ld32(volatile uint8_t  *b, uint32_t i) { return *(volatile uint32_t *)(b + i); }
static NI uint64_t ld64(volatile uint8_t  *b, uint32_t i) { return *(volatile uint64_t *)(b + i); }
static NI void st8 (volatile uint8_t *b, uint32_t i, uint8_t  v) { *(volatile uint8_t  *)(b + i) = v; }
static NI void st16(volatile uint8_t *b, uint32_t i, uint16_t v) { *(volatile uint16_t *)(b + i) = v; }
static NI void st32(volatile uint8_t *b, uint32_t i, uint32_t v) { *(volatile uint32_t *)(b + i) = v; }
static NI void st64(volatile uint8_t *b, uint32_t i, uint64_t v) { *(volatile uint64_t *)(b + i) = v; }
static NI int64_t lds8 (volatile uint8_t *b, uint32_t i) { return *(volatile int8_t  *)(b + i); }
static NI int64_t lds16(volatile uint8_t *b, uint32_t i) { return *(volatile int16_t *)(b + i); }
static NI int64_t lds32(volatile uint8_t *b, uint32_t i) { return *(volatile int32_t *)(b + i); }
static NI int32_t lds8w(volatile uint8_t *b, uint32_t i) { return *(volatile int8_t  *)(b + i); }
static NI void    st32z(volatile uint8_t *b, uint32_t i)  { *(volatile uint32_t *)(b + i) = 0; }   /* wzr */
static NI void    st64z(volatile uint8_t *b, uint32_t i)  { *(volatile uint64_t *)(b + i) = 0; }   /* xzr */
/* constant offsets from a pointer: unsigned-immediate and, for negative ones, LDUR/STUR */
static NI uint32_t ldc_pos(volatile uint8_t *p) { return *(volatile uint32_t *)(p + 0x10); }
static NI uint32_t ldc_neg(volatile uint8_t *p) { return *(volatile uint32_t *)(p - 0x10); }
static NI void     stc_neg(volatile uint8_t *p, uint32_t v) { *(volatile uint32_t *)(p - 8) = v; }
/* a walk over consecutive registers: -O1 and up turn this into post-indexed loads */
static NI uint32_t sum_words(volatile uint32_t *p, int n) { uint32_t s = 0; while (n--) s += *p++; return s; }
static NI void     fill_words(volatile uint32_t *p, int n, uint32_t v) { while (n--) { *p++ = v; v += 0x11; } }

static void test_in_process(volatile uint8_t *m)
{
    volatile uint8_t *dev = m + WIN_OFF;
    uint64_t base_count = ot_mmio_trap_count();

    /* scalar loads: the device holds a known pattern */
    for (unsigned i = 0; i < WIN_LEN; i++) g_regs[i] = (uint8_t)(i * 7 + 3);
    CHECK(ld8 (dev, 0x21) == g_regs[0x21], "ld8");
    CHECK(g_last_off == 0x21 && g_last_size == 1 && !g_last_write, "ld8 reached device as (0x21,1)");
    uint16_t e16 = (uint16_t)peek(0x30, 2);
    CHECK(ld16(dev, 0x30) == e16 && g_last_size == 2, "ld16");
    uint32_t e32 = (uint32_t)peek(0x40, 4);
    CHECK(ld32(dev, 0x40) == e32 && g_last_size == 4, "ld32");
    uint64_t e64 = peek(0x48, 8);
    CHECK(ld64(dev, 0x48) == e64 && g_last_size == 8, "ld64");

    /* stores */
    st8 (dev, 0x100, 0xAB);                  CHECK(g_regs[0x100] == 0xAB && g_last_write && g_last_size == 1 && g_last_off == 0x100, "st8");
    st16(dev, 0x102, 0xBEEF);                CHECK(g_regs[0x102] == 0xEF && g_regs[0x103] == 0xBE && g_last_size == 2, "st16");
    st32(dev, 0x104, 0xDEADC0DEu);           CHECK(ld32(dev, 0x104) == 0xDEADC0DEu && g_last_size == 4, "st32 then ld32");
    st64(dev, 0x108, 0x0123456789ABCDEFull); CHECK(ld64(dev, 0x108) == 0x0123456789ABCDEFull, "st64 then ld64");

    /* sign-extending loads, to X and to W; the register must hold the extended value */
    g_regs[0x200] = 0x80; g_regs[0x201] = 0x00;
    CHECK(lds8 (dev, 0x200) == -128, "ldrsb x");
    CHECK(lds8w(dev, 0x200) == -128, "ldrsb w");
    g_regs[0x210] = 0x00; g_regs[0x211] = 0x80;
    CHECK(lds16(dev, 0x210) == -32768, "ldrsh");
    g_regs[0x220] = 0; g_regs[0x221] = 0; g_regs[0x222] = 0; g_regs[0x223] = 0x80;
    CHECK(lds32(dev, 0x220) == (int64_t)(int32_t)0x80000000, "ldrsw");
    CHECK(ld32(dev, 0x220) == 0x80000000u, "plain ld32 is zero-extended");

    /* storing zero: Rt == 31 is WZR/XZR, not SP */
    st64(dev, 0x300, ~0ull); st32z(dev, 0x300);
    CHECK(ld32(dev, 0x300) == 0 && ld32(dev, 0x304) == 0xFFFFFFFFu, "wzr store");
    st64z(dev, 0x300);
    CHECK(ld64(dev, 0x300) == 0, "xzr store");

    /* constant offsets, positive and negative (unsigned-immediate and unscaled forms) */
    poke(0x410, 0x11112222u, 4);
    poke(0x3F0, 0x33334444u, 4);
    CHECK(ldc_pos(dev + 0x400) == 0x11112222u && g_last_off == 0x410, "ldr imm");
    CHECK(ldc_neg(dev + 0x400) == 0x33334444u && g_last_off == 0x3F0, "ldur negative");
    stc_neg(dev + 0x500, 0x55556666u);
    uint32_t got = (uint32_t)peek(0x4F8, 4);
    CHECK(got == 0x55556666u && g_last_off == 0x4F8 && g_last_write, "stur negative");

    /* consecutive walks: a handler that mishandles writeback gets the sum wrong */
    fill_words((volatile uint32_t *)(dev + 0x600), 8, 0x100);
    uint32_t want = 0; for (int i = 0; i < 8; i++) want += 0x100 + 0x11u * (unsigned)i;
    CHECK(sum_words((volatile uint32_t *)(dev + 0x600), 8) == want, "post-indexed walk");

    /* writeback forms the compiler rarely emits for a volatile scalar: written by hand so
     * the base-register update is checked too (post-index load, pre-index load and store) */
    {
        uint32_t v; volatile uint8_t *p = dev + 0x700;
        poke(0x700, 0xA0A1A2A3u, 4); poke(0x70C, 0xB0B1B2B3u, 4);
        __asm__ volatile("ldr %w0, [%1], #4" : "=r"(v), "+r"(p) :: "memory");
        CHECK(v == 0xA0A1A2A3u && p == dev + 0x704 && g_last_off == 0x700, "post-index load updates the base");
        __asm__ volatile("ldr %w0, [%1, #8]!" : "=r"(v), "+r"(p) :: "memory");
        CHECK(v == 0xB0B1B2B3u && p == dev + 0x70C && g_last_off == 0x70C, "pre-index load updates the base");
        uint32_t sv = 0xC0C1C2C3u;
        __asm__ volatile("str %w1, [%0, #-4]!" : "+r"(p) : "r"(sv) : "memory");
        CHECK(p == dev + 0x708 && peek(0x708, 4) == 0xC0C1C2C3u && g_last_write, "pre-index store updates the base");
        /* a load into an X register must overwrite all 64 bits, a W load must clear the upper half */
        uint64_t x = ~0ull;
        __asm__ volatile("ldr %w0, [%1]" : "+r"(x) : "r"(dev + 0x700) : "memory");
        CHECK(x == 0xA0A1A2A3u, "W load zero-extends into the X register");
    }

    /* the same page outside the device window is plain memory with its old content */
    CHECK(ld32(m, 0x4000) == 0xCAFE0000u, "shadow keeps the pre-trap content (page start)");
    CHECK(ld32(m, WIN_OFF + WIN_LEN) == 0xCAFE1000u, "shadow keeps the pre-trap content (after the window)");
    int reads_before = g_reads, writes_before = g_writes;
    st32(m, 0x4010, 0x600DF00Du);
    CHECK(ld32(m, 0x4010) == 0x600DF00Du, "shadow write then read");
    st8(m, 0x7FFF, 0x5A); CHECK(ld8(m, 0x7FFF) == 0x5A, "shadow byte at the end of the page");
    CHECK(g_reads == reads_before && g_writes == writes_before, "shadow accesses never reach the device");

    CHECK(ot_mmio_trap_count() - base_count > 40, "handler ran %llu times", (unsigned long long)(ot_mmio_trap_count() - base_count));
}

/* An access the decoder does not handle (LDP) must kill the process, not be skipped. */
static void test_unsupported_dies(volatile uint8_t *m)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        uint64_t a, b;
        __asm__ volatile("ldp %0, %1, [%2]" : "=r"(a), "=r"(b) : "r"(m + WIN_OFF) : "memory");
        _exit(0);   /* reached only if the access was silently skipped */
    }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && (WTERMSIG(st) == SIGBUS || WTERMSIG(st) == SIGSEGV),
          "ldp on a trapped page must fault fatally (status 0x%x)", st);
}

static void test_decoder(void)
{
    ot_a64_access a;
    /* encodings produced by the assembler: ldr w0,[x1,#4]  str x2,[x3]  ldrsb x4,[x5,x6]  ldr w0,[x1],#4  ldr w0,[x1,#4]! */
    CHECK(ot_a64_decode(0xB9400420u, &a) && a.size == 4 && !a.is_store && a.rt == 0 && a.rn == 1 && !a.dest64, "ldr w0,[x1,#4]");
    CHECK(ot_a64_decode(0xF9000062u, &a) && a.size == 8 && a.is_store && a.rt == 2 && a.rn == 3, "str x2,[x3]");
    CHECK(ot_a64_decode(0x38A668A4u, &a) && a.size == 1 && a.sign_extend && a.dest64 && a.rt == 4, "ldrsb x4,[x5,x6]");
    CHECK(ot_a64_decode(0xB8404420u, &a) && a.writeback == 2 && a.imm == 4, "ldr w0,[x1],#4");
    CHECK(ot_a64_decode(0xB8404C20u, &a) && a.writeback == 1 && a.imm == 4, "ldr w0,[x1,#4]!");
    CHECK(ot_a64_decode(0xB85FCC20u, &a) && a.writeback == 1 && a.imm == -4, "ldr w0,[x1,#-4]!");
    CHECK(!ot_a64_decode(0xA9400C20u, &a), "ldp rejected");
    CHECK(!ot_a64_decode(0xBD400020u, &a), "ldr s0 (SIMD) rejected");
    CHECK(!ot_a64_decode(0xC8DFFC20u, &a), "ldar rejected");
}

int main(void)
{
    test_decoder();

    uint8_t *m = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    /* known content around the window, taken into the shadow when the trap goes in */
    *(uint32_t *)(m + 0x4000) = 0xCAFE0000u;
    *(uint32_t *)(m + WIN_OFF + WIN_LEN) = 0xCAFE1000u;
    CHECK(ot_mmio_trap(m + WIN_OFF, WIN_LEN, "test", dev_read, dev_write, NULL), "ot_mmio_trap");

    test_in_process(m);
    test_unsupported_dies(m);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
