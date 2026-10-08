/*
 * ps2legend -- the loading screens' controller legend in the PS2 control scheme.
 *
 * While a track loads, the game shows a drawing of the pad with one label
 * per button, taken from the language file data/lang/<language>.loc. In the
 * PS2 layout (ctlscheme.h) five of those labels name the Xbox layout's
 * action: Y (Triangle) and B (Circle) have no function in the default map
 * (Y was a grab button, B tweaked / boosted with X), the black button (R1)
 * grabs (it was the modifier in Default), and the left stick has no
 * "+modifier=late flips / spins" line (no modifier button: the PS2 files have
 * none, and the PS2 legend has no such line). The same strings label the
 * Options > Configure Controller picture. The host serves a copy of the
 * language file with these five strings changed; the disc and the extracted
 * files are never modified. The legend keeps the Xbox words with the debug
 * Xbox layout (XBOX_CONTROL_SCHEME=xbox).
 *
 * Y (Triangle) in the Pro preset: btnmap1 puts the reverse camera on it
 * (held), btnmap0 nothing. The PS2 legend says "no function" in Default and
 * "combat cam" in Pro, the word the Xbox Pro legend gives its own reverse
 * camera button (black, kLD_PRO_XBOX_buttonBLACK, served unchanged). But the
 * Xbox labels Y once for both presets:
 * - Configure Controller lays its labels out from three tables in .data,
 *   entries {x, y, flags, string index} (0x0A2512 / 0x0A256D create the
 *   labels, 0x0A1C70 places them; nothing else reads the tables): common 0x1BA0C8
 *   (15, Y among them), Default 0x1BA1B8 (3, black first) and Pro 0x1BA1E8
 *   (3, black first); the screen shows the group of the preset chosen. Black
 *   reads "grab board" in both presets here, Y does not: the two swap places.
 *   Y's entry in the common table becomes black's (kLD_DC_XBOX_buttonBLACK,
 *   "grab board"), black's in Default becomes Y's (kLD_XBOX_buttonY, "no
 *   function"), black's in Pro becomes Y's with kLD_PRO_XBOX_buttonBLACK
 *   ("combat cam"). Same counts, same strings, only two y positions and two
 *   indices change, in place, once the game image is loaded (the first
 *   language file served) and only if the tables hold the expected values.
 * - The loading legend (0x1387B0, [0x1DD848] != 0 = the saved Pro preset)
 *   draws Y with kLD_XBOX_buttonY and black with kLD_PRO_XBOX_buttonBLACK in
 *   Pro: while it draws in Pro, those two index entries point at "combat cam"
 *   and "grab board" (kLD_DC_XBOX_buttonBLACK) and are put back on return.
 *
 * The "Basic Controls" picture shown while the game boots (a splash screen,
 * SplashScreen_DrawTexture 0x136270) draws the Xbox pad; it is always
 * skipped, the way the game itself skips it when the picture is missing
 * ([this+0x414] < 0): the screen stays black for that second.
 *
 * PC platform layer (the console's own screens and words, for every layout):
 * - The console's words in the language file (XBOX_FIX_PC_TEXT, default 1;
 *   0 = Xbox text): "Xbox console", "hard disk", "controller port", "Press A
 *   ... B" in the storage messages read as on a PC. "exit to dashboard"
 *   (offered when there is no room to save) reads "quit game". Messages that
 *   only a memory unit can raise are kept: the host never reports one. The
 *   block counts stay: the game counts in blocks.
 * - "quit game" (XBOX_FIX_QUIT_GAME, default 1; 0 = the choice does nothing,
 *   as before): the save manager answers B on its "no room" messages with
 *   XLaunchNewImage(NULL, dashboard "memory" page), which ends in
 *   HalReturnToFirmware(2), a no-op in the kernel bridge. The game is closed
 *   the way closing its window does (d3d8_HostExit), but only for a call made
 *   during the save manager's update: XAPI's own start-up checks also launch
 *   the dashboard (an "invalid hard disk" error) and must keep running.
 *   Nothing is being written while those messages are up.
 * - Memory units (XBOX_FIX_HIDE_MU, default 1; 0 = Xbox list): the Save /
 *   Load device list keeps one row per device, the hard disk then the eight
 *   memory unit slots ("MU 1A not inserted"...), all greyed out on a PC. The
 *   eight slot rows are hidden (the row's SetVisible, vtable +0x10); the list
 *   closes up and keeps only the save folder. Those rows can never be chosen
 *   anyway: the game disables a device that is not there.
 * - "Checking hard disk" while the game boots: the save manager
 *   (update 0x0AF7B0, state at [this+0x3E84]) keeps each storage
 *   message up for a minimum number of frames ([this+0x14B8], 75 frames =
 *   1.25 s; the console's rule against flashing messages) after the check
 *   itself is over. In the checking states that wait is cut, so the screen
 *   lasts only as long as the check: the check itself, the save loaded from
 *   it and every error message (180 frames, or a button) are untouched.
 *   XBOX_FIX_SKIP_HDD_CHECK: 0 = Xbox timing, 1 = checking states only,
 *   2 (default) = also the "Autoloading from hard disk" wait (state 8,
 *   [this+0x14C4], 120 frames): the save is still loaded, only the message's
 *   minimum time on screen goes.
 *
 * .loc layout: "LOCH" header (0x14 bytes), then "LOCL" {u32 size to the end
 * of the file, u32 0, u32 count}, a table of `count` u32 offsets from the
 * "LOCL" tag, and the strings (UTF-16LE, NUL-terminated), 4-byte padded.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "recomp/recomp_types.h"
#include "xbox_file_hook.h"
#include "ps2legend.h"
#include "ctlscheme.h"

int btnicons_cfg_style(void);           /* nv2a_btnicons.c */

