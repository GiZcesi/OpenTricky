/*
 * pass_tags -- render pass tags.
 * See pass_tags.h for the modes, the tag format and the conventions.
 *
 * Layout:
 *   - core: frame state (game thread only), de-duplication, dispatch of the
 *     events to the sinks;
 *   - hooks H1/H2/H3;
 *   - LOG sink (text block per frame, sampling, size cap).
 */
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Same include as recomp_manual.c: g_esp, MEM32, MEMF, without the register
 * aliases of the generated code. */
#include "recomp/recomp_types.h"
#include "pass_tags.h"
#include "hud_anchor.h"

extern void sub_00105CE0(void);     /* SceneRenderer_RenderFrame */
extern void sub_000FAAE0(void);     /* GfxContext_ApplyStateBlock */
extern void sub_000FE940(void);     /* GfxContext_SetOrthographicViewAndApply */

int g_pass_tags_mode = PASS_TAGS_OFF;

/* ── Noyau ───────────────────────────────────────────────────── */

#define PT_MAX_SINKS 4
static const pass_tags_sink *s_sinks[PT_MAX_SINKS];
static int s_nsinks;

/* Owner thread: the one of the last H1 (RenderFrame). The game renders from
 * two successive threads (the first during boot, then the main loop; the
 * arrival order varies between runs): ownership moves to the thread that
 * enters H1 when the owner is not in the middle of a frame. Calls from a
 * non-owner thread go straight to the original and are only counted.
 * All the state below is guarded by s_lock, never held while the original
 * runs (no cross wait between game threads). */
static CRITICAL_SECTION s_lock;
static DWORD s_owner_tid;
static unsigned s_owner_switches;

static struct {
    uint32_t frame;             /* current frame number (0 = before the 1st H1) */
    uint32_t event;             /* index of the event in the frame */
    int      in_frame;
    /* triplet H2 courant */
    int      grp_open;
    uint32_t grp_key;
    uint32_t grp_nrec;
    uint32_t frame_nrec;
    uint32_t hud_tag, hud_tag_y;  /* last HUD tags emitted in the group */
    /* derniers arguments H3 */
    int      ortho_valid;
    uint32_t ortho_key[7];
} s_st;

static struct {
    unsigned long long h1, h2, h2_kept, h3, events;
    unsigned long long foreign[3];      /* non-owner calls per hook */
    DWORD foreign_tid[3];               /* last non-owner thread */
} s_cnt;

static volatile LONG s_esp_bad;         /* atomic: checked outside the lock */

int pass_tags_add_sink(const pass_tags_sink *s)
{
    if (s_nsinks >= PT_MAX_SINKS) return 0;
    s_sinks[s_nsinks++] = s;
    return 1;
}

/* Takes the lock and returns 1 if the caller is the owner (lock kept, to be
 * released by pt_leave); otherwise counts the call and returns 0 (lock
 * released). hook: 0 = H1, 1 = H2, 2 = H3. */
static int pt_enter(int hook)
{
    DWORD me = GetCurrentThreadId();
    EnterCriticalSection(&s_lock);
    if (hook == 0 && me != s_owner_tid && !s_st.in_frame) {
        if (s_owner_switches++ < 16)
            fprintf(stderr, "[PASS-TAGS] render thread: %lu -> %lu (frame %u)\n",
                    (unsigned long)s_owner_tid, (unsigned long)me, s_st.frame + 1);
        s_owner_tid = me;
        s_st.ortho_valid = 0;
    }
    if (!s_owner_tid) s_owner_tid = me;
    if (me == s_owner_tid) return 1;
    s_cnt.foreign[hook]++;
    s_cnt.foreign_tid[hook] = me;
    LeaveCriticalSection(&s_lock);
    return 0;
}

static void pt_leave(void)
{
    LeaveCriticalSection(&s_lock);
}

/* Sinks must not touch the guest registers; they are protected anyway, an
 * EMIT sink calls guest code. */
static void pt_dispatch(pass_tag_event *ev)
{
    uint32_t sv_eax = g_eax, sv_ecx = g_ecx, sv_edx = g_edx;
    int i;
    ev->seq_frame = s_st.frame;
    /* GROUP_END has no tag: it uses no index (the HUD tag has its own
     * format: no index either). */
    ev->seq_event = (ev->type == PT_EV_GROUP_END || ev->type == PT_EV_HUD) ? s_st.event : s_st.event++;
    ev->thread = s_owner_tid;
    s_cnt.events++;
    for (i = 0; i < s_nsinks; i++)
        s_sinks[i]->event(ev);
    g_eax = sv_eax; g_ecx = sv_ecx; g_edx = sv_edx;
}

