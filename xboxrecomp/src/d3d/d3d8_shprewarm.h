/**
 * The pump's shaders built before they are drawn (XBOX_SHADER_PREWARM).
 *
 * The pump builds a combiner pixel shader or a vertex program the first time
 * a draw needs it (d3d8_nv2a.c); with an empty disk cache (d3d8_shcache.h)
 * that is a D3DCompile of 1-30 ms in the middle of a frame, sometimes in a
 * race. Here a background thread started at the pump's first combiner draw
 * (in the menus) turns every known "recipe" into HLSL, compiles it through
 * the same disk cache and creates the D3D11 object; the pump then takes the
 * object instead of building it. Same HLSL, same compiler arguments, so the
 * same bytecode: nothing drawn changes, only when the compile happens.
 *
 * Recipes:
 *   pixel shader  : the combiner state (Nv2aPshState) without the session's
 *                   settings (snow look, relief, lightmap filter, ...) and a
 *                   flag telling whether the lightmap filter applies to it;
 *                   the settings of the session that prewarms are put back
 *                   (nv2a_pgraph_d3d11.c), so a list made with one preset
 *                   serves the others;
 *   vertex program: the decoded program, its hash, its inputs, the kind of
 *                   each input and the point flag (d3d8_nv2a.c).
 * They come from two files, read once at the pump's first combiner draw:
 *   <shader cache folder>/recipes.otsr  what this installation has drawn
 *                                       (appended by the pump, bounded; it
 *                                       stays on the player's machine, like
 *                                       the bytecode cache next to it);
 *   XBOX_SHADER_PREWARM_LIST            the list given with the game, read only
 *                                       (default: shaders.otsr next to the exe).
 * The given list holds no byte of the game: each of its records names where
 * the title's own program is in the disc's default.xbe -- the file offset of
 * a vertex program or of a pixel shader definition, which the title creates
 * its shaders from -- plus the translator's own state for the draw (input
 * kinds, texture modes, alpha test, ...) and the key the rebuilt recipe must
 * hash to. The program's bytes are read from the player's default.xbe as the
 * game loaded it (shpw_set_xbe), never from the list; a record that does not
 * rebuild to its key (another disc) is skipped. Raw recipes are accepted
 * from the learned file only, references from the given list only.
 *
 * File format: header "OTSR", version, the sizes of the structures and an id
 * of the default.xbe they were made against (a file of another build or
 * disc is ignored, the learned one started again); then records type, size,
 * payload, checksum. A record that does not check stops the reading (a cut
 * tail).
 *
 * XBOX_SHADER_PREWARM  1 (default): on; 0 = the previous path (no recipe
 *                      read or written, no thread).
 */
#ifndef D3D8_SHPREWARM_H
#define D3D8_SHPREWARM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SHPW_PS = 1,        /* learned: Nv2aPshState (normalized) + lightmap flag */
    SHPW_VS = 2,        /* learned: d3d8_nv2a.c's vertex recipe */
    SHPW_PS_REF = 3,    /* given: a pixel shader definition in default.xbe + our state */
    SHPW_VS_REF = 4     /* given: a vertex program in default.xbe + our state */
};

/* A pixel-shader recipe -> its key (nv2a_psh_key of the state with this
 * session's settings) and HLSL; the length, or <= 0. Set by the pump. */
typedef int (*ShpwPsHlsl)(const void *rec, int size, uint64_t *key, char *buf, int cap);

/* XBOX_SHADER_PREWARM: 1 unless it is "0". */
int  shpw_on(void);

/* main.c, once, before the game starts: the disc's default.xbe as loaded
 * (kept until the game ends). Without it the given list is not read. */
void shpw_set_xbe(const void *xbe, size_t size);

/* Pump, once, at its first combiner draw (after its settings are read):
 * reads the recipe files and starts the thread. */
void shpw_start(ShpwPsHlsl ps_hlsl);

/* Pump: a shader it had to build itself (not one taken from the thread). */
void shpw_note(int type, const void *rec, int size);

/* Pump, on a miss of its own table: the object built ahead for this key
 * (handed over: the caller owns it), or NULL / 0. */
void *shpw_take_ps(uint64_t key);                           /* ID3D11PixelShader * */
int   shpw_take_vs(uint64_t key, void **vs, void **code);   /* ID3D11VertexShader *, ID3D10Blob * */

/* d3d8_nv2a.c: the pump's own two builds (same compiler arguments for the
 * pump and the thread), and a vertex-program recipe -> its key and HLSL. */
void *d3d8_nv2a_build_ps(const char *hlsl, int len, int *hit);                   /* ID3D11PixelShader * or NULL */
int   d3d8_nv2a_build_vs(const char *src, int len, void **vs, void **code, int *hit);   /* 1 = built */
int   d3d8_nv2a_vs_recipe_hlsl(const void *rec, int size, uint64_t *key, char *buf, int cap);
/* d3d8_nv2a.c: a vertex recipe from a program's instruction words (4 dwords
 * each, as in the transform program memory) and our state for the draw
 * (t0z: the terrain seams' blend overlay instruction added); its size in
 * `out` (<= 0: not a program the pump would build) and key. */
int   d3d8_nv2a_vs_recipe_make(const uint32_t (*words)[4], int count, int t0z, const uint8_t kind[16],
                               int pts, void *out, int cap, uint64_t *key);
/* d3d8_nv2a.c: the pump's fixed shaders it builds on first use (the point
 * geometry shader), compiled into the disk cache. */
void  d3d8_nv2a_prewarm_fixed(void);

#ifdef __cplusplus
}
#endif
#endif