enum { EDIT_LEGEND, EDIT_PC, EDIT_PSCFG };
typedef struct { uint32_t index; int group; const char *text; } LocEdit;

/* Same indices in american.loc and letter.loc. The PC texts keep the
 * original's format arguments in the same order; a few drop trailing ones
 * (or all of them), which the game's formatting simply ignores. Where the PS2
 * release words a message without its memory card, that wording is used. */
static const LocEdit k_edits[] = {
    { 0xC92, EDIT_LEGEND, "no function" }, /* kLD_XBOX_buttonY:         "grab board" */
    { 0xC93, EDIT_LEGEND, "no function" }, /* kLD_XBOX_buttonB:         "tweak/boost" */
    { 0xE02, EDIT_LEGEND, "" },            /* kLD_PRO_la_ud1:           "+modifier=late flips" */
    { 0xE04, EDIT_LEGEND, "" },            /* kLD_PRO_la_ud1:           "+modifier=late spins" */
    { 0xE08, EDIT_LEGEND, "grab board" },  /* kLD_DC_XBOX_buttonBLACK:  "modifier" */
    /* kLD_PRO_XBOX_buttonBLACK "combat cam" is kept: it labels Y in Pro. */
    { 0xDEE, EDIT_PC,     "quit game" },   /* kOVExitToDashBoard:       "exit to dashboard" */
    /* Configure Controller in the drawn button styles (all three): the PS2
     * game's words where its legend differs (see k_cfg_layout); the original
     * style (no drawn buttons) keeps the Xbox words */
    { 0xC94, EDIT_PSCFG,  "jump" },           /* kLD_XBOX_buttonA:   "crouch/jump" */
    { 0xC8A, EDIT_PSCFG,  "crouch/brake" },   /* kLD_DC_la_ud1:      "late flip" (D-pad) */
    { 0xC8C, EDIT_PSCFG,  "turn/late spin" }, /* kLD_DC_la_lr1:      "late spin" (D-pad) */

    /* Controllers: "...In Controller Port 1", "...controller port %d..." */
    { 0x057, EDIT_PC, "No Controller Detected \\For Player 1" },             /* kFE_NoController1 */
    { 0x058, EDIT_PC, "No Controller Detected \\For Player 2" },             /* kFE_NoController2 */
    { 0xDFB, EDIT_PC, "Controller %d has been disconnected. "
                      "Please reconnect the controller." },                   /* kPause_unplugsingular */
    { 0xDFC, EDIT_PC, "Controllers %d and %d have been disconnected. "
                      "Please reconnect the controllers." },                  /* kPause_unplugplural */

    /* Storage: "Hard disk", "Your Xbox", "Press A to continue"... */
    { 0x151, EDIT_PC, "Delete failed." },                                    /* kOVDeleteFailed (PS2) */
    { 0x16C, EDIT_PC, "Load failed." },                                      /* kOVLoadFailed (PS2) */
    { 0x1B7, EDIT_PC, "Save failed." },                                      /* kOVSaveFailed (PS2) */
    { 0x16D, EDIT_PC, "Unable to load %s." },                                /* kOVLoadContentFailed */
    { 0x172, EDIT_PC, "Loading %s, please wait." },                          /* kOVMCLoading (PS2) */
    { 0x1BA, EDIT_PC, "Saving %s, please wait." },                           /* kOvSaveSaving (PS2) */
    { 0x1BB, EDIT_PC, "Saving %s, please wait." },                           /* kOvSaveToHardDrive */
    { 0x161, EDIT_PC, "Save folder" },                                       /* kOVHardDrive */
    { 0x174, EDIT_PC, "Not enough free disk space to save games." },         /* kOVMCNoGameSpace */
    { 0x175, EDIT_PC, "Not enough free disk space to save replays." },       /* kOVMCNoReplaySpace */
    { 0x1C9, EDIT_PC, "Not enough free disk space to save this file." },     /* kOVTooBigHardDrive */
    { 0xDEC, EDIT_PC, "%d blocks are required to save the Game/Options file." }, /* kOVBlocksRequiredOptions (PS2) */
    { 0xDED, EDIT_PC, "%d blocks are required to save a replay file." },     /* kOVBlocksRequiredReplay (PS2) */
    { 0xDEF, EDIT_PC, "Full" },                                              /* kOVFullMU */
    { 0x196, EDIT_PC, "Reading save data" },                                 /* kOVReadingHardDrive */
    { 0x197, EDIT_PC, "Checking save data" },                                /* kOVQueryingDevices */
    { 0xBA7, EDIT_PC, "Checking save data" },                                /* kOVMCCheckingCard */
    { 0xBA8, EDIT_PC, "Auto-Loading saved data" },                           /* kOVAutoLoading (PS2, first words) */
    { 0xBA9, EDIT_PC, "Read error, checking SSX save files." },              /* kOVMCError (PS2, first words) */
    { 0xBAA, EDIT_PC, "The save folder has a broken replay save file." },    /* kOVMCReplayCorrupt */
    { 0xC69, EDIT_PC, "The save folder has a broken game save file." },      /* kOVMCGameCorrupt */
    { 0xDE5, EDIT_PC, "The save data is damaged and cannot be used." },      /* kOVDeviceDamaged ("Memory Card in Slot A") */
    { 0xDF0, EDIT_PC, "Please don't quit the game." },                       /* kOVDontRemoveCard */
    { 0xDF1, EDIT_PC, "Please don't quit the game." },                       /* kOVDontTurnOffPower */
    { 0xDB2, EDIT_PC, "Save the current Game and Options." },                /* kFEHelp_SaveOptions */
    { 0xDB3, EDIT_PC, "Load a Game and Options." },                          /* kFEHelp_LoadOptions */
    { 0xDB4, EDIT_PC, "Load and view a replay." },                           /* kFEHelp_LoadReplay */
    { 0xDBA, EDIT_PC, "Load from the save folder when game is first started." }, /* kFEHelp_AutoLoad */
};
#define N_EDITS (sizeof k_edits / sizeof k_edits[0])
#define LOCL_AT 0x14u