static void pt_group_close(void)
{
    pass_tag_event ev;
    if (!s_st.grp_open) return;
    memset(&ev, 0, sizeof ev);
    ev.type  = PT_EV_GROUP_END;
    ev.view  = (s_st.grp_key >> 8) & 7u;
    ev.group = s_st.grp_key & 0x1Fu;
    ev.ortho = (s_st.grp_key >> 7) & 1u;
    ev.extra = s_st.grp_nrec;
    pt_dispatch(&ev);
    s_st.grp_open = 0;
}

static void pt_check_esp(uint32_t esp0, uint32_t popped)
{
    if (g_esp != esp0 + 4u + popped) InterlockedIncrement(&s_esp_bad);
}

/* ── H1 : SceneRenderer_RenderFrame 0x105CE0 (thiscall, 0 arg, ret) ── */

static void hook_105CE0(void)
{
    uint32_t gfx = g_ecx, esp0 = g_esp, refl = 0, p, i, frame_tag;
    pass_tag_event ev;

    if (!pt_enter(0)) {
        sub_00105CE0();
        pt_check_esp(esp0, 0);
        return;
    }
    s_cnt.h1++;

    /* Same conditions as the reflection block of RenderFrame (0x105D34). */
    p = MEM32(0x001E3C7Cu);
    if (gfx && p >= 0x1000u && MEM32(p + 0x72Cu) && MEM32(gfx + 0x22C928u)
        && !MEM32(0x001C0820u))
        refl = 1;

    s_st.frame++;
    frame_tag = (g_hud_anchor_on || g_box_on || g_race2d_on || g_panel_on) ? hud_anchor_frame() : 0;   /* HUD tags recomputed per frame */
    s_st.event = 0;
    s_st.in_frame = 1;
    s_st.grp_open = 0;
    s_st.frame_nrec = 0;
    s_st.ortho_valid = 0;
    for (i = 0; i < 7; i++) s_st.ortho_key[i] = 0;

    memset(&ev, 0, sizeof ev);
    ev.type  = PT_EV_FRAME_BEGIN;
    ev.view  = PASS_TAG_VIEW_NONE;
    ev.extra = refl;
    pt_dispatch(&ev);
    if (frame_tag) {                    /* wide or 16:9 frame */
        memset(&ev, 0, sizeof ev);
        ev.type  = PT_EV_HUD;
        ev.view  = PASS_TAG_VIEW_NONE;
        ev.extra = frame_tag;
        pt_dispatch(&ev);
    }
    if (s_st.frame % 60u == 0) {
        for (i = 0; i < (uint32_t)s_nsinks; i++)
            if (s_sinks[i]->flush) s_sinks[i]->flush();
    }
    pt_leave();

    g_ecx = gfx;
    sub_00105CE0();

    {
        uint32_t sv_eax = g_eax, sv_edx = g_edx;
        EnterCriticalSection(&s_lock);
        pt_group_close();
        memset(&ev, 0, sizeof ev);
        ev.type  = PT_EV_FRAME_END;
        ev.view  = PASS_TAG_VIEW_NONE;
        ev.extra = s_st.frame_nrec;
        pt_dispatch(&ev);
        s_st.in_frame = 0;
        pt_leave();
        g_eax = sv_eax; g_edx = sv_edx;
    }
    pt_check_esp(esp0, 0);
}

/* ── H2: GfxContext_ApplyStateBlock 0xFAAE0 (thiscall, 1 arg, ret 4) ──
 *
 * Only the call site in the record loop of SceneView_RenderPass (0xFFD92)
 * passes the filter: rec = blk - 4 must be a record of the current view. */

