/*
 * pumplag -- constant blocks rewritten before the pump draws them: the fix
 * (XBOX_FIX_CONSTWAIT, default 1) and its diagnostic (XBOX_PUMPLAG).
 *
 * Mesh parts ("defMeshList", drawn by MeshRenderer_DrawPartsList 0xFFDC0) and
 * the records of the "Grid" objects (queued by 0x1017E0, drawn by 0xFFEE0)
 * read their world-view-projection matrix from 0xE0-byte blocks of one array
 * in the render context (ctx+0x1A790, 4 x 0x6A4 blocks), each bound as a
 * stride-0 vertex stream through the vertex buffer header at block + 0xC8.
 * The GPU reads a block when it reaches the draw, so the title must not
 * rewrite it before then. Mesh parts take their block through D3D Lock
 * (0x16B070), which waits for the GPU to be done with it (BlockOnResource
 * 0x16B890). 0x1017E0 indexes the same array with a mod-2 frame index (the
 * parts use mod 4) and the same per-frame counter, and writes without that
 * wait: on the next frame it lands on blocks the pump may not have drawn yet,
 * and those draws then use another object's matrix.
 *
 * XBOX_FIX_CONSTWAIT=1 (default): before 0x1017E0, the same BlockOnResource
 * on the header of the block it is about to fill. 0: the original. [CONSTWAIT]
 * lines count the waits.
 *
 * XBOX_PUMPLAG=1 (diagnostic): before each part / Grid record, its block is
 * copied and a NOP marker 0x5Dxxxxxx (sequence number) is pushed; the
 * translator compares the block with guest memory when it reaches the marker
 * ([PLAG] lines: the draw, how many frames the title is ahead, who wrote the
 * block in between; [PLAGD]: where it was drawn and where it was meant).
 * XBOX_PUMPLAG_SHOTS=<prefix>: those frames (and the ones before / after) are
 * saved. XBOX_PUMPLAG_SLOW=<ms>: the pump stalls that long per present (stress).
 */
#ifndef FORK_PUMPLAG_H
#define FORK_PUMPLAG_H

#include <stdint.h>

extern int g_pumplag;

void pumplag_init(void);
void (*pumplag_lookup(unsigned int xbox_va))(void);

#endif
