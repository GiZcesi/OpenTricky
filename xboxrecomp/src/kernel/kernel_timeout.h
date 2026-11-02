/*
 * kernel_timeout.h -- NT timeouts to Win32 waits, without truncation.
 *
 * An NT timeout is a 64-bit count of 100 ns: negative for a relative
 * interval, positive for an absolute system time. A Win32 wait takes a 32-bit
 * count of milliseconds in which 0xFFFFFFFF (INFINITE) means forever, so the
 * longest finite Win32 wait is 0xFFFFFFFE ms, about 49.7 days.
 *
 * Longer timeouts used to be cast to 32 bits, which keeps only the low bits
 * of the count: 2^32 ms + 5 ms waited 5 ms, a timeout of 0xFFFFFFFF ms never
 * ended, and Sleep(INFINITE) -- which XAPI passes on as the longest relative
 * interval, 0x8000000000000000 -- slept for a few weeks. Negating that
 * interval as a signed 64-bit value was undefined behaviour besides.
 *
 * The waits now take a timeout in slices: nt_wait_slice_ms() returns the next
 * Win32 wait and takes it off what is left. A timeout that fits in one Win32
 * wait is a single slice, the same count of milliseconds as before.
 */
#ifndef XBOXRECOMP_KERNEL_TIMEOUT_H
#define XBOXRECOMP_KERNEL_TIMEOUT_H

#include <stdint.h>

/* Longest finite Win32 wait, in milliseconds (0xFFFFFFFF is INFINITE). */
#define NT_WAIT_SLICE_MAX_MS 0xFFFFFFFEu

/* Length of a relative NT interval (a negative count of 100 ns), negated
 * without overflow: -INT64_MIN does not fit an int64_t. */
static inline uint64_t nt_relative_100ns(int64_t interval)
{
    return (uint64_t)0 - (uint64_t)interval;
}

/* The next Win32 wait, in milliseconds, of a wait with *left_ms still to go;
 * takes it off *left_ms. */
static inline uint32_t nt_wait_slice_ms(uint64_t *left_ms)
{
    uint64_t ms = *left_ms < NT_WAIT_SLICE_MAX_MS ? *left_ms : NT_WAIT_SLICE_MAX_MS;
    *left_ms -= ms;
    return (uint32_t)ms;
}

#endif /* XBOXRECOMP_KERNEL_TIMEOUT_H */
