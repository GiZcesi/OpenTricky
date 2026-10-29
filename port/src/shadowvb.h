/*
 * shadowvb -- a rider part whose dynamic vertex buffers cannot be allocated
 * is not drawn, instead of sharing a vertex buffer slot with another part
 * and writing past its buffers (XBOX_FIX_SHADOWVB). The names "shadowvb" and
 * "shadow" in this file and in the variables come from the part's edge count
 * the buffers are sized by; the buffers themselves hold more than a shadow.
 *
 * The graphics context keeps 0x200 vertex buffer slots (ctx + 0x18FE40, 48
 * bytes each: three 16-byte D3DVertexBuffer headers, Data at +4). A slot is
 * free when the Data of its first buffer is 0. When a rider is loaded, every
 * part with edges (part + 0x184) gets one dynamic slot from 0x103900 (ctx
 * vtable +0x19C): three buffers of edges * 128 bytes each, from 0x0FE100,
 * stored at part + 0x60. Each frame the per-part writer 0x1033D0 (ctx vtable
 * +0x1A4) locks the slot's current buffer, writes edges * 128 bytes of
 * vertices into it, then queues the draw of that buffer. Skipping the writer
 * for a part was seen to remove the part from the picture (an arm, the
 * torso, the head or the board), while its ground shadow stays: the buffers
 * are most likely the part's CPU-skinned mesh, not only its shadow.
 *
 * 0x0FE100 does not check the game arena's answer (0x150D70). When the arena
 * is full, the slot keeps Data = 0, so it still looks free and is handed out
 * again to the next part -- typically one with no edges, whose buffers are
 * 0 bytes long. Both parts then own the slot, and every frame the writer
 * puts the first part's edges * 128 bytes into the second part's empty
 * buffers, over whatever the arena placed after them (in practice the
 * track's D3DTextures table, allocated next). A later SetTexture frees one of
 * the overwritten texture headers and the game freezes or crashes in the
 * heap. The arena is short only at the peak of a race load, with a heavy
 * six-rider roster: never seen offline in normal play, seen when a race is
 * started straight from the title screen (online). This is a latent defect
 * of the original game, not of the port.
 *
 * XBOX_FIX_SHADOWVB=1 (default), a last resort: when a part's buffers are
 * refused, the slot is released (with any buffer that did get allocated),
 * the part takes a slot of size 0 like the parts without edges (its edge
 * count is 0 only while 0x103900 fills the buffers, then restored: the
 * callers use it), and the per-frame writer is skipped for that part, so
 * that part of the rider is not drawn for the rest of the race (an
 * invisible arm, torso, head or board instead of a freeze). The writer's
 * loop is a do-while: it is skipped, never run with 0 edges. One "[MEM]"
 * line is logged the first time. As long as every allocation succeeds,
 * nothing differs from the original.
 * XBOX_FIX_SHADOWVB=0: the original code.
 *
 * Test option (off by default): XBOX_TEST_SHADOWVB_FAIL=N fills the game
 * arena right before every Nth such vertex buffer allocation (1 = all of
 * them), so that it really fails, then releases the filler at once. Only
 * those allocations are refused; every other allocation of the load sees
 * the arena as it was. With the fix, those parts are not drawn; with
 * XBOX_FIX_SHADOWVB=0, it reproduces the original overflow.
 *
 * Direct calls (0x0FE100 from 0x103900, 0x0FE260 from the part record's
 * release) reach the hooks after the fork pass
 * port/tools/fork/fix_manual_hook_calls.py; indirect calls (the writer,
 * through the vtable) through recomp_lookup_manual().
 */
#ifndef FORK_SHADOWVB_H
#define FORK_SHADOWVB_H

void (*shadowvb_lookup(unsigned int xbox_va))(void);

#endif
