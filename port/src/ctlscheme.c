/*
 * ctlscheme -- the control scheme: the PS2 layout on every pad. See ctlscheme.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "recomp/recomp_types.h"
#include "kernel/xbox_file_hook.h"
#include "ctlscheme.h"
#include "nv2a/nv2a_btnicons.h"         /* btnicons_on, the bumpers' sizes */

#define GAME_MODE_VA   0x001DEC94u      /* GameMode_Current; 6 = lesson (tutorial) */
#define GAME_MODE_LESSON 6u


/* The title's PS2 table (0x5B650, 0x5B68E-0x5B6FE): the mask of each grab
 * slot, easyA..easyD, medA..medF, hardA..hardE -- every combination of the
 * four shoulder bits, one button, then two, then three, then four. */
static const uint16_t k_slot_ps2[15] = {
    0x1000, 0x0400, 0x2000, 0x0200,
    0x2400, 0x1200, 0x2200, 0x3000, 0x0600, 0x1400,
    0x2600, 0x3400, 0x1600, 0x3200, 0x3600,
};

/* PS2 shoulder buttons, and the btnmap token of the modern-pad button in
 * the same place (LB / RB are the Xbox White / Black, LT / RT its triggers). */
enum { L1, R1, L2, R2, SHOULDERS };
static const char *const k_token[SHOULDERS] = { "WHITE", "BLACK", "LSHIFT", "RSHIFT" };

/* Which button each bit of the PS2 table is. Not known for sure (no PS2 data
 * here): "a" takes the title's LSHIFT / RSHIFT names (0x200 / 0x400) for L1 /
 * R1, which also puts L2 / R2 on the Xbox's LT / RT for the single-button
 * slots the Xbox files kept; "b" swaps L1 and L2. */
static const struct { uint16_t bit; int btn[2]; } k_bits[4] = {
    { 0x0200, { L1, L2 } }, { 0x0400, { R1, R1 } },
    { 0x1000, { L2, L1 } }, { 0x2000, { R2, R2 } },
};
static int s_order;                     /* 0 = "a", 1 = "b" */
static int s_on;                        /* PS2 layout served */
static int s_lesson;                    /* ... in lessons too (XBOX_FIX_LESSON_PS2) */

/* Button bits of the title's token table (0x1BA378). */
#define BIT_X       0x00000010u         /* Square */
#define BIT_Y       0x00000020u
#define BIT_B       0x00000040u         /* Circle */
#define BIT_LSHIFT  0x00000200u         /* left trigger */
#define BIT_RSHIFT  0x00000400u         /* right trigger */
#define BIT_BLACK   0x02000000u
#define BIT_WHITE   0x04000000u
static const uint32_t k_token_bit[SHOULDERS] = { BIT_WHITE, BIT_BLACK, BIT_LSHIFT, BIT_RSHIFT };

/* The Xbox files' grab slots (btnmap0.dat and btnmap1.dat of the disc, the
 * same in both): pressed mask per slot, 0 for (NONE). Observed: Y | LT | RT. */
#define XBOX_GRAB_BITS (BIT_Y | BIT_LSHIFT | BIT_RSHIFT)
static const uint32_t k_slot_xbox[15] = {
    BIT_LSHIFT, BIT_Y, BIT_RSHIFT, BIT_LSHIFT | BIT_RSHIFT,
    BIT_Y | BIT_RSHIFT, BIT_Y | BIT_LSHIFT, 0, 0, 0, 0,
    0, 0, 0, 0, BIT_Y | BIT_LSHIFT | BIT_RSHIFT,
};

/* ── Text rewrite ──────────────────────────────────────────────────── */

typedef struct { char *p; uint32_t n, cap; int ok; } Buf;

