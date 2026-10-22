/* d3d8_null.c -- headless backend for the D3D8 / PGRAPH seam (OT_HEADLESS).
 *
 * Stands in for d3d8_*.c and nv2a_pgraph_d3d11.c. Nothing is drawn. What it keeps
 * is everything the *guest* can observe or wait for:
 *
 *   - frames: a surface flip (NV097_SET_SURFACE_COLOR_OFFSET changing) or, before
 *     the title has flipped at all, a CLEAR_SURFACE after draws, counts as one
 *     present, exactly where the Direct3D 11 translator presents. d3d8_PresentSeq()
 *     is that counter;
 *   - occlusion reports: NV097_GET_REPORT is completed on the spot -- the title
 *     spins on the 16-byte report's status word until the GPU writes it;
 *   - the guest framebuffer the movie player writes through LockRect, which can be
 *     dumped as PPM (XBOX_DUMP_FB=<dir>), the headless way to look at the picture.
 *
 * Everything else the translator and the host D3D layer expose (the OpenTricky
 * display options, the window, screenshots, shaders) is accepted and ignored.
 * nv2a_live_pb.c, the push-buffer ring walker, is the same file as on Windows and
 * drives this through the same calls.
 *
 * The entry points are the d3d8_* and pgraph_d3d11_* functions the rest of the
 * runtime and the port call; the list is in macos/d3d_seam.txt.
 */
#include <wchar.h>
#include <stdlib.h>
#include "platform/xbox_winnt.h"
#include "d3d8_xbox.h"
#include "../nv2a/nv2a_pgraph_d3d11.h"
#include "../nv2a/nv2a_regs.h"
#include "../nv2a/nv2a_btnicons.h"
#include "../nv2a/nv2a_hdtex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The title's NV097_GET_REPORT reports live in this address range's low 24 bits. */
#define NULL_REPORT_ZPASS 1u    /* report type: zpass pixel count */

/* ======================================================================= */
/* state                                                                   */
/* ======================================================================= */

/* referenced from nv2a_pb_replay.c / nv2a_live_pb.c (real definitions live in the D3D layer) */
volatile int g_suppress_present;
uint32_t     g_guest_fb_alt_va;

/* Draw fence: the last NOP 0x5C000000|n the stream passed. The port's pumplag.c (XBOX_FIX_CONSTWAIT)
 * rewrites a constant block only once this has reached the stamp it left there. */
volatile uint32_t pgraph_d3d11_fork_fence;

static uint8_t  *g_mem;                 /* guest RAM window, from set_mem_base */
static uint32_t  g_mem_size;
static int       g_initialized;

static uint32_t  g_surface_offset;      /* current NV097 colour surface offset */
static int       g_surface_seen;
static int       g_flip_mode;           /* the title flips surfaces */
static uint64_t  g_last_flip_ms;
static unsigned  g_draws_since_present;

static volatile unsigned g_present_seq;
static uint64_t  g_last_present_ms;
static unsigned  g_draw_total, g_clear_total, g_report_total;
static PgraphD3D11Stats g_stats;

/* guest framebuffer registered by the movie player's LockRect (recomp_manual.c) */
/* Where finished frames go, if anything shows them (the port's SDL window): RGBA, 4 bytes a pixel,
 * rows packed. Called on the thread that presents; the sink copies what it keeps. */
typedef void (*d3d8_frame_sink_fn)(const uint8_t *rgba, unsigned w, unsigned h);
static d3d8_frame_sink_fn g_frame_sink;
void d3d8_SetFrameSink(d3d8_frame_sink_fn fn) { g_frame_sink = fn; }

static const void *g_fb_src, *g_fb_alt;
static unsigned    g_fb_pitch, g_fb_w, g_fb_h;
static unsigned    g_fb_lock_seq, g_fb_locks;
#define GUEST_FB_IDLE_PRESENTS 30u

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t report_value(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("XBOX_NULL_ZPASS"); v = e ? atoi(e) : 256; }
    return (uint32_t)v;
}

/* ======================================================================= */
/* guest framebuffer dump                                                  */
/* ======================================================================= */

/* Cheap freshness probe and buffer choice, ported from d3d8_device.c
 * (guest_fb_sig / guest_fb_freshest): the movie player alternates between two
 * buffers, and a present must never show the one being blitted into right now
 * nor go back to an older frame. NULL = "hold". */