/* Y in the Pro preset (see the top of the file). */
#define LOC_BUTTON_Y   0xC92u    /* kLD_XBOX_buttonY:         "no function" (served) */
#define LOC_BLACK_DC   0xE08u    /* kLD_DC_XBOX_buttonBLACK:  "grab board" (served) */
#define LOC_BLACK_PRO  0xE09u    /* kLD_PRO_XBOX_buttonBLACK: "combat cam" */

#define CFG_COMMON_Y   0x1BA108u /* common table, entry 4 (Y) */
#define CFG_DC_BLACK   0x1BA1B8u /* Default table, entry 0 (black) */
#define CFG_PRO_BLACK  0x1BA1E8u /* Pro table, entry 0 (black) */
#define F_200 0x43480000u        /* 200.0f: x of the right-hand labels */
#define F_21  0x41A80000u        /* 21.0f: y of Y's label */
#define F_41  0x42240000u        /* 41.0f: y of black's label */

static int entry_is(uint32_t e, uint32_t x, uint32_t y, uint32_t flags, uint32_t id)
{
    return MEM32(e) == x && MEM32(e + 4u) == y && MEM32(e + 8u) == flags && MEM32(e + 12u) == id;
}

/* Configure Controller: Y and black swap places between the common table
 * and the preset tables. Once (the game image must be loaded), and only if
 * all three entries are the original ones. .data, so writable. */
