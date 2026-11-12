/**
 * D3D8 Render State → D3D11 State Object Translation
 *
 * Converts D3D8 render state values into D3D11 state objects:
 *   - Blend state (alpha blending, color write mask)
 *   - Depth-stencil state (z-test, z-write, stencil)
 *   - Rasterizer state (cull mode, fill mode)
 *   - Sampler state (texture filtering, addressing)
 *
 * State objects are cached and recreated only when dirty.
 */

#include "d3d8_internal.h"
#include "d3d8_temporal.h"
#include "../kernel/xbox_perf.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ================================================================
 * Cached D3D11 state objects
 * ================================================================ */

static ID3D11BlendState        *g_blend_state = NULL;
static ID3D11DepthStencilState *g_ds_state = NULL;
static ID3D11RasterizerState   *g_raster_state = NULL;
static ID3D11SamplerState      *g_sampler_states[4] = { NULL, NULL, NULL, NULL };

/* The render states each object above was last built from (its key), for
 * dirty detection: see states_key. */
#define BLEND_KEY  5
#define DS_KEY     10
#define RASTER_KEY 3
static DWORD g_last_blend_key[BLEND_KEY];
static DWORD g_last_ds_key[DS_KEY];
static DWORD g_last_raster_key[RASTER_KEY];

/* NV2A window clip as a scissor rectangle. The frontend draws with
 * the window clip set to the TV-safe area (30..609 x 22..457) and clears with
 * it set to the whole surface; the translator reports a single inclusive
 * rectangle here, and every draw that goes through d3d8_states_apply (the
 * 2D path and the program path) is scissored to it. Clears and the video
 * framebuffer copy are not draws, so they are unaffected, as on hardware. */
static int        g_wclip_on;
static D3D11_RECT g_wclip;

void d3d8_SetWindowClip(int on, long x0, long y0, long x1, long y1)
{
    g_wclip_on = on;
    g_wclip.left = x0; g_wclip.top = y0; g_wclip.right = x1; g_wclip.bottom = y1;
}

int d3d8_WindowClipOn(void) { return g_wclip_on; }

/* ================================================================
 * D3D8 → D3D11 enum translation
 * ================================================================ */

static D3D11_BLEND d3d8_to_d3d11_blend(DWORD d3d8blend)
{
    switch (d3d8blend) {
    case D3DBLEND_ZERO:         return D3D11_BLEND_ZERO;
    case D3DBLEND_ONE:          return D3D11_BLEND_ONE;
    case D3DBLEND_SRCCOLOR:     return D3D11_BLEND_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:  return D3D11_BLEND_INV_SRC_COLOR;
    case D3DBLEND_SRCALPHA:     return D3D11_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:    return D3D11_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3DBLEND_DESTCOLOR:    return D3D11_BLEND_DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return D3D11_BLEND_INV_DEST_COLOR;
    case D3DBLEND_SRCALPHASAT:  return D3D11_BLEND_SRC_ALPHA_SAT;
    default:                    return D3D11_BLEND_ONE;
    }
}

static D3D11_COMPARISON_FUNC d3d8_to_d3d11_cmp(DWORD d3d8cmp)
{
    switch (d3d8cmp) {
    case D3DCMP_NEVER:        return D3D11_COMPARISON_NEVER;
    case D3DCMP_LESS:         return D3D11_COMPARISON_LESS;
    case D3DCMP_EQUAL:        return D3D11_COMPARISON_EQUAL;
    case D3DCMP_LESSEQUAL:    return D3D11_COMPARISON_LESS_EQUAL;
    case D3DCMP_GREATER:      return D3D11_COMPARISON_GREATER;
    case D3DCMP_NOTEQUAL:     return D3D11_COMPARISON_NOT_EQUAL;
    case D3DCMP_GREATEREQUAL: return D3D11_COMPARISON_GREATER_EQUAL;
    case D3DCMP_ALWAYS:       return D3D11_COMPARISON_ALWAYS;
    default:                  return D3D11_COMPARISON_LESS_EQUAL;
    }
}