static void pt_note_record(uint32_t gfx, uint32_t blk)
{
    uint32_t v, rec, base, end, vbase, view, group, ortho, key, obj;
    pass_tag_event ev;

    if (!gfx) return;
    v = MEM32(gfx + 0x196004u);
    if (v == 0xFFFFFFFFu || !v) return;
    rec  = blk - 4u;
    base = v + 0x8C0u;
    end  = MEM32(v + 0x808u);
    if (rec < base || rec >= end || (rec - base) % 0x18u) return;
    s_cnt.h2_kept++;
    s_st.frame_nrec++;

    vbase = MEM32(gfx + 0x196018u);
    view  = (v >= vbase && (v - vbase) % 0xC8C0u == 0 && (v - vbase) / 0xC8C0u < 6u)
          ? (v - vbase) / 0xC8C0u : PASS_TAG_VIEW_NONE;
    group = MEM32(rec + 0xCu) >> 27;
    ortho = MEMF(v + 0x80Cu) == 0.0f;
    key   = (view << 8) | (ortho << 7) | group;

    if (s_st.grp_open && key == s_st.grp_key) {
        s_st.grp_nrec++;
    } else {
        pt_group_close();
        s_st.grp_open = 1;
        s_st.grp_key  = key;
        s_st.grp_nrec = 1;
        s_st.hud_tag  = 0;      /* the translator forgets the HUD tag at a GROUP marker */

        memset(&ev, 0, sizeof ev);
        ev.type  = PT_EV_GROUP;
        ev.view  = view;
        ev.group = group;
        ev.ortho = ortho;
        obj = MEM32(rec);
        ev.handler_va = obj >= 0x1000u ? MEM32(obj) : 0;
        pt_dispatch(&ev);
    }

    /* Race HUD element -> its two tags (x anchor, y anchor) before its
     * draws; the next record outside the HUD sets the tag back to
     * "none". */
    if ((g_hud_anchor_on || g_box_on || g_race2d_on || g_panel_on) && ortho) {
        uint32_t ty, t = hud_anchor_tag(MEM32(rec), v, &ty);
        if (t != s_st.hud_tag || (t && ty != s_st.hud_tag_y)) {
            memset(&ev, 0, sizeof ev);
            ev.type  = PT_EV_HUD;
            ev.view  = view;
            ev.extra = t ? t : HUD_TAG_MAGIC << 16;
            pt_dispatch(&ev);
            if (t) {
                ev.extra = ty;
                pt_dispatch(&ev);
            }
            s_st.hud_tag = t;
            s_st.hud_tag_y = ty;
        }
    }
}

/* A draw that keeps the stretch whatever its record's tag says.
 * fullfade.c calls it from the world-quads draw (0x1009D0), before the
 * title's method, when it has just widened a full-screen fade quad. The
 * record's tag is worked out earlier (above, hud_anchor_tag), from the
 * quads it reads in the first 2D vertex buffer; the title picks the buffer
 * only when it draws, and in the trick tutorial the respawn flash comes
 * from the second one while the first holds other, smaller quads at the
 * same place: the flash was taken for a menu record and framed at 4:3.
 * Here the draw gets "as the title drew it" (BOX_OFF: no frame, no HUD
 * scale, no panel) when the current tag says otherwise; the next record's
 * tag differs again and is written again. In a race the marker is written
 * too (the flash was already drawn stretched there: same image before and
 * after). Only draws that fullfade widens are concerned. */
int pass_tags_draw_stretched(void)
{
    pass_tag_event ev;
    int written = 0;
    const uint32_t off = (HUD_TAG_MAGIC_BOX << 16) | HUD_BOX_OFF;
    if (g_pass_tags_mode == PASS_TAGS_OFF) return 0;
    if (!(g_hud_anchor_on || g_box_on || g_race2d_on || g_panel_on)) return 0;
    if (!pt_enter(1)) return 0;
    if (s_st.in_frame && s_st.hud_tag && s_st.hud_tag != off) {
        memset(&ev, 0, sizeof ev);
        ev.type  = PT_EV_HUD;
        ev.extra = off;
        pt_dispatch(&ev);
        s_st.hud_tag = off;
        s_st.hud_tag_y = 0;
        written = 1;
    }
    pt_leave();
    return written;
}

static void hook_FAAE0(void)
{
    uint32_t gfx = g_ecx, esp0 = g_esp, blk = MEM32(g_esp + 4u);

    if (pt_enter(1)) {
        s_cnt.h2++;
        pt_note_record(gfx, blk);
        pt_leave();
    }
    g_ecx = gfx;
    sub_000FAAE0();
    pt_check_esp(esp0, 4);
}

/* ── H3 : GfxContext_SetOrthographicViewAndApply 0xFE940 (thiscall, 7 args, ret 0x1C) ── */