static unsigned guest_fb_sig(const void *base, unsigned pitch, unsigned h)
{
    const unsigned char *p = (const unsigned char *)base;
    unsigned sig = 2166136261u, y;
    if (!p) return 0;
    for (y = 0; y < h; y += 16) {
        const unsigned char *r = p + (size_t)y * pitch;
        for (unsigned k = 0; k < 64; k += 4) { sig ^= r[(pitch / 2) + k]; sig *= 16777619u; }
    }
    return sig;
}

static const void *guest_fb_freshest(void)
{
    static unsigned sig_a, sig_b, gen_a, gen_b, now, shown;
    static int seeded;
    const void *pick;
    unsigned a, b, cand_gen;
    int a_busy, b_busy;

    if (!g_fb_alt) return g_fb_src;
    a = guest_fb_sig(g_fb_src, g_fb_pitch, g_fb_h);
    b = guest_fb_sig(g_fb_alt, g_fb_pitch, g_fb_h);
    if (!seeded) { seeded = 1; sig_a = a; sig_b = b; return g_fb_src; }

    now++;
    if (a != sig_a) gen_a = now;
    if (b != sig_b) gen_b = now;
    sig_a = a; sig_b = b;
    a_busy = (gen_a == now);
    b_busy = (gen_b == now);

    if (a_busy && b_busy)      { pick = NULL;     cand_gen = 0; }
    else if (a_busy)           { pick = g_fb_alt; cand_gen = gen_b; }
    else if (b_busy)           { pick = g_fb_src; cand_gen = gen_a; }
    else if (gen_a >= gen_b)   { pick = g_fb_src; cand_gen = gen_a; }
    else                       { pick = g_fb_alt; cand_gen = gen_b; }

    if (pick && cand_gen < shown) pick = NULL;      /* would go backwards */
    else if (pick)                shown = cand_gen;
    return pick;
}

/* The guest framebuffer (X8R8G8B8: bytes B G R X) as RGBA for the window. NULL = hold the last frame. */
/* A GPU backend (d3d8_gpu.c) sets this: it gets each frame, the movie's RGBA or NULL for the 3D scene. */
void (*d3d8_present_hook)(const uint8_t *rgba, unsigned w, unsigned h);

static void post_guest_fb(const void *fb)
{
    static uint8_t *rgba; static size_t cap;
    unsigned x, y;
    size_t need = (size_t)g_fb_w * g_fb_h * 4;
    if ((!g_frame_sink && !d3d8_present_hook) || !fb || !g_fb_w || !g_fb_h) return;
    if (need > cap) { free(rgba); rgba = (uint8_t *)malloc(need); cap = rgba ? need : 0; }
    if (!rgba) return;
    for (y = 0; y < g_fb_h; y++) {
        const uint8_t *s = (const uint8_t *)fb + (size_t)y * g_fb_pitch;
        uint8_t *d = rgba + (size_t)y * g_fb_w * 4;
        for (x = 0; x < g_fb_w; x++) { d[x * 4] = s[x * 4 + 2]; d[x * 4 + 1] = s[x * 4 + 1]; d[x * 4 + 2] = s[x * 4]; d[x * 4 + 3] = 255; }
    }
    if (d3d8_present_hook) d3d8_present_hook(rgba, g_fb_w, g_fb_h);
    else g_frame_sink(rgba, g_fb_w, g_fb_h);
}