static D3D11_STENCIL_OP d3d8_to_d3d11_stencilop(DWORD op)
{
    switch (op) {
    case 1: return D3D11_STENCIL_OP_KEEP;
    case 2: return D3D11_STENCIL_OP_ZERO;
    case 3: return D3D11_STENCIL_OP_REPLACE;
    case 4: return D3D11_STENCIL_OP_INCR_SAT;
    case 5: return D3D11_STENCIL_OP_DECR_SAT;
    case 6: return D3D11_STENCIL_OP_INVERT;
    case 7: return D3D11_STENCIL_OP_INCR;
    case 8: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

/* the alpha-channel counterpart of a blend factor (D3D11 allows no colour factor there) */
static D3D11_BLEND blend_alpha_factor(D3D11_BLEND b)
{
    switch (b) {
    case D3D11_BLEND_SRC_COLOR:      return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR:     return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default:                         return b;
    }
}

static D3D11_BLEND_OP d3d8_to_d3d11_blendop(DWORD op)
{
    switch (op) {
    case 1: return D3D11_BLEND_OP_ADD;
    case 2: return D3D11_BLEND_OP_SUBTRACT;
    case 3: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 4: return D3D11_BLEND_OP_MIN;
    case 5: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

/* The key of a D3D11 state object: every render state it is built from, in
 * a fixed order, kept and compared whole for dirty detection. The
 * old XORs of shifted values collided (and the depth-stencil one left out
 * the stencil ops and write mask, so a change of op alone reused the old
 * state object); the FNV-1a hash that replaced them was still trusted alone,
 * and 1 332 pairs of depth / stencil combinations share one (the second of
 * two such draws in a row would have kept the first one's object). */
static const int k_blend_rs[BLEND_KEY] = {
    D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_COLORWRITEENABLE };
static const int k_ds_rs[DS_KEY] = {
    D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC,
    D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILMASK,
    D3DRS_STENCILWRITEMASK, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS };
static const int k_raster_rs[RASTER_KEY - 1] = { D3DRS_CULLMODE, D3DRS_FILLMODE };  /* + the window clip */

static void states_key(const DWORD *rs, const int *idx, int n, DWORD *key)
{
    int i;
    for (i = 0; i < n; i++) key[i] = rs[idx[i]];
}

/* ================================================================
 * State object creation
 * ================================================================ */

static void update_blend_state(const DWORD *rs)
{
    DWORD key[BLEND_KEY];
    D3D11_BLEND_DESC bd;
    HRESULT hr;

    states_key(rs, k_blend_rs, BLEND_KEY, key);
    if (g_blend_state && !memcmp(key, g_last_blend_key, sizeof key)) return;
    memcpy(g_last_blend_key, key, sizeof key);

    if (g_blend_state) {
        ID3D11BlendState_Release(g_blend_state);
        g_blend_state = NULL;
    }

    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable = rs[D3DRS_ALPHABLENDENABLE] ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = d3d8_to_d3d11_blend(rs[D3DRS_SRCBLEND]);
    bd.RenderTarget[0].DestBlend = d3d8_to_d3d11_blend(rs[D3DRS_DESTBLEND]);
    bd.RenderTarget[0].BlendOp = d3d8_to_d3d11_blendop(rs[D3DRS_BLENDOP] ? rs[D3DRS_BLENDOP] : 1);
    /* D3D11 refuses colour factors on the alpha channel (CreateBlendState
     * fails, and the draw went out unblended): their alpha counterparts there */
    bd.RenderTarget[0].SrcBlendAlpha = blend_alpha_factor(bd.RenderTarget[0].SrcBlend);
    bd.RenderTarget[0].DestBlendAlpha = blend_alpha_factor(bd.RenderTarget[0].DestBlend);
    bd.RenderTarget[0].BlendOpAlpha = bd.RenderTarget[0].BlendOp;
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)(rs[D3DRS_COLORWRITEENABLE] & 0x0F);

    if (g_perf_on) perf_count(PC_SOBJ, 1);
    hr = ID3D11Device_CreateBlendState(d3d8_GetD3D11Device(), &bd, &g_blend_state);
    if (FAILED(hr))
        fprintf(stderr, "D3D8: CreateBlendState failed: 0x%08lX\n", hr);
}

static void update_depth_stencil_state(const DWORD *rs)
{
    DWORD key[DS_KEY];
    D3D11_DEPTH_STENCIL_DESC dsd;
    HRESULT hr;

    states_key(rs, k_ds_rs, DS_KEY, key);
    if (g_ds_state && !memcmp(key, g_last_ds_key, sizeof key)) return;
    memcpy(g_last_ds_key, key, sizeof key);

    if (g_ds_state) {
        ID3D11DepthStencilState_Release(g_ds_state);
        g_ds_state = NULL;
    }

    memset(&dsd, 0, sizeof(dsd));
    dsd.DepthEnable = rs[D3DRS_ZENABLE] ? TRUE : FALSE;
    dsd.DepthWriteMask = rs[D3DRS_ZWRITEENABLE] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = d3d8_to_d3d11_cmp(rs[D3DRS_ZFUNC]);

    dsd.StencilEnable = rs[D3DRS_STENCILENABLE] ? TRUE : FALSE;
    dsd.StencilReadMask = (UINT8)(rs[D3DRS_STENCILMASK] & 0xFF);
    dsd.StencilWriteMask = (UINT8)(rs[D3DRS_STENCILWRITEMASK] & 0xFF);

    dsd.FrontFace.StencilFunc = d3d8_to_d3d11_cmp(rs[D3DRS_STENCILFUNC]);
    dsd.FrontFace.StencilFailOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILFAIL]);
    dsd.FrontFace.StencilDepthFailOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILZFAIL]);
    dsd.FrontFace.StencilPassOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILPASS]);
    dsd.BackFace = dsd.FrontFace;

    if (g_perf_on) perf_count(PC_SOBJ, 1);
    hr = ID3D11Device_CreateDepthStencilState(d3d8_GetD3D11Device(), &dsd, &g_ds_state);
    if (FAILED(hr))
        fprintf(stderr, "D3D8: CreateDepthStencilState failed: 0x%08lX\n", hr);
}