static void hook_FE940(void)
{
    uint32_t gfx = g_ecx, esp0 = g_esp, a[7], i, same = 1;

    for (i = 0; i < 7; i++) a[i] = MEM32(esp0 + 4u + 4u * i);
    a[6] &= 0xFFu;                      /* the 7th argument is a byte */

    if (!pt_enter(2)) goto call;
    s_cnt.h3++;

    for (i = 0; i < 7; i++) if (a[i] != s_st.ortho_key[i]) same = 0;
    if (!s_st.ortho_valid || !same) {
        pass_tag_event ev;
        memset(&ev, 0, sizeof ev);
        ev.type  = PT_EV_ORTHO;
        ev.view  = PASS_TAG_VIEW_NONE;
        ev.ortho = 1;
        for (i = 0; i < 6; i++) memcpy(&ev.ortho_args[i], &a[i], 4);
        ev.extra = a[6];
        pt_dispatch(&ev);
        s_st.ortho_valid = 1;
        for (i = 0; i < 7; i++) s_st.ortho_key[i] = a[i];
    }
    pt_leave();

call:
    g_ecx = gfx;
    sub_000FE940();
    pt_check_esp(esp0, 0x1C);
}

void (*pass_tags_lookup(uint32_t xbox_va))(void)
{
    if (xbox_va == 0x00105CE0u) return hook_105CE0;
    if (xbox_va == 0x000FAAE0u) return hook_FAAE0;
    if (xbox_va == 0x000FE940u) return hook_FE940;
    return 0;
}

/* ── LOG sink ────────────────────────────────────────────────────
 *
 * The events of a frame (from FRAME_BEGIN to the next FRAME_BEGIN, so also
 * what is drawn outside RenderFrame) are formatted into a memory block. At
 * the start of the next frame the block is written or dropped: written in
 * full for the first FULL frames, then one frame in EVERY, plus any frame
 * whose sequence is new. */

#define LOG_BLOCK_CAP   (64 * 1024)
#define LOG_RECENT_SIGS 16

static struct {
    FILE    *f;
    char     path[MAX_PATH];
    unsigned full, every;
    unsigned long long max_bytes, bytes;
    int      capped;
    char     blk[LOG_BLOCK_CAP];
    size_t   len;
    int      truncated;
    size_t   pend_n;            /* position of the "n=      " of the open GROUP */
    int      pend_valid;
    uint32_t blk_frame;
    uint32_t sig;
    uint32_t recent[LOG_RECENT_SIGS];
    unsigned recent_n, recent_pos;
    unsigned long long blocks_written, blocks_skipped;
    unsigned since_flush;
} s_log;

static void log_sig(uint32_t v)
{
    s_log.sig = (s_log.sig ^ v) * 16777619u;     /* FNV-1a */
}

static void log_append(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (s_log.truncated) return;
    va_start(ap, fmt);
    n = vsnprintf(s_log.blk + s_log.len, LOG_BLOCK_CAP - s_log.len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= LOG_BLOCK_CAP - s_log.len) {
        s_log.truncated = 1;
        s_log.blk[s_log.len] = 0;
        return;
    }
    s_log.len += (size_t)n;
}

static void log_write(const char *s, size_t n)
{
    if (!s_log.f || s_log.capped) return;
    if (s_log.bytes + n > s_log.max_bytes) {
        fprintf(s_log.f, "# cap of %llu bytes reached: no more detail, summaries only\n",
                s_log.max_bytes);
        s_log.capped = 1;
        return;
    }
    fwrite(s, 1, n, s_log.f);
    s_log.bytes += n;
}

static void log_summary(void)
{
    char line[512];
    int n;
    if (!s_log.f) return;
    n = snprintf(line, sizeof line,
                 "# summary frame %u: h1=%llu h2=%llu h2_kept=%llu h3=%llu events=%llu "
                 "esp_bad=%ld thread=%lu changes=%u non_owner=H1:%llu,H2:%llu,H3:%llu (tid %lu/%lu/%lu) "
                 "blocks_written=%llu blocks_dropped=%llu bytes=%llu\n",
                 s_st.frame, s_cnt.h1, s_cnt.h2, s_cnt.h2_kept, s_cnt.h3, s_cnt.events,
                 (long)s_esp_bad, (unsigned long)s_owner_tid, s_owner_switches,
                 s_cnt.foreign[0], s_cnt.foreign[1], s_cnt.foreign[2],
                 (unsigned long)s_cnt.foreign_tid[0], (unsigned long)s_cnt.foreign_tid[1],
                 (unsigned long)s_cnt.foreign_tid[2], s_log.blocks_written,
                 s_log.blocks_skipped, s_log.bytes);
    if (n > 0) {
        /* Summaries go on even past the cap. */
        fwrite(line, 1, (size_t)n, s_log.f);
        s_log.bytes += (size_t)n;
    }
}