static void dump_guest_fb(const void *fb)
{
    static const char *dir; static int checked, max_frames = 60, dumped;
    static unsigned last_locks = ~0u;
    if (!checked) {
        const char *m = getenv("XBOX_DUMP_FB_MAX");
        dir = getenv("XBOX_DUMP_FB");
        if (m && *m) max_frames = atoi(m);
        checked = 1;
    }
    if (!dir || !*dir || dumped >= max_frames) return;
    if (!fb || !g_fb_w || !g_fb_h || g_fb_locks == last_locks) return;   /* a new decoded frame only */
    last_locks = g_fb_locks;

    char path[1024];
    snprintf(path, sizeof path, "%s/fb_%05u.ppm", dir, (unsigned)dumped);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", g_fb_w, g_fb_h);
    for (unsigned y = 0; y < g_fb_h; y++) {
        const uint8_t *row = (const uint8_t *)fb + (size_t)y * g_fb_pitch;
        for (unsigned x = 0; x < g_fb_w; x++) {          /* X8R8G8B8, little endian: B G R X */
            uint8_t px[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0] };
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    dumped++;
}

/* ======================================================================= */
/* d3d8_* : present, window, display options                               */
/* ======================================================================= */

int d3d8_GuestFramebufferActive(void);

/* One frame has been presented (called from the flip / clear paths below and,
 * as the video fallback, from nv2a_live_pb.c). */
void d3d8_PresentFrame(void)
{
    g_present_seq++;
    g_last_present_ms = now_ms();
    if (g_fb_src && d3d8_GuestFramebufferActive()) {   /* what the real present shows */
        const void *fb = guest_fb_freshest();
        dump_guest_fb(fb);
        post_guest_fb(fb);       /* a GPU backend presents the movie frame itself */
    } else if (d3d8_present_hook) {
        d3d8_present_hook(NULL, 0, 0);      /* the 3D scene */
    }
}

unsigned d3d8_PresentSeq(void)   { return g_present_seq; }
DWORD    d3d8_MsSincePresent(void) { return (DWORD)(now_ms() - g_last_present_ms); }
void     d3d8_PumpMessages(void) {}
int      d3d8_VideoPresentReady(void) { return 1; }

void d3d8_SetGuestFramebuffer(const void *src, unsigned pitch, unsigned w, unsigned h)
{ g_fb_src = src; g_fb_pitch = pitch; g_fb_w = w; g_fb_h = h; }
void d3d8_SetGuestFramebufferAlt(const void *alt) { g_fb_alt = alt; }
void d3d8_NoteGuestFramebufferLock(void) { g_fb_lock_seq = g_present_seq; g_fb_locks++; }
int  d3d8_HasGuestFramebuffer(void)      { return g_fb_src != NULL; }
unsigned d3d8_GuestFramebufferLockCount(void) { return g_fb_locks; }
int  d3d8_GuestFramebufferActive(void)
{
    return g_fb_src && g_fb_locks != 0 && (g_present_seq - g_fb_lock_seq) <= GUEST_FB_IDLE_PRESENTS;
}

/* The Metal backend (d3d8_gpu.c) saves the screenshot; without it (a headless build) the request is ignored. */
void d3d8_gpu_request_screenshot(const char *path) __attribute__((weak));
void d3d8_RequestScreenshot(const wchar_t *path)
{
    char buf[1024];
    if (!path || !d3d8_gpu_request_screenshot) return;
    if (wcstombs(buf, path, sizeof buf - 1) == (size_t)-1) return;
    buf[sizeof buf - 1] = 0;
    d3d8_gpu_request_screenshot(buf);
}
int  d3d8_ScreenshotPending(void)                { return 0; }

void d3d8_SetDacPalette(const unsigned char lut[256][3]) { (void)lut; }
void d3d8_SetShowFps(int on)                       { (void)on; }
void d3d8_SetMsaa(int samples)                     { (void)samples; }
void d3d8_SetHostUiHooks(const D3D8HostUiHooks *h) { (void)h; }

/* Host display configuration (set by the port before the title creates its device; read by the GPU backend and
 * the window). The semantics are those of the Windows layer (d3d8_xbox.h): render size of the scene target (0 =
 * default), the display shape, and whether the title was told it is widescreen. */
static unsigned s_cfg_rw, s_cfg_rh;
static int      s_cfg_wide, s_cfg_full;
static double   s_cfg_aspect;
static volatile int s_present_box;
static double   s_box_shape = 16.0 / 9.0;
static float    s_point_zoom = 1.0f;

void d3d8_SetHostDisplay(unsigned w, unsigned h, int widescreen, int fullscreen)
{ s_cfg_rw = w; s_cfg_rh = h; s_cfg_wide = widescreen ? 1 : 0; s_cfg_full = fullscreen ? 1 : 0; }
void d3d8_SetHostAspect(double aspect)             { s_cfg_aspect = aspect > 1.0 ? aspect : 0.0; }
double d3d8_HostAspect(void)
{
    if (s_cfg_aspect > 0.0) return s_cfg_aspect;
    return s_cfg_wide ? 16.0 / 9.0 : 4.0 / 3.0;
}
int  d3d8_HostIsWidescreen(void)                   { return s_cfg_wide; }
int  d3d8_HostWantsFullscreen(void)                { return s_cfg_full; }
void d3d8_SetPresentBox(int on)                    { s_present_box = on ? 1 : 0; }
int  d3d8_PresentBoxOn(void)                       { return s_present_box; }
void d3d8_SetPresentBoxShape(double shape)         { s_box_shape = shape > 1.0 ? shape : 16.0 / 9.0; }
double d3d8_PresentBoxShape(void)                  { return s_box_shape; }
void d3d8_SetPointZoom(float zoom)                 { s_point_zoom = zoom > 0.0f ? zoom : 1.0f; }
float d3d8_PointZoom(void)                         { return s_point_zoom; }

/* The size of the scene target: XBOX_RENDER's (set by the port) or, by default, 960 rows (2x the title's 480) at
 * the display shape: 1280x960 at 4:3. */
void d3d8_host_target_size(unsigned *w, unsigned *h)
{
    unsigned rw = s_cfg_rw, rh = s_cfg_rh;
    if (!rw || !rh) {
        rh = 960;
        rw = (unsigned)(rh * d3d8_HostAspect() + 0.5);
    }
    if (rw < 640) rw = 640;
    if (rh < 480) rh = 480;
    if (rw > 8192) rw = 8192;
    if (rh > 4320) rh = 4320;
    *w = rw & ~1u; *h = rh & ~1u;
}
void d3d8_SetAnisotropy(int n)                     { (void)n; }
#if !defined(OT_GPU)      /* the GPU backend (d3d8_gpu.c) runs the post chain at FRAME_END */
void d3d8_PassPhaseChanged(int o, int n, unsigned tag) { (void)o; (void)n; (void)tag; }
#endif
/* the window (port/src/ot_window.c) installs these when it owns a window */
void (*d3d8_host_fullscreen_hook)(int on);
int  (*d3d8_host_isfullscreen_hook)(void);
void d3d8_HostSetFullscreen(int on)                { if (d3d8_host_fullscreen_hook) d3d8_host_fullscreen_hook(on); }
int  d3d8_HostIsFullscreen(void)                   { return d3d8_host_isfullscreen_hook ? d3d8_host_isfullscreen_hook() : 0; }
void d3d8_HostExit(void)                           { exit(0); }
HWND d3d8_GetHostWindow(void)                      { return NULL; }
int  (*d3d8_monitor_hz_hook)(void);              /* the window (port/src/ot_window.c) reports its display's refresh rate */
int  d3d8_monitor_hz(HWND hwnd)                    { (void)hwnd; int hz = d3d8_monitor_hz_hook ? d3d8_monitor_hz_hook() : 60; return hz > 0 ? hz : 60; }
void d3d8_sync_set_pace(int fps)                   { (void)fps; }
#if !defined(OT_GPU)
int  d3d8_post_split_wanted(void)                  { return 0; }
#endif
void gpuprof_init(void)                            {}
int  d3d8_pump_alt_off(void)                       { return 0; }

/* occlusion: reports are completed at GET_REPORT, so nothing is ever outstanding */
void  d3d8_OcclusionFlush(void) {}
int   d3d8_OcclusionPoll(int h, unsigned long long *samples) { (void)h; if (samples) *samples = report_value(); return 1; }
float d3d8_OcclusionScale(void) { return 1.0f; }

#if !defined(OT_GPU)   /* a GPU build has a real device (d3d8_gpu.c) */
/* The host device the translator would draw into. The title never calls
 * Direct3DCreate8 (its D3D8 is linked into the game); nv2a_live_pb.c creates one
 * and only checks the result for NULL. The "device" is a non-NULL token that
 * nothing may dereference: xbox_GetD3DDevice() below returns NULL, so a caller
 * that guards with `if (dev)` before going through lpVtbl cannot crash on it
 * (the AddRef/Release slots of the objects handed out are NULL too). */
static IDirect3DDevice8 *const g_null_dev_token = (IDirect3DDevice8 *)(uintptr_t)0x1;

static HRESULT __stdcall null_CreateDevice(IDirect3D8 *self, UINT adapter, DWORD type, HWND wnd, DWORD flags,
                                           D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **out)
{
    (void)self; (void)adapter; (void)type; (void)wnd; (void)flags; (void)pp;
    *out = g_null_dev_token;
    return 0;   /* S_OK */
}

IDirect3D8 *xbox_Direct3DCreate8(UINT sdk)
{
    static IDirect3D8Vtbl vtbl;
    static IDirect3D8 obj;
    (void)sdk;
    memset(&vtbl, 0, sizeof vtbl);
    vtbl.CreateDevice = null_CreateDevice;
    obj.lpVtbl = &vtbl;
    return &obj;
}
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return NULL; }   /* no device: nothing draws */

