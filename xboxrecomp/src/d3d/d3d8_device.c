/**
 * D3D8→D3D11 Compatibility Device Implementation
 *
 * Implements the Xbox D3D8 IDirect3DDevice8 interface using D3D11.
 * The game's translated RenderWare code calls D3D8 methods through
 * COM vtables; this layer translates those calls to D3D11 equivalents.
 *
 * Architecture:
 * - D3D11 device and swap chain created during initialization
 * - Render state tracking: D3D8 states mapped to D3D11 state objects
 * - Texture/buffer management: D3D8 resource handles wrap D3D11 resources
 * - Fixed-function pipeline: emulated via D3D11 shaders (the Xbox D3D8
 *   pipeline is configurable but not fully programmable)
 *
 * Build: Requires Windows SDK with d3d11.h and dxgi.h
 */

#include <time.h>
#include "d3d8_internal.h"
#include "d3d8_nv2a_vsh.h"   /* the ghost's draws (fork) */
#include "d3d8_post.h"
#include "d3d8_gpuprof.h"
#include "d3d8_softshadow.h"
#include "d3d8_veil.h"
#include "d3d8_speedfx.h"
#include "d3d8_lights.h"
#include "d3d8_bloom.h"
#include "d3d8_haze_halo.h"
#include "d3d8_rscale.h"
#include "d3d8_temporal.h"
#include "d3d8_sync.h"
#include "../kernel/xbox_perf.h"
#include "../kernel/xbox_logstats.h"
#include <intrin.h>
#include <d3d11sdklayers.h>
#include <d3d11_4.h>         /* the visibility reports' fence */
#include <d3dcompiler.h>
#include <wincodec.h>
#include <stdio.h>
#include <string.h>

/* ================================================================
 * Internal device state
 * ================================================================ */

/* Maximum tracked render states, texture stages, and transforms */
#define MAX_RENDER_STATES    256
#define MAX_TEXTURE_STAGES   4
#define MAX_TSS_STATES       32
#define MAX_TRANSFORMS       512
#define MAX_LIGHTS           8

typedef struct D3D8DeviceState {
    /* D3D11 objects */
    ID3D11Device            *d3d11_device;
    ID3D11DeviceContext     *d3d11_context;
    IDXGISwapChain          *swap_chain;

    /* Default render targets */
    ID3D11RenderTargetView  *default_rtv;
    ID3D11DepthStencilView  *default_dsv;
    ID3D11Texture2D         *default_depth;
    ID3D11ShaderResourceView *depth_srv;     /* soft shadows on only */

    /* The title renders into an offscreen scene target (default_rtv
     * above) at the configured resolution; the swap chain is only the
     * window's output, and every present scales the scene into it. */
    ID3D11Texture2D          *scene_tex;      /* resolved, sampleable */
    ID3D11ShaderResourceView *scene_srv;
    ID3D11Texture2D          *scene_ms;       /* multisampled, when anti-aliasing */
    UINT                      msaa, msaa_requested;
    ID3D11Texture2D          *swap_tex;
    ID3D11RenderTargetView   *swap_rtv;
    UINT                      swap_w, swap_h;
    UINT                      guest_w, guest_h;   /* the title's own backbuffer size */

    /* Fork: render scale of the 3D (d3d8_rscale.h). The 3D of an image in
     * a race goes here, at rs_w x rs_h; the targets above stay at the
     * render size and get the rest of the image. Made only when the scale
     * is not 100 %. Depth always typeless with a shader view (the copy to
     * the render size and the soft shadows read it). */
    ID3D11Texture2D          *rs_tex;         /* resolved, sampleable */
    ID3D11ShaderResourceView *rs_srv;
    ID3D11Texture2D          *rs_ms;          /* multisampled, when anti-aliasing */
    ID3D11RenderTargetView   *rs_rtv;
    ID3D11Texture2D          *rs_depth;
    ID3D11DepthStencilView   *rs_dsv;
    ID3D11ShaderResourceView *rs_depth_srv;
    UINT                      rs_w, rs_h, rs_msaa;

    /* Window; width/height are the scene target's size */
    HWND                    hwnd;
    UINT                    width;
    UINT                    height;
    D3DFORMAT               backbuffer_format;

    /* State tracking */
    DWORD                   render_states[MAX_RENDER_STATES];
    DWORD                   tss[MAX_TEXTURE_STAGES][MAX_TSS_STATES];
    D3DMATRIX               transforms[MAX_TRANSFORMS];
    D3DVIEWPORT8            viewport;
    D3DMATERIAL8            material;
    D3DLIGHT8               lights[MAX_LIGHTS];
    BOOL                    light_enable[MAX_LIGHTS];

    /* Current shader/FVF */
    DWORD                   vertex_shader;
    DWORD                   pixel_shader;

    /* Scene state */
    BOOL                    in_scene;

    /* Reference count */
    LONG                    ref_count;
} D3D8DeviceState;

/* Global device instance (Xbox has a single D3D device) */
static D3D8DeviceState g_device_state;
static IDirect3DDevice8 g_device;
static BOOL g_device_initialized = FALSE;

/* The texture the title's frames are rendered into: the scene target, not the
 * swap chain. Frame dumps, hashes and readbacks read it so that a
 * capture is the rendered frame at the render resolution, whatever size the
 * window happens to be. AddRef'd, like IDXGISwapChain_GetBuffer. */
static HRESULT host_present(void);

static void scene_resolve(D3D8DeviceState *s);

/* ── Render scale of the 3D (fork; d3d8_rscale.h) ─────────────────────
 *
 * s_rs_pct is the scale applied: 100 (the default) and none of this runs --
 * the cur_* helpers then give the render-size targets, the very objects the
 * code used before, and the image is the title's, bit for bit.
 *
 * Otherwise, per presented image (the pass phases: nv2a_pgraph_d3d11.c):
 *   - at a present, when the image just shown carried pass markers (a race;
 *     menus, loading screens and videos carry none), the rs_* targets are
 *     bound for the next image (rs_bind): its 3D -- the main view's draws
 *     ahead of its first marker, groups 0-6, the post at FRAME_END, then the
 *     perspective groups >= 7 (clouds, lens flare and its occlusion test) --
 *     is drawn at the scaled size;
 *   - at its first orthographic group >= 7 (PGRAPH_PHASE_HUD: the HUD, the
 *     pause and result panels, the fades), rs_to_screen brings the colour to
 *     the render-size target (box above 100 %, bilinear below, never
 *     sharpened) and the depth with it, and binds that target: the 2D is
 *     drawn at the render size, over the 3D, with the title's own blending;
 *   - an image that never reaches a HUD group is brought down the same way
 *     at its present (or when anything reads the scene first: captures).
 * Viewport and window-clip / scissor rectangles follow the bound target
 * (d3d8_GetGuestScale); occlusion counts are scaled back to title pixels
 * with the scale of the target they were counted on. Pump thread only. */
#define RS_MAX_PIXELS  (3840u * 2160u * 4u)     /* 4K at 200 % */
#define RS_MAX_SAMPLES (RS_MAX_PIXELS * 4ull)   /* x anti-aliasing samples */
#define RS_MAX_SIDE    16384u                   /* D3D11 texture limit */
static UINT s_rs_pct = 100;     /* applied, percent; 100 = off */
static int  s_rs_bound;         /* the image being built draws into rs_* */
static int  s_rs_img;           /* ... and did from its start */
static int  s_rs_marked;        /* this image saw a pass marker */
static float s_occ_rs_scale;    /* occlusion: samples per title pixel at the last Begin */
static struct {
    unsigned long long at_hud, at_present, video, native, depth_fail;
} s_rs_stats;

static ID3D11RenderTargetView *cur_rtv(const D3D8DeviceState *s)
    { return s_rs_bound ? s->rs_rtv : s->default_rtv; }
static ID3D11DepthStencilView *cur_dsv(const D3D8DeviceState *s)
    { return s_rs_bound ? s->rs_dsv : s->default_dsv; }
static ID3D11Texture2D *cur_depth(const D3D8DeviceState *s)
    { return s_rs_bound ? s->rs_depth : s->default_depth; }
static ID3D11ShaderResourceView *cur_depth_srv(const D3D8DeviceState *s)
    { return s_rs_bound ? s->rs_depth_srv : s->depth_srv; }
static ID3D11Texture2D *cur_tex(const D3D8DeviceState *s)
    { return s_rs_bound ? s->rs_tex : s->scene_tex; }
static ID3D11ShaderResourceView *cur_srv(const D3D8DeviceState *s)
    { return s_rs_bound ? s->rs_srv : s->scene_srv; }
static UINT cur_w(const D3D8DeviceState *s) { return s_rs_bound ? s->rs_w : s->width; }
static UINT cur_h(const D3D8DeviceState *s) { return s_rs_bound ? s->rs_h : s->height; }
static UINT cur_msaa(const D3D8DeviceState *s) { return s_rs_bound ? s->rs_msaa : s->msaa; }

/* Bring the sampleable copy of the bound target up to date. */
static void cur_resolve(D3D8DeviceState *s)
{
    if (!s_rs_bound) { scene_resolve(s); return; }
    if (s->rs_ms && s->rs_tex)
        ID3D11DeviceContext_ResolveSubresource(s->d3d11_context,
            (ID3D11Resource *)s->rs_tex, 0, (ID3D11Resource *)s->rs_ms, 0,
            DXGI_FORMAT_R8G8B8A8_UNORM);
}

static int rs_fits(UINT w, UINT h, UINT msaa)
{
    unsigned long long px = (unsigned long long)w * h;
    return w <= RS_MAX_SIDE && h <= RS_MAX_SIDE && px <= RS_MAX_PIXELS &&
           px * (msaa ? msaa : 1u) <= RS_MAX_SAMPLES;
}

/* The scale and the scaled size, from XBOX_RENDER_SCALE and the render size
 * (s->width x s->height, which never change). Above 100 % the next step
 * down is taken while the budget is exceeded (200 -> 175 -> ... -> 100). */
static void rs_choose(D3D8DeviceState *s)
{
    int want = d3d8_rscale_wanted(), n, i;
    const int *st = d3d8_rscale_steps(&n);
    s_rs_pct = 100;
    s->rs_w = s->rs_h = 0;
    if (want == 100 || !s->width || !s->height) return;
    for (i = n - 1; i >= 0; i--) {
        UINT p = (UINT)st[i], w, h;
        if ((int)p > want || (want > 100 && p <= 100)) continue;
        if (want < 100 && (int)p != want) continue;
        w = (s->width * p + 50u) / 100u;
        h = (s->height * p + 50u) / 100u;
        if (w < 1u) w = 1u;
        if (h < 1u) h = 1u;
        if (rs_fits(w, h, 1u)) {
            s_rs_pct = p;
            s->rs_w = w;
            s->rs_h = h;
            break;
        }
    }
    fprintf(stderr, "D3D8: render scale %u %% (asked %d %%): 3D at %ux%u, render size %ux%u\n",
            s_rs_pct, want, s_rs_pct != 100 ? s->rs_w : s->width,
            s_rs_pct != 100 ? s->rs_h : s->height, s->width, s->height);
    fflush(stderr);
}

static void rs_targets_release(D3D8DeviceState *s)
{
    if (s->rs_depth_srv) { ID3D11ShaderResourceView_Release(s->rs_depth_srv); s->rs_depth_srv = NULL; }
    if (s->rs_dsv)       { ID3D11DepthStencilView_Release(s->rs_dsv); s->rs_dsv = NULL; }
    if (s->rs_depth)     { ID3D11Texture2D_Release(s->rs_depth); s->rs_depth = NULL; }
    if (s->rs_rtv)       { ID3D11RenderTargetView_Release(s->rs_rtv); s->rs_rtv = NULL; }
    if (s->rs_srv)       { ID3D11ShaderResourceView_Release(s->rs_srv); s->rs_srv = NULL; }
    if (s->rs_ms)        { ID3D11Texture2D_Release(s->rs_ms); s->rs_ms = NULL; }
    if (s->rs_tex)       { ID3D11Texture2D_Release(s->rs_tex); s->rs_tex = NULL; }
    s->rs_msaa = 0;
}

/* The scaled targets, with the render-size target's anti-aliasing held to
 * the budget. Any failure: everything released and the scale back to 100. */
static void rs_targets_create(D3D8DeviceState *s)
{
    static const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D11Device *dev = s->d3d11_device;
    D3D11_TEXTURE2D_DESC td;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    UINT msaa = s->msaa ? s->msaa : 1u;

    rs_targets_release(s);
    if (s_rs_pct == 100 || !dev || !s->rs_w || !s->rs_h) return;
    while (msaa > 1u && !rs_fits(s->rs_w, s->rs_h, msaa)) msaa >>= 1;
    if (!d3d8_rscale_init(dev)) goto fail;

    memset(&td, 0, sizeof td);
    td.Width = s->rs_w;
    td.Height = s->rs_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &s->rs_tex))) goto fail;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)s->rs_tex,
                                                     NULL, &s->rs_srv))) goto fail;
    if (msaa > 1u) {
        td.SampleDesc.Count = msaa;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &s->rs_ms))) goto fail;
    }
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev,
            (ID3D11Resource *)(s->rs_ms ? s->rs_ms : s->rs_tex), NULL, &s->rs_rtv))) goto fail;

    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = msaa;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &s->rs_depth))) goto fail;
    memset(&dv, 0, sizeof dv);
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = msaa > 1u ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)s->rs_depth,
                                                   &dv, &s->rs_dsv))) goto fail;
    memset(&sv, 0, sizeof sv);
    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    if (msaa > 1u) sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    else { sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1; }
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)s->rs_depth,
                                                     &sv, &s->rs_depth_srv))) goto fail;

    ID3D11DeviceContext_ClearRenderTargetView(s->d3d11_context, s->rs_rtv, black);
    ID3D11DeviceContext_ClearDepthStencilView(s->d3d11_context, s->rs_dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    s->rs_msaa = msaa;
    fprintf(stderr, "D3D8: render scale targets %ux%u, %ux AA\n", s->rs_w, s->rs_h, msaa);
    fflush(stderr);
    return;
fail:
    rs_targets_release(s);
    fprintf(stderr, "D3D8: render scale %u %%: targets not made, 100 %%\n", s_rs_pct);
    fflush(stderr);
    s_rs_pct = 100;
}

/* The viewport from one bound size to the other: the whole target stays the
 * whole target (exact); anything else is scaled by the ratio. */
static void rs_viewport_move(ID3D11DeviceContext *ctx, UINT ow, UINT oh, UINT nw, UINT nh)
{
    D3D11_VIEWPORT vp;
    UINT n = 1;
    ID3D11DeviceContext_RSGetViewports(ctx, &n, &vp);
    if (!n || !ow || !oh || (vp.TopLeftX == 0.0f && vp.TopLeftY == 0.0f &&
                             vp.Width == (FLOAT)ow && vp.Height == (FLOAT)oh)) {
        if (!n) { vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f; }
        vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
        vp.Width = (FLOAT)nw; vp.Height = (FLOAT)nh;
    } else {
        float kx = (float)nw / (float)ow, ky = (float)nh / (float)oh;
        vp.TopLeftX *= kx; vp.Width *= kx;
        vp.TopLeftY *= ky; vp.Height *= ky;
    }
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
}

/* At a present: the next image's 3D goes to the scaled targets. */
static void rs_bind(D3D8DeviceState *s)
{
    ID3D11DeviceContext *ctx = s->d3d11_context;
    if (s_rs_pct == 100 || s_rs_bound || !ctx || !s->rs_rtv || !s->rs_dsv) return;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s->rs_rtv, s->rs_dsv);
    rs_viewport_move(ctx, s->width, s->height, s->rs_w, s->rs_h);
    s_rs_bound = 1;
}

/* The 3D to the render-size targets, which are bound from here on. keep = 0
 * (a video frame replaces the image): nothing is copied. */
static void rs_to_screen(D3D8DeviceState *s, int at_hud, int keep)
{
    ID3D11DeviceContext *ctx = s->d3d11_context;
    if (!s_rs_bound) return;
    s_rs_bound = 0;
    if (!ctx || !s->default_rtv) return;
    if (keep) {
        if (g_gpuprof_on) gpuprof_begin(GP_RSCALE);
        if (s->rs_ms && s->rs_tex)
            ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *)s->rs_tex, 0,
                (ID3D11Resource *)s->rs_ms, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        d3d8_rscale_resample(s->d3d11_device, ctx, s->rs_srv, s->rs_w, s->rs_h,
                             s->default_rtv, s->width, s->height);
        if (s->default_dsv &&
            !d3d8_rscale_depth_copy(s->d3d11_device, ctx, s->rs_depth_srv, s->rs_msaa > 1u,
                                    s->rs_w, s->rs_h, s->default_dsv, s->width, s->height)) {
            ID3D11DeviceContext_ClearDepthStencilView(ctx, s->default_dsv,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            s_rs_stats.depth_fail++;
        }
        if (g_gpuprof_on) gpuprof_end();
        if (at_hud) s_rs_stats.at_hud++; else s_rs_stats.at_present++;
    } else {
        s_rs_stats.video++;
    }
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s->default_rtv, s->default_dsv);
    rs_viewport_move(ctx, s->rs_w, s->rs_h, s->width, s->height);
}

/* Before anything reads the scene target outside the image's own order
 * (captures, the video upload): the 3D brought down first. */
static void rs_finish(D3D8DeviceState *s)
{
    if (s_rs_bound) rs_to_screen(s, 0, 1);
}

/* End of a present: counts, then the next image's targets. */
static void rs_next_image(D3D8DeviceState *s)
{
    int marked = s_rs_marked;
    if (s_rs_pct == 100) return;
    if (!s_rs_img) s_rs_stats.native++;      /* menus, loading screens, videos */
    {
        unsigned long long n = s_rs_stats.at_hud + s_rs_stats.at_present + s_rs_stats.video +
                               s_rs_stats.native;
        if (n && n % 600 == 0)
            fprintf(stderr, "[RSCALE] %llu images: %llu brought down at the HUD, %llu at the "
                    "present, %llu replaced by a video, %llu at the render size; depth copy "
                    "failed %llu\n", n, s_rs_stats.at_hud, s_rs_stats.at_present,
                    s_rs_stats.video, s_rs_stats.native, s_rs_stats.depth_fail);
    }
    s_rs_marked = 0;
    s_rs_img = 0;
    if (marked) {
        rs_bind(s);
        s_rs_img = s_rs_bound;
    }
}

static HRESULT scene_texture_get(ID3D11Texture2D **out)
{
    rs_finish(&g_device_state);
    scene_resolve(&g_device_state);
    *out = g_device_state.scene_tex;
    if (!*out) return E_FAIL;
    ID3D11Texture2D_AddRef(*out);
    return S_OK;
}

/* The image the present shows. With the post chain off (the
 * default) that is scene_tex itself and nothing below changes. On, it is the
 * chain's output for this present -- never scene_tex, which the "Master"
 * chrome (prev_frame_update) and the guest-framebuffer upload keep using.
 * s_post_frame identifies the present being built: captures taken before
 * host_present and host_present itself share it, so the chain runs once. A
 * video frame (the guest framebuffer) is shown as it is. */
static unsigned s_post_frame = 1;
static unsigned s_frames_cycle_base(void) { return s_post_frame; }
static int      s_video_frame;      /* set by d3d8_PresentGuestFramebuffer */
/* The post already ran on the 3D of the image being built, at its
 * FRAME_END marker (split 3D / overlay, d3d8_PassPhaseChanged): the scene
 * target holds post(3D) + overlay, shown as it is. Cleared by host_present. */
static int      s_split_done;
static int      s_cycle_mode = -1;  /* XBOX_POST_CYCLE: mode of the image being built */
static struct {
    unsigned long long presents, split, fallback, none, doubles, video;
} s_split_stats;

static void present_source(ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv)
{
    D3D8DeviceState *s = &g_device_state;
    ID3D11Texture2D *pt;
    ID3D11ShaderResourceView *ps;
    D3D11_TEXTURE2D_DESC td;
    *tex = s->scene_tex;
    *srv = s->scene_srv;
    if (!d3d8_post_enabled() || s_video_frame || !s->scene_tex || !s->scene_srv) return;
    if (s_split_done) return;       /* post done at FRAME_END, overlay drawn over it */
    ID3D11Texture2D_GetDesc(s->scene_tex, &td);
    if (g_gpuprof_on) gpuprof_begin(GP_POST_PRESENT);
    if (d3d8_post_run(s->d3d11_device, s->d3d11_context, s->scene_tex, s->scene_srv,
                      td.Width, td.Height, s_post_frame, &pt, &ps)) {
        *tex = pt;
        *srv = ps;
    }
    if (g_gpuprof_on) gpuprof_end();
}

/* Fork: the image's lights (d3d8_lights.h), the selective bloom
 * (d3d8_bloom.h), onto the bound 3D target, before the HUD: at the first
 * change into the HUD phase, or at the present for an image without one.
 * Once per image; nothing when no light was drawn (bloom off, the default). */
static void lights_apply(D3D8DeviceState *s)
{
    ID3D11ShaderResourceView *src[3] = { NULL, NULL, NULL };
    if (!d3d8_lights_pending() || !s->d3d11_context || !cur_rtv(s)) return;
    if (g_gpuprof_on) gpuprof_begin(GP_LIGHTS);
    if (d3d8_lights_take(s->d3d11_context, cur_w(s), cur_h(s), src))
        d3d8_bloom_composite(s->d3d11_device, s->d3d11_context, src[0], src[1], src[2], cur_depth_srv(s),
                             cur_rtv(s), cur_w(s), cur_h(s));
    if (src[0]) d3d8_haze_halo_composite(s->d3d11_device, s->d3d11_context, src[0], src[1], src[2],  /* the lights in the haze */
                                         cur_depth_srv(s), cur_rtv(s), cur_w(s), cur_h(s), cur_w(s), cur_h(s));
    d3d8_vignette_image_done();         /* the image's vignette read by the composites */
    if (g_gpuprof_on) gpuprof_end();
}

ID3D11RenderTargetView *d3d8_Get3DRTV(void) { return cur_rtv(&g_device_state); }

/* Split 3D / overlay. Called by the translator on the pump
 * thread when a pass_tags marker changes the render phase (registered by
 * main.c with pgraph_d3d11_set_pass_phase_callback when XBOX_POST_SPLIT or
 * XBOX_POST_CYCLE is set). FRAME_END (new_phase == 5, PGRAPH_PHASE_END) is
 * the end of the 3D of the image the next flip presents: the scene
 * target then holds that 3D and nothing else. Resolve it, run the post
 * chain, and write the result back into the render target -- a full-screen
 * identity draw, so a multisampled target gets every sample -- before the
 * title draws the overlay (groups >= 7: mist, lens flare, HUD) over it with
 * its own blending. Once per image: a second FRAME_END before the present is
 * counted and left alone, never posted twice. */
static void post_at_frame_end(D3D8DeviceState *s, unsigned tag);
static int  s_tmp_marked;       /* fork: this image saw a pass marker (temporal jitter) */

void d3d8_PassPhaseChanged(int old_phase, int new_phase, unsigned tag)
{
    D3D8DeviceState *s = &g_device_state;
    int tmp = d3d8_temporal_active() && !s_video_frame;     /* fork: temporal inputs */
    (void)old_phase;
    s_tmp_marked = 1;
    if (new_phase == 4 /* PGRAPH_PHASE_HUD */) d3d8_temporal_jitter_off();
    if (new_phase == 4 /* PGRAPH_PHASE_HUD */ && !s_video_frame)
        lights_apply(s);                /* fork: the bloom before the HUD */
    if (s_rs_pct != 100) {              /* fork: render scale of the 3D */
        s_rs_marked = 1;
        if (new_phase == 4 /* PGRAPH_PHASE_HUD */ && s_rs_bound && !s_video_frame)
            rs_to_screen(s, 1, 1);
    }
    if (new_phase != 5 /* PGRAPH_PHASE_END */) return;
    /* fork: the temporal inputs of the 3D (d3d8_temporal.h): the jitter
     * off, the motion vectors and the reactive mask, before the post */
    if (tmp && s->d3d11_context)
        d3d8_temporal_frame_end(s->d3d11_device, s->d3d11_context, cur_depth_srv(s), cur_depth(s),
                                cur_w(s), cur_h(s), tag);
    else d3d8_temporal_jitter_off();
    if (tmp && s->d3d11_context && d3d8_temporal_probing() && cur_tex(s)) {    /* diagnostic */
        cur_resolve(s);
        d3d8_temporal_probe(s->d3d11_device, s->d3d11_context, cur_tex(s));
    }
    if (d3d8_post_split_active() && !s_video_frame)
        post_at_frame_end(s, tag);
    if (tmp && s->d3d11_context && cur_rtv(s)) {
        d3d8_temporal_show(s->d3d11_device, s->d3d11_context, cur_rtv(s), cur_w(s), cur_h(s));
        /* the target switches here: what is drawn after FRAME_END is at the
         * output size (where a super-resolution pass will put its output); the lights
         * of the 3D first, at the size they were drawn at */
        if (s_rs_bound) {
            lights_apply(s);
            rs_to_screen(s, 1, 1);
            d3d8_temporal_set_moved();
        }
    }
}

/* The post chain on the 3D at FRAME_END (split 3D / overlay). */
static void post_at_frame_end(D3D8DeviceState *s, unsigned tag)
{
    ID3D11Texture2D *pt;
    ID3D11ShaderResourceView *ps;
    D3D11_TEXTURE2D_DESC td;
    /* the bound target: the scaled one when the render scale is on */
    if (!s->d3d11_context || !cur_tex(s) || !cur_srv(s) || !cur_rtv(s)) return;
    if (s_split_done) { s_split_stats.doubles++; return; }
    if (g_gpuprof_on) gpuprof_begin(GP_POST_SPLIT);
    cur_resolve(s);
    ID3D11Texture2D_GetDesc(cur_tex(s), &td);
    d3d8_post_split_call(1);
    d3d8_speedfx_set_depth(cur_depth_srv(s), cur_depth(s));    /* fork: speed effect */
    d3d8_speedfx_set_tag(tag);          /* the image's own camera, by its frame number */
    if (d3d8_post_run(s->d3d11_device, s->d3d11_context, cur_tex(s), cur_srv(s),
                      td.Width, td.Height, s_post_frame, &pt, &ps)
        && d3d8_post_copy_into(s->d3d11_device, s->d3d11_context, cur_rtv(s), ps,
                               td.Width, td.Height))
        s_split_done = 1;           /* else chain off or failed: the present falls back */
    d3d8_post_split_call(0);
    if (g_gpuprof_on) gpuprof_end();
}

/* Soft shadows. Called by the translator just before the title's
 * shadow quad (stencil-tested, blend ZERO / SRC_ALPHA, see d3d8_softshadow.h):
 * runs the mask / blur / apply passes on the scene target. Returns 1 when the
 * shadow is applied -- the translator then draws the quad without colour
 * writes, only to zero the stencil as before. */
int d3d8_SoftShadowOn(void) { return d3d8_softshadow_enabled(); }

int d3d8_SoftShadowDraw(float shade, unsigned cmp, unsigned ref, unsigned rmask,
                        float fx0, float fy0, float fx1, float fy1)
{
    D3D8DeviceState *s = &g_device_state;
    static unsigned long long done, fails;
    D3D11_TEXTURE2D_DESC td;
    RECT rc;
    int ok;
    static int alt = -1;
    if (!d3d8_softshadow_enabled() || !s->d3d11_context || !cur_rtv(s) ||
        !cur_dsv(s) || !cur_depth_srv(s) || !cur_depth(s))
        return 0;
    /* XBOX_SOFT_SHADOWS_ALT=1 (test): soft on even presents only, so that
     * consecutive captures of one run compare hard and soft. */
    if (alt < 0) { const char *e = getenv("XBOX_SOFT_SHADOWS_ALT"); alt = e && e[0] == '1'; }
    if (alt && (d3d8_PresentSeq() & 1u)) return 0;
    ID3D11Texture2D_GetDesc(cur_depth(s), &td);     /* the bound target's (render scale) */
    rc.left = (LONG)(fx0 * (float)td.Width);
    rc.top = (LONG)(fy0 * (float)td.Height);
    rc.right = (LONG)(fx1 * (float)td.Width + 0.999f);
    rc.bottom = (LONG)(fy1 * (float)td.Height + 0.999f);
    if (rc.left < 0) rc.left = 0;
    if (rc.top < 0) rc.top = 0;
    if (rc.right > (LONG)td.Width) rc.right = (LONG)td.Width;
    if (rc.bottom > (LONG)td.Height) rc.bottom = (LONG)td.Height;
    if (g_gpuprof_on) gpuprof_begin(GP_SOFTSHADOW);
    ok = d3d8_softshadow_run(s->d3d11_device, s->d3d11_context, cur_rtv(s), cur_dsv(s),
                             cur_depth_srv(s), td.Width, td.Height, td.SampleDesc.Count,
                             shade, cmp, ref, rmask, &rc);
    if (g_gpuprof_on) gpuprof_end();
    if (ok) done++; else fails++;
    if (done == 1 && ok)
        fprintf(stderr, "[SOFTSHADOW] on: %ux%u, %u sample(s), shade %.3f, edge width %d %%\n",
                td.Width, td.Height, td.SampleDesc.Count, shade, d3d8_softshadow_width());
    if (fails == 1 && !ok) { fprintf(stderr, "[SOFTSHADOW] failed, title's quad kept\n"); fails++; }
    return ok;
}

/* Fork: distance veil (d3d8_veil.h). Called by the translator: the snapshot
 * once the sky dome is drawn, the pass once per image after the opaque
 * world; k maps a scene pixel (x, y, depth, 1) to the sky dome's
 * coordinates. Both on the bound target (the scaled one with the render
 * scale). XBOX_VEIL_ALT=1: even images only. */