static int log_sig_is_new(uint32_t sig)
{
    unsigned i;
    for (i = 0; i < s_log.recent_n; i++)
        if (s_log.recent[i] == sig) return 0;
    return 1;
}

static void log_block_finish(void)
{
    const char *why = 0;
    int isnew;
    if (!s_log.len) return;
    isnew = log_sig_is_new(s_log.sig);
    if (s_log.blk_frame <= s_log.full)              why = "start";
    else if (isnew)                                 why = "new";
    else if (s_log.blk_frame % s_log.every == 0)    why = "sample";
    if (why) {
        char hdr[96];
        int n = snprintf(hdr, sizeof hdr, "\n= frame %u [%s] sig=%08X%s\n", s_log.blk_frame,
                         why, s_log.sig, s_log.truncated ? " TRUNCATED" : "");
        log_write(hdr, (size_t)n);
        log_write(s_log.blk, s_log.len);
        s_log.blocks_written++;
        if (isnew) {
            s_log.recent[s_log.recent_pos] = s_log.sig;
            s_log.recent_pos = (s_log.recent_pos + 1) % LOG_RECENT_SIGS;
            if (s_log.recent_n < LOG_RECENT_SIGS) s_log.recent_n++;
        }
    } else {
        s_log.blocks_skipped++;
    }
    s_log.len = 0;
    s_log.blk[0] = 0;
    s_log.truncated = 0;
    s_log.pend_valid = 0;
    s_log.sig = 2166136261u;
}

/* ~ every 60 frames: file flush; summary every 600. */
static void log_flush(void)
{
    if (!s_log.f) return;
    if (++s_log.since_flush >= 10) { s_log.since_flush = 0; log_summary(); }
    fflush(s_log.f);
}

static void log_event(const pass_tag_event *ev)
{
    switch (ev->type) {
    case PT_EV_FRAME_BEGIN:
        log_block_finish();
        s_log.blk_frame = ev->seq_frame;
        log_sig(0xB0u | ev->extra);
        log_append("F%u BEGIN reflection=%u thread=%u tag=%08X\n", ev->seq_frame, ev->extra,
                   ev->thread, pass_tags_tag(ev));
        break;
    case PT_EV_FRAME_END:
        log_sig(0xE0u);
        log_append("F%u END records=%u events=%u tag=%08X\n", ev->seq_frame,
                   ev->extra, ev->seq_event + 1, pass_tags_tag(ev));
        break;
    case PT_EV_GROUP:
        log_sig(0x1000000u | (ev->view << 8) | (ev->ortho << 7) | ev->group);
        log_append("  #%-3u G view=%u group=%2u %s handler=%08X ", ev->seq_event,
                   ev->view, ev->group, ev->ortho ? "ORTHO" : "persp", ev->handler_va);
        if (!s_log.truncated) { s_log.pend_n = s_log.len; s_log.pend_valid = 1; }
        log_append("n=      tag=%08X\n", pass_tags_tag(ev));
        break;
    case PT_EV_GROUP_END:
        if (s_log.pend_valid && !s_log.truncated) {
            char num[16];
            int k = snprintf(num, sizeof num, "n=%-6u", ev->extra);
            if (k == 8) memcpy(s_log.blk + s_log.pend_n, num, 8);
        }
        s_log.pend_valid = 0;
        break;
    case PT_EV_ORTHO:
    {
        uint32_t i, b;
        for (i = 0; i < 6; i++) { memcpy(&b, &ev->ortho_args[i], 4); log_sig(b); }
        log_sig(0x0F000000u | ev->extra);
        log_append("  #%-3u O %s left=%g top=%g width=%g height=%g near=%g far=%g flag=%u tag=%08X\n",
                   ev->seq_event, s_st.in_frame ? "in-frame" : "OUT-OF-FRAME",
                   ev->ortho_args[0], ev->ortho_args[1], ev->ortho_args[2],
                   ev->ortho_args[3], ev->ortho_args[4], ev->ortho_args[5],
                   ev->extra, pass_tags_tag(ev));
        break;
    }
    default:
        break;
    }
}

