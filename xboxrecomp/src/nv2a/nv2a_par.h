/* nv2a_par -- run a loop over [0, n) on several cores (macOS builds; elsewhere it runs on the calling thread).
 * Used for the CPU vertex programs: draws are usually small, so the workers spin briefly between jobs instead of
 * sleeping, which keeps the cost of a dispatch to a few microseconds.
 *   OT_VSH_THREADS=<n>   worker threads (default 4, 0 = everything on the calling thread) */
#ifndef NV2A_PAR_H
#define NV2A_PAR_H
#include <stdint.h>

/* Calls fn(ctx, lo, hi) over disjoint ranges that together cover [0, n). Returns when all are done.
 * Ranges are at least min_chunk long (a smaller n runs as one call on the calling thread). fn must be safe to run
 * concurrently on different ranges. Not reentrant; one caller at a time. */
void nv2a_par_for(uint32_t n, uint32_t min_chunk, void (*fn)(void *ctx, uint32_t lo, uint32_t hi), void *ctx);

#endif
