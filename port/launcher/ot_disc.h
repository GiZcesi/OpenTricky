/*
 * ot_disc.h -- the launcher's pictures, lettering and sounds, read from the
 * player's own disc image at run time.
 *
 * Every picture, glyph and sound below is read from the .iso and decoded
 * here, each time.
 *   data/textures/xboxload.big  tracks' loading cards (c0fb archive, RefPack, SHPX)
 *   data/fonts/title.ffn        the title font (FNTF, 4-bit atlas)
 *   data/audio/audio.big        the menus' sounds (BIGF > zbxfe.bnk, EA-XA R2)
 *   data/audio/music.big        the title screen's music (BIGF > ssxmenu.mus, EA-XA R1)
 *   default.xbe                 title ID and region, to tell the right disc
 * The formats are described where each decoder is (ot_disc.c). These are
 * the decoders of the launcher once built into the game, rewritten without
 * any dependency on the game or on a platform API (portable C99; file names
 * are UTF-8).
 */
#ifndef OT_DISC_H
#define OT_DISC_H

#include <stdbool.h>
#include <stdint.h>

#include "ot_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ten tracks, in the order the game's files sort them. */
typedef enum {
    OT_TRACK_ALAS, OT_TRACK_ALOH, OT_TRACK_ELYS, OT_TRACK_GARI, OT_TRACK_MERQ,
    OT_TRACK_MESA, OT_TRACK_PIPE, OT_TRACK_SNOW, OT_TRACK_TOKY, OT_TRACK_UNTR,
    OT_TRACK_COUNT
} OtTrack;

const char *ot_track_id(int track);     /* "alas" ... (file-name stem) */
const char *ot_track_name(int track);   /* "ALASKA" ... */

/* ── Disc check (the launcher's disc states) ───────────────────────── */

typedef enum {
    OT_DISC_OK,            /* SSX Tricky (USA) for Xbox */
    OT_DISC_NO_PATH,       /* no disc image chosen yet */
    OT_DISC_NOT_FOUND,     /* the file is gone (moved, drive unplugged) */
    OT_DISC_WRONG,         /* not an Xbox disc image, or another game / region */
    OT_DISC_INCOMPLETE     /* SSX Tricky (USA), but the file ends before the disc's last file does */
} OtDiscStatus;

typedef struct {
    OtDiscStatus status;
    bool         xbox;           /* an Xbox (XDVDFS) disc image */
    uint32_t     title_id;       /* from default.xbe (0 if unread) */
    uint32_t     region;         /* XBE game region flags */
    char         title[48];      /* XBE title name, ASCII */
} OtDiscCheck;

/* SSX Tricky's title ID in default.xbe's certificate ("EA" 0x4541, game 4;
 * read on the USA disc). */
#define OT_SSX_TRICKY_USA_TITLE_ID 0x45410004u
/* The entry point of the USA disc's default.xbe, the one SSX Tricky.exe was
 * recompiled from (YOUR_GAME_ENTRY_POINT in port/src/main.c): the game
 * refuses any other, so the launcher checks it too (ot_disc_check_entry). */
#define OT_SSX_TRICKY_USA_ENTRY 0x00154218u

/* Look at `iso` without decoding anything heavy (reads a few kilobytes).
 * OK needs the game's title ID and the North America region flag; with
 * `expected_entry` (the entry point the game executable was recompiled
 * from) the XBE's entry point must match it instead, as the game checks. */
OtDiscCheck ot_disc_check(const char *iso);
OtDiscCheck ot_disc_check_entry(const char *iso, uint32_t expected_entry);

/* ── Decoded assets ──────────────────────────────────────────────── */

typedef struct {
    uint8_t  w, h;            /* glyph box in the atlas */
    uint16_t x, y;
    uint8_t  adv;             /* pen advance */
    int8_t   ox, oy;          /* offset of the box from the pen */
} OtGlyph;

typedef struct {
    int      w, h;            /* atlas */
    uint8_t *cov;             /* coverage 0..255 */
    OtGlyph  g[128];          /* by ASCII code; adv == 0: missing */
    int      line;            /* tallest glyph */
} OtFont;

/* 16-bit PCM, channels interleaved. */
typedef struct {
    int      rate, channels, frames;
    int16_t *pcm;
} OtSound;

enum { OT_SND_MOVE, OT_SND_CHANGE, OT_SND_SELECT, OT_SND_BACK, OT_SND_COUNT };

typedef struct {
    bool    ok;                         /* cards and font are there */
    OtImage card_tex[OT_TRACK_COUNT];   /* loading cards as stored: 256 x 256 */
    OtImage card[OT_TRACK_COUNT];       /* in the game's own 16:5 shape (256 x 80) */
    OtImage emblem[OT_TRACK_COUNT];     /* the track's round emblem, 240 x 240 */
    OtFont  title;
    bool    sounds_ok;                  /* the front end's sounds below */
    OtSound sound[OT_SND_COUNT];
    bool    music_ok;                   /* the title screen's music loop below */
    OtSound music;
} OtDiscAssets;

/* Read and decode everything from `iso`. On failure, `ok` is false and
 * nothing is kept (sounds and music are optional: silent without). */
bool ot_disc_load(const char *iso, OtDiscAssets *a);
void ot_disc_free(OtDiscAssets *a);

/* The round emblem (240 x 240, as `emblem` above) cut from any picture of a
 * track card: the stored 256 x 256 texture or a card already in its 16:5
 * shape at any size (the launcher's HD cards). */
bool ot_card_emblem(const OtImage *card, OtImage *out);

/* Any FNTF font of the disc (e.g. "data/fonts/menu.ffn"); free it with
 * ot_font_free. */
bool ot_disc_font(const char *iso, const char *path, OtFont *f);
void ot_font_free(OtFont *f);

/* Text width in pixels with the title font (ASCII; others skipped). */
int ot_font_text_width(const OtFont *f, const char *s);

/* ── Raw files, for other readers ───────────────────────────────────── */

typedef struct OtDiscFile OtDiscFile;

OtDiscFile *ot_disc_open(const char *iso);          /* NULL: not an Xbox disc image */
void ot_disc_close(OtDiscFile *f);
/* A whole file (at most 64 MB), caller frees. */
uint8_t *ot_disc_read(OtDiscFile *f, const char *path, uint32_t *len);
/* One entry of an EA archive (BIGF or c0fb) by its full name, e.g.
 * ("data/char/texxbx.big", "data/char/eddie_head.xsh"), unpacked; caller frees. */
uint8_t *ot_disc_archive_entry(OtDiscFile *f, const char *archive, const char *name, uint32_t *len);
/* One picture of a SHPX texture bank: the first, or the one named `name`
 * (4 characters). */
bool ot_shpx_decode(const uint8_t *d, uint32_t n, const char *name, OtImage *out);

#ifdef __cplusplus
}
#endif

#endif /* OT_DISC_H */
