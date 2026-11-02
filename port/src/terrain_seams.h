/*
 * terrain_seams -- texture seams between terrain patches (PC options, off by
 * default: nothing is hooked and the terrain is drawn exactly as the title
 * draws it). The settled set is XBOX_TERRAIN_SEAMS=b0 XBOX_TERRAIN_BLEND=2
 * XBOX_TERRAIN_CLAMP=2 (band 0.75 m, strict content match).
 * XBOX_TERRAIN_SEAMS=0 turns all of them off, whatever the other variables.
 *
 * The title draws the terrain patch by patch (sub_000F8BA0 lays each bicubic
 * patch into a cache slot of 8x8 vertices). Each patch places its own snow
 * texture: the texture coordinates are bilinear over the patch, from its four
 * corner values. Where two patches of the same texture meet, those placements
 * often do not follow each other (the track data itself), and the snow shows a
 * straight line along the shared edge.
 *
 * The game thread notes which patch each cache slot holds (host memory only;
 * nothing in guest memory is written) and, once per level, classifies every
 * edge shared by two patches. On the pump thread the translator hands each
 * terrain draw to a filter that may draw the same patch with adjusted texture
 * coordinates, and in the debug view tints the edges by kind.
 *
 * XBOX_TERRAIN_SEAMS=1     both corrections below (b0 + b1); b0 / b1 one only
 *                          0 every correction off.
 *   b0: patches whose corners are inset by one texel (0.008 / 0.992) use 0 / 1,
 *       so that neighbours of the same texture meet without a 2-texel jump.
 *   b1: along an edge where both sides agree but the texture is stretched very
 *       differently on each side, the first rows of nodes slide towards the
 *       neighbour's placement (extended across the edge), so the change of
 *       direction / scale is spread over a band instead of a crease.
 * XBOX_TERRAIN_SEAMS_ROWS=1..3  rows of nodes b1 moves on each side (default 2).
 * XBOX_TERRAIN_SEAMS_DEBUG=1  debug view: edges tinted by kind (red rotated /
 *   mirrored / rescaled placement, magenta continuous but stretched very
 *   differently, yellow 2-texel offset, cyan different texture), the patch under
 *   the rider in green; its edges are written to stderr.
 * XBOX_TERRAIN_BLEND=1      where two patches of different snow textures meet
 *   with a placement that follows across the edge, each side fades into the
 *   other's textures (snow and normal map) over a band: half and half on the
 *   edge, the patch's own texture alone after XBOX_TERRAIN_BLEND_ROWS (1..3,
 *   default 1) rows of nodes. Drawn as a second pass over the band with the
 *   title's own shading. XBOX_TERRAIN_BLEND=2: also the edges whose placement
 *   does not follow (rotated / mirrored, stretched very differently, other
 *   texture placed otherwise): the band reads the neighbour's textures with
 *   the neighbour's own placement carried across the edge.
 *   Only between textures of the same content (XBOX_TERRAIN_BLEND_MATCH):
 *   strict (default) the same texture, or mean colours within 40 / 255, colour
 *   histograms 70 % in common and luminance spreads within 15 -- ice with ice,
 *   plain snow with plain snow, never striped or marked snow with plain snow,
 *   nor the track's ice with its snow; same: the same texture only; loose: the
 *   mean colours within XBOX_TERRAIN_BLEND_SIMILAR (80) only.
 *   The copies also carry the neighbour's tangent, so that its relief is lit
 *   in its own frame (XBOX_TERRAIN_BLEND_TANGENT=0: the patch's own).
 * XBOX_TERRAIN_BLEND_WIDTH=<metres>: the band's width in metres instead of rows
 *   of nodes (the alpha runs from 1/2 on the edge to 0 at that distance;
 *   default 0.75).
 * XBOX_TERRAIN_SEAMS_DEBUG=2  the same kinds as thin lines on the edges, other
 *   texture in cyan when the placement follows, blue when it does not.
 * XBOX_TERRAIN_CLAMP=1|2   a patch that lays a single repeat of its texture on
 *   an axis samples it clamped on that axis, so that the filter no longer reads
 *   the far side of the texture at the patch's edge: 1 everywhere, 2 only
 *   where the neighbour does not continue the placement; 0 off.
 *   XBOX_TERRAIN_SEAMS_MEMO=0 (comparison): the clamped axes are worked out at
 *   every draw instead of being remembered per patch with its corners' bits.
 * XBOX_TERRAIN_SEAMS_LOG=1  counters on stderr.
 */
#ifndef TERRAIN_SEAMS_H
#define TERRAIN_SEAMS_H

#include <stdint.h>

extern int g_terrain_seams;     /* any of the options above: the hook is handed out */

void terrain_seams_init(void);

/* Game thread, right after sub_000F8BA0(obj, vb, pidx) laid patch `pidx` of
 * terrain object `obj` into the cache slot at `vb`. */
void terrain_seams_note(uint32_t obj, uint32_t vb, uint32_t pidx);

#endif
