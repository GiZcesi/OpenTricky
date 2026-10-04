/*
 * discart.h -- the launcher's pictures and lettering, read from the player's
 * own disc image each time the launcher opens.
 *
 * Nothing of the game is stored in the executable: the logo, the front-end
 * backdrop, the selection bar, the riders' and tracks' loading cards
 * and the title font are read from the .iso and decoded here:
 *   data/textures/loading.xsh   logo (DXT3)
 *   data/textures/fe_1.xsh      backdrop, selection bar (32-bit)
 *   data/textures/xboxload.big  loading cards (c0fb archive, RefPack)
 *   data/fonts/title.ffn        the title font (4-bit atlas)
 *   data/audio/audio.big        the menus' sounds (zbxfe.bnk, EA-XA)
 * Formats: SHPX pictures, FFN fonts and BIG archives (the c0fb TOC holds a 24-bit big-endian
 * offset and size per entry; checked on the disc).
 */
#ifndef SSX_DISCART_H
#define SSX_DISCART_H

#include <windows.h>
#include <stdint.h>

/* 32-bit pixels 0xAARRGGBB, straight alpha, top-down. */
typedef struct {
    int       w, h;
    uint32_t *px;
} ArtImage;

typedef struct {
    uint8_t w, h;           /* glyph box in the atlas */
    uint16_t x, y;
    uint8_t adv;            /* pen advance */
    int8_t  ox, oy;         /* offset of the box from the pen */
} ArtGlyph;

typedef struct {
    int      w, h;          /* atlas */
    uint8_t *cov;           /* coverage 0..255 */
    ArtGlyph g[128];        /* by ASCII code; adv == 0: missing */
    int      line;          /* tallest glyph */
} ArtFont;

enum { ART_RIDERS = 12, ART_TRACKS = 10 };

/* A front-end sound, 16-bit PCM, channels interleaved. */
typedef struct {
    int      rate, channels, frames;
    int16_t *pcm;
} ArtSound;
enum { ART_SND_MOVE, ART_SND_CHANGE, ART_SND_SELECT, ART_SND_BACK, ART_SND_COUNT };

typedef struct {
    BOOL     ok;                       /* everything below is there */
    ArtImage logo;                     /* "SSX Tricky" */
    ArtImage backdrop;                 /* orange front-end background */
    ArtImage selbar;                   /* yellow selection swoosh */
    ArtImage rider[ART_RIDERS];        /* loading cards, with the rider's name, */
    ArtImage track[ART_TRACKS];        /* in the game's own 16:5 shape (256 x 80) */
    ArtFont  title;
    BOOL     sounds_ok;                 /* the front end's sounds below */
    ArtSound sound[ART_SND_COUNT];
} DiscArt;

/* Read and decode everything from `iso` (mounts it, then unmounts). FALSE,
 * with nothing allocated, if any piece is missing or does not decode. */
BOOL discart_load(const char *iso, DiscArt *art);
void discart_free(DiscArt *art);

void artimage_free(ArtImage *im);

#endif /* SSX_DISCART_H */
