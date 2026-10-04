/* d3d8_softshadow.c -- see d3d8_softshadow.h (fork). */
#include "d3d8_internal.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_softshadow.h"

/* Full-screen triangle; t0 = source mask, t1 = depth, s0 linear clamp.
 * b0: { w, h, 1/w, 1/h } (full size), { shade, taps each side, sigma, depth
 * falloff } (taps and sigma in half-size pixels), { half w, half h }.
 * The blur runs at half size: h reads the full-size mask one 2x2 block per
 * tap (a bilinear fetch at the block centre), v stays at half size, apply
 * brings it back to full size from the 4 nearest half-size texels weighted by
 * depth (no shadow fringe over a silhouette). Depth term: relative depth
 * difference (z - z0) / (1 - z0) ~ (d - d0) / d for a perspective depth
 * buffer, weight 1 / (1 + k dz^2). Most pixels are far from any shadow edge:
 * the mask alone is read first, and a window that is all 0 (no shadow) or all
 * 1 (inside one) is returned as is -- only edge pixels read the depth. */
static const char k_hlsl[] =
"cbuffer C : register(b0) { float4 size; float4 prm; float4 half_size; };\n"
"Texture2D<float> Src : register(t0);\n"
"SamplerState Lin : register(s0);\n"
"#ifdef SS_MSAA\n"
"Texture2DMS<float> Dep : register(t1);\n"
"float depth_at(int2 c) { return Dep.Load(c, 0); }\n"
"#else\n"
"Texture2D<float> Dep : register(t1);\n"
"float depth_at(int2 c) { return Dep.Load(int3(c, 0)); }\n"
"#endif\n"
"void vs(uint id : SV_VertexID, out float4 pos : SV_Position) {\n"
"    float2 t = float2((id << 1) & 2, id & 2);\n"
"    pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
"}\n"
"float4 ps_one(float4 pos : SV_Position) : SV_Target { return 1.0; }\n"
"int2 full_hi() { return int2(size.xy) - 1; }\n"
"int2 half_hi() { return int2(half_size.xy) - 1; }\n"
"float dweight(float z, float z0, float inv) { float dz = (z - z0) * inv; return 1.0 / (1.0 + dz * dz * prm.w); }\n"
"float gweight(int i) { return exp2(-1.442695 * (float)(i * i) / (2.0 * prm.z * prm.z)); }\n"
/* h: half-size output, full-size mask in (one bilinear 2x2 average per tap) */
"float4 ps_h(float4 pos : SV_Position) : SV_Target {\n"
"    int2 c = int2(pos.xy), f = c * 2;\n"
"    int r = (int)prm.y, i;\n"
"    float lo = 1.0, top = 0.0, m[33];\n"
"    [loop] for (i = -r; i <= r; i++) {\n"
"        float2 uv = (float2(f + int2(2 * i, 0)) + 1.0) * size.zw;\n"
"        float v = Src.SampleLevel(Lin, uv, 0);\n"
"        m[i + r] = v; lo = min(lo, v); top = max(top, v);\n"
"    }\n"
"    if (top <= 0.0) return 0.0;\n"
"    if (lo >= 1.0) return 1.0;\n"
"    float z0 = depth_at(min(f, full_hi())), inv = 1.0 / max(1.0 - z0, 1e-5);\n"
"    float acc = 0.0, wsum = 0.0;\n"
"    [loop] for (i = -r; i <= r; i++) {\n"
"        int2 q = clamp(f + int2(2 * i, 0), int2(0, 0), full_hi());\n"
"        float w = gweight(i) * dweight(depth_at(q), z0, inv);\n"
"        acc += w * m[i + r]; wsum += w;\n"
"    }\n"
"    return acc / wsum;\n"
"}\n"
/* v: half size in and out */
"float4 ps_v(float4 pos : SV_Position) : SV_Target {\n"
"    int2 c = int2(pos.xy);\n"
"    int r = (int)prm.y, i;\n"
"    float lo = 1.0, top = 0.0, m[33];\n"
"    [loop] for (i = -r; i <= r; i++) {\n"
"        float v = Src.Load(int3(clamp(c + int2(0, i), int2(0, 0), half_hi()), 0));\n"
"        m[i + r] = v; lo = min(lo, v); top = max(top, v);\n"
"    }\n"
"    if (top <= 0.0) return 0.0;\n"
"    if (lo >= 1.0) return 1.0;\n"
"    float z0 = depth_at(min(c * 2, full_hi())), inv = 1.0 / max(1.0 - z0, 1e-5);\n"
"    float acc = 0.0, wsum = 0.0;\n"
"    [loop] for (i = -r; i <= r; i++) {\n"
"        int2 q = clamp(c + int2(0, i), int2(0, 0), half_hi());\n"
"        float w = gweight(i) * dweight(depth_at(min(q * 2, full_hi())), z0, inv);\n"
"        acc += w * m[i + r]; wsum += w;\n"
"    }\n"
"    return acc / wsum;\n"
"}\n"
/* apply: full size; the 4 half-size texels around, bilinear x depth weights */
"float4 ps_apply(float4 pos : SV_Position) : SV_Target {\n"
"    float2 hc = pos.xy * 0.5 - 0.5;\n"
"    int2 b = int2(floor(hc));\n"
"    float2 t = hc - (float2)b;\n"
"    int2 o[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };\n"
"    float bw[4] = { (1 - t.x) * (1 - t.y), t.x * (1 - t.y), (1 - t.x) * t.y, t.x * t.y };\n"
"    float v[4], lo = 1.0, top = 0.0, mask;\n"
"    int i;\n"
"    [unroll] for (i = 0; i < 4; i++) {\n"
"        v[i] = Src.Load(int3(clamp(b + o[i], int2(0, 0), half_hi()), 0));\n"
"        lo = min(lo, v[i]); top = max(top, v[i]);\n"
"    }\n"
"    if (top <= 0.0) return 1.0;\n"
"    if (lo >= 1.0) { mask = 1.0; }\n"
"    else {\n"
"        float z0 = depth_at(int2(pos.xy)), inv = 1.0 / max(1.0 - z0, 1e-5);\n"
"        float acc = 0.0, wsum = 1e-6;\n"
"        [unroll] for (i = 0; i < 4; i++) {\n"
"            int2 q = clamp(b + o[i], int2(0, 0), half_hi());\n"
"            float w = bw[i] * dweight(depth_at(min(q * 2, full_hi())), z0, inv);\n"
"            acc += w * v[i]; wsum += w;\n"
"        }\n"
"        mask = acc / wsum;\n"
"    }\n"
"    float k = lerp(1.0, prm.x, mask);\n"
"    return float4(k, k, k, k);\n"
"}\n";

