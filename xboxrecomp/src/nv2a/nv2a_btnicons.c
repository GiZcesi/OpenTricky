/* Button icons -- see nv2a_btnicons.h.
 *
 * Runs on the pump thread only (texture uploads), so no locking. Each atlas
 * is composed once (a few ms, logged with XBOX_BUTTON_ICONS_LOG=1) and kept:
 * the texture cache may drop and re-upload an atlas, which then gets the
 * same composed texture again. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_btnicons.h"

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"     /* only BC2 is used here */
#include "bcdec/bcdec.h"
#pragma GCC diagnostic pop

#define SCALE 4                 /* composed atlas = 4x the game's (1024 x 1024) */
#define MARGIN 1                /* texels cleared around a glyph, against filtering bleed */
#define INSET 1                 /* texels left clear inside the rectangle: the game's quads
                                 * for a glyph reach a texel or two into its neighbour's */
#define FE1_FACES_DY 1          /* menus (fe_1): the face buttons sit this many texels below the
                                 * middle of their rectangle (about one 640-wide unit), on the
                                 * middle of the "select / previous / options" text: the Duke's
                                 * glyph body there is about 1.5 texels higher, its shadow below */

/* The PS2 game's own buttons (MODE_PS2): built in unless CMake has
 * -DSSX_PS2_BUTTONS=OFF, which leaves their pictures out; "ps2" is then auto
 * (and the launcher only announces that choice). */
#ifdef SSX_PS2_BUTTONS_READY
#define PS2_ORIGINAL_READY 1
#else
#define PS2_ORIGINAL_READY 0
#endif

enum { MODE_OFF, MODE_MODERN, MODE_PS, MODE_DEBUG, MODE_PS2 };   /* new modes go last: their numbers are compared elsewhere */
enum { B_A, B_B, B_X, B_Y, B_WHITE, B_BLACK, B_LT, B_RT, B_BACK, B_START,
       B_DLEFT, B_DRIGHT, B_DUP, B_DDOWN, B_DPAD, B_LSTICK, B_RSTICK, B_COUNT };

/* Texels left clear inside a glyph's rectangle. The shoulder buttons, the
 * triggers and Back / Start fill theirs: their modern drawings are wider than
 * the Duke's glyphs and only come close to the original's body that way (the
 * texels between those rectangles and their neighbours are cleared margins). */
static int slot_inset(int button)
{
    switch (button) {
    case B_WHITE: case B_BLACK: case B_LT: case B_RT: case B_BACK: case B_START: return 0;
    default: return INSET;
    }
}

/* The drawing for each button, per pad (resource names, ssx_recomp.rc.in).
 * Mapping by position: L1 = White, R1 = Black, L2 = LT, R2 = RT in the PS2
 * control scheme, and the same buttons in the Xbox one. */
static const char *const k_res[2][B_COUNT] = {
    { "BTN_X_A", "BTN_X_B", "BTN_X_X", "BTN_X_Y", "BTN_X_LB", "BTN_X_RB",
      "BTN_X_LT", "BTN_X_RT", "BTN_X_VIEW", "BTN_X_MENU",
      "BTN_X_DLEFT", "BTN_X_DRIGHT", "BTN_X_DUP", "BTN_X_DDOWN",
      "BTN_X_DPAD", "BTN_X_LSTICK", "BTN_X_RSTICK" },
    { "BTN_P_CROSS", "BTN_P_CIRCLE", "BTN_P_SQUARE", "BTN_P_TRIANGLE", "BTN_P_L1", "BTN_P_R1",
      "BTN_P_L2", "BTN_P_R2", "BTN_P_SELECT", "BTN_P_START",
      "BTN_P_DLEFT", "BTN_P_DRIGHT", "BTN_P_DUP", "BTN_P_DDOWN",
      "BTN_P_DPAD", "BTN_P_LSTICK", "BTN_P_RSTICK" },
};

/* PS2 original (MODE_PS2): the PS2 button pictures of port/assets/btnicons/ps2/
 * (4x, nearest neighbour). Resource "PS2_" + set + name, one set per PS2 atlas:
 * the trick HUD's (hudtrick, also used for the menus' fe_1 and the loading
 * screens' hud) and the race HUD's (hudgame: same pictures, its own
 * palette). The menus take the HUD's light face buttons, not the darker ones
 * of the PS2 front end's own atlas, which read badly on the grey panels.
 * The PS2 pad has one stick picture, for both sticks. */
enum { SET_TRICK, SET_GAME, N_SETS };
static const char *const k_set_name[N_SETS] = { "PS2_", "PS2_GAME_" };
static const char *const k_res_ps2[B_COUNT] = {
    "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "L1", "R1", "L2", "R2", "SELECT", "START",
    "DPAD_LEFT", "DPAD_RIGHT", "DPAD_UP", "DPAD_DOWN", "DPAD", "STICK", "STICK" };

/* Glyph rectangles at 1x (bounding box of each glyph's opaque pixels, shadow
 * included, measured on the decoded atlases). Black / Back / Start are three
 * black ovals: which is which comes from the debug mode on screen. */
typedef struct { uint8_t x, y, w, h, button, trim_r; } Slot;   /* trim_r: texels kept clear on the right */

static const Slot k_fe1[] = {                 /* menus */
    { 5, 230, 20, 23, B_Y, 0 }, { 31, 230, 20, 23, B_X, 0 }, { 56, 230, 19, 23, B_A, 0 }, { 80, 230, 20, 23, B_B, 0 },
    { 220, 33, 14, 18, B_WHITE, 0 }, { 221, 52, 14, 18, B_BLACK, 0 }, { 236, 32, 18, 14, B_BACK, 0 }, { 237, 51, 18, 14, B_START, 0 },
};
static const Slot k_hud1[] = {                /* race HUD and trick HUD (same layout) */
    { 5, 230, 20, 23, B_Y, 0 }, { 31, 230, 20, 23, B_X, 0 }, { 56, 230, 19, 23, B_A, 0 }, { 80, 230, 20, 23, B_B, 0 },
    /* the RT quad starts about 3 texels left of its glyph: the LT drawing stops short */
    { 137, 185, 21, 28, B_LT, 3 }, { 160, 185, 19, 28, B_RT, 0 }, { 196, 236, 14, 18, B_WHITE, 0 },
    /* Back and Start as the replay help shows them: "BACK-Change Camera" (the
     * Back button changes the camera) points at the glyph at (194,220),
     * "START-Exit" at the one at (236,187); Black is Skip Forward. */
    { 220, 189, 14, 18, B_BLACK, 0 }, { 236, 187, 18, 14, B_START, 0 }, { 194, 220, 18, 14, B_BACK, 0 },
    /* the four D-pad arms of the Duke (shapes 110-113: the spins and flips of the trick text,
     * glyph characters 'd' 'a' 'w' 'x' = right, left, up, down) */
    { 141, 225, 25, 31, B_DRIGHT, 0 }, { 168, 225, 24, 30, B_DLEFT, 0 },
    { 181, 186, 33, 24, B_DUP, 0 }, { 107, 230, 33, 24, B_DDOWN, 0 },
    /* the Duke's D-pad disc and its two sticks, as the replay help shows them:
     * "Toggle Subject / Menu Up / Menu Down" = the disc, "Select Highlight /
     * Play Highlights" = the left stick (97,180), "Camera Rotation" = the right
     * one (56,181); no glyph of a pressed stick here */
    { 7, 180, 44, 44, B_DPAD, 0 }, { 56, 181, 34, 36, B_RSTICK, 0 }, { 97, 180, 34, 36, B_LSTICK, 0 },
};
static const Slot k_hudload[] = {             /* loading screens */
    { 6, 229, 18, 23, B_Y, 0 }, { 30, 230, 18, 22, B_X, 0 }, { 56, 230, 18, 22, B_A, 0 }, { 82, 229, 17, 23, B_B, 0 },
    { 220, 32, 14, 18, B_WHITE, 0 }, { 221, 51, 14, 18, B_BLACK, 0 }, { 236, 31, 18, 14, B_BACK, 0 }, { 237, 50, 18, 14, B_START, 0 },
};