static int s_veil_last;             /* applied to the image being drawn */
int d3d8_VeilWanted(void) { return d3d8_veil_wanted(); }
int d3d8_VeilOn(void) { return d3d8_veil_on(); }
int d3d8_VeilAmbOn(void) { return d3d8_veil_amb_on(); }
int d3d8_VeilProbe(void) { return d3d8_veil_probe(); }
void d3d8_VeilPrepare(void)
{
    D3D8DeviceState *s = &g_device_state;
    if (!d3d8_veil_wanted() || !s->d3d11_device) return;
    d3d8_veil_prepare(s->d3d11_device);
    d3d8_veil_pretargets(s->d3d11_device, cur_rtv(s));
}

int d3d8_VeilSnapshot(void)
{
    D3D8DeviceState *s = &g_device_state;
    if (!d3d8_veil_wants_bg() || !s->d3d11_context || !cur_rtv(s)) return 0;
    return d3d8_veil_snapshot(s->d3d11_device, s->d3d11_context, cur_rtv(s));
}

int d3d8_VeilTerrainDepth(void)
{
    D3D8DeviceState *s = &g_device_state;
    if (!s->d3d11_context || !cur_depth(s)) return 0;
    return d3d8_veil_terrain_depth(s->d3d11_device, s->d3d11_context, cur_depth(s));
}

int d3d8_VeilLimit(float *limit, float *cell)
{
    d3d8_veil_cull c;
    int ok = d3d8_VeilCull(&c);
    *limit = ok ? c.limit : 0.0f;
    *cell = ok ? c.cell : 0.0f;
    return ok;
}

int d3d8_VeilDraw(const float k[16], const float eye[3], float clear_depth,
                  float near_z, float far_z, const float fog_rgb[3], float fog_rho,
                  const float *kw, const float eye_w[3])
{
    D3D8DeviceState *s = &g_device_state;
    static unsigned long long done;
    static int alt = -1;
    D3D11_TEXTURE2D_DESC td;
    d3d8_veil_frame f;
    int ok;
    s_veil_last = 0;
    if ((!d3d8_veil_on() && !d3d8_veil_amb_on()) || !s->d3d11_context || !cur_rtv(s) ||
        !cur_depth_srv(s) || !cur_depth(s))
        return 0;
    if (alt < 0) { const char *e = getenv("XBOX_VEIL_ALT"); alt = e && e[0] == '1'; }
    if (alt && (d3d8_PresentSeq() & 1u)) return 0;
    memset(&f, 0, sizeof f);
    f.have_cull = d3d8_VeilCull(&f.cull);
    ID3D11Texture2D_GetDesc(cur_depth(s), &td);
    memcpy(f.k, k, sizeof f.k);
    memcpy(f.eye, eye, sizeof f.eye);
    f.clear_depth = clear_depth;
    f.near_z = near_z;
    f.far_z = far_z;
    memcpy(f.fog_rgb, fog_rgb, sizeof f.fog_rgb);
    f.fog_rho = fog_rho;
    if (kw) {
        f.have_world = 1;
        memcpy(f.kw, kw, sizeof f.kw);
        memcpy(f.eye_w, eye_w, sizeof f.eye_w);
    }
    if (g_gpuprof_on) gpuprof_begin(GP_SOFTSHADOW);
    ok = d3d8_veil_run(s->d3d11_device, s->d3d11_context, cur_rtv(s), cur_depth_srv(s),
                       td.Width, td.Height, td.SampleDesc.Count, &f);
    if (g_gpuprof_on) gpuprof_end();
    s_veil_last = ok;
    if (ok && done++ == 0)
        fprintf(stderr, "[VEIL] first pass: %ux%u, %u sample(s), planes %.1f / %.0f, cell window %s "
                "(limit %.0f, cell %.0f, radius %.0f)\n", td.Width, td.Height, td.SampleDesc.Count,
                near_z, far_z, f.have_cull ? "read" : "not read", f.cull.limit, f.cull.cell, f.cull.radius);
    return ok;
}

/* The bound scene's size, for the translator's pixel -> guest mapping. */
int d3d8_SceneSize(unsigned *w, unsigned *h)
{
    D3D8DeviceState *s = &g_device_state;
    D3D11_TEXTURE2D_DESC td;
    if (!cur_depth(s)) return 0;
    ID3D11Texture2D_GetDesc(cur_depth(s), &td);
    *w = td.Width; *h = td.Height;
    return 1;
}

/* scene_texture_get for captures of what is shown (dumps, hashes,
 * screenshots): the post chain's output when it runs. AddRef'd. */
static HRESULT shown_texture_get(ID3D11Texture2D **out)
{
    ID3D11ShaderResourceView *srv;
    HRESULT hr = scene_texture_get(out);
    if (FAILED(hr) || !d3d8_post_enabled()) return hr;
    ID3D11Texture2D_Release(*out);
    present_source(out, &srv);
    if (!*out) return E_FAIL;
    ID3D11Texture2D_AddRef(*out);
    return S_OK;
}

/* Current resource bindings */
static IDirect3DVertexBuffer8 *g_cur_vb = NULL;
static UINT                    g_cur_vb_stride = 0;
static IDirect3DIndexBuffer8  *g_cur_ib = NULL;
static UINT                    g_cur_ib_base_vertex = 0;
static IDirect3DBaseTexture8  *g_cur_textures[4] = { NULL };

/* Forward declarations */
static const IDirect3DDevice8Vtbl g_device_vtbl;
static void up_ring_shutdown(void);

/* ================================================================
 * Public frame pump (called from recompiled game code)
 * ================================================================ */
/* Pump the window's message queue without presenting.
 *
 * Presenting is driven by the title's frame boundary now, but the window still
 * has to stay responsive between boundaries, and the two must not be welded
 * together: presenting on a timer to keep messages flowing is what put a
 * freshly cleared buffer on screen. */
static DWORD g_last_present_tick = 0;

/* Milliseconds since the last actual Present, from any caller. The pump uses
 * this so its safety-net present only fires when the title has genuinely
 * stopped producing frames, rather than on a fixed period that would cut in
 * between a frame's clear and its draw. */
DWORD d3d8_MsSincePresent(void)
{
    DWORD now = GetTickCount();
    if (g_last_present_tick == 0) g_last_present_tick = now;
    return now - g_last_present_tick;
}

/* Sample the back buffer at an arbitrary point, for diagnosis. */
/* Write the whole back buffer out as a .bmp.
 *
 * Sampling two pixels is enough to tell a cleared buffer from a filled one,
 * but not to tell whether something small -- a line of text, say -- was drawn
 * somewhere in between. XBOX_D3D_DUMP=<path-prefix> saves complete frames so
 * the question can be answered by looking rather than inferring. */
/*
 * Cheap fingerprint of the current back buffer, or 0 if it cannot be read.
 *
 * Exists for XBOX_D3D_DUMP_ONCHANGE. Catching something that appears for a
 * single frame otherwise means dumping every present -- hundreds of identical
 * 900 KB images of a static screen, and the interesting one buried among them.
 * Sampling a sparse grid is enough: two frames that differ anywhere a person
 * could see will differ here, and an unchanged screen hashes identically.
 */
unsigned d3d8_BackbufferHash(void)
{
    ID3D11Texture2D *back = NULL, *stage = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned hash = 0;

    if (!g_device_state.swap_chain || !g_device_state.d3d11_device) return 0;
    if (FAILED(shown_texture_get(&back)) || !back) return 0;
    ID3D11Texture2D_GetDesc(back, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
            &td, NULL, &stage)) || !stage) { ID3D11Texture2D_Release(back); return 0; }
    ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
        (ID3D11Resource *)stage, (ID3D11Resource *)back);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_device_state.d3d11_context,
            (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &m))) {
        unsigned y, x;
        hash = 2166136261u;                 /* FNV-1a */
        for (y = 0; y < td.Height; y += 4) {
            const unsigned char *r = (const unsigned char *)m.pData + y * m.RowPitch;
            for (x = 0; x < td.Width; x += 4) {
                hash ^= r[x * 4 + 0]; hash *= 16777619u;
                hash ^= r[x * 4 + 1]; hash *= 16777619u;
                hash ^= r[x * 4 + 2]; hash *= 16777619u;
            }
        }
        if (hash == 0) hash = 1;            /* 0 means "could not read" */
        ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                                  (ID3D11Resource *)stage, 0);
    }
    ID3D11Texture2D_Release(stage);
    ID3D11Texture2D_Release(back);
    return hash;
}

/* XBOX_D3D_DUMP_PRE=1 with the post chain on also writes the
 * title's own frame (before the post) beside each dump as <name>_pre.bmp:
 * the same frame with and without the effect, for exact comparisons. */
static int s_dump_scene_only;
static ID3D11Texture2D *s_dump_override;    /* XBOX_PUMPLAG_SHOTS: dump this texture instead */
static int  dac_active(void);
static void dac_lut_copy(unsigned char out[256][3]);

static int  present_box_cols(unsigned w, unsigned *x0, unsigned *bw);
static void present_box_row(const unsigned char *src, unsigned char *dst, unsigned w,
                            unsigned x0, unsigned bw);

void d3d8_DumpBackbuffer(const char *path)
{
    ID3D11Texture2D *back = NULL, *stage = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    FILE *f;
    unsigned w, h, y, x, row, imgsz;
    unsigned char hdr[54];
    unsigned char dlut[256][3];
    int dlut_on = !s_dump_scene_only && dac_active();

    if (!g_device_state.swap_chain || !g_device_state.d3d11_device) return;
    if (dlut_on) dac_lut_copy(dlut);
    if (!s_dump_scene_only && d3d8_post_enabled()) {
        static int pre = -1;
        if (pre < 0) { const char *e = getenv("XBOX_D3D_DUMP_PRE"); pre = e && e[0] == '1'; }
        if (pre) {
            char p2[600];
            size_t n = strlen(path);
            if (n > 4 && !strcmp(path + n - 4, ".bmp")) n -= 4;
            snprintf(p2, sizeof p2, "%.*s_pre.bmp", (int)n, path);
            s_dump_scene_only = 1;
            d3d8_DumpBackbuffer(p2);
            s_dump_scene_only = 0;
        }
    }
    if (s_dump_override) { back = s_dump_override; ID3D11Texture2D_AddRef(back); }
    else if (FAILED((s_dump_scene_only ? scene_texture_get : shown_texture_get)(&back)) || !back)
        return;
    ID3D11Texture2D_GetDesc(back, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
            &td, NULL, &stage)) || !stage) { ID3D11Texture2D_Release(back); return; }
    ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
        (ID3D11Resource *)stage, (ID3D11Resource *)back);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_device_state.d3d11_context,
            (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &m))) {
        w = td.Width; h = td.Height; row = w * 3;
        row = (row + 3) & ~3u;              /* BMP rows are 4-byte aligned */
        imgsz = row * h;
        f = fopen(path, "wb");
        unsigned bx0 = 0, bw = 0;
        int boxed = present_box_cols(w, &bx0, &bw);     /* as shown */
        unsigned char *rb = boxed ? (unsigned char *)malloc((size_t)w * 6u) : NULL;
        if (!rb) boxed = 0;
        if (f) {
            memset(hdr, 0, sizeof hdr);
            hdr[0] = 'B'; hdr[1] = 'M';
            *(unsigned *)(hdr + 2)  = 54 + imgsz;
            *(unsigned *)(hdr + 10) = 54;
            *(unsigned *)(hdr + 14) = 40;
            *(int *)(hdr + 18) = (int)w;
            *(int *)(hdr + 22) = (int)h;   /* positive = bottom-up */
            *(unsigned short *)(hdr + 26) = 1;
            *(unsigned short *)(hdr + 28) = 24;
            *(unsigned *)(hdr + 34) = imgsz;
            fwrite(hdr, 1, sizeof hdr, f);
            for (y = 0; y < h; y++) {
                const unsigned char *src =
                    (const unsigned char *)m.pData + (h - 1 - y) * m.RowPitch;
                unsigned char pad[4] = {0,0,0,0};
                /* The swap chain is DXGI_FORMAT_R8G8B8A8_UNORM, so the source
                 * bytes are R,G,B,A -- but BMP stores B,G,R. Copying the first
                 * three bytes straight through (as this did) swaps red and
                 * blue in every capture, which is why the XBOX_TEST_CLEAR teal
                 * read back as (153,153,0) and why the EA logo looked blue in
                 * dumps while being correct on screen. */
                for (x = 0; x < w; x++) {
                    unsigned char bgr[3];
                    bgr[0] = src[x * 4 + 2];   /* B */
                    bgr[1] = src[x * 4 + 1];   /* G */
                    bgr[2] = src[x * 4 + 0];   /* R */
                    if (dlut_on) {             /* what the window shows */
                        bgr[0] = dlut[bgr[0]][2];
                        bgr[1] = dlut[bgr[1]][1];
                        bgr[2] = dlut[bgr[2]][0];
                    }
                    if (boxed) memcpy(rb + x * 3, bgr, 3);
                    else fwrite(bgr, 1, 3, f);
                }
                if (boxed) {
                    present_box_row(rb, rb + (size_t)w * 3u, w, bx0, bw);
                    fwrite(rb + (size_t)w * 3u, 1, (size_t)w * 3u, f);
                }
                fwrite(pad, 1, row - w * 3, f);
            }
            fclose(f);
            fprintf(stderr, "  [D3D] wrote %ux%u frame to %s%s\n", w, h, path,
                    boxed ? (s_video_frame ? " (video frame)" : " (16:9 frame)") : "");
            fflush(stderr);
        }
        free(rb);
        ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
            (ID3D11Resource *)stage, 0);
    }
    ID3D11Texture2D_Release(stage);
    ID3D11Texture2D_Release(back);
}

void d3d8_DebugSampleBackbuffer(const char *tag)
{
    ID3D11Texture2D *back = NULL;
    if (!g_device_state.swap_chain || !g_device_state.d3d11_device) return;
    if (FAILED(scene_texture_get(&back)) || !back) return;
    {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D *stage = NULL;
        ID3D11Texture2D_GetDesc(back, &td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
                &td, NULL, &stage)) && stage) {
            D3D11_MAPPED_SUBRESOURCE m;
            ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
                (ID3D11Resource *)stage, (ID3D11Resource *)back);
            if (SUCCEEDED(ID3D11DeviceContext_Map(g_device_state.d3d11_context,
                    (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &m))) {
                const unsigned char *px = (const unsigned char *)m.pData;
                unsigned cx = td.Width / 2, cy = td.Height / 2;
                fprintf(stderr, "  [D3D] sample @%s: tl=%02X%02X%02X ctr=%02X%02X%02X\n",
                        tag, px[2], px[1], px[0],
                        px[cy * m.RowPitch + cx * 4 + 2],
                        px[cy * m.RowPitch + cx * 4 + 1],
                        px[cy * m.RowPitch + cx * 4 + 0]);
                fflush(stderr);
                ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                    (ID3D11Resource *)stage, 0);
            }
            ID3D11Texture2D_Release(stage);
        }
    }
    ID3D11Texture2D_Release(back);
}

void d3d8_PumpMessages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) ExitProcess(0);
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* The guest framebuffer the video player writes into, if any. Stored rather
 * than passed, because the upload has to happen inside the present path: the
 * swap chain is DXGI_SWAP_EFFECT_DISCARD and there are three Present call
 * sites, so a copy made before d3d8_PresentFrame is destroyed by any Present
 * that lands in between. */
static const void *g_guest_fb_src   = NULL;
static const void *g_guest_fb_alt   = NULL;   /* the other half of the flip */
static unsigned    g_guest_fb_pitch = 0;
static unsigned    g_guest_fb_w     = 0;
static unsigned    g_guest_fb_h     = 0;

void d3d8_SetGuestFramebuffer(const void *src, unsigned pitch,
                              unsigned w, unsigned h)
{
    g_guest_fb_src = src; g_guest_fb_pitch = pitch;
    g_guest_fb_w = w;     g_guest_fb_h = h;
}

/* The title double-buffers its video surface and alternates between two
 * addresses, so uploading a fixed one shows a stale frame every other present
 * -- which reads as texture glitching rather than as a missed frame. Register
 * both and let the present pick whichever was written most recently. */
void d3d8_SetGuestFramebufferAlt(const void *alt)
{
    g_guest_fb_alt = alt;
}

/* The video player writes the guest framebuffer through LockRect; everything
 * else the title draws goes through the push buffer to the host render target.
 * Uploading the guest buffer on every present therefore pasted the last video
 * frame over every screen that followed: after START skipped the attract movie
 * the frontend issued ~10,000 draws a second behind a frozen picture of the
 * intro's first frame. The upload now runs only while the video is
 * actually locking the buffer -- the player locks once per decoded frame and
 * presents once per frame, so 30 presents without a lock (the frontend's
 * frames, or the fallback below) mean it stopped. */
#define GUEST_FB_IDLE_PRESENTS 30u
static unsigned g_present_seq     = 0;
static unsigned g_guest_fb_lock_seq = 0;
static unsigned g_guest_fb_locks  = 0;

/* One present per video frame, at the title's own flip.
 *
 * The player (0x00148B40) clears the back buffer, locks it, copies the decoded
 * picture into it and calls Present, whose swap points the render surface at
 * the other buffer (SET_SURFACE_COLOR_OFFSET, nv2a pgraph). That flip is the
 * moment the frame is finished; the console shows it from the next vertical
 * blank, one Present per frame (PresentationInterval ONE): 2 refreshes per
 * frame of a 29.97 fps file at 59.94 Hz. Presenting on a 16 ms timer instead,
 * measured with GetTickCount (15.6 ms steps), gave 40 presents a second at
 * 31/31/16/31/16 ms, each showing whichever buffer a sample test guessed was
 * newest: frames on screen for 1, 2, 3 or 4 refreshes.
 *
 * So each lock records which buffer the title is writing, and the flip that
 * retires that buffer hands it over to the next present, once. The title's
 * next lock of that buffer waits (BlockOnResource, 0x0016BF20) until the pump
 * has translated the commands after this flip, so the frame cannot change
 * under the copy. The locks are written on the title's thread and read on the
 * pump's at the flip that follows the frame's Present; the interlocked writes
 * keep them ahead of the push-buffer words carrying that flip. */
static volatile LONG g_guest_fb_va[2];        /* guest address of src / alt, 0 = not locked yet */
static volatile LONG g_guest_fb_written[2];   /* lock count at its last lock */
static unsigned      g_guest_fb_handed[2];    /* lock count it had when a flip handed it over */
static int           g_video_next = -1;       /* buffer handed over, not presented yet (-1: none) */
static unsigned      g_video_flips, g_video_shown, g_video_replaced, g_video_relocked;
static unsigned      g_video_left_out;        /* video presents the display path left out (d3d8_sync.c: never) */
static int           s_present_rendered;      /* the next present carries the title's draws */

void d3d8_NoteGuestFramebufferLock(void)
{
    g_guest_fb_lock_seq = g_present_seq;
    g_guest_fb_locks++;
}

/* LockRect hook (title's thread): the buffer at guest address va (host p) is
 * being written. */
void d3d8_NoteGuestFramebufferLockAt(uint32_t va, const void *p)
{
    int i = !p ? -1 : p == g_guest_fb_src ? 0 : p == g_guest_fb_alt ? 1 : -1;
    d3d8_NoteGuestFramebufferLock();
    if (i >= 0) {
        InterlockedExchange(&g_guest_fb_va[i], (LONG)va);
        InterlockedExchange(&g_guest_fb_written[i], (LONG)g_guest_fb_locks);
    }
}

/* Surface flip (pump thread, nv2a pgraph): the title's render surface moves
 * away from `offset`. 1 when that surface is a video buffer written since a
 * flip last handed it over: it is then the frame the next present shows.
 * The surface offset is the buffer's physical address and LockRect hands the
 * title that address in the write-combined alias, | 0xF0000000 (0x0016BF58). */
int d3d8_GuestFramebufferFlip(uint32_t offset)
{
    unsigned w;
    int i;
    if (!g_guest_fb_src) return 0;
    for (i = 0; i < 2; i++) {
        uint32_t va = (uint32_t)g_guest_fb_va[i];
        if (va && (va & 0x0FFFFFFFu) == (offset & 0x0FFFFFFFu)) break;
    }
    if (i == 2) return 0;
    w = (unsigned)g_guest_fb_written[i];
    if (w == g_guest_fb_handed[i]) return 0;      /* nothing new in it: an ordinary flip */
    g_guest_fb_handed[i] = w;                     /* handed over once, shown or replaced */
    if (g_video_next >= 0) g_video_replaced++;    /* the previous frame never got its present */
    g_video_next = i;
    g_video_flips++;
    return 1;
}

/* 1 while a frame handed over by a flip still waits for its present (the
 * GPU check below held it: the GPU was not done with the previous one). */
int d3d8_VideoFramePending(void)
{
    return g_video_next >= 0;
}

/* The next present shows the title's own draws (a flip or a clear after
 * draws, nv2a pgraph), not a video frame: nothing is pasted over them. */
void d3d8_NoteRenderedFrame(void)
{
    s_present_rendered = 1;
}

/* Milliseconds since the last actual present, on the performance counter
 * (the video fallback in nv2a_live_pb.c; GetTickCount moves in 15.6 ms steps). */
static LARGE_INTEGER s_last_present_qpc;
double d3d8_MsSincePresentQpc(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER now;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    if (!s_last_present_qpc.QuadPart) s_last_present_qpc = now;
    return (double)(now.QuadPart - s_last_present_qpc.QuadPart) * 1000.0 / (double)f.QuadPart;
}

/* Video frames the title has written so far (the freeze watchdog, port
 * crashreport.c, counts them as progress: a video issues no draws). */
unsigned d3d8_GuestFramebufferLockCount(void)
{
    return *(volatile unsigned *)&g_guest_fb_locks;
}

static int guest_fb_active(void)
{
    static int was_active = 0;
    int active = g_guest_fb_locks != 0
              && g_present_seq - g_guest_fb_lock_seq <= GUEST_FB_IDLE_PRESENTS;
    if (active != was_active) {
        fprintf(stderr, "  [FB] video framebuffer %s after %u locks (present %u)",
                active ? "active: presenting the video"
                       : "idle: presenting rendered frames",
                g_guest_fb_locks, g_present_seq);
        if (!active)    /* so far, frames handed over by the title's flips */
            fprintf(stderr, "; %u frames at flips, %u presented, %u replaced, %u relocked during the copy, "
                    "%u left out by the display",
                    g_video_flips, g_video_shown, g_video_replaced, g_video_relocked, g_video_left_out);
        fprintf(stderr, "\n");
        xbox_log_flush();
        was_active = active;
    }
    return active;
}

/* XBOX_PUMPLAG: the frame being drawn has a part drawn from a rewritten
 * constant block (set by the translator, same thread). */
static int s_plag_req;
void d3d8_PlagShot(void) { s_plag_req = 1; }

/* Presents so far; draws issued now belong to dump frame f<this>. */
unsigned d3d8_PresentSeq(void)
{
    return g_present_seq;
}

/* Video presents never wait for the GPU.
 *
 * While a movie plays, this thread presents its frames (at the title's flips
 * now; on a 16 ms timer before, nv2a_live_pb.c) -- and this thread is
 * also the one that acknowledges the title's GPU fences. The movie player
 * waits on such a fence every frame
 * (D3D BlockOnTime under its back-buffer LockRect), and between two frames
 * its loop is the only reader of the game's 8-sample input ring. On the
 * laptop's Intel UHD the driver regularly kept a present-path call (Map of
 * the upload texture, then Present once that was gone) blocked for 350-500 ms
 * while the GPU was still on earlier work: the fences stopped, the movie loop
 * stopped reading input, and a START pressed in that window was lost.
 * An event query is ended with each movie frame, just before its Present
 *; the next one is issued only once the GPU has passed it, so the
 * present is put off to a later pump tick instead of blocking (the frame is
 * shown a little later, as it would have been; a newer frame handed over
 * meanwhile replaces it). */
static ID3D11Query *s_vid_query;
static int          s_vid_query_live;
static unsigned     s_vid_skips;

/* XBOX_FLIP_LOG=1 (measuring): the time the guard itself takes on the
 * presenting thread -- its End before each video present, and its GetData
 * checks before it -- one [VMARK] line per video present. */
static int          s_vid_tlog = -1;
static unsigned     s_vid_checks;           /* GetData calls since the last video present */
static double       s_vid_check_ms;         /* their time */

static double vid_qpc_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int vid_tlog_on(void)
{
    if (s_vid_tlog < 0) { const char *e = getenv("XBOX_FLIP_LOG"); s_vid_tlog = e && e[0] == '1'; }
    return s_vid_tlog;
}

static void video_present_mark(void)
{
    double m0 = 0.0;
    if (!g_device_state.d3d11_device || !g_device_state.d3d11_context) return;
    if (vid_tlog_on()) m0 = vid_qpc_ms();
    if (!s_vid_query) {
        D3D11_QUERY_DESC qd;
        qd.Query = D3D11_QUERY_EVENT;
        qd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(g_device_state.d3d11_device, &qd, &s_vid_query)))
            s_vid_query = NULL;
    }
    if (!s_vid_query) return;
    /* Ended after the frame's last draw and before its Present,
     * which sends it to the GPU with the frame (a Present flushes the
     * commands ahead of it). Ended after the Present, it waited in the next
     * command buffer until the next frame's check flushed it, and the GPU,
     * idle between two video frames, took ~2 ms more to pass it: every video
     * frame was put off that long after its flip (60 Hz, B2 31/10), and a
     * flip of the title inside that time (a catch-up after a late frame, the
     * menu's first frame) replaced the frame waiting, never shown. A Flush
     * after that End sent it, but took up to 2.3 ms on this thread when the
     * Present had just been queued at a refresh (B3 31/10). The query now
     * tells when the GPU has drawn the frame; the Present's own GPU work
     * comes after it. */
    ID3D11DeviceContext_End(g_device_state.d3d11_context, (ID3D11Asynchronous *)s_vid_query);
    s_vid_query_live = 1;
    if (s_vid_tlog > 0) {
        double m1 = vid_qpc_ms();
        fprintf(stderr, "[VMARK] t=%.1f end %.3f ms (before the present); guard checks %u, %.3f ms\n",
                m0, m1 - m0, s_vid_checks, s_vid_check_ms);
        s_vid_checks = 0;
        s_vid_check_ms = 0.0;
    }
}

int d3d8_VideoPresentReady(void)
{
    HRESULT hr;
    double c0;
    if (!s_vid_query_live || !s_vid_query || !g_device_state.d3d11_context) return 1;
    c0 = vid_tlog_on() ? vid_qpc_ms() : 0.0;
    hr = ID3D11DeviceContext_GetData(g_device_state.d3d11_context,
                                     (ID3D11Asynchronous *)s_vid_query, NULL, 0, 0);
    if (s_vid_tlog > 0) {
        s_vid_check_ms += vid_qpc_ms() - c0;
        s_vid_checks++;
    }
    if (hr == S_FALSE) {
        if (++s_vid_skips == 1 || s_vid_skips % 500 == 0) {
            fprintf(stderr, "[FB] video present held: GPU still on the previous frame (%u so far)\n",
                    s_vid_skips);
            xbox_log_flush();
        }
        return 0;
    }
    s_vid_query_live = 0;
    return 1;
}

