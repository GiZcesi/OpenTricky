/* d3d8_rscale.c -- see d3d8_rscale.h (fork: render scale of the 3D). */
#include "d3d8_internal.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_rscale.h"

/* The steps the setting offers, in percent. */
static const int k_steps[] = { 50, 67, 75, 85, 100, 125, 150, 175, 200 };
#define N_STEPS ((int)(sizeof k_steps / sizeof k_steps[0]))

const int *d3d8_rscale_steps(int *count)
{
    if (count) *count = N_STEPS;
    return k_steps;
}

static int s_want = -1;
static volatile LONG s_gen;

static int snap_step(int want)
{
    int i, best = 100, bd = 1 << 30;
    if (want <= 0) want = 100;
    for (i = 0; i < N_STEPS; i++) {
        int d = abs(k_steps[i] - want);
        if (d < bd) { bd = d; best = k_steps[i]; }
    }
    return best;
}

int d3d8_rscale_wanted(void)
{
    if (s_want < 0) {
        const char *e = getenv("XBOX_RENDER_SCALE");
        s_want = snap_step(e && *e ? atoi(e) : 100);
    }
    return s_want;
}

void d3d8_rscale_set(int pct)
{
    s_want = snap_step(pct);
    InterlockedIncrement(&s_gen);
}

int d3d8_rscale_generation(void) { return (int)s_gen; }

/* Full-screen triangle. b0: { source w, h, 1/w, 1/h }, { output w, h, 1/w,
 * 1/h }. t0 = colour, t1 = depth (R24_UNORM_X8; Texture2DMS with RS_MSAA),
 * s0 = bilinear clamp.
 * ps_down: the output pixel's own extent in the source, [x, x + 1) * k with
 * k = source / output size, and every texel it overlaps weighted by the
 * overlap area. Rows and columns are visited until the overlap ends (at most
 * 4 a side: k up to 3), no array, no unrolled loop (vkd3d).
 * ps_up: one bilinear fetch at the pixel centre.
 * ps_depth: the source sample nearest the output pixel centre. */
static const char k_hlsl[] =
"cbuffer C : register(b0) { float4 src_size; float4 dst_size; };\n"
"Texture2D<float4> Src : register(t0);\n"
"SamplerState Lin : register(s0);\n"
"#ifdef RS_MSAA\n"
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
"float4 ps_up(float4 pos : SV_Position) : SV_Target {\n"
"    return Src.SampleLevel(Lin, pos.xy * dst_size.zw, 0);\n"
"}\n"
"float4 ps_down(float4 pos : SV_Position) : SV_Target {\n"
"    float2 k = src_size.xy * dst_size.zw;\n"
"    float2 a = floor(pos.xy) * k, b = a + k;\n"
"    int2 a0 = int2(floor(a)), hi = int2(src_size.xy) - 1;\n"
"    float4 acc = 0.0;\n"
"    float wsum = 0.0;\n"
"    int x, y;\n"
"    [loop] for (y = 0; y < 4; y++) {\n"
"        float wy = min(b.y, (float)(a0.y + y + 1)) - max(a.y, (float)(a0.y + y));\n"
"        if (wy <= 0.0) break;\n"
"        [loop] for (x = 0; x < 4; x++) {\n"
"            float wx = min(b.x, (float)(a0.x + x + 1)) - max(a.x, (float)(a0.x + x));\n"
"            if (wx <= 0.0) break;\n"
"            acc += Src.Load(int3(min(a0 + int2(x, y), hi), 0)) * (wx * wy);\n"
"            wsum += wx * wy;\n"
"        }\n"
"    }\n"
"    return wsum > 0.0 ? acc / wsum : Src.SampleLevel(Lin, pos.xy * dst_size.zw, 0);\n"
"}\n"
"float ps_depth(float4 pos : SV_Position) : SV_Depth {\n"
"    int2 c = min(int2(pos.xy * src_size.xy * dst_size.zw), int2(src_size.xy) - 1);\n"
"    return depth_at(c);\n"
"}\n";

static struct {
    int init, failed;
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps_up, *ps_down, *ps_depth[2];    /* [source multisampled] */
    ID3D11Buffer            *cb;
    ID3D11SamplerState      *smp;
    ID3D11RasterizerState   *rs;
    ID3D11BlendState        *bs;            /* opaque, all channels */
    ID3D11DepthStencilState *ds_off, *ds_write;
} R;

