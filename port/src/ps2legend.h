/* ps2legend -- the loading screens' controller legend in the PS2 control
 * scheme (see ps2legend.c). */
#ifndef PS2LEGEND_H
#define PS2LEGEND_H

/* Hooks the language files: the PS2 loading legend (ctlscheme_ps2) and the
 * PC words for the console's own (XBOX_FIX_PC_TEXT). Call after ctlscheme_init. */
void ps2legend_init(void);

/* recomp_lookup_manual: the Basic Controls splash hook (always), the
 * loading legend (Y in the Pro preset, PS2 layout only), the
 * save manager (hard disk check's wait, XBOX_FIX_SKIP_HDD_CHECK, and "quit
 * game", XBOX_FIX_QUIT_GAME; both default on) and the Save / Load device
 * rows (memory units hidden, XBOX_FIX_HIDE_MU, default on). */
void (*ps2legend_lookup(unsigned int xbox_va))(void);

#endif
