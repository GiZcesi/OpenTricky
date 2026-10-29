/* HD texture replacement (community texture pack, DDS files).
 *
 * Off unless XBOX_HD_TEXTURES names the pack folder or XBOX_HD_HUD is on:
 * otherwise nothing here is called and the translator's texture path is the
 * original one.
 *
 * On a texture-cache miss the translator hands over the guest texture's
 * level-0 bytes; their FNV-1a 64 key is looked up in an index
 * ("<key> <w>x<h> <factors> <3d|menu|hud|font> <file.dds>" per line, built by
 * port/tools/hdtex_index.py). A hit decodes the DDS (BC1/2/3/7 or 32-bit,
 * with its mips) to BGRA8, scales the colours back from the PS2 range (per
 * texture, from the index: "22" = RGB x2 alpha x2, "12" = RGB x1 alpha x2,
 * "1.28:2" = RGB x1.28 alpha x2) into a new immutable texture, which the
 * texture cache then binds instead of the guest one.
 *
 * Variables:
 *   XBOX_HD_HUD=1               the race HUD from the HD HUD the game ships
 *                               (hd-hud beside the executable; or a folder),
 *                               at x2, with or without a pack, which then
 *                               never gives its own HUD files;
 *                               0 / empty = off (nv2a_hdtex.c, "The shipped
 *                               HD HUD")
 *   XBOX_HD_TEXTURES=<dir>      the pack folder (or a folder above its
 *                               "replacements" folder); empty = off
 *   XBOX_HD_TEXTURES_MENUS=1    also the menu / interface pictures (default 0)
 *   XBOX_HD_TEXTURES_HUD=0      not the race HUD atlases (default 1: with the
 *                               pack, the HUD's own atlases are HD too; their
 *                               strips one texel wide keep the game's colour;
 *                               and the HUD's font, rebuilt from the pack's
 *                               glyphs in the game's layout: "glyph" lines)
 *   XBOX_HD_TEXTURES_HUD=only   the race HUD atlases and the font only: the
 *                               rest of the pack (3D, menus) stays the game's
 *   XBOX_HD_FONT_SOFT=<n>       edge softening of the fonts the index softens
 *                               ("fontsoft" lines; none at present), n passes at x2;
 *                               0 = as their HD files draw them
 *   XBOX_HD_HUD_SCALE=4         HUD atlases and fonts at the pack's own size
 *                               (x4 the game's; default 2: half of it)
 *   XBOX_HD_TEXTURES_INDEX=<f>  index file instead of <dir>/hdtex_index.txt
 *                               or the one built into the game
 *   XBOX_HD_TEXTURES_MB=<n>     GPU memory for HD textures (default 1024);
 *                               the least recently used ones go above it
 *   XBOX_HD_LOG=1               one line per replacement (2: and per miss)
 */
#ifndef NV2A_HDTEX_H
#define NV2A_HDTEX_H

#include <stddef.h>
#include <stdint.h>
#include "../d3d/d3d8_xbox.h"

/* The index the game carries (port/assets/hdtex_index.txt, a resource of the
 * executable), used when no index file is given. Set before the first frame;
 * the text (len bytes, not NUL-terminated) must stay alive. */
void hdtex_set_builtin_index(const char *text, size_t len);

/* The key of a guest texture: FNV-1a 64 of its level-0 bytes as the game
 * holds them (the index's key; also the light textures of the selective
 * bloom, nv2a_pgraph_d3d11.c). */
uint64_t hdtex_key(const uint8_t *level0, uint32_t n0);

/* 1 when replacement is on (reads the variables and the index once). */
int hdtex_on(void);

/* Loading an HD texture: a worker thread reads and decodes it while the
 * game's own texture is drawn (XBOX_HD_TEXTURES_SYNC=1: at once, on the
 * calling thread; XBOX_HD_TEXTURES_THREADS=<n> workers, default a quarter
 * of the logical processors). */
typedef struct HdJob HdJob;

/* After a texture-cache miss: a job for the HD texture of these guest level-0
 * bytes of a w x h texture of `levels` mip levels, or NULL when there is
 * none. The HD texture gets no level smaller than the guest's smallest one
 * (see build). The bytes are only read during the call. *gate: -1, or, with
 * the per-image gate on, HDTEX_GATE_FONT for a font and HDTEX_GATE_HUD for a
 * HUD atlas: the cache then keeps the title's texture beside the HD one and
 * binds the one hdtex_gate_now says, image by image. */
HdJob *hdtex_request(const uint8_t *level0, uint32_t n0, uint32_t w, uint32_t h, uint32_t levels, int *gate);

/* The per-image gate of the HUD's HD pictures. The game says, in the
 * pushbuffer at the start of each image (a NOP marker, see
 * nv2a_pgraph_d3d11.h PGRAPH_HDGATE_TAG_MAGIC), whether that image shows its
 * fonts and its HUD atlases HD: bit HDTEX_GATE_FONT, bit HDTEX_GATE_HUD.
 * The translator hands the bits over as it reaches the marker, so each image
 * is drawn with its own answer. hdtex_set_frame_gate(1) turns it on (the
 * game, before its first image; until the first marker: atlases HD, fonts
 * not); off, everything is HD. hdtex_configured: XBOX_HD_TEXTURES or
 * XBOX_HD_HUD set (nothing read). Pump thread, except the two set-up calls. */
enum { HDTEX_GATE_FONT = 0, HDTEX_GATE_HUD = 1 };
int  hdtex_configured(void);
void hdtex_set_frame_gate(int on);
void hdtex_frame_gate(unsigned bits);
int  hdtex_gate_now(int kind);

/* 0 = not ready yet. 1 = finished and the job is gone: *tex is the HD texture
 * (the caller's reference, NULL if it could not be loaded) and *gpu_bytes its
 * size in GPU memory. */
int hdtex_collect(HdJob *job, IDirect3DTexture8 **tex, uint32_t *gpu_bytes);

/* The texture went away before its HD copy was collected. */
void hdtex_cancel(HdJob *job);

/* GPU memory budget for live HD textures, in bytes. */
uint64_t hdtex_budget(void);

/* Bookkeeping from the texture cache: an HD texture of `bytes` was dropped. */
void hdtex_note_evicted(uint32_t bytes, int over_budget);

#endif
