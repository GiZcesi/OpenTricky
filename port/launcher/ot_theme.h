/*
 * ot_theme.h -- the launcher's look for each track: accent colours, the
 * background gradient, the track card drawn as a clean badge, and the plain
 * blue look used before a disc image is chosen.
 *
 * All colours are 0xAARRGGBB. Nothing here comes from the game except what
 * ot_disc.h reads from the player's disc image (and the player's own texture
 * pack, if any).
 */
#ifndef OT_THEME_H
#define OT_THEME_H

#include <stdbool.h>
#include <stdint.h>

#include "ot_disc.h"
#include "ot_image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t acc;        /* accent: PLAY, "Tricky" of the logo, small titles */
    uint32_t acc_dark;   /* PLAY's gradient, dark end */
    uint32_t acc_light;  /* PLAY's gradient, light end; small headings */
    uint32_t glow;       /* accent at 40 % alpha: PLAY's glow, the emblem's shadow */
    uint32_t play_text;  /* PLAY's lettering: white, or a dark tone on light accents */
    uint32_t grad[5];    /* background gradient, left (very dark) to right (bright) */
    float    grad_pos[5];/* its stops, 0..1 (tracks: 0, .30, .62, .88, 1) */
    float    grad_angle; /* CSS-style degrees: 90 = to the right (tracks: 120) */
    bool     from_disc;  /* false: the default blue look (no disc image) */
} OtTheme;

/* The look of a track. The tuned palette (the colours shown and approved on
 * the launcher's mock-ups) when `tuned` is true, else measured from the
 * disc's emblem and card with ot_theme_measure(). */
void ot_theme_for_track(const OtDiscAssets *a, int track, bool tuned, OtTheme *t);

/* Measure a theme from the disc: the accent is the emblem's dominant vivid
 * colour, the background the two main hues of the card (dark first one,
 * bright second one). */
bool ot_theme_measure(const OtDiscAssets *a, int track, OtTheme *t);

/* The default look, without a disc image: blue, the "OT" badge, a snow dot
 * pattern instead of the carpet of track cards. */
void ot_theme_default(OtTheme *t);

/* The background gradient at w x h. With the
 * default look, a soft glow at the top right and the snow dot pattern. */
bool ot_theme_background(const OtTheme *t, int w, int h, OtImage *out);

/* ── Track cards as badges ─────────────────────────────────────────────
 * The card's picture fills w x h (16:5), cut by a smooth rounded rectangle,
 * inside a rim we draw ourselves in a fixed grey, #c5c5c3 (the card's name
 * tab; fixed, not measured). Proportions of the
 * approved mock-up at 1040 x 325: inset 9, radius 92, rim 13. */
#define OT_BADGE_RIM_DEFAULT 0xFFC5C5C3u

/* From the disc's stored card (256 x 256): a light blur (0.8 texel), then
 * scaled up -- no sharpening. */
bool ot_badge_from_disc(const OtDiscAssets *a, int track, int w, OtImage *out);

/* From any picture of the card (a pack's 1024 x 1024 one), as is. */
bool ot_badge_make(const OtImage *card, float blur_sigma, int w, OtImage *out);

/* The grey measured on a card picture at the badge's size (not used for the
 * rim any more; kept for tools). */
uint32_t ot_badge_rim_colour(const OtImage *card_wh);

/* The "OT" badge's square background (blue gradient, rounded corners
 * 13/46 of the side); the lettering is drawn by the interface. */
bool ot_badge_ot(int size, OtImage *out);

/* ── HD track cards from the player's texture pack ─────────────────────
 * The pack names its files by hash; the ten loading cards share one name
 * suffix (ot_theme.c) and are 1024 x 1024, BC7. Each one is
 * matched to a track by likeness with the disc's card (so no hash is
 * hard-coded and any pack made from the same disc works). The pack keeps
 * the PlayStation 2's alpha (0x80 = opaque): it is doubled.
 * `pack_dir` may be the replacements folder or any folder above it (searched
 * 4 levels deep). Returns the number of tracks found; hd[t].px == NULL for
 * the others. `score` (optional): the match distance per track (0..255). */
int ot_hdpack_cards(const char *pack_dir, const OtDiscAssets *a, OtImage hd[OT_TRACK_COUNT],
                    float score[OT_TRACK_COUNT]);

/* The ten HD cards shipped with the launcher (ui/cards/<track id>.png)
 * are such pack pictures already in the card's 16:5
 * shape at the badge's size, so a badge made from them at 1040 is the same
 * as one made from the pack. They are only shown once a verified disc image
 * is loaded. */
#define OT_CARD_HD_W 1040
#define OT_CARD_HD_H 325

#ifdef __cplusplus
}
#endif

#endif /* OT_THEME_H */