static struct {
    int                       init, failed;
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps_one, *ps_apply, *ps_h[2], *ps_v[2];   /* [msaa depth] */
    ID3D11Buffer             *cb;
    ID3D11SamplerState       *smp;
    ID3D11RasterizerState    *rs, *rs_scissor;
    ID3D11BlendState         *bs_apply;
    ID3D11DepthStencilState  *ds_mask;
    UINT                      ds_cmp, ds_rmask;
    /* targets */
    UINT                      w, h, msaa;
    ID3D11Texture2D          *mask_ms, *tex[3];          /* tex: mask (resolved), blur h, blur v */
    ID3D11RenderTargetView   *mask_rtv, *rtv[3];
    ID3D11ShaderResourceView *srv[3];
} S;

int d3d8_softshadow_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_SOFT_SHADOWS"); on = e && e[0] == '1'; }
    return on;
}

static ID3D10Blob *compile(const char *entry, const char *target, int msaa)
{
    static const D3D_SHADER_MACRO ms[] = { { "SS_MSAA", "1" }, { NULL, NULL } };
    ID3D10Blob *code = NULL, *err = NULL;
    if (FAILED(D3DCompile(k_hlsl, sizeof k_hlsl - 1, "softshadow", msaa ? ms : NULL, NULL,
                          entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        fprintf(stderr, "[SOFTSHADOW] shader %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "compile failed");
        if (err) ID3D10Blob_Release(err);
        if (code) ID3D10Blob_Release(code);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static ID3D11PixelShader *pixel(ID3D11Device *dev, const char *entry, int msaa)
{
    ID3D11PixelShader *ps = NULL;
    ID3D10Blob *b = compile(entry, "ps_5_0", msaa);
    if (!b) return NULL;
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b),
                                   ID3D10Blob_GetBufferSize(b), NULL, &ps);
    ID3D10Blob_Release(b);
    return ps;
}

static int base_init(ID3D11Device *dev)
{
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC bl;
    ID3D10Blob *b;
    int m;
    if (S.init) return !S.failed;
    S.init = 1;
    S.failed = 1;
    b = compile("vs", "vs_5_0", 0);
    if (!b) return 0;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b),
                                    ID3D10Blob_GetBufferSize(b), NULL, &S.vs);
    ID3D10Blob_Release(b);
    S.ps_one = pixel(dev, "ps_one", 0);
    S.ps_apply = pixel(dev, "ps_apply", 0);
    for (m = 0; m < 2; m++) {
        S.ps_h[m] = pixel(dev, "ps_h", m);
        S.ps_v[m] = pixel(dev, "ps_v", m);
    }
    if (!S.vs || !S.ps_one || !S.ps_apply || !S.ps_h[0] || !S.ps_h[1] || !S.ps_v[0] || !S.ps_v[1])
        return 0;

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &S.cb))) return 0;
    {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(ID3D11Device_CreateSamplerState(dev, &sd, &S.smp))) return 0;
    }

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &S.rs))) return 0;
    rd.ScissorEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &S.rs_scissor))) return 0;

    /* dest.rgb *= src.rgb, dest.a *= src.a: the title's ZERO / SRC_ALPHA with
     * a per-pixel factor instead of one alpha. */
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(dev, &bl, &S.bs_apply))) return 0;

    S.failed = 0;
    return 1;
}

