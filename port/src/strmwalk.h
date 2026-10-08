/*
 * strmwalk -- EA stream reader: bounded block search (XBOX_FIX_STRMWALK) and
 * lock barrier before the current block is read (XBOX_FIX_STRMLOCK).
 *
 * The EA stream object ('STRM', set up by 0x14ED70) keeps a ring of blocks
 * read from disc. Each block starts with [B+0] = fourcc (0xFFFFFFFE free,
 * 0xFFFFFFFF = "wrap to the ring base [S+0x3C]") and [B+4] = (channel << 24)
 * | size. A channel handle H holds [H+0] = STRM, [H+4] = channel,
 * [H+8] = bytes pending (signed) and [H+0xC] = its current block.
 *
 * 0x14F197 (tail of "release current block" 0x14F170) clears the current
 * block's channel tag and subtracts its size from [H+8] under the STRM lock,
 * then, if [H+8] > 0, walks the ring -- without the lock and without any
 * bound -- until a block carries the channel's tag. It assumes "[H+8] > 0
 * means a tagged block lies ahead". When that does not hold the walk never
 * ends: it runs past the parsed data, out of the ring, and spins on whatever
 * word it lands on (seen: [H+8] = 0x7FF40600 for a 1 MiB ring, the walk
 * stuck outside the ring on a block of size 0, while stopping the intro
 * video). The game then shows no further image.
 *
 * XBOX_FIX_STRMWALK=2 (default): the same code, but the walk gives up when it
 * leaves the ring [S+0x38, S+0x40), meets a block of size 0, wraps a second
 * time or exceeds one ring's worth of steps. It then sets [H+8] = 0 (under
 * the STRM lock) and leaves [H+0xC] alone -- what the stop path (0x14F8D0)
 * does to every handle right after anyway. A healthy walk is not affected.
 * One [STRMWALK] line per give-up (first 16, then every 256th).
 * Before all this, a current block [H+0xC] outside the ring is not a block
 * yet (the reader counts a channel's first block before it stores it, and
 * the count is read without the lock): the hook then answers "no block", as
 * 0x14F170 does for a count of 0, and changes nothing ("[STRMWALK] not yet").
 * XBOX_FIX_STRMWALK=1: the same, plus a stop at the parse pointer [S+0x60]:
 * the reader tags a block, then (under the lock) counts it and moves [S+0x60]
 * past it, so every block [H+8] counts lies below the parse pointer, and
 * beyond it are only blocks not parsed yet (stale tags from the previous lap
 * included). [S+0x60] is read outside the lock, hence not the default.
 * XBOX_FIX_STRMWALK=0: the original code, unchanged.
 *
 * XBOX_FIX_STRMLOCK=1 (default), independent of the above: before reading
 * [H+0xC], take and release the STRM lock. The reader stores the count, then
 * the current block, inside that lock; 0x14F170 reads the count outside it.
 * The barrier waits for a reader still between its two stores, so the block
 * read matches the count (the single-core Xbox gave this for free). Without
 * it, a channel refilled from empty can hand back its last consumed block,
 * consumed a second time: the count drifts below zero and wraps. It applies
 * with XBOX_FIX_STRMWALK=0 too. XBOX_FIX_STRMLOCK=0: the original order.
 *
 * The only call is a direct tail call from 0x14F170: it reaches the hook
 * after the fork pass port/tools/fork/fix_manual_hook_calls.py.
 */
#ifndef FORK_STRMWALK_H
#define FORK_STRMWALK_H

void strmwalk_init(void);
void (*strmwalk_lookup(unsigned int xbox_va))(void);

#endif