static void update_rasterizer_state(const DWORD *rs)
{
    DWORD key[RASTER_KEY];
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    states_key(rs, k_raster_rs, RASTER_KEY - 1, key);
    key[RASTER_KEY - 1] = g_wclip_on ? 1u : 0u;
    if (g_raster_state && !memcmp(key, g_last_raster_key, sizeof key)) return;
    memcpy(g_last_raster_key, key, sizeof key);

    if (g_raster_state) {
        ID3D11RasterizerState_Release(g_raster_state);
        g_raster_state = NULL;
    }

    memset(&rd, 0, sizeof(rd));

    switch (rs[D3DRS_FILLMODE]) {
    case D3DFILL_POINT:     rd.FillMode = D3D11_FILL_WIREFRAME; break;  /* D3D11 has no point fill */
    case D3DFILL_WIREFRAME: rd.FillMode = D3D11_FILL_WIREFRAME; break;
    default:                rd.FillMode = D3D11_FILL_SOLID; break;
    }

    switch (rs[D3DRS_CULLMODE]) {
    case D3DCULL_NONE: rd.CullMode = D3D11_CULL_NONE; break;
    case D3DCULL_CW:   rd.CullMode = D3D11_CULL_FRONT; break;  /* D3D8 CW = cull front in D3D11 convention */
    case D3DCULL_CCW:  rd.CullMode = D3D11_CULL_BACK; break;
    default:           rd.CullMode = D3D11_CULL_BACK; break;
    }

    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = g_wclip_on ? TRUE : FALSE;
    rd.MultisampleEnable = FALSE;
    rd.AntialiasedLineEnable = FALSE;

    if (g_perf_on) perf_count(PC_SOBJ, 1);
    hr = ID3D11Device_CreateRasterizerState(d3d8_GetD3D11Device(), &rd, &g_raster_state);
    if (FAILED(hr))
        fprintf(stderr, "D3D8: CreateRasterizerState failed: 0x%08lX\n", hr);
}

/* ================================================================
 * Sampler state
 * ================================================================ */

