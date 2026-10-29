/*
 * host_clock.h -- host performance-counter ticks to time units, without
 * 64-bit overflow.
 *
 * The obvious count * unit / freq overflows a signed 64-bit product as soon
 * as count exceeds INT64_MAX / unit: with the usual 10 MHz performance
 * counter that is 922 s of host uptime for nanoseconds and ~10.7 days for
 * microseconds. The overflow is undefined behaviour; in practice the product
 * wraps and the clock jumps back by 2^64 / freq units every ~30.7 minutes.
 *
 * Splitting the count into whole seconds and a remainder gives the exact
 * same result as the wide product (floor(count * unit / freq)) for any
 * count >= 0, as long as freq * unit fits in 63 bits (true for any real
 * performance-counter frequency and unit <= 1e9).
 */
#ifndef XBOXRECOMP_HOST_CLOCK_H
#define XBOXRECOMP_HOST_CLOCK_H

#include <stdint.h>

static inline int64_t host_ticks_to_units(int64_t count, int64_t freq,
                                          int64_t unit)
{
    return (count / freq) * unit + (count % freq) * unit / freq;
}

#endif /* XBOXRECOMP_HOST_CLOCK_H */
