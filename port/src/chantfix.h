/*
 * chantfix -- crowd chant names: the slot one past the end of each list
 * holds a valid name (XBOX_FIX_CHANT).
 *
 * The race's sound-emitter manager [0x1F8908] (0x443C bytes, filled with
 * 0xDEADC0DE when created) keeps the crowd chant bank names read from
 * data/config/chant.inf by 0x11EE20: 64 bytes each, from mgr + 4, 20 slots per
 * character (CHARCHANTBANKS = 4 used, count at mgr + 0x4104), then the
 * general chants from mgr + 0x3C04 (GENCHANTBANKS = 12 used, count at
 * mgr + 0x4108). The other slots are never written.
 *
 * Starting a chant (0x11F3E0, four places) picks a slot as
 * (rand & 0x7FFF) * count / 0x7FFF -- which gives `count` itself when the
 * random number is 0x7FFF -- and copies its name with an unbounded strcpy
 * into a 64-byte stack buffer. Slot `count` is one of the unwritten ones:
 * 0xDEADC0DE with no terminating zero, so the copy runs on over the stack of
 * the game thread (its own frame, then its callers'). What follows is the
 * crash or freeze seen in long replayed races: the sound-emitter
 * loop 0x120CF0 resumes with a garbage index, zeroes a byte of an EA audio
 * producer (callback 0x19950 -> 0x9950), stops a sound with a garbage handle,
 * the async loader opens "D:\", and the stream mixer's converter writes
 * from address 0 (.text[0xFB030] = 0).
 *
 * The chance is 1 in 32768 per pick. A pick is made on every render while a
 * chant is pending, so the uncapped frame rate (XBOX_FPS_CAP=0, several
 * hundred renders a second) makes it likely within a race.
 *
 * XBOX_FIX_CHANT=1 (default): right after 0x11EE20 has read the names, slot
 * `count` of each list -- when it lies inside the list's 20 slots and holds
 * no terminated name -- gets a copy of slot `count - 1`. A pick of `count`
 * then plays the last bank instead of overrunning the stack; every other
 * pick is unchanged. XBOX_FIX_CHANT=0: the original data.
 *
 * The only call is a direct call from 0x1108C0: it reaches the hook after the
 * fork pass port/tools/fork/fix_manual_hook_calls.py.
 */
#ifndef FORK_CHANTFIX_H
#define FORK_CHANTFIX_H

void chantfix_init(void);
void (*chantfix_lookup(unsigned int xbox_va))(void);

#endif