static void cfg_tables_ps2(void)
{
    static int s_done;
    if (s_done) return;
    s_done = 1;
    if (!entry_is(CFG_COMMON_Y, F_200, F_21, 0, LOC_BUTTON_Y) ||
        !entry_is(CFG_DC_BLACK, F_200, F_41, 0, LOC_BLACK_DC) ||
        !entry_is(CFG_PRO_BLACK, F_200, F_41, 0, LOC_BLACK_PRO)) {
        fprintf(stderr, "[PS2LEGEND] Configure Controller tables not as expected, left as they are\n");
        return;
    }
    MEM32(CFG_COMMON_Y + 4u) = F_41;  MEM32(CFG_COMMON_Y + 12u) = LOC_BLACK_DC;
    MEM32(CFG_DC_BLACK + 4u) = F_21;  MEM32(CFG_DC_BLACK + 12u) = LOC_BUTTON_Y;
    MEM32(CFG_PRO_BLACK + 4u) = F_21; /* index kept: "combat cam" */
    fprintf(stderr, "[PS2LEGEND] Configure Controller: Y labelled per preset (Pro: combat cam)\n");
}

/* Configure Controller in the drawn button styles (nv2a_btnicons.h,
 * btnicons_cfg_style): the pad is the style's picture, its legend lines
 * drawn to these labels, which take these places (port/assets/btnicons/pads/
 * make_config.py, LABELS): the 21 rows of the three tables, common 15,
 * Default 3, Pro 3, {x, y, flags, string}; the two stick circles (shapes 38,
 * 39, the icons' table 0x1BA218) go off the screen. Rows a style does not use
 * show the empty string 0xE02, off the screen too. The Default preset's
 * stick labels sit where the Pro preset's first label of each pair sits, so
 * they are common rows and the Pro preset's own three rows suffice. Once,
 * after cfg_tables_ps2, only on the tables it leaves (strings checked). */