void d3d8_PresentFrame(void)
{
    /* Pump Windows messages */
    MSG msg;
    int rendered = s_present_rendered;

    s_present_rendered = 0;
    g_present_seq++;
    if (g_guest_fb_src && guest_fb_active()) {
        /* The frame a flip handed over, once. A present that is
         * neither that nor the title's own draws (the fallback) holds the
         * frame shown last: fb == NULL re-presents the staging copy rather
         * than re-reading a buffer the title may be writing. A present of the
         * title's draws (the frontend, right after a video) shows them, as
         * the console shows the buffer it flips to; a frame still waiting
         * then is older than they are and is dropped. */
        if (rendered) {
            if (g_video_next >= 0) {
                g_video_next = -1;
                g_video_replaced++;
            }
        } else {
            const void *fb = NULL;
            int i = g_video_next;
            if (i >= 0) {
                fb = i ? g_guest_fb_alt : g_guest_fb_src;
                g_video_next = -1;
                g_video_shown++;
            }
            d3d8_PresentGuestFramebuffer(fb, g_guest_fb_pitch,
                                         g_guest_fb_w, g_guest_fb_h);
            /* The copy is done: had the title locked that buffer again
             * meanwhile (it waits for the pump past the next flip, so it
             * should not), the frame copied could be torn. Counted. */
            if (i >= 0 && (unsigned)g_guest_fb_written[i] != g_guest_fb_handed[i])
                g_video_relocked++;
        }
    }

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) ExitProcess(0);
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    /* XBOX_TEST_CLEAR=1 clears the default RTV to a known colour immediately
     * before the readback. This exercises the RTV -> back buffer -> readback
     * chain with no draw involved, splitting 'the draw fails' from 'the target
     * we render to is not the surface we present and sample'. */
    {
        static int want = -1;
        if (want < 0) {
            const char *e = getenv("XBOX_TEST_CLEAR");
            want = (e && e[0] == '1') ? 1 : 0;
        }
        if (want && g_device_state.d3d11_context && g_device_state.default_rtv) {
            const FLOAT teal[4] = { 0.0f, 0.6f, 0.6f, 1.0f };
            ID3D11DeviceContext_ClearRenderTargetView(g_device_state.d3d11_context,
                g_device_state.default_rtv, teal);
        }
    }

    /* Sampled before Present, not after: with DXGI_SWAP_EFFECT_DISCARD the
     * back buffer's contents are undefined once Present returns, so a
     * readback taken afterwards reports the discard, not the frame. */
    /* XBOX_D3D_DUMP=<prefix> saves whole frames as <prefix>NNN.bmp.
     *
     * Two sampled pixels can tell a cleared buffer from a filled one, but not
     * whether something small -- a line of text -- was drawn somewhere in
     * between. This answers that by looking instead of inferring. */
    {
        /* Spread the captures across the run, not just the opening frames.
         * The first version only ever dumped the first few and concluded the
         * screen stayed white -- it does not; it changes a few seconds in, and
         * sampling only the start hid that completely. */
        static unsigned seen = 0, dn = 0;
        /* XBOX_D3D_DUMP_MAX raises the 24-capture cap. The title reaches its
         * second screen around frame 170 and can then stop presenting
         * entirely, so the interesting frames are the last ones a run
         * produces, not the first 24. */
        static unsigned g_dump_cap = 0;
        if (!g_dump_cap) {
            const char *m = getenv("XBOX_D3D_DUMP_MAX");
            g_dump_cap = (m && atoi(m) > 0) ? (unsigned)atoi(m) : 24u;
        }
        const char *pre = getenv("XBOX_D3D_DUMP");
        if (pre) {
            unsigned every = 60;
            const char *e = getenv("XBOX_D3D_DUMP_EVERY");
            /* XBOX_D3D_DUMP_FROM skips the first N presents before capturing.
             * Without it, catching something that appears for a single frame
             * several seconds in means dumping every frame from zero -- which
             * blows the capture cap long before the interesting window and
             * writes hundreds of megabytes of the same static screen. */
            static unsigned from = 0xFFFFFFFFu;
            if (from == 0xFFFFFFFFu) {
                const char *f = getenv("XBOX_D3D_DUMP_FROM");
                from = f ? (unsigned)atoi(f) : 0u;
            }
            if (e) { unsigned v = (unsigned)atoi(e); if (v) every = v; }
            /* XBOX_D3D_DUMP_ONCHANGE=1 dumps only when the image actually
             * changes, which is the only practical way to catch a screen that
             * appears for one frame in the middle of a static one. */
            static int onchange = -1;
            static unsigned last_hash = 0;
            int changed = 1;
            if (onchange < 0) {
                const char *c = getenv("XBOX_D3D_DUMP_ONCHANGE");
                onchange = (c && c[0] == '1') ? 1 : 0;
            }
            if (onchange) {
                unsigned h = d3d8_BackbufferHash();
                changed = (h != 0 && h != last_hash);
                if (h) last_hash = h;
            }
            if (changed && seen >= from && ((seen - from) % every) == 0
                && dn < g_dump_cap) {
                char path[512];
                if (s_cycle_mode >= 0)  /* XBOX_POST_CYCLE mode in the name */
                    snprintf(path, sizeof path, "%s%03u_f%05u_m%d.bmp", pre, dn++, seen,
                             s_cycle_mode);
                else
                    snprintf(path, sizeof path, "%s%03u_f%05u.bmp", pre, dn++, seen);
                d3d8_DumpBackbuffer(path);
                /* Stamp each dump with wall-clock seconds. Frame indices alone
                 * cannot answer "how long was this on screen" once the frame
                 * rate stops being 60 Hz -- and it does: the frontend runs at
                 * about 6. */
                if (getenv("XBOX_PRESENT_TIME")) {
                    static clock_t t0 = 0;
                    if (!t0) t0 = clock();
                    fprintf(stderr, "[PRESENTT] dump %s at t=%.2fs (present %u)\n",
                            path, (double)(clock() - t0) / (double)CLOCKS_PER_SEC,
                            seen);
                    fflush(stderr);
                }
            }
            seen++;
        }
    }

    /* XBOX_PUMPLAG_SHOTS=<prefix> (diagnostic, port/src/pumplag.c): a frame
     * with a mesh part drawn from a rewritten constant block is saved as
     * <prefix>fNNNNN_bad.bmp, with the frame before (_before, kept on the GPU
     * every frame) and the one after (_after). At most 40 such frames. */
    {
        static int on = -1;
        static char pre[400];
        static ID3D11Texture2D *prev;
        static unsigned prev_f = 0xFFFFFFFFu, next_due = 0xFFFFFFFFu, last_saved = 0xFFFFFFFFu, events;
        unsigned f = g_present_seq - 1u;
        {   /* XBOX_PUMPLAG_SLOW=<ms> (stress test): the pump stalls this long
             * every present, as a slower GPU would; the title then runs as far
             * ahead as its own throttle lets it. */
            static int slow = -1;
            if (slow < 0) { const char *e = getenv("XBOX_PUMPLAG_SLOW"); slow = e ? atoi(e) : 0; }
            if (slow > 0) Sleep((DWORD)slow);
        }
        if (on < 0) {
            const char *e = getenv("XBOX_PUMPLAG_SHOTS");
            on = e && e[0];
            if (on) snprintf(pre, sizeof pre, "%s", e);
        }
        if (on && g_device_state.d3d11_device) {
            char path[512];
            ID3D11Texture2D *cur = NULL;
            if (s_plag_req && events < 40u) {
                events++;
                if (prev && prev_f + 1u == f && last_saved != prev_f) {
                    snprintf(path, sizeof path, "%sf%05u_before.bmp", pre, prev_f);
                    s_dump_override = prev;
                    d3d8_DumpBackbuffer(path);
                    s_dump_override = NULL;
                }
                snprintf(path, sizeof path, "%sf%05u_bad.bmp", pre, f);
                d3d8_DumpBackbuffer(path);
                fprintf(stderr, "[PLAG] shot %s\n", path);
                last_saved = f;
                next_due = f + 1u;
            } else if (f == next_due) {
                snprintf(path, sizeof path, "%sf%05u_after.bmp", pre, f);
                d3d8_DumpBackbuffer(path);
                last_saved = f;
            }
            s_plag_req = 0;
            if (SUCCEEDED(shown_texture_get(&cur)) && cur) {
                if (!prev) {
                    D3D11_TEXTURE2D_DESC td;
                    ID3D11Texture2D_GetDesc(cur, &td);
                    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = 0;
                    td.CPUAccessFlags = 0; td.MiscFlags = 0;
                    if (FAILED(ID3D11Device_CreateTexture2D(g_device_state.d3d11_device, &td, NULL, &prev)))
                        prev = NULL;
                }
                if (prev) {
                    D3D11_TEXTURE2D_DESC a, b;
                    ID3D11Texture2D_GetDesc(cur, &a);
                    ID3D11Texture2D_GetDesc(prev, &b);
                    if (a.Width == b.Width && a.Height == b.Height && a.Format == b.Format) {
                        ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
                            (ID3D11Resource *)prev, (ID3D11Resource *)cur);
                        prev_f = f;
                    } else { ID3D11Texture2D_Release(prev); prev = NULL; }
                }
                ID3D11Texture2D_Release(cur);
            }
        }
    }

    /* Read back what was actually rasterised.
     *
     * Draw counts cannot distinguish 'issued and rasterised' from 'issued and
     * dropped', so sample the surface itself: four points, once every 120
     * frames. */
    {
        static unsigned frame = 0;
        static int want = -1;
        if (want < 0) {
            const char *e = getenv("XBOX_D3D_READBACK");
            want = (e && e[0] == '1') ? 1 : 0;
        }
        /* Off by default: this allocates a staging texture and does a full
         * CopyResource on the pump thread, which is far too heavy to leave in
         * the frame path. XBOX_D3D_READBACK=1 turns it back on. */
        if (want && g_device_state.swap_chain && g_device_state.d3d11_device &&
            (frame <= 6 || (frame % 120) == 0) && ++frame <= 600) {
            ID3D11Texture2D *back = NULL;
            HRESULT hr = scene_texture_get(&back);
            if (SUCCEEDED(hr) && back) {
                D3D11_TEXTURE2D_DESC td;
                ID3D11Texture2D_GetDesc(back, &td);
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                td.MiscFlags = 0;
                {
                    ID3D11Texture2D *stage = NULL;
                    if (SUCCEEDED(ID3D11Device_CreateTexture2D(
                            g_device_state.d3d11_device, &td, NULL, &stage)) && stage) {
                        D3D11_MAPPED_SUBRESOURCE m;
                        ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
                            (ID3D11Resource *)stage, (ID3D11Resource *)back);
                        if (SUCCEEDED(ID3D11DeviceContext_Map(g_device_state.d3d11_context,
                                (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &m))) {
                            const unsigned char *px = (const unsigned char *)m.pData;
                            unsigned cx = td.Width / 2, cy = td.Height / 2;
                            fprintf(stderr, "  [D3D] frame %u backbuffer %ux%u fmt=%d:"
                                    " tl=%02X%02X%02X ctr=%02X%02X%02X\n",
                                    frame, td.Width, td.Height, (int)td.Format,
                                    px[2], px[1], px[0],
                                    px[cy * m.RowPitch + cx * 4 + 2],
                                    px[cy * m.RowPitch + cx * 4 + 1],
                                    px[cy * m.RowPitch + cx * 4 + 0]);
                            fflush(stderr);
                            ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                                (ID3D11Resource *)stage, 0);
                        }
                        ID3D11Texture2D_Release(stage);
                    }
                }
                ID3D11Texture2D_Release(back);
            }
        }
    }
    /* Present the backbuffer (VSync = 1) */
    g_last_present_tick = GetTickCount();
    QueryPerformanceCounter(&s_last_present_qpc);     /* the video fallback */
    if (g_device_state.swap_chain)
        { static unsigned c = 0; if (++c == 1 || ((c % 300) == 0 && xbox_log_stats())) { fprintf(stderr, "  [PRESENT] PresentFrame #%u\n", c); xbox_log_flush(); } }
        host_present();
}

/* ================================================================
 * Internal accessors (used by d3d8_resources/shaders/states)
 * ================================================================ */

IDirect3DDevice8    *d3d8_GetDevice(void) { return &g_device; }
ID3D11Device        *d3d8_GetD3D11Device(void) { return g_device_state.d3d11_device; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return g_device_state.d3d11_context; }
IDXGISwapChain      *d3d8_GetSwapChain(void) { return g_device_state.swap_chain; }
ID3D11RenderTargetView *d3d8_GetDefaultRTV(void) { return g_device_state.default_rtv; }
ID3D11DepthStencilView *d3d8_GetDefaultDSV(void) { return g_device_state.default_dsv; }
HWND                 d3d8_GetHWND(void) { return g_device_state.hwnd; }
/* The title's own backbuffer size (640x480), which is the space its
 * vertices and pre-transformed positions are in -- not the scene target's. */
UINT                 d3d8_GetBackbufferWidth(void)
    { return g_device_state.guest_w ? g_device_state.guest_w : g_device_state.width; }
UINT                 d3d8_GetBackbufferHeight(void)
    { return g_device_state.guest_h ? g_device_state.guest_h : g_device_state.height; }

/* Scene-target pixels per title pixel, for anything specified in the title's
 * pixel units: scissor and clip rectangles. 1 in every automated run.
 * Fork: of the bound target -- the scaled one during the 3D of an image
 * when the render scale is on, the render size otherwise. */
void d3d8_GetGuestScale(float *sx, float *sy)
{
    const D3D8DeviceState *s = &g_device_state;
    UINT w = cur_w(s), h = cur_h(s);
    *sx = (s->guest_w && w) ? (float)w / (float)s->guest_w : 1.0f;
    *sy = (s->guest_h && h) ? (float)h / (float)s->guest_h : 1.0f;
}
/* Occlusion queries for the NV2A zpass pixel counter (fork). A small
 * pool: the translator begins/ends one around each zpass-enabled span and
 * polls without blocking (GetData flag 0 flushes if needed, never waits).
 * Samples are counted on the scene target: d3d8_OcclusionScale gives the
 * divisor back to title pixels (MSAA samples x scale^2). Pump thread only. */
#define OCC_POOL 256
static ID3D11Query *s_occ[OCC_POOL];
static unsigned char s_occ_busy[OCC_POOL];

int d3d8_OcclusionBegin(void)
{
    ID3D11Device *dev = g_device_state.d3d11_device;
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context;
    int i;
    if (!dev || !ctx) return -1;
    for (i = 0; i < OCC_POOL; i++) if (!s_occ_busy[i]) break;
    if (i == OCC_POOL) return -1;
    if (!s_occ[i]) {
        D3D11_QUERY_DESC qd;
        qd.Query = D3D11_QUERY_OCCLUSION; qd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &s_occ[i]))) { s_occ[i] = NULL; return -1; }
    }
    s_occ_busy[i] = 1;
    if (s_rs_pct != 100) {      /* fork: counted on the bound target (render scale) */
        float sx, sy;
        d3d8_GetGuestScale(&sx, &sy);
        s_occ_rs_scale = (float)(cur_msaa(&g_device_state) ? cur_msaa(&g_device_state) : 1u) * sx * sy;
    }
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)s_occ[i]);
    return i;
}

void d3d8_OcclusionEnd(int h)
{
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context;
    if (h < 0 || h >= OCC_POOL || !s_occ[h] || !ctx) return;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)s_occ[h]);
}

/* 1 = ready (*samples set), 0 = not yet, -1 = invalid. Never blocks. */
int d3d8_OcclusionPoll(int h, unsigned long long *samples)
{
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context;
    UINT64 n = 0;
    HRESULT hr;
    if (h < 0 || h >= OCC_POOL || !s_occ[h] || !ctx) return -1;
    hr = ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)s_occ[h], &n, sizeof n, 0);
    if (hr == S_OK) { *samples = n; return 1; }
    if (hr == S_FALSE && g_gpuprof_on) gpuprof_count_flush();
    return hr == S_FALSE ? 0 : -1;
}

/* Fork: submit what is queued (the occlusion queries a report waits for). */
void d3d8_OcclusionFlush(void)
{
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context;
    if (ctx) ID3D11DeviceContext_Flush(ctx);
}

/* O-B: a D3D11.4 fence, signalled at each GET_REPORT after the
 * queries the report waits for, so that the pump can tell they are done
 * without GetData and write the report before the Present
 * (nv2a_pgraph_d3d11.c, occ_poll). GetCompletedValue only reads the fence
 * (DXVK: vkGetSemaphoreCounterValue), where a GetData that answers S_FALSE
 * may submit the work queued so far (DXVK ignores DONOTFLUSH there). DXVK's
 * Signal submits what is queued itself, as the Flush that follows it at
 * GET_REPORT does already with DXVK's default settings. Two cases where DXVK
 * skips that Flush and the Signal adds one submission per report:
 * d3d11.ignoreExplicitFlush, and a tiler GPU (DXVK's tilerMode: mobile and
 * Apple drivers) while a multisampled pass waits for its resolve; neither is
 * the case of the Steam Deck or of a desktop GPU by default. Created without
 * flags: (D3D11_FENCE_FLAG)0, not D3D11_FENCE_FLAG_NONE, which MinGW's
 * headers define as 1 (Microsoft's as 0). Pump thread only; created at the
 * first mark and kept (the device lives as long as the process). Without
 * ID3D11Device5 there is no fence: every mark is 0, and the reports are
 * written at the pump's next tick, as before. */
static const IID k_iid_occ_device5  = { 0x8ffde202, 0xa0e7, 0x45df, { 0x9e, 0x01, 0xe8, 0x37, 0x80, 0x1b, 0x5e, 0xa0 } };
static const IID k_iid_occ_context4 = { 0x917600da, 0xf58c, 0x4c33, { 0x98, 0xd8, 0x3e, 0x15, 0xb3, 0x90, 0xfa, 0x24 } };
static const IID k_iid_occ_fence    = { 0xaffde9d1, 0x1df7, 0x4bb7, { 0x8a, 0x34, 0x0f, 0x46, 0x25, 0x1d, 0xab, 0x80 } };
static struct { int tried; ID3D11DeviceContext4 *ctx4; ID3D11Fence *fence; UINT64 value; } s_occ_fence;

/* A mark after every command recorded so far: its value, or 0 (no fence, or
 * the Signal failed). */
unsigned long long d3d8_OcclusionMark(void)
{
    if (!s_occ_fence.tried && g_device_state.d3d11_device && g_device_state.d3d11_context) {
        ID3D11Device5 *d5 = NULL;
        s_occ_fence.tried = 1;
        if (SUCCEEDED(ID3D11Device_QueryInterface(g_device_state.d3d11_device, &k_iid_occ_device5, (void **)&d5)) && d5
            && SUCCEEDED(ID3D11DeviceContext_QueryInterface(g_device_state.d3d11_context, &k_iid_occ_context4,
                                                            (void **)&s_occ_fence.ctx4)) && s_occ_fence.ctx4
            && SUCCEEDED(ID3D11Device5_CreateFence(d5, 0, (D3D11_FENCE_FLAG)0, &k_iid_occ_fence,
                                                   (void **)&s_occ_fence.fence)) && s_occ_fence.fence) {
            fprintf(stderr, "[OCCL] visibility reports written as soon as their queries are done, before the Present "
                    "too (XBOX_FIX_OCCL_EARLY=0: at the pump's next tick only)\n");
        } else {
            if (s_occ_fence.ctx4) { ID3D11DeviceContext4_Release(s_occ_fence.ctx4); s_occ_fence.ctx4 = NULL; }
            s_occ_fence.fence = NULL;
            fprintf(stderr, "[OCCL] no D3D11 fence: visibility reports written at the pump's next tick only\n");
        }
        if (d5) ID3D11Device5_Release(d5);
    }
    if (!s_occ_fence.fence) return 0;
    if (FAILED(ID3D11DeviceContext4_Signal(s_occ_fence.ctx4, s_occ_fence.fence, s_occ_fence.value + 1))) return 0;
    return ++s_occ_fence.value;
}

/* The last mark the GPU is past (0: none yet, or no fence). */
unsigned long long d3d8_OcclusionMarkDone(void)
{
    return s_occ_fence.fence ? (unsigned long long)ID3D11Fence_GetCompletedValue(s_occ_fence.fence) : 0ull;
}

/* d3d8_OcclusionPoll without the flush of flag 0, for a query whose mark the
 * GPU is past: 1 = ready (*samples set), 0 = not yet (the report then waits
 * for the next tick's poll), -1 = invalid. Never blocks. */
int d3d8_OcclusionPollDone(int h, unsigned long long *samples)
{
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context;
    UINT64 n = 0;
    HRESULT hr;
    if (h < 0 || h >= OCC_POOL || !s_occ[h] || !ctx) return -1;
    hr = ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)s_occ[h], &n, sizeof n, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr == S_OK) { *samples = n; return 1; }
    return hr == S_FALSE ? 0 : -1;
}

/* The pump has run out of commands. The D3D11 driver keeps what
 * was recorded since the last submission until a Present, a Flush or a full
 * buffer: at 60 fps the scene of frame N-1, translated right after its flip,
 * waited ~14 ms in that buffer and the GPU only started it at the Present of
 * frame N (~3.3 ms of GPU wait there). Submitting it when the pump
 * goes idle lets the GPU work while the title waits for its next tick.
 * Pump thread only (it owns the immediate context). */
void d3d8_IdleFlush(void)
{
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context;
    if (ctx) ID3D11DeviceContext_Flush(ctx);
}

void d3d8_OcclusionRelease(int h)
{
    if (h >= 0 && h < OCC_POOL) s_occ_busy[h] = 0;
}

float d3d8_OcclusionScale(void)
{
    const D3D8DeviceState *s = &g_device_state;
    float sx, sy;
    if (s_rs_pct != 100 && s_occ_rs_scale > 0.0f)
        return s_occ_rs_scale;      /* fork: the target the samples were counted on */
    d3d8_GetGuestScale(&sx, &sy);
    return (float)(s->msaa ? s->msaa : 1) * sx * sy;
}

const DWORD         *d3d8_GetRenderStates(void) { return g_device_state.render_states; }
const DWORD         *d3d8_GetTSS(DWORD stage) { return (stage < MAX_TEXTURE_STAGES) ? g_device_state.tss[stage] : NULL; }
void                *d3d8_CurTexture(int stage) { return stage >= 0 && stage < 4 ? (void *)g_cur_textures[stage] : NULL; }
const D3DMATRIX     *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type) {
    return ((DWORD)type < MAX_TRANSFORMS) ? &g_device_state.transforms[(DWORD)type] : NULL;
}

const D3DLIGHT8     *d3d8_GetLight(DWORD index) {
    return (index < MAX_LIGHTS) ? &g_device_state.lights[index] : NULL;
}

BOOL                 d3d8_GetLightEnable(DWORD index) {
    return (index < MAX_LIGHTS) ? g_device_state.light_enable[index] : FALSE;
}

const D3DMATERIAL8  *d3d8_GetMaterial(void) {
    return &g_device_state.material;
}

UINT                 d3d8_GetNumLights(void) {
    return MAX_LIGHTS;
}

/* ================================================================
 * D3D11 initialization helpers
 * ================================================================ */

/*
 * Host output window.
 *
 * On Xbox there is no window at all -- a title scans out straight to the
 * framebuffer, so D3DPRESENT_PARAMETERS::hDeviceWindow is meaningless and
 * SSX Tricky leaves it null. DXGI, though, requires a real HWND for the
 * swap chain's OutputWindow. So when the title supplies no window we make
 * one here: on PC the window *is* the TV.
 *
 * Created on whichever thread first initialises the device, and given a
 * plain WndProc that swallows WM_CLOSE into a flag rather than tearing the
 * window down underneath the renderer -- the recompiled game owns the
 * shutdown path, and destroying its output surface from a message handler
 * would pull the swap chain out from under a frame in flight.
 */
static HWND  s_host_window = NULL;
static HMENU s_host_menu = NULL;
static BOOL  s_host_window_close_requested = FALSE;

BOOL d3d8_HostWindowCloseRequested(void) { return s_host_window_close_requested; }
HWND d3d8_GetHostWindow(void)            { return s_host_window; }

/*
 * Host display settings.
 *
 * Set by the launcher before the title creates its device. The title always
 * renders its own 640x480 frame; render_w x render_h is the size of the scene
 * target that frame is rasterised into, so the 3D is drawn at that
 * resolution rather than scaled up afterwards. The aspect is the shape the
 * frame is shown at: 16:9 is only right when the title has itself been told
 * the display is widescreen, because it then renders anamorphic -- a wider
 * field of view squeezed into 640x480, for the TV to stretch back out (see
 * xbox_SetVideoFlags). Zero sizes mean the title's own size, which is what
 * every automated run uses, so captures there are unchanged.
 */
static UINT s_cfg_render_w = 0, s_cfg_render_h = 0;
static int  s_cfg_widescreen = 0;
static double s_cfg_aspect = 0.0;       /* 0 = 4:3 / 16:9 from the flag */
static int  s_cfg_fullscreen = 0;
static int  s_fullscreen = 0;
static WINDOWPLACEMENT s_windowed_placement;

/* Requested anti-aliasing (samples per pixel), picked up at the next frame
 * boundary; the show-FPS flag and a pending screenshot path, all set from the
 * window's thread and read on the render thread. */
static volatile LONG s_want_msaa = 1;
static volatile LONG s_show_fps = 0;
static volatile LONG s_frames = 0;
/* Texture decode time, filled in by the push-buffer translator. */
volatile long g_tex_uploads_log = 0;
volatile double g_tex_upload_ms = 0;
static DWORD s_start_tick = 0;
static CRITICAL_SECTION s_shot_lock;
static volatile LONG s_shot_lock_ready = 0;
static WCHAR s_shot_path[MAX_PATH];
static volatile LONG s_toast_tick = 0;
static WCHAR s_toast[128];

static D3D8HostUiHooks s_ui_hooks;

void d3d8_SetHostDisplay(unsigned render_w, unsigned render_h,
                         int widescreen, int fullscreen)
{
    s_cfg_render_w   = render_w;
    s_cfg_render_h   = render_h;
    s_cfg_widescreen = widescreen ? 1 : 0;
    s_cfg_fullscreen = fullscreen ? 1 : 0;
}

void d3d8_SetHostAspect(double aspect)
{
    s_cfg_aspect = aspect > 1.0 ? aspect : 0.0;
}

double d3d8_HostAspect(void)
{
    if (s_cfg_aspect > 0.0) return s_cfg_aspect;
    return s_cfg_widescreen ? 16.0 / 9.0 : 4.0 / 3.0;
}

/* Fork: the image about to be presented is shown in a centred frame
 * (black bars), set by the translator per image (D3D8_PRESENT_BOX_*). */
static volatile LONG s_present_box;

void d3d8_SetPresentBox(int mode)
{
    InterlockedExchange(&s_present_box, mode == D3D8_PRESENT_BOX_FRAME || mode == D3D8_PRESENT_BOX_169 ? mode : 0);
}

/* The frame's shape, 16:9 (default) or 4:3 (menus whole at 4:3). */
static double s_present_box_shape = 16.0 / 9.0;

void d3d8_SetPresentBoxShape(double shape)
{
    s_present_box_shape = shape > 1.0 ? shape : 16.0 / 9.0;
}

/* Fork: video frames keep the shape they were authored for.
 *
 * The title's video player copies its decoded MPEG pictures 1:1 into the
 * guest framebuffer (576x448 for almost every movie, 640x448 for two, inside
 * a 640x480 buffer -- see hook_lockrect_0016B1C0 and the guest framebuffer
 * note in nv2a_pgraph_d3d11.c). That buffer is a 4:3 picture: a 4:3 TV shows
 * the movies with their own proportions. Presenting it across a 16:9 or wider
 * window stretched every movie by a third or more. With this on, a present
 * that shows a video frame is shown at the guest framebuffer's own shape,
 * centred with black bars; nothing about the player, its timing or its
 * buffers changes, only the rectangle the finished image is drawn into.
 * XBOX_FIX_VIDEO_ASPECT=0 restores the stretched picture. */
static int video_aspect_fix(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XBOX_FIX_VIDEO_ASPECT");
        on = !(e && e[0] == '0');
        fprintf(stderr, "[D3D] XBOX_FIX_VIDEO_ASPECT=%d (videos %s)\n", on,
                on ? "at their own 4:3 shape" : "stretched to the window");
        xbox_log_flush();
    }
    return on;
}

/* The shape the current image is shown at. */
static double present_aspect(void)
{
    double a = d3d8_HostAspect();
    if (s_video_frame && g_guest_fb_w && g_guest_fb_h && video_aspect_fix()) {
        double va = (double)g_guest_fb_w / (double)g_guest_fb_h;
        if (a > va + 0.005) return va;  /* narrower windows: as before */
    }
    {
        LONG box = s_present_box;
        double fs = box == D3D8_PRESENT_BOX_169 ? 16.0 / 9.0 : s_present_box_shape;
        return box && a > fs + 0.005 ? fs : a;
    }
}

/* Columns of a w-wide capture the image covers when framed (x0, width);
 * 0 when it covers them all. */
static int present_box_cols(unsigned w, unsigned *x0, unsigned *bw)
{
    double a = d3d8_HostAspect(), pa = present_aspect();
    if (!(pa < a)) return 0;
    *bw = (unsigned)(w * pa / a + 0.5);
    if (*bw >= w || !*bw) return 0;
    *x0 = (w - *bw) / 2;
    return 1;
}

/* One captured row (B, G, R) as shown: squeezed into [x0, x0 + bw) with
 * linear filtering, black elsewhere. */
static void present_box_row(const unsigned char *src, unsigned char *dst, unsigned w,
                            unsigned x0, unsigned bw)
{
    unsigned x, c;
    for (x = 0; x < w; x++) {
        if (x < x0 || x >= x0 + bw) { dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = 0; continue; }
        {
            double u = ((double)(x - x0) + 0.5) * (double)w / (double)bw - 0.5;
            unsigned i0 = u <= 0.0 ? 0u : (unsigned)u, i1 = i0 + 1u < w ? i0 + 1u : i0;
            double t = u <= 0.0 ? 0.0 : u - (double)i0;
            for (c = 0; c < 3; c++)
                dst[x * 3 + c] = (unsigned char)(src[i0 * 3 + c] * (1.0 - t) + src[i1 * 3 + c] * t + 0.5);
        }
    }
}

static float s_point_zoom = 1.0f;

void d3d8_SetPointZoom(float zoom)
{
    s_point_zoom = zoom > 0.0f ? zoom : 1.0f;
}

float d3d8_PointZoom(void)
{
    return s_point_zoom;
}

void d3d8_SetHostUiHooks(const D3D8HostUiHooks *hooks)
{
    if (hooks) s_ui_hooks = *hooks;
    else memset(&s_ui_hooks, 0, sizeof s_ui_hooks);
}


void d3d8_SetMsaa(int samples)
{
    InterlockedExchange(&s_want_msaa, (samples == 2 || samples == 4 || samples == 8) ? samples : 1);
}
int  d3d8_GetMsaa(void)          { return (int)s_want_msaa; }
void d3d8_SetShowFps(int on)     { InterlockedExchange(&s_show_fps, on ? 1 : 0); }
int  d3d8_GetShowFps(void)       { return (int)s_show_fps; }
int  d3d8_HostIsWidescreen(void) { return s_cfg_widescreen; }
int  d3d8_HostIsFullscreen(void) { return s_fullscreen; }

/* 0 = not made, 1 = being made, 2 = ready: the render thread only enters
 * the lock once it reads 2 (InitializeCriticalSection is done by then). */
static void shot_lock_init(void)
{
    if (InterlockedCompareExchange(&s_shot_lock_ready, 1, 0) == 0) {
        InitializeCriticalSection(&s_shot_lock);
        InterlockedExchange(&s_shot_lock_ready, 2);
    } else {
        while (s_shot_lock_ready != 2) Sleep(0);
    }
}

/* Saved as PNG at the end of the next frame, from the scene target. */
void d3d8_RequestScreenshot(const wchar_t *path)
{
    shot_lock_init();
    EnterCriticalSection(&s_shot_lock);
    lstrcpynW(s_shot_path, path ? path : L"", MAX_PATH);
    LeaveCriticalSection(&s_shot_lock);
}

/* 1 while a requested screenshot has not been taken yet (bench tools). */
int d3d8_ScreenshotPending(void)
{
    return ((volatile WCHAR *)s_shot_path)[0] != 0;
}

/* A short note in the title bar for a few seconds ("Screenshot saved"). */
void d3d8_HostToast(const wchar_t *text)
{
    lstrcpynW(s_toast, text ? text : L"", 128);
    InterlockedExchange(&s_toast_tick, (LONG)GetTickCount());
}

/* Called once each by d3d8_HostExit before the process ends, in the order
 * they were set (up to 4; NULL, or one set already: nothing). An optional
 * module may close what it opened; the PlayStation controllers stop
 * their motors. */
#define HOST_EXIT_HOOKS 4
static void (*volatile s_host_exit_hooks[HOST_EXIT_HOOKS])(void);