#endif

#if !defined(OT_GPU)       /* the GPU backend (d3d8_gpu.c) builds real textures */
HRESULT d3d8_CreateTextureFromLevels(UINT w, UINT h, UINT levels, const void *const *bits,
                                     IDirect3DTexture8 **ppTex)
{ (void)w; (void)h; (void)levels; (void)bits; if (ppTex) *ppTex = NULL; return (HRESULT)0x8876086C; /* D3DERR_INVALIDCALL */ }
#endif

#if !defined(OT_GPU)   /* a GPU build uses the real translator (nv2a_pgraph_d3d11.c) */
/* ======================================================================= */
/* pgraph_d3d11_* : the NV2A method stream                                 */
/* ======================================================================= */

void pgraph_d3d11_init(void)     { g_initialized = 1; }
void pgraph_d3d11_shutdown(void) { g_initialized = 0; }
void pgraph_d3d11_set_mem_base(void *base, uint32_t size) { g_mem = (uint8_t *)base; g_mem_size = size; }
void pgraph_d3d11_tick_begin(void) {}
void pgraph_d3d11_poll_reports(void) {}
int  pgraph_d3d11_reports_pending(void) { return 0; }
unsigned pgraph_d3d11_draws_pending(void) { return g_draws_since_present; }
uint32_t pgraph_d3d11_draw_count(void) { return g_draw_total; }
int  pgraph_d3d11_take_frame_complete(void) { return 0; }