/* Atlases by the FNV-1a 64 key of their level-0 guest bytes (as nv2a_hdtex.c
 * and port/tools/hdtex_index.py compute it); 32-bit atlases have two keys
 * (linear and swizzled bytes). */
typedef struct { uint64_t key; const char *name; const Slot *slots; int n, set; } Atlas;
#define N(a) (int)(sizeof(a) / sizeof((a)[0]))
static const Atlas k_atlas[] = {
    { 0x4eed54c00fcc7bb3ull, "fe_1",     k_fe1,     N(k_fe1),     SET_TRICK },
    { 0x0d4cdae8ef7bef83ull, "fe_1",     k_fe1,     N(k_fe1),     SET_TRICK },
    { 0x8c8800a11b0d092cull, "hudgame",  k_hud1,    N(k_hud1),    SET_GAME },
    { 0x7d16f4697d06f9a4ull, "hudgame",  k_hud1,    N(k_hud1),    SET_GAME },
    { 0x4d46ac29a6066fb5ull, "hudtrick", k_hud1,    N(k_hud1),    SET_TRICK },
    { 0x192ed96c355beb6dull, "hudtrick", k_hud1,    N(k_hud1),    SET_TRICK },
    { 0x20f3fcaa821cd2a4ull, "hud",      k_hudload, N(k_hudload), SET_TRICK },
};
#define N_ATLAS N(k_atlas)

static int g_mode = -1, g_log;
static IDirect3DTexture8 *g_done[N_ATLAS];      /* composed, kept for re-uploads */

typedef struct { int w, h; uint8_t *rgba; } Img;   /* straight alpha */
static Img g_icon[B_COUNT];
static Img g_ps2[N_SETS][B_COUNT];              /* MODE_PS2 */
static int g_icons_loaded;

/* Trick tutorial ("show me"), MODE_PS2 and MODE_PS: the style's own pad and
 * gold markers in place of the Duke and its markers, in the trick HUD's atlas
 * (hudtrick hud1): the pad at the atlas' corner, the markers in the free rows
 * under it (the Duke's place, texels 0-179). Pictures already 4x, pasted
 * texel for texel, never resampled:
 *   - MODE_PS2: the PS2 pictures (resources "PS2_LESSON_*",
 *     port/assets/btnicons/ps2/); the pad is the PS2 atlas' own box
 *     (0,0) 256 x 126, the origin of the PS2 lesson's marker positions;
 *   - MODE_PS and MODE_MODERN: a DualShock 4 / Xbox One drawing in its own
 *     shape and its markers drawn from its buttons' outlines (resources
 *     "PSM_LESSON_*" / "XBM_LESSON_*", port/assets/btnicons/pads/make_pads.py,
 *     CC0), positions from the same drawing (pads/ps_lesson.txt, xb_lesson.txt;
 *     L1 / L2 there = LB / LT).
 * mark[]: each marker's corner on the pad, in the rows' order of the title's
 * table 0x1AA6C8 (ctlscheme.c): LT, RT, A, B, X, Y, White, Black, left,
 * right, down, up (L2, R2, cross, circle, square, triangle, L1, R1, ...). */
enum { L_PAD, L_RING, L_L2, L_L1, L_DPAD_V, L_DPAD_H, N_LESSON };
#define LESSON_MARKS 12
typedef struct {
    const char *name;
    const char *res[N_LESSON];
    struct { uint16_t x, y, w, h; } at[N_LESSON];      /* texels, 1x */
    float mark[LESSON_MARKS][2];
    int smooth;                                         /* the pad: see pad_smooth */
} LessonStyle;
static const LessonStyle k_lesson_ps2 = {
    "ps2",
    { "PS2_LESSON_PAD", "PS2_LESSON_RING", "PS2_LESSON_L2", "PS2_LESSON_L1",
      "PS2_LESSON_DPAD_V", "PS2_LESSON_DPAD_H" },
    { { 0, 0, 256, 126 }, { 2, 132, 19, 19 }, { 24, 132, 32, 10 }, { 24, 145, 30, 8 },
      { 60, 132, 15, 19 }, { 78, 132, 19, 15 } },
    { { 34, 0 }, { 185, 0 }, { 194, 82 }, { 216, 64 }, { 177, 65 }, { 198, 46 },
      { 29, 9 }, { 192, 9 }, { 22, 66 }, { 51, 66 }, { 38, 76 }, { 36, 50 } },
    1,
};
static const LessonStyle k_lesson_ps = {
    "playstation",
    { "PSM_LESSON_PAD", "PSM_LESSON_RING", "PSM_LESSON_L2", "PSM_LESSON_L1",
      "PSM_LESSON_DPAD_V", "PSM_LESSON_DPAD_H" },
    { { 0, 0, 240, 152 }, { 0, 154, 21, 21 }, { 59, 154, 27, 11 }, { 23, 154, 34, 14 },
      { 88, 154, 19, 20 }, { 109, 154, 21, 20 } },
    { { 35, -2 }, { 178, -2 }, { 185, 66 }, { 202, 48 }, { 167, 48 }, { 185, 30 },
      { 29, 7 }, { 177, 7 }, { 24, 49 }, { 45, 49 }, { 35, 59 }, { 35, 38 } },
    0,
};
static const LessonStyle k_lesson_xb = {
    "modern",
    { "XBM_LESSON_PAD", "XBM_LESSON_RING", "XBM_LESSON_L2", "XBM_LESSON_L1",
      "XBM_LESSON_DPAD_V", "XBM_LESSON_DPAD_H" },
    { { 0, 0, 206, 155 }, { 0, 157, 21, 20 }, { 80, 157, 22, 15 }, { 23, 157, 55, 23 },
      { 104, 157, 15, 17 }, { 121, 157, 16, 16 } },
    { { 29, -3 }, { 155, -3 }, { 146, 47 }, { 161, 33 }, { 132, 33 }, { 146, 19 },
      { 26, -2 }, { 125, -2 }, { 57, 74 }, { 78, 74 }, { 68, 84 }, { 68, 63 } },
    0,
};
#define LESSON_CLEAR_H 180          /* rows 0-179: the Duke, its markers and unused rings */
static Img g_lesson[N_LESSON];
static const LessonStyle *g_ls;     /* the style's, once loaded (lesson_ready) */

/* The lesson's shapes (the trick HUD's table, written by 0xEF960 on each
 * course or tutorial load): pad 96 and the markers' shapes of the title's
 * table 0x1AA6C8, each with the game's own record (checked before it is
 * changed) and the PS2 picture it shows, mirrored as the PS2 pad needs it
 * (the right shoulders, the D-pad's left and down arms; a mirrored UV runs
 * from its right or bottom edge, as the game's own 125 / 131 / 132 do). */
typedef struct {
    uint16_t shape;
    uint8_t pic, flip_h, flip_v;
    float game[6];                  /* w, h, u0, v0, u1, v1 in texels (texel centres) */
} LessonShape;
static const LessonShape k_lesson_shape[] = {
    {  96, L_PAD,    0, 0, { 219, 169,  0.5f,   2.5f, 218.5f, 170.5f } },
    { 117, L_RING,   0, 0, {  17,  21, 239.5f, 160.5f, 255.5f, 180.5f } },   /* A  = cross */
    { 118, L_RING,   0, 0, {  17,  21, 239.5f, 160.5f, 255.5f, 180.5f } },   /* B  = circle */
    { 119, L_RING,   0, 0, {  17,  21, 239.5f, 160.5f, 255.5f, 180.5f } },   /* X  = square */
    { 120, L_RING,   0, 0, {  17,  21, 239.5f, 160.5f, 255.5f, 180.5f } },   /* Y  = triangle */
    { 123, L_L1,     0, 0, {  12,  17, 221.5f, 130.5f, 232.5f, 146.5f } },   /* White = L1 */
    { 124, L_L1,     1, 0, {  12,  17, 221.5f, 130.5f, 232.5f, 146.5f } },   /* Black = R1 */
    { 125, L_L2,     0, 0, {  18,  19, 237.5f, 162.5f, 220.5f, 180.5f } },   /* LT = L2 */
    { 126, L_L2,     1, 0, {  18,  19, 220.5f, 162.5f, 237.5f, 180.5f } },   /* RT = R2 */
    { 130, L_DPAD_V, 0, 0, {  32,  22, 224.5f, 101.5f, 255.5f, 122.5f } },   /* up */
    { 131, L_DPAD_V, 0, 1, {  32,  22, 224.5f, 122.5f, 255.5f, 101.5f } },   /* down */
    { 132, L_DPAD_H, 1, 0, {  22,  32, 255.5f, 124.5f, 234.5f, 155.5f } },   /* left */
    { 133, L_DPAD_H, 0, 0, {  22,  32, 234.5f, 124.5f, 255.5f, 155.5f } },   /* right */
};
#define N_LESSON_SHAPES (int)(sizeof k_lesson_shape / sizeof k_lesson_shape[0])

