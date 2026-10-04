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
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Cached D3D11 state objects
 * ================================================================ */

static ID3D11BlendState        *g_blend_state = NULL;
static ID3D11DepthStencilState *g_ds_state = NULL;
static ID3D11RasterizerState   *g_raster_state = NULL;
static ID3D11SamplerState      *g_sampler_states[4] = { NULL, NULL, NULL, NULL };

/* Last known render state hash for dirty detection */
static DWORD g_last_blend_hash = 0;
static DWORD g_last_ds_hash = 0;
static DWORD g_last_raster_hash = 0;

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

/* Hash of the render states each D3D11 state object is built from, for dirty
 * detection. These were XORs of shifted values, which collide (and
 * the depth-stencil one left out the stencil ops and write mask, so a change
 * of op alone reused the old state object). FNV-1a over every input. */
static DWORD hash_states(const DWORD *rs, const int *idx, int n)
{
    DWORD h = 2166136261u;
    int i;
    for (i = 0; i < n; i++) {
        DWORD v = rs[idx[i]];
        int k;
        for (k = 0; k < 4; k++) { h ^= (v >> (8 * k)) & 0xFF; h *= 16777619u; }
    }
    return h;
}

static DWORD hash_blend_states(const DWORD *rs)
{
    static const int idx[] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
                               D3DRS_BLENDOP, D3DRS_COLORWRITEENABLE };
    return hash_states(rs, idx, (int)(sizeof idx / sizeof idx[0]));
}

static DWORD hash_ds_states(const DWORD *rs)
{
    static const int idx[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC,
                               D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILMASK,
                               D3DRS_STENCILWRITEMASK, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL,
                               D3DRS_STENCILPASS };
    return hash_states(rs, idx, (int)(sizeof idx / sizeof idx[0]));
}

static DWORD hash_raster_states(const DWORD *rs)
{
    static const int idx[] = { D3DRS_CULLMODE, D3DRS_FILLMODE };
    return hash_states(rs, idx, (int)(sizeof idx / sizeof idx[0]));
}

/* ================================================================
 * State object creation
 * ================================================================ */

static void update_blend_state(const DWORD *rs)
{
    DWORD hash = hash_blend_states(rs);
    D3D11_BLEND_DESC bd;
    HRESULT hr;

    if (hash == g_last_blend_hash && g_blend_state) return;
    g_last_blend_hash = hash;

    if (g_blend_state) {
        ID3D11BlendState_Release(g_blend_state);
        g_blend_state = NULL;
    }

    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable = rs[D3DRS_ALPHABLENDENABLE] ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = d3d8_to_d3d11_blend(rs[D3DRS_SRCBLEND]);
    bd.RenderTarget[0].DestBlend = d3d8_to_d3d11_blend(rs[D3DRS_DESTBLEND]);
    bd.RenderTarget[0].BlendOp = d3d8_to_d3d11_blendop(rs[D3DRS_BLENDOP] ? rs[D3DRS_BLENDOP] : 1);
    bd.RenderTarget[0].SrcBlendAlpha = bd.RenderTarget[0].SrcBlend;
    bd.RenderTarget[0].DestBlendAlpha = bd.RenderTarget[0].DestBlend;
    bd.RenderTarget[0].BlendOpAlpha = bd.RenderTarget[0].BlendOp;
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)(rs[D3DRS_COLORWRITEENABLE] & 0x0F);

    hr = ID3D11Device_CreateBlendState(d3d8_GetD3D11Device(), &bd, &g_blend_state);
    if (FAILED(hr))
        fprintf(stderr, "D3D8: CreateBlendState failed: 0x%08lX\n", hr);
}

static void update_depth_stencil_state(const DWORD *rs)
{
    DWORD hash = hash_ds_states(rs);
    D3D11_DEPTH_STENCIL_DESC dsd;
    HRESULT hr;

    if (hash == g_last_ds_hash && g_ds_state) return;
    g_last_ds_hash = hash;

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

    hr = ID3D11Device_CreateDepthStencilState(d3d8_GetD3D11Device(), &dsd, &g_ds_state);
    if (FAILED(hr))
        fprintf(stderr, "D3D8: CreateDepthStencilState failed: 0x%08lX\n", hr);
}

static void update_rasterizer_state(const DWORD *rs)
{
    DWORD hash = hash_raster_states(rs) ^ (g_wclip_on ? 0x9E3779B9u : 0u);
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    if (hash == g_last_raster_hash && g_raster_state) return;
    g_last_raster_hash = hash;

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
        if (k == ncache) {
            ID3D11SamplerState *ss = NULL;
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
    if (g_blend_state)  { ID3D11BlendState_Release(g_blend_state); g_blend_state = NULL; }
    if (g_ds_state)     { ID3D11DepthStencilState_Release(g_ds_state); g_ds_state = NULL; }
    if (g_raster_state) { ID3D11RasterizerState_Release(g_raster_state); g_raster_state = NULL; }
    for (i = 0; i < 4; i++) {
        if (g_sampler_states[i]) {
            ID3D11SamplerState_Release(g_sampler_states[i]);
            g_sampler_states[i] = NULL;
        }
    }
    g_last_blend_hash = 0;
    g_last_ds_hash = 0;
    g_last_raster_hash = 0;
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