static ID3D10Blob *compile(const char *entry, const char *target, int msaa)
{
    static const D3D_SHADER_MACRO ms[] = { { "RS_MSAA", "1" }, { NULL, NULL } };
    ID3D10Blob *code = NULL, *err = NULL;
    if (FAILED(d3d8_shader_compile_logged(k_hlsl, sizeof k_hlsl - 1, "rscale", msaa ? ms : NULL,
                                          entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                          &code, &err))) {
        fprintf(stderr, "[RSCALE] shader %s: %s\n", entry,
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

int d3d8_rscale_init(ID3D11Device *dev)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC bl;
    D3D11_DEPTH_STENCIL_DESC dd;
    ID3D10Blob *b;
    if (R.init) return !R.failed;
    if (!dev) return 0;
    R.init = 1;
    R.failed = 1;
    b = compile("vs", "vs_5_0", 0);
    if (!b) return 0;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b),
                                    ID3D10Blob_GetBufferSize(b), NULL, &R.vs);
    ID3D10Blob_Release(b);
    R.ps_up = pixel(dev, "ps_up", 0);
    R.ps_down = pixel(dev, "ps_down", 0);
    R.ps_depth[0] = pixel(dev, "ps_depth", 0);
    R.ps_depth[1] = pixel(dev, "ps_depth", 1);
    if (!R.vs || !R.ps_up || !R.ps_down || !R.ps_depth[0] || !R.ps_depth[1]) return 0;

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &R.cb))) return 0;

    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(dev, &sd, &R.smp))) return 0;

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &R.rs))) return 0;

    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(dev, &bl, &R.bs))) return 0;

    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(ID3D11Device_CreateDepthStencilState(dev, &dd, &R.ds_off))) return 0;
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    if (FAILED(ID3D11Device_CreateDepthStencilState(dev, &dd, &R.ds_write))) return 0;

    R.failed = 0;
    fprintf(stderr, "[RSCALE] shaders ready\n");
    return 1;
}

void d3d8_rscale_release(void)
{
    if (R.vs) ID3D11VertexShader_Release(R.vs);
    if (R.ps_up) ID3D11PixelShader_Release(R.ps_up);
    if (R.ps_down) ID3D11PixelShader_Release(R.ps_down);
    if (R.ps_depth[0]) ID3D11PixelShader_Release(R.ps_depth[0]);
    if (R.ps_depth[1]) ID3D11PixelShader_Release(R.ps_depth[1]);
    if (R.cb) ID3D11Buffer_Release(R.cb);
    if (R.smp) ID3D11SamplerState_Release(R.smp);
    if (R.rs) ID3D11RasterizerState_Release(R.rs);
    if (R.bs) ID3D11BlendState_Release(R.bs);
    if (R.ds_off) ID3D11DepthStencilState_Release(R.ds_off);
    if (R.ds_write) ID3D11DepthStencilState_Release(R.ds_write);
    memset(&R, 0, sizeof R);
}

/* ── State save / restore (as d3d8_softshadow.c) ───────────────────── */

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
} rs_saved;

static void state_save(ID3D11DeviceContext *ctx, rs_saved *o)
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

static void state_restore(ID3D11DeviceContext *ctx, rs_saved *o)
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

/* Shared set-up of one full-screen pass: sizes, viewport, fixed states. */
static void pass_begin(ID3D11DeviceContext *ctx, UINT sw, UINT sh, UINT dw, UINT dh)
{
    D3D11_VIEWPORT vp;
    FLOAT cb[8];
    cb[0] = (FLOAT)sw; cb[1] = (FLOAT)sh; cb[2] = 1.0f / (FLOAT)sw; cb[3] = 1.0f / (FLOAT)sh;
    cb[4] = (FLOAT)dw; cb[5] = (FLOAT)dh; cb[6] = 1.0f / (FLOAT)dw; cb[7] = 1.0f / (FLOAT)dh;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)R.cb, 0, NULL, cb, 0, 0);
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (FLOAT)dw; vp.Height = (FLOAT)dh;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, R.rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, R.vs, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &R.cb);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &R.smp);
    ID3D11DeviceContext_OMSetBlendState(ctx, R.bs, NULL, 0xFFFFFFFFu);
}

int d3d8_rscale_resample(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                         ID3D11ShaderResourceView *src, UINT sw, UINT sh,
                         ID3D11RenderTargetView *dst, UINT dw, UINT dh)
{
    ID3D11ShaderResourceView *none[2] = { NULL, NULL };
    rs_saved saved;
    if (!ctx || !src || !dst || !sw || !sh || !dw || !dh || !d3d8_rscale_init(dev)) return 0;
    state_save(ctx, &saved);
    pass_begin(ctx, sw, sh, dw, dh);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, R.ds_off, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &dst, NULL);
    ID3D11DeviceContext_PSSetShader(ctx, (sw > dw || sh > dh) ? R.ps_down : R.ps_up, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &src);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
    state_restore(ctx, &saved);
    return 1;
}

int d3d8_rscale_depth_copy(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                           ID3D11ShaderResourceView *src, int src_ms, UINT sw, UINT sh,
                           ID3D11DepthStencilView *dsv, UINT dw, UINT dh)
{
    ID3D11ShaderResourceView *none[2] = { NULL, NULL }, *in[2];
    ID3D11RenderTargetView *no_rtv = NULL;
    rs_saved saved;
    if (!ctx || !dsv || !sw || !sh || !dw || !dh || !d3d8_rscale_init(dev)) return 0;
    ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
    if (!src) return 0;
    state_save(ctx, &saved);
    pass_begin(ctx, sw, sh, dw, dh);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, R.ds_write, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &no_rtv, dsv);
    ID3D11DeviceContext_PSSetShader(ctx, R.ps_depth[src_ms ? 1 : 0], NULL, 0);
    in[0] = NULL;
    in[1] = src;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, in);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
    state_restore(ctx, &saved);
    return 1;
}