static void targets_release(void)
{
    int i;
    if (S.mask_rtv) { ID3D11RenderTargetView_Release(S.mask_rtv); S.mask_rtv = NULL; }
    if (S.mask_ms) { ID3D11Texture2D_Release(S.mask_ms); S.mask_ms = NULL; }
    for (i = 0; i < 3; i++) {
        if (S.srv[i]) { ID3D11ShaderResourceView_Release(S.srv[i]); S.srv[i] = NULL; }
        if (S.rtv[i]) { ID3D11RenderTargetView_Release(S.rtv[i]); S.rtv[i] = NULL; }
        if (S.tex[i]) { ID3D11Texture2D_Release(S.tex[i]); S.tex[i] = NULL; }
    }
    S.w = S.h = S.msaa = 0;
}

static int targets_ensure(ID3D11Device *dev, UINT w, UINT h, UINT msaa)
{
    D3D11_TEXTURE2D_DESC td;
    int i;
    if (S.w == w && S.h == h && S.msaa == msaa && S.tex[2]) return 1;
    targets_release();
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (i = 0; i < 3; i++) {
        td.Width = i ? (w + 1) / 2 : w;          /* the blur runs at half size */
        td.Height = i ? (h + 1) / 2 : h;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &S.tex[i])) ||
            FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)S.tex[i], NULL, &S.rtv[i])) ||
            FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)S.tex[i], NULL, &S.srv[i])))
            goto fail;
    }
    if (msaa > 1) {
        /* The mask shares the depth-stencil buffer: same sample count. */
        td.Width = w;
        td.Height = h;
        td.SampleDesc.Count = msaa;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &S.mask_ms)) ||
            FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)S.mask_ms, NULL, &S.mask_rtv)))
            goto fail;
    } else {
        S.mask_rtv = S.rtv[0];
        ID3D11RenderTargetView_AddRef(S.mask_rtv);
    }
    S.w = w; S.h = h; S.msaa = msaa;
    return 1;
fail:
    targets_release();
    return 0;
}