static void put(Buf *b, const char *s, uint32_t n)
{
    if (!b->ok) return;
    if (b->n + n + 1 > b->cap) {
        uint32_t cap = (b->n + n + 1) * 2;
        char *q = (char *)realloc(b->p, cap);
        if (!q) { b->ok = 0; return; }
        b->p = q; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

/* The PS2 shoulder buttons of one grab slot, as button bits. */
static uint32_t grab_mask_bits(int slot)
{
    uint32_t m = 0;
    int i;
    for (i = 0; i < 4; i++)
        if (k_slot_ps2[slot] & k_bits[i].bit) m |= k_token_bit[k_bits[i].btn[s_order]];
    return m;
}

static void grab_mask_text(int slot, char *out, size_t n)
{
    int b, i, first = 1;
    int has[SHOULDERS] = { 0 };
    out[0] = '\0';
    for (i = 0; i < 4; i++)
        if (k_slot_ps2[slot] & k_bits[i].bit) has[k_bits[i].btn[s_order]] = 1;
    for (b = 0; b < SHOULDERS; b++) {
        if (!has[b]) continue;
        snprintf(out + strlen(out), n - strlen(out), "%s%s", first ? "" : " | ", k_token[b]);
        first = 0;
    }
}

#define SHOULDER_MASK "WHITE | BLACK | LSHIFT | RSHIFT"

enum { SEC_NONE, SEC_BOOL, SEC_GRAB, SEC_ANALOG };      /* IS_BOOL, IS_BOOLGRAB, IS_ANALOG */

/* The two PS2 button maps, written here by hand: every line that
 * differs from the Xbox files apart from the grab slots (k_slot_ps2) and the
 * renamed buttons (Cross / Select are the Xbox A / Back bits). Each rule
 * gives the Xbox text it expects and the PS2 function in the Xbox tokens
 * (Square = X, Triangle = Y, L3 = LJOY); a line that does not read as
 * expected is left as it is. map -1: both files. */
typedef struct { signed char map, section; const char *name, *xbox, *ps2; } Rule;
static const Rule k_rules[] = {
    { -1, SEC_BOOL,   "Boost",         "B|X",   "X" },      /* Square alone */
    { -1, SEC_BOOL,   "Tweak",         "B|X",   "X" },
    {  0, SEC_BOOL,   "AnticAbort",    "NONE",  "LJOY" },   /* L3 */
    {  0, SEC_BOOL,   "LateSpinMode",  "BLACK", "NONE" },   /* no button on the PS2 */
    {  1, SEC_BOOL,   "CameraReverse", "BLACK", "Y" },      /* Triangle */
    {  0, SEC_ANALOG, "Spin",     "ANALOG_PAD_X|ANALOG_JOY_L_X", "ANALOG_PAD_X" },  /* D-pad alone */
    {  0, SEC_ANALOG, "Flip",     "ANALOG_PAD_Y|ANALOG_JOY_L_Y", "ANALOG_PAD_Y" },
    {  0, SEC_ANALOG, "GateRock", "ANALOG_PAD_Y|ANALOG_JOY_L_Y", "ANALOG_JOY_L_Y" }, /* stick alone */
};
#define N_RULES (sizeof k_rules / sizeof k_rules[0])

/* a and b equal once their blanks are skipped. */
static int same_tokens(const char *a, const char *b)
{
    for (;;) {
        while (*a == ' ' || *a == '\t') a++;
        while (*b == ' ' || *b == '\t') b++;
        if (*a != *b) return 0;
        if (!*a) return 1;
        a++; b++;
    }
}

/* New "pressed" / "observed" text for one entry, or NULL to keep it. */
static void entry_rule(int map, int section, int index, const char *name, const char *pressed,
                       char *np, const char **pp, const char **po, size_t n, int *missed)
{
    size_t r;
    *pp = NULL; *po = NULL;
    if (section == SEC_GRAB) {                          /* slot by line order */
        if (index < 15) {
            grab_mask_text(index, np, n);
            *pp = np;
            *po = SHOULDER_MASK;
        }
        return;
    }
    if (section == SEC_BOOL && !_stricmp(name, "RandomGrab")) {
        *po = SHOULDER_MASK;                            /* never active; observes the grabs' buttons */
        return;
    }
    for (r = 0; r < N_RULES; r++) {
        const Rule *u = &k_rules[r];
        if (u->section != section || (u->map >= 0 && u->map != map) || _stricmp(name, u->name))
            continue;
        if (same_tokens(pressed, u->xbox)) *pp = u->ps2;
        else (*missed)++;
        return;
    }
}

char *ctlscheme_rewrite_text(const char *data, uint32_t size, int map, uint32_t *out_size)
{
    Buf b = { NULL, 0, 0, 1 };
    const char *p = data, *end = data + size;
    int section = SEC_NONE, index = 0, grabs = 0, missed = 0;

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *le = eol ? eol + 1 : end;           /* line incl. its newline */
        const char *t = p;
        while (t < le && (*t == ' ' || *t == '\t')) t++;
        if (!strncmp(t, "IS_BOOLGRAB", 11) && !isalnum((unsigned char)t[11]) && t[11] != '_') {
            section = SEC_GRAB; index = 0;
        } else if (!strncmp(t, "IS_BOOL", 7) && !isalnum((unsigned char)t[7]) && t[7] != '_') {
            section = SEC_BOOL; index = 0;
        } else if (!strncmp(t, "IS_ANALOG", 9)) {
            section = SEC_ANALOG; index = 0;
        } else if (*t == '{' && section) {
            /* { Name, (pressed), (observed) }, */
            const char *nm = t + 1, *ne, *a0, *a1, *b0, *b1;
            char name[32], pressed[96], np[96];
            const char *pp, *po;
            while (nm < le && (*nm == ' ' || *nm == '\t')) nm++;
            ne = nm;
            while (ne < le && isalpha((unsigned char)*ne)) ne++;
            a0 = memchr(ne, '(', (size_t)(le - ne));
            a1 = a0 ? memchr(a0, ')', (size_t)(le - a0)) : NULL;
            b0 = a1 ? memchr(a1, '(', (size_t)(le - a1)) : NULL;
            b1 = b0 ? memchr(b0, ')', (size_t)(le - b0)) : NULL;
            if (b1 && ne > nm && (size_t)(ne - nm) < sizeof name && (size_t)(a1 - a0) < sizeof pressed) {
                memcpy(name, nm, (size_t)(ne - nm)); name[ne - nm] = '\0';
                memcpy(pressed, a0 + 1, (size_t)(a1 - a0 - 1)); pressed[a1 - a0 - 1] = '\0';
                entry_rule(map, section, index, name, pressed, np, &pp, &po, sizeof np, &missed);
                if (section == SEC_GRAB) grabs++;
                index++;
                if (pp || po) {
                    put(&b, p, (uint32_t)(a0 + 1 - p));
                    if (pp) put(&b, pp, (uint32_t)strlen(pp)); else put(&b, a0 + 1, (uint32_t)(a1 - a0 - 1));
                    put(&b, a1, (uint32_t)(b0 + 1 - a1));
                    if (po) put(&b, po, (uint32_t)strlen(po)); else put(&b, b0 + 1, (uint32_t)(b1 - b0 - 1));
                    put(&b, b1, (uint32_t)(le - b1));
                    p = le;
                    continue;
                }
            }
        }
        put(&b, p, (uint32_t)(le - p));
        p = le;
    }
    if (!b.ok || grabs != 15) {             /* not the layout we know: serve the original */
        free(b.p);
        return NULL;
    }
    if (missed)
        fprintf(stderr, "[CTLSCHEME] btnmap%d: %d line(s) not as expected, kept as they are\n",
                map, missed);
    *out_size = b.n;
    return b.p;
}

/* ── Hook ──────────────────────────────────────────────────────────── */

static int match(const char *path)
{
    static const char tail[] = "data\\config\\btnmap";
    size_t n = strlen(path), k = sizeof tail - 1, i;
    const char *s;
    /* ...data\config\btnmap<digit>.dat, either slash, any case */
    if (n < k + 5) return 0;
    s = path + n - (k + 5);
    for (i = 0; i < k; i++) {
        char c = s[i] == '/' ? '\\' : (char)tolower((unsigned char)s[i]);
        if (c != tail[i]) return 0;
    }
    return isdigit((unsigned char)s[k]) && !_stricmp(s + k + 1, ".dat");
}

/* ── Trick text ────────────────────────────────────────────────────
 *
 * The text of a trick's buttons (trick book of the pause, UBER TRICK
 * announcement, lessons; 0x5C980) is built from the button tables the served
 * file filled, through the title's glyph table 0x1AA820: 12 entries
 * {u32 mask, u32 character}, read in order and by nothing else. As the PS2
 * game shows them: the buttons in the PS2 table's order, the shoulder buttons
 * L1 R1 L2 R2 first. The tweak reads Square alone because the served file has
 * Tweak on Square alone. Only the text changes, never the controls. The
 * original table is put back whenever the original files are served. */

#define GLYPH_TABLE 0x001AA820u
#define GLYPHS      12

/* With drawn button icons (nv2a_btnicons.h), L1 and R1 are the game's White
 * and Black glyphs (shapes 103 / 104, 16 x 18 / 16 x 19), smaller than the
 * triggers' (22 x 28): as on the PS2, where the four shoulder buttons have the
 * same size, they are drawn larger in the trick text -- the shapes' size
 * while 0x5CEB0 draws one glyph, their spacing in the glyph layout table
 * 0x1AA880 (24-byte rows {char, scale, dx, dy, advance, shape}). */
#define GLYPH_LAYOUT   0x001AA880u
#define SHAPE_SIZE     1.45f            /* 16 x 18 -> 23 x 26, about the triggers' */
static int s_bumper_big;
static uint32_t s_layout_orig[2][3];

static uint32_t s_glyph_orig[GLYPHS * 2];
static int s_glyph_saved, s_glyph_ps2, s_tricktext;

static void glyph_table(int ps2)
{
    uint32_t t[GLYPHS * 2];
    int i, n = 0;
    if (!s_glyph_saved) {
        for (i = 0; i < GLYPHS * 2; i++) s_glyph_orig[i] = MEM32(GLYPH_TABLE + 4u * i);
        s_glyph_saved = 1;
    }
    if (ps2 == s_glyph_ps2) return;
    if (!ps2) {
        memcpy(t, s_glyph_orig, sizeof t);
    } else {
        static const uint32_t order[GLYPHS] = {
            BIT_WHITE, BIT_BLACK, BIT_LSHIFT, BIT_RSHIFT,       /* L1 R1 L2 R2 */
            0x10u, BIT_Y, 0x40u, 0x800u, 8u, 4u, 1u, 2u,         /* X Y B A, D-pad */
        };
        int k;
        for (k = 0; k < GLYPHS; k++)
            for (i = 0; i < GLYPHS; i++)
                if (s_glyph_orig[2 * i] == order[k]) {
                    t[2 * n] = order[k];
                    t[2 * n + 1] = s_glyph_orig[2 * i + 1];
                    n++;
                    break;
                }
        if (n != GLYPHS) {                  /* not the table we know: leave it */
            fprintf(stderr, "[CTLSCHEME] trick text: unknown glyph table, left as is\n");
            return;
        }
    }
    for (i = 0; i < GLYPHS * 2; i++) MEM32(GLYPH_TABLE + 4u * i) = t[i];
    s_glyph_ps2 = ps2;
    {   /* layout rows 6 and 7 (':' Black = R1, ';' White = L1): dx, dy, advance;
         * "wide": the shapes are already wide and low (hook_shapes_000EF960) */
        static const int32_t big[3] = { 3, -2, 24 };
        int32_t wide[3] = { 3, 2, 27 };
        int r, f;
        int on = ps2 && btnicons_on(), w = on && btnicons_wide_bumpers();
        if (w) wide[1] = btnicons_bumper_text_dy();         /* letters on the triggers' line */
        for (r = 0; r < 2 && !btnicons_ps2(); r++) {    /* ps2 style: hook_shapes_000EF960 */
            uint32_t row = GLYPH_LAYOUT + 24u * (uint32_t)(6 + r);
            if (!s_layout_orig[r][2])
                for (f = 0; f < 3; f++) s_layout_orig[r][f] = MEM32(row + 8u + 4u * f);
            for (f = 0; f < 3; f++)
                MEM32(row + 8u + 4u * f) = w ? (uint32_t)wide[f] : on ? (uint32_t)big[f] : s_layout_orig[r][f];
        }
        s_bumper_big = on && !w && !btnicons_ps2();
    }
    fprintf(stderr, "[CTLSCHEME] trick text: %s\n",
            ps2 ? "PS2 (L1 R1 L2 R2)" : "original");
}

/* ── The PS2 button style (XBOX_BUTTON_ICONS=ps2) ───────────────────
 *
 * nv2a_btnicons.c fills the UV rectangle of each button shape of the trick
 * and race HUDs (97-113, measured in game) with the PS2-style picture,
 * stretched; hook_shapes_000EF960 (below), right after the game has filled
 * the HUD's shapes, gives those shapes the size that shows the picture at the
 * PS2 size (btnicons_ps2_shape), which undoes the stretch. A shape is
 * drawn at its size wherever it is used (lessons, trick text, UBER panel,
 * replay help, dialogs, race HUD), so this one change is global. Only a
 * shape at the game's own size and UVs is changed. The trick text's glyph
 * layout takes the PS2 values (dx, dy, advance). */
/* 0x1AA880 rows in order ({char, scale, dx, dy, advance, shape}), each with
 * the PS2 dx, dy, advance for the same button. */
static const struct { uint32_t ch, shape; int32_t dx, dy, adv; } k_ps2_layout[GLYPHS] = {
    { '@', 105, 4, -8, 32 }, { '!', 106, 4, -8, 32 },          /* L2 R2 */
    { '%',  99, 3,  0, 26 }, { '^', 100, 3,  0, 26 },          /* Square Triangle */
    { '&',  98, 3,  0, 26 }, { '*',  97, 3,  0, 26 },          /* Circle Cross */
    { ':', 104, 4,  1, 32 }, { ';', 103, 4,  1, 32 },          /* R1 L1 */
    { 'a', 111, 5,  5, 22 }, { 'd', 110, 5,  5, 22 },          /* left right */
    { 'w', 112, 5,  4, 18 }, { 'x', 113, 5,  4, 18 },          /* up down */
};

static void lesson_pad(uint32_t mgr);

static void shapes_ps2(uint32_t mgr)
{
    static int s_layout;
    unsigned shape;
    float uv[4], game[2], ps2[2];
    int k;
    if (!s_layout) {                        /* glyph layout: data, written once */
        s_layout = 1;
        for (k = 0; k < GLYPHS; k++) {
            uint32_t row = GLYPH_LAYOUT + 24u * (uint32_t)k;
            if (MEM32(row) != k_ps2_layout[k].ch || MEM32(row + 20u) != k_ps2_layout[k].shape) break;
        }
        if (k < GLYPHS) {
            fprintf(stderr, "[CTLSCHEME] ps2 buttons: unknown glyph layout table, left as is\n");
        } else {
            for (k = 0; k < GLYPHS; k++) {
                uint32_t row = GLYPH_LAYOUT + 24u * (uint32_t)k;
                MEM32(row + 8u) = (uint32_t)k_ps2_layout[k].dx;
                MEM32(row + 12u) = (uint32_t)k_ps2_layout[k].dy;
                MEM32(row + 16u) = (uint32_t)k_ps2_layout[k].adv;
            }
            fprintf(stderr, "[CTLSCHEME] ps2 buttons: PS2 glyph layout\n");
        }
    }
    for (k = 0; btnicons_ps2_shape(k, &shape, uv, game, ps2); k++) {
        uint32_t sh = mgr + 8u + shape * 0x1Cu;                 /* ShapeManager_GetShape 0xF24F0 */
        float w = MEMF(sh + 4u), h = MEMF(sh + 8u);             /* {tex, w, h, v0, u0, u1, v1} */
        float v0 = MEMF(sh + 12u) * 256.0f, u0 = MEMF(sh + 16u) * 256.0f;
        float u1 = MEMF(sh + 20u) * 256.0f, v1 = MEMF(sh + 24u) * 256.0f;
        if (w != game[0] || h != game[1] || fabsf(u0 - uv[0]) > 0.25f || fabsf(v0 - uv[1]) > 0.25f ||
            fabsf(u1 - uv[2]) > 0.25f || fabsf(v1 - uv[3]) > 0.25f) {
            fprintf(stderr, "[CTLSCHEME] ps2 buttons: shape %u is %.2f x %.2f (uv %.1f,%.1f-%.1f,%.1f), left as is\n",
                    shape, w, h, u0, v0, u1, v1);
            continue;
        }
        MEMF(sh + 4u) = ps2[0];
        MEMF(sh + 8u) = ps2[1];
        fprintf(stderr, "[CTLSCHEME] ps2 buttons: shape %u %.0f x %.0f -> %.2f x %.2f\n",
                shape, w, h, ps2[0], ps2[1]);
    }
}

/* ── Trick tutorial pad, button styles ps2, playstation, modern ─────
 *
 * The lesson's demo ("show me", LessonMan_DrawRecordedButtonPrompt 0x56760)
 * draws shape 96 (the pad) at its shape size, then a gold marker on the
 * button the demo presses: the title's table 0x1AA6C8, 12 rows {float dx,
 * float dy, u32 shape, u32 button mask}, positions from the pad's corner.
 * With the style's own pad (nv2a_btnicons.c: the PS2 style's, or a
 * DualShock 4 or Xbox One drawing), the shapes show its pictures and the rows take its
 * positions for the same button (btnicons_lesson_mark; L1 = White, R1 =
 * Black, L2 = LT, R2 = RT, as the PS2 layout puts them; the PS2 Select /
 * Start rows have no place here and the demo never shows them). Both are
 * changed only from the game's own values, checked first. */
#define LESSON_MARKS 0x001AA6C8u
static const struct { uint32_t mask, shape; float dx, dy; } k_lesson_marks[12] = {
    { 0x00000200u, 125,   9,   0 },     /* LT    = L2 */
    { 0x00000400u, 126, 189,   0 },     /* RT    = R2 */
    { 0x00000800u, 117, 172,  72 },     /* A     = cross */
    { 0x00000040u, 118, 175,  53 },     /* B     = circle */
    { 0x00000010u, 119, 154,  64 },     /* X     = square */
    { 0x00000020u, 120, 156,  44 },     /* Y     = triangle */
    { 0x04000000u, 123, 166,  27 },     /* White = L1 */
    { 0x02000000u, 124, 186,  34 },     /* Black = R1 */
    { 0x00000008u, 132,  38,  96 },     /* left */
    { 0x00000004u, 133,  59,  96 },     /* right */
    { 0x00000002u, 131,  44, 111 },     /* down */
    { 0x00000001u, 130,  44,  90 },     /* up */
};

static void lesson_pad(uint32_t mgr)
{
    static int s_rows, s_logged, s_odd;
    int k, f, pad = 0;
    for (k = 0; k < 32; k++) {
        unsigned shape;
        float g[6], p[6];
        uint32_t sh;
        int is_game = 1, is_ours = 1;
        if (!btnicons_lesson_shape(k, &shape, g, p)) break;        /* none: no lesson pad in this style */
        sh = mgr + 8u + shape * 0x1Cu;      /* {tex, w, h, v0, u0, u1, v1} */
        for (f = 0; f < 6; f++) {
            float v = MEMF(sh + 4u + 4u * (uint32_t)f);
            if (v != g[f]) is_game = 0;
            if (v != p[f]) is_ours = 0;
        }
        if (!is_ours && !is_game) {         /* another shape set (the front end's), or unknown */
            if (!(s_odd & (1 << k))) {
                s_odd |= 1 << k;
                fprintf(stderr, "[CTLSCHEME] lesson pad: shape %u is not the game's, left as is\n", shape);
            }
            continue;
        }
        if (is_game) {
            for (f = 0; f < 6; f++) MEMF(sh + 4u + 4u * (uint32_t)f) = p[f];
            if (s_logged < 64) {
                s_logged++;
                fprintf(stderr, "[CTLSCHEME] lesson pad: shape %u %.0f x %.0f -> %.0f x %.0f (style picture)\n",
                        shape, g[0], g[1], p[0], p[1]);
            }
        }
        if (k == 0) pad = 1;                /* shape 96 shows the style's pad */
    }
    if (s_rows || !pad) return;
    /* the rows, once the pad is the style's (data, never rewritten) */
    {
        int game = 0;
        for (k = 0; k < 12; k++) {
            uint32_t row = LESSON_MARKS + 16u * (uint32_t)k;
            if (MEM32(row + 8u) != k_lesson_marks[k].shape || MEM32(row + 12u) != k_lesson_marks[k].mask) break;
            if (MEMF(row) == k_lesson_marks[k].dx && MEMF(row + 4u) == k_lesson_marks[k].dy) game++;
        }
        if (k < 12 || game != 12) {
            fprintf(stderr, "[CTLSCHEME] lesson pad: unknown marker table, left as is\n");
            s_rows = -1;
            return;
        }
        for (k = 0; k < 12; k++) {
            uint32_t row = LESSON_MARKS + 16u * (uint32_t)k;
            float dx, dy;
            if (!btnicons_lesson_mark(k, &dx, &dy)) return;
            MEMF(row) = dx;
            MEMF(row + 4u) = dy;
        }
        s_rows = 1;
        fprintf(stderr, "[CTLSCHEME] lesson pad: markers at the style pad's buttons\n");
    }
}

void sub_0005CEB0(void);    /* trick text: draw one glyph (thiscall, 3 args, ret 12) */

/* The two bumper shapes of the current shape set, larger for this one draw. */
void hook_tricktext_0005CEB0(void)
{
    uint32_t self = g_ecx, app, mgr = 0, sh[2] = { 0, 0 };
    float w[2] = { 0, 0 }, h[2] = { 0, 0 };
    int k;
    if (s_bumper_big && (app = MEM32(0x001E3C7Cu)) != 0)
        mgr = MEM32(app + 0x724u);
    if (mgr) {
        for (k = 0; k < 2; k++) {
            sh[k] = mgr + 8u + (uint32_t)(103 + k) * 0x1Cu;     /* ShapeManager_GetShape 0xF24F0 */
            w[k] = MEMF(sh[k] + 4u);
            h[k] = MEMF(sh[k] + 8u);
            if (w[k] < 8.0f || w[k] > 20.0f || h[k] < 8.0f || h[k] > 22.0f) { sh[k] = 0; continue; }
            MEMF(sh[k] + 4u) = w[k] * SHAPE_SIZE;
            MEMF(sh[k] + 8u) = h[k] * SHAPE_SIZE;
        }
    }
    g_ecx = self;
    sub_0005CEB0();
    for (k = 0; k < 2; k++)
        if (sh[k]) {
            MEMF(sh[k] + 4u) = w[k];
            MEMF(sh[k] + 8u) = h[k];
        }
}

void sub_000EF960(void);    /* HUD shapes: size and UVs of shapes 0-139 (thiscall, no arg, plain ret) */

/* The HUD's shape set is filled by 0xEF960 (immediates, once per race or
 * lesson load); nothing else writes it until the next load. Right after it,
 * each button style rewrites its shapes, only those still at the game's own
 * size and UVs:
 *   - modern and PlayStation: the bumpers (shapes 103 White = LB / L1, 104
 *     Black = RB / R1) wide and low, the size that shows the squeezed region
 *     of their glyph (nv2a_btnicons.h, BTNICONS_BUMPER_*) unstretched;
 *   - ps2: shapes 97-113 at the PS2 sizes, and its glyph layout
 *     (shapes_ps2 above);
 *   - ps2, playstation and modern: the trick tutorial's pad and markers
 *     (lesson_pad above). */
static void shapes_wide_bumpers(uint32_t mgr)
{
    static const float game_h[2] = { 18.0f, 19.0f };
    int k;
    for (k = 0; k < 2; k++) {
        uint32_t sh = mgr + 8u + (uint32_t)(103 + k) * 0x1Cu;    /* ShapeManager_GetShape 0xF24F0 */
        float w = MEMF(sh + 4u), h = MEMF(sh + 8u);
        float uw = (MEMF(sh + 20u) - MEMF(sh + 16u)) * 256.0f, uh = (MEMF(sh + 24u) - MEMF(sh + 12u)) * 256.0f;
        if (w != 16.0f || h != game_h[k] || uw < 14.5f || uw > 15.5f || uh < 16.5f || uh > 18.5f) {
            fprintf(stderr, "[CTLSCHEME] bumpers: shape %d is %.2f x %.2f (uv %.2f x %.2f), left as is\n",
                    103 + k, w, h, uw, uh);
            continue;
        }
        MEMF(sh + 4u) = uw * BTNICONS_BUMPER_W / BTNICONS_BUMPER_RW;
        MEMF(sh + 8u) = uh * BTNICONS_BUMPER_H / BTNICONS_BUMPER_RH_SLOT;
        {   /* UV rows narrowed to the drawn rows, scaled about the drawing's top:
             * the same texel lands on the same point of the quad */
            const float r = (float)BTNICONS_BUMPER_RH / BTNICONS_BUMPER_RH_SLOT;
            float v0 = MEMF(sh + 12u) + 0.5f * (1.0f - r) / 256.0f;
            MEMF(sh + 12u) = v0;
            MEMF(sh + 24u) = v0 + uh * r / 256.0f;
        }
        fprintf(stderr, "[CTLSCHEME] bumpers: shape %d %.0f x %.0f -> %.2f x %.2f, uv rows %.1f -> %.2f\n",
                103 + k, w, h, MEMF(sh + 4u), MEMF(sh + 8u), uh, (MEMF(sh + 24u) - MEMF(sh + 12u)) * 256.0f);
    }
}

void hook_shapes_000EF960(void)
{
    uint32_t mgr = g_ecx;
    sub_000EF960();
    if (mgr < 0x1000u) return;
    if (btnicons_wide_bumpers()) shapes_wide_bumpers(mgr);
    else if (btnicons_ps2()) shapes_ps2(mgr);
    lesson_pad(mgr);                        /* the trick tutorial's pad, in the styles that have one */
}

static void *rewrite(const char *path, const void *data, uint32_t size, uint32_t *out_size)
{
    uint32_t mode = MEM32(GAME_MODE_VA);
    size_t n = strlen(path);
    int map = path[n - 5] - '0';            /* ...btnmap<digit>.dat (match) */
    char *out;
    if (mode == GAME_MODE_LESSON && !s_lesson) {
        fprintf(stderr, "[CTLSCHEME] %s: lesson, original served (XBOX_FIX_LESSON_PS2=0)\n", path);
        glyph_table(0);
        return NULL;
    }
    out = ctlscheme_rewrite_text((const char *)data, size, map, out_size);
    glyph_table(out && s_tricktext);
    fprintf(stderr, "[CTLSCHEME] %s: %s (game mode %u%s)\n", path,
            out ? "PS2 layout served" : "unknown layout, original served", mode,
            mode == GAME_MODE_LESSON ? ", lesson" : "");
    return out;
}

/* ── Lesson demos ──────────────────────────────────────────────────── */

#define LESSON_STEP        0x0Cu        /* LessonMan: current step; 5 = demo playing */
#define LESSON_STEP_DEMO   5u
#define LESSON_BUFFER      0x458u       /* LessonMan: the loaded .inp buffer */
#define INP_SIZE           0x4D598u     /* its fixed size (LessonMan_Construct) */
#define INP_RECORD         0x2Cu        /* one frame */
#define INP_STATE          0x24u        /* + pos * INP_RECORD: the state injected at pos */

uint32_t ctlscheme_lesson_buttons(uint32_t m)
{
    uint32_t g = m & XBOX_GRAB_BITS;
    uint32_t out = m & ~(XBOX_GRAB_BITS | BIT_BLACK | BIT_WHITE | BIT_B);
    int s;
    if (g)
        for (s = 0; s < 15; s++)
            if (k_slot_xbox[s] == g) { out |= grab_mask_bits(s); break; }
    if (m & BIT_B) out |= BIT_X;                /* tweak / boost: Square alone */
    return out;                                 /* Black (late spin mode) has no PS2 button */
}

void sub_000565B0(void);    /* LessonMan_InjectRecordedInputFrame (thiscall, no arg, plain ret) */

/* The state about to be injected is translated in the buffer for the time of
 * the call, then put back: a restarted lesson replays the same recording. */
void hook_lesson_000565B0(void)
{
    static int s_logged;
    uint32_t self = g_ecx, buf = 0, at = 0, prev = 0, cur = 0;
    int on = 0;

    if (s_on && s_lesson && MEM32(self + LESSON_STEP) == LESSON_STEP_DEMO &&
        (buf = MEM32(self + LESSON_BUFFER)) != 0) {
        uint32_t pos = MEM32(buf);
        if (pos < (INP_SIZE - INP_STATE - 8u) / INP_RECORD) {
            at = buf + INP_STATE + pos * INP_RECORD;
            prev = MEM32(at);
            cur = MEM32(at + 4u);
            MEM32(at) = ctlscheme_lesson_buttons(prev);
            MEM32(at + 4u) = ctlscheme_lesson_buttons(cur);
            on = 1;
            if (!s_logged) {
                s_logged = 1;
                fprintf(stderr, "[CTLSCHEME] lesson demo: recorded Xbox buttons replayed in the PS2 layout\n");
            }
        }
    }
    g_ecx = self;
    sub_000565B0();
    if (on) {
        MEM32(at) = prev;
        MEM32(at + 4u) = cur;
    }
}

int ctlscheme_ps2(void)
{
    return s_on;
}

void (*ctlscheme_lookup(unsigned int xbox_va))(void)
{
    if (s_on && s_lesson && xbox_va == 0x000565B0u) return hook_lesson_000565B0;
    if (s_on && s_tricktext && xbox_va == 0x0005CEB0u) return hook_tricktext_0005CEB0;
    if ((btnicons_wide_bumpers() || btnicons_ps2()) && xbox_va == 0x000EF960u) return hook_shapes_000EF960;
    return 0;
}

void ctlscheme_init(void)
{
    const char *e = getenv("XBOX_CONTROL_SCHEME");
    const char *o = getenv("XBOX_PS2_SHOULDERS");
    const char *l = getenv("XBOX_FIX_LESSON_PS2");
    const char *t = getenv("XBOX_FIX_TRICKTEXT_PS2");
    s_on = !(e && !_stricmp(e, "xbox"));
    s_order = o && (o[0] == 'b' || o[0] == 'B');
    s_lesson = !(l && l[0] == '0');
    s_tricktext = !(t && t[0] == '0');
    if (!s_on) {
        fprintf(stderr, "[CTLSCHEME] XBOX_CONTROL_SCHEME=xbox: original Xbox layout (debug)\n");
        return;
    }
    xbox_file_set_hook(match, rewrite);
    fprintf(stderr, "[CTLSCHEME] PS2 layout (shoulders order %c, lessons %s, trick text %s)\n",
            s_order ? 'b' : 'a', s_lesson ? "PS2" : "Xbox", s_tricktext ? "PS2" : "original");
}
