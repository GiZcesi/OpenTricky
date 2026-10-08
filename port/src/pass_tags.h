/*
 * pass_tags -- render pass tags.
 *
 * Watches the render passes of the SSX engine through three hooks installed
 * via recomp_lookup_manual(), and turns them into events consumed by
 * "sinks": a LOG sink (text journal) and an EMIT sink (NOP markers in the
 * pushbuffer, read by the translator).
 *
 * ── Modes (environment variable XBOX_PASS_TAGS, read once at start)
 *
 *   unset, "0", "off"   OFF (default). recomp_lookup_manual() returns NONE of
 *                       the hooks: the original path is strictly unchanged,
 *                       the only cost is an integer test in lookup_manual.
 *   "log"               LOG. The 3 hooks are active, the events go to
 *                       _local/logs/pass_tags_<YYYYMMDD_HHMMSS>.log.
 *   "emit"              EMIT. The hooks write NOP markers in the pushbuffer
 *                       (EMIT sink below); the translator reads them
 *                       (nv2a_pgraph_d3d11.c, phases). No visual effect.
 *                       Summary on stderr every ~600 frames
 *                       ("[PASS-TAGS] emit : ...").
 *   "log+emit"          both (also "emit+log").
 *
 * Log settings (optional):
 *   XBOX_PASS_TAGS_DIR     log folder. Default: the highest "_local" folder
 *                          found walking up from the exe, + /logs (from
 *                          <root>/_local/<worktree>/port/build this gives
 *                          <root>/_local/logs); else the exe's folder.
 *   XBOX_PASS_TAGS_FULL    number of first frames logged in full
 *                          (default 300).
 *   XBOX_PASS_TAGS_EVERY   then one frame in N (default 60, ~1/s), plus any
 *                          frame whose sequence is new (signature absent
 *                          from the last 16 logged signatures).
 *   XBOX_PASS_TAGS_MAXMB   file size cap (default 64 MB). Beyond it, no more
 *                          detail, only the summaries go on.
 *
 * ── The hooks (conventions checked in the XBE bytes)
 *
 *   H1 0x00105CE0 SceneRenderer_RenderFrame      thiscall, 0 arg, ret
 *        -> FRAME_BEGIN (extra = reflection planned 0/1) ... FRAME_END
 *   H2 0x000FAAE0 GfxContext_ApplyStateBlock     thiscall, 1 arg, ret 4
 *        -> GROUP (view, group, ortho, handler) when the triple changes;
 *           GROUP_END (extra = number of records) when it ends.
 *           Filter: only the call site in the record loop of
 *           SceneView_RenderPass is kept (rec = blk-4 must be a valid
 *           record of the current view).
 *   H3 0x000FE940 GfxContext_SetOrthographicViewAndApply  thiscall, 7 args, ret 0x1C
 *        -> ORTHO (left, top, width, height, near, far, flag) when the
 *           arguments change. Reset by H1, like the H2 triple.
 *
 *   Rules for every hook (mandatory):
 *     1. read ALL the arguments before calling the original (it pops them);
 *     2. call the original exactly once, with g_ecx unchanged;
 *     3. do not change g_eax / g_edx / g_ecx after the call (they are saved
 *        and restored around the sinks as a precaution);
 *     4. only one "owner" thread is logged: the one that enters H1 (the game
 *        renders from two successive threads, boot then main loop). Calls
 *        from another thread are counted and go straight to the original.
 *        The state is under a lock (never held while the original runs).
 *   Stack check: on entry g_esp points at the dummy return address; after
 *   the original it must equal entry + 4 + bytes popped. Any mismatch is
 *   counted ("esp_bad" in the log summaries).
 *
 * ── Adding a hook
 *   1. Check the convention in the BYTES (never the "CC:" header of gen/,
 *      wrong by default) and that the function is only reached by indirect
 *      calls (otherwise lookup_manual does not see it).
 *   2. Write hook_XXXXXXXX() in pass_tags.c after the model of hook_FE940:
 *      read the arguments, pt_dispatch() an event, call sub_XXXXXXXX(),
 *      check g_esp.
 *   3. Add it to pass_tags_lookup(); a new PT_EV_* if needed.
 *
 * ── Adding a sink
 *   Fill a pass_tags_sink (name, event, flush) and register it with
 *   pass_tags_add_sink() during pass_tags_init(). event() is called on the
 *   game thread, inside the hook: it must be short and must not touch the
 *   guest registers (except the EMIT sink, see below).
 *
 * ── EMIT sink (pass_tags.c emit_event)
 *   For FRAME_BEGIN, GROUP and FRAME_END (not ORTHO nor GROUP_END), only on
 *   the owner thread and inside an H1 frame, write `0x00040100, tag`
 *   (NV097_NO_OPERATION, 1 parameter) into the pushbuffer:
 *     ctx = MEM32(0x001776C0); refuse if ctx == 0 or MEM32(ctx+0xC) & 4
 *     (pushbuffer recording); call sub_0016B920(ctx) (stdcall, ret 4,
 *     guarantees wp < limit with a 0x200 margin); wp = g_eax;
 *     MEM32(wp) = 0x00040100; MEM32(wp+4) = tag; MEM32(ctx) = wp + 8;
 *     save/restore g_eax, g_ecx, g_edx around it.
 *   GROUP/ORTHO/FRAME_BEGIN events are delivered BEFORE the original (so
 *   the marker precedes the pass's commands), FRAME_END after.
 *   The translator (nv2a_pgraph_d3d11.c, case NV097_NO_OPERATION) derives
 *   the current render phase from it, an absolute state and so tolerant of
 *   losses: see the "Pass phases" comment in the translator. Frames rendered
 *   by the second thread (loading) have no markers.
 *
 * ── 32-bit tag format (the NOP parameter)
 *
 *   31            16 15 14 13 12      10 9        5 4       0
 *   +---------------+-----+--+----------+----------+---------+
 *   | magic 0x5358  |type |o | view (3) | group (5)| cnt (5) |   GROUP / ORTHO
 *   +---------------+-----+--+----------+----------+---------+
 *   | magic 0x5358  |type |r |  frame number modulo 8192 (13)|   FRAME_BEGIN / END
 *   +---------------+-----+--+-------------------------------+
 *
 *   magic  0x5358 ("SX") in the high 16 bits.
 *   type   0 FRAME_BEGIN, 1 FRAME_END, 2 GROUP, 3 ORTHO.
 *   o      1 if the view is orthographic (FOV view+0x80C == 0.0); always 1
 *          for ORTHO.
 *   r      FRAME_BEGIN: reflection planned; FRAME_END: 0.
 *   view   view index 0..5; 7 = none / unknown.
 *   group  pass group 0..0x17 (rec+0xC >> 27); 0 for ORTHO.
 *   cnt    index of the event in the frame modulo 32 (detects losses and
 *          reordering in the stream).
 *   GROUP_END has no tag (pass_tags_tag() returns 0).
 */
