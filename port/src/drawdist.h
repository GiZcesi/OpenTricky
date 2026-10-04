/*
 * drawdist -- draw distance Original / Far / Max.
 *
 * What the title draws far away is set by three numbers, all read again at
 * every level load:
 *   - the radius of visible terrain cells, [[lvl+0x2C]+0x60] (30000, one
 *     player) and +0x64 (20000, split screen), read each tick by
 *     TerrainNode_TickVisibilityUpdate (0xDE230) -- cells of 10000 units;
 *   - the far plane: view+0x814 (30000) times the float [0x1C0834] (1.0) in
 *     SceneView_RenderPass (0xFFB50);
 *   - the distance of the fine terrain patches, [0x1BD160] (10000; split
 *     screen uses half of it), set by the per-track setup (0xFD3E0).
 * Far multiplies the three by 1.5 and Max by 2, at every level, after the
 * title has set them.
 *
 * The one hard limit: TerrainNode_BuildVisibleCellList (0x1420B0) writes the
 * visible cells into a 162-entry list (0x1FAF88+0x3C) with no bound -- the
 * 162nd entry overwrites the count. At each level the radius is capped so
 * that no square of (2k+1)^2 cells of that level's grid holds more than 150
 * non-empty cells (exact: the title's window is that square, clipped to the
 * grid -- 0x30380). At run time, a count of 150 or more cuts the radius by
 * one cell for the following frames (logged; never below the title's own).
 *
 *   XBOX_DRAW_DISTANCE   0 / original (default: nothing is written, the hooks
 *                        pass through), 1 / far, 2 / max.  The launcher sets
 *                        it from [Fork] DrawDistance.
 *   XBOX_DRAW_DISTANCE_LOG=1  also count in Original (cells, terrain patch
 *                        cache) -- the [DRAWDIST] lines on stderr.
 *
 * The terrain patch cache (BoardMesh_DrawAttachedPatches 0xF8F10, LRU of
 * [0x1BD158] slots, one re-tessellation 0xF8BA0 per miss) is watched: a slot
 * filled twice in one call would mean a patch overwritten before it is drawn.
 */
#ifndef FORK_DRAWDIST_H
#define FORK_DRAWDIST_H

extern int g_drawdist_level;     /* 0 Original, 1 Far, 2 Max */

void drawdist_init(void);        /* reads XBOX_DRAW_DISTANCE(_LOG) */
void (*drawdist_lookup(unsigned int xbox_va))(void);

#endif