void d3d8_SetHostExitHook(void (*fn)(void))
{
    int i;
    if (!fn) return;
    for (i = 0; i < HOST_EXIT_HOOKS; i++) {
        void *was = InterlockedCompareExchangePointer((void *volatile *)&s_host_exit_hooks[i], (void *)fn, NULL);
        if (!was || was == (void *)fn) return;
    }
}

/* Called at each present on the image about to be shown (NULL: nothing):
 * returns 1 with another image in *out / *out_srv (a copy with something
 * drawn over it), else 0. Only an optional module sets it. */
static int (*s_present_image_overlay)(ID3D11Texture2D *img, ID3D11Texture2D **out,
                                      ID3D11ShaderResourceView **out_srv);

void d3d8_SetPresentImageOverlay(int (*fn)(ID3D11Texture2D *img, ID3D11Texture2D **out,
                                           ID3D11ShaderResourceView **out_srv))
{
    s_present_image_overlay = fn;
}

/* Closing the window ends the game at once: the title has no quit path of
 * its own (a console is switched off), and waiting on its threads is what
 * left the window unclosable -- WM_CLOSE used to set a flag nothing read. */
void d3d8_HostExit(void)
{
    int i;
    for (i = 0; i < HOST_EXIT_HOOKS && s_host_exit_hooks[i]; i++) s_host_exit_hooks[i]();
    d3d8_sync_report();                 /* [PACING] summary */
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 0);
}

/* XBOX_NO_ACTIVATE=1 (test tools only, never set by the launcher): the
 * window never takes the focus from the program in front. It is shown
 * without activation and put behind the other windows, and every later
 * move or resize keeps it so (SWP_NOACTIVATE, no SW_RESTORE); with
 * WS_EX_NOACTIVATE it never becomes the active window, not even when the
 * window in front is minimised or closed, nor when it is clicked. The game
 * renders, plays and takes its captures the same way: nothing here reads
 * the focus (no pause or mute in the background; keyboard keys are read
 * only while the window is in front, and test key presses don't need it).
 * Off by default: unset or 0 changes nothing. */
static int host_no_activate(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_NO_ACTIVATE"); on = (e && e[0] && e[0] != '0'); }
    return on;
}

/* Borderless fullscreen on the window's monitor, or back to the saved
 * window. Only the window changes; the swap chain follows the new client
 * size at the next present (host_output_resize), and the scene target --
 * the render resolution -- does not change at all. The menu bar is hidden
 * in fullscreen. Runs on the window's thread. */
static void host_window_set_fullscreen(HWND h, int on)
{
    /* XBOX_NO_ACTIVATE while another window is in front: same change, but
     * the window keeps its place behind and is not activated. */
    int quiet;
    if (!h || on == s_fullscreen) return;
    quiet = host_no_activate() && GetForegroundWindow() != h;
    if (on) {
        MONITORINFO mi;
        s_windowed_placement.length = sizeof s_windowed_placement;
        GetWindowPlacement(h, &s_windowed_placement);
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        if (s_host_menu) SetMenu(h, NULL);
        SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER |
                     (quiet ? SWP_NOZORDER | SWP_NOACTIVATE : 0));
    } else {
        WINDOWPLACEMENT wp = s_windowed_placement;
        SetWindowLongPtrW(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        if (s_host_menu) SetMenu(h, s_host_menu);
        if (quiet)                      /* every other show command activates */
            wp.showCmd = wp.showCmd == SW_SHOWMINIMIZED ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE;
        SetWindowPlacement(h, &wp);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED |
                     (quiet ? SWP_NOACTIVATE : 0));
    }
    s_fullscreen = on;
}

void d3d8_HostSetFullscreen(int on)
{
    host_window_set_fullscreen(s_host_window, on ? 1 : 0);
}

/* Resize the window so its client area -- the picture -- is w x h, leaving
 * fullscreen or a maximised state first. Runs on the window's thread. */
void d3d8_HostSetClientSize(unsigned w, unsigned h)
{
    HWND hw = s_host_window;
    RECT r;
    int quiet;                          /* XBOX_NO_ACTIVATE, see host_no_activate */
    if (!hw || !w || !h) return;
    quiet = host_no_activate() && GetForegroundWindow() != hw;
    if (s_fullscreen) host_window_set_fullscreen(hw, 0);
    if (IsZoomed(hw)) ShowWindow(hw, quiet ? SW_SHOWNOACTIVATE : SW_RESTORE);
    r.left = 0; r.top = 0; r.right = (LONG)w; r.bottom = (LONG)h;
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(hw, GWL_STYLE), GetMenu(hw) != NULL,
                       (DWORD)GetWindowLongPtrW(hw, GWL_EXSTYLE));
    SetWindowPos(hw, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                 (quiet ? SWP_NOACTIVATE : 0));
}

/* Title bar: the name, frames per second if asked for, and any short note. */
static void host_window_update_title(HWND h)
{
    static LONG last_frames = 0;
    static DWORD last_tick = 0;
    static UINT fps = 0;
    WCHAR title[256];
    DWORD now = GetTickCount();
    LONG frames = s_frames;

    if (last_tick && now - last_tick >= 900) {
        fps = (UINT)((frames - last_frames) * 1000u / (now - last_tick));
        last_frames = frames;
        last_tick = now;
    } else if (!last_tick) {
        last_tick = now;
        last_frames = frames;
    }
    lstrcpyW(title, L"SSX Tricky");
    if (s_show_fps) {
        WCHAR part[32];
        wsprintfW(part, L"  \x00B7  %u fps", fps);
        lstrcatW(title, part);
    }
    if (s_toast_tick && now - (DWORD)s_toast_tick < 3000 && s_toast[0]) {
        lstrcatW(title, L"  \x00B7  ");
        lstrcatW(title, s_toast);
    }
    {
        WCHAR cur[256];
        GetWindowTextW(h, cur, 256);
        if (lstrcmpW(cur, title) != 0) SetWindowTextW(h, title);
    }
}

/* XBOX_NO_ACTIVATE: WS_EX_NOACTIVATE does not cover everything. When the
 * window in front closes, Windows hands the activation to the next window
 * even so (seen: ours in front for 9.8 s after another program's game
 * closed). Then the activation is given back at once, on the window's
 * thread: to the window that lost it if it is still there, else to the
 * last window seen in front (noted by the title timer), else to the next
 * visible window of another program in the z-order, else to the desktop;
 * and ours goes to the bottom of the z-order. The game itself never stops:
 * only the window changes. */
#define HOST_WM_HAND_BACK (WM_APP + 0x606)
static HWND s_na_prev;                  /* the window that lost the activation */
static HWND s_na_last_front;            /* the last window in front that was not ours */
static LONG s_na_logged;

static BOOL na_is_ours(HWND w)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

/* A window that can take the activation back: there, shown, not minimised,
 * another program's, not a tool or no-activate window, not cloaked (a
 * suspended store app is "visible" but not on screen), not empty. */
static BOOL na_candidate(HWND w)
{
    typedef HRESULT (WINAPI *DwmAttrFn)(HWND, DWORD, PVOID, DWORD);
    static DwmAttrFn dwm_attr;
    static LONG dwm_tried;
    LONG_PTR ex;
    RECT r;
    DWORD cloaked = 0;
    if (!w || !IsWindow(w) || !IsWindowVisible(w) || IsIconic(w) || na_is_ours(w)) return FALSE;
    ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return FALSE;
    if (!GetWindowRect(w, &r) || r.right <= r.left || r.bottom <= r.top) return FALSE;
    if (!InterlockedExchange(&dwm_tried, 1)) {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        dwm_attr = m ? (DwmAttrFn)(void *)GetProcAddress(m, "DwmGetWindowAttribute") : NULL;
    }
    if (dwm_attr && SUCCEEDED(dwm_attr(w, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof cloaked)) && cloaked)
        return FALSE;
    return TRUE;
}

static void na_note_front(void)
{
    HWND f = GetForegroundWindow();
    if (f && !na_is_ours(f)) s_na_last_front = f;
}

static void na_hand_back(HWND self)
{
    HWND to = NULL, w;
    const char *how = "the desktop";
    BOOL ok;
    if (GetForegroundWindow() != self) return;        /* already gone elsewhere */
    if (na_candidate(s_na_prev)) { to = s_na_prev; how = "the window that lost it"; }
    else if (na_candidate(s_na_last_front)) { to = s_na_last_front; how = "the last window in front"; }
    else {
        for (w = GetTopWindow(NULL); w; w = GetWindow(w, GW_HWNDNEXT))
            if (na_candidate(w)) { to = w; how = "the next window in the z-order"; break; }
    }
    if (!to) to = GetShellWindow();
    ok = to && SetForegroundWindow(to);
    SetWindowPos(self, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (InterlockedIncrement(&s_na_logged) <= 20) {
        fprintf(stderr, "[FOCUS] XBOX_NO_ACTIVATE: activated by the system, handed back to %s (hwnd=%p)%s; "
                "window sent to the bottom\n", how, (void *)to, ok ? "" : " -- refused");
        fflush(stderr);
    }
}

static LRESULT CALLBACK host_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (host_no_activate()) {
        if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) {
            /* Not here: Windows is still in the middle of the switch. */
            s_na_prev = (HWND)lp;
            PostMessageW(h, HOST_WM_HAND_BACK, 0, 0);
        } else if (msg == HOST_WM_HAND_BACK) {
            na_hand_back(h);
            return 0;
        } else if (msg == WM_TIMER) {
            na_note_front();
        }
    }
    switch (msg) {
        case WM_DISPLAYCHANGE:          /* the frame rate limit follows the display's rate */
            d3d8_sync_display_changed(0);
            break;
        case WM_MOVE:
            d3d8_sync_display_changed(1);
            break;
        case WM_CLOSE:
            s_host_window_close_requested = TRUE;
            d3d8_HostExit();
            return 0;
        case WM_DESTROY:
            s_host_window = NULL;
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;   /* the renderer owns every pixel; avoid flicker */
        case WM_TIMER:
            host_window_update_title(h);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == 0 && lp == 0 && s_ui_hooks.on_command) {
                s_ui_hooks.on_command(h, LOWORD(wp));
                return 0;
            }
            break;
        case WM_INITMENUPOPUP:
            if (s_ui_hooks.on_init_menu) s_ui_hooks.on_init_menu((HMENU)wp);
            return 0;
        case WM_SYSCOMMAND:
            /* No menu bar: Alt or F10 alone would select the
             * window menu and steal the next keys from the game. Alt+Space
             * (lp = ' ') still opens it. */
            if ((wp & 0xFFF0) == SC_KEYMENU && !s_host_menu && lp == 0)
                return 0;
            break;
        case WM_ENTERMENULOOP:
        case WM_EXITMENULOOP:
            if (s_ui_hooks.on_menu_loop) s_ui_hooks.on_menu_loop(msg == WM_ENTERMENULOOP);
            break;
        case WM_KEYDOWN:
            if (s_ui_hooks.on_key && !(lp & (1 << 30)) && s_ui_hooks.on_key(h, (UINT)wp))
                return 0;
            break;
        case WM_SYSKEYDOWN:
            /* Alt+Enter toggles borderless fullscreen. DXGI's own handler
             * (exclusive mode) is switched off in the device setup. */
            if (wp == VK_RETURN && (lp & (1 << 29))) {
                host_window_set_fullscreen(h, !s_fullscreen);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (wp == VK_RETURN) return 0;   /* no "invalid key" beep */
            break;
        case WM_SETCURSOR:
            if (s_fullscreen && LOWORD(lp) == HTCLIENT) {
                SetCursor(NULL);
                return TRUE;
            }
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND host_window_create_here(UINT width, UINT height)
{
    static const WCHAR *cls = L"XboxRecompOutput";
    WNDCLASSEXW wc;
    HINSTANCE inst = GetModuleHandleW(NULL);
    MONITORINFO mi;
    POINT origin = { 0, 0 };
    RECT r;
    HWND h;
    LONG ww, wh, x, y;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = host_wnd_proc;
    wc.hInstance     = inst;
    wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = cls;
    /* Re-registering is harmless if a previous device init already did it. */
    RegisterClassExW(&wc);

    s_host_menu = s_ui_hooks.create_menu ? s_ui_hooks.create_menu() : NULL;

    /* Size the *client* area to the requested output. A window that would
     * not fit the monitor's work area is shrunk, keeping its shape; the
     * scene is then scaled down into it (and shown 1:1 in fullscreen). */
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    {
        RECT frame = { 0, 0, 0, 0 };
        LONG maxw, maxh;
        AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, s_host_menu != NULL);
        maxw = (mi.rcWork.right - mi.rcWork.left) - (frame.right - frame.left);
        maxh = (mi.rcWork.bottom - mi.rcWork.top) - (frame.bottom - frame.top);
        if (maxw > 0 && maxh > 0 && ((LONG)width > maxw || (LONG)height > maxh)) {
            double kx = (double)maxw / width, ky = (double)maxh / height;
            double k = kx < ky ? kx : ky;
            width  = (UINT)(width * k);
            height = (UINT)(height * k);
        }
    }
    r.left = 0; r.top = 0; r.right = (LONG)width; r.bottom = (LONG)height;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, s_host_menu != NULL);
    ww = r.right - r.left;
    wh = r.bottom - r.top;
    x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
    y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top)  y = mi.rcWork.top;

    /* XBOX_NO_ACTIVATE: WS_EX_NOACTIVATE too, or minimising or closing the
     * window in front would hand it the focus (WS_EX_APPWINDOW keeps its
     * task bar button). */
    h = CreateWindowExW(host_no_activate() ? WS_EX_NOACTIVATE | WS_EX_APPWINDOW : 0, cls, L"SSX Tricky",
                        WS_OVERLAPPEDWINDOW, x, y, ww, wh,
                        NULL, s_host_menu, inst, NULL);
    if (!h) {
        fprintf(stderr, "D3D8: could not create output window (error %lu)\n",
                GetLastError());
        return NULL;
    }

    if (host_no_activate()) {
        /* Shown without the focus and behind the window in front, in one
         * step (shown first, it would cover that window for a moment). */
        SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        na_note_front();
    } else
        ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    s_host_window = h;
    SetTimer(h, 1, 500, NULL);
    if (s_cfg_fullscreen)
        host_window_set_fullscreen(h, 1);
    fprintf(stderr, "D3D8: created output window %ux%u%s%s%s (hwnd=%p)\n",
            width, height, s_fullscreen ? ", fullscreen" : "",
            s_host_menu ? ", menu" : "",
            host_no_activate() ? ", not activated (XBOX_NO_ACTIVATE)" : "", (void *)h);
    fprintf(stderr, "[TIME] output window at t+%.0f ms\n", d3d8_ms_since_start());
    fflush(stderr);
    return h;
}

/*
 * The window lives on a thread of its own. It used to belong to
 * whichever thread created the device and was only serviced when a frame was
 * presented, so during a load, while a menu was open, or while the window was
 * dragged, nothing answered it -- which is also why closing it did nothing.
 * Now its messages are always handled, and a menu or a resize never stalls
 * the renderer. DXGI is content with a window owned by another thread as long
 * as that thread pumps, and nothing here makes the window thread wait on the
 * render thread.
 */
static HANDLE s_ui_ready = NULL;
static UINT   s_ui_w, s_ui_h;

static DWORD WINAPI host_ui_thread(LPVOID unused)
{
    MSG msg;
    (void)unused;
    host_window_create_here(s_ui_w, s_ui_h);
    SetEvent(s_ui_ready);
    if (!s_host_window) return 1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static HWND host_window_create(UINT width, UINT height)
{
    HANDLE t;
    if (!s_start_tick) s_start_tick = GetTickCount();
    if (s_host_window) return s_host_window;
    s_ui_w = width;
    s_ui_h = height;
    s_ui_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    t = CreateThread(NULL, 0, host_ui_thread, NULL, 0, NULL);
    if (!t || !s_ui_ready) return NULL;
    CloseHandle(t);
    WaitForSingleObject(s_ui_ready, 15000);
    return s_host_window;
}

/* ── DAC palette / gamma ramp (fork) ───────────────────────────
 *
 * The NV2A's DAC passes every scanned-out pixel through its palette, and the
 * title loads a gamma ramp there (slope 0.875): the TV picture is that
 * much darker than the framebuffer. nv2a_core.c emulates the palette
 * registers and hands each full table over here. With the fix on (default) it is
 * applied to the SHOWN image only, as the last step of the present (after
 * the post and the overlay, during the blit) -- never to scene_tex, which
 * prev_frame_update copies for the "Master" chrome: on the console the chrome
 * reads framebuffer memory, which the DAC does not change. The video frames
 * go through it too: the DAC applies to everything it scans out. Frame dumps
 * and screenshots apply it on the CPU, so they show what the window shows.
 * On by default; XBOX_FIX_GAMMA=0 turns it off. */
static CRITICAL_SECTION s_dac_lock;
static volatile LONG    s_dac_lock_ready;
static unsigned char    s_dac_lut[256][3];
static volatile LONG    s_dac_dirty;        /* a new table to upload */
static volatile LONG    s_dac_have;         /* a table was received */
static int              s_dac_identity = 1;

/* 0 = not made, 1 = being made, 2 = ready (as shot_lock_init). */
static void dac_lock_init(void)
{
    if (InterlockedCompareExchange(&s_dac_lock_ready, 1, 0) == 0) {
        InitializeCriticalSection(&s_dac_lock);
        InterlockedExchange(&s_dac_lock_ready, 2);
    } else {
        while (s_dac_lock_ready != 2) Sleep(0);
    }
}

int d3d8_GammaFixEnabled(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_GAMMA"); on = !(e && e[0] == '0'); }
    return on;
}

/* Called by the translator's DAC emulation (game thread, MMIO). */
void d3d8_SetDacPalette(const unsigned char lut[256][3])
{
    unsigned i;
    int ident = 1;
    if (s_dac_lock_ready != 2) dac_lock_init();
    EnterCriticalSection(&s_dac_lock);
    memcpy(s_dac_lut, lut, sizeof s_dac_lut);
    for (i = 0; i < 256; i++)
        if (lut[i][0] != i || lut[i][1] != i || lut[i][2] != i) ident = 0;
    s_dac_identity = ident;
    LeaveCriticalSection(&s_dac_lock);
    InterlockedExchange(&s_dac_have, 1);
    InterlockedExchange(&s_dac_dirty, 1);
}

/* 1 if the shown image must go through the palette (fix on, a non-identity
 * table received). */
static int dac_active(void)
{
    return d3d8_GammaFixEnabled() && s_dac_have && !s_dac_identity;
}

/* CPU copy of the table for captures (R, G, B channels). */
static void dac_lut_copy(unsigned char out[256][3])
{
    if (s_dac_lock_ready != 2) dac_lock_init();
    EnterCriticalSection(&s_dac_lock);
    memcpy(out, s_dac_lut, sizeof s_dac_lut);
    LeaveCriticalSection(&s_dac_lock);
}

/* ── Scene-to-window scaling ───────────────────────────────────────── */

/* One full-screen triangle sampling a texture: used to scale the scene into
 * the window, and the video player's 640x480 frames into a larger scene. */
static const char s_blit_hlsl[] =
    "Texture2D t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "    V o;\n"
    "    o.uv = float2((id << 1) & 2, id & 2);\n"
    "    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n"
    /* The same, then through the DAC palette (256x1 RGBA8, t1) */
    "Texture2D lut : register(t1);\n"
    "float4 ps_lut(V i) : SV_Target {\n"
    "    float3 c = t.Sample(s, i.uv).rgb;\n"
    "    int3 k = int3(round(saturate(c) * 255.0));\n"
    "    return float4(lut.Load(int3(k.r, 0, 0)).r, lut.Load(int3(k.g, 0, 0)).g,\n"
    "                  lut.Load(int3(k.b, 0, 0)).b, 1);\n"
    "}\n";

static ID3D11VertexShader    *s_blit_vs;
static ID3D11PixelShader     *s_blit_ps;
static ID3D11PixelShader     *s_blit_ps_lut;
static ID3D11Texture2D       *s_lut_tex;
static ID3D11ShaderResourceView *s_lut_srv;
static ID3D11SamplerState    *s_blit_smp;
static ID3D11RasterizerState *s_blit_rs;

static int blit_init(void)
{
    static int failed = 0;
    ID3D11Device *dev = g_device_state.d3d11_device;
    ID3D10Blob *code = NULL, *err = NULL;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;

    if (s_blit_vs && s_blit_ps && s_blit_smp && s_blit_rs) return 1;
    if (failed || !dev) return 0;
    failed = 1;   /* cleared on success; never retried every frame */

    if (FAILED(d3d8_shader_compile_logged(s_blit_hlsl, sizeof s_blit_hlsl - 1, "blit", NULL,
                                          "vs", "vs_5_0", 0, &code, &err))) goto fail;
    if (FAILED(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code), NULL, &s_blit_vs))) goto fail;
    ID3D10Blob_Release(code); code = NULL;
    if (FAILED(d3d8_shader_compile_logged(s_blit_hlsl, sizeof s_blit_hlsl - 1, "blit", NULL,
                                          "ps", "ps_5_0", 0, &code, &err))) goto fail;
    if (FAILED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code), NULL, &s_blit_ps))) goto fail;
    ID3D10Blob_Release(code); code = NULL;
    if (FAILED(d3d8_shader_compile_logged(s_blit_hlsl, sizeof s_blit_hlsl - 1, "blit", NULL,
                                          "ps_lut", "ps_5_0", 0, &code, &err))) goto fail;
    if (FAILED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code), NULL, &s_blit_ps_lut))) goto fail;
    ID3D10Blob_Release(code); code = NULL;

    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(dev, &sd, &s_blit_smp))) goto fail;

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &s_blit_rs))) goto fail;

    failed = 0;
    return 1;
fail:
    if (err) {
        fprintf(stderr, "D3D8: blit shader: %s\n", (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    } else {
        fprintf(stderr, "D3D8: could not create the scaling blit\n");
    }
    if (code) ID3D10Blob_Release(code);
    fflush(stderr);
    return 0;
}

/* Draw srv into rtv over vp. Everything the blit touches is saved first and
 * put back afterwards: the translator's state cache assumes the pipeline
 * still holds what it last set, so a present must leave no trace. */
static void blit_draw_lut(ID3D11ShaderResourceView *srv, ID3D11RenderTargetView *rtv,
                          const D3D11_VIEWPORT *vp, ID3D11ShaderResourceView *lut);

static void blit_draw(ID3D11ShaderResourceView *srv, ID3D11RenderTargetView *rtv,
                      const D3D11_VIEWPORT *vp)
{
    blit_draw_lut(srv, rtv, vp, NULL);
}

/* blit_draw, through the DAC palette when lut is given. */
static void blit_draw_lut(ID3D11ShaderResourceView *srv, ID3D11RenderTargetView *rtv,
                          const D3D11_VIEWPORT *vp, ID3D11ShaderResourceView *lut)
{
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context;
    ID3D11RenderTargetView *o_rtv = NULL;
    ID3D11DepthStencilView *o_dsv = NULL;
    D3D11_VIEWPORT o_vp;
    UINT o_nvp = 1;
    ID3D11VertexShader *o_vs = NULL;
    ID3D11PixelShader *o_ps = NULL;
    ID3D11InputLayout *o_il = NULL;
    D3D11_PRIMITIVE_TOPOLOGY o_topo;
    ID3D11ShaderResourceView *o_srv = NULL, *none = NULL, *o_srv1 = NULL;
    ID3D11SamplerState *o_smp = NULL;
    ID3D11BlendState *o_bs = NULL;
    FLOAT o_bf[4];
    UINT o_sm = 0;
    ID3D11DepthStencilState *o_ds = NULL;
    UINT o_sref = 0;
    ID3D11RasterizerState *o_rs = NULL;

    if (!ctx || !srv || !rtv || !blit_init()) return;

    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &o_rtv, &o_dsv);
    ID3D11DeviceContext_RSGetViewports(ctx, &o_nvp, &o_vp);
    ID3D11DeviceContext_VSGetShader(ctx, &o_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(ctx, &o_ps, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(ctx, &o_il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(ctx, &o_topo);
    ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 1, &o_srv);
    ID3D11DeviceContext_PSGetShaderResources(ctx, 1, 1, &o_srv1);
    ID3D11DeviceContext_PSGetSamplers(ctx, 0, 1, &o_smp);
    ID3D11DeviceContext_OMGetBlendState(ctx, &o_bs, o_bf, &o_sm);
    ID3D11DeviceContext_OMGetDepthStencilState(ctx, &o_ds, &o_sref);
    ID3D11DeviceContext_RSGetState(ctx, &o_rs);

    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, lut ? s_blit_ps_lut : s_blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    if (lut) ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &lut);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &s_blit_smp);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_RSSetState(ctx, s_blit_rs);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    /* Unbind the source before its texture can be a render target again. */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    if (lut) ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &none);

    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &o_rtv, o_dsv);
    if (o_nvp) ID3D11DeviceContext_RSSetViewports(ctx, 1, &o_vp);
    ID3D11DeviceContext_VSSetShader(ctx, o_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, o_ps, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, o_il);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, o_topo);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &o_srv);
    if (lut) ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &o_srv1);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &o_smp);
    ID3D11DeviceContext_OMSetBlendState(ctx, o_bs, o_bf, o_sm);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, o_ds, o_sref);
    ID3D11DeviceContext_RSSetState(ctx, o_rs);

    if (o_rtv) ID3D11RenderTargetView_Release(o_rtv);
    if (o_dsv) ID3D11DepthStencilView_Release(o_dsv);
    if (o_vs)  ID3D11VertexShader_Release(o_vs);
    if (o_ps)  ID3D11PixelShader_Release(o_ps);
    if (o_il)  ID3D11InputLayout_Release(o_il);
    if (o_srv) ID3D11ShaderResourceView_Release(o_srv);
    if (o_srv1) ID3D11ShaderResourceView_Release(o_srv1);
    if (o_smp) ID3D11SamplerState_Release(o_smp);
    if (o_bs)  ID3D11BlendState_Release(o_bs);
    if (o_ds)  ID3D11DepthStencilState_Release(o_ds);
    if (o_rs)  ID3D11RasterizerState_Release(o_rs);
}

/* The DAC palette as a 256x1 texture, refreshed when a new table came in
 * (pump thread). NULL when the fix is off or the table is the identity. */
static ID3D11ShaderResourceView *dac_lut_srv(void)
{
    ID3D11Device *dev = g_device_state.d3d11_device;
    if (!dac_active() || !dev) return NULL;
    if (!s_lut_tex) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = 256; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &s_lut_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)s_lut_tex,
                                                         NULL, &s_lut_srv)))
            return NULL;
        InterlockedExchange(&s_dac_dirty, 1);
    }
    if (InterlockedExchange(&s_dac_dirty, 0)) {
        unsigned char lut[256][3], rgba[256 * 4];
        unsigned i;
        dac_lut_copy(lut);
        for (i = 0; i < 256; i++) {
            rgba[i * 4 + 0] = lut[i][0]; rgba[i * 4 + 1] = lut[i][1];
            rgba[i * 4 + 2] = lut[i][2]; rgba[i * 4 + 3] = 255;
        }
        ID3D11DeviceContext_UpdateSubresource(g_device_state.d3d11_context,
            (ID3D11Resource *)s_lut_tex, 0, NULL, rgba, sizeof rgba, 0);
    }
    return s_lut_srv;
}

/* (Re)acquire the swap chain's back buffer and a view of it. */
static HRESULT swap_targets_create(D3D8DeviceState *s)
{
    DXGI_SWAP_CHAIN_DESC d;
    HRESULT hr;

    hr = IDXGISwapChain_GetBuffer(s->swap_chain, 0, &IID_ID3D11Texture2D,
                                  (void **)&s->swap_tex);
    if (FAILED(hr)) return hr;
    hr = ID3D11Device_CreateRenderTargetView(s->d3d11_device,
            (ID3D11Resource *)s->swap_tex, NULL, &s->swap_rtv);
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(IDXGISwapChain_GetDesc(s->swap_chain, &d))) {
        s->swap_w = d.BufferDesc.Width;
        s->swap_h = d.BufferDesc.Height;
    }
    return S_OK;
}

/*
 * The scene target and its depth buffer, at the render size, with `msaa`
 * samples per pixel. With anti-aliasing the title draws into a
 * multisampled texture that is resolved into scene_tex, the sampleable copy
 * everything downstream reads (the present, captures, screenshots). A count
 * the adapter cannot do falls back to the next lower one.
 */
static void scene_targets_release(D3D8DeviceState *s)
{
    d3d8_lights_release();              /* fork: their targets follow these */
    d3d8_bloom_release();
    d3d8_temporal_release();
    d3d8_haze_halo_release();
    if (s->depth_srv)     { ID3D11ShaderResourceView_Release(s->depth_srv); s->depth_srv = NULL; }
    if (s->default_dsv)   { ID3D11DepthStencilView_Release(s->default_dsv); s->default_dsv = NULL; }
    if (s->default_depth) { ID3D11Texture2D_Release(s->default_depth); s->default_depth = NULL; }
    if (s->default_rtv)   { ID3D11RenderTargetView_Release(s->default_rtv); s->default_rtv = NULL; }
    if (s->scene_srv)     { ID3D11ShaderResourceView_Release(s->scene_srv); s->scene_srv = NULL; }
    if (s->scene_ms)      { ID3D11Texture2D_Release(s->scene_ms); s->scene_ms = NULL; }
    if (s->scene_tex)     { ID3D11Texture2D_Release(s->scene_tex); s->scene_tex = NULL; }
}