static D3D11_TEXTURE_ADDRESS_MODE d3d8_to_d3d11_address(DWORD mode)
{
    switch (mode) {
    case D3DTADDRESS_WRAP:       return D3D11_TEXTURE_ADDRESS_WRAP;
    case D3DTADDRESS_MIRROR:     return D3D11_TEXTURE_ADDRESS_MIRROR;
    case D3DTADDRESS_CLAMP:      return D3D11_TEXTURE_ADDRESS_CLAMP;
    case D3DTADDRESS_BORDER:     return D3D11_TEXTURE_ADDRESS_BORDER;
    case D3DTADDRESS_MIRRORONCE: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default:                     return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static D3D11_FILTER d3d8_to_d3d11_filter(DWORD mag, DWORD min, DWORD mip)
{
    BOOL mag_linear = (mag == D3DTEXF_LINEAR || mag == D3DTEXF_ANISOTROPIC);
    BOOL min_linear = (min == D3DTEXF_LINEAR || min == D3DTEXF_ANISOTROPIC);
    BOOL mip_linear = (mip == D3DTEXF_LINEAR);

    if (mag == D3DTEXF_ANISOTROPIC || min == D3DTEXF_ANISOTROPIC)
        return D3D11_FILTER_ANISOTROPIC;
    /* D3D11_FILTER spells the three choices as bits: min 0x10, mag 0x04,
     * mip 0x01 (linear when set). The old table had five of the eight, and
     * none with a linear mip, which only mattered once mipmaps existed. */
    return (D3D11_FILTER)((min_linear ? 0x10 : 0) | (mag_linear ? 0x04 : 0) |
                          (mip_linear ? 0x01 : 0));
}

/* Anisotropic filtering chosen by the player: 0 = the title's own
 * filtering; 2..16 = anisotropic for every texture the title filters
 * linearly. Point-sampled textures (pixel fonts) are left alone. */
static int g_host_aniso = 0;
static int s_tmp_wanted = -1;           /* fork: XBOX_TEMPORAL, read once */

void d3d8_SetAnisotropy(int n)
{
    g_host_aniso = (n == 2 || n == 4 || n == 8 || n == 16) ? n : 0;
}

int d3d8_GetAnisotropy(void) { return g_host_aniso; }

void d3d8_states_apply_sampler(DWORD stage)
{
    const DWORD *tss;
    D3D11_SAMPLER_DESC sd;
    HRESULT hr;
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    union { DWORD u; float f; } bias;
    UINT aniso;

    if (stage >= 4) return;
    tss = d3d8_GetTSS(stage);
    if (!tss) return;

    /* Release old sampler */
    if (g_sampler_states[stage]) {
        ID3D11SamplerState_Release(g_sampler_states[stage]);
        g_sampler_states[stage] = NULL;
    }

    memset(&sd, 0, sizeof(sd));
    sd.Filter = d3d8_to_d3d11_filter(
        tss[D3DTSS_MAGFILTER],
        tss[D3DTSS_MINFILTER],
        tss[D3DTSS_MIPFILTER]);
    sd.AddressU = d3d8_to_d3d11_address(tss[D3DTSS_ADDRESSU] ? tss[D3DTSS_ADDRESSU] : D3DTADDRESS_WRAP);
    sd.AddressV = d3d8_to_d3d11_address(tss[D3DTSS_ADDRESSV] ? tss[D3DTSS_ADDRESSV] : D3DTADDRESS_WRAP);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    aniso = tss[D3DTSS_MAXANISOTROPY] ? tss[D3DTSS_MAXANISOTROPY] : 1;
    if (aniso > 16) aniso = 16;
    /* Anisotropic when the title asks for more than 1x (NV2A CONTROL0, as
     * xemu applies it) or the player chose it, for linearly filtered
     * textures; the larger of the two wins. */
    if ((g_host_aniso > 1 || aniso > 1) && tss[D3DTSS_MINFILTER] != D3DTEXF_POINT &&
        tss[D3DTSS_MINFILTER] != D3DTEXF_NONE) {
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        if ((UINT)g_host_aniso > aniso) aniso = (UINT)g_host_aniso;
    }
    sd.MaxAnisotropy = aniso;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    /* Mipmaps: the most detailed level allowed, the bias, and --
     * with no mip filter -- the base level only. */
    bias.u = tss[D3DTSS_MIPMAPLODBIAS];
    sd.MipLODBias = (bias.f == bias.f && bias.f > -16.0f && bias.f < 16.0f) ? bias.f : 0.0f;
    /* Fork: the temporal jitter's negative bias (d3d8_temporal.h), on the 3D
     * of a race only; 0 when off. */
    if (s_tmp_wanted < 0) s_tmp_wanted = d3d8_temporal_wanted();
    if (s_tmp_wanted) sd.MipLODBias += d3d8_temporal_mip_bias();
    sd.MinLOD = (FLOAT)tss[D3DTSS_MAXMIPLEVEL];
    sd.MaxLOD = (tss[D3DTSS_MIPFILTER] == D3DTEXF_NONE) ? sd.MinLOD : D3D11_FLOAT32_MAX;

    /* Sampler objects, kept by description: creating one per
     * stage per draw cost a trip into the runtime every time, even though it
     * hands back the same object for the same description. */
    {
        #define SAMPLER_CACHE 256
        static struct { D3D11_SAMPLER_DESC d; ID3D11SamplerState *s; } cache[SAMPLER_CACHE];
        static int ncache = 0;
        int k;
        for (k = 0; k < ncache; k++)
            if (!memcmp(&cache[k].d, &sd, sizeof sd)) break;
        if (g_perf_on) perf_count(PC_SMPL, (unsigned)k + 1u);   /* compares */
        if (k == ncache) {
            ID3D11SamplerState *ss = NULL;
            if (g_perf_on) perf_count(PC_SOBJ, 1);
            hr = ID3D11Device_CreateSamplerState(d3d8_GetD3D11Device(), &sd, &ss);
            if (FAILED(hr) || !ss) return;
            if (ncache < SAMPLER_CACHE) { cache[k].d = sd; cache[k].s = ss; ncache++; }
            else { g_sampler_states[stage] = ss; ID3D11DeviceContext_PSSetSamplers(ctx, stage, 1, &ss); return; }
        }
        g_sampler_states[stage] = cache[k].s;
        ID3D11SamplerState_AddRef(cache[k].s);   /* released on the next apply, as before */
        ID3D11DeviceContext_PSSetSamplers(ctx, stage, 1, &g_sampler_states[stage]);
    }
}

/* ================================================================
 * XBOX_FIX_PUMP_SOBJ, default 1; 0 = the path above: state objects
 * from tables keyed by every render / stage state they are built from, and
 * compared on all of it -- the one-entry caches above release and recreate
 * an object whenever two draws in a row differ (a trip into the runtime,
 * which then hands back the object it already had). Same descriptions, same
 * objects as D3D11 would return; the tables own their objects for the life
 * of the device. Off on the frames
 * XBOX_FIX_PUMP_ALT turns the pump optimizations off.
 * ================================================================ */

extern int d3d8_pump_alt_off(void);
#include "d3d8_pumpflags.h"

static int pump_sobj_on(void) { return (d3d8_pump_flags() & PF_SOBJ) != 0; }   /* d3d8_pumpflags.h */

#define SO_TAB 256                      /* per kind; far more than the title uses */
#define SO_KEY 14                       /* DWORDs of a key (the sampler description is 13) */
typedef struct { DWORD key[SO_KEY]; unsigned n; IUnknown *obj; } SoEnt;
static SoEnt g_so[4][SO_TAB];           /* blend, depth-stencil, (rasteriser: unused), sampler */

static unsigned so_hash(const DWORD *k, unsigned n)
{
    unsigned h = 2166136261u, i;
    for (i = 0; i < n; i++) h = (h ^ k[i]) * 16777619u;
    return h ^ (h >> 15);
}

/* The entry of key k (n DWORDs) in table t: found, or the free slot it goes
 * to (obj NULL), or NULL when the table is full. */
static SoEnt *so_find(int t, const DWORD *k, unsigned n)
{
    unsigned i = so_hash(k, n) & (SO_TAB - 1u), step;
    for (step = 0; step < SO_TAB; step++, i = (i + 1u) & (SO_TAB - 1u)) {
        SoEnt *e = &g_so[t][i];
        if (!e->obj) return e;
        if (e->n == n && !memcmp(e->key, k, n * sizeof *k)) return e;
    }
    return NULL;
}

/* A new object: into its free slot, or, the table full, kept alive until
 * the next one of its kind (as bound until then). */
static void so_put(int t, SoEnt *e, const DWORD *k, unsigned n, IUnknown *obj)
{
    static IUnknown *spare[4];
    if (!e) {
        if (spare[t]) IUnknown_Release(spare[t]);
        spare[t] = obj;
        return;
    }
    memcpy(e->key, k, n * sizeof *k);
    e->n = n;
    e->obj = obj;
}

static ID3D11BlendState *so_blend(const DWORD *rs)
{
    DWORD k[5];
    SoEnt *e;
    D3D11_BLEND_DESC bd;
    ID3D11BlendState *b = NULL;
    k[0] = rs[D3DRS_ALPHABLENDENABLE]; k[1] = rs[D3DRS_SRCBLEND]; k[2] = rs[D3DRS_DESTBLEND];
    k[3] = rs[D3DRS_BLENDOP]; k[4] = rs[D3DRS_COLORWRITEENABLE];
    e = so_find(0, k, 5);
    if (e && e->obj) return (ID3D11BlendState *)e->obj;
    memset(&bd, 0, sizeof(bd));         /* as update_blend_state */
    bd.RenderTarget[0].BlendEnable = rs[D3DRS_ALPHABLENDENABLE] ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = d3d8_to_d3d11_blend(rs[D3DRS_SRCBLEND]);
    bd.RenderTarget[0].DestBlend = d3d8_to_d3d11_blend(rs[D3DRS_DESTBLEND]);
    bd.RenderTarget[0].BlendOp = d3d8_to_d3d11_blendop(rs[D3DRS_BLENDOP] ? rs[D3DRS_BLENDOP] : 1);
    bd.RenderTarget[0].SrcBlendAlpha = blend_alpha_factor(bd.RenderTarget[0].SrcBlend);
    bd.RenderTarget[0].DestBlendAlpha = blend_alpha_factor(bd.RenderTarget[0].DestBlend);
    bd.RenderTarget[0].BlendOpAlpha = bd.RenderTarget[0].BlendOp;
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)(rs[D3DRS_COLORWRITEENABLE] & 0x0F);
    if (g_perf_on) perf_count(PC_SOBJ, 1);
    if (FAILED(ID3D11Device_CreateBlendState(d3d8_GetD3D11Device(), &bd, &b)) || !b) {
        fprintf(stderr, "D3D8: CreateBlendState failed\n");
        return NULL;
    }
    so_put(0, e, k, 5, (IUnknown *)b);
    return b;
}

static ID3D11DepthStencilState *so_ds(const DWORD *rs)
{
    DWORD k[10];
    SoEnt *e;
    D3D11_DEPTH_STENCIL_DESC dsd;
    ID3D11DepthStencilState *d = NULL;
    k[0] = rs[D3DRS_ZENABLE]; k[1] = rs[D3DRS_ZWRITEENABLE]; k[2] = rs[D3DRS_ZFUNC];
    k[3] = rs[D3DRS_STENCILENABLE]; k[4] = rs[D3DRS_STENCILFUNC]; k[5] = rs[D3DRS_STENCILMASK];
    k[6] = rs[D3DRS_STENCILWRITEMASK]; k[7] = rs[D3DRS_STENCILFAIL]; k[8] = rs[D3DRS_STENCILZFAIL];
    k[9] = rs[D3DRS_STENCILPASS];
    e = so_find(1, k, 10);
    if (e && e->obj) return (ID3D11DepthStencilState *)e->obj;
    memset(&dsd, 0, sizeof(dsd));       /* as update_depth_stencil_state */
    dsd.DepthEnable = rs[D3DRS_ZENABLE] ? TRUE : FALSE;
    dsd.DepthWriteMask = rs[D3DRS_ZWRITEENABLE] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = d3d8_to_d3d11_cmp(rs[D3DRS_ZFUNC]);
    dsd.StencilEnable = rs[D3DRS_STENCILENABLE] ? TRUE : FALSE;
    dsd.StencilReadMask = (UINT8)(rs[D3DRS_STENCILMASK] & 0xFF);
    dsd.StencilWriteMask = (UINT8)(rs[D3DRS_STENCILWRITEMASK] & 0xFF);
    dsd.FrontFace.StencilFunc = d3d8_to_d3d11_cmp(rs[D3DRS_STENCILFUNC]);
    dsd.FrontFace.StencilFailOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILFAIL]);
    dsd.FrontFace.StencilDepthFailOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILZFAIL]);
    dsd.FrontFace.StencilPassOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILPASS]);
    dsd.BackFace = dsd.FrontFace;
    if (g_perf_on) perf_count(PC_SOBJ, 1);
    if (FAILED(ID3D11Device_CreateDepthStencilState(d3d8_GetD3D11Device(), &dsd, &d)) || !d) {
        fprintf(stderr, "D3D8: CreateDepthStencilState failed\n");
        return NULL;
    }
    so_put(1, e, k, 10, (IUnknown *)d);
    return d;
}

