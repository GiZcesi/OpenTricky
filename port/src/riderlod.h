/*
 * riderlod -- riders always drawn with their most detailed model set
 * (XBOX_RIDER_LOD_MAX).
 *
 * Each frame, the rider draw (0x44FD0) asks 0x3DD70 (thiscall: ecx = rider)
 * for the rider's level of detail. It measures the rider's size on screen
 * (view depth, scaled by 0.05 -- 0.085 in split screen -- at [0x1AA4F8]) and
 * writes level 0, 1 or 2 to [rider+0x408] and [rider+0x40C] (thresholds
 * 26.02 / 45.18 at [0x188B0C] / [0x188B10]), then clamps both to the rider's
 * level count minus 2 ([rider + vt[1] + 0x450C]). With the race camera this
 * switches the player's own rider between levels 0 and 1 at about 5 m, and to
 * level 2 at about 9 m: a visible pop many times per race.
 *
 * [rider+0x408] is read only by the rider draw, which passes it to 0x109D80:
 * that one builds the bone matrices from model set 0 ([rider + vt[1] + 0x7CC + 0x3D3C])
 * on every call, whatever the level, then draws set [..+0x3D3C] + level *
 * 0x5900. Set 0 is therefore always loaded and read; choosing it draws the
 * same number of parts with more vertices. [rider+0x40C] is written next to
 * it and read by nothing else in the title.
 *
 * XBOX_RIDER_LOD_MAX=1 (default): right after the pick, a level above 0 is
 * brought down to 0, in both fields -- the state the title itself sets when
 * the rider is close to the camera. Drawing only: nothing in the simulation
 * reads these fields. XBOX_RIDER_LOD_MAX=0: the original pick.
 * XBOX_RIDER_LOD_LOG=1 (diagnostic): each change of a rider's original pick
 * (with the present count), and every 6000 picks the original levels picked
 * since the start.
 *
 * The only call is a direct call from 0x44FD0: it reaches the hook after the
 * fork pass port/tools/fork/fix_manual_hook_calls.py.
 */
#ifndef FORK_RIDERLOD_H
#define FORK_RIDERLOD_H

void riderlod_init(void);
void riderlod_set(int on);              /* diag port `set riderlod 0|1` (bench) */
void (*riderlod_lookup(unsigned int xbox_va))(void);

#endif