int btnicons_on(void)
{
    if (g_mode < 0) {
        const char *e = getenv("XBOX_BUTTON_ICONS");
        g_mode = MODE_OFF;                  /* unset or "0": test runs only */
        if (e && e[0] && strcmp(e, "0")) {
            if (!_stricmp(e, "playstation") || !_stricmp(e, "ps")) g_mode = MODE_PS;
            else if (!_stricmp(e, "ps2")) g_mode = PS2_ORIGINAL_READY ? MODE_PS2 : MODE_MODERN;
            else if (!_stricmp(e, "debug")) g_mode = MODE_DEBUG;
            else g_mode = MODE_MODERN;      /* "modern", "auto", or a value no longer known */
            if (!_stricmp(e, "ps2") && g_mode != MODE_PS2)
                fprintf(stderr, "[BTNICONS] ps2: the PS2 game's buttons are not in yet, auto\n");
        }
        e = getenv("XBOX_BUTTON_ICONS_LOG");
        g_log = e && e[0] == '1';
        if (g_mode != MODE_OFF)
            fprintf(stderr, "[BTNICONS] mode %s\n",
                    g_mode == MODE_MODERN ? "modern" : g_mode == MODE_PS ? "playstation" :
                    g_mode == MODE_PS2 ? "ps2" : "debug");
    }
    return g_mode != MODE_OFF;
}

int btnicons_wide_bumpers(void)
{
    static int s_wide = -1;
    if (s_wide < 0) {               /* XBOX_FIX_BUMPERS_WIDE=0: the Duke glyphs' size (comparisons) */
        const char *e = getenv("XBOX_FIX_BUMPERS_WIDE");
        s_wide = btnicons_on() && (g_mode == MODE_MODERN || g_mode == MODE_PS) && !(e && e[0] == '0');
    }
    return s_wide;
}

int btnicons_bumper_text_dy(void)
{
    /* LB / RB's light letters sit about 1.2 texels lower in their squeezed
     * region than L1 / R1's (T_X_LB / T_P4_L1_Retro, both filling it): one
     * unit higher in the trick text puts them on LT / RT's line (measured on
     * trick 11 at 3440 x 1440: 3 px lower before, L1 R1 already on L2 R2's) */
    return g_mode == MODE_PS ? 2 : 1;
}

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static uint64_t fnv64(const uint8_t *p, uint32_t n)
{
    /* FNV-1a over 8-byte words, then the tail bytes (as nv2a_hdtex.c) */
    uint64_t h = 0xcbf29ce484222325ull, v;
    uint32_t i;
    for (i = 0; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); h = (h ^ v) * 0x100000001b3ull; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* ── Drawings: PNG resources decoded by WIC to straight RGBA ───────────── */
static int decode_png(const void *data, DWORD size, Img *out)
{
    IWICImagingFactory *f = NULL;
    IWICStream *s = NULL;
    IWICBitmapDecoder *d = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    UINT w = 0, h = 0;
    int ok = 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int uninit = SUCCEEDED(hr);

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&f)))
        goto done;
    if (FAILED(IWICImagingFactory_CreateStream(f, &s)) ||
        FAILED(IWICStream_InitializeFromMemory(s, (BYTE *)data, size)) ||
        FAILED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)s, NULL,
                                                          WICDecodeMetadataCacheOnDemand, &d)) ||
        FAILED(IWICBitmapDecoder_GetFrame(d, 0, &fr)) ||
        FAILED(IWICImagingFactory_CreateFormatConverter(f, &cv)) ||
        FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppRGBA,
                                              WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(IWICFormatConverter_GetSize(cv, &w, &h)) || !w || !h || w > 1024 || h > 1024)
        goto done;
    out->rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!out->rgba) goto done;
    if (FAILED(IWICFormatConverter_CopyPixels(cv, NULL, w * 4, w * h * 4, out->rgba))) {
        free(out->rgba);
        out->rgba = NULL;
        goto done;
    }
    out->w = (int)w;
    out->h = (int)h;
    ok = 1;
done:
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (d) IWICBitmapDecoder_Release(d);
    if (s) IWICStream_Release(s);
    if (f) IWICImagingFactory_Release(f);
    if (uninit) CoUninitialize();
    return ok;
}

/* The replay help draws the D-pad disc and the two sticks on a dark
 * background, where the pack's near-black bodies vanish: their dark texels
 * are lightened towards a mid grey (#5F6169); the light letter is kept. */
static void lighten_body(Img *im)
{
    size_t i, n = (size_t)im->w * im->h;
    for (i = 0; i < n; i++) {
        uint8_t *q = im->rgba + i * 4;
        if (q[3] > 40 && q[0] + q[1] + q[2] < 270) {
            q[0] = (uint8_t)(q[0] * 0.4f + 95 * 0.6f + 0.5f);
            q[1] = (uint8_t)(q[1] * 0.4f + 97 * 0.6f + 0.5f);
            q[2] = (uint8_t)(q[2] * 0.4f + 105 * 0.6f + 0.5f);
        }
    }
}