static const pass_tags_sink s_sink_log = { "log", log_event, log_flush };

static unsigned env_uint(const char *name, unsigned def)
{
    const char *e = getenv(name);
    if (e && *e) {
        unsigned long v = strtoul(e, 0, 10);
        if (v) return (unsigned)v;
    }
    return def;
}

/* The highest "_local" walking up from the exe's folder: a worktree under
 * _local often has its own _local, the log still goes to the top
 * _local/logs. Failing that, the exe's folder. */
static void log_default_dir(char *out, size_t cap)
{
    char dir[MAX_PATH], probe[MAX_PATH];
    char *s;
    DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) { snprintf(out, cap, "."); return; }
    s = strrchr(dir, '\\');
    if (s) *s = 0;
    snprintf(out, cap, "%s", dir);
    for (;;) {
        DWORD a;
        if (snprintf(probe, sizeof probe, "%s\\_local", dir) >= (int)sizeof probe) return;
        a = GetFileAttributesA(probe);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)
            && snprintf(out, cap, "%s\\logs", probe) >= (int)cap)
            snprintf(out, cap, ".");
        s = strrchr(dir, '\\');
        if (!s) return;
        *s = 0;
    }
}

static void log_open(void)
{
    char dir[MAX_PATH];
    const char *e = getenv("XBOX_PASS_TAGS_DIR");
    SYSTEMTIME t;

    if (e && *e) snprintf(dir, sizeof dir, "%s", e);
    else log_default_dir(dir, sizeof dir);
    CreateDirectoryA(dir, NULL);
    GetLocalTime(&t);
    if (snprintf(s_log.path, sizeof s_log.path, "%s\\pass_tags_%04u%02u%02u_%02u%02u%02u.log",
                 dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond)
        >= (int)sizeof s_log.path) {
        fprintf(stderr, "[PASS-TAGS] log path too long: log disabled\n");
        return;
    }
    s_log.f = fopen(s_log.path, "wb");
    if (!s_log.f) {
        fprintf(stderr, "[PASS-TAGS] cannot open %s: log disabled\n", s_log.path);
        return;
    }
    setvbuf(s_log.f, NULL, _IOFBF, 256 * 1024);
    s_log.full  = env_uint("XBOX_PASS_TAGS_FULL", 300);
    s_log.every = env_uint("XBOX_PASS_TAGS_EVERY", 60);
    s_log.max_bytes = (unsigned long long)env_uint("XBOX_PASS_TAGS_MAXMB", 64) << 20;
    s_log.sig = 2166136261u;
    fprintf(s_log.f, "# pass_tags, log mode. full=%u every=%u max=%llu bytes\n"
                     "# G = H2 record(s) of a triple (view, group, ortho), n = count;"
                     " O = H3 call (ortho); tag = 32-bit value of the NOP marker\n",
            s_log.full, s_log.every, s_log.max_bytes);
    fprintf(stderr, "[PASS-TAGS] mode log -> %s\n", s_log.path);
    pass_tags_add_sink(&s_sink_log);
}

/* ── EMIT sink ───────────────────────────────────────────────────
 *
 * Each FRAME_BEGIN, GROUP and FRAME_END becomes an NV2A NOP
 * `0x00040100, tag` written into the game's pushbuffer, in place in the
 * stream: the translator (nv2a_pgraph_d3d11.c, case NV097_NO_OPERATION)
 * reads it in the exact order of the commands. ORTHO and GROUP_END are not
 * emitted.
 *
 * Called under the lock, on the owner thread, from a hook (H1 before/after
 * the original, H2 before): never in the middle of a D3D sequence. Only
 * inside an H1 frame (in_frame), and never while a pushbuffer is recorded
 * ([ctx+0xC] & 4). pt_dispatch saves and restores g_eax/g_ecx/g_edx around
 * the sinks; sub_0016B920 is stdcall, ret 4: it pops the argument and the
 * dummy return address. */

extern void sub_0016B920(void);     /* guarantees wp < limit (+0x200 margin) */

static struct {
    unsigned long long written, skipped_recording, skipped_noframe, skipped_noctx, esp_bad;
} s_emit;