static HRESULT scene_targets_create(D3D8DeviceState *s, UINT msaa)
{
    static const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D11Device *dev = s->d3d11_device;
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    while (msaa > 1) {
        UINT qc = 0, qd = 0;
        if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(dev,
                DXGI_FORMAT_R8G8B8A8_UNORM, msaa, &qc)) && qc &&
            SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(dev,
                DXGI_FORMAT_D24_UNORM_S8_UINT, msaa, &qd)) && qd)
            break;
        msaa >>= 1;
    }
    if (msaa < 1) msaa = 1;

    memset(&td, 0, sizeof td);
    td.Width = s->width;
    td.Height = s->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &s->scene_tex);
    if (FAILED(hr)) return hr;
    hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)s->scene_tex,
                                               NULL, &s->scene_srv);
    if (FAILED(hr)) return hr;

    if (msaa > 1) {
        td.SampleDesc.Count = msaa;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &s->scene_ms);
        if (FAILED(hr)) return hr;
    }
    hr = ID3D11Device_CreateRenderTargetView(dev,
            (ID3D11Resource *)(s->scene_ms ? s->scene_ms : s->scene_tex),
            NULL, &s->default_rtv);
    if (FAILED(hr)) return hr;

    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = msaa;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (d3d8_softshadow_enabled() || d3d8_speedfx_wanted() || d3d8_veil_wanted() || d3d8_bloom_wanted() ||
        d3d8_temporal_wanted()) {
        /* The soft shadows read the depth, so the buffer is typeless
         * with a depth view and a shader view; the speed effect too (its
         * Strong blur follows the depth). Off, it is created as before. */
        D3D11_DEPTH_STENCIL_VIEW_DESC dv;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv;
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &s->default_depth);
        if (FAILED(hr)) return hr;
        memset(&dv, 0, sizeof dv);
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = msaa > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)s->default_depth,
                                                 &dv, &s->default_dsv);
        if (FAILED(hr)) return hr;
        memset(&sv, 0, sizeof sv);
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        if (msaa > 1) sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        else { sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1; }
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)s->default_depth,
                                                         &sv, &s->depth_srv)))
            s->depth_srv = NULL;    /* soft shadows then fall back to the title's quad */
    } else {
        hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &s->default_depth);
        if (FAILED(hr)) return hr;
        hr = ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)s->default_depth,
                                                 NULL, &s->default_dsv);
        if (FAILED(hr)) return hr;
    }

    ID3D11DeviceContext_ClearRenderTargetView(s->d3d11_context, s->default_rtv, black);
    ID3D11DeviceContext_ClearDepthStencilView(s->d3d11_context, s->default_dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(s->d3d11_context, 1, &s->default_rtv, s->default_dsv);
    s->msaa = msaa;
    return S_OK;
}

/* Bring scene_tex up to date with a multisampled frame. */
static void scene_resolve(D3D8DeviceState *s)
{
    if (s->scene_ms && s->scene_tex)
        ID3D11DeviceContext_ResolveSubresource(s->d3d11_context,
            (ID3D11Resource *)s->scene_tex, 0, (ID3D11Resource *)s->scene_ms, 0,
            DXGI_FORMAT_R8G8B8A8_UNORM);
}

/* Diagnostic: one pixel of the scene target, (fx, fy) in 0..1 of its size,
 * as 0xAARRGGBB. Resolves and reads back synchronously -- only for draw-log
 * frames ("which draw turned this pixel black"). */
unsigned d3d8_DebugPeekScene(float fx, float fy)
{
    D3D8DeviceState *s = &g_device_state;
    D3D11_TEXTURE2D_DESC td;
    D3D11_BOX box;
    D3D11_MAPPED_SUBRESOURCE m;
    static ID3D11Texture2D *stage;
    unsigned x, y, v = 0xDEADBEEFu;
    if (!cur_tex(s) || !s->d3d11_context) return v;
    cur_resolve(s);                 /* fork: the bound target (render scale) */
    ID3D11Texture2D_GetDesc(cur_tex(s), &td);
    if (!stage) {
        D3D11_TEXTURE2D_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Width = 1; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = td.Format; sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s->d3d11_device, &sd, NULL, &stage))) return v;
    }
    x = (unsigned)(fx * (float)td.Width);  if (x >= td.Width)  x = td.Width - 1;
    y = (unsigned)(fy * (float)td.Height); if (y >= td.Height) y = td.Height - 1;
    box.left = x; box.right = x + 1; box.top = y; box.bottom = y + 1; box.front = 0; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s->d3d11_context, (ID3D11Resource *)stage, 0, 0, 0, 0,
                                              (ID3D11Resource *)cur_tex(s), 0, &box);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s->d3d11_context, (ID3D11Resource *)stage, 0,
                                          D3D11_MAP_READ, 0, &m))) {
        const unsigned char *p = (const unsigned char *)m.pData;   /* R8G8B8A8 */
        v = ((unsigned)p[3] << 24) | ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
        ID3D11DeviceContext_Unmap(s->d3d11_context, (ID3D11Resource *)stage, 0);
    }
    return v;
}

/* Follow the window: a resize, maximise or fullscreen toggle changes the
 * client area, and the swap chain is resized to match so the output is never
 * stretched by the window system. A minimised window keeps its buffers. */
static void host_output_resize(void)
{
    D3D8DeviceState *s = &g_device_state;
    RECT rc;
    UINT cw, ch;
    HRESULT hr;

    if (!s->swap_chain || !s->hwnd || !GetClientRect(s->hwnd, &rc)) return;
    cw = (UINT)(rc.right - rc.left);
    ch = (UINT)(rc.bottom - rc.top);
    if (!cw || !ch) return;
    if (s->swap_rtv && cw == s->swap_w && ch == s->swap_h) return;

    if (s->swap_rtv) { ID3D11RenderTargetView_Release(s->swap_rtv); s->swap_rtv = NULL; }
    if (s->swap_tex) { ID3D11Texture2D_Release(s->swap_tex); s->swap_tex = NULL; }
    /* The texture mirror forgets its views (and its references) first */
    sc_ForgetTextures();
    /* the creation flags (latency, tearing) must be passed again */
    hr = IDXGISwapChain_ResizeBuffers(s->swap_chain, 0, cw, ch, DXGI_FORMAT_UNKNOWN, d3d8_sync_swap_flags());
    if (FAILED(hr))
        fprintf(stderr, "D3D8: resizing the output to %ux%u failed: 0x%08lX\n",
                cw, ch, (unsigned long)hr);
    swap_targets_create(s);
    d3d8_sync_window_changed();         /* the display may have changed */
}

/* PNG encoders for screenshots. Both take top-down BGR rows,
 * stride bytes apart, and return a malloc'd PNG in *out. */
static HRESULT png_encode_wic(const BYTE *bgr, UINT w, UINT h, UINT stride,
                              BYTE **out, size_t *out_len, const char **step)
{
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    IStream *st = NULL;
    HGLOBAL mem = NULL;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    STATSTG stat;
    HRESULT hr, co;

    *out = NULL; *out_len = 0;
    co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* The PNG encoder by its CLSID: IWICImagingFactory::CreateEncoder walks
     * the registered codecs, and one broken registration can fail it. */
    *step = "CoCreateInstance(PngEncoder)";
    hr = CoCreateInstance(&CLSID_WICPngEncoder, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICBitmapEncoder, (void **)&enc);
    if (SUCCEEDED(hr)) { *step = "CreateStreamOnHGlobal"; hr = CreateStreamOnHGlobal(NULL, TRUE, &st); }
    if (SUCCEEDED(hr)) { *step = "Initialize"; hr = IWICBitmapEncoder_Initialize(enc, st, WICBitmapEncoderNoCache); }
    if (SUCCEEDED(hr)) { *step = "CreateNewFrame"; hr = IWICBitmapEncoder_CreateNewFrame(enc, &fr, &props); }
    if (SUCCEEDED(hr)) { *step = "Frame.Initialize"; hr = IWICBitmapFrameEncode_Initialize(fr, props); }
    if (SUCCEEDED(hr)) { *step = "SetSize"; hr = IWICBitmapFrameEncode_SetSize(fr, w, h); }
    if (SUCCEEDED(hr)) { *step = "SetPixelFormat"; hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &pf); }
    if (SUCCEEDED(hr) && !IsEqualGUID(&pf, &GUID_WICPixelFormat24bppBGR)) hr = E_FAIL;
    if (SUCCEEDED(hr)) { *step = "WritePixels";
                         hr = IWICBitmapFrameEncode_WritePixels(fr, h, stride, stride * h, (BYTE *)bgr); }
    if (SUCCEEDED(hr)) { *step = "Frame.Commit"; hr = IWICBitmapFrameEncode_Commit(fr); }
    if (SUCCEEDED(hr)) { *step = "Commit"; hr = IWICBitmapEncoder_Commit(enc); }
    if (SUCCEEDED(hr)) { *step = "Stat"; hr = IStream_Stat(st, &stat, STATFLAG_NONAME); }
    if (SUCCEEDED(hr)) { *step = "GetHGlobalFromStream"; hr = GetHGlobalFromStream(st, &mem); }
    if (SUCCEEDED(hr)) {
        const void *p = GlobalLock(mem);
        *out_len = (size_t)stat.cbSize.QuadPart;
        *out = (BYTE *)malloc(*out_len ? *out_len : 1);
        if (!p || !*out || !*out_len) { *step = "GlobalLock"; hr = E_OUTOFMEMORY; }
        else memcpy(*out, p, *out_len);
        if (p) GlobalUnlock(mem);
    }
    if (props) IPropertyBag2_Release(props);
    if (fr)    IWICBitmapFrameEncode_Release(fr);
    if (enc)   IWICBitmapEncoder_Release(enc);
    if (st)    IStream_Release(st);
    if (SUCCEEDED(co)) CoUninitialize();
    if (FAILED(hr)) { free(*out); *out = NULL; *out_len = 0; }
    return hr;
}

static unsigned long png_crc(unsigned long c, const BYTE *p, size_t n)
{
    static unsigned long t[256];
    size_t i;
    if (!t[1]) {
        unsigned long k, v;
        for (k = 0; k < 256; k++) {
            v = k;
            for (i = 0; i < 8; i++) v = (v & 1) ? 0xEDB88320UL ^ (v >> 1) : v >> 1;
            t[k] = v;
        }
    }
    c ^= 0xFFFFFFFFUL;
    for (i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static BYTE *png_be32(BYTE *p, unsigned long v)
{
    p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16); p[2] = (BYTE)(v >> 8); p[3] = (BYTE)v;
    return p + 4;
}

/* Close the chunk whose data sits at p + 8: length, type, CRC. */
static BYTE *png_chunk_end(BYTE *p, const char *type, size_t len)
{
    png_be32(p, (unsigned long)len);
    memcpy(p + 4, type, 4);
    return png_be32(p + 8 + len, png_crc(0, p + 4, len + 4));
}

/* The fallback: uncompressed (zlib stored blocks), ~6 MB at 1080p, but with
 * no dependency at all. */
static HRESULT png_encode_own(const BYTE *bgr, UINT w, UINT h, UINT stride,
                              BYTE **out, size_t *out_len)
{
    static const BYTE sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    const size_t row = (size_t)w * 3 + 1, raw = row * h;
    const size_t blocks = (raw + 65534) / 65535;
    const size_t zlen = 2 + raw + blocks * 5 + 4;
    BYTE *png, *p, *z;
    size_t left = raw, y, x, done = 0;
    unsigned long a = 1, b = 0;

    *out = NULL; *out_len = 0;
    if (!w || !h) return E_FAIL;
    png = (BYTE *)malloc(8 + 25 + 12 + zlen + 12);
    if (!png) return E_OUTOFMEMORY;
    memcpy(png, sig, 8);
    p = png + 8;
    png_be32(p + 8, w); png_be32(p + 12, h);
    p[16] = 8; p[17] = 2; p[18] = 0; p[19] = 0; p[20] = 0;     /* 8-bit RGB */
    p = png_chunk_end(p, "IHDR", 13);
    z = p + 8;
    *z++ = 0x78; *z++ = 0x01;
    for (y = 0; y < h; y++) {
        const BYTE *src = bgr + y * stride;
        for (x = 0; x < row; x++, done++) {
            /* filter byte 0, then RGB from BGR */
            BYTE v = x == 0 ? 0 : src[(x - 1) / 3 * 3 + 2 - (x - 1) % 3];
            if (done % 65535 == 0) {
                size_t n = left < 65535 ? left : 65535;
                *z++ = (BYTE)(left == n);                          /* BFINAL, stored */
                *z++ = (BYTE)n; *z++ = (BYTE)(n >> 8);
                *z++ = (BYTE)~n; *z++ = (BYTE)(~n >> 8);
                left -= n;
            }
            *z++ = v;
            a = (a + v) % 65521; b = (b + a) % 65521;
        }
    }
    z = png_be32(z, (b << 16) | a);
    p = png_chunk_end(p, "IDAT", (size_t)(z - (p + 8)));
    p = png_chunk_end(p, "IEND", 0);
    *out = png;
    *out_len = (size_t)(p - png);
    return S_OK;
}

/* Into "<path>.part", then renamed: never a partial or empty file at path. */
static HRESULT file_write_whole(const WCHAR *path, const BYTE *data, size_t len)
{
    WCHAR tmp[MAX_PATH + 8];
    HANDLE fh;
    DWORD wr = 0, err = 0;
    swprintf(tmp, MAX_PATH + 8, L"%ls.part", path);
    fh = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) err = GetLastError();
    else {
        if (!WriteFile(fh, data, (DWORD)len, &wr, NULL)) err = GetLastError();
        else if (wr != len) err = ERROR_WRITE_FAULT;
        if (!CloseHandle(fh) && !err) err = GetLastError();
        if (!err && !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) err = GetLastError();
        if (err) DeleteFileW(tmp);
    }
    if (err) fprintf(stderr, "D3D8: screenshot: writing %ls failed (Windows error %lu)\n",
                     path, (unsigned long)err);
    return err ? HRESULT_FROM_WIN32(err) : S_OK;
}

/* Write scene_tex as a PNG (WIC, else a built-in writer). */
static HRESULT save_scene_png(D3D8DeviceState *s, ID3D11Texture2D *img, const WCHAR *path)
{
    unsigned char slut[256][3];
    int slut_on = dac_active();
    ID3D11Texture2D *stage = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    BYTE *bgr = NULL;
    UINT stride, x, y;
    HRESULT hr;

    if (!img) return E_FAIL;
    if (slut_on) dac_lut_copy(slut);
    ID3D11Texture2D_GetDesc(img, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(s->d3d11_device, &td, NULL, &stage);
    if (FAILED(hr)) return hr;
    ID3D11DeviceContext_CopyResource(s->d3d11_context, (ID3D11Resource *)stage,
                                     (ID3D11Resource *)img);
    hr = ID3D11DeviceContext_Map(s->d3d11_context, (ID3D11Resource *)stage, 0,
                                 D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) { ID3D11Texture2D_Release(stage); return hr; }
    stride = (td.Width * 3 + 3) & ~3u;
    bgr = (BYTE *)malloc((size_t)stride * td.Height);
    if (bgr) {
        for (y = 0; y < td.Height; y++) {
            const BYTE *src = (const BYTE *)m.pData + (size_t)y * m.RowPitch;
            BYTE *dst = bgr + (size_t)y * stride;
            for (x = 0; x < td.Width; x++) {
                dst[x * 3 + 0] = src[x * 4 + 2];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 0];
                if (slut_on) {                  /* the DAC palette */
                    dst[x * 3 + 0] = slut[dst[x * 3 + 0]][2];
                    dst[x * 3 + 1] = slut[dst[x * 3 + 1]][1];
                    dst[x * 3 + 2] = slut[dst[x * 3 + 2]][0];
                }
            }
        }
    }
    ID3D11DeviceContext_Unmap(s->d3d11_context, (ID3D11Resource *)stage, 0);
    ID3D11Texture2D_Release(stage);
    if (!bgr) return E_OUTOFMEMORY;
    {   /* fork: the image as shown, in its frame (16:9 box or 4:3 video) */
        unsigned bx0, bw;
        unsigned char *rb;
        if (present_box_cols(td.Width, &bx0, &bw) && (rb = (unsigned char *)malloc((size_t)td.Width * 3u))) {
            for (y = 0; y < td.Height; y++) {
                BYTE *row = bgr + (size_t)y * stride;
                present_box_row(row, rb, td.Width, bx0, bw);
                memcpy(row, rb, (size_t)td.Width * 3u);
            }
            free(rb);
        }
    }

    /* Encode in memory first, write the file only once that worked
     * (a WIC failure used to leave a 0-byte .png), and fall back to a plain
     * PNG writer when WIC cannot (0x88982F8A on a player's PC). */
    {
        const char *e = getenv("XBOX_SHOT_ENCODER");     /* wic | builtin, for tests */
        int want_wic = !(e && !strcmp(e, "builtin"));
        int want_own = !(e && !strcmp(e, "wic"));
        BYTE *png = NULL;
        size_t png_len = 0;
        const char *step = "";
        hr = E_FAIL;
        if (want_wic) {
            hr = png_encode_wic(bgr, td.Width, td.Height, stride, &png, &png_len, &step);
            if (FAILED(hr))
                fprintf(stderr, "D3D8: screenshot: WIC failed at %s: 0x%08lX%s\n", step,
                        (unsigned long)hr, want_own ? ", using the built-in PNG writer" : "");
        }
        if (FAILED(hr) && want_own)
            hr = png_encode_own(bgr, td.Width, td.Height, stride, &png, &png_len);
        if (SUCCEEDED(hr)) hr = file_write_whole(path, png, png_len);
        free(png);
    }
    free(bgr);
    return hr;
}

/* ── Screenshots of the image shown, and bursts ────────────
 *
 * F12 (hostui.c) saves the image the window shows: the swap chain's back
 * buffer, read right after the blit -- at the window's size, after the post
 * chain, the scaling into the window, the DAC palette and any black bars:
 * the very pixels handed to Present. Shift+F12 saves a burst: N consecutive presents
 * (XBOX_SHOT_BURST, 60 by default, 2 to 999) into a folder, to show what
 * moves (a flicker, a grain). The render thread never waits for it: each
 * image is copied into one of SHOWN_SLOTS staging textures, read back once
 * the GPU is done with it (a present or two later), and the PNG is encoded
 * and written by worker threads (XBOX_SHOT_THREADS, 4 by default; they start
 * once the burst is copied, or at half the memory allowed), with the
 * host's encoder (d3d8_SetPngEncoder; WIC fails in this process on some PCs,
 * And the built-in writer does not compress), else those. A text
 * file beside it says what was shown: hostui.c writes the game's context,
 * shown_finish adds the image's (sizes, present times, cost per present).
 * One capture at a time, from its request to its last file.
 * d3d8_RequestScreenshot (the tools' internal capture: the scene target at
 * the render size, the palette applied on the CPU) is unchanged;
 * XBOX_SHOT_VERIFY=1 also takes one at the present of the first image
 * ("<name> internal.png"), for comparisons. Nothing runs before the first
 * request but one timestamp per present (the intervals before a burst).
 */
#define SHOWN_SLOTS   4
#define SHOWN_LOG_MAX 4096
#define SHOWN_HIST    120

typedef struct ShownFrame {
    struct ShownFrame *next;
    unsigned index;
    UINT w, h;
    int bgra;                       /* bytes B,G,R,A (else R,G,B,A) */
    size_t bytes;
    BYTE *px;                       /* w * 4 bytes per row, top-down */
} ShownFrame;

typedef struct {
    double t;                       /* ms (QPC) at this point of the present */
    float cost;                     /* ms spent here at that present */
    int frame;                      /* image captured at it, or -1 */
    unsigned present;               /* s_frames */
    char info[48];                  /* the host's note (race tick): captured images */
} ShownLog;

static struct {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE work, room;
    volatile LONG ready;            /* 2 once the lock exists */
    /* the request (any thread -> the render thread) */
    int pending;
    WCHAR req_path[MAX_PATH], req_notes[MAX_PATH];
    unsigned req_n;
    /* the job, from its first image to its last file */
    volatile LONG active;
    int capturing;                  /* images still to copy: the workers wait (below) */
    WCHAR path[MAX_PATH], notes[MAX_PATH];   /* single: the .png; burst: the folder */
    unsigned n, captured, read;     /* render thread */
    unsigned n_asked, burst_mb;     /* n before the burst's memory limit, and the limit */
    unsigned written, failed;       /* under the lock */
    ShownFrame *head, *tail;        /* for the workers */
    size_t bytes, cap;              /* raw bytes held for the workers, and the limit */
    double enc_ms, enc_max, t_written;
    int wic_fail, threads, own_png;
    BYTE *pool[8];                  /* buffers given back by the workers, pool_bytes each */
    int pool_n;
    size_t pool_bytes;
} SH;

/* The host's PNG encoder (packed R,G,B rows; a malloc'd PNG), else the
 * built-in ones. */
static void *(*s_sh_png)(const unsigned char *rgb, unsigned w, unsigned h, size_t *len);

void d3d8_SetPngEncoder(void *(*fn)(const unsigned char *rgb, unsigned w, unsigned h, size_t *len))
{
    s_sh_png = fn;
}

/* Render thread only. */
static struct {
    ID3D11Texture2D *tex;
    UINT w, h;
    DXGI_FORMAT fmt;
    int busy;
    unsigned index;
} s_sh_slot[SHOWN_SLOTS];
static ShownLog s_sh_log[SHOWN_LOG_MAX];
static unsigned s_sh_log_n;
static double   s_sh_hist[SHOWN_HIST];   /* present times, for the intervals before a job */
static unsigned s_sh_hist_n;
static double   s_sh_before[SHOWN_HIST]; /* the job's: the last present times before it */
static unsigned s_sh_before_n;
static unsigned s_sh_gpu_waits, s_sh_mem_waits;
static double   s_sh_mem_wait_ms;
static void   (*s_sh_info_hook)(char *buf, size_t n);

static double shown_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int shown_env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    int v = e && e[0] ? atoi(e) : def;
    return v < lo ? lo : v > hi ? hi : v;
}

unsigned d3d8_ShotBurstCount(void)
{
    return (unsigned)shown_env_int("XBOX_SHOT_BURST", 60, 2, 999);
}

void d3d8_SetShotInfoHook(void (*fn)(char *buf, size_t n))
{
    s_sh_info_hook = fn;
}

/* Where image `index` of the job goes. SH.path and SH.n do not change while
 * the job runs. */
static void shown_frame_path(unsigned index, WCHAR *out)
{
    if (SH.n == 1) lstrcpynW(out, SH.path, MAX_PATH);
    else swprintf(out, MAX_PATH, L"%ls\\%03u.png", SH.path, index);
}

static DWORD WINAPI shown_worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        ShownFrame *f;
        WCHAR path[MAX_PATH];
        BYTE *bgr, *png = NULL;
        size_t png_len = 0;
        UINT stride, x, y;
        HRESULT hr = E_OUTOFMEMORY;
        const char *step = "";
        double t0, el;
        int wic_failed = 0, own = 0;

        /* Not while a burst is being copied, unless half the memory allowed is
         * used: encoding next to the game slows it (measured: 4 threads at
         * ~300 fps, presents 3.3 -> 5.3 ms), and the images would then show
         * a slower game than the one played. */
        EnterCriticalSection(&SH.lock);
        while (!SH.head || (SH.capturing && SH.bytes < SH.cap / 2))
            SleepConditionVariableCS(&SH.work, &SH.lock, INFINITE);
        f = SH.head;
        SH.head = f->next;
        if (!SH.head) SH.tail = NULL;
        shown_frame_path(f->index, path);
        LeaveCriticalSection(&SH.lock);

        t0 = shown_now_ms();
        {
            /* XBOX_SHOT_ENCODER (tests): host (default: the host's, else the
             * two below) | wic | builtin */
            const char *e = getenv("XBOX_SHOT_ENCODER");
            int want_host = s_sh_png && !(e && (!strcmp(e, "wic") || !strcmp(e, "builtin")));
            int want_wic = !want_host && !(e && !strcmp(e, "builtin"));
            /* the host's: packed R,G,B rows; WIC and the built-in writer: B,G,R rows, 4-byte aligned */
            const unsigned r = f->bgra ? 2u : 0u, b = f->bgra ? 0u : 2u;
            const unsigned o0 = want_host ? r : b, o2 = want_host ? b : r;
            stride = want_host ? f->w * 3u : (f->w * 3 + 3) & ~3u;
            bgr = (BYTE *)malloc((size_t)stride * f->h);
            if (bgr) {
                for (y = 0; y < f->h; y++) {
                    const BYTE *src = f->px + (size_t)y * f->w * 4u;
                    BYTE *dst = bgr + (size_t)y * stride;
                    for (x = 0; x < f->w; x++) {
                        dst[x * 3 + 0] = src[x * 4 + o0];
                        dst[x * 3 + 1] = src[x * 4 + 1];
                        dst[x * 3 + 2] = src[x * 4 + o2];
                    }
                }
                hr = E_FAIL;
                if (want_host) {
                    png = (BYTE *)s_sh_png(bgr, f->w, f->h, &png_len);
                    hr = png && png_len ? S_OK : E_FAIL;
                    if (FAILED(hr)) {           /* the built-in writer wants B,G,R */
                        for (y = 0; y < f->h; y++)
                            for (x = 0; x < f->w; x++) {
                                BYTE t = bgr[(size_t)y * stride + x * 3];
                                bgr[(size_t)y * stride + x * 3] = bgr[(size_t)y * stride + x * 3 + 2];
                                bgr[(size_t)y * stride + x * 3 + 2] = t;
                            }
                    }
                } else if (want_wic) {
                    hr = png_encode_wic(bgr, f->w, f->h, stride, &png, &png_len, &step);
                    if (FAILED(hr)) wic_failed = 1;
                }
                if (FAILED(hr)) {
                    free(png);
                    png = NULL;
                    hr = png_encode_own(bgr, f->w, f->h, stride, &png, &png_len);
                    own = 1;
                }
                if (SUCCEEDED(hr)) hr = file_write_whole(path, png, png_len);
                free(png);
                free(bgr);
            }
        }
        el = shown_now_ms() - t0;
        if (wic_failed)
            fprintf(stderr, "D3D8: screenshot: WIC failed at %s%s\n", step,
                    SUCCEEDED(hr) ? ", the built-in PNG writer was used" : "");
        if (FAILED(hr))
            fprintf(stderr, "D3D8: screenshot %ls: 0x%08lX\n", path, (unsigned long)hr);

        EnterCriticalSection(&SH.lock);
        SH.bytes -= f->bytes;
        if (SUCCEEDED(hr)) SH.written++; else SH.failed++;
        SH.enc_ms += el;
        if (el > SH.enc_max) SH.enc_max = el;
        SH.wic_fail += wic_failed;
        SH.own_png += own;
        SH.t_written = shown_now_ms();
        if (SH.pool_n < 8 && (!SH.pool_n || SH.pool_bytes == f->bytes)) {
            SH.pool_bytes = f->bytes;   /* kept for the next image: no new pages to touch */
            SH.pool[SH.pool_n++] = f->px;
            f->px = NULL;
        }
        WakeAllConditionVariable(&SH.room);
        LeaveCriticalSection(&SH.lock);
        free(f->px);
        free(f);
    }
    return 0;                           /* not reached */
}

static void shown_init(void)
{
    static volatile LONG once;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        InitializeCriticalSection(&SH.lock);
        InitializeConditionVariable(&SH.work);
        InitializeConditionVariable(&SH.room);
        InterlockedExchange(&SH.ready, 2);
    } else {
        while (SH.ready != 2) Sleep(0);
    }
}

/* Raw bytes the workers may hold: a quarter of the free memory, 256 MB to
 * 4 GB (XBOX_SHOT_MEM_MB). Beyond it the render thread waits for them. */
static size_t shown_mem_cap(void)
{
    MEMORYSTATUSEX ms;
    unsigned long long c = 1024ull << 20;
    int mb = shown_env_int("XBOX_SHOT_MEM_MB", 0, 0, 65536);
    if (mb) return (size_t)mb << 20;
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) c = ms.ullAvailPhys / 4u;
    if (c < (256ull << 20)) c = 256ull << 20;
    if (c > (4096ull << 20)) c = 4096ull << 20;
    return (size_t)c;
}

/* Ask for n images of the window from the next present: path is the .png
 * (n = 1) or an existing folder (n > 1, images 000.png, 001.png, ...); a
 * description is appended to notes (may be NULL) once every file is written.
 * Returns 0 when a capture is still running (nothing asked). Any thread. */
int d3d8_RequestShownShot(const wchar_t *path, unsigned n, const wchar_t *notes)
{
    int ok = 0;
    if (!path || !path[0]) return 0;
    shown_init();
    EnterCriticalSection(&SH.lock);
    if (!SH.pending && !SH.active) {
        lstrcpynW(SH.req_path, path, MAX_PATH);
        lstrcpynW(SH.req_notes, notes ? notes : L"", MAX_PATH);
        SH.req_n = n < 1 ? 1 : n > 999 ? 999 : n;
        SH.pending = 1;
        ok = 1;
        if (!SH.threads) {
            int i, want = shown_env_int("XBOX_SHOT_THREADS", 4, 1, 16);
            for (i = 0; i < want; i++) {
                HANDLE t = CreateThread(NULL, 0, shown_worker, NULL, 0, NULL);
                if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); SH.threads++; }
            }
            if (!SH.threads) { SH.pending = 0; ok = 0; }
        }
    }
    LeaveCriticalSection(&SH.lock);
    return ok;
}

/* 1 while a capture asked with d3d8_RequestShownShot is not finished. */
int d3d8_ShownShotBusy(void)
{
    return SH.ready == 2 && (SH.pending || SH.active);
}

/* Render thread, at each present before anything is read: a request becomes
 * the job, its first image is this present's. */