#define CFG_ICONS 0x1BA218u      /* {x, y, ?, shape} x 3: the pad 61, circles 38 and 39 */
typedef struct { float x, y; uint32_t flags, index; } CfgRow;
static const CfgRow k_cfg_layout[3][21] = {
    { /* ps2 */
        {   113.04f,   134.75f, 0, 0xC90 },   /* pause */
        {    95.60f,   153.91f, 8, 0xC8F },   /* reset */
        {    38.32f,    -4.16f, 8, 0xC91 },   /* grab board */
        {   153.28f,    -4.16f, 0, 0xC91 },   /* grab board */
        {   201.18f,    76.31f, 0, 0xC94 },   /* jump */
        {   201.18f,    99.30f, 0, 0xC95 },   /* tweak/boost */
        {   201.18f,   117.51f, 0, 0xC8E },   /* shove */
        {    -9.58f,    24.58f, 8, 0xC8A },   /* crouch/brake */
        {    -9.58f,    39.91f, 8, 0xC8B },   /* prewind flips */
        {    -9.58f,    60.98f, 8, 0xC8C },   /* turn/late spin */
        {    -9.58f,    76.31f, 8, 0xC8D },   /* prewind spins */
        {   -43.11f,    96.43f, 8, 0xDFD },   /* crouch/brake */
        {   -34.49f,   129.96f, 8, 0xDFF },   /* turn */
        { -2000.00f,     0.00f, 0, 0xE02 },   /* (hidden) */
        { -2000.00f,     0.00f, 0, 0xE02 },   /* (hidden) */
        {   201.18f,    37.99f, 0, 0xC92 },   /* no function */
        { -2000.00f,     0.00f, 0, 0xE02 },   /* (hidden) */
        { -2000.00f,     0.00f, 0, 0xE02 },   /* (hidden) */
        {   201.18f,    37.99f, 0, 0xE09 },   /* combat cam */
        {   -43.11f,   111.76f, 8, 0xDFE },   /* prewind flips */
        {   -34.49f,   145.29f, 8, 0xE00 },   /* prewind spins */
    },
    { /* ps */
        {   106.70f,   150.00f, 0, 0xC90 },   /* pause */
        {    89.75f,   150.00f, 8, 0xC8F },   /* reset */
        {    38.30f,    -4.20f, 8, 0xC91 },   /* grab board */
        {   153.30f,    -4.20f, 0, 0xC91 },   /* grab board */
        {   201.20f,    76.30f, 0, 0xC94 },   /* jump */
        {   201.20f,    99.30f, 0, 0xC95 },   /* tweak/boost */
        {   201.20f,   117.50f, 0, 0xC8E },   /* shove */
        {    -9.60f,    24.60f, 8, 0xC8A },   /* crouch/brake */
        {    -9.60f,    39.90f, 8, 0xC8B },   /* prewind flips */
        {    -9.60f,    61.00f, 8, 0xC8C },   /* turn/late spin */
        {    -9.60f,    76.30f, 8, 0xC8D },   /* prewind spins */
        {    -9.60f,    94.40f, 8, 0xDFD },   /* crouch/brake */
        {    -9.60f,   128.70f, 8, 0xDFF },   /* turn */
        { -2000.00f,     0.00f, 0, 0xE02 },   /* (hidden) */
        { -2000.00f,     0.00f, 0, 0xE02 },   /* (hidden) */
        {   201.20f,    38.00f, 0, 0xC92 },   /* no function */
        { -2000.00f,     0.00f, 0, 0xE02 },   /* (hidden) */
        { -2000.00f,     0.00f, 0, 0xE02 },   /* (hidden) */
        {   201.20f,    38.00f, 0, 0xE09 },   /* combat cam */
        {    -9.60f,   109.70f, 8, 0xDFE },   /* prewind flips */
        {    -9.60f,   144.00f, 8, 0xE00 },   /* prewind spins */
    },
    { /* xb */
        {   105.00f,   165.00f, 0, 0xC90 },   /* pause */
        {    95.00f,   165.00f, 8, 0xC8F },   /* reset */
        {    30.00f,    -8.00f, 8, 0xC91 },   /* grab board */
        {   166.00f,    -8.00f, 0, 0xC91 },   /* grab board */
        {   200.00f,    21.00f, 0, 0xE08 },   /* grab board */
        {   200.00f,    78.00f, 0, 0xC94 },   /* crouch/jump */
        {   200.00f,    99.00f, 0, 0xC95 },   /* tweak/boost */
        {   200.00f,    59.00f, 0, 0xC93 },   /* no function */
        {   200.00f,   129.00f, 0, 0xC8E },   /* shove */
        {    -5.00f,   138.00f, 8, 0xC8A },   /* late flip */
        {    -5.00f,   155.00f, 8, 0xC8B },   /* prewind flips */
        {    -5.00f,   102.00f, 8, 0xC8C },   /* late spin */
        {    -5.00f,   118.00f, 8, 0xC8D },   /* prewind spins */
        {    -5.00f,    21.00f, 8, 0xDFD },   /* crouch/brake */
        {    -5.00f,    64.00f, 8, 0xDFF },   /* turn */
        {   200.00f,    41.00f, 0, 0xC92 },   /* no function */
        { -2000.00f,     0.00f, 0, 0xE02 },   /* (hidden) */
        { -2000.00f,     0.00f, 0, 0xE02 },   /* (hidden) */
        {   200.00f,    41.00f, 0, 0xE09 },   /* combat cam */
        {    -5.00f,    37.00f, 8, 0xDFE },   /* prewind flips */
        {    -5.00f,    80.00f, 8, 0xE00 },   /* prewind spins */
    },
};
static const uint32_t k_cfg_tables[3] = { 0x1BA0C8u, 0x1BA1B8u, 0x1BA1E8u };
static const uint32_t k_cfg_rows[3] = { 15, 3, 3 };
/* the strings the three tables hold once cfg_tables_ps2 has run */
static const uint32_t k_cfg_index[21] = {
    0xC90, 0xC8F, 0xC91, 0xC91, LOC_BLACK_DC, 0xC94, 0xC95, 0xC93, 0xC8E, 0xC8A, 0xC8B, 0xC8C, 0xC8D, 0xDFD, 0xDFF,
    LOC_BUTTON_Y, 0xE02, 0xE04,
    LOC_BLACK_PRO, 0xDFE, 0xE00,
};

static void cfg_tables_style(int style)
{
    static int s_done;
    uint32_t t, r, k = 0;
    if (s_done || style < 1 || style > 3) return;
    s_done = 1;
    for (t = 0; t < 3; t++)
        for (r = 0; r < k_cfg_rows[t]; r++, k++)
            if (MEM32(k_cfg_tables[t] + 16u * r + 12u) != k_cfg_index[k]) {
                fprintf(stderr, "[PS2LEGEND] Configure Controller tables not as expected (row %u), "
                        "the style's layout is not set\n", (unsigned)k);
                return;
            }
    if (MEM32(CFG_ICONS + 12u) != 61u || MEM32(CFG_ICONS + 28u) != 38u || MEM32(CFG_ICONS + 44u) != 39u) {
        fprintf(stderr, "[PS2LEGEND] Configure Controller icons table not as expected, the style's layout is not set\n");
        return;
    }
    for (k = 0, t = 0; t < 3; t++)
        for (r = 0; r < k_cfg_rows[t]; r++, k++) {
            const CfgRow *c = &k_cfg_layout[style - 1][k];
            uint32_t e = k_cfg_tables[t] + 16u * r;
            MEMF(e) = c->x;
            MEMF(e + 4u) = c->y;
            MEM32(e + 8u) = c->flags;
            MEM32(e + 12u) = c->index;
        }
    MEMF(CFG_ICONS + 16u) = -2000.0f;       /* circle 38 */
    MEMF(CFG_ICONS + 32u) = -2000.0f;       /* circle 39 */
    fprintf(stderr, "[PS2LEGEND] Configure Controller: the %s style's layout\n",
            style == 1 ? "ps2" : style == 2 ? "playstation" : "modern");
}