/* The sampler of a stage: the description d3d8_states_apply_sampler builds,
 * the object from the table (that description is its key). */
/* (XBOX_FIX_PUMP_SMEMO, default 1; 0 = every description built and
 * looked up as before): per stage, the stage states the sampler description
 * is made of (filters, address modes, anisotropy, LOD bias, most-detailed
 * level), the host anisotropy and the temporal LOD bias, with the object they
 * gave last time. Same inputs, same description (so_sampler below is a
 * function of these alone), same object: bound again without the
 * description, its hash and its lookup. Only an object held by g_so (not the
 * spare of a full table) is remembered; so_release_all forgets them. Off on
 * the frames XBOX_FIX_PUMP_ALT turns the pump optimizations off. */
static const D3DTEXTURESTAGESTATETYPE k_smemo_tss[8] = {
    D3DTSS_MAGFILTER, D3DTSS_MINFILTER, D3DTSS_MIPFILTER, D3DTSS_ADDRESSU,
    D3DTSS_ADDRESSV, D3DTSS_MAXANISOTROPY, D3DTSS_MIPMAPLODBIAS, D3DTSS_MAXMIPLEVEL,
};
static struct { DWORD in[10]; ID3D11SamplerState *ss; } s_smemo[4];

static int pump_smemo_on(void) { return (d3d8_pump_flags() & PF_SMEMO) != 0; }