static void shown_shots_begin(void)
{
    WCHAR vp[MAX_PATH];
    if (SH.ready != 2 || SH.active) return;
    EnterCriticalSection(&SH.lock);
    if (SH.pending) {
        SH.pending = 0;
        lstrcpynW(SH.path, SH.req_path, MAX_PATH);
        lstrcpynW(SH.notes, SH.req_notes, MAX_PATH);
        SH.n = SH.n_asked = SH.req_n;
        /* A burst holds its raw images until the last one is copied: at most
         * XBOX_SHOT_BURST_MB (1536) of them, fewer images if the window is
         * large (60 at 3440x1440 = 1.2 GB). Consecutive images, never spread. */
        SH.burst_mb = (unsigned)shown_env_int("XBOX_SHOT_BURST_MB", 1536, 64, 65536);
        if (SH.n > 1 && g_device_state.swap_w && g_device_state.swap_h) {
            unsigned long long one = (unsigned long long)g_device_state.swap_w * g_device_state.swap_h * 4u;
            unsigned long long fit = ((unsigned long long)SH.burst_mb << 20) / one;
            if (fit < 2) fit = 2;
            if (fit < SH.n) SH.n = (unsigned)fit;
        }
        SH.captured = SH.read = SH.written = SH.failed = 0;
        SH.enc_ms = SH.enc_max = SH.t_written = 0.0;
        SH.wic_fail = SH.own_png = 0;
        SH.capturing = 1;
        if (!SH.cap) SH.cap = shown_mem_cap();
        InterlockedExchange(&SH.active, 1);
    }
    LeaveCriticalSection(&SH.lock);
    if (!SH.active) return;
    s_sh_log_n = 0;
    s_sh_before_n = s_sh_hist_n < SHOWN_HIST ? s_sh_hist_n : SHOWN_HIST;
    {
        unsigned i;
        for (i = 0; i < s_sh_before_n; i++)
            s_sh_before[i] = s_sh_hist[(s_sh_hist_n - s_sh_before_n + i) % SHOWN_HIST];
    }
    s_sh_gpu_waits = s_sh_mem_waits = 0;
    s_sh_mem_wait_ms = 0.0;
    if (SH.n > 1) d3d8_HostToast(L"Burst: capturing");
    {   /* XBOX_SHOT_VERIFY=1: the internal capture of the same present */
        const char *e = NULL;
        if (e && e[0] == '1') {
            size_t k;
            if (SH.n > 1) swprintf(vp, MAX_PATH, L"%ls\\000 internal.png", SH.path);
            else {
                lstrcpynW(vp, SH.path, MAX_PATH);
                k = wcslen(vp);
                if (k > 4 && !_wcsicmp(vp + k - 4, L".png")) vp[k - 4] = 0;
                wcsncat(vp, L" internal.png", MAX_PATH - wcslen(vp) - 1);
            }
            d3d8_RequestScreenshot(vp);
        }
    }
}

/* Copy the staging images the GPU is done with into memory for the workers,
 * in order; wait = 1 waits for the oldest. */
static void shown_readback(D3D8DeviceState *s, int wait)
{
    ID3D11DeviceContext *ctx = s->d3d11_context;
    while (SH.read < SH.captured) {
        unsigned k = SH.read % SHOWN_SLOTS, y;
        D3D11_MAPPED_SUBRESOURCE m;
        ShownFrame *f = NULL;
        BYTE *px;
        size_t bytes, row;
        HRESULT hr;
        if (!s_sh_slot[k].busy || s_sh_slot[k].index != SH.read) {      /* its copy failed */
            EnterCriticalSection(&SH.lock);
            SH.failed++;
            LeaveCriticalSection(&SH.lock);
            SH.read++;
            continue;
        }
        row = (size_t)s_sh_slot[k].w * 4u;
        bytes = row * s_sh_slot[k].h;
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_sh_slot[k].tex, 0, D3D11_MAP_READ,
                                     wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        wait = 0;                       /* one blocking map at most */
        if (SUCCEEDED(hr)) {
            EnterCriticalSection(&SH.lock);
            if (SH.bytes && SH.bytes + bytes > SH.cap) {
                double w0 = shown_now_ms();
                s_sh_mem_waits++;
                WakeAllConditionVariable(&SH.work);
                while (SH.bytes && SH.bytes + bytes > SH.cap)
                    SleepConditionVariableCS(&SH.room, &SH.lock, INFINITE);
                s_sh_mem_wait_ms += shown_now_ms() - w0;
            }
            SH.bytes += bytes;
            px = SH.pool_n && SH.pool_bytes == bytes ? SH.pool[--SH.pool_n] : NULL;
            LeaveCriticalSection(&SH.lock);
            f = (ShownFrame *)calloc(1, sizeof *f);
            if (f) f->px = px ? px : (BYTE *)malloc(bytes);
            else free(px);
            if (f && f->px) {
                for (y = 0; y < s_sh_slot[k].h; y++)
                    memcpy(f->px + row * y, (const BYTE *)m.pData + (size_t)m.RowPitch * y, row);
                f->index = SH.read;
                f->w = s_sh_slot[k].w;
                f->h = s_sh_slot[k].h;
                f->bgra = s_sh_slot[k].fmt == DXGI_FORMAT_B8G8R8A8_UNORM ||
                          s_sh_slot[k].fmt == DXGI_FORMAT_B8G8R8X8_UNORM;
                f->bytes = bytes;
            } else if (f) {
                free(f);
                f = NULL;
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_sh_slot[k].tex, 0);
        }
        s_sh_slot[k].busy = 0;
        EnterCriticalSection(&SH.lock);
        if (f) {
            if (SH.tail) SH.tail->next = f; else SH.head = f;
            SH.tail = f;
            WakeAllConditionVariable(&SH.work);
        } else {
            if (SUCCEEDED(hr)) SH.bytes -= bytes;
            SH.failed++;
        }
        LeaveCriticalSection(&SH.lock);
        SH.read++;
    }
}

static int shown_cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* "median / p95 / max" of n values (sorted in place). */
static void shown_stats(double *v, unsigned n, char *out, size_t sz)
{
    if (!n) { snprintf(out, sz, "-"); return; }
    qsort(v, n, sizeof *v, shown_cmp_d);
    snprintf(out, sz, "%.2f / %.2f / %.2f", v[n / 2], v[(n * 95u) / 100u < n ? (n * 95u) / 100u : n - 1],
             v[n - 1]);
}

/* Every file written: the description, the toast, the slots let go. */
static void shown_finish(D3D8DeviceState *s)
{
    static double v[SHOWN_LOG_MAX + SHOWN_HIST];
    unsigned i, nv, first = 0, last = 0, have = 0, consecutive = 1, nwrite = 0;
    double cost_sum = 0.0, cost_max = 0.0, t_first = 0.0, prev;
    unsigned written, failed;
    char a[64], b[64], c[64];
    WCHAR toast[96];
    FILE *f = NULL;

    EnterCriticalSection(&SH.lock);
    written = SH.written;
    failed = SH.failed;
    LeaveCriticalSection(&SH.lock);
    for (i = 0; i < s_sh_log_n; i++) {
        if (s_sh_log[i].frame < 0) continue;
        if (!have) { first = i; t_first = s_sh_log[i].t; }
        else if (s_sh_log[i].present != s_sh_log[last].present + 1u) consecutive = 0;
        last = i;
        have = 1;
    }
    if (have && last + 1 - first != SH.n) consecutive = 0;
    for (i = 0; i < s_sh_log_n; i++) {
        cost_sum += s_sh_log[i].cost;
        if (s_sh_log[i].cost > cost_max) cost_max = s_sh_log[i].cost;
    }
    if (SH.notes[0]) f = _wfopen(SH.notes, L"ab");
    if (f) {
        fprintf(f, "\n[image]\n");
        fprintf(f, "what = the image shown in the window: the swap chain's back buffer after the blit "
                   "(post effects, scaling into the window, DAC palette, black bars)\n");
        fprintf(f, "shown = %ux%u (window client area)\n", s->swap_w, s->swap_h);
        fprintf(f, "render = %ux%u (scene target)", s->width, s->height);
        if (s_rs_pct != 100) fprintf(f, ", 3D at %d %% (%ux%u)", s_rs_pct, s->rs_w, s->rs_h);
        fprintf(f, "\nscaled into the window = %s\n",
                s->swap_w == s->width && s->swap_h == s->height ? "no (1:1)" : "yes (bilinear)");
        fprintf(f, "dac palette = %s; post chain = %s; fullscreen = %s\n",
                dac_active() ? "applied" : "none", d3d8_post_enabled() ? "on" : "off",
                s_fullscreen ? "yes" : "no");
        fprintf(f, "images = %u asked, %u captured, %u written, %u failed\n",
                SH.n, SH.captured, written, failed);
        if (SH.n < SH.n_asked)
            fprintf(f, "burst limited = %u images asked, %u kept: %ux%u images, memory limit %u MB "
                       "(XBOX_SHOT_BURST_MB)\n", SH.n_asked, SH.n, s->swap_w, s->swap_h, SH.burst_mb);
        if (have)
            fprintf(f, "presents = #%u to #%u: %s\n", s_sh_log[first].present, s_sh_log[last].present,
                    consecutive ? "one image per present, none skipped" : "NOT consecutive (see the table)");
        fprintf(f, "render thread, per present = mean %.3f ms, max %.3f ms (copy, read back, notes); "
                   "GPU waits %u; memory waits %u (%.0f ms)\n",
                s_sh_log_n ? cost_sum / s_sh_log_n : 0.0, cost_max, s_sh_gpu_waits, s_sh_mem_waits,
                s_sh_mem_wait_ms);
        fprintf(f, "encoding = %d thread(s), mean %.1f ms, max %.1f ms per image%s; last file %.0f ms "
                   "after the first image\n",
                SH.threads, (written + failed) ? SH.enc_ms / (written + failed) : 0.0, SH.enc_max,
                SH.own_png ? " (built-in PNG writer, uncompressed)" : s_sh_png ? " (host PNG writer)" : " (WIC PNG)",
                have && SH.t_written > t_first ? SH.t_written - t_first : 0.0);
        /* present intervals: before the job, during the capture, while writing */
        nv = 0;
        for (i = 1; i < s_sh_before_n; i++) v[nv++] = s_sh_before[i] - s_sh_before[i - 1];
        shown_stats(v, nv, a, sizeof a);
        fprintf(f, "present intervals, median / p95 / max ms = before %s (%u)", a, nv);
        nv = 0;
        for (i = first + 1; have && i <= last; i++) v[nv++] = s_sh_log[i].t - s_sh_log[i - 1].t;
        shown_stats(v, nv, b, sizeof b);
        fprintf(f, "; during the capture %s (%u)", b, nv);
        nv = 0;
        for (i = last + 1; have && i < s_sh_log_n; i++) v[nv++] = s_sh_log[i].t - s_sh_log[i - 1].t;
        nwrite = nv;
        shown_stats(v, nv, c, sizeof c);
        fprintf(f, "; while the files were written %s (%u)\n", c, nwrite);
        fprintf(f, "\nimage  present  t_ms      interval_ms  here_ms  game\n");
        prev = s_sh_before_n ? s_sh_before[s_sh_before_n - 1] : 0.0;
        for (i = 0; i < s_sh_log_n; i++) {
            const ShownLog *L = &s_sh_log[i];
            if (L->frame >= 0)
                fprintf(f, "%03d    #%-7u %8.2f  %11.2f  %7.3f  %s\n", L->frame, L->present,
                        L->t - t_first, prev ? L->t - prev : 0.0, L->cost, L->info);
            prev = L->t;
        }
        fclose(f);
    }
    if (SH.n == 1) lstrcpynW(toast, written ? L"Screenshot saved" : L"Screenshot failed", 96);
    else if (written == SH.n) swprintf(toast, 96, L"Burst saved: %u images", written);
    else swprintf(toast, 96, L"Burst: %u of %u images saved", written, SH.n);
    d3d8_HostToast(toast);
    fprintf(stderr, "[SHOT] %ls: %u of %u image(s) written, %ux%u, presents %s, render thread max %.3f ms, "
            "GPU waits %u, memory waits %u\n", SH.path, written, SH.n, s->swap_w, s->swap_h,
            consecutive ? "consecutive" : "NOT consecutive", cost_max, s_sh_gpu_waits, s_sh_mem_waits);
    fflush(stderr);
    EnterCriticalSection(&SH.lock);
    while (SH.pool_n) free(SH.pool[--SH.pool_n]);
    LeaveCriticalSection(&SH.lock);
    for (i = 0; i < SHOWN_SLOTS; i++)
        if (s_sh_slot[i].tex) { ID3D11Texture2D_Release(s_sh_slot[i].tex); memset(&s_sh_slot[i], 0, sizeof s_sh_slot[i]); }
    InterlockedExchange(&SH.active, 0);
}

/* Render thread, at each present right after the blit into the back buffer:
 * this image, then what the GPU has finished. */
static void shown_shots_present(D3D8DeviceState *s)
{
    double t0 = shown_now_ms();
    int frame = -1;
    ShownLog *L;
    s_sh_hist[s_sh_hist_n++ % SHOWN_HIST] = t0;
    if (!SH.active) return;
    if (SH.captured < SH.n) {
        unsigned k = SH.captured % SHOWN_SLOTS;
        D3D11_TEXTURE2D_DESC td;
        if (s_sh_slot[k].busy) { s_sh_gpu_waits++; shown_readback(s, 1); }
        ID3D11Texture2D_GetDesc(s->swap_tex, &td);
        if (s_sh_slot[k].tex && (s_sh_slot[k].w != td.Width || s_sh_slot[k].h != td.Height ||
                                 s_sh_slot[k].fmt != td.Format)) {
            ID3D11Texture2D_Release(s_sh_slot[k].tex);
            s_sh_slot[k].tex = NULL;
        }
        if (!s_sh_slot[k].tex) {
            td.MipLevels = 1; td.ArraySize = 1;
            td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
            td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(s->d3d11_device, &td, NULL, &s_sh_slot[k].tex))) {
                s_sh_slot[k].w = td.Width;
                s_sh_slot[k].h = td.Height;
                s_sh_slot[k].fmt = td.Format;
            } else {
                s_sh_slot[k].tex = NULL;
            }
        }
        if (s_sh_slot[k].tex) {
            ID3D11DeviceContext_CopyResource(s->d3d11_context, (ID3D11Resource *)s_sh_slot[k].tex,
                                             (ID3D11Resource *)s->swap_tex);
            s_sh_slot[k].busy = 1;
            s_sh_slot[k].index = SH.captured;
        }
        frame = (int)SH.captured++;
        if (SH.captured == SH.n) {      /* the last one: the workers may start */
            EnterCriticalSection(&SH.lock);
            SH.capturing = 0;
            WakeAllConditionVariable(&SH.work);
            LeaveCriticalSection(&SH.lock);
        }
    }
    shown_readback(s, 0);
    if (s_sh_log_n < SHOWN_LOG_MAX) {
        L = &s_sh_log[s_sh_log_n++];
        L->t = t0;
        L->frame = frame;
        L->present = (unsigned)s_frames;
        L->info[0] = 0;
        if (frame >= 0 && s_sh_info_hook) s_sh_info_hook(L->info, sizeof L->info);
        L->cost = (float)(shown_now_ms() - t0);
    }
    if (SH.captured == SH.n && SH.read == SH.n) {
        int done;
        EnterCriticalSection(&SH.lock);
        done = SH.written + SH.failed == SH.n;
        LeaveCriticalSection(&SH.lock);
        if (done) shown_finish(s);
    }
}

/* Show the finished scene: fit it, at its display aspect, into the window --
 * centred, with black bars where the window's shape differs -- then present.
 * A 1:1 fit is a plain copy, which is every automated run. Settings changed
 * from the window's menu take effect here, between frames. */
/* The last finished frame, for titles that sample the previous frame buffer as
 * a texture: SSX's chrome "Master" outfits reflect it, binding the buffer that
 * was just shown as a linear texture. Frames only exist on the host,
 * so guest RAM there is black. Copied from the scene target at each present,
 * once something has asked for it (d3d8_PrevFrameTexture); the copy is an
 * RGBA texture of the scene's own size wrapped in a D3D8 texture object, and
 * the NV2A path normalises linear-texture coordinates by the image rect, so
 * the render resolution does not matter. */
static IDirect3DTexture8 *g_prev_frame;
static unsigned           g_prev_frame_wanted;   /* present seq of the last request */

static void prev_frame_update(D3D8DeviceState *s)
{
    D3D11_TEXTURE2D_DESC sd;
    D3D8Texture *t;
    if (!s->scene_tex || !g_prev_frame_wanted || g_present_seq - g_prev_frame_wanted > 120)
        return;
    ID3D11Texture2D_GetDesc(s->scene_tex, &sd);
    t = (D3D8Texture *)g_prev_frame;
    if (t && (t->width != sd.Width || t->height != sd.Height)) {
        g_prev_frame->lpVtbl->Release(g_prev_frame);
        g_prev_frame = NULL;
        t = NULL;
    }
    if (!t) {
        D3D11_TEXTURE2D_DESC td = sd;
        ID3D11Texture2D *tex = NULL;
        ID3D11ShaderResourceView *srv = NULL;
        IDirect3DTexture8 *wrap = NULL;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s->d3d11_device, &td, NULL, &tex)))
            return;
        if (FAILED(ID3D11Device_CreateShaderResourceView(s->d3d11_device,
                        (ID3D11Resource *)tex, NULL, &srv)) ||
            FAILED(d3d8_CreateTextureImpl(sd.Width, sd.Height, 1, 0, D3DFMT_A8R8G8B8, &wrap))) {
            if (srv) ID3D11ShaderResourceView_Release(srv);
            ID3D11Texture2D_Release(tex);
            return;
        }
        t = (D3D8Texture *)wrap;
        if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
        if (t->d3d11_texture) ID3D11Texture2D_Release(t->d3d11_texture);
        t->d3d11_texture = tex;
        t->srv = srv;
        t->dxgi_format = td.Format;
        g_prev_frame = wrap;
    }
    ID3D11DeviceContext_CopyResource(s->d3d11_context,
        (ID3D11Resource *)t->d3d11_texture, (ID3D11Resource *)s->scene_tex);
}

IDirect3DTexture8 *d3d8_PrevFrameTexture(void)
{
    g_prev_frame_wanted = g_present_seq ? g_present_seq : 1;
    return g_prev_frame;
}

/* XBOX_PERF=1: duration of host_present, of the DXGI Present,
 * and GPU time between two Presents (timestamp queries, read back 3 frames
 * later without waiting). The GPU time runs from the return of the previous
 * Present to the start of this one: it includes any gaps when the CPU does
 * not feed the GPU, so it is an upper bound of the frame's GPU work. */
static HRESULT host_present_impl(void);
static double s_perf_dxgi_ms;
static HRESULT host_present(void)
{
    enum { NQ = 4 };
    static ID3D11Query *qd[NQ], *qa[NQ], *qb[NQ];
    static int cur = -1, ok = -1;
    static unsigned long long n;
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context;
    ID3D11Device *dev = g_device_state.d3d11_device;
    double t0, gpu = -1.0;
    HRESULT hr;
    {   /* frame counter for XBOX_FIX_PUMP_ALT */
        extern void d3d8_pump_frame_tick(void);
        d3d8_pump_frame_tick();
    }
    if (!g_perf_on) return host_present_impl();
    if (ok < 0 && dev) {
        D3D11_QUERY_DESC qdesc;
        int i;
        ok = 1;
        memset(&qdesc, 0, sizeof qdesc);
        for (i = 0; i < NQ; i++) {
            qdesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            if (FAILED(ID3D11Device_CreateQuery(dev, &qdesc, &qd[i]))) ok = 0;
            qdesc.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(ID3D11Device_CreateQuery(dev, &qdesc, &qa[i]))) ok = 0;
            if (FAILED(ID3D11Device_CreateQuery(dev, &qdesc, &qb[i]))) ok = 0;
        }
    }
    if (ok == 1 && ctx && cur >= 0) {
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)qb[cur]);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)qd[cur]);
    }
    t0 = perf_now();
    s_perf_dxgi_ms = 0.0;
    hr = host_present_impl();
    perf_add(PZ_PRESENT, perf_now() - t0);
    perf_add(PZ_DXGI, s_perf_dxgi_ms);
    if (ok == 1 && ctx) {
        int old = (int)((n + 1) % NQ);          /* the oldest, 3 frames back */
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a, b;
        if (n >= NQ
            && ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)qd[old], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)qa[old], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)qb[old], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && !dj.Disjoint && dj.Frequency)
            gpu = (double)(b - a) * 1000.0 / (double)dj.Frequency;
        n++;
        cur = (int)(n % NQ);
        ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)qd[cur]);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)qa[cur]);
    }
    perf_present_done(gpu);
    return hr;
}

static HRESULT host_present_impl(void)
{
    D3D8DeviceState *s = &g_device_state;
    ID3D11DeviceContext *ctx = s->d3d11_context;
    HRESULT hr;

    if (!s->swap_chain) return E_FAIL;
    if (!s_video_frame) lights_apply(s);    /* fork: an image without HUD phase */
    /* fork: render scale -- the 3D at the render size before the image is
     * read, unless a video frame replaced it */
    if (s_rs_bound) rs_to_screen(s, 0, !s_video_frame);
    if (ctx && s->scene_tex) {
        ID3D11Texture2D *img;
        ID3D11ShaderResourceView *img_srv;
        if (g_gpuprof_on) gpuprof_begin(GP_RESOLVE);
        scene_resolve(s);
        if (g_gpuprof_on) { gpuprof_end(); gpuprof_begin(GP_PREVFRAME); }
        prev_frame_update(s);           /* before the post: the chrome reads the title's frame */
        if (g_gpuprof_on) gpuprof_end();
        present_source(&img, &img_srv); /* scene_tex, or the post chain's output */
        if (s_present_image_overlay) {   /* fork: an optional overlay over the shown image (d3d8_SetPresentImageOverlay) */
            ID3D11Texture2D *ov;
            ID3D11ShaderResourceView *ov_srv;
            if (s_present_image_overlay(img, &ov, &ov_srv)) { img = ov; img_srv = ov_srv; }
        }

        shown_shots_begin();            /* F12 / Shift+F12 (before the internal one) */
        /* A screenshot, if one was asked for. */
        if (s_shot_lock_ready == 2) {
            WCHAR path[MAX_PATH];
            EnterCriticalSection(&s_shot_lock);
            lstrcpynW(path, s_shot_path, MAX_PATH);
            s_shot_path[0] = 0;
            LeaveCriticalSection(&s_shot_lock);
            if (path[0]) {
                HRESULT sh = save_scene_png(s, img, path);
                d3d8_HostToast(SUCCEEDED(sh) ? L"Screenshot saved" : L"Screenshot failed");
                if (d3d8_veil_wanted())     /* fork: which side of an A/B pair */
                    fprintf(stderr, "D3D8: screenshot %ls: 0x%08lX (veil %s)\n", path,
                            (unsigned long)sh, s_veil_last ? "on" : "off");
                else
                    fprintf(stderr, "D3D8: screenshot %ls: 0x%08lX\n", path, (unsigned long)sh);
            }
        }

        host_output_resize();
        if (s->swap_rtv && s->swap_tex && s->swap_w && s->swap_h) {
            const double aspect = present_aspect();
            UINT w = s->swap_w, h = s->swap_h, rw, rh, rx, ry;
            if ((double)w / (double)h > aspect) {
                rh = h; rw = (UINT)(h * aspect + 0.5);
            } else {
                rw = w; rh = (UINT)(w / aspect + 0.5);
            }
            if (rw > w) rw = w;
            if (rh > h) rh = h;
            rx = (w - rw) / 2;
            ry = (h - rh) / 2;
            if (rw != w || rh != h) {
                static const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                ID3D11DeviceContext_ClearRenderTargetView(ctx, s->swap_rtv, black);
            }
            ID3D11ShaderResourceView *lut = dac_lut_srv();
            if (g_gpuprof_on) gpuprof_begin(GP_BLIT);
            if (rw == s->width && rh == s->height && !lut) {
                ID3D11DeviceContext_CopySubresourceRegion(ctx,
                    (ID3D11Resource *)s->swap_tex, 0, rx, ry, 0,
                    (ID3D11Resource *)img, 0, NULL);
            } else {
                D3D11_VIEWPORT vp;
                vp.TopLeftX = (FLOAT)rx; vp.TopLeftY = (FLOAT)ry;
                vp.Width = (FLOAT)rw;    vp.Height = (FLOAT)rh;
                vp.MinDepth = 0.0f;      vp.MaxDepth = 1.0f;
                blit_draw_lut(img_srv, s->swap_rtv, &vp, lut);
            }
            if (g_gpuprof_on) gpuprof_end();
            shown_shots_present(s);     /* the image shown, as presented */
        }
    }
    if (g_gpuprof_on) gpuprof_frame();
    {
        /* Sync interval. The title paces itself to its own 60 Hz
         * frame timer and vertical blank; waiting for the monitor's refresh
         * as well blocked the render thread whenever two of the title's
         * flips fell inside one refresh -- the thread the title's fences
         * wait on -- and cost frames. The window is composed by the desktop,
         * so presenting without waiting does not tear. XBOX_VSYNC=1 waits. */
        /* XBOX_SYNC chooses the presentation (d3d8_sync.h); its
         * default, legacy, is the above unchanged. */
        static int plog = -1;
        double dxgi_ms = 0.0;
        LARGE_INTEGER p0, p1, pf;
        if (plog < 0) { const char *e = getenv("XBOX_FLIP_LOG"); plog = e && e[0] == '1'; }
        if (s_video_frame) video_present_mark();   /* sent with the frame by its Present */
        if (plog) QueryPerformanceCounter(&p0);
        d3d8_sync_video_frame(s_video_frame);   /* a video frame keeps the file's pace */
        hr = d3d8_sync_present(s->swap_chain, &dxgi_ms);
        if (s_video_frame && d3d8_sync_left_out()) g_video_left_out++;
        {   /* one line: the time from the start to the first image shown */
            static int told;
            if (!told) {
                told = 1;
                fprintf(stderr, "[TIME] first present at t+%.0f ms\n", d3d8_ms_since_start());
            }
        }
        if (g_perf_on) s_perf_dxgi_ms = dxgi_ms;
        if (plog) {     /* XBOX_FLIP_LOG=1: how long Present itself blocks */
            QueryPerformanceCounter(&p1);
            QueryPerformanceFrequency(&pf);
            fprintf(stderr, "[PRES] t=%.1f present %.2f ms%s\n",
                    (double)p0.QuadPart * 1000.0 / (double)pf.QuadPart,
                    (double)(p1.QuadPart - p0.QuadPart) * 1000.0 / (double)pf.QuadPart,
                    d3d8_sync_left_out() ? " left out (60 Hz display: two on one refresh)" : "");
        }
    }
    InterlockedIncrement(&s_frames);
    if (d3d8_post_split_wanted()) {     /* posts per presented image */
        s_split_stats.presents++;
        if (s_video_frame) s_split_stats.video++;
        else if (s_split_done) s_split_stats.split++;
        else if (d3d8_post_enabled()) s_split_stats.fallback++;
        else s_split_stats.none++;
        if (s_split_stats.presents % 600 == 0 && xbox_log_stats())
            fprintf(stderr, "[POST] split: %llu presents -- %llu posted at FRAME_END, "
                    "%llu fallback at present, %llu without post, %llu video; "
                    "%llu second FRAME_END skipped\n",
                    s_split_stats.presents, s_split_stats.split, s_split_stats.fallback,
                    s_split_stats.none, s_split_stats.video, s_split_stats.doubles);
        s_cycle_mode = d3d8_post_cycle(s_frames_cycle_base() + 1u);
    }
    s_split_done = 0;
    s_post_frame++;                     /* the next present is a new frame */
    s_video_frame = 0;
    {   /* XBOX_FPS_LOG=1: frames per second every 2 s, for measuring */
        static int on = -1;
        static DWORD t0 = 0;
        static LONG f0 = 0;
        if (on < 0) { const char *e = getenv("XBOX_FPS_LOG"); on = e && e[0] == '1'; }
        if (on) {
            DWORD now = GetTickCount();
            {   /* histogram of present intervals: <12, 12-20, 20-28, 28-40, >40 ms */
                static LARGE_INTEGER qf, qlast;
                static unsigned hist[5];
                LARGE_INTEGER qn;
                double ms;
                if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
                QueryPerformanceCounter(&qn);
                if (qlast.QuadPart) {
                    ms = (double)(qn.QuadPart - qlast.QuadPart) * 1000.0 / (double)qf.QuadPart;
                    hist[ms < 12 ? 0 : ms < 20 ? 1 : ms < 28 ? 2 : ms < 40 ? 3 : 4]++;
                    if (ms >= 40) {
                        /* XBOX_PROFILE=...,stall attributes samples in this window */
                        extern void xbox_profile_note_stall(LONGLONG, LONGLONG);
                        xbox_profile_note_stall(qlast.QuadPart, qn.QuadPart);
                        if (getenv("XBOX_WAIT_LOG"))
                            fprintf(stderr, "[STALL] t=%.1f..%.1f %.1f ms\n",
                                    (double)qlast.QuadPart * 1000.0 / (double)qf.QuadPart,
                                    (double)qn.QuadPart * 1000.0 / (double)qf.QuadPart, ms);
                    }
                }
                qlast = qn;
                if (t0 && now - t0 >= 2000) {
                    extern volatile long g_nv_compiles;
                    extern volatile double g_nv_compile_ms;
                    fprintf(stderr, "[FPS] intervals <12:%u 12-20:%u 20-28:%u 28-40:%u >40:%u; "
                            "%ld shader compiles, %.0f ms; %ld texture uploads, %.0f ms\n",
                            hist[0], hist[1], hist[2], hist[3], hist[4],
                            g_nv_compiles, g_nv_compile_ms, g_tex_uploads_log, g_tex_upload_ms);
                    g_nv_compiles = 0;
                    g_nv_compile_ms = 0;
                    {   /* shaders read from the disk cache (none with XBOX_SHADER_CACHE=0) */
                        extern volatile long g_nv_shc_hits;
                        if (g_nv_shc_hits) fprintf(stderr, "[SHC] %ld shaders from the cache\n", g_nv_shc_hits);
                        g_nv_shc_hits = 0;
                    }
                    g_tex_uploads_log = 0;
                    g_tex_upload_ms = 0;
                    memset(hist, 0, sizeof hist);
                }
            }
            if (!t0) { t0 = now; f0 = s_frames; }
            else if (now - t0 >= 2000) {
                fprintf(stderr, "[FPS] %.1f at %.0f s\n", (s_frames - f0) * 1000.0 / (now - t0),
                        now / 1000.0 - s_start_tick / 1000.0);
                t0 = now; f0 = s_frames;
            }
        }
    }

    /* Anti-aliasing changed from the menu: rebuild the scene targets now,
     * between frames. The next frame starts with the title's own clear. */
    if (ctx && s->scene_tex && (UINT)s_want_msaa != s->msaa_requested) {
        UINT want = (UINT)s_want_msaa;
        s->msaa_requested = want;
        scene_targets_release(s);
        if (FAILED(scene_targets_create(s, want))) {
            scene_targets_release(s);
            scene_targets_create(s, 1);
        }
        d3d8_post_release();            /* the post targets follow the scene targets */
        if (s_rs_pct != 100) rs_targets_create(s);     /* fork: and the render scale's */
        fprintf(stderr, "D3D8: anti-aliasing now %ux\n", s->msaa);
    }
    {   /* fork: render scale changed live (diagnostics port `set rscale`) */
        static int rs_gen;
        int g = d3d8_rscale_generation();
        if (ctx && s->scene_tex && g != rs_gen) {
            rs_gen = g;
            rs_finish(s);
            rs_choose(s);
            rs_targets_create(s);
        }
    }
    rs_next_image(s);                   /* fork: render scale, the next image's targets */
    /* fork: the next image jittered when this one carried markers (a race);
     * its 3D is drawn into the target bound now (d3d8_temporal.h) */
    d3d8_temporal_next_image(s_tmp_marked, cur_w(s), cur_h(s), s_rs_pct);
    s_tmp_marked = 0;
    return hr;
}