static int mask_state(ID3D11Device *dev, UINT cmp, UINT rmask)
{
    D3D11_DEPTH_STENCIL_DESC dd;
    if (S.ds_mask && S.ds_cmp == cmp && S.ds_rmask == rmask) return 1;
    if (S.ds_mask) { ID3D11DepthStencilState_Release(S.ds_mask); S.ds_mask = NULL; }
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = TRUE;
    dd.StencilReadMask = (UINT8)rmask;
    dd.StencilWriteMask = 0;                    /* the title's quad zeroes it afterwards */
    dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    dd.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)cmp;
    dd.BackFace = dd.FrontFace;
    if (FAILED(ID3D11Device_CreateDepthStencilState(dev, &dd, &S.ds_mask))) return 0;
    S.ds_cmp = cmp;
    S.ds_rmask = rmask;
    return 1;
}

/* ── State save / restore (as d3d8_post.c) ─────────────────────────── */

typedef struct {
    ID3D11RenderTargetView   *rtv;
    ID3D11DepthStencilView   *dsv;
    D3D11_VIEWPORT            vp;
    UINT                      nvp;
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps;
    ID3D11InputLayout        *il;
    D3D11_PRIMITIVE_TOPOLOGY  topo;
    ID3D11ShaderResourceView *srv[2];
    ID3D11SamplerState       *smp;
    ID3D11Buffer             *pscb, *vscb;
    ID3D11BlendState         *bs;
    FLOAT                     bf[4];
    UINT                      sm;
    ID3D11DepthStencilState  *ds;
    UINT                      sref;
    ID3D11RasterizerState    *rs;
    D3D11_RECT                sc;
    UINT                      nsc;
} ss_saved;

static void state_save(ID3D11DeviceContext *ctx, ss_saved *o)
{
    memset(o, 0, sizeof *o);
    o->nvp = 1;
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &o->rtv, &o->dsv);
    ID3D11DeviceContext_RSGetViewports(ctx, &o->nvp, &o->vp);
    ID3D11DeviceContext_VSGetShader(ctx, &o->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(ctx, &o->ps, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(ctx, &o->il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(ctx, &o->topo);
    ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 2, o->srv);
    ID3D11DeviceContext_PSGetSamplers(ctx, 0, 1, &o->smp);
    ID3D11DeviceContext_PSGetConstantBuffers(ctx, 0, 1, &o->pscb);
    ID3D11DeviceContext_VSGetConstantBuffers(ctx, 0, 1, &o->vscb);
    ID3D11DeviceContext_OMGetBlendState(ctx, &o->bs, o->bf, &o->sm);
    ID3D11DeviceContext_OMGetDepthStencilState(ctx, &o->ds, &o->sref);
    ID3D11DeviceContext_RSGetState(ctx, &o->rs);
    o->nsc = 1;
    ID3D11DeviceContext_RSGetScissorRects(ctx, &o->nsc, &o->sc);
}

static void state_restore(ID3D11DeviceContext *ctx, ss_saved *o)
{
    int i;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &o->rtv, o->dsv);
    if (o->nvp) ID3D11DeviceContext_RSSetViewports(ctx, 1, &o->vp);
    ID3D11DeviceContext_VSSetShader(ctx, o->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, o->ps, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, o->il);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, o->topo);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, o->srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &o->smp);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &o->pscb);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &o->vscb);
    ID3D11DeviceContext_OMSetBlendState(ctx, o->bs, o->bf, o->sm);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, o->ds, o->sref);
    ID3D11DeviceContext_RSSetState(ctx, o->rs);
    ID3D11DeviceContext_RSSetScissorRects(ctx, o->nsc, o->nsc ? &o->sc : NULL);
    if (o->rtv) ID3D11RenderTargetView_Release(o->rtv);
    if (o->dsv) ID3D11DepthStencilView_Release(o->dsv);
    if (o->vs)  ID3D11VertexShader_Release(o->vs);
    if (o->ps)  ID3D11PixelShader_Release(o->ps);
    if (o->il)  ID3D11InputLayout_Release(o->il);
    for (i = 0; i < 2; i++) if (o->srv[i]) ID3D11ShaderResourceView_Release(o->srv[i]);
    if (o->smp) ID3D11SamplerState_Release(o->smp);
    if (o->pscb) ID3D11Buffer_Release(o->pscb);
    if (o->vscb) ID3D11Buffer_Release(o->vscb);
    if (o->bs)  ID3D11BlendState_Release(o->bs);
    if (o->ds)  ID3D11DepthStencilState_Release(o->ds);
    if (o->rs)  ID3D11RasterizerState_Release(o->rs);
}