static int load_res(const char *name, Img *out)
{
    HRSRC r = FindResourceA(NULL, name, MAKEINTRESOURCEA(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const void *p = g ? LockResource(g) : NULL;
    return p && decode_png(p, SizeofResource(NULL, r), out);
}

/* LB / RB (L1 / R1) shown wide in the race / trick HUD: their letters are
 * drawn at the triggers' cap height and stroke there (measured on the
 * drawings as the HUD shows them, in units: modern LB cap 8.21 / stroke 1.76
 * against LT RT 7.58-8.00 / 1.38-1.45; PlayStation L1 8.52 / 1.83 against
 * L2 R2 7.31-7.72 / 1.53-1.77; stroke = median width of the letters' runs,
 * measured on the drawings). The drawing is shown smaller by
 * bumper_scale(); the modern letters, a bolder cut than the triggers', are
 * also thinned by about 1.5 pixels of the drawing (g_bump_thin): after, modern
 * 7.63 / 1.43, PlayStation 7.50 / 1.61. */
static Img g_bump_thin[2];                      /* White, Black: modern style only */

static float bumper_scale(void)
{
    return g_mode == MODE_PS ? 0.88f : 0.95f;
}

static int light(const uint8_t *q)              /* the drawings' light label (as icon_anchor) */
{
    return q[3] > 128 && q[0] * 30 + q[1] * 59 + q[2] * 11 > 170 * 100;
}

/* A copy of `im` whose light letters lose about 3/4 of a pixel on each side:
 * an edge pixel of a letter takes 3/4 of the colour of its darker neighbour. */
static void thin_letters(const Img *im, Img *out)
{
    static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
    size_t n = (size_t)im->w * im->h * 4;
    int x, y, k;
    out->rgba = (uint8_t *)malloc(n);
    if (!out->rgba) return;
    memcpy(out->rgba, im->rgba, n);
    out->w = im->w; out->h = im->h;
    for (y = 1; y < im->h - 1; y++)
        for (x = 1; x < im->w - 1; x++) {
            const uint8_t *q = im->rgba + ((size_t)y * im->w + x) * 4;
            if (!light(q)) continue;
            for (k = 0; k < 4; k++) {
                const uint8_t *e = im->rgba + ((size_t)(y + dy[k]) * im->w + x + dx[k]) * 4;
                if (e[3] > 128 && !light(e)) {
                    uint8_t *o = out->rgba + ((size_t)y * im->w + x) * 4;
                    o[0] = (uint8_t)((q[0] + 3 * e[0] + 2) / 4);
                    o[1] = (uint8_t)((q[1] + 3 * e[1] + 2) / 4);
                    o[2] = (uint8_t)((q[2] + 3 * e[2] + 2) / 4);
                    break;
                }
            }
        }
}
/* Configure Controller (options menu): shape 61 of the front end's atlas,
 * texels (0,0)-(218,171), the pad and its legend lines, gets the style's
 * picture (4x, pasted texel for texel; port/assets/btnicons/pads/
 * make_config.py; the PS2 one: port/assets/btnicons/ps2/ps2_config.png). The labels'
 * layout that matches it is ps2legend.c's. */
#define CFG_W 218
#define CFG_H 171
static const char *cfg_res(void)
{
    switch (g_mode) {
    case MODE_PS2: return "PS2_CONFIG";
    case MODE_PS: return "PSM_CONFIG";
    case MODE_MODERN: return "XBM_CONFIG";
    default: return NULL;
    }
}
static Img g_cfg;

static int pad_smooth(void);
static void smooth(Img *im);

static void load_icons(void)
{
    int b, pad = g_mode == MODE_PS ? 1 : 0;
    g_icons_loaded = 1;
    if (g_mode == MODE_DEBUG) return;
    if (cfg_res()) {
        if (!load_res(cfg_res(), &g_cfg) || g_cfg.w != CFG_W * SCALE || g_cfg.h != CFG_H * SCALE) {
            fprintf(stderr, "[BTNICONS] picture %s missing or not %dx%d: Configure Controller keeps the Duke\n",
                    cfg_res(), CFG_W * SCALE, CFG_H * SCALE);
            free(g_cfg.rgba);
            memset(&g_cfg, 0, sizeof g_cfg);
        } else if (g_mode == MODE_PS2 && pad_smooth()) {
            smooth(&g_cfg);             /* sampled nearest neighbour: smoothed as the lesson pad */
        }
    }
    if (g_mode == MODE_PS2) {
        int set;
        char name[32];
        for (set = 0; set < N_SETS; set++)
            for (b = 0; b < B_COUNT; b++) {
                snprintf(name, sizeof name, "%s%s", k_set_name[set], k_res_ps2[b]);
                if (!load_res(name, &g_ps2[set][b]))
                    fprintf(stderr, "[BTNICONS] picture %s missing: that glyph is kept\n", name);
            }
        return;
    }
    for (b = 0; b < B_COUNT; b++) {
        if (!load_res(k_res[pad][b], &g_icon[b]))
            fprintf(stderr, "[BTNICONS] drawing %s missing: that glyph is kept\n", k_res[pad][b]);
        else if (b == B_DPAD || b == B_LSTICK || b == B_RSTICK)
            lighten_body(&g_icon[b]);
    }
    if (g_mode == MODE_MODERN && btnicons_wide_bumpers())
        for (b = 0; b < 2; b++)
            if (g_icon[B_WHITE + b].rgba) thin_letters(&g_icon[B_WHITE + b], &g_bump_thin[b]);
}

/* MODE_PS2: the lesson's pictures, loaded once by whichever thread asks first
 * (the pump thread composing the atlas, the game's thread setting the
 * shapes): both must agree, all of them or none. */
static INIT_ONCE g_lesson_once = INIT_ONCE_STATIC_INIT;
static int g_lesson_ok;

/* The lesson pad only (on: chosen on the in-game captures; XBOX_BUTTON_ICONS_PAD_SMOOTH=0
 * to compare): the atlas is sampled
 * nearest neighbour in this style, so the 4x pad, drawn smaller than
 * its 4 texels per unit (0.75 at 1440 lines), shows stepped edges. Smoothed
 * (a 3 x 3 box over its premultiplied texels) it shows about as a linear
 * filter would, while the markers keep their raw pixels: the sampler is set
 * per texture, not per shape, and one atlas holds both.
 * XBOX_BUTTON_ICONS_PAD_SMOOTH=0 / 1 overrides PAD_SMOOTH_DEFAULT. */
#define PAD_SMOOTH_DEFAULT 1
static int pad_smooth(void)
{
    const char *e = getenv("XBOX_BUTTON_ICONS_PAD_SMOOTH");
    return e && e[0] ? e[0] == '1' : PAD_SMOOTH_DEFAULT;
}

static void smooth(Img *im)
{
    int w = im->w, h = im->h, x, y, i, j, c;
    float *pm = (float *)malloc((size_t)w * h * 4 * sizeof(float));
    if (!pm) return;
    for (i = 0; i < w * h; i++) {                       /* premultiplied: no dark fringe */
        float a = im->rgba[i * 4 + 3] / 255.0f;
        for (c = 0; c < 3; c++) pm[i * 4 + c] = im->rgba[i * 4 + c] * a;
        pm[i * 4 + 3] = im->rgba[i * 4 + 3];
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float acc[4] = { 0, 0, 0, 0 }, a;
            uint8_t *p = im->rgba + ((size_t)y * w + x) * 4;
            int n = 0;
            for (j = y - 1; j <= y + 1; j++)
                for (i = x - 1; i <= x + 1; i++) {
                    if (i < 0 || j < 0 || i >= w || j >= h) continue;
                    for (c = 0; c < 4; c++) acc[c] += pm[((size_t)j * w + i) * 4 + c];
                    n++;
                }
            a = acc[3] / n;
            for (c = 0; c < 3; c++) {
                float v = a > 0.5f ? acc[c] / n / (a / 255.0f) : 0.0f;
                p[c] = (uint8_t)(v > 255.0f ? 255 : v + 0.5f);
            }
            p[3] = (uint8_t)(a + 0.5f);
        }
    free(pm);
}

static BOOL CALLBACK lesson_load(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    const LessonStyle *ls = g_mode == MODE_PS2 ? &k_lesson_ps2 : g_mode == MODE_PS ? &k_lesson_ps :
                            g_mode == MODE_MODERN ? &k_lesson_xb : NULL;
    int i, ok = ls != NULL;
    (void)once; (void)param; (void)ctx;
    for (i = 0; ls && i < N_LESSON; i++)
        if (!load_res(ls->res[i], &g_lesson[i]) ||
            g_lesson[i].w != ls->at[i].w * SCALE || g_lesson[i].h != ls->at[i].h * SCALE) {
            fprintf(stderr, "[BTNICONS] picture %s missing or not %dx%d: the lesson keeps the Duke\n",
                    ls->res[i], ls->at[i].w * SCALE, ls->at[i].h * SCALE);
            ok = 0;
        }
    if (ok && ls->smooth && pad_smooth()) {
        smooth(&g_lesson[L_PAD]);
        fprintf(stderr, "[BTNICONS] %s: lesson pad smoothed\n", ls->name);
    }
    if (ok && g_log) fprintf(stderr, "[BTNICONS] %s: lesson pad and markers loaded\n", ls->name);
    g_ls = ok ? ls : NULL;
    g_lesson_ok = ok;
    return TRUE;
}

/* The lesson's pad and markers of the button style, when it has them. */
static int lesson_ready(void)
{
    if (!btnicons_on() || (g_mode != MODE_PS2 && g_mode != MODE_PS && g_mode != MODE_MODERN)) return 0;
    InitOnceExecuteOnce(&g_lesson_once, lesson_load, NULL, NULL);
    return g_lesson_ok;
}

/* Paste a 4x picture at (x, y) of an atlas composed at `sc` x (1x texels):
 * texel for texel at 4x; at a larger scale (the HUD's 8x), resampled
 * bilinearly with premultiplied alpha. Its transparent pixels stay cleared. */
static void paste(uint8_t *dst, int dw, const Img *im, int x, int y, int sc)
{
    int i, j, c;
    if (sc == SCALE) {
        for (j = 0; j < im->h; j++)
            for (i = 0; i < im->w; i++) {
                const uint8_t *q = im->rgba + ((size_t)j * im->w + i) * 4;
                uint8_t *p = dst + ((size_t)(y * SCALE + j) * dw + x * SCALE + i) * 4;
                if (q[3] == 0) continue;
                p[0] = q[2]; p[1] = q[1]; p[2] = q[0]; p[3] = q[3];
            }
        return;
    }
    for (j = 0; j < im->h * sc / SCALE; j++)
        for (i = 0; i < im->w * sc / SCALE; i++) {
            float u = (i + 0.5f) * SCALE / sc - 0.5f, v = (j + 0.5f) * SCALE / sc - 0.5f, acc[4] = { 0, 0, 0, 0 };
            int x0 = u < 0 ? 0 : (int)u, y0 = v < 0 ? 0 : (int)v, k;
            float fx = u - x0 < 0 ? 0 : u - x0, fy = v - y0 < 0 ? 0 : v - y0;
            uint8_t *p = dst + ((size_t)(y * sc + j) * dw + x * sc + i) * 4;
            for (k = 0; k < 4; k++) {
                int xx = x0 + (k & 1), yy = y0 + (k >> 1);
                float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                const uint8_t *q;
                if (xx >= im->w) xx = im->w - 1;
                if (yy >= im->h) yy = im->h - 1;
                q = im->rgba + ((size_t)yy * im->w + xx) * 4;
                for (c = 0; c < 3; c++) acc[c] += q[c] * (q[3] / 255.0f) * wgt;
                acc[3] += q[3] * wgt;
            }
            if (acc[3] < 0.5f) continue;
            for (c = 0; c < 3; c++) {
                float cv = acc[c] / (acc[3] / 255.0f);
                p[2 - c] = (uint8_t)(cv > 255 ? 255 : cv + 0.5f);
            }
            p[3] = (uint8_t)(acc[3] + 0.5f);
        }
}

/* MODE_PS2: the picture of a button for an atlas. */
static const Img *ps2_icon(int set, int button)
{
    if (g_ps2[set][button].rgba) return &g_ps2[set][button];
    return g_ps2[SET_TRICK][button].rgba ? &g_ps2[SET_TRICK][button] : NULL;
}

/* ── Composition ───────────────────────────────────────────────────────── */

/* Bilinear sample of a BGRA image at (u, v) in texel units, premultiplied out. */
static void sample_bgra(const uint8_t *s, int w, int h, float u, float v, float out[4])
{
    int x0, y0, x1, y1, c;
    float fx, fy;
    u -= 0.5f; v -= 0.5f;
    if (u < 0) u = 0;
    if (v < 0) v = 0;
    x0 = (int)u; y0 = (int)v;
    fx = u - x0; fy = v - y0;
    if (x0 > w - 1) { x0 = w - 1; fx = 0; }
    if (y0 > h - 1) { y0 = h - 1; fy = 0; }
    x1 = x0 + 1 < w ? x0 + 1 : w - 1;
    y1 = y0 + 1 < h ? y0 + 1 : h - 1;
    for (c = 0; c < 4; c++) {
        float a = s[((size_t)y0 * w + x0) * 4 + c], b = s[((size_t)y0 * w + x1) * 4 + c];
        float d = s[((size_t)y1 * w + x0) * 4 + c], e = s[((size_t)y1 * w + x1) * 4 + c];
        out[c] = (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
    }
}

/* The drawing's opaque box (alpha > 8). */
static void icon_box(const Img *im, int *bx, int *by, int *bw, int *bh)
{
    int x, y, x0 = im->w, y0 = im->h, x1 = -1, y1 = -1;
    for (y = 0; y < im->h; y++)
        for (x = 0; x < im->w; x++)
            if (im->rgba[((size_t)y * im->w + x) * 4 + 3] > 8) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) { x0 = y0 = 0; x1 = im->w - 1; y1 = im->h - 1; }
    *bx = x0; *by = y0; *bw = x1 - x0 + 1; *bh = y1 - y0 + 1;
}

/* The point of a drawing that is lined up with the glyph it replaces: the
 * middle of its light label ("LB", "L2" ...) when `label` asks for it and it
 * has one -- a trigger's bump or a bumper's tab must not move the label off
 * the line --, else the middle of its opaque box. */
static void icon_anchor(const Img *im, int label, int bx, int by, int bw, int bh, float *ax, float *ay)
{
    int x, y, x0 = im->w, y0 = im->h, x1 = -1, y1 = -1;
    if (label)
        for (y = 0; y < im->h; y++)
            for (x = 0; x < im->w; x++) {
                const uint8_t *q = im->rgba + ((size_t)y * im->w + x) * 4;
                if (q[3] > 128 && q[0] * 30 + q[1] * 59 + q[2] * 11 > 170 * 100) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
            }
    if (x1 < 0) { *ax = bx + bw * 0.5f; *ay = by + bh * 0.5f; return; }
    *ax = (x0 + x1 + 1) * 0.5f;
    *ay = (y0 + y1 + 1) * 0.5f;
}

/* The middle of the game's glyph body in a slot (alpha > 128, so without its
 * soft shadow), in texels of the guest atlas (BGRA); 0 when there is none. */
static int glyph_body(const uint8_t *src, int w, int h, const Slot *sl, float *cx, float *cy)
{
    int x, y, x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (y = sl->y; y < sl->y + sl->h && y < h; y++)
        for (x = sl->x; x < sl->x + sl->w && x < w; x++)
            if (src[((size_t)y * w + x) * 4 + 3] > 128) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) return 0;
    *cx = (x0 + x1 + 1) * 0.5f;
    *cy = (y0 + y1 + 1) * 0.5f;
    return 1;
}

/* Fit the drawing's opaque box inside the rectangle (aspect kept),
 * area-averaged with premultiplied alpha, over the cleared texels. Centred,
 * or, with tx >= 0, its anchor (icon_anchor) put on (tx, ty) as far as the
 * rectangle allows: the game lines its glyphs up by their bodies.
 * (sqx, sqy) = (1, 1), or the squeeze of a rectangle that the game shows
 * stretched by (1/sqx, 1/sqy): the aspect is then kept on screen. */
static void draw_icon(uint8_t *dst, int dw, const Img *im, int rx, int ry, int rw, int rh,
                      float tx, float ty, int label, float sqx, float sqy)
{
    int bx, by, bw, bh, ox, oy, fw, fh, x, y, i, j;
    const int SS = 4;                               /* samples per axis per texel */
    float k, kx, ky;
    icon_box(im, &bx, &by, &bw, &bh);
    k = (float)rw / (sqx * bw) < (float)rh / (sqy * bh) ? (float)rw / (sqx * bw) : (float)rh / (sqy * bh);
    kx = k * sqx;
    ky = k * sqy;
    fw = (int)(bw * kx + 0.5f); fh = (int)(bh * ky + 0.5f);
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    ox = rx + (rw - fw) / 2; oy = ry + (rh - fh) / 2;
    if (tx >= 0) {
        float ax, ay;
        icon_anchor(im, label, bx, by, bw, bh, &ax, &ay);
        ox = (int)(tx - (ax - bx) * kx + 0.5f);
        oy = (int)(ty - (ay - by) * ky + 0.5f);
        if (ox > rx + rw - fw) ox = rx + rw - fw;
        if (ox < rx) ox = rx;
        if (oy > ry + rh - fh) oy = ry + rh - fh;
        if (oy < ry) oy = ry;
    }
    for (y = 0; y < fh; y++)
        for (x = 0; x < fw; x++) {
            float acc[4] = { 0, 0, 0, 0 };
            uint8_t *p = dst + ((size_t)(oy + y) * dw + ox + x) * 4;
            for (j = 0; j < SS; j++)
                for (i = 0; i < SS; i++) {
                    float u = bx + (x + (i + 0.5f) / SS) / kx, v = by + (y + (j + 0.5f) / SS) / ky;
                    int sx = (int)u, sy = (int)v;
                    const uint8_t *q;
                    float a;
                    if (sx >= im->w) sx = im->w - 1;
                    if (sy >= im->h) sy = im->h - 1;
                    q = im->rgba + ((size_t)sy * im->w + sx) * 4;
                    a = q[3] / 255.0f;
                    acc[0] += q[2] * a; acc[1] += q[1] * a; acc[2] += q[0] * a; acc[3] += q[3];
                }
            acc[3] /= SS * SS;
            if (acc[3] < 0.5f) continue;
            for (i = 0; i < 3; i++) {
                float c = acc[i] / (SS * SS) / (acc[3] / 255.0f);
                p[i] = (uint8_t)(c > 255 ? 255 : c + 0.5f);
            }
            p[3] = (uint8_t)(acc[3] + 0.5f);
        }
}

/* MODE_PS2: the whole picture (the PS2 glyph's box, the size the shape
 * sizes of ctlscheme.c are worked out from), nearest neighbour: its pixels
 * are kept as they are, never blended. fill: it fills the rectangle,
 * stretched on each axis, for the glyphs whose shapes are drawn at the PS2
 * size, which undoes the stretch on screen; else it fits inside, aspect kept
 * and centred. */
static void draw_ps2(uint8_t *dst, int dw, const Img *im, int rx, int ry, int rw, int rh, int fill)
{
    int bx = 0, by = 0, bw = im->w, bh = im->h, ox, oy, fw, fh, x, y;
    float kx, ky;
    kx = (float)rw / bw;
    ky = (float)rh / bh;
    if (!fill) kx = ky = kx < ky ? kx : ky;
    fw = (int)(bw * kx + 0.5f); fh = (int)(bh * ky + 0.5f);
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    if (fw > rw) fw = rw;
    if (fh > rh) fh = rh;
    ox = rx + (rw - fw) / 2; oy = ry + (rh - fh) / 2;
    for (y = 0; y < fh; y++)
        for (x = 0; x < fw; x++) {
            int sx = bx + (int)((x + 0.5f) / kx), sy = by + (int)((y + 0.5f) / ky);
            const uint8_t *q;
            uint8_t *p = dst + ((size_t)(oy + y) * dw + ox + x) * 4;
            if (sx >= bx + bw) sx = bx + bw - 1;
            if (sy >= by + bh) sy = by + bh - 1;
            q = im->rgba + ((size_t)sy * im->w + sx) * 4;
            if (q[3] <= 8) continue;                    /* the cleared texel stays */
            p[0] = q[2]; p[1] = q[1]; p[2] = q[0]; p[3] = q[3];
        }
}

/* MODE_PS2, trick and race HUDs (hud1): the shape that shows each button, as
 * measured in game (UV rectangle in texels, texel centres; the game's shape
 * size is the UV size + 1, the same in hudtrick and hudgame), and the PS2
 * glyph's size. The PS2 picture fills the UV rectangle, stretched on each
 * axis, and ctlscheme.c gives the shape the PS2 size (btnicons_ps2_shape),
 * which undoes the stretch on screen. RT's rectangle starts one texel inside
 * LT's: each keeps clear of that shared texel (cut). */
typedef struct {
    uint16_t shape;
    uint8_t button, cut_l, cut_r, pw, ph;
    float u0, v0, u1, v1;
} Ps2Uv;
static const Ps2Uv k_ps2_uv[] = {
    {  97, B_A,      0, 0, 20, 20,  55.5f, 230.5f,  76.5f, 253.5f },
    {  98, B_B,      0, 0, 20, 20,  79.5f, 230.5f, 100.5f, 253.5f },
    {  99, B_X,      0, 0, 20, 20,  30.5f, 230.5f,  51.5f, 253.5f },
    { 100, B_Y,      0, 0, 20, 20,   4.5f, 230.5f,  25.5f, 253.5f },
    { 101, B_START,  0, 0, 17, 12, 236.5f, 187.5f, 254.5f, 201.5f },
    { 102, B_BACK,   0, 0, 15, 10, 194.5f, 220.5f, 212.5f, 234.5f },
    { 103, B_WHITE,  0, 0, 28, 15, 195.5f, 236.5f, 210.5f, 253.5f },
    { 104, B_BLACK,  0, 0, 28, 15, 219.5f, 189.5f, 234.5f, 207.5f },
    { 105, B_LT,     0, 1, 28, 32, 136.5f, 185.5f, 157.5f, 212.5f },
    { 106, B_RT,     1, 0, 28, 32, 156.5f, 185.5f, 178.5f, 212.5f },
    { 107, B_LSTICK, 0, 0, 34, 35,  96.5f, 180.5f, 131.5f, 215.5f },
    { 108, B_RSTICK, 0, 0, 34, 35,  55.5f, 181.5f,  90.5f, 216.5f },
    { 109, B_DPAD,   0, 0, 36, 37,   6.5f, 180.5f,  50.5f, 223.5f },
    { 110, B_DRIGHT, 0, 0, 14, 13, 140.5f, 224.5f, 165.5f, 255.5f },
    { 111, B_DLEFT,  0, 0, 14, 13, 167.5f, 224.5f, 191.5f, 254.5f },
    { 112, B_DUP,    0, 0, 12, 14, 180.5f, 185.5f, 213.5f, 209.5f },
    { 113, B_DDOWN,  0, 0, 12, 15, 106.5f, 229.5f, 139.5f, 253.5f },
};
#define N_PS2_UV (int)(sizeof k_ps2_uv / sizeof k_ps2_uv[0])

static const Ps2Uv *ps2_uv(const Atlas *a, int button)
{
    int i;
    if (a->slots != k_hud1) return NULL;        /* fe_1, hud: shapes not measured, fitted */
    for (i = 0; i < N_PS2_UV; i++)
        if (k_ps2_uv[i].button == button) return &k_ps2_uv[i];
    return NULL;
}

/* Debug mode: a 3 x 5 digit font, scaled. */
static const uint16_t k_digit[10] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF
};
static void fill(uint8_t *dst, int dw, int x0, int y0, int w, int h, uint32_t bgra)
{
    int x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            memcpy(dst + ((size_t)y * dw + x) * 4, &bgra, 4);
}
static void draw_number(uint8_t *dst, int dw, int n, int rx, int ry, int rw, int rh)
{
    static const uint32_t k_col[] = { 0xFFFF0000u, 0xFF00C000u, 0xFF0060FFu, 0xFFFF00FFu, 0xFFFF8000u,
                                      0xFF00C0C0u, 0xFF8000FFu, 0xFFC0C000u, 0xFFFF0080u, 0xFF00FF80u };
    int digits[2], nd = 0, d, px, s, gx, gy, cx, cy;
    fill(dst, dw, rx, ry, rw, rh, k_col[n % 10]);
    if (n >= 10) digits[nd++] = n / 10;
    digits[nd++] = n % 10;
    s = rh / 6 < rw / (4 * nd) ? rh / 6 : rw / (4 * nd);
    if (s < 1) s = 1;
    cx = rx + (rw - (4 * nd - 1) * s) / 2;
    cy = ry + (rh - 5 * s) / 2;
    for (d = 0; d < nd; d++)
        for (px = 0; px < 15; px++)
            if (k_digit[digits[d]] & (1u << (14 - px))) {
                gx = cx + (d * 4 + px % 3) * s;
                gy = cy + (px / 3) * s;
                fill(dst, dw, gx, gy, s, s, 0xFFFFFFFFu);
            }
}

