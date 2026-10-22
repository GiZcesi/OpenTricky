/* intrin.h -- non-Windows shim for the few MSVC intrinsics the runtime uses. */
#ifndef OT_SHIM_INTRIN_H
#define OT_SHIM_INTRIN_H
#include <stdint.h>

#if defined(__aarch64__)
/* __rdtsc: x86 callers use it as a monotonic tick counter. ARM's virtual
 * counter (cntvct_el0) plays that role; it ticks at cntfrq_el0 (24 MHz on
 * Apple Silicon), NOT at the CPU clock, so callers that convert ticks to
 * time must use ot_tsc_hz() instead of assuming a clock speed. */
static inline uint64_t __rdtsc(void)
{
    uint64_t v;
    __asm__ __volatile__("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
static inline uint64_t ot_tsc_hz(void)
{
    uint64_t f;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
static inline uint64_t ot_tsc_hz(void) { return 0; /* calibrate at runtime */ }
#endif

static inline unsigned char _BitScanForward(uint32_t *idx, uint32_t mask)
{
    if (!mask) return 0;
    *idx = (uint32_t)__builtin_ctz(mask);
    return 1;
}
static inline unsigned char _BitScanReverse(uint32_t *idx, uint32_t mask)
{
    if (!mask) return 0;
    *idx = 31u - (uint32_t)__builtin_clz(mask);
    return 1;
}
#define _ReadWriteBarrier() __asm__ __volatile__("" ::: "memory")
#ifndef __debugbreak
#define __debugbreak()      __builtin_trap()
#endif
#endif