int pgraph_d3d11_flipping(void)
{
    return g_flip_mode && now_ms() - g_last_flip_ms < 250 && !d3d8_GuestFramebufferActive();
}

void pgraph_d3d11_flush(void) {}
void pgraph_d3d11_present(uint32_t crtc_start) { (void)crtc_start; d3d8_PresentFrame(); }
void pgraph_d3d11_present_guest_fb(uint32_t va, uint32_t pitch, uint32_t w, uint32_t h)
{ (void)va; (void)pitch; (void)w; (void)h; }
void pgraph_d3d11_test_quad(void) {}
/* Called every 5 s by nv2a_live_pb.c: the rate at which the title presents, the headless "frame rate". */
void pgraph_d3d11_report_nops(void)
{
    static uint64_t t0; static unsigned p0;
    uint64_t now = now_ms();
    unsigned p = g_present_seq;
    if (t0)
        fprintf(stderr, "[NULL] presents=%u (%.1f/s over %.1f s) draws=%u clears=%u reports=%u flip_mode=%d fb_locks=%u\n",
                p, (p - p0) * 1000.0 / (double)(now - t0 ? now - t0 : 1), (double)(now - t0) / 1000.0,
                g_draw_total, g_clear_total, g_report_total, g_flip_mode, g_fb_locks);
    t0 = now; p0 = p;
}
void pgraph_d3d11_report_occlusion(void) {}
void pgraph_d3d11_perf_counts(unsigned *draws, unsigned *methods, unsigned *verts)
{ if (draws) *draws = g_stats.draw_calls; if (methods) *methods = g_stats.methods_handled; if (verts) *verts = 0; }
void pgraph_d3d11_get_stats(PgraphD3D11Stats *out) { if (out) *out = g_stats; }

/* The title-side hooks and display options of the OpenTricky renderer: nothing draws. */
typedef int (*pgraph_plag_fn)(uint32_t q, uint32_t *cb, const uint8_t **snap);   /* declared in the port's pumplag.c */
void pgraph_d3d11_set_chyron_scroll(uint32_t frame) { (void)frame; }
void pgraph_d3d11_set_hud_scale(float kx, float ky) { (void)kx; (void)ky; }
void pgraph_d3d11_set_box(float k)                  { (void)k; }
void pgraph_d3d11_set_box_race2d(float k)           { (void)k; }
void pgraph_d3d11_set_box_frame(float k)            { (void)k; }
void pgraph_d3d11_set_plag_check(pgraph_plag_fn fn) { (void)fn; }
void pgraph_d3d11_set_pass_phase_callback(pgraph_pass_phase_fn fn) { (void)fn; }
void pgraph_d3d11_set_draw_subst(pgraph_draw_subst_fn fn)          { (void)fn; }
int  pgraph_d3d11_pass_phase(void) { return 0; }
void pgraph_d3d11_tex_state(int stage, uint32_t *format, uint32_t *filter, uint32_t *address,
                            uint32_t *control0, int *enabled)
{
    (void)stage;
    if (format) *format = 0;   if (filter) *filter = 0;
    if (address) *address = 0; if (control0) *control0 = 0;
    if (enabled) *enabled = 0;
}
void pgraph_d3d11_tex_binding(int stage, uint32_t *offset, uint32_t *format)
{ (void)stage; if (offset) *offset = 0; if (format) *format = 0; }
uint32_t pgraph_d3d11_tex_address(int stage) { (void)stage; return 0; }