/* ctx: the caller's context; memo: pump_smemo_on(), decided by the caller
 * once per draw (XBOX_FIX_PUMP_ALT changes only from one frame to the next). */
static void so_sampler(DWORD stage, ID3D11DeviceContext *ctx, int memo)
{
    const DWORD *tss = d3d8_GetTSS(stage);
    D3D11_SAMPLER_DESC sd;
    union { DWORD u; float f; } bias, tb;
    UINT aniso;
    DWORD k[(sizeof(D3D11_SAMPLER_DESC) + 3) / 4];
    SoEnt *e;
    ID3D11SamplerState *ss = NULL;
    DWORD in[10];
    int i;
    if (stage >= 4 || !tss) return;
    if (s_tmp_wanted < 0) s_tmp_wanted = d3d8_temporal_wanted();
    tb.f = s_tmp_wanted ? d3d8_temporal_mip_bias() : 0.0f;   /* read once: the memo's key and the description */
    if (memo) {
        for (i = 0; i < 8; i++) in[i] = tss[k_smemo_tss[i]];
        in[8] = (DWORD)g_host_aniso;
        in[9] = tb.u;
        if (s_smemo[stage].ss && !memcmp(s_smemo[stage].in, in, sizeof in)) {
            if (g_perf_on) { perf_count(PC_SMPL, 1); if (g_perf_detail) perf_count(PC_SMEMO, 1); }   /* PC_SMPL as the lookup counts it; the hit itself: XBOX_PERF_DETAIL only */
            ID3D11DeviceContext_PSSetSamplers(ctx, stage, 1, &s_smemo[stage].ss);
            return;
        }
    }
    memset(&sd, 0, sizeof(sd));         /* as d3d8_states_apply_sampler */
    sd.Filter = d3d8_to_d3d11_filter(tss[D3DTSS_MAGFILTER], tss[D3DTSS_MINFILTER], tss[D3DTSS_MIPFILTER]);
    sd.AddressU = d3d8_to_d3d11_address(tss[D3DTSS_ADDRESSU] ? tss[D3DTSS_ADDRESSU] : D3DTADDRESS_WRAP);
    sd.AddressV = d3d8_to_d3d11_address(tss[D3DTSS_ADDRESSV] ? tss[D3DTSS_ADDRESSV] : D3DTADDRESS_WRAP);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    aniso = tss[D3DTSS_MAXANISOTROPY] ? tss[D3DTSS_MAXANISOTROPY] : 1;
    if (aniso > 16) aniso = 16;
    if ((g_host_aniso > 1 || aniso > 1) && tss[D3DTSS_MINFILTER] != D3DTEXF_POINT &&
        tss[D3DTSS_MINFILTER] != D3DTEXF_NONE) {
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        if ((UINT)g_host_aniso > aniso) aniso = (UINT)g_host_aniso;
    }
    sd.MaxAnisotropy = aniso;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    bias.u = tss[D3DTSS_MIPMAPLODBIAS];
    sd.MipLODBias = (bias.f == bias.f && bias.f > -16.0f && bias.f < 16.0f) ? bias.f : 0.0f;
    if (s_tmp_wanted) sd.MipLODBias += tb.f;
    sd.MinLOD = (FLOAT)tss[D3DTSS_MAXMIPLEVEL];
    sd.MaxLOD = (tss[D3DTSS_MIPFILTER] == D3DTEXF_NONE) ? sd.MinLOD : D3D11_FLOAT32_MAX;
    memset(k, 0, sizeof k);
    memcpy(k, &sd, sizeof sd);
    e = so_find(3, k, (unsigned)(sizeof k / sizeof k[0]));
    if (g_perf_on) perf_count(PC_SMPL, 1);
    if (e && e->obj) ss = (ID3D11SamplerState *)e->obj;
    else {
        if (g_perf_on) perf_count(PC_SOBJ, 1);
        if (FAILED(ID3D11Device_CreateSamplerState(d3d8_GetD3D11Device(), &sd, &ss)) || !ss) return;
        so_put(3, e, k, (unsigned)(sizeof k / sizeof k[0]), (IUnknown *)ss);
    }
    if (memo) {                         /* held by g_so for the device's life, or not kept */
        memcpy(s_smemo[stage].in, in, sizeof in);
        s_smemo[stage].ss = e ? ss : NULL;
    }
    ID3D11DeviceContext_PSSetSamplers(ctx, stage, 1, &ss);
}

