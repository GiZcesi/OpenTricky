/* nv2a_par.c -- see nv2a_par.h */
#include "nv2a_par.h"
#include <stdio.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <pthread.h>
#include <stdatomic.h>

typedef struct { void (*fn)(void *, uint32_t, uint32_t); void *ctx; uint32_t n, chunk; } ParJob;

static struct {
    int nworkers, state;                 /* state: 0 = not started, 1 = running, -1 = unavailable */
    pthread_mutex_t m;
    pthread_cond_t cv;
    ParJob job;
    atomic_uint gen, next;
    atomic_int done, sleeping;
} P = { .m = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static inline void relax(void)
{
#if defined(__aarch64__)
    __asm__ volatile("yield");
#else
    __asm__ volatile("pause");
#endif
}

static void run_chunks(void)
{
    ParJob j = P.job;
    for (;;) {
        uint32_t lo = atomic_fetch_add(&P.next, j.chunk), hi;
        if (lo >= j.n) break;
        hi = lo + j.chunk; if (hi > j.n) hi = j.n;
        j.fn(j.ctx, lo, hi);
    }
}

static void *worker(void *arg)
{
    unsigned last = 0;
    (void)arg;
    for (;;) {
        unsigned g; int spins = 0;
        while ((g = atomic_load(&P.gen)) == last) {
            if (++spins < 30000) { relax(); continue; }          /* a few hundred microseconds, then sleep */
            pthread_mutex_lock(&P.m);
            atomic_fetch_add(&P.sleeping, 1);
            while (atomic_load(&P.gen) == last) pthread_cond_wait(&P.cv, &P.m);
            atomic_fetch_sub(&P.sleeping, 1);
            pthread_mutex_unlock(&P.m);
            spins = 0;
        }
        last = g;
        run_chunks();
        atomic_fetch_add(&P.done, 1);
    }
    return NULL;
}

static int par_init(void)
{
    if (P.state) return P.state > 0;
    {
        const char *e = getenv("OT_VSH_THREADS");
        int n = (e && e[0]) ? atoi(e) : 4, i;
        if (n > 16) n = 16;
        P.state = -1;
        if (n <= 0) return 0;
        for (i = 0; i < n; i++) {
            pthread_t t;
            if (pthread_create(&t, NULL, worker, NULL) != 0) break;
            pthread_detach(t);
            P.nworkers++;
        }
        if (P.nworkers != n) {              /* a worker that did not start would never count in `done`: give up */
            fprintf(stderr, "[PAR] could not start %d vertex workers (%d started): running on one thread\n", n, P.nworkers);
            if (P.nworkers) { /* the started ones keep spinning down to sleep; nothing will wake them */ }
            P.nworkers = 0;
            return 0;
        }
        P.state = 1;
        fprintf(stderr, "[PAR] %d vertex-program worker threads\n", n);
    }
    return 1;
}

void nv2a_par_for(uint32_t n, uint32_t min_chunk, void (*fn)(void *, uint32_t, uint32_t), void *ctx)
{
    uint32_t chunk;
    if (min_chunk == 0) min_chunk = 1;
    if (n < 2u * min_chunk || !par_init()) { fn(ctx, 0, n); return; }
    chunk = n / (uint32_t)((P.nworkers + 1) * 4);
    if (chunk < min_chunk) chunk = min_chunk;
    P.job.fn = fn; P.job.ctx = ctx; P.job.n = n; P.job.chunk = chunk;
    atomic_store(&P.next, 0);
    atomic_store(&P.done, 0);
    atomic_fetch_add(&P.gen, 1);
    if (atomic_load(&P.sleeping)) {
        pthread_mutex_lock(&P.m);
        pthread_cond_broadcast(&P.cv);
        pthread_mutex_unlock(&P.m);
    }
    run_chunks();
    while (atomic_load(&P.done) < P.nworkers) relax();
}

#else

void nv2a_par_for(uint32_t n, uint32_t min_chunk, void (*fn)(void *, uint32_t, uint32_t), void *ctx)
{
    (void)min_chunk;
    fn(ctx, 0, n);
}

#endif
