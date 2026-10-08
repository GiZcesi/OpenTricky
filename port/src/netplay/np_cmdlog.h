/*
 * np_cmdlog -- log of the riders' commands and state.
 *
 * First building block of multiplayer, WITHOUT network: on each race tick,
 * the 32-bit command word of every rider and its key state are written to a
 * replayable binary file. The hooks change nothing: they always call the
 * original and read afterwards.
 *
 * ── Switch (environment variable, read once at start)
 *
 *   XBOX_NETLOG unset / "0"   OFF (default). recomp_lookup_manual() returns
 *                             none of the hooks: original path unchanged.
 *   XBOX_NETLOG=1             LOG. One file per race:
 *                             <_local>/netlog/cmdlog_<YYYYMMDD_HHMMSS>_<n>.npcl
 *   XBOX_NETLOG_DIR           output folder (default: the highest "_local"
 *                             found walking up from the exe, + \netlog; else
 *                             the exe's folder).
 *
 * ── The hooks (conventions checked in the bytes)
 *
 *   The command is requested once per rider and per tick by
 *   Rider_UpdatePhysicsState (0x36990), call site 0x36F7F:
 *       push &cmd ; ecx = rider ; call [ [rider] + 0x24 ]   (RECOMP_ICALL_SAFE)
 *   then consumed by 0x31640 (dispatch on rider+0x458, see below).
 *
 *   0x0005BEB0  Player command (pad)          slot +0x24 of vtable 0x189ED0
 *   0x00048C40  OtherRider command (AI)       slot +0x24 of vtable 0x188D50
 *
 *   Both: thiscall, 1 argument (pointer to the command word, 4 bytes, zeroed
 *   then filled by the original), ret 4. They are adjustor THUNKS
 *   (`sub ecx,[ecx-4]` then jmp 0x5AA30 / 0x48AB0): the ENTRY ecx (the one
 *   the hook reads) is the Rider sub-object pointer BEFORE adjustment, i.e.
 *   exactly the value stored in race+0xC4[i] (checked at runtime: esi at the
 *   call site = these entries). Only references: their vtables ->
 *   lookup_manual sees 100 % of the calls.
 *   Rules: read ecx and the argument BEFORE the original (it pops 4 bytes);
 *   after the original, read the word; g_eax/g_ecx/g_edx restored around the
 *   log; stack checked (entry + 4 + 4, else counter esp_bad).
 *
 * ── Measured
 *   Thread: every call comes on ONE thread, the RenderFrame one during the
 *     race (checked with XBOX_PASS_TAGS=log alongside): the physics update
 *     and the render are sequential on the game thread.
 *   Tick: 1 call per rider and per tick; race+0x18 goes up by 1 per tick.
 *   U2: the game CATCHES UP. The simulation holds 60 ticks/s of real time
 *     when the display slows down (several ticks between two Presents, never
 *     a Present without a tick); under very heavy load (16 Presents/s) it
 *     drops to ~52 ticks/s (up to 10 ticks per Present observed).

 * ── Offsets used (Rider sub-object unless stated; status)
 *
 *   race object   race = [[0x1E3C7C]+0x72C]+0x1C                     MEASURED
 *     race+0x88   number of riders                                   MEASURED (runtime 6)
 *     race+0xC4[] pointers to the Rider sub-objects                  MEASURED
 *     race+0x7C / +0x80  number of Players / AIs                     MEASURED (runtime 1/5)
 *     race+0x18   race frame counter                                 INFERRED (checked by the tool)
 *     race+0x1C   race state (4 = Race)                              INFERRED
 *   roster 0x1DE900, stride 0x98, count [0x1DE8FC]                   MEASURED
 *   RNG seeds: [0x1DEC98] (per race), [0x1DEC9C] (load)              MEASURED
 *   track [0x1DEC90], game mode [0x1DEC94]                           INFERRED (upstream notes)
 *   rider+0x170..+0x178  position                                    MEASURED (notes + runtime)
 *   rider+0x180..+0x188  velocity                                    MEASURED (notes)
 *   rider+0x458  rider state / mode: switch key of both command
 *                generators AND of 0x31640                          MEASURED
 *   rider+0x15C  speed factor (written by the AI)                    write MEASURED / role INFERRED
 *   key of 0x31640: read exactly as 0x31640 reads it, m = [rider+0x58E0],
 *     key = [m + [[m+0x30]+4] + 0x488]. The 0x488 offset is relative to the
 *     full object (virtual base at +0x30), so the key is rider+0x458 (not
 *     a "physics mode at rider+0x488"); the NPCL_F_MODE_IS_458 flag checks
 *     it on every record (100 % of 116,280 records in one run).
 *
 * ── .npcl file format (little endian, all uint32 unless stated)
 *
 *   Header:
 *     magic 'NPCL' (0x4C43504E), version (1), header size in bytes, record
 *     size (56), track, game mode, seed 0x1DEC98, seed 0x1DEC9C, address of
 *     the race object, number of riders n (<= 16), number of Players, number
 *     of AIs, n Rider sub-object pointers (race+0xC4[]), number of roster
 *     entries r (<= 16), r x 0x98 raw roster bytes, 2 reserved dwords (0).
 *     Size = 4 x (15 + n) + 0x98 x r.
 *   Then 56-byte records (struct npcl_record below), one per command call,
 *   in call order.
 *   v3: on each change of race+0x1C, on the first command call of the tick
 *   and BEFORE the original, two "RNG" records: kind NPCL_KIND_RNG_A /
 *   NPCL_KIND_RNG_B, idx 0xFE, cmd = race_state, and the 6 dwords of the RNG
 *   state in pos[0..2] then vel[0..2] (raw bits). Tools must skip idx 0xFE in
 *   per-rider statistics.
 *
 *   tick: the log's own counter, +1 when a rider already seen in the current
 *   tick asks for its command again (so one tick = one pass of
 *   Rider_UpdatePhysicsState over the riders). present: d3d8_PresentSeq() at
 *   the time of the call (used for the U2 measurement). New file when the
 *   race object changes or race+0x18 goes back.
 *
 *   Tool: port/tools/np_cmdlog_dump.py (CSV + PNG of the paths + U2).
 */