/* The guest atlas as BGRA rows: 32-bit ones read back from the texture just
 * decoded, DXT3 ones decoded from the guest bytes. */
static uint8_t *guest_bgra(const uint8_t *level0, uint32_t n0, uint32_t w, uint32_t h,
                           IDirect3DTexture8 *guest)
{
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    uint32_t x, y;
    if (!px) return NULL;
    if (n0 == w * h * 4) {
        D3DLOCKED_RECT lr;
        memset(&lr, 0, sizeof lr);
        if (FAILED(guest->lpVtbl->LockRect(guest, 0, &lr, NULL, 0)) || !lr.pBits) { free(px); return NULL; }
        for (y = 0; y < h; y++)
            memcpy(px + (size_t)y * w * 4, (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch, (size_t)w * 4);
        guest->lpVtbl->UnlockRect(guest, 0);
    } else if (n0 == w * h && !(w & 3) && !(h & 3)) {           /* DXT3: 16 bytes per 4 x 4 block */
        const uint8_t *b = level0;
        for (y = 0; y < h; y += 4)
            for (x = 0; x < w; x += 4, b += 16)
                bcdec_bc2(b, px + ((size_t)y * w + x) * 4, (int)w * 4);
        for (x = 0; x < w * h; x++) {                            /* RGBA -> BGRA */
            uint8_t t = px[x * 4]; px[x * 4] = px[x * 4 + 2]; px[x * 4 + 2] = t;
        }
    } else {
        free(px);
        return NULL;
    }
    return px;
}

static void half_level(const uint8_t *s, uint32_t w, uint32_t h, uint8_t *d)
{
    uint32_t W = w > 1 ? w / 2 : 1, H = h > 1 ? h / 2 : 1, x, y, c;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            for (c = 0; c < 4; c++)
                d[((size_t)y * W + x) * 4 + c] = (uint8_t)((s[((size_t)(2 * y) * w + 2 * x) * 4 + c] +
                                                            s[((size_t)(2 * y) * w + 2 * x + 1) * 4 + c] +
                                                            s[((size_t)(2 * y + 1) * w + 2 * x) * 4 + c] +
                                                            s[((size_t)(2 * y + 1) * w + 2 * x + 1) * 4 + c] + 2) / 4);
}

/* The race / trick HUD atlases (hud1) are composed at HUD_SCALE in the
 * modern and PlayStation styles: the bumpers' drawings fill a 14 x 8 texel
 * region there that the game shows 24 x 15 (BTNICONS_BUMPER_*); at 8x they
 * have at least the triggers' texels per unit. XBOX_BUTTON_ICONS_HUD_SCALE=4
 * keeps 4x (comparisons). The other atlases stay at SCALE. */
#define HUD_SCALE 8
static int atlas_scale(const Atlas *a)
{
    static int s_hud = -1;
    if (s_hud < 0) {
        const char *e = getenv("XBOX_BUTTON_ICONS_HUD_SCALE");
        s_hud = e && e[0] == '4' ? SCALE : HUD_SCALE;
    }
    return a->slots == k_hud1 && btnicons_wide_bumpers() ? s_hud : SCALE;
}

static IDirect3DTexture8 *compose(const Atlas *a, const uint8_t *src, uint32_t w, uint32_t h, int sc)
{
    uint32_t W = w * (uint32_t)sc, H = h * (uint32_t)sc, levels = 0, d, i, x, y;
    size_t total = 0, off = 0;
    const void *bits[16];
    uint8_t *pix;
    IDirect3DTexture8 *tex = NULL;
    int s, in;
    float cx, cy;

    for (d = W > H ? W : H; ; d >>= 1) {
        uint32_t lw = W >> levels ? W >> levels : 1, lh = H >> levels ? H >> levels : 1;
        total += (size_t)lw * lh * 4;
        levels++;
        if (d <= 1 || levels == 16) break;
    }
    if (!(pix = (uint8_t *)malloc(total))) return NULL;
    for (y = 0; y < H; y++)                                  /* level 0: the atlas, bilinear sc x */
        for (x = 0; x < W; x++) {
            float c[4];
            int k;
            sample_bgra(src, (int)w, (int)h, (x + 0.5f) / sc, (y + 0.5f) / sc, c);
            for (k = 0; k < 4; k++) pix[((size_t)y * W + x) * 4 + k] = (uint8_t)(c[k] + 0.5f);
        }
    if (a->slots == k_fe1 && g_cfg.rgba) {
        /* Configure Controller: the Duke and its lines out, the style's pad in */
        fill(pix, (int)W, 0, 0, CFG_W * sc, CFG_H * sc, 0x00000000u);
        paste(pix, (int)W, &g_cfg, 0, 0, sc);
    }
    if (a->set == SET_TRICK && a->slots == k_hud1 && lesson_ready()) {
        /* the lesson: Duke and markers out, the PS2 pad and markers in (the
         * rows' right end below them: the Duke's ring and arrow markers) */
        fill(pix, (int)W, 0, 0, (int)W, LESSON_CLEAR_H * sc, 0x00000000u);
        fill(pix, (int)W, 216 * sc, LESSON_CLEAR_H * sc, 40 * sc, 2 * sc, 0x00000000u);
        for (i = 0; i < N_LESSON; i++)
            paste(pix, (int)W, &g_lesson[i], g_ls->at[i].x, g_ls->at[i].y, sc);
    }
    for (s = 0; s < a->n; s++) {
        const Slot *sl = &a->slots[s];
        int x0 = (sl->x - MARGIN) * sc, y0 = (sl->y - MARGIN) * sc;
        int cw = (sl->w + 2 * MARGIN) * sc, ch = (sl->h + 2 * MARGIN) * sc;
        if (x0 < 0) { cw += x0; x0 = 0; }
        if (y0 < 0) { ch += y0; y0 = 0; }
        if (x0 + cw > (int)W) cw = (int)W - x0;
        if (y0 + ch > (int)H) ch = (int)H - y0;
        if (g_mode == MODE_DEBUG) {
            draw_number(pix, (int)W, s, sl->x * sc, sl->y * sc, sl->w * sc, sl->h * sc);
            continue;
        }
        if (g_mode == MODE_PS2) {                            /* cleared here, drawn below */
            const Ps2Uv *u = ps2_uv(a, sl->button);
            if (!ps2_icon(a->set, sl->button)) continue;     /* picture missing: keep the glyph */
            fill(pix, (int)W, x0, y0, cw, ch, 0x00000000u);
            if (u)
                fill(pix, (int)W, (int)(u->u0 * sc), (int)(u->v0 * sc),
                     (int)((u->u1 - u->u0) * sc), (int)((u->v1 - u->v0) * sc), 0x00000000u);
            continue;
        }
        if (!g_icon[sl->button].rgba) continue;              /* drawing missing: keep the glyph */
        fill(pix, (int)W, x0, y0, cw, ch, 0x00000000u);
        in = slot_inset(sl->button);
        if (a->slots == k_hud1 && (sl->button == B_WHITE || sl->button == B_BLACK) && btnicons_wide_bumpers()) {
            /* shapes 103 / 104, which the game then shows wider (ctlscheme.c):
             * the region's texels are squeezed to show the drawing unstretched,
             * at the triggers' letter size (bumper_scale, g_bump_thin), centred */
            const Img *bi = g_bump_thin[sl->button - B_WHITE].rgba ? &g_bump_thin[sl->button - B_WHITE] : &g_icon[sl->button];
            int rh = BTNICONS_BUMPER_RH * sc, rhf = (int)(rh * bumper_scale() + 0.5f);
            draw_icon(pix, (int)W, bi, sl->x * sc, (sl->y + 1) * sc + (rh - rhf) / 2,
                      BTNICONS_BUMPER_RW * sc, rhf, -1.0f, 0.0f, 0,
                      (float)BTNICONS_BUMPER_RW / BTNICONS_BUMPER_W, (float)BTNICONS_BUMPER_RH / BTNICONS_BUMPER_H);
            continue;
        }
        /* buttons and triggers sit on the line of the game's glyph bodies; the
         * D-pad and sticks are whole pads, centred */
        if (sl->button > B_START || !glyph_body(src, (int)w, (int)h, sl, &cx, &cy)) cx = cy = -1.0f;
        else if (a->slots == k_fe1 && sl->button <= B_Y)   /* menus: faces on the text line (FE1_FACES_DY) */
            cy = sl->y + in + (sl->h - 2 * in) * 0.5f + FE1_FACES_DY;
        draw_icon(pix, (int)W, &g_icon[sl->button], (sl->x + in) * sc, (sl->y + in) * sc,
                  (sl->w - 2 * in - sl->trim_r) * sc, (sl->h - 2 * in) * sc,
                  cx < 0 ? -1.0f : cx * sc, cy * sc,
                  sl->button == B_WHITE || sl->button == B_BLACK || sl->button == B_LT || sl->button == B_RT,
                  1.0f, 1.0f);
    }
    /* MODE_PS2: drawn once every rectangle is cleared (a shape's UV can reach
     * past its glyph's rectangle): the shape's UV rectangle filled when its
     * size is known, else fitted in the glyph's rectangle. */
    for (s = 0; g_mode == MODE_PS2 && s < a->n; s++) {
        const Slot *sl = &a->slots[s];
        const Img *im = ps2_icon(a->set, sl->button);
        const Ps2Uv *u = ps2_uv(a, sl->button);
        if (!im) continue;
        if (u) {
            int fx0 = (int)((u->u0 + u->cut_l) * sc), fx1 = (int)((u->u1 - u->cut_r) * sc);
            int fy0 = (int)(u->v0 * sc), fy1 = (int)(u->v1 * sc);
            draw_ps2(pix, (int)W, im, fx0, fy0, fx1 - fx0, fy1 - fy0, 1);
        } else {
            int dy = a->slots == k_fe1 && sl->button <= B_Y ? FE1_FACES_DY : 0;  /* menus: on the text line */
            in = slot_inset(sl->button);
            draw_ps2(pix, (int)W, im, (sl->x + in) * sc, (sl->y + in + dy) * sc,
                     (sl->w - 2 * in - sl->trim_r) * sc, (sl->h - 2 * in) * sc, 0);
        }
    }
    bits[0] = pix;
    for (i = 1; i < levels; i++) {
        uint32_t pw = W >> (i - 1) ? W >> (i - 1) : 1, ph = H >> (i - 1) ? H >> (i - 1) : 1;
        off += (size_t)pw * ph * 4;
        half_level((const uint8_t *)bits[i - 1], pw, ph, pix + off);
        bits[i] = pix + off;
    }
    if (FAILED(d3d8_CreateTextureFromLevels(W, H, levels, bits, &tex))) tex = NULL;
    free(pix);
    return tex;
}

IDirect3DTexture8 *btnicons_compose(const uint8_t *level0, uint32_t n0, uint32_t w, uint32_t h,
                                    uint32_t color, IDirect3DTexture8 *guest)
{
    uint64_t key;
    int i;
    uint8_t *src;
    double t0;
    int sc;
    (void)color;
    if (!level0 || !guest || w != 256 || h != 256 || !btnicons_on()) return NULL;
    key = fnv64(level0, n0);
    for (i = 0; i < (int)N_ATLAS; i++)
        if (k_atlas[i].key == key) break;
    if (i == (int)N_ATLAS) return NULL;
    if (!g_done[i]) {
        t0 = now_ms();
        if (!g_icons_loaded) load_icons();
        src = guest_bgra(level0, n0, w, h, guest);
        if (!src) return NULL;
        sc = atlas_scale(&k_atlas[i]);
        g_done[i] = compose(&k_atlas[i], src, w, h, sc);
        if (!g_done[i] && sc != SCALE) {                     /* the larger texture refused: back to 4x */
            fprintf(stderr, "[BTNICONS] %s: %dx refused, composed at %dx\n", k_atlas[i].name, sc, SCALE);
            sc = SCALE;
            g_done[i] = compose(&k_atlas[i], src, w, h, sc);
        }
        free(src);
        if (!g_done[i]) return NULL;
        fprintf(stderr, "[BTNICONS] %s composed (key %016llx) at %dx (%ux%u, %.1f MB with mips) in %.1f ms\n",
                k_atlas[i].name, (unsigned long long)key, sc, w * (uint32_t)sc, h * (uint32_t)sc,
                (double)w * sc * h * sc * 4.0 * 4.0 / 3.0 / (1024.0 * 1024.0), now_ms() - t0);
    } else if (g_log) {
        fprintf(stderr, "[BTNICONS] %s re-uploaded: composed copy reused\n", k_atlas[i].name);
    }
    g_done[i]->lpVtbl->AddRef(g_done[i]);
    return g_done[i];
}

int btnicons_ps2(void)
{
    return btnicons_on() && g_mode == MODE_PS2;
}

int btnicons_point(const IDirect3DTexture8 *t)
{
    int i;
    if (g_mode != MODE_PS2 || !t) return 0;
    for (i = 0; i < (int)N_ATLAS; i++)
        if (g_done[i] == t) return 1;
    return 0;
}

int btnicons_cfg_style(void)
{
    const char *r;
    if (!btnicons_on() || !(r = cfg_res())) return 0;
    if (!FindResourceA(NULL, r, MAKEINTRESOURCEA(10))) return 0;
    return g_mode == MODE_PS2 ? 1 : g_mode == MODE_PS ? 2 : 3;
}

int btnicons_lesson_mark(int row, float *dx, float *dy)
{
    if (row < 0 || row >= LESSON_MARKS || !lesson_ready()) return 0;
    *dx = g_ls->mark[row][0];
    *dy = g_ls->mark[row][1];
    return 1;
}

int btnicons_lesson_shape(int i, unsigned *shape, float game[6], float ps2[6])
{
    const LessonShape *l;
    float x0, y0, x1, y1, t;
    if (i < 0 || i >= N_LESSON_SHAPES || !lesson_ready()) return 0;
    l = &k_lesson_shape[i];
    *shape = l->shape;
    /* record order {w, h, v0, u0, u1, v1}, UV normalised (atlas 256) */
    game[0] = l->game[0];
    game[1] = l->game[1];
    game[2] = l->game[3] / 256.0f;
    game[3] = l->game[2] / 256.0f;
    game[4] = l->game[4] / 256.0f;
    game[5] = l->game[5] / 256.0f;
    /* the picture's texels, centre to centre; the shape's size is the UV's
     * + 1 (the game's rule), so one texel is one unit, as on the PS2 */
    x0 = g_ls->at[l->pic].x + 0.5f;
    y0 = g_ls->at[l->pic].y + 0.5f;
    x1 = x0 + g_ls->at[l->pic].w - 1.0f;
    y1 = y0 + g_ls->at[l->pic].h - 1.0f;
    if (l->flip_h) { t = x0; x0 = x1; x1 = t; }
    if (l->flip_v) { t = y0; y0 = y1; y1 = t; }
    ps2[0] = (float)g_ls->at[l->pic].w;
    ps2[1] = (float)g_ls->at[l->pic].h;
    ps2[2] = y0 / 256.0f;
    ps2[3] = x0 / 256.0f;
    ps2[4] = x1 / 256.0f;
    ps2[5] = y1 / 256.0f;
    return 1;
}

int btnicons_ps2_shape(int i, unsigned *shape, float uv[4], float game_wh[2], float ps2_wh[2])
{
    const Ps2Uv *u;
    float uw, uh;
    if (i < 0 || i >= N_PS2_UV) return 0;
    u = &k_ps2_uv[i];
    uw = u->u1 - u->u0;
    uh = u->v1 - u->v0;
    *shape = u->shape;
    uv[0] = u->u0; uv[1] = u->v0; uv[2] = u->u1; uv[3] = u->v1;
    game_wh[0] = uw + 1.0f;                 /* the game's own size: UV + 1 */
    game_wh[1] = uh + 1.0f;
    /* the PS2 glyph (pw texels) fills uw - cuts of the uw texels the shape shows */
    ps2_wh[0] = (u->pw + 1.0f) * uw / (uw - u->cut_l - u->cut_r);
    ps2_wh[1] = (u->ph + 1.0f);
    return 1;
}
