/*
 * d3d8_pumpflags.h -- the pump's switches, as they stand for the frame
 * being drawn.
 *
 * Each pump optimization has its switch (XBOX_FIX_PUMP_*, read once from
 * the environment) and is off on the XBOX_FIX_PUMP_ALT frames. Asking each
 * switch on every draw, lookup and state call cost a chain of calls and
 * tests, about 2 % of the pump. The answers only change when the presented
 * frame does (the ALT parity), so d3d8_nv2a.c works them all out once per
 * frame (d3d8_pump_frame_tick) into one word; the switches read a bit of it.
 * The word starts stale and is worked out at its first use.
 */
#ifndef D3D8_PUMPFLAGS_H
#define D3D8_PUMPFLAGS_H

#define PF_ALT_OFF  (1u << 0)   /* XBOX_FIX_PUMP_ALT: the optimizations are off on this frame */
#define PF_STATE    (1u << 1)   /* XBOX_FIX_PUMP_STATE */
#define PF_CB       (1u << 2)   /* XBOX_FIX_PUMP_CB */
#define PF_ONEMAP   (1u << 3)   /* XBOX_FIX_PUMP_ONEMAP (and its own _ALT) */
#define PF_CGEN     (1u << 4)   /* XBOX_FIX_PUMP_CGEN */
#define PF_CACHE    (1u << 5)   /* XBOX_FIX_PUMP_CACHE */
#define PF_MEMO2    (1u << 6)   /* XBOX_FIX_PUMP_MEMO2 */
#define PF_GROUP    (1u << 7)   /* XBOX_FIX_PUMP_GROUP */
#define PF_TEXMEMO  (1u << 8)   /* XBOX_FIX_PUMP_TEXMEMO, with the signature cache */
#define PF_SRV      (1u << 9)   /* XBOX_FIX_PUMP_SRV, with the state mirror's skipping */
#define PF_SOBJ     (1u << 10)  /* XBOX_FIX_PUMP_SOBJ */
#define PF_SMEMO    (1u << 11)  /* XBOX_FIX_PUMP_SMEMO */
#define PF_RSKIP    (1u << 12)  /* XBOX_FIX_PUMP_RSKIP */
#define PF_IDX      (1u << 13)  /* XBOX_FIX_PUMP_IDX */
#define PF_ARGB     (1u << 14)  /* XBOX_FIX_PUMP_ARGB */
#define PF_STALE    (1u << 31)  /* not worked out yet */

extern unsigned g_pump_flags;
unsigned d3d8_pump_flags_refresh(void);

static inline unsigned d3d8_pump_flags(void)
{
    unsigned f = g_pump_flags;
    return (f & PF_STALE) ? d3d8_pump_flags_refresh() : f;
}

#endif