#ifndef PASS_TAGS_H
#define PASS_TAGS_H

#include <stdint.h>

#define PASS_TAG_MAGIC        0x5358u
#define PASS_TAG_IS(p)        (((uint32_t)(p) >> 16) == PASS_TAG_MAGIC)
#define PASS_TAG_TYPE(p)      (((uint32_t)(p) >> 14) & 3u)
#define PASS_TAG_VIEW_NONE    7u

enum pass_tags_mode {
    PASS_TAGS_OFF  = 0,
    PASS_TAGS_LOG  = 1,
    PASS_TAGS_EMIT = 2          /* emit or log+emit */
};

enum pass_tag_type {
    PT_EV_FRAME_BEGIN = 0,
    PT_EV_FRAME_END   = 1,
    PT_EV_GROUP       = 2,
    PT_EV_ORTHO       = 3,
    PT_EV_GROUP_END   = 4,      /* log only, no tag */
    PT_EV_HUD         = 5       /* extra = HUD tag (hud_anchor.h), emitted as is */
};

typedef struct pass_tag_event {
    uint32_t seq_frame;         /* frame number (incremented on each H1) */
    uint32_t seq_event;         /* index of the event in the frame */
    uint32_t type;              /* enum pass_tag_type */
    uint32_t view;              /* 0..5, PASS_TAG_VIEW_NONE otherwise */
    uint32_t group;             /* pass group 0..0x17 */
    uint32_t ortho;             /* ortho view (FOV == 0) */
    uint32_t handler_va;        /* GROUP: handler [[rec]] of the 1st record */
    uint32_t extra;             /* FRAME_BEGIN: reflection; GROUP_END: number of records;
                                 * FRAME_END: number of records kept in the frame;
                                 * ORTHO: flag (7th argument) */
    float    ortho_args[6];     /* ORTHO: left, top, width, height, near, far */
    uint32_t thread;            /* Windows id of the owner thread */
} pass_tag_event;

typedef struct pass_tags_sink {
    const char *name;
    void (*event)(const pass_tag_event *ev);
    void (*flush)(void);        /* optional, called ~ every 60 frames */
} pass_tags_sink;

/* 32-bit tag of an event, 0 if it has none (GROUP_END). */
static inline uint32_t pass_tags_tag(const pass_tag_event *ev)
{
    uint32_t t = PASS_TAG_MAGIC << 16;
    switch (ev->type) {
    case PT_EV_FRAME_BEGIN:
    case PT_EV_FRAME_END:
        return t | (ev->type << 14)
                 | ((ev->type == PT_EV_FRAME_BEGIN && ev->extra) ? 1u << 13 : 0u)
                 | (ev->seq_frame & 0x1FFFu);
    case PT_EV_GROUP:
    case PT_EV_ORTHO:
        return t | (ev->type << 14) | ((ev->ortho & 1u) << 13)
                 | ((ev->view & 7u) << 10) | ((ev->group & 0x1Fu) << 5)
                 | (ev->seq_event & 0x1Fu);
    default:
        return 0;
    }
}

/* Reads XBOX_PASS_TAGS and sets up the sinks. Call once, at start, before
 * the game runs. */
void pass_tags_init(void);

/* Active mode. Read by recomp_lookup_manual(): when OFF the hooks do not exist. */
extern int g_pass_tags_mode;

/* Hook for this VA, or NULL. Call only if g_pass_tags_mode != OFF. */
void (*pass_tags_lookup(uint32_t xbox_va))(void);

/* The draw about to be made keeps the stretch (full-screen fade
 * quad, fullfade.c). Game thread, inside the title's draw of a record.
 * Returns 1 when a tag was written (the record's own would have framed it). */
int pass_tags_draw_stretched(void);

/* Registers a sink (4 at most). Returns 0 if the table is full. */
int pass_tags_add_sink(const pass_tags_sink *s);

#endif /* PASS_TAGS_H */
