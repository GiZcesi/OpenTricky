/* HD texture replacement (community texture pack, DDS files).
 *
 * Off unless XBOX_HD_TEXTURES names the pack folder: then nothing here is
 * called and the translator's texture path is the original one.
 *
 * On a texture-cache miss the translator hands over the guest texture's
 * level-0 bytes; their FNV-1a 64 key is looked up in an index
 * ("<key> <w>x<h> <rgb><alpha> <3d|menu> <file.dds>" per line, built by
 * port/tools/hdtex_index.py). A hit decodes the DDS (BC1/2/3/7 or 32-bit,
 * with its mips) to BGRA8, scales the colours back from the PS2 range (x1 or
 * x2 per channel group, from the index) into a new immutable texture, which
 * the texture cache then binds instead of the guest one.
 *
 * Variables:
 *   XBOX_HD_TEXTURES=<dir>      the pack folder (or a folder above its
 *                               "replacements" folder); empty = off
 *   XBOX_HD_TEXTURES_MENUS=1    also the menu / interface pictures (default 0)
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

/* 1 when replacement is on (reads the variables and the index once). */
int hdtex_on(void);

/* Loading an HD texture: a worker thread reads and decodes it while the
 * game's own texture is drawn (XBOX_HD_TEXTURES_SYNC=1: at once, on the
 * calling thread; XBOX_HD_TEXTURES_THREADS=<n> workers, default a quarter
 * of the logical processors). */
typedef struct HdJob HdJob;

/* After a texture-cache miss: a job for the HD texture of these guest level-0
 * bytes of a w x h texture, or NULL when the pack has none. The bytes are
 * only read during the call. */
HdJob *hdtex_request(const uint8_t *level0, uint32_t n0, uint32_t w, uint32_t h);

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