static void emit_event(const pass_tag_event *ev)
{
    uint32_t tag, ctx, wp, esp0;

    if (ev->type != PT_EV_FRAME_BEGIN && ev->type != PT_EV_FRAME_END &&
        ev->type != PT_EV_GROUP && ev->type != PT_EV_HUD) return;
    tag = ev->type == PT_EV_HUD ? ev->extra : pass_tags_tag(ev);
    if (!tag) return;
    if (!s_st.in_frame) { s_emit.skipped_noframe++; return; }
    ctx = MEM32(0x001776C0u);
    if (!ctx) { s_emit.skipped_noctx++; return; }
    if (MEM32(ctx + 0x0Cu) & 4u) { s_emit.skipped_recording++; return; }

    esp0 = g_esp;
    PUSH32(g_esp, ctx);                 /* argument: the pushbuffer context */
    PUSH32(g_esp, 0);                   /* dummy return address */
    sub_0016B920();                     /* ret 4: pops the 8 bytes */
    if (g_esp != esp0) { s_emit.esp_bad++; g_esp = esp0; }
    wp = g_eax;
    MEM32(wp) = 0x00040100u;            /* NV097_NO_OPERATION, 1 parameter, subchannel 0 */
    MEM32(wp + 4u) = tag;
    MEM32(ctx) = wp + 8u;               /* published AFTER the data */
    s_emit.written++;
}

static void emit_flush(void)
{
    static unsigned n;
    if (++n % 10u) return;              /* ~ every 600 frames */
    fprintf(stderr, "[PASS-TAGS] emit: %llu markers written; skipped: %llu out of frame, "
            "%llu while recording, %llu without context; esp_bad %llu\n",
            s_emit.written, s_emit.skipped_noframe, s_emit.skipped_recording,
            s_emit.skipped_noctx, s_emit.esp_bad);
}

static const pass_tags_sink s_sink_emit = { "emit", emit_event, emit_flush };

void pass_tags_init(void)
{
    const char *e = getenv("XBOX_PASS_TAGS");
    int want_log, want_emit, split;
    InitializeCriticalSection(&s_lock);
    /* The split 3D / overlay post needs the markers. XBOX_POST_SPLIT=1
     * (or the XBOX_POST_CYCLE diagnostic) with a post on (XBOX_POST=1 or
     * XBOX_SMAA=1) turns emit on whatever XBOX_PASS_TAGS says; nothing is
     * emitted otherwise. */
    {
        const char *sp = getenv("XBOX_POST_SPLIT"), *cy = getenv("XBOX_POST_CYCLE");
        const char *po = getenv("XBOX_POST"), *sm = getenv("XBOX_SMAA");
        int post = (po && po[0] == '1') || (sm && sm[0] == '1');
        /* SMAA implies the split unless XBOX_POST_SPLIT=0 */
        int want_split = (sp && sp[0]) ? (sp[0] == '1') : (sm && sm[0] == '1');
        split = (want_split && post) || (cy && atoi(cy) > 0);
    }
    /* Proportional HUD needs the markers (phase + HUD tags). */
    if (g_hud_anchor_on || g_box_on || g_race2d_on || g_panel_on) split = 1;
    if (!split && (!e || !*e || !strcmp(e, "0") || !strcmp(e, "off"))) {
        g_pass_tags_mode = PASS_TAGS_OFF;
        return;
    }
    if (!e) e = "";
    want_log  = !strcmp(e, "log") || !strcmp(e, "log+emit") || !strcmp(e, "emit+log");
    want_emit = !strcmp(e, "emit") || !strcmp(e, "log+emit") || !strcmp(e, "emit+log");
    if (split && !want_emit) {
        want_emit = 1;
        fprintf(stderr, "[PASS-TAGS] emit turned on by %s\n", g_hud_anchor_on
                ? "the proportional HUD" : "XBOX_POST_SPLIT (3D post at the FRAME_END marker)");
    }
    if (!want_log && !want_emit) {
        fprintf(stderr, "[PASS-TAGS] XBOX_PASS_TAGS=%s unknown (0 | log | emit | log+emit): off\n", e);
        g_pass_tags_mode = PASS_TAGS_OFF;
        return;
    }
    if (want_log) log_open();
    if (want_emit) {
        pass_tags_add_sink(&s_sink_emit);
        fprintf(stderr, "[PASS-TAGS] emit mode: NOP markers 0x5358xxxx in the pushbuffer\n");
    }
    g_pass_tags_mode = !s_nsinks ? PASS_TAGS_OFF : want_emit ? PASS_TAGS_EMIT : PASS_TAGS_LOG;
}
