/*
 * Fork link shim (versioned in port/tools/fork/, copied into gen/ by fork_regen.sh).
 *
 * port/tools/regen.sh output lacks three symbols that the hand-maintained
 * upstream gen/ tree defines (see docs/building.md, "Regeneration is not yet
 * complete"):
 *   - g_last_loc / g_main_loc: diagnostic location stamps read by the crash
 *     reporter and xbox_diag. A fresh regen emits no stamps, so they stay 0.
 *   - recomp_dispatch_init(): sorts the dispatch table. The regen table was
 *     checked to be already in ascending order (12581 entries, 0 unordered),
 *     so there is nothing to sort: return 0.
 */
#include <stddef.h>

volatile unsigned g_last_loc = 0;
volatile unsigned g_main_loc = 0;

size_t recomp_dispatch_init(void)
{
    return 0;
}