static HRESULT d3d11_create_device_and_swap_chain(
    D3D8DeviceState *state,
    D3DPRESENT_PARAMETERS *pp)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    UINT create_flags = 0;
    HRESULT hr;

#ifdef _DEBUG
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    /* XBOX_D3D_DEBUG=1 turns the validation layer on in a release build.
     * Every stage of the draw audits clean -- correct geometry, states,
     * viewport, RTV, shaders, input layout -- and the back buffer is still
     * black, including for a hardcoded quad. At that point the runtime's own
     * diagnosis is worth more than further inspection. */
    {
        const char *e = getenv("XBOX_D3D_DEBUG");
        if (e && e[0] == '1')
            create_flags |= D3D11_CREATE_DEVICE_DEBUG;
    }

    /* The title's own backbuffer size, and the scene target it is rendered
     * into: the configured render size, or the title's size. */
    state->guest_w = pp->BackBufferWidth  ? pp->BackBufferWidth  : 640;
    state->guest_h = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    state->width   = s_cfg_render_w ? s_cfg_render_w : state->guest_w;
    state->height  = s_cfg_render_h ? s_cfg_render_h : state->guest_h;
    rs_choose(state);               /* fork: render scale of the 3D (100 % by default) */

    memset(&scd, 0, sizeof(scd));
    scd.BufferCount = pp->BackBufferCount ? pp->BackBufferCount : 1;
    /* Zero: the swap chain takes the window's client size, which is the
     * render size unless the window had to be shrunk to fit the monitor or
     * starts fullscreen. host_output_resize keeps it matched afterwards. */
    scd.BufferDesc.Width = 0;
    scd.BufferDesc.Height = 0;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    /* The title runs on hardware with no window system, so it supplies no
     * output window. Make one sized to the frame it will be shown at. */
    scd.OutputWindow = (pp->hDeviceWindow && IsWindow(pp->hDeviceWindow))
                     ? pp->hDeviceWindow
                     : host_window_create(state->width, state->height);
    if (!scd.OutputWindow) return E_FAIL;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    /* Always windowed: an exclusive-fullscreen swap chain on a synthesised
     * window would take over the display before the game has drawn a frame.
     * Fullscreen is a borderless window instead (host_window_set_fullscreen). */
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    d3d8_sync_swap_desc(&scd);          /* flip model according to XBOX_SYNC */

    hr = D3D11CreateDeviceAndSwapChain(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        create_flags,
        NULL, 0,
        D3D11_SDK_VERSION,
        &scd,
        &state->swap_chain,
        &state->d3d11_device,
        &feature_level,
        &state->d3d11_context
    );
    if (FAILED(hr) && d3d8_sync_mode() != D3D8_SYNC_LEGACY) {
        d3d8_sync_fallback(&scd, hr);   /* flip model refused: the original chain */
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, create_flags,
                                           NULL, 0, D3D11_SDK_VERSION, &scd, &state->swap_chain,
                                           &state->d3d11_device, &feature_level, &state->d3d11_context);
    }

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Failed to create D3D11 device: 0x%08lX\n", hr);
        return hr;
    }

    state->hwnd = scd.OutputWindow;
    d3d8_sync_created(state->swap_chain, state->hwnd);

    /* Alt+Enter is ours (borderless toggle in host_wnd_proc); DXGI's would
     * switch to exclusive mode behind the renderer's back. */
    {
        IDXGIFactory *f = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(state->swap_chain, &IID_IDXGIFactory,
                                               (void **)&f)) && f) {
            IDXGIFactory_MakeWindowAssociation(f, state->hwnd, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory_Release(f);
        }
    }

    return S_OK;
}

static HRESULT d3d11_create_render_targets(D3D8DeviceState *state)
{
    HRESULT hr;

    /* The scene target: what the title renders into, at the render size,
     * with the chosen anti-aliasing; sampleable so a present can scale it. */
    state->msaa_requested = (UINT)s_want_msaa;
    hr = scene_targets_create(state, state->msaa_requested);
    if (FAILED(hr)) {
        scene_targets_release(state);
        hr = scene_targets_create(state, 1);
        if (FAILED(hr)) return hr;
    }

    /* Fork: the render scale's targets (none at 100 %). */
    rs_targets_create(state);

    /* The window's output buffer. */
    hr = swap_targets_create(state);
    if (FAILED(hr)) return hr;
    fprintf(stderr, "D3D8: rendering %ux%u (title frame %ux%u), output %ux%u, %s, %ux AA\n",
            state->width, state->height, state->guest_w, state->guest_h,
            state->swap_w, state->swap_h,
            s_cfg_aspect > 0.0 ? "wider than 16:9" : s_cfg_widescreen ? "16:9" : "4:3", state->msaa);
    if (s_cfg_aspect > 0.0)
        fprintf(stderr, "D3D8: display shape %.4f\n", s_cfg_aspect);
    fflush(stderr);
    return S_OK;
}

static void d3d8_init_default_states(D3D8DeviceState *state)
{
    /* Set Xbox D3D8 default render states */
    memset(state->render_states, 0, sizeof(state->render_states));
    state->render_states[D3DRS_ZENABLE]           = 1;
    state->render_states[D3DRS_FILLMODE]          = D3DFILL_SOLID;
    state->render_states[D3DRS_SHADEMODE]         = 2; /* D3DSHADE_GOURAUD */
    state->render_states[D3DRS_ZWRITEENABLE]      = TRUE;
    state->render_states[D3DRS_ALPHATESTENABLE]    = FALSE;
    state->render_states[D3DRS_SRCBLEND]          = D3DBLEND_ONE;
    state->render_states[D3DRS_DESTBLEND]         = D3DBLEND_ZERO;
    state->render_states[D3DRS_CULLMODE]          = D3DCULL_CCW;
    state->render_states[D3DRS_ZFUNC]             = D3DCMP_LESSEQUAL;
    state->render_states[D3DRS_ALPHAREF]          = 0;
    state->render_states[D3DRS_ALPHAFUNC]         = D3DCMP_ALWAYS;
    state->render_states[D3DRS_ALPHABLENDENABLE]   = FALSE;
    state->render_states[D3DRS_FOGENABLE]         = FALSE;
    state->render_states[D3DRS_STENCILENABLE]     = FALSE;
    state->render_states[D3DRS_COLORWRITEENABLE]  = 0x0F;

    /* Texture stage state defaults, per the D3D8 documented table.
     *
     * These were never initialised -- the table was left zeroed -- so every
     * consumer had to guess whether a zero meant 'unset' or a real value, and
     * did it with `tss[X] ? tss[X] : SOME_DEFAULT`. That idiom cannot express
     * D3DTA_DIFFUSE, which *is* zero: setting COLORARG1 to DIFFUSE read back as
     * TEXTURE, so a draw asking for vertex colour sampled an unbound texture
     * instead and came out transparent black. Seeding the real defaults here
     * lets every consumer just read the table.
     */
    {
        int st;
        memset(state->tss, 0, sizeof(state->tss));
        for (st = 0; st < MAX_TEXTURE_STAGES; st++) {
            state->tss[st][D3DTSS_COLOROP]   = (st == 0) ? D3DTOP_MODULATE
                                                         : D3DTOP_DISABLE;
            state->tss[st][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            state->tss[st][D3DTSS_COLORARG2] = D3DTA_CURRENT;
            state->tss[st][D3DTSS_ALPHAOP]   = (st == 0) ? D3DTOP_SELECTARG1
                                                         : D3DTOP_DISABLE;
            state->tss[st][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            state->tss[st][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
            state->tss[st][D3DTSS_TEXCOORDINDEX] = (DWORD)st;
            state->tss[st][D3DTSS_ADDRESSU]  = 1; /* D3DTADDRESS_WRAP */
            state->tss[st][D3DTSS_ADDRESSV]  = 1;
            state->tss[st][D3DTSS_MAGFILTER] = 1; /* D3DTEXF_POINT */
            state->tss[st][D3DTSS_MINFILTER] = 1;
            state->tss[st][D3DTSS_MIPFILTER] = 0; /* D3DTEXF_NONE */
        }
    }

    /* Default viewport */
    state->viewport.X = 0;
    state->viewport.Y = 0;
    state->viewport.Width = state->width;
    state->viewport.Height = state->height;
    state->viewport.MinZ = 0.0f;
    state->viewport.MaxZ = 1.0f;

    /* Identity matrices */
    for (int i = 0; i < MAX_TRANSFORMS; i++) {
        memset(&state->transforms[i], 0, sizeof(D3DMATRIX));
        state->transforms[i]._11 = 1.0f;
        state->transforms[i]._22 = 1.0f;
        state->transforms[i]._33 = 1.0f;
        state->transforms[i]._44 = 1.0f;
    }

    state->vertex_shader = 0;
    state->pixel_shader = 0;
    state->in_scene = FALSE;
}

/* ================================================================
 * IDirect3DDevice8 method implementations
 * ================================================================ */

static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *self)
{
    (void)self;
    return InterlockedIncrement(&g_device_state.ref_count);
}

static ULONG __stdcall dev_Release(IDirect3DDevice8 *self)
{
    (void)self;
    LONG ref = InterlockedDecrement(&g_device_state.ref_count);
    if (ref <= 0) {
        /* Cleanup subsystems first */
        up_ring_shutdown();
        d3d8_vsh_shutdown();
        d3d8_combiners_shutdown();
        d3d8_states_shutdown();
        d3d8_shaders_shutdown();

        /* Cleanup D3D11 resources */
        D3D8DeviceState *s = &g_device_state;
        rs_targets_release(s);          /* fork: render scale */
        d3d8_rscale_release();
        if (s->depth_srv) { ID3D11ShaderResourceView_Release(s->depth_srv); s->depth_srv = NULL; }
        if (s->default_dsv) { ID3D11DepthStencilView_Release(s->default_dsv); s->default_dsv = NULL; }
        if (s->default_depth) { ID3D11Texture2D_Release(s->default_depth); s->default_depth = NULL; }
        if (s->default_rtv) { ID3D11RenderTargetView_Release(s->default_rtv); s->default_rtv = NULL; }
        if (s->scene_srv) { ID3D11ShaderResourceView_Release(s->scene_srv); s->scene_srv = NULL; }
        if (s->scene_tex) { ID3D11Texture2D_Release(s->scene_tex); s->scene_tex = NULL; }
        if (s->scene_ms) { ID3D11Texture2D_Release(s->scene_ms); s->scene_ms = NULL; }
        if (s->swap_rtv) { ID3D11RenderTargetView_Release(s->swap_rtv); s->swap_rtv = NULL; }
        if (s->swap_tex) { ID3D11Texture2D_Release(s->swap_tex); s->swap_tex = NULL; }
        d3d8_sync_release();
        if (s->swap_chain) { IDXGISwapChain_Release(s->swap_chain); s->swap_chain = NULL; }
        if (s->d3d11_context) { ID3D11DeviceContext_Release(s->d3d11_context); s->d3d11_context = NULL; }
        if (s->d3d11_device) { ID3D11Device_Release(s->d3d11_device); s->d3d11_device = NULL; }
        g_device_initialized = FALSE;
    }
    return (ULONG)ref;
}

static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *self, IDirect3D8 **ppD3D8)
{
    (void)self; (void)ppD3D8;
    /* TODO: return the factory */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *self, void *pCaps)
{
    (void)self; (void)pCaps;
    /* TODO: fill with Xbox NV2A capabilities */
    return S_OK;
}

static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *self, void *pMode)
{
    (void)self; (void)pMode;
    return S_OK;
}

static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *self, void *pParams)
{
    (void)self; (void)pParams;
    return S_OK;
}

static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS *pPP)
{
    (void)self; (void)pPP;
    /* TODO: resize swap chain */
    return S_OK;
}

static DWORD g_d3d_begin_count = 0;
static DWORD g_d3d_end_count = 0;
static DWORD g_d3d_clear_count = 0;
static DWORD g_d3d_draw_count = 0;
static DWORD g_d3d_settransform_count = 0;
static DWORD g_d3d_setrs_count = 0;
static DWORD g_d3d_settexture_count = 0;

static HRESULT __stdcall dev_Present(IDirect3DDevice8 *self, const RECT *src, const RECT *dst, HWND hWnd, void *pDirty)
{
    static DWORD frame_count = 0;
    static DWORD last_tick = 0;
    (void)self; (void)src; (void)dst; (void)hWnd; (void)pDirty;

    frame_count++;
    DWORD now = GetTickCount();
    if (last_tick == 0) last_tick = now;
    if (now - last_tick >= 2000 && xbox_log_stats()) {
        fprintf(stderr, "  [D3D] %.1fs: %u present (%.1f fps), %u begin, %u end, "
                "%u clear, %u draw, %u xform, %u rs, %u tex\n",
                (now - last_tick) / 1000.0, frame_count,
                frame_count * 1000.0 / (now - last_tick),
                g_d3d_begin_count, g_d3d_end_count,
                g_d3d_clear_count, g_d3d_draw_count,
                g_d3d_settransform_count, g_d3d_setrs_count,
                g_d3d_settexture_count);
        fflush(stderr);
        frame_count = 0;
        g_d3d_begin_count = g_d3d_end_count = 0;
        g_d3d_clear_count = g_d3d_draw_count = 0;
        g_d3d_settransform_count = g_d3d_setrs_count = 0;
        g_d3d_settexture_count = 0;
        last_tick = now;
    }

    /* Pump Windows messages: the game's internal main loop drives rendering,
     * so our external message pump never runs. Process messages here to keep
     * the window responsive and handle input. */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    { static unsigned c = 0; if (++c == 1 || ((c % 300) == 0 && xbox_log_stats())) { fprintf(stderr, "  [PRESENT] dev_Present #%u\n", c); xbox_log_flush(); } }
    return host_present();
}

static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *self, INT iBackBuffer, DWORD Type, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)iBackBuffer; (void)Type; (void)ppSurface;
    /* TODO: wrap back buffer as D3D8 surface */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = TRUE;
    g_d3d_begin_count++;
    return S_OK;
}

static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = FALSE;
    g_d3d_end_count++;
    return S_OK;
}

static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *self, DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    (void)self; (void)Count; (void)pRects; (void)Stencil;
    g_d3d_clear_count++;

    if (Flags & D3DCLEAR_TARGET) {
        float clear_color[4] = {
            ((Color >> 16) & 0xFF) / 255.0f,  /* R */
            ((Color >>  8) & 0xFF) / 255.0f,  /* G */
            ((Color >>  0) & 0xFF) / 255.0f,  /* B */
            ((Color >> 24) & 0xFF) / 255.0f,  /* A */
        };
        ID3D11DeviceContext_ClearRenderTargetView(g_device_state.d3d11_context,
                                                   cur_rtv(&g_device_state),
                                                   clear_color);
    }

    if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
        UINT clear_flags = 0;
        if (Flags & D3DCLEAR_ZBUFFER) clear_flags |= D3D11_CLEAR_DEPTH;
        if (Flags & D3DCLEAR_STENCIL) clear_flags |= D3D11_CLEAR_STENCIL;

        ID3D11DeviceContext_ClearDepthStencilView(g_device_state.d3d11_context,
                                                    cur_dsv(&g_device_state),
                                                    clear_flags, Z, (UINT8)Stencil);
    }

    return S_OK;
}

/*
 * A clear limited to the title's clear rectangle. The race intro and the
 * winner camera end every frame by clearing the top and bottom rows of the
 * surface to black (rects y 0..0 and 479..479, after a blue clear of rows
 * 1..476 and ~1,200 draws). Clearing the whole target for those wiped the
 * finished frame, so the screen was black unless the intro was skipped; it
 * only ever showed when a slow frame (a draw log) made the clear present
 * the frame first. ClearView (D3D11.1) takes rectangles for render targets;
 * depth and stencil have no rectangle clear and are cleared whole, which
 * is harmless for the end-of-frame strips and for per-viewport clears.
 */
void d3d8_ClearRect(DWORD flags, D3DCOLOR color, float z, DWORD stencil,
                    unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    D3D8DeviceState *s = &g_device_state;
    UINT gw = s->guest_w ? s->guest_w : s->width, gh = s->guest_h ? s->guest_h : s->height;
    static ID3D11DeviceContext1 *ctx1 = NULL;
    static int tried = 0;

    if ((flags & D3DCLEAR_TARGET) && cur_rtv(s) &&
        !(x0 == 0 && y0 == 0 && x1 + 1 >= gw && y1 + 1 >= gh)) {
        if (!tried) {
            tried = 1;
            if (FAILED(ID3D11DeviceContext_QueryInterface(s->d3d11_context,
                           &IID_ID3D11DeviceContext1, (void **)&ctx1)))
                ctx1 = NULL;
        }
        if (ctx1) {
            float sx, sy;
            D3D11_RECT r;
            const FLOAT c[4] = { ((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f,
                                 (color & 0xFF) / 255.0f, ((color >> 24) & 0xFF) / 255.0f };
            d3d8_GetGuestScale(&sx, &sy);
            if (x1 >= gw) x1 = gw - 1;
            if (y1 >= gh) y1 = gh - 1;
            r.left = (LONG)(x0 * sx + 0.5f);
            r.top = (LONG)(y0 * sy + 0.5f);
            r.right = (LONG)((x1 + 1) * sx + 0.5f);
            r.bottom = (LONG)((y1 + 1) * sy + 0.5f);
            if (g_gpuprof_on) gpuprof_begin(GP_CLEAR);
            if (x0 <= x1 && y0 <= y1)
                ID3D11DeviceContext1_ClearView(ctx1, (ID3D11View *)cur_rtv(s), c, &r, 1);
            if (g_gpuprof_on) gpuprof_end();
            flags &= ~(DWORD)D3DCLEAR_TARGET;
        }
    }
    if (flags) {
        if (g_gpuprof_on) gpuprof_begin(GP_CLEAR);
        dev_Clear(NULL, 0, NULL, flags, color, z, stencil);
        if (g_gpuprof_on) gpuprof_end();
    }
}

static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
    (void)self;
    g_d3d_settransform_count++;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        g_device_state.transforms[(DWORD)State] = *pMatrix;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
    (void)self;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        *pMatrix = g_device_state.transforms[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD Value)
{
    (void)self;
    g_d3d_setrs_count++;
    if ((DWORD)State < MAX_RENDER_STATES) {
        g_device_state.render_states[(DWORD)State] = Value;
    }
    /* Mark combiner state dirty if any PS register combiner state changed */
    if ((DWORD)State >= D3DRS_PSALPHAINPUTS0 && (DWORD)State <= D3DRS_PSINPUTTEXTURE) {
        d3d8_combiners_mark_dirty();
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD *pValue)
{
    (void)self;
    if ((DWORD)State < MAX_RENDER_STATES && pValue) {
        *pValue = g_device_state.render_states[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES) {
        g_device_state.tss[Stage][(DWORD)Type] = Value;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES && pValue) {
        *pValue = g_device_state.tss[Stage][(DWORD)Type];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 *pTexture)
{
    (void)self;
    g_d3d_settexture_count++;
    if (Stage >= 4) return E_INVALIDARG;
    if (g_perf_on) perf_count(PC_SRV, 1);      /* a texture bind (PSSetShaderResources) */
    g_cur_textures[Stage] = pTexture;

    /* Bind SRV to pixel shader */
    if (pTexture) {
        D3D8Texture *tex = (D3D8Texture *)pTexture;
        if (tex->srv) {
            ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                Stage, 1, &tex->srv);
        }
        /* Mark texture stage as active */
        if (g_device_state.tss[Stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
            g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_MODULATE;
    } else {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
            Stage, 1, &null_srv);
        g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_DISABLE;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 **ppTexture)
{
    (void)self; (void)Stage; (void)ppTexture;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride)
{
    (void)self;
    if (StreamNumber != 0) return S_OK; /* Only stream 0 supported */
    g_cur_vb = pStreamData;
    g_cur_vb_stride = Stride;

    if (pStreamData) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)pStreamData;
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &Stride, &offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride)
{
    (void)self; (void)StreamNumber; (void)ppStreamData; (void)pStride;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex)
{
    (void)self;
    g_cur_ib = pIndexData;
    g_cur_ib_base_vertex = BaseVertexIndex;

    if (pIndexData) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)pIndexData;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32)
            ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex)
{
    (void)self; (void)ppIndexData; (void)pBaseVertexIndex;
    return E_NOTIMPL;
}

static D3D11_PRIMITIVE_TOPOLOGY map_primitive_type(D3DPRIMITIVETYPE pt, UINT count, UINT *out_count)
{
    switch (pt) {
    case D3DPT_TRIANGLELIST:  *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_TRIANGLESTRIP: *out_count = count + 2; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case D3DPT_TRIANGLEFAN:   *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_LINELIST:      *out_count = count * 2; return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case D3DPT_LINESTRIP:     *out_count = count + 1; return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case D3DPT_POINTLIST:     *out_count = count;     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case D3DPT_QUADLIST:      *out_count = count * 6; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                  *out_count = 0;          return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/* ================================================================
 * Triangle fan / quad list → triangle list conversion
 *
 * D3D11 doesn't support triangle fans or quad lists.
 * Convert vertex data in-place to triangle list.
 * Returns malloc'd buffer (caller must free) or NULL if no conversion needed.
 * ================================================================ */

static void *convert_fan_or_quad(D3DPRIMITIVETYPE pt, const void *src,
                                  UINT prim_count, UINT stride,
                                  UINT *out_vertex_count)
{
    BYTE *dst;
    const BYTE *s = (const BYTE *)src;
    UINT i;

    if (pt == D3DPT_TRIANGLEFAN) {
        /* Fan: vertex 0 is the hub, each triangle is (0, i+1, i+2) */
        UINT tri_verts = prim_count * 3;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            memcpy(dst + (i * 3 + 0) * stride, s, stride);                      /* v0 (hub) */
            memcpy(dst + (i * 3 + 1) * stride, s + (i + 1) * stride, stride);   /* v[i+1] */
            memcpy(dst + (i * 3 + 2) * stride, s + (i + 2) * stride, stride);   /* v[i+2] */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    if (pt == D3DPT_QUADLIST) {
        /* Quad list: each quad (v0,v1,v2,v3) → 2 triangles (v0,v1,v2), (v0,v2,v3) */
        UINT tri_verts = prim_count * 6;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            const BYTE *q = s + i * 4 * stride;
            memcpy(dst + (i * 6 + 0) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 1) * stride, q + 1 * stride, stride);  /* v1 */
            memcpy(dst + (i * 6 + 2) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 3) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 4) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 5) * stride, q + 3 * stride, stride);  /* v3 */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    return NULL; /* no conversion needed */
}

/* ================================================================
 * DrawPrimitiveUP ring buffer
 *
 * Instead of creating and destroying a D3D11 buffer on every
 * DrawPrimitiveUP call, use a persistent ring buffer.
 * ================================================================ */

/* 32 MB. The program-draw path (d3d8_nv2a.c) uploads expanded
 * 116-byte vertices; character select alone sends ~1,000 draws a frame, well
 * past the old 4 MB, so the ring wrapped several times per frame. */
#define UP_RING_BUFFER_SIZE (32 * 1024 * 1024)

static ID3D11Buffer *g_up_ring_buffer = NULL;
static UINT          g_up_ring_offset = 0;

static HRESULT up_ring_init(void)
{
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = UP_RING_BUFFER_SIZE;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, NULL, &g_up_ring_buffer);
}

static void up_ring_shutdown(void)
{
    if (g_up_ring_buffer) {
        ID3D11Buffer_Release(g_up_ring_buffer);
        g_up_ring_buffer = NULL;
    }
    g_up_ring_offset = 0;
}

/* Upload vertex data to ring buffer, returns offset. Returns (UINT)-1 on failure. */
static UINT up_ring_upload(const void *data, UINT size)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_MAP map_type;
    HRESULT hr;
    UINT offset;

    if (!g_up_ring_buffer) {
        if (FAILED(up_ring_init())) return (UINT)-1;
    }

    if (size > UP_RING_BUFFER_SIZE) return (UINT)-1;

    /* Wrap around if not enough space. The wrap must DISCARD: a dynamic
     * buffer accepts only WRITE_DISCARD and WRITE_NO_OVERWRITE, so the old
     * D3D11_MAP_WRITE failed, the draw was dropped, and the next upload wrote
     * at offset 0 with NO_OVERWRITE -- over vertices that draws queued
     * earlier in the frame had not consumed yet. Character select showed it
     * as screen-filling shards that changed every frame. */
    if (g_up_ring_offset + size > UP_RING_BUFFER_SIZE) {
        g_up_ring_offset = 0;
        map_type = D3D11_MAP_WRITE_DISCARD;
    } else {
        map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
    }

    {
        unsigned long long t = g_perf_maptime ? __rdtsc() : 0;   /* XBOX_PERF_MAPTIME */
        hr = ID3D11DeviceContext_Map(g_device_state.d3d11_context,
            (ID3D11Resource *)g_up_ring_buffer, 0, map_type, 0, &mapped);
        if (g_perf_maptime) g_perf_map_cyc += __rdtsc() - t;
    }
    if (FAILED(hr)) return (UINT)-1;

    offset = g_up_ring_offset;
    memcpy((BYTE *)mapped.pData + offset, data, size);

    {
        unsigned long long t = g_perf_maptime ? __rdtsc() : 0;
        ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
            (ID3D11Resource *)g_up_ring_buffer, 0);
        if (g_perf_maptime) { g_perf_map_cyc += __rdtsc() - t; g_perf_map_pairs++; }
    }

    g_up_ring_offset = (offset + size + 15) & ~15;  /* 16-byte align */
    return offset;
}

/* Make the next `total` bytes of uploads (in `pieces` separate uploads) land
 * in one instance of the ring. A draw that uploads several vertex
 * streams must not wrap between them: the wrap DISCARDs the buffer, so the
 * streams written before it are gone while the draw still points at them --
 * an intermittent frame of screen-filling shards in character select. When
 * they would not fit, the wrap is taken before the first of them. */
void d3d8_UpRingReserve(UINT total, UINT pieces)
{
    UINT need = total + pieces * 16u;
    if (need <= UP_RING_BUFFER_SIZE && g_up_ring_offset + need > UP_RING_BUFFER_SIZE)
        g_up_ring_offset = UP_RING_BUFFER_SIZE;   /* the next upload wraps */
}

/* The ring upload for d3d8_nv2a.c's NV2A-native draws. */
UINT d3d8_UpRingUpload(const void *data, UINT size, ID3D11Buffer **buf)
{
    UINT off = up_ring_upload(data, size);
    *buf = g_up_ring_buffer;
    return off;
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Prepare pipeline: shaders, input layout, constant buffers, render states */
    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    if (!d3d8_vsh_prepare_draw(g_device_state.vertex_shader))
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    {   /* the Time Challenge ghost (d3d8_nv2a.c): see-through, or not drawn */
        int gm = d3d8_ghost_mode();
        if (gm == 2) {
            d3d8_ghost_pass(g_device_state.d3d11_context, 0);
            ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, StartVertex);
            d3d8_ghost_pass(g_device_state.d3d11_context, 1);
            ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, StartVertex);
            d3d8_ghost_pass(g_device_state.d3d11_context, 2);
        } else if (gm == 0) {
            ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, StartVertex);
        }
    }
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
    (void)self; (void)MinVertexIndex; (void)NumVertices;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT index_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    if (!d3d8_vsh_prepare_draw(g_device_state.vertex_shader))
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    {   /* the Time Challenge ghost (d3d8_nv2a.c): see-through, or not drawn */
        int gm = d3d8_ghost_mode();
        if (gm == 2) {
            d3d8_ghost_pass(g_device_state.d3d11_context, 0);
            ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, StartIndex, (INT)g_cur_ib_base_vertex);
            d3d8_ghost_pass(g_device_state.d3d11_context, 1);
            ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, StartIndex, (INT)g_cur_ib_base_vertex);
            d3d8_ghost_pass(g_device_state.d3d11_context, 2);
        } else if (gm == 0) {
            ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, StartIndex, (INT)g_cur_ib_base_vertex);
        }
    }
    return S_OK;
}