static void so_release_all(void)
{
    int t, i;
    for (t = 0; t < 4; t++) {
        for (i = 0; i < SO_TAB; i++)
            if (g_so[t][i].obj) IUnknown_Release(g_so[t][i].obj);
        memset(g_so[t], 0, sizeof g_so[t]);
    }
    memset(s_smemo, 0, sizeof s_smemo);
}

/* ================================================================
 * Apply all states before draw call
 * ================================================================ */

HRESULT d3d8_states_init(void)
{
    /* States are created on first apply */
    return S_OK;
}

void d3d8_states_shutdown(void)
{
    int i;
    so_release_all();
    if (g_blend_state)  { ID3D11BlendState_Release(g_blend_state); g_blend_state = NULL; }
    if (g_ds_state)     { ID3D11DepthStencilState_Release(g_ds_state); g_ds_state = NULL; }
    if (g_raster_state) { ID3D11RasterizerState_Release(g_raster_state); g_raster_state = NULL; }
    for (i = 0; i < 4; i++) {
        if (g_sampler_states[i]) {
            ID3D11SamplerState_Release(g_sampler_states[i]);
            g_sampler_states[i] = NULL;
        }
    }
    memset(g_last_blend_key, 0, sizeof g_last_blend_key);
    memset(g_last_ds_key, 0, sizeof g_last_ds_key);
    memset(g_last_raster_key, 0, sizeof g_last_raster_key);
}