/* ── The passes ────────────────────────────────────────────────────── */

int d3d8_softshadow_run(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                        ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv,
                        ID3D11ShaderResourceView *depth, UINT w, UINT h, UINT msaa,
                        float shade, UINT cmp, UINT ref, UINT rmask, const RECT *rect)
{
    static const FLOAT zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ID3D11ShaderResourceView *none[2] = { NULL, NULL }, *in[2];
    ss_saved saved;
    D3D11_VIEWPORT vp;
    FLOAT cb[12];
    D3D11_VIEWPORT hvp;
    float sigma;
    int taps;
    int ms = msaa > 1;

    if (!dev || !ctx || !rtv || !dsv || !depth || !w || !h || !rect ||
        rect->right <= rect->left || rect->bottom <= rect->top) return 0;
    if (!base_init(dev) || !targets_ensure(dev, w, h, msaa) || !mask_state(dev, cmp, rmask))
        return 0;

    /* Sigma 2.5 px at 480 lines, scaled with the render height; the blur
     * works in half-size pixels. */
    sigma = 1.25f * (float)h / 480.0f;
    if (sigma < 0.75f) sigma = 0.75f;
    taps = (int)(sigma * 1.5f + 0.5f);
    if (taps < 1) taps = 1;
    if (taps > 16) taps = 16;                       /* m[33] in the shaders */
    cb[0] = (FLOAT)w; cb[1] = (FLOAT)h; cb[2] = 1.0f / (FLOAT)w; cb[3] = 1.0f / (FLOAT)h;
    cb[4] = shade;
    cb[5] = (FLOAT)taps;
    cb[6] = sigma;
    cb[7] = 1600.0f;                                /* 1 / (1 + (dz / 0.025)^2): 0.2 at 5 % */
    cb[8] = (FLOAT)((w + 1) / 2); cb[9] = (FLOAT)((h + 1) / 2); cb[10] = cb[11] = 0.0f;

    state_save(ctx, &saved);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)S.cb, 0, NULL, cb, 0, 0);
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (FLOAT)w; vp.Height = (FLOAT)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, S.rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, S.vs, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &S.cb);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &S.smp);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
    hvp = vp;
    hvp.Width = (FLOAT)((w + 1) / 2); hvp.Height = (FLOAT)((h + 1) / 2);

    /* 1. mask: 1 where the title's stencil test passes, inside its quad */
    ID3D11DeviceContext_ClearRenderTargetView(ctx, S.mask_rtv, zero);
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, (const D3D11_RECT *)rect);
    ID3D11DeviceContext_RSSetState(ctx, S.rs_scissor);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &S.mask_rtv, dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, S.ds_mask, ref);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_PSSetShader(ctx, S.ps_one, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    if (ms)
        ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *)S.tex[0], 0,
                                               (ID3D11Resource *)S.mask_ms, 0, DXGI_FORMAT_R8_UNORM);

    /* 2. blur, depth-weighted (the depth buffer is read, so not bound) */
    ID3D11DeviceContext_RSSetState(ctx, S.rs);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &hvp);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &S.rtv[1], NULL);
    in[0] = S.srv[0]; in[1] = depth;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, in);
    ID3D11DeviceContext_PSSetShader(ctx, S.ps_h[ms], NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &S.rtv[2], NULL);
    in[0] = S.srv[1];
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, in);
    ID3D11DeviceContext_PSSetShader(ctx, S.ps_v[ms], NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);

    /* 3. apply: scene *= lerp(1, shade, blurred mask), inside the quad */
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    in[1] = depth;
    ID3D11DeviceContext_RSSetState(ctx, S.rs_scissor);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, S.bs_apply, NULL, 0xFFFFFFFFu);
    in[0] = S.srv[2];
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, in);
    ID3D11DeviceContext_PSSetShader(ctx, S.ps_apply, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);

    state_restore(ctx, &saved);
    return 1;
}
