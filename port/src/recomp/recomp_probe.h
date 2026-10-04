/*
 * recomp_probe.h -- runtime-armable probes at generated-code labels.
 *
 * WHY THIS EXISTS
 * ---------------
 * Instrumenting a label used to mean editing a generated .c file and
 * rebuilding: 52 s for one touched file, ~90 s for a full probe cycle, and the
 * site had to be guessed correctly *before* paying that cost.  Every label now
 * carries a RECOMP_LOC() check instead, so probes are armed at runtime -- from
 * the diag server or XBOX_PROBE -- with no rebuild at all.
 *
 * Cost when nothing is armed: one load of a global and a branch the predictor
 * always gets right.  g_probe_armed stays 0 unless a probe is actually set.
 *
 * LIMITATION: ebp is a local in every generated function, not a global, so the
 * evaluator maps `ebp` onto g_seh_ebp.  That is the SEH bridge value, which is
 * correct for functions that publish it and stale for those that do not -- for
 * an ebp-relative read, verify against the disassembly before trusting it.
 */
#ifndef RECOMP_PROBE_H
#define RECOMP_PROBE_H

#include <stdint.h>
#include <stddef.h>

extern volatile int g_probe_armed;

void recomp_probe_hit(uint32_t addr);

/* spec: one or more probes separated by ';'
 *   ADDR [ '|' WHEN ] [ '|' SHOW,SHOW,... ] [ '|' LIMIT ]
 * e.g.  0xAA22C|esi==0|esi,[esi+4],[[esi+4]+8]|3
 * EXPR := TERM (('+'|'-') TERM)* ;  TERM := reg | 0xhex | '[' EXPR ']'
 * Returns probes armed, or -1 on a parse error. */
int  recomp_probe_arm(const char *spec);
void recomp_probe_clear(void);
void recomp_probe_list(char *out, size_t cap);
void recomp_probe_init_from_env(void);

#define RECOMP_LOC(a) do { \
    if (__builtin_expect(g_probe_armed, 0)) recomp_probe_hit((uint32_t)(a)); \
} while (0)

#endif /* RECOMP_PROBE_H */