void d3d8_states_apply(void)
{
    const DWORD *rs = d3d8_GetRenderStates();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    float blend_factor[4] = { 1, 1, 1, 1 };

    if (!rs || !ctx) return;

    update_blend_state(rs);
    update_depth_stencil_state(rs);
    update_rasterizer_state(rs);

    if (g_blend_state)
        ID3D11DeviceContext_OMSetBlendState(ctx, g_blend_state, blend_factor, 0xFFFFFFFF);
    if (g_ds_state)
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, g_ds_state, rs[D3DRS_STENCILREF]);
    if (g_raster_state)
        ID3D11DeviceContext_RSSetState(ctx, g_raster_state);
    if (g_wclip_on) {
        /* The clip is in the title's 640x480 pixels; the scene target may be
         * larger. Scale 1 leaves it exactly as given. */
        D3D11_RECT r;
        float sx, sy;
        d3d8_GetGuestScale(&sx, &sy);
        r.left   = (LONG)(g_wclip.left   * sx + 0.5f);
        r.top    = (LONG)(g_wclip.top    * sy + 0.5f);
        r.right  = (LONG)(g_wclip.right  * sx + 0.5f);
        r.bottom = (LONG)(g_wclip.bottom * sy + 0.5f);
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &r);
    }

    /* Apply samplers for all 4 texture stages */
    {
        DWORD s;
        for (s = 0; s < 4; s++)
            d3d8_states_apply_sampler(s);
    }
}

void d3d8_states_apply_nv2a(void)
{
    const DWORD *rs = d3d8_GetRenderStates();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    float blend_factor[4] = { 1, 1, 1, 1 };
    ID3D11BlendState *b;
    ID3D11DepthStencilState *d;
    DWORD s;
    int memo;

    if (!pump_sobj_on()) { d3d8_states_apply(); return; }
    if (!rs || !ctx) return;
    b = so_blend(rs);
    d = so_ds(rs);
    if (b) ID3D11DeviceContext_OMSetBlendState(ctx, b, blend_factor, 0xFFFFFFFF);
    if (d) ID3D11DeviceContext_OMSetDepthStencilState(ctx, d, rs[D3DRS_STENCILREF]);
    /* no rasteriser state: the caller binds its own right after */
    if (g_wclip_on) {                   /* as d3d8_states_apply */
        D3D11_RECT r;
        float sx, sy;
        d3d8_GetGuestScale(&sx, &sy);
        r.left   = (LONG)(g_wclip.left   * sx + 0.5f);
        r.top    = (LONG)(g_wclip.top    * sy + 0.5f);
        r.right  = (LONG)(g_wclip.right  * sx + 0.5f);
        r.bottom = (LONG)(g_wclip.bottom * sy + 0.5f);
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &r);
    }
    memo = pump_smemo_on();             /* once per draw, not per stage */
    for (s = 0; s < 4; s++)
        so_sampler(s, ctx, memo);
}