static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count, vb_size, ring_offset;
    const void *draw_data = pVertexData;
    void *converted = NULL;

    if (!pVertexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Convert triangle fans and quad lists to triangle lists */
    if (PrimitiveType == D3DPT_TRIANGLEFAN || PrimitiveType == D3DPT_QUADLIST) {
        converted = convert_fan_or_quad(PrimitiveType, pVertexData,
                                         PrimitiveCount, VertexStreamZeroStride,
                                         &vertex_count);
        if (converted) draw_data = converted;
    }

    vb_size = vertex_count * VertexStreamZeroStride;

    /* Upload to ring buffer */
    ring_offset = up_ring_upload(draw_data, vb_size);
    if (converted) free(converted);

    if (ring_offset == (UINT)-1) return E_OUTOFMEMORY;

    /* Bind ring buffer at the right offset */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &g_up_ring_buffer, &VertexStreamZeroStride, &ring_offset);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    if (!d3d8_vsh_prepare_draw(g_device_state.vertex_shader))
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);

    /* One-shot pipeline audit. The translator submits a correct full-screen
     * quad and the draw count climbs, but the back buffer reads pure black,
     * so something between here and the output merger is dropping it. Report
     * what is actually bound rather than assuming. */
    {
        static int audited = 0;
        if (!audited) {
            ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL;
            ID3D11InputLayout *il = NULL; ID3D11RenderTargetView *rtv = NULL;
            ID3D11DepthStencilView *dsv = NULL; ID3D11DepthStencilState *dss = NULL;
            ID3D11BlendState *bs = NULL; ID3D11RasterizerState *rs = NULL;
            D3D11_VIEWPORT vp[8]; UINT nvp = 8; UINT sref = 0;
            FLOAT bf[4]; UINT smask = 0;
            audited = 1;
            ID3D11DeviceContext_VSGetShader(g_device_state.d3d11_context, &vs, NULL, NULL);
            ID3D11DeviceContext_PSGetShader(g_device_state.d3d11_context, &ps, NULL, NULL);
            ID3D11DeviceContext_IAGetInputLayout(g_device_state.d3d11_context, &il);
            ID3D11DeviceContext_OMGetRenderTargets(g_device_state.d3d11_context, 1, &rtv, &dsv);
            ID3D11DeviceContext_OMGetDepthStencilState(g_device_state.d3d11_context, &dss, &sref);
            ID3D11DeviceContext_OMGetBlendState(g_device_state.d3d11_context, &bs, bf, &smask);
            ID3D11DeviceContext_RSGetState(g_device_state.d3d11_context, &rs);
            ID3D11DeviceContext_RSGetViewports(g_device_state.d3d11_context, &nvp, vp);
            fprintf(stderr, "  [D3D]   rtv is %s (default_rtv=%p)\n",
                    (rtv == g_device_state.default_rtv) ? "the back buffer"
                                                        : "NOT the back buffer",
                    (void *)g_device_state.default_rtv);
            fprintf(stderr, "  [D3D] draw audit: vs=%p ps=%p layout=%p rtv=%p dsv=%p\n"
                            "  [D3D]   dss=%p bs=%p rs=%p smask=0x%X viewports=%u",
                    (void*)vs, (void*)ps, (void*)il, (void*)rtv, (void*)dsv,
                    (void*)dss, (void*)bs, (void*)rs, smask, nvp);
            if (nvp)
                fprintf(stderr, " vp0=(%.0f,%.0f %.0fx%.0f z %.2f..%.2f)",
                        vp[0].TopLeftX, vp[0].TopLeftY, vp[0].Width, vp[0].Height,
                        vp[0].MinDepth, vp[0].MaxDepth);
            if (dss) {
                D3D11_DEPTH_STENCIL_DESC dd;
                ID3D11DepthStencilState_GetDesc(dss, &dd);
                fprintf(stderr, "\n  [D3D]   depth: enable=%d writemask=%d func=%d",
                        (int)dd.DepthEnable, (int)dd.DepthWriteMask, (int)dd.DepthFunc);
            }
            if (bs) {
                D3D11_BLEND_DESC bd;
                ID3D11BlendState_GetDesc(bs, &bd);
                fprintf(stderr, "\n  [D3D]   blend: enable=%d src=%d dst=%d rtwritemask=0x%X",
                        (int)bd.RenderTarget[0].BlendEnable,
                        (int)bd.RenderTarget[0].SrcBlend,
                        (int)bd.RenderTarget[0].DestBlend,
                        (unsigned)bd.RenderTarget[0].RenderTargetWriteMask);
            }
            if (rs) {
                D3D11_RASTERIZER_DESC rd;
                ID3D11RasterizerState_GetDesc(rs, &rd);
                fprintf(stderr, "\n  [D3D]   raster: cull=%d fill=%d scissor=%d",
                        (int)rd.CullMode, (int)rd.FillMode, (int)rd.ScissorEnable);
            }
            fprintf(stderr, "\n  [D3D]   topology=%d verts=%u stride=%u\n",
                    (int)topology, vertex_count, VertexStreamZeroStride);
            fflush(stderr);
            if (vs) ID3D11VertexShader_Release(vs);
            if (ps) ID3D11PixelShader_Release(ps);
            if (il) ID3D11InputLayout_Release(il);
            if (rtv) ID3D11RenderTargetView_Release(rtv);
            if (dsv) ID3D11DepthStencilView_Release(dsv);
            if (dss) ID3D11DepthStencilState_Release(dss);
            if (bs) ID3D11BlendState_Release(bs);
            if (rs) ID3D11RasterizerState_Release(rs);
        }
    }
    {   /* the Time Challenge ghost (d3d8_nv2a.c): see-through, or not drawn; fork: the
         * lights (d3d8_lights.h): drawn again into the emission target, or there only */
        ID3D11DeviceContext *lctx = g_device_state.d3d11_context;
        int gm = d3d8_ghost_mode();
        if (gm == 2) {
            d3d8_ghost_pass(lctx, 0);
            ID3D11DeviceContext_Draw(lctx, vertex_count, 0);
            d3d8_ghost_pass(lctx, 1);
            ID3D11DeviceContext_Draw(lctx, vertex_count, 0);
            d3d8_ghost_pass(lctx, 2);
        } else if (gm == 0) {
            int lr = g_d3d8_light_mark ? d3d8_lights_pre(lctx) : 0;
            ID3D11DeviceContext_Draw(lctx, vertex_count, 0);
            if (lr == 2) {
                if (d3d8_lights_z_begin(lctx)) {   /* its depth, for the occlusion */
                    ID3D11DeviceContext_Draw(lctx, vertex_count, 0);
                    d3d8_lights_z_end(lctx);
                }
                d3d8_lights_mirror_end(lctx);
            } else if (lr == 1 && d3d8_lights_mirror_begin(lctx)) {
                ID3D11DeviceContext_Draw(lctx, vertex_count, 0);
                if (d3d8_lights_z_begin(lctx)) {
                    ID3D11DeviceContext_Draw(lctx, vertex_count, 0);
                    d3d8_lights_z_end(lctx);
                }
                d3d8_lights_mirror_end(lctx);
            }
        }
    }

    /* Read the vertices back out of the GPU buffer. Everything CPU-side has
     * checked out, so confirm the bytes the input assembler will actually
     * fetch rather than the bytes we believe we uploaded. */
    {
        static int dumped = 0;
        if (!dumped && g_up_ring_buffer && vb_size <= 256) {
            D3D11_BUFFER_DESC sd;
            ID3D11Buffer *stage = NULL;
            dumped = 1;
            memset(&sd, 0, sizeof(sd));
            sd.ByteWidth = 256;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (SUCCEEDED(ID3D11Device_CreateBuffer(g_device_state.d3d11_device,
                    &sd, NULL, &stage)) && stage) {
                D3D11_BOX box;
                D3D11_MAPPED_SUBRESOURCE m;
                box.left = ring_offset; box.right = ring_offset + vb_size;
                box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
                ID3D11DeviceContext_CopySubresourceRegion(g_device_state.d3d11_context,
                    (ID3D11Resource *)stage, 0, 0, 0, 0,
                    (ID3D11Resource *)g_up_ring_buffer, 0, &box);
                if (SUCCEEDED(ID3D11DeviceContext_Map(g_device_state.d3d11_context,
                        (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &m))) {
                    const float *f = (const float *)m.pData;
                    UINT v, nv = vb_size / VertexStreamZeroStride;
                    fprintf(stderr, "  [D3D] vertices in GPU buffer (offset %u, %u bytes):\n",
                            ring_offset, vb_size);
                    for (v = 0; v < nv && v < 4; v++) {
                        const unsigned char *b = (const unsigned char *)m.pData
                                               + v * VertexStreamZeroStride;
                        memcpy((void *)&f, &b, sizeof b);
                        fprintf(stderr, "  [D3D]   v%u = (%.1f, %.1f, %.1f, %.1f) color=%08X\n",
                                v, ((const float *)b)[0], ((const float *)b)[1],
                                ((const float *)b)[2], ((const float *)b)[3],
                                *(const unsigned *)(b + 16));
                    }
                    fflush(stderr);
                    ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                        (ID3D11Resource *)stage, 0);
                }
                ID3D11Buffer_Release(stage);
            }
        }
    }

    /* Drain whatever the validation layer has to say about that draw. */
    {
        static int drained = 0;
        if (drained < 3 && g_device_state.d3d11_device) {
            ID3D11InfoQueue *iq = NULL;
            if (SUCCEEDED(ID3D11Device_QueryInterface(g_device_state.d3d11_device,
                    &IID_ID3D11InfoQueue, (void **)&iq)) && iq) {
                UINT64 n = ID3D11InfoQueue_GetNumStoredMessages(iq), k;
                drained++;
                for (k = 0; k < n && k < 24; k++) {
                    SIZE_T len = 0;
                    if (SUCCEEDED(ID3D11InfoQueue_GetMessage(iq, k, NULL, &len)) && len) {
                        D3D11_MESSAGE *m = (D3D11_MESSAGE *)malloc(len);
                        if (m && SUCCEEDED(ID3D11InfoQueue_GetMessage(iq, k, m, &len)))
                            fprintf(stderr, "  [D3D-VALID] sev=%d id=%d: %.*s\n",
                                    (int)m->Severity, (int)m->ID,
                                    (int)m->DescriptionByteLength, m->pDescription);
                        free(m);
                    }
                }
                ID3D11InfoQueue_ClearStoredMessages(iq);
                ID3D11InfoQueue_Release(iq);
                fflush(stderr);
            }
        }
    }

    /* Restore previous VB binding if any */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        UINT restore_offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &restore_offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self; (void)MinVertexIndex;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Buffer *tmp_vb = NULL, *tmp_ib = NULL;
    UINT index_count, vb_size, ib_size, offset = 0;
    UINT idx_bytes;
    DXGI_FORMAT ib_fmt;
    HRESULT hr;

    if (!pVertexData || !pIndexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    idx_bytes = (IndexDataFormat == D3DFMT_INDEX32) ? 4 : 2;
    ib_fmt = (IndexDataFormat == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    vb_size = NumVertices * VertexStreamZeroStride;
    ib_size = index_count * idx_bytes;

    /* Create temp vertex buffer */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = vb_size;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = pVertexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_vb);
    if (FAILED(hr)) return hr;

    /* Create temp index buffer */
    bd.ByteWidth = ib_size;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = pIndexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_ib);
    if (FAILED(hr)) { ID3D11Buffer_Release(tmp_vb); return hr; }

    /* Bind, prepare, draw */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &tmp_vb, &VertexStreamZeroStride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
        tmp_ib, ib_fmt, 0);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    if (!d3d8_vsh_prepare_draw(g_device_state.vertex_shader))
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    {   /* the Time Challenge ghost (d3d8_nv2a.c): see-through, or not drawn */
        int gm = d3d8_ghost_mode();
        if (gm == 2) {
            d3d8_ghost_pass(g_device_state.d3d11_context, 0);
            ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, 0, 0);
            d3d8_ghost_pass(g_device_state.d3d11_context, 1);
            ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, 0, 0);
            d3d8_ghost_pass(g_device_state.d3d11_context, 2);
        } else if (gm == 0) {
            ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, 0, 0);
        }
    }

    /* Cleanup temp buffers */
    ID3D11Buffer_Release(tmp_ib);
    ID3D11Buffer_Release(tmp_vb);

    /* Restore previous bindings */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &offset);
    }
    if (g_cur_ib) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)g_cur_ib;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_CreateTexture(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateTextureImpl(Width, Height, Levels, Usage, Format, ppTexture);
}

static HRESULT __stdcall dev_CreateVertexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateVertexBufferImpl(Length, Usage, FVF, ppVertexBuffer);
}

static HRESULT __stdcall dev_CreateIndexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateIndexBufferImpl(Length, Usage, Format, ppIndexBuffer);
}

static HRESULT __stdcall dev_CreateRenderTarget(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)Lockable; (void)ppSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_CreateDepthStencilSurface(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)ppSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pZStencilSurface)
{
    (void)self; (void)pRenderTarget; (void)pZStencilSurface;
    /* TODO: resolve D3D8 surface to D3D11 RTV/DSV */
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 **ppRenderTarget)
{
    (void)self; (void)ppRenderTarget;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDepthStencilSurface(IDirect3DDevice8 *self, IDirect3DSurface8 **ppZStencilSurface)
{
    (void)self; (void)ppZStencilSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *self, const D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) {
        g_device_state.viewport = *pViewport;

        D3D11_VIEWPORT d3d11_vp;
        float sx, sy;
        d3d8_GetGuestScale(&sx, &sy);   /* title pixels -> scene target */
        d3d11_vp.TopLeftX = (FLOAT)pViewport->X * sx;
        d3d11_vp.TopLeftY = (FLOAT)pViewport->Y * sy;
        d3d11_vp.Width    = (FLOAT)pViewport->Width * sx;
        d3d11_vp.Height   = (FLOAT)pViewport->Height * sy;
        d3d11_vp.MinDepth = pViewport->MinZ;
        d3d11_vp.MaxDepth = pViewport->MaxZ;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &d3d11_vp);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *self, D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) *pViewport = g_device_state.viewport;
    return S_OK;
}

static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *self, const D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) g_device_state.material = *pMaterial;
    return S_OK;
}

static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *self, D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) *pMaterial = g_device_state.material;
    return S_OK;
}

static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *self, DWORD Index, const D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) g_device_state.lights[Index] = *pLight;
    return S_OK;
}

static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *self, DWORD Index, D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) *pLight = g_device_state.lights[Index];
    return S_OK;
}

static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *self, DWORD Index, BOOL Enable)
{
    (void)self;
    if (Index < MAX_LIGHTS) g_device_state.light_enable[Index] = Enable;
    return S_OK;
}

static HRESULT __stdcall dev_CreateVertexShader(IDirect3DDevice8 *self, const DWORD *pDeclaration, const DWORD *pFunction, DWORD *pHandle, DWORD Usage)
{
    (void)self; (void)pDeclaration; (void)Usage;
    if (!pHandle) return E_INVALIDARG;
    if (!pFunction) return E_INVALIDARG;
    /* Count instructions: each is 4 DWORDs, last has bit 0 of word[3] set (END flag) */
    {
        int i, num_insns = 0;
        for (i = 0; i < 136; i++) {
            num_insns++;
            if (pFunction[i * 4 + 3] & 1) break;  /* END bit in last word */
        }
        return d3d8_vsh_create_shader(pFunction, num_insns, pHandle);
    }
}

static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.vertex_shader = Handle;
    return S_OK;
}

static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.vertex_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetVertexShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self;
    d3d8_vsh_set_constant(Register, pConstantData, ConstantCount);
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.pixel_shader = Handle;
    d3d8_combiners_set_pixel_shader(Handle);
    return S_OK;
}

static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.pixel_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self; (void)Register; (void)pConstantData; (void)ConstantCount;
    return S_OK;
}

static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *self, DWORD Flags, const D3DGAMMARAMP *pRamp)
{
    (void)self; (void)Flags; (void)pRamp;
}

static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *self, D3DGAMMARAMP *pRamp)
{
    (void)self; (void)pRamp;
}

static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *self, DWORD PaletteNumber, const void *pEntries)
{
    (void)self; (void)PaletteNumber; (void)pEntries;
    return S_OK;
}

static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *self, DWORD Count, DWORD **ppPush)
{
    (void)self; (void)Count; (void)ppPush;
    /* TODO: Xbox push buffer emulation */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *self, DWORD *pPush)
{
    (void)self; (void)pPush;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *self, DWORD Flags)
{
    (void)self; (void)Flags;

    /* Pump Windows messages (same as dev_Present) */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    { static unsigned c = 0; if (++c == 1 || ((c % 300) == 0 && xbox_log_stats())) { fprintf(stderr, "  [PRESENT] dev_BeginPush #%u\n", c); xbox_log_flush(); } }
    return host_present();
}

/* ================================================================
 * Vtable
 * ================================================================ */

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface,
    dev_AddRef,
    dev_Release,
    dev_GetDirect3D,
    dev_GetDeviceCaps,
    dev_GetDisplayMode,
    dev_GetCreationParameters,
    dev_Reset,
    dev_Present,
    dev_GetBackBuffer,
    dev_BeginScene,
    dev_EndScene,
    dev_Clear,
    dev_SetTransform,
    dev_GetTransform,
    dev_SetRenderState,
    dev_GetRenderState,
    dev_SetTextureStageState,
    dev_GetTextureStageState,
    dev_SetTexture,
    dev_GetTexture,
    dev_SetStreamSource,
    dev_GetStreamSource,
    dev_SetIndices,
    dev_GetIndices,
    dev_DrawPrimitive,
    dev_DrawIndexedPrimitive,
    dev_DrawPrimitiveUP,
    dev_DrawIndexedPrimitiveUP,
    dev_CreateTexture,
    dev_CreateVertexBuffer,
    dev_CreateIndexBuffer,
    dev_CreateRenderTarget,
    dev_CreateDepthStencilSurface,
    dev_SetRenderTarget,
    dev_GetRenderTarget,
    dev_GetDepthStencilSurface,
    dev_SetViewport,
    dev_GetViewport,
    dev_SetMaterial,
    dev_GetMaterial,
    dev_SetLight,
    dev_GetLight,
    dev_LightEnable,
    dev_SetVertexShader,
    dev_GetVertexShader,
    dev_SetVertexShaderConstant,
    dev_SetPixelShader,
    dev_GetPixelShader,
    dev_SetPixelShaderConstant,
    dev_SetGammaRamp,
    dev_GetGammaRamp,
    dev_SetPalette,
    dev_BeginPush,
    dev_EndPush,
    dev_Swap,
};

/* ================================================================
 * Public API
 * ================================================================ */

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    return g_device_initialized ? &g_device : NULL;
}

/* ================================================================
 * IDirect3D8 factory implementation
 * ================================================================ */

static IDirect3D8 g_d3d8;
static LONG g_d3d8_ref = 0;

static HRESULT __stdcall d3d8_QueryInterface(IDirect3D8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall d3d8_AddRef(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedIncrement(&g_d3d8_ref);
}

static ULONG __stdcall d3d8_Release(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedDecrement(&g_d3d8_ref);
}

static HRESULT __stdcall d3d8_CreateDevice(IDirect3D8 *self, UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice8 **ppDevice)
{
    (void)self; (void)Adapter; (void)DeviceType; (void)BehaviorFlags;
    HRESULT hr;

    if (!pPP || !ppDevice) return E_INVALIDARG;

    memset(&g_device_state, 0, sizeof(g_device_state));
    g_device_state.ref_count = 1;

    if (!pPP->hDeviceWindow) pPP->hDeviceWindow = hFocusWindow;

    hr = d3d11_create_device_and_swap_chain(&g_device_state, pPP);
    if (FAILED(hr)) return hr;

    hr = d3d11_create_render_targets(&g_device_state);
    if (FAILED(hr)) return hr;

    d3d8_init_default_states(&g_device_state);

    /* Set initial viewport (D3D11 requires explicit viewport) */
    {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width    = (FLOAT)g_device_state.width;
        vp.Height   = (FLOAT)g_device_state.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &vp);
    }

    /* Initialize shader and state subsystems */
    hr = d3d8_shaders_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Shader init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_states_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: State init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_combiners_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Combiner init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to fixed-function pixel shaders */
    }

    hr = d3d8_vsh_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: VSH init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to FVF vertex shaders */
    }

    g_device.lpVtbl = &g_device_vtbl;
    g_device_initialized = TRUE;

    *ppDevice = &g_device;
    fprintf(stderr, "D3D8: Device created (%ux%u)\n", g_device_state.width, g_device_state.height);
    return S_OK;
}

static const IDirect3D8Vtbl g_d3d8_vtbl = {
    d3d8_QueryInterface,
    d3d8_AddRef,
    d3d8_Release,
    d3d8_CreateDevice,
};

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    g_d3d8.lpVtbl = &g_d3d8_vtbl;
    g_d3d8_ref = 1;
    return &g_d3d8;
}

/* ---------------------------------------------------------------------------
 * d3d8_PresentGuestFramebuffer -- copy guest video memory into the back buffer.
 *
 * The title's video player bypasses the push buffer entirely: it locks the back
 * buffer through the Xbox D3D8 linked into the XBE and writes decoded MPEG
 * pixels straight into guest VRAM. On hardware the GPU scans that out; here the
 * host swap chain is a separate surface, so without this the frames have
 * nowhere to go.
 *
 * Done with raw D3D11 rather than through the D3D8 shim's texture objects: the
 * shim's LockRect hands back its own sys_mem/pitch, which is not a route to the
 * swap chain and produced a wild pointer when used as one.
 *
 * Guest pixels are A8R8G8B8 (B,G,R,A in memory); the swap chain is
 * R8G8B8A8_UNORM, so red and blue are swapped per pixel on the way in.
 * ------------------------------------------------------------------------ */
void d3d8_PresentGuestFramebuffer(const void *src, unsigned pitch,
                                  unsigned w, unsigned h)
{
    static ID3D11Texture2D *stage = NULL;
    static unsigned stage_w = 0, stage_h = 0;
    static unsigned char *conv = NULL;    /* the frame converted, CPU side */
    ID3D11Texture2D *back = NULL;
    unsigned y, x;

    /* src == NULL means "nothing new is safe to copy -- re-present the frame
     * staging already holds". Re-reading guest memory instead would re-copy a
     * buffer that may be mid-write by now, which is where the residual
     * backwards/torn frames came from. */
    if (!w || !h || !g_device_state.d3d11_device
        || !g_device_state.d3d11_context || !g_device_state.swap_chain)
        return;
    if (!src && !stage) return;
    s_video_frame = 1;      /* this present shows the video as it is */

    if (stage && (stage_w != w || stage_h != h)) {
        ID3D11Texture2D_Release(stage);
        stage = NULL;
        free(conv);
        conv = NULL;
    }
    if (!stage) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        /* STAGING, not DYNAMIC: a DYNAMIC resource is not a legal participant
         * in CopySubresourceRegion, so the copy was silently doing nothing
         * while the map and the pixel conversion both succeeded -- which is
         * why the source reported 4032 non-black pixels and the screen stayed
         * black. STAGING is the canonical CPU-write-then-copy path. */
        /* DEFAULT, filled with UpdateSubresource, no longer a
         * STAGING texture mapped for writing. The map waited for the GPU to be
         * done with the copy queued from the same texture one present
         * earlier, and on some drivers (Intel UHD, measured) that wait
         * sometimes lasted ~450 ms. This thread is the one that acknowledges
         * the title's GPU fences, so the title's D3D BlockOnTime (under the
         * video player's LockRect) stalled with it, the video loop read no
         * input for ~0.5 s and START pressed in that window was lost (input
         * ring of 8 samples). UpdateSubresource does not wait: the runtime
         * stages the data when the texture is still in use. A DEFAULT
         * texture is a legal copy source, so the copies below are unchanged. */
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = 0;
        td.CPUAccessFlags = 0;
        conv = (unsigned char *)malloc((size_t)w * h * 4u);
        if (!conv) return;
        if (FAILED(ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
                                                &td, NULL, &stage)) || !stage) {
            free(conv);
            conv = NULL;
            return;
        }
        stage_w = w; stage_h = h;
    }

    if (src) {
    for (y = 0; y < h; y++) {
        const unsigned char *s = (const unsigned char *)src + (size_t)y * pitch;
        unsigned char *d = conv + (size_t)y * w * 4u;
        for (x = 0; x < w; x++) {
            d[x * 4 + 0] = s[x * 4 + 2];   /* R <- guest R */
            d[x * 4 + 1] = s[x * 4 + 1];   /* G */
            d[x * 4 + 2] = s[x * 4 + 0];   /* B <- guest B */
            d[x * 4 + 3] = 0xFF;           /* opaque: the video has no alpha */
        }
    }
    ID3D11DeviceContext_UpdateSubresource(g_device_state.d3d11_context,
                                          (ID3D11Resource *)stage, 0, NULL,
                                          conv, w * 4u, 0);
    }

    /* Into the scene target. At the title's own size that is the
     * same 1:1 copy as always; a larger scene gets the frame scaled up through
     * a sampleable copy, since a staging texture cannot be sampled. */
    if (SUCCEEDED(scene_texture_get(&back)) && back) {
        if (w == g_device_state.width && h == g_device_state.height &&
            !g_device_state.scene_ms) {
            D3D11_BOX box;
            box.left = 0; box.top = 0; box.front = 0;
            box.right = w; box.bottom = h; box.back = 1;
            ID3D11DeviceContext_CopySubresourceRegion(g_device_state.d3d11_context,
                (ID3D11Resource *)back, 0, 0, 0, 0,
                (ID3D11Resource *)stage, 0, &box);
        } else {
            static ID3D11Texture2D *vtex = NULL;
            static ID3D11ShaderResourceView *vsrv = NULL;
            static unsigned vw = 0, vh = 0;
            if (vtex && (vw != w || vh != h)) {
                ID3D11ShaderResourceView_Release(vsrv); vsrv = NULL;
                ID3D11Texture2D_Release(vtex); vtex = NULL;
            }
            if (!vtex) {
                D3D11_TEXTURE2D_DESC td;
                memset(&td, 0, sizeof td);
                td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
                        &td, NULL, &vtex)) && vtex
                    && FAILED(ID3D11Device_CreateShaderResourceView(g_device_state.d3d11_device,
                        (ID3D11Resource *)vtex, NULL, &vsrv))) {
                    ID3D11Texture2D_Release(vtex); vtex = NULL;
                }
                vw = w; vh = h;
            }
            if (vtex && vsrv) {
                D3D11_VIEWPORT vp;
                ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
                    (ID3D11Resource *)vtex, (ID3D11Resource *)stage);
                vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
                vp.Width  = (FLOAT)g_device_state.width;
                vp.Height = (FLOAT)g_device_state.height;
                vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
                blit_draw(vsrv, g_device_state.default_rtv, &vp);
            }
        }
        ID3D11Texture2D_Release(back);

        /* One line, once: a silent no-op here is indistinguishable from the
         * feature not being wired at all. Report the first copy and how much
         * of the source was actually non-black, so "it ran" and "it had
         * something to show" stay separate questions. */
        if (src) {
            static int told = 0;
            static unsigned calls = 0;
            unsigned nz = 0, sy, sx;
            calls++;
            for (sy = 0; sy < h; sy += 8) {
                const unsigned char *s =
                    (const unsigned char *)src + (size_t)sy * pitch;
                for (sx = 0; sx < w; sx += 8)
                    if (s[sx * 4] | s[sx * 4 + 1] | s[sx * 4 + 2]) nz++;
            }
            /* Report the first frame that actually carries pixels, not the
             * first call: the earliest presents happen long before the video
             * starts blitting, so a one-shot report only ever says "black". */
            if (!told && nz) {
                unsigned dst_nz = 0;
                ID3D11Texture2D *rb = NULL;
                D3D11_TEXTURE2D_DESC rd;
                D3D11_MAPPED_SUBRESOURCE rm;
                told = 1;
                /* Count non-black pixels in the DESTINATION. A hash proves
                 * nothing here -- FNV-1a of an all-zero buffer is still a
                 * non-zero number, which is how the previous check reported
                 * success over a copy that had done nothing. */
                ID3D11Texture2D_GetDesc(back, &rd);
                rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0;
                rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; rd.MiscFlags = 0;
                if (SUCCEEDED(ID3D11Device_CreateTexture2D(
                        g_device_state.d3d11_device, &rd, NULL, &rb)) && rb) {
                    ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
                        (ID3D11Resource *)rb, (ID3D11Resource *)back);
                    if (SUCCEEDED(ID3D11DeviceContext_Map(
                            g_device_state.d3d11_context, (ID3D11Resource *)rb,
                            0, D3D11_MAP_READ, 0, &rm))) {
                        unsigned ry, rx;
                        for (ry = 0; ry < rd.Height; ry += 8) {
                            const unsigned char *r =
                                (const unsigned char *)rm.pData + (size_t)ry * rm.RowPitch;
                            for (rx = 0; rx < rd.Width; rx += 8)
                                if (r[rx*4] | r[rx*4+1] | r[rx*4+2]) dst_nz++;
                        }
                        ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                                                  (ID3D11Resource *)rb, 0);
                    }
                    ID3D11Texture2D_Release(rb);
                }
                fprintf(stderr, "  [FB] call %u: source %u/%u non-black, "
                        "destination %u non-black after copy (%ux%u pitch %u)\n",
                        calls, nz, (h / 8) * (w / 8), dst_nz, w, h, pitch);
                /* Raw source bytes from a row inside the picture. The green
                 * channel reads zero in the finished image, and guessing at the
                 * pixel format from the finished image is how that gets
                 * misdiagnosed -- look at what the guest actually wrote. */
                {
                    const unsigned char *r0 =
                        (const unsigned char *)src + (size_t)(h / 2) * pitch;
                    unsigned k;
                    fprintf(stderr, "  [FB] source row %u bytes:", h / 2);
                    for (k = 0; k < 32; k++)
                        fprintf(stderr, " %02X", r0[(w / 2) * 4 + k]);
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
        }
    }
}

/* True once the video player has registered a guest framebuffer, so the
 * present pacing can follow the video instead of the 500 ms safety net. */
int d3d8_HasGuestFramebuffer(void)
{
    return g_guest_fb_src != NULL;
}

/* True while the video player is still locking the guest framebuffer: its
 * frames reach the screen through the presents at its flips, and the
 * presents in between hold the frame shown last. */
int d3d8_GuestFramebufferActive(void)
{
    return g_guest_fb_src != NULL && g_guest_fb_locks != 0
        && g_present_seq - g_guest_fb_lock_seq <= GUEST_FB_IDLE_PRESENTS;
}