/* diagnostics wired from the port's main.c */
void nv2a_skipprog_set(uint32_t h) { (void)h; }
void nv2a_ignored_dump(void)       {}
void nv2a_drawlog_arm(int frames)  { (void)frames; }

/* Reached through nv2a_live_pb.c (kept) and nv2a_core.c. Accept every method;
 * track only what the guest can see. */
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel;
    if (!g_initialized) return 0;     /* as the translator: not handled before init */
    g_stats.methods_handled++;

    switch (method) {
    case NV097_NO_OPERATION:
        if ((param >> 24) == 0x5Cu) pgraph_d3d11_fork_fence = param & 0xFFFFFFu;   /* the guest's draw fence */
        return 1;

    case NV097_SET_BEGIN_END:
        if (param != 0) { g_draws_since_present++; g_draw_total++; g_stats.draw_calls++; }
        return 1;

    case NV097_GET_REPORT:
        /* The GPU would write {timestamp, count, status = 0} at the report's
         * address; the title zeroed nothing but set status to 0xFFFFFFFF and
         * spins until it reads 0. Write the count, then (release) the status. */
        if ((param >> 24) == NULL_REPORT_ZPASS) {
            uint32_t addr = param & 0x00FFFFFFu;
            if (g_mem && addr + 16 <= g_mem_size) {
                uint8_t *p = g_mem + addr;
                uint64_t ts = now_ms();
                memcpy(p, &ts, 8);
                *(volatile uint32_t *)(p + 8) = report_value();
                __atomic_store_n((volatile uint32_t *)(p + 12), 0u, __ATOMIC_RELEASE);
                g_report_total++;
            }
        }
        return 1;

    case NV097_SET_SURFACE_COLOR_OFFSET:
        /* The title double-buffers by pointing the render surface at the other
         * buffer: the only frame boundary it gives. Present at the flip, and only
         * if something was drawn, as the translator does. */
        if (g_surface_seen && param != g_surface_offset) {
            g_flip_mode = 1;
            g_last_flip_ms = now_ms();
            if (g_draws_since_present > 0) {
                g_draws_since_present = 0;
                d3d8_PresentFrame();
            }
        }
        g_surface_offset = param;
        g_surface_seen = 1;
        return 1;

    case NV097_CLEAR_SURFACE:
        g_clear_total++; g_stats.clears++;
        /* A clear starts a frame: before the title has flipped (no flip seen
         * recently) whatever was drawn is a finished frame. */
        if (g_draws_since_present > 0 && !pgraph_d3d11_flipping()) {
            g_draws_since_present = 0;
            d3d8_PresentFrame();
        }
        return 1;

    default:
        return 1;
    }
}

#endif /* !OT_GPU */

/* ======================================================================= */
/* OpenTricky texture features that live in the dropped translator side    */
/* ======================================================================= */

#if !defined(OT_GPU)   /* the GPU build compiles the real ones (nv2a_btnicons.c, nv2a_hdtex.c) */
/* Button icons (nv2a_btnicons.c) and HD textures (nv2a_hdtex.c) replace what the
 * renderer samples. The port still configures and queries them: all off here. */
int btnicons_on(void)                 { return 0; }
int btnicons_wide_bumpers(void)       { return 0; }
int btnicons_bumper_text_dy(void)     { return 0; }
int btnicons_ps2(void)                { return 0; }
int btnicons_cfg_style(void)          { return 0; }
int btnicons_point(const IDirect3DTexture8 *t) { (void)t; return 0; }
IDirect3DTexture8 *btnicons_compose(const uint8_t *l0, uint32_t n0, uint32_t w, uint32_t h,
                                    uint32_t color, IDirect3DTexture8 *guest)
{ (void)l0; (void)n0; (void)w; (void)h; (void)color; (void)guest; return NULL; }
int btnicons_ps2_shape(int i, unsigned *shape, float uv[4], float game_wh[2], float ps2_wh[2])
{ (void)i; (void)shape; (void)uv; (void)game_wh; (void)ps2_wh; return 0; }
int btnicons_lesson_shape(int i, unsigned *shape, float game[6], float pic[6])
{ (void)i; (void)shape; (void)game; (void)pic; return 0; }
int btnicons_lesson_mark(int row, float *dx, float *dy)
{ (void)row; (void)dx; (void)dy; return 0; }
void hdtex_set_builtin_index(const char *text, size_t len) { (void)text; (void)len; }
#endif /* !OT_GPU */