#ifndef NP_CMDLOG_H
#define NP_CMDLOG_H

#include <stdint.h>

#define NPCL_MAGIC        0x4C43504Eu   /* "NPCL" */
#define NPCL_VERSION      3u   /* 2: phys_mode = exact key of 0x31640; 3: RNG events */
#define NPCL_MAX_RIDERS   16u
#define NPCL_ROSTER_STRIDE 0x98u

/* kind */
#define NPCL_KIND_PLAYER  0u
#define NPCL_KIND_AI      1u
#define NPCL_KIND_RNG_A   2u   /* v3: global RNG state (0x1FAD70) */
#define NPCL_KIND_RNG_B   3u   /* v3: "race" RNG state (0x1FAD88) */

#define NPCL_RNG_A_VA     0x001FAD70u   /* 6 dwords, seeded from the clock at boot */
#define NPCL_RNG_B_VA     0x001FAD88u   /* 6 dwords, reseeded per race from [0x1DEC98] */
/* flags */
#define NPCL_F_MODE_IS_458 0x01u        /* the key of 0x31640 is indeed rider+0x458 */
#define NPCL_F_IDX_UNKNOWN 0x02u        /* rider missing from race+0xC4[] */

#pragma pack(push, 1)
typedef struct {
    uint32_t tick;          /* log counter (see header) */
    uint32_t race_frame;    /* race+0x18 */
    uint32_t present;       /* d3d8_PresentSeq() */
    uint8_t  idx;           /* index in race+0xC4[], 0xFF if unknown */
    uint8_t  kind;          /* NPCL_KIND_* (by hook) */
    uint8_t  flags;         /* NPCL_F_* */
    uint8_t  race_state;    /* race+0x1C (low byte) */
    uint32_t cmd;           /* command word after the original */
    float    pos[3];        /* rider+0x170 */
    float    vel[3];        /* rider+0x180 */
    uint32_t ev_state;      /* rider+0x458 */
    float    speed_factor;  /* rider+0x15C */
    uint32_t phys_mode;     /* switch key of 0x31640 (= rider+0x458) */
} npcl_record;

/* Player state snapshots, .npst file next to the .npcl
 * (XBOX_NETLOG_STATE=1). Header: magic 'NPST', version 1, block size
 * (NPST_BLOCK), index of the Player in race+0xC4[]. Then, on each tick where
 * the Player asks for its command (same point as the .npcl records, after
 * the original): npst_record then NPST_BLOCK raw bytes of the Rider
 * sub-object, from its offset 0. Used for the ghost's state corrections
 * (np_ghost.h, XBOX_GHOST_STATE) and for the field inventory. */
#define NPST_MAGIC   0x5453504Eu   /* "NPST" */
#define NPST_VERSION 1u
#define NPST_BLOCK   0x5A00u
typedef struct {
    uint32_t race_frame;    /* race+0x18 */
    uint32_t race_state;    /* race+0x1C */
} npst_record;
#pragma pack(pop)

typedef char npcl_record_size_check[sizeof(npcl_record) == 56 ? 1 : -1];

extern int g_np_cmdlog_on;

void np_cmdlog_init(void);
void (*np_cmdlog_lookup(uint32_t xbox_va))(void);

#endif /* NP_CMDLOG_H */