static int s_on;        /* the PS2 loading legend */
static int s_pctext;    /* XBOX_FIX_PC_TEXT */
static int s_hddskip;   /* XBOX_FIX_SKIP_HDD_CHECK */
static int s_quit;      /* XBOX_FIX_QUIT_GAME */
static int s_hidemu;    /* XBOX_FIX_HIDE_MU */

static int s_cfg;       /* btnicons_cfg_style(): the style's Configure Controller layout */

static const char *edit_for(uint32_t i)
{
    size_t j;
    for (j = 0; j < N_EDITS; j++)
        if (k_edits[j].index == i &&
            (k_edits[j].group == EDIT_LEGEND ? s_on :
             k_edits[j].group == EDIT_PSCFG ? s_on && s_cfg > 0 : s_pctext))
            return k_edits[j].text;
    return NULL;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static int match(const char *path)
{
    static const char *const names[] = { "data\\lang\\american.loc", "data\\lang\\letter.loc" };
    size_t n = strlen(path), i, k;
    for (k = 0; k < 2; k++) {
        size_t m = strlen(names[k]);
        const char *s;
        if (n < m) continue;
        s = path + n - m;
        for (i = 0; i < m; i++) {
            char c = s[i] == '/' ? '\\' : (char)tolower((unsigned char)s[i]);
            if (c != names[k][i]) break;
        }
        if (i == m) return 1;
    }
    return 0;
}

static void *rewrite(const char *path, const void *data, uint32_t size, uint32_t *out_size)
{
    const uint8_t *b = (const uint8_t *)data;
    uint32_t count, i, total, at, len, changed = 0;
    uint8_t *out;

    if (size < LOCL_AT + 16 || memcmp(b, "LOCH", 4) || memcmp(b + LOCL_AT, "LOCL", 4)) goto unknown;
    count = rd32(b + LOCL_AT + 12);
    if (count < 0xE0A || LOCL_AT + 16 + (uint64_t)count * 4 > size) goto unknown;

    /* The new size: every string, edited or not, then the padding. */
    total = LOCL_AT + 16 + count * 4;
    for (i = 0; i < count; i++) {
        const char *t = edit_for(i);
        if (t) { len = (uint32_t)strlen(t) * 2 + 2; changed++; }
        else {
            uint32_t o = LOCL_AT + rd32(b + LOCL_AT + 16 + i * 4), q = o;
            while (q + 1 < size && (b[q] | b[q + 1])) q += 2;
            if (q + 1 >= size) goto unknown;
            len = q + 2 - o;
        }
        total += len;
    }
    total = (total + 3) & ~3u;
    if (!(out = (uint8_t *)calloc(1, total))) return NULL;
    memcpy(out, b, LOCL_AT + 16);
    wr32(out + LOCL_AT + 4, total - LOCL_AT);
    at = LOCL_AT + 16 + count * 4;
    for (i = 0; i < count; i++) {
        const char *t = edit_for(i);
        wr32(out + LOCL_AT + 16 + i * 4, at - LOCL_AT);
        if (t) {
            for (; *t; t++, at += 2) out[at] = (uint8_t)*t;
            at += 2;
        } else {
            uint32_t o = LOCL_AT + rd32(b + LOCL_AT + 16 + i * 4), q = o;
            while (b[q] | b[q + 1]) q += 2;
            memcpy(out + at, b + o, q + 2 - o);
            at += q + 2 - o;
        }
    }
    *out_size = total;
    fprintf(stderr, "[PS2LEGEND] %s: served with %u strings changed (legend %d, PC text %d)\n",
            path, (unsigned)changed, s_on, s_pctext);
    if (s_on) {                     /* the game reads its first language file: image loaded */
        cfg_tables_ps2();
        cfg_tables_style(s_cfg);
    }
    return out;
unknown:
    fprintf(stderr, "[PS2LEGEND] %s: unknown layout, original served\n", path);
    return NULL;
}

/* The loading legend (called through a vtable only, so the lookup is enough).
 * Its labels come from the language file's string lookup (0x14FEC0): the
 * file as loaded at [[0x1E3C7C] + 0x50], "LOCL" section, u32 offsets from
 * "LOCL" at +0x10, count at +0xC. */
void sub_001387B0(void);

#define PRESET_PRO   0x1DD848u   /* != 0: the saved 1P preset is Pro */
#define GAME_GLOBALS 0x1E3C7Cu   /* [+0x50] = the language file */
#define TAG_LOCH     0x48434F4Cu
#define TAG_LOCL     0x4C434F4Cu

/* The address of the loaded language file's offset table, 0 if unknown. */
static uint32_t loc_table(void)
{
    uint32_t g = MEM32(GAME_GLOBALS), blob, locl;
    if (g < 0x1000u) return 0;
    blob = MEM32(g + 0x50u);
    if (blob < 0x1000u || MEM32(blob) != TAG_LOCH || (MEM32(blob + 8u) & 1u)) return 0;
    locl = blob + MEM32(blob + 0x10u + (uint32_t)MEM16(blob + 0xEu) * 4u);
    if (MEM32(locl) != TAG_LOCL || MEM32(locl + 0xCu) <= LOC_BLACK_PRO) return 0;
    return locl + 0x10u;
}

static void hook_legend_001387B0(void)
{
    uint32_t t = MEM32(PRESET_PRO) ? loc_table() : 0, y = 0, black = 0;

    if (t) {
        static int s_logged;
        y = MEM32(t + LOC_BUTTON_Y * 4u);
        black = MEM32(t + LOC_BLACK_PRO * 4u);
        MEM32(t + LOC_BUTTON_Y * 4u) = black;                         /* "combat cam" */
        MEM32(t + LOC_BLACK_PRO * 4u) = MEM32(t + LOC_BLACK_DC * 4u); /* "grab board" */
        if (!s_logged) {
            s_logged = 1;
            fprintf(stderr, "[PS2LEGEND] loading legend, Pro preset: Y labelled \"combat cam\"\n");
        }
    }
    sub_001387B0();
    if (t) {
        MEM32(t + LOC_BUTTON_Y * 4u) = y;
        MEM32(t + LOC_BLACK_PRO * 4u) = black;
    }
}

/* SplashScreen_DrawTexture (thiscall, no argument, plain ret): draws nothing. */
static void hook_splash_00136270(void)
{
    g_esp += 4;
}

/* The save manager's per-frame update (thiscall, no argument, plain ret;
 * called through its vtable only, so the lookup is enough). */
void sub_000AF7B0(void);

#define MCM_STATE     0x3E84u   /* 0..0x1D */
#define MCM_WAIT      0x14B8u   /* frames left before the next state may start */
#define MCM_WAIT_LOAD 0x14C4u   /* state 8: the same for "Autoloading from hard
                                   disk"; state 1: frames before the message shows */

/* Only the short waits set on the way through a check (0x4B), never an
 * error message's (0xB4). States 1, 9 and 0xB show "Checking hard disk" and
 * wait for the device first (vtable +0x28), then for MCM_WAIT. */
static __thread int s_in_save_update;   /* inside sub_000AF7B0 */

static void hook_hddcheck_000AF7B0(void)
{
    static int s_logged;
    uint32_t self = g_ecx, st = MEM32(self + MCM_STATE), w;
    int cut = 0;

    if (s_hddskip <= 0) {
        /* hooked for "quit game" only */
    } else if (st == 1u || st == 9u || st == 0xBu) {
        w = MEM32(self + MCM_WAIT);
        if ((int32_t)w > 0 && w <= 0x4Bu) { MEM32(self + MCM_WAIT) = 0; cut = 1; }
    } else if (st == 8u && s_hddskip >= 2) {
        w = MEM32(self + MCM_WAIT_LOAD);
        if ((int32_t)w > 0 && w <= 0x78u) { MEM32(self + MCM_WAIT_LOAD) = 0; cut = 1; }
    }
    if (cut && s_logged < 8) {
        s_logged++;
        fprintf(stderr, "[PS2LEGEND] storage message wait cut (state %u)\n", (unsigned)st);
    }
    g_ecx = self;
    s_in_save_update++;
    sub_000AF7B0();
    s_in_save_update--;
}

/* HalReturnToFirmware(Routine), from the kernel bridge. Routine 2 (quick
 * reboot) is how XLaunchNewImage leaves for the dashboard. */
extern void (*g_hal_return_hook)(uint32_t routine);
void d3d8_HostExit(void);

static void on_hal_return(uint32_t routine)
{
    if (s_quit && s_in_save_update && routine == 2u) {
        fprintf(stderr, "[PS2LEGEND] quit game chosen: closing the game\n");
        fflush(stderr);
        d3d8_HostExit();                    /* does not return */
    }
    fprintf(stderr, "[PS2LEGEND] HalReturnToFirmware(%u) ignored (%s)\n", (unsigned)routine,
            s_in_save_update ? "save manager" : "not the player's choice");
}

/* Calls a guest thiscall function that takes one argument; returns eax.
 * The registers the caller may still need are kept. */
static uint32_t guest_this1(recomp_func_t fn, uint32_t self, uint32_t arg)
{
    uint32_t sv_eax = g_eax, sv_ecx = g_ecx, sv_edx = g_edx, esp0 = g_esp, r;
    g_esp -= 4; MEM32(g_esp) = arg;
    g_esp -= 4; MEM32(g_esp) = 0;           /* dummy return address */
    g_ecx = self;
    fn();
    r = g_eax;
    g_esp = esp0;
    g_eax = sv_eax; g_ecx = sv_ecx; g_edx = sv_edx;
    return r;
}

/* The Save / Load screen's per-device row update (thiscall, the device
 * index as its only argument, ret 4; called through the screen's vtable
 * 0x198C40 only). Device 0 is the hard disk, 1..8 the memory unit slots.
 * The row is found the way the update finds it: child 0 of the screen's
 * tree (+0xEA8, list at +0xF0), then child <device> of that list (+0xF8). */
void sub_00094260(void);
void sub_000A3990(void);
void sub_000A3940(void);

static void hook_devrow_00094260(void)
{
    uint32_t self = g_ecx, dev = MEM32(g_esp + 4), tree, list, row, va;
    recomp_func_t setvis;

    sub_00094260();
    if (dev < 1u || dev > 8u) return;
    tree = MEM32(self + 0xEA8u);
    if (tree < 0x1000u) return;
    list = guest_this1(sub_000A3990, tree + 0xF0u, 0);
    if (list < 0x1000u) return;
    row = guest_this1(sub_000A3940, list + 0xF8u, dev);
    if (row < 0x1000u || !MEM8(row + 0x11u)) return;  /* already hidden */
    va = MEM32(MEM32(row) + 0x10u);                    /* SetVisible(bool) */
    setvis = recomp_lookup_manual(va);
    if (!setvis) setvis = recomp_lookup(va);
    if (!setvis) return;
    guest_this1(setvis, row, 0);
    {
        static int s_logged;
        if (s_logged < 8) {
            s_logged++;
            fprintf(stderr, "[PS2LEGEND] device list: memory unit row %u hidden\n", (unsigned)dev);
        }
    }
}

void ps2legend_init(void)
{
    const char *e = getenv("XBOX_FIX_PC_TEXT");
    s_on = ctlscheme_ps2();             /* after ctlscheme_init */
    s_pctext = !(e && e[0] == '0');
    e = getenv("XBOX_FIX_SKIP_HDD_CHECK");
    s_hddskip = e ? atoi(e) : 2;
    e = getenv("XBOX_FIX_QUIT_GAME");
    s_quit = !(e && e[0] == '0');
    e = getenv("XBOX_FIX_HIDE_MU");
    s_hidemu = !(e && e[0] == '0');
    s_cfg = btnicons_cfg_style();
    if (s_on || s_pctext)
        xbox_file_add_hook(match, rewrite);
    g_hal_return_hook = on_hal_return;      /* logs each call; closes only with s_quit */
    fprintf(stderr, "[PS2LEGEND] PS2 loading legend %d, PC text %d, hard disk check wait cut %d, "
            "quit game %d, memory units hidden %d\n", s_on, s_pctext, s_hddskip, s_quit, s_hidemu);
}

void (*ps2legend_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x00136270u) return hook_splash_00136270;
    if (xbox_va == 0x001387B0u && s_on) return hook_legend_001387B0;
    if (xbox_va == 0x000AF7B0u && (s_hddskip > 0 || s_quit)) return hook_hddcheck_000AF7B0;
    if (xbox_va == 0x00094260u && s_hidemu) return hook_devrow_00094260;
    return 0;
}
