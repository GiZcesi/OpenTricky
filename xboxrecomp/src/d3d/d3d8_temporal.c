/* d3d8_temporal.c -- see d3d8_temporal.h (fork). */
#include "d3d8_internal.h"
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_temporal.h"

int   g_d3d8_temporal_mark;             /* d3d8_xbox.h */
float g_d3d8_temporal_delta[4];

/* Full-screen triangle. b0:
 *   size = { w, h, 1/w, 1/h };
 *   cam  = { tan of half the vertical angle, aspect } of this image, then of
 *          the previous one;
 *   m0..m3 = rows of M: a point of this image's view (row vector, w = 1)
 *          times M = the same world point in the previous image's view;
 *   r0..r2 = rows 0-2 of the previous view (a world displacement into it);
 *   lin  = { near, far, camera valid, reactive gain };
 *   jit  = { this image's jitter in uv (x, y), object target valid,
 *          reactive accumulation valid };
 *   shw  = { view: 1 mv, 2 reactive, 3 object; the riders' displacement used }.
 * t0 depth, t1 the riders' object target (sample 0 when multisampled),
 * t2 the reactive accumulation (resolved); for the view, t3 the vectors and
 * t4 the mask. b5: the object mirror's constant. */
static const char k_hlsl[] =
"cbuffer C : register(b0) { float4 size; float4 cam; float4 m0; float4 m1; float4 m2; float4 m3;\n"
"                           float4 r0; float4 r1; float4 r2; float4 lin; float4 jit; float4 shw; };\n"
"cbuffer O : register(b5) { float4 obj; };\n"
"#ifdef MS\n"
"Texture2DMS<float> Dep : register(t0);\n"
"Texture2DMS<float4> Obj : register(t1);\n"
"float dep(int2 p) { return Dep.Load(p, 0); }\n"
"float4 objv(int2 p) { return Obj.Load(p, 0); }\n"
"#else\n"
"Texture2D<float> Dep : register(t0);\n"
"Texture2D<float4> Obj : register(t1);\n"
"float dep(int2 p) { return Dep.Load(int3(p, 0)); }\n"
"float4 objv(int2 p) { return Obj.Load(int3(p, 0)); }\n"
"#endif\n"
"Texture2D<float4> Rea : register(t2);\n"
"Texture2D<float2> Mv : register(t3);\n"
"Texture2D<float> Rm : register(t4);\n"
"float4 vs(uint id : SV_VertexID) : SV_Position {\n"
"    float2 t = float2((id << 1) & 2, id & 2);\n"
"    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
"}\n"
"float vz(float d) { return lin.x * lin.y / (lin.y - d * (lin.y - lin.x)); }\n"
"struct MvOut { float4 mv : SV_Target0; float4 r : SV_Target1; };\n"
"MvOut mv_ps(float4 p : SV_Position) {\n"
"    MvOut o;\n"
"    int2 ip = int2(p.xy);\n"
"    float2 uv = p.xy * size.zw - jit.xy;\n"     /* the pixel without the jitter */
"    float2 mv = float2(0.0, 0.0);\n"
"    float4 ob = jit.z > 0.5 ? objv(ip) : float4(0.0, 0.0, 0.0, 0.0);\n"
"    if (lin.z > 0.5) {\n"
"        float z = vz(dep(ip));\n"
"        float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
"        float3 c = float3(ndc.x * cam.x * cam.y * z, ndc.y * cam.x * z, z);\n"
"        float3 q = c.x * m0.xyz + c.y * m1.xyz + c.z * m2.xyz + m3.xyz;\n"
"        if (ob.w > 0.75 && shw.y > 0.5) q -= ob.x * r0.xyz + ob.y * r1.xyz + ob.z * r2.xyz;\n"
"        if (q.z > lin.x * 0.5) {\n"
"            float2 np = float2(q.x / (q.z * cam.z * cam.w), q.y / (q.z * cam.z));\n"
"            mv = float2(np.x * 0.5 + 0.5, 0.5 - np.y * 0.5) - uv;\n"
"        }\n"
"    }\n"
"    float r = 0.0;\n"
"    if (jit.w > 0.5) { float3 a = Rea.Load(int3(ip, 0)).rgb; r = max(a.r, max(a.g, a.b)) * lin.w; }\n"
"    if (ob.w > 0.25 && ob.w <= 0.75) r = max(r, 0.8);\n"   /* a rider draw with no history */
"    o.mv = float4(mv, 0.0, 0.0);\n"
"    o.r = float4(min(r, 0.9), 0.0, 0.0, 0.0);\n"
"    return o;\n"
"}\n"
"float4 obj_ps(float4 p : SV_Position) : SV_Target { return obj; }\n"
/* the view: colour over the 3D, alpha-blended */
"float4 show_ps(float4 p : SV_Position) : SV_Target {\n"
"    int2 ip = int2(p.xy);\n"
"    if (shw.x < 1.5) {\n"
"        float2 v = Mv.Load(int3(ip, 0)) * size.xy;\n"
"        float l = length(v);\n"
"        float h = atan2(-v.y, v.x) / 6.2831853 + 0.5;\n"
"        float3 rgb = saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);\n"
"        return float4(rgb * saturate(l / 8.0), 0.85);\n"
"    }\n"
"    if (shw.x < 2.5) return float4(Rm.Load(int3(ip, 0)), 0.0, 0.0, 0.85);\n"
"    float4 ob = objv(ip);\n"
"    if (ob.w > 0.75) return float4(0.0, saturate(0.3 + length(ob.xyz) / 20.0), 0.0, 0.85);\n"
"    if (ob.w > 0.25) return float4(1.0, 0.0, 1.0, 0.85);\n"
"    return float4(0.0, 0.0, 0.0, 0.85);\n"
"}\n";

#define CUT_DIST   400.0f               /* camera moved more than this between two images: a cut */
#define CUT_COS    0.5f                 /* or turned by more than 60 degrees */
#define MAX_PHASES 64

static int s_wanted = -1;

static struct {
    int                       failed, inited;
    int                       view, log;
    float                     bias_env, gain;
    int                       has_bias_env;
    float                     near_env, far_env;      /* XBOX_TEMPORAL_NEAR / _FAR (diagnostic) */
    int                       rigid;                  /* XBOX_TEMPORAL_RIGID=0: riders get the camera's vector (diagnostic) */
    /* jitter */
    int                       jit_on, phases, moved;
    unsigned                  index;
    float                     jit_ndc[2], jit_px[2], bias;
    UINT                      jit_w, jit_h;
    /* camera */
    int                     (*src)(unsigned frame, float *cam);
    float                     v_last[16], v_prev[16], tan_last, asp_last, tan_prev, asp_prev;
    int                       have_last, have_prev, cut;
    /* shaders and states */
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps_mv[2], *ps_show[2], *ps_obj;
    ID3D11Buffer             *cb, *cb_obj;
    ID3D11RasterizerState    *rs;
    ID3D11BlendState         *bs_show;
    /* targets: object and reactive accumulation at the bound 3D target's
     * size and sample count; vectors and mask single-sampled */
    ID3D11Texture2D          *obj, *rea, *rea_res, *mv, *rm;
    ID3D11RenderTargetView   *obj_rtv, *rea_rtv, *mv_rtv, *rm_rtv;
    ID3D11ShaderResourceView *obj_srv, *rea_srv, *mv_srv, *rm_srv;
    UINT                      tw, th, tmsaa, ow, oh;
    unsigned                  obj_seq, rea_seq;
    int                       obj_used, rea_used;
    /* depth-stencil variants of the mirrors: [0] reactive (the title's test,
     * LESS_EQUAL when it writes), [1] object (EQUAL); writes off */
    struct { ID3D11DepthStencilState *orig, *var; int kind; } dsv[32];
    int                       ndsv;
    /* the draw being mirrored */
    ID3D11RenderTargetView   *m_rtv, *m_rtv1;
    ID3D11DepthStencilView   *m_dsv;
    ID3D11DepthStencilState  *m_ds, *m_uncached;
    UINT                      m_sref;
    ID3D11PixelShader        *m_ps;
    ID3D11BlendState         *m_bs;
    FLOAT                     m_bf[4];
    UINT                      m_sm;
    int                       m_kind;
    /* this image's inputs */
    d3d8_temporal_inputs      in;
    unsigned                  in_seq;
    int                       have_in;
    unsigned long long        images, resets, mir_rea, mir_obj, mir_new;
} T;

int d3d8_temporal_wanted(void)
{
    if (s_wanted < 0) {
        const char *e = NULL;                           /* unfinished: never on */
        s_wanted = e && e[0] == '1';
        if (s_wanted) fprintf(stderr, "[TEMPORAL] on: jitter, motion vectors, reactive mask (no resolve yet)\n");
    }
    return s_wanted;
}

int d3d8_temporal_active(void) { return d3d8_temporal_wanted() && !T.failed; }

void d3d8_temporal_set_frame_source(int (*fn)(unsigned frame, float *cam)) { T.src = fn; }

static void env_once(void)
{
    const char *e;
    if (T.inited) return;
    T.inited = 1;
    e = getenv("XBOX_TEMPORAL_VIEW");
    T.view = !e ? 0 : !strcmp(e, "mv") ? 1 : !strcmp(e, "reactive") ? 2 : !strcmp(e, "object") ? 3 : 0;
    e = getenv("XBOX_TEMPORAL_LOG");
    T.log = e && e[0] == '1';
    e = getenv("XBOX_TEMPORAL_MIPBIAS");
    if (e && *e) { T.bias_env = (float)atof(e); T.has_bias_env = 1; }
    e = getenv("XBOX_TEMPORAL_REACTIVE_GAIN");
    T.gain = (e && *e) ? (float)atof(e) : 1.0f;
    e = getenv("XBOX_TEMPORAL_RIGID");
    T.rigid = !(e && e[0] == '0');
    e = getenv("XBOX_TEMPORAL_NEAR");
    T.near_env = (e && *e) ? (float)atof(e) : 0.0f;
    e = getenv("XBOX_TEMPORAL_FAR");
    T.far_env = (e && *e) ? (float)atof(e) : 0.0f;
}

/* ── Jitter ─────────────────────────────────────────────────────────── */

static float halton(unsigned i, unsigned b)
{
    float f = 1.0f, r = 0.0f;
    while (i) { f /= (float)b; r += f * (float)(i % b); i /= b; }
    return r;
}

void d3d8_temporal_next_image(int marked, UINT w, UINT h, UINT pct)
{
    int n = 8;
    T.jit_on = 0;
    T.moved = 0;
    if (!d3d8_temporal_active() || !marked || !w || !h) return;
    env_once();
    if (pct && pct < 100) {
        float k = 100.0f / (float)pct;
        n = (int)ceilf(8.0f * k * k);
        if (n > MAX_PHASES) n = MAX_PHASES;
    }
    T.phases = n;
    T.index = T.index % (unsigned)n + 1u;           /* 1..n: never (0, 0) */
    T.jit_px[0] = halton(T.index, 2) - 0.5f;        /* pixels, x right */
    T.jit_px[1] = halton(T.index, 3) - 0.5f;        /* pixels, y down */
    T.jit_ndc[0] = 2.0f * T.jit_px[0] / (float)w;
    T.jit_ndc[1] = -2.0f * T.jit_px[1] / (float)h;
    T.jit_w = w; T.jit_h = h;
    T.bias = T.has_bias_env ? T.bias_env : log2f((float)(pct ? pct : 100u) / 100.0f) - 1.0f;
    T.jit_on = 1;
}

int d3d8_temporal_jitter_ndc(float *jx, float *jy)
{
    if (!T.jit_on) { *jx = *jy = 0.0f; return 0; }
    *jx = T.jit_ndc[0];
    *jy = T.jit_ndc[1];
    return 1;
}

float d3d8_temporal_mip_bias(void) { return T.jit_on ? T.bias : 0.0f; }

void d3d8_temporal_jitter_off(void) { T.jit_on = 0; }
void d3d8_temporal_set_moved(void) { T.moved = 1; }
int  d3d8_temporal_moved(void) { return T.moved; }

/* ── Shaders, states, targets ──────────────────────────────────────── */

static ID3D10Blob *compile(const char *entry, const char *target, int ms)
{
    static const D3D_SHADER_MACRO defs_ms[] = { { "MS", "1" }, { NULL, NULL } };
    ID3D10Blob *code = NULL, *err = NULL;
    if (FAILED(d3d8_shader_compile_logged(k_hlsl, sizeof k_hlsl - 1, "temporal", ms ? defs_ms : NULL, entry, target,
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, &code, &err))) {
        fprintf(stderr, "[TEMPORAL] shader %s%s: %s\n", entry, ms ? " (ms)" : "",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "compile failed");
        if (err) ID3D10Blob_Release(err);
        if (code) ID3D10Blob_Release(code);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static int make_ps(ID3D11Device *dev, const char *entry, int ms, ID3D11PixelShader **ps)
{
    ID3D10Blob *code = compile(entry, "ps_5_0", ms);
    HRESULT hr;
    if (!code) return 0;
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
                                        ID3D10Blob_GetBufferSize(code), NULL, ps);
    ID3D10Blob_Release(code);
    return SUCCEEDED(hr);
}

static void targets_release(void)
{
#define REL(x, T_) do { if (x) { T_##_Release(x); x = NULL; } } while (0)
    REL(T.obj_srv, ID3D11ShaderResourceView); REL(T.obj_rtv, ID3D11RenderTargetView); REL(T.obj, ID3D11Texture2D);
    REL(T.rea_srv, ID3D11ShaderResourceView); REL(T.rea_rtv, ID3D11RenderTargetView);
    REL(T.rea_res, ID3D11Texture2D); REL(T.rea, ID3D11Texture2D);
    T.tw = T.th = T.tmsaa = 0;
}

static void out_release(void)
{
    REL(T.mv_srv, ID3D11ShaderResourceView); REL(T.mv_rtv, ID3D11RenderTargetView); REL(T.mv, ID3D11Texture2D);
    REL(T.rm_srv, ID3D11ShaderResourceView); REL(T.rm_rtv, ID3D11RenderTargetView); REL(T.rm, ID3D11Texture2D);
    T.ow = T.oh = 0;
    T.have_in = 0;
}

void d3d8_temporal_release(void)
{
    int i;
    targets_release();
    out_release();
    for (i = 0; i < T.ndsv; i++) {
        if (T.dsv[i].orig) ID3D11DepthStencilState_Release(T.dsv[i].orig);
        if (T.dsv[i].var) ID3D11DepthStencilState_Release(T.dsv[i].var);
    }
    T.ndsv = 0;
    REL(T.vs, ID3D11VertexShader);
    for (i = 0; i < 2; i++) { REL(T.ps_mv[i], ID3D11PixelShader); REL(T.ps_show[i], ID3D11PixelShader); }
    REL(T.ps_obj, ID3D11PixelShader);
    REL(T.cb, ID3D11Buffer); REL(T.cb_obj, ID3D11Buffer);
    REL(T.rs, ID3D11RasterizerState);
    REL(T.bs_show, ID3D11BlendState);
#undef REL
}

static int base_init(ID3D11Device *dev)
{
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC bl;
    ID3D10Blob *code;
    HRESULT hr;
    int k;
    if (T.cb) return 1;
    if (T.failed || !dev) return 0;
    env_once();
    code = compile("vs", "vs_5_0", 0);
    if (!code) goto fail;
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &T.vs);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) goto fail;
    for (k = 0; k < 2; k++)
        if (!make_ps(dev, "mv_ps", k, &T.ps_mv[k]) || !make_ps(dev, "show_ps", k, &T.ps_show[k])) goto fail;
    if (!make_ps(dev, "obj_ps", 0, &T.ps_obj)) goto fail;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 12 * 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &T.cb))) goto fail;
    bd.ByteWidth = 16;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &T.cb_obj))) goto fail;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &T.rs))) goto fail;
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(dev, &bl, &T.bs_show))) goto fail;
    fprintf(stderr, "[TEMPORAL] ready\n");
    return 1;
fail:
    fprintf(stderr, "[TEMPORAL] could not be set up: off\n");
    d3d8_temporal_release();
    T.failed = 1;
    T.jit_on = 0;
    return 0;
}

/* The object and reactive accumulation targets: the size and sample count
 * of the bound 3D target (its depth buffer tests them). */
static int targets_ensure(ID3D11Device *dev, ID3D11RenderTargetView *rt)
{
    D3D11_TEXTURE2D_DESC sd, td;
    ID3D11Resource *res = NULL;
    int ms;
    ID3D11RenderTargetView_GetResource(rt, &res);
    if (!res) return 0;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &sd);
    ID3D11Resource_Release(res);
    if (T.obj && T.tw == sd.Width && T.th == sd.Height && T.tmsaa == sd.SampleDesc.Count) return 1;
    targets_release();
    ms = sd.SampleDesc.Count > 1;
    memset(&td, 0, sizeof td);
    td.Width = sd.Width;
    td.Height = sd.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc = sd.SampleDesc;
    td.Usage = D3D11_USAGE_DEFAULT;
    /* object: read at sample 0 (never resolved: an average of
     * displacements would be wrong at the edges) */
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &T.obj)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)T.obj, NULL, &T.obj_rtv)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)T.obj, NULL, &T.obj_srv)))
        goto fail;
    /* reactive accumulation: the title's blending, resolved (an average) */
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | (ms ? 0 : D3D11_BIND_SHADER_RESOURCE);
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &T.rea)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)T.rea, NULL, &T.rea_rtv)))
        goto fail;
    if (ms) {
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &T.rea_res))) goto fail;
    }
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)(T.rea_res ? T.rea_res : T.rea),
                                                     NULL, &T.rea_srv)))
        goto fail;
    T.tw = sd.Width; T.th = sd.Height; T.tmsaa = sd.SampleDesc.Count;
    T.obj_seq = T.rea_seq = 0;
    T.obj_used = T.rea_used = 0;
    fprintf(stderr, "[TEMPORAL] object and reactive targets %ux%u, %u sample(s)\n", T.tw, T.th, T.tmsaa);
    return 1;
fail:
    fprintf(stderr, "[TEMPORAL] object / reactive targets %ux%u failed: camera vectors only\n",
            sd.Width, sd.Height);
    targets_release();
    return 0;
}

static int out_ensure(ID3D11Device *dev, UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC td;
    if (T.mv && T.ow == w && T.oh == h) return 1;
    out_release();
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &T.mv)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)T.mv, NULL, &T.mv_rtv)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)T.mv, NULL, &T.mv_srv)))
        goto fail;
    td.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &T.rm)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)T.rm, NULL, &T.rm_rtv)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)T.rm, NULL, &T.rm_srv)))
        goto fail;
    T.ow = w; T.oh = h;
    fprintf(stderr, "[TEMPORAL] motion vectors and reactive mask %ux%u\n", w, h);
    return 1;
fail:
    fprintf(stderr, "[TEMPORAL] vector targets %ux%u failed\n", w, h);
    out_release();
    return 0;
}

/* ── Draw mirrors ──────────────────────────────────────────────────── */

/* The title's depth-stencil state with writes off: kind 0 (reactive) the
 * same test, LESS_EQUAL / GREATER_EQUAL when the state writes depth (the
 * second draw meets the depth the first one wrote); kind 1 (object) EQUAL:
 * only the pixels the draw itself left in the depth buffer (not its
 * alpha-tested holes). */
static ID3D11DepthStencilState *ds_variant(ID3D11Device *dev, ID3D11DepthStencilState *orig, int kind,
                                           int *cached)
{
    D3D11_DEPTH_STENCIL_DESC dd;
    ID3D11DepthStencilState *v = NULL;
    int i;
    *cached = 1;
    for (i = 0; i < T.ndsv; i++)
        if (T.dsv[i].orig == orig && T.dsv[i].kind == kind) return T.dsv[i].var;
    if (orig) ID3D11DepthStencilState_GetDesc(orig, &dd);
    else {
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
        dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dd.BackFace = dd.FrontFace;
    }
    /* EQUAL only for a draw that writes its depth; one that does not (a part
     * drawn over the depth of what is behind it) keeps the title's test */
    if (kind == 1 && dd.DepthEnable && dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO)
        dd.DepthFunc = D3D11_COMPARISON_EQUAL;
    else if (dd.DepthEnable && dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO) {
        if (dd.DepthFunc == D3D11_COMPARISON_LESS) dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        else if (dd.DepthFunc == D3D11_COMPARISON_GREATER) dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    }
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.StencilWriteMask = 0;
    if (FAILED(ID3D11Device_CreateDepthStencilState(dev, &dd, &v))) return NULL;
    if (!orig || T.ndsv == (int)(sizeof T.dsv / sizeof T.dsv[0])) {
        *cached = 0;                    /* released by mirror_end */
        return v;
    }
    ID3D11DepthStencilState_AddRef(orig);       /* held: its address stays its own */
    T.dsv[T.ndsv].orig = orig;
    T.dsv[T.ndsv].var = v;
    T.dsv[T.ndsv].kind = kind;
    T.ndsv++;
    return v;
}

static void m_clear(void)
{
    if (T.m_uncached) { ID3D11DepthStencilState_Release(T.m_uncached); T.m_uncached = NULL; }
    if (T.m_ds) { ID3D11DepthStencilState_Release(T.m_ds); T.m_ds = NULL; }
    if (T.m_rtv) { ID3D11RenderTargetView_Release(T.m_rtv); T.m_rtv = NULL; }
    if (T.m_rtv1) { ID3D11RenderTargetView_Release(T.m_rtv1); T.m_rtv1 = NULL; }
    if (T.m_dsv) { ID3D11DepthStencilView_Release(T.m_dsv); T.m_dsv = NULL; }
    if (T.m_ps) { ID3D11PixelShader_Release(T.m_ps); T.m_ps = NULL; }
    if (T.m_bs) { ID3D11BlendState_Release(T.m_bs); T.m_bs = NULL; }
}

int d3d8_temporal_mirror_begin(ID3D11DeviceContext *ctx)
{
    static const FLOAT zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3D11RenderTargetView *rt[2] = { NULL, NULL };
    ID3D11DepthStencilState *v;
    unsigned seq = d3d8_PresentSeq();
    int kind = g_d3d8_temporal_mark == 2 ? 1 : 0, cached;
    if (!g_d3d8_temporal_mark || !T.jit_on || !dev || !base_init(dev)) return 0;
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 2, rt, &T.m_dsv);
    T.m_rtv = rt[0]; T.m_rtv1 = rt[1];
    /* the 3D target only, never a render to texture */
    if (!T.m_rtv || !T.m_dsv || T.m_rtv != d3d8_Get3DRTV() || !targets_ensure(dev, T.m_rtv)) goto none;
    ID3D11DeviceContext_OMGetDepthStencilState(ctx, &T.m_ds, &T.m_sref);
    v = ds_variant(dev, T.m_ds, kind, &cached);
    if (!v) goto none;
    T.m_uncached = cached ? NULL : v;
    if (kind == 1) {
        if (T.obj_seq != seq || !T.obj_used) {
            ID3D11DeviceContext_ClearRenderTargetView(ctx, T.obj_rtv, zero);
            T.obj_seq = seq; T.obj_used = 1;
        }
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)T.cb_obj, 0, NULL, g_d3d8_temporal_delta, 0, 0);
        ID3D11DeviceContext_PSGetShader(ctx, &T.m_ps, NULL, NULL);
        ID3D11DeviceContext_OMGetBlendState(ctx, &T.m_bs, T.m_bf, &T.m_sm);
        ID3D11DeviceContext_PSSetShader(ctx, T.ps_obj, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 5, 1, &T.cb_obj);
        ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &T.obj_rtv, T.m_dsv);
        T.mir_obj++;
        if (g_d3d8_temporal_delta[3] < 0.75f) T.mir_new++;
    } else {
        if (T.rea_seq != seq || !T.rea_used) {
            ID3D11DeviceContext_ClearRenderTargetView(ctx, T.rea_rtv, zero);
            T.rea_seq = seq; T.rea_used = 1;
        }
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &T.rea_rtv, T.m_dsv);
        T.mir_rea++;
    }
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, v, T.m_sref);
    T.m_kind = kind;
    return 1;
none:
    m_clear();
    return 0;
}

void d3d8_temporal_mirror_end(ID3D11DeviceContext *ctx)
{
    ID3D11RenderTargetView *rt[2];
    ID3D11Buffer *nocb = NULL;
    rt[0] = T.m_rtv; rt[1] = T.m_rtv1;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, T.m_rtv1 ? 2 : 1, rt, T.m_dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, T.m_ds, T.m_sref);
    if (T.m_kind == 1) {
        ID3D11DeviceContext_PSSetShader(ctx, T.m_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 5, 1, &nocb);
        ID3D11DeviceContext_OMSetBlendState(ctx, T.m_bs, T.m_bf, T.m_sm);
    }
    m_clear();
}

/* ── The camera ────────────────────────────────────────────────────── */

/* Inverse of a 4x4 (Gauss-Jordan, partial pivot); 0 when singular. */
static int inv4(const float *m, float *o)
{
    double a[4][8];
    int r, c, k;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (c = 0; c < 4; c++) {
        int p = c;
        double d;
        for (r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (!(fabs(a[p][c]) > 1e-12)) return 0;
        if (p != c) for (k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        d = a[c][c];
        for (k = 0; k < 8; k++) a[c][k] /= d;
        for (r = 0; r < 4; r++) {
            double f = a[r][c];
            if (r == c || f == 0.0) continue;
            for (k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) o[r * 4 + c] = (float)a[r][c + 4];
    return 1;
}

static void mul4(const float *a, const float *b, float *o)     /* o = a x b (rows) */
{
    int r, c, k;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
}

static void cam_pos(const float *m, float *c)
{
    int i;
    for (i = 0; i < 3; i++) c[i] = -(m[12] * m[4 * i] + m[13] * m[4 * i + 1] + m[14] * m[4 * i + 2]);
}

/* A cut between two views: turned by more than 60 degrees or moved more
 * than 400 units (as the camera blur). */
static int is_cut(const float *a, const float *b)
{
    float ca[3], cb[3], tr = 0.0f, d;
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) tr += a[4 * i + j] * b[4 * i + j];
    cam_pos(a, ca);
    cam_pos(b, cb);
    d = sqrtf((ca[0] - cb[0]) * (ca[0] - cb[0]) + (ca[1] - cb[1]) * (ca[1] - cb[1]) + (ca[2] - cb[2]) * (ca[2] - cb[2]));
    return !(0.5f * (tr - 1.0f) > CUT_COS) || !(d < CUT_DIST);
}

/* ── FRAME_END ─────────────────────────────────────────────────────── */

typedef struct {
    ID3D11RenderTargetView   *rtv[2];
    ID3D11DepthStencilView   *dsv;
    D3D11_VIEWPORT            vp;
    UINT                      nvp;
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps;
    ID3D11GeometryShader     *gs;
    ID3D11InputLayout        *il;
    D3D11_PRIMITIVE_TOPOLOGY  topo;
    ID3D11ShaderResourceView *srv[5];
    ID3D11Buffer             *pscb, *vscb;
    ID3D11BlendState         *bs;
    FLOAT                     bf[4];
    UINT                      sm;
    ID3D11DepthStencilState  *ds;
    UINT                      sref;
    ID3D11RasterizerState    *rs;
} t_saved;

static void state_save(ID3D11DeviceContext *ctx, t_saved *o)
{
    memset(o, 0, sizeof *o);
    o->nvp = 1;
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 2, o->rtv, &o->dsv);
    ID3D11DeviceContext_RSGetViewports(ctx, &o->nvp, &o->vp);
    ID3D11DeviceContext_VSGetShader(ctx, &o->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(ctx, &o->ps, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(ctx, &o->gs, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(ctx, &o->il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(ctx, &o->topo);
    ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 5, o->srv);
    ID3D11DeviceContext_PSGetConstantBuffers(ctx, 0, 1, &o->pscb);
    ID3D11DeviceContext_VSGetConstantBuffers(ctx, 0, 1, &o->vscb);
    ID3D11DeviceContext_OMGetBlendState(ctx, &o->bs, o->bf, &o->sm);
    ID3D11DeviceContext_OMGetDepthStencilState(ctx, &o->ds, &o->sref);
    ID3D11DeviceContext_RSGetState(ctx, &o->rs);
}

static void state_restore(ID3D11DeviceContext *ctx, t_saved *o)
{
    int i;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, o->rtv[1] ? 2 : 1, o->rtv, o->dsv);
    if (o->nvp) ID3D11DeviceContext_RSSetViewports(ctx, 1, &o->vp);
    ID3D11DeviceContext_VSSetShader(ctx, o->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, o->ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, o->gs, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, o->il);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, o->topo);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, o->srv);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &o->pscb);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &o->vscb);
    ID3D11DeviceContext_OMSetBlendState(ctx, o->bs, o->bf, o->sm);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, o->ds, o->sref);
    ID3D11DeviceContext_RSSetState(ctx, o->rs);
    for (i = 0; i < 2; i++) if (o->rtv[i]) ID3D11RenderTargetView_Release(o->rtv[i]);
    if (o->dsv) ID3D11DepthStencilView_Release(o->dsv);
    if (o->vs) ID3D11VertexShader_Release(o->vs);
    if (o->ps) ID3D11PixelShader_Release(o->ps);
    if (o->gs) ID3D11GeometryShader_Release(o->gs);
    if (o->il) ID3D11InputLayout_Release(o->il);
    for (i = 0; i < 5; i++) if (o->srv[i]) ID3D11ShaderResourceView_Release(o->srv[i]);
    if (o->pscb) ID3D11Buffer_Release(o->pscb);
    if (o->vscb) ID3D11Buffer_Release(o->vscb);
    if (o->bs) ID3D11BlendState_Release(o->bs);
    if (o->ds) ID3D11DepthStencilState_Release(o->ds);
    if (o->rs) ID3D11RasterizerState_Release(o->rs);
}

static void fullscreen_setup(ID3D11DeviceContext *ctx, UINT w, UINT h)
{
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (FLOAT)w; vp.Height = (FLOAT)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, T.rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_VSSetShader(ctx, T.vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &T.cb);
}

void d3d8_temporal_frame_end(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                             ID3D11ShaderResourceView *depth_srv, ID3D11Texture2D *depth_tex,
                             UINT w, UINT h, unsigned tag)
{
    ID3D11ShaderResourceView *none[5] = { NULL, NULL, NULL, NULL, NULL }, *in[3];
    ID3D11RenderTargetView *rt[2];
    D3D11_TEXTURE2D_DESC dd;
    float cam[27], cb[48], m[16], inv[16];
    unsigned seq = d3d8_PresentSeq();
    int valid = 0, ms = 0, has_obj, has_rea, jit = T.jit_on;
    t_saved saved;

    T.jit_on = 0;                       /* the overlay is never jittered */
    T.have_in = 0;
    if (!d3d8_temporal_active() || !dev || !ctx || !w || !h || !base_init(dev)) return;
    if (!jit) return;                   /* an image that was not jittered: no inputs */
    T.images++;

    /* the camera of this image and of the previous one */
    memset(cam, 0, sizeof cam);
    if (T.src && T.src(tag & 0x1FFFu, cam) && cam[24] > 0.5f) valid = 1;
    if (valid) {
        if (T.have_last) {
            memcpy(T.v_prev, T.v_last, sizeof T.v_prev);
            T.tan_prev = T.tan_last; T.asp_prev = T.asp_last;
            T.have_prev = 1;
            T.cut = is_cut(T.v_prev, cam);
        }
        memcpy(T.v_last, cam, sizeof T.v_last);
        T.tan_last = cam[16]; T.asp_last = cam[17];
        T.have_last = 1;
    } else {
        T.have_last = T.have_prev = 0;
    }
    T.in.reset = !valid || !T.have_prev || T.cut;
    if (T.in.reset) T.resets++;

    if (!out_ensure(dev, w, h)) return;
    if (depth_tex) { ID3D11Texture2D_GetDesc(depth_tex, &dd); ms = dd.SampleDesc.Count > 1; }
    has_obj = T.obj_used && T.obj_seq == seq && T.tw == w && T.th == h && (T.tmsaa > 1) == ms;
    has_rea = T.rea_used && T.rea_seq == seq && T.tw == w && T.th == h;
    if (has_rea && T.rea_res)
        ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *)T.rea_res, 0,
                                               (ID3D11Resource *)T.rea, 0, DXGI_FORMAT_R8G8B8A8_UNORM);

    memset(cb, 0, sizeof cb);
    cb[0] = (float)w; cb[1] = (float)h; cb[2] = 1.0f / (float)w; cb[3] = 1.0f / (float)h;
    if (!T.in.reset && depth_srv && inv4(T.v_last, inv)) {
        mul4(inv, T.v_prev, m);
        cb[4] = T.tan_last; cb[5] = T.asp_last; cb[6] = T.tan_prev; cb[7] = T.asp_prev;
        memcpy(&cb[8], m, sizeof m);
        memcpy(&cb[24], &T.v_prev[0], 4 * sizeof(float));
        memcpy(&cb[28], &T.v_prev[4], 4 * sizeof(float));
        memcpy(&cb[32], &T.v_prev[8], 4 * sizeof(float));
        cb[38] = 1.0f;
    }
    cb[36] = cam[25] > 0.0f ? cam[25] : 25.0f;
    cb[37] = cam[26] > cb[36] ? cam[26] : 30000.0f;
    if (T.near_env > 0.0f) cb[36] = T.near_env;
    if (T.far_env > cb[36]) cb[37] = T.far_env;
    cb[39] = T.gain;
    cb[40] = T.jit_ndc[0] * 0.5f;       /* the jitter in uv: x right, y down */
    cb[41] = -T.jit_ndc[1] * 0.5f;
    cb[42] = has_obj ? 1.0f : 0.0f;
    cb[43] = has_rea ? 1.0f : 0.0f;
    cb[44] = (float)T.view;
    cb[45] = T.rigid ? 1.0f : 0.0f;

    state_save(ctx, &saved);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)T.cb, 0, NULL, cb, 0, 0);
    fullscreen_setup(ctx, w, h);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, none);
    rt[0] = T.mv_rtv; rt[1] = T.rm_rtv;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 2, rt, NULL);
    in[0] = depth_srv;
    in[1] = has_obj ? T.obj_srv : NULL;
    in[2] = has_rea ? T.rea_srv : NULL;
    ID3D11DeviceContext_PSSetShader(ctx, T.ps_mv[ms], NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, in);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, none);
    state_restore(ctx, &saved);

    T.in.mv = T.mv_srv;
    T.in.reactive = T.rm_srv;
    T.in.depth = depth_srv;
    T.in.depth_ms = ms;
    T.in.w = w; T.in.h = h;
    T.in.jitter_px[0] = T.jit_px[0];
    T.in.jitter_px[1] = T.jit_px[1];
    T.in.phases = T.phases;
    T.have_in = 1;
    T.in_seq = seq;
    if (T.log && T.images % 300u == 1u)
        fprintf(stderr, "[TEMPORAL] image %llu: %ux%u%s, jitter %u/%d (%.3f, %.3f) px, bias %.2f, camera %d, "
                "reset %d (%llu resets), object %d, reactive %d; mirrors: %llu reactive, %llu rider "
                "(%llu without history)\n",
                T.images, w, h, ms ? " ms" : "", T.index, T.phases, T.jit_px[0], T.jit_px[1], T.bias,
                valid, T.in.reset, T.resets, has_obj, has_rea, T.mir_rea, T.mir_obj, T.mir_new);
}

void d3d8_temporal_show(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                        ID3D11RenderTargetView *rtv, UINT w, UINT h)
{
    ID3D11ShaderResourceView *none[5] = { NULL, NULL, NULL, NULL, NULL }, *in[5];
    t_saved saved;
    int ms;
    if (!T.view || !T.have_in || T.in_seq != d3d8_PresentSeq() || !dev || !ctx || !rtv ||
        w != T.in.w || h != T.in.h || !base_init(dev))
        return;
    ms = T.in.depth_ms;
    if (T.view == 3 && !(T.obj_used && T.obj_seq == T.in_seq && (T.tmsaa > 1) == ms)) return;
    state_save(ctx, &saved);
    fullscreen_setup(ctx, w, h);
    ID3D11DeviceContext_OMSetBlendState(ctx, T.bs_show, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    in[0] = NULL;
    in[1] = T.view == 3 ? T.obj_srv : NULL;
    in[2] = NULL;
    in[3] = T.mv_srv;
    in[4] = T.rm_srv;
    ID3D11DeviceContext_PSSetShader(ctx, T.ps_show[ms], NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, in);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, none);
    state_restore(ctx, &saved);
}

int d3d8_temporal_get(d3d8_temporal_inputs *o)
{
    if (!T.have_in || T.in_seq != d3d8_PresentSeq()) return 0;
    *o = T.in;
    return 1;
}

/* ── XBOX_TEMPORAL_PROBE=<file> (diagnostic) ───────────────────────────
 * Checks the vectors against the images themselves. Every 30th jittered
 * image, its 3D colour (before the post) and the one of the image before
 * are read back with the vectors (and the riders' object target when it is
 * single-sampled), and the previous image is warped onto this one, one
 * pixel in 4 x 4:
 *   e0    |cur - prev| at the same pixel (no motion);
 *   emv   prev taken at the pixel + mv, both jitters compensated;
 *   eneg  the same with -mv (a sign error would win here);
 *   enoj  mv without the jitters;
 * mean absolute error per channel (0..255) on all pixels, on those that
 * move more than 1 pixel, and on the riders' matched pixels; then the mean
 * vector (pixels) of a 3 x 3 grid of the screen. Stalls the pump on those
 * images. */
static struct {
    int       on, failed;
    FILE     *f;
    ID3D11Texture2D *sa, *sb, *sm, *so;
    UINT      w, h;
    DXGI_FORMAT cfmt;
    unsigned long long prev_img;
    float     jprev[2];
} PR = { .on = -1 };

int d3d8_temporal_probing(void)
{
    if (PR.on < 0) {
        const char *e = NULL;                           /* no file written */
        PR.on = e && *e;
        if (PR.on) PR.f = fopen(e, "w");
        if (!PR.f) PR.on = 0;
    }
    return PR.on && d3d8_temporal_active();
}

static float half_f(uint16_t h)
{
    uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu, u;
    float f;
    if (e == 0) { f = (float)m / 1024.0f / 16384.0f; return s ? -f : f; }
    if (e == 31) u = s | 0x7F800000u | (m << 13);
    else u = s | ((e + 112u) << 23) | (m << 13);
    memcpy(&f, &u, 4);
    return f;
}

static ID3D11Texture2D *staging(ID3D11Device *dev, ID3D11Texture2D *src)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *t = NULL;
    ID3D11Texture2D_GetDesc(src, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    td.MipLevels = 1;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &t))) return NULL;
    return t;
}

/* Bilinear fetch of a 4-byte colour image at pixel coordinates (centres at +0.5). */
static int fetch(const uint8_t *b, UINT pitch, UINT w, UINT h, float x, float y, float *o)
{
    int x0, y0, c;
    float fx, fy;
    x -= 0.5f; y -= 0.5f;
    if (!(x >= 0.0f && y >= 0.0f && x < (float)(w - 1) && y < (float)(h - 1))) return 0;
    x0 = (int)x; y0 = (int)y; fx = x - (float)x0; fy = y - (float)y0;
    for (c = 0; c < 3; c++) {
        const uint8_t *p = b + (size_t)y0 * pitch + (size_t)x0 * 4 + c;
        float a = p[0] + (p[4] - p[0]) * fx, d = p[pitch] + (p[pitch + 4] - p[pitch]) * fx;
        o[c] = a + (d - a) * fy;
    }
    return 1;
}

void d3d8_temporal_probe(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *color)
{
    D3D11_TEXTURE2D_DESC cd;
    D3D11_MAPPED_SUBRESOURCE ma, mb, mm, mo;
    unsigned long long n = T.images;
    int obj_ok, k;
    if (!d3d8_temporal_probing() || PR.failed || !color || !T.have_in || !T.mv) return;
    ID3D11Texture2D_GetDesc(color, &cd);
    if (cd.SampleDesc.Count != 1 || cd.Width != T.in.w || cd.Height != T.in.h) return;
    if (!PR.sa || PR.w != cd.Width || PR.h != cd.Height || PR.cfmt != cd.Format) {
        if (PR.sa) { ID3D11Texture2D_Release(PR.sa); PR.sa = NULL; }
        if (PR.sb) { ID3D11Texture2D_Release(PR.sb); PR.sb = NULL; }
        if (PR.sm) { ID3D11Texture2D_Release(PR.sm); PR.sm = NULL; }
        if (PR.so) { ID3D11Texture2D_Release(PR.so); PR.so = NULL; }
        PR.sa = staging(dev, color);
        PR.sb = staging(dev, color);
        PR.sm = staging(dev, T.mv);
        if (!PR.sa || !PR.sb || !PR.sm) { PR.failed = 1; return; }
        PR.w = cd.Width; PR.h = cd.Height; PR.cfmt = cd.Format;
        PR.prev_img = 0;
    }
    if (n % 30u == 29u) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)PR.sa, (ID3D11Resource *)color);
        PR.jprev[0] = T.in.jitter_px[0]; PR.jprev[1] = T.in.jitter_px[1];
        PR.prev_img = n;
        return;
    }
    if (n % 30u != 0u || PR.prev_img + 1u != n || T.in.reset) return;
    obj_ok = T.obj && T.tmsaa == 1 && T.obj_used && T.obj_seq == T.in_seq && T.tw == PR.w && T.th == PR.h;
    if (obj_ok && !PR.so) PR.so = staging(dev, T.obj);
    obj_ok = obj_ok && PR.so;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)PR.sb, (ID3D11Resource *)color);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)PR.sm, (ID3D11Resource *)T.mv);
    if (obj_ok) ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)PR.so, (ID3D11Resource *)T.obj);
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)PR.sa, 0, D3D11_MAP_READ, 0, &ma))) return;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)PR.sb, 0, D3D11_MAP_READ, 0, &mb))) {
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)PR.sa, 0);
        return;
    }
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)PR.sm, 0, D3D11_MAP_READ, 0, &mm))) {
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)PR.sa, 0);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)PR.sb, 0);
        return;
    }
    if (obj_ok && FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)PR.so, 0, D3D11_MAP_READ, 0, &mo))) obj_ok = 0;
    {
        /* [set][kind]: set 0 all, 1 moving > 1 px, 2 riders; kind 0 e0, 1 emv, 2 eneg, 3 enoj */
        double e[3][4], cnt[3][4], gx[9], gy[9], gn[9], mvsum = 0.0, mvn = 0.0;
        UINT x, y, W = PR.w, H = PR.h;
        memset(e, 0, sizeof e); memset(cnt, 0, sizeof cnt);
        memset(gx, 0, sizeof gx); memset(gy, 0, sizeof gy); memset(gn, 0, sizeof gn);
        for (y = 2; y < H; y += 4)
            for (x = 2; x < W; x += 4) {
                const uint8_t *cp = (const uint8_t *)mb.pData + (size_t)y * mb.RowPitch + (size_t)x * 4;
                const uint16_t *vp = (const uint16_t *)((const uint8_t *)mm.pData + (size_t)y * mm.RowPitch) + 2 * x;
                float mx = half_f(vp[0]) * (float)W, my = half_f(vp[1]) * (float)H, cur[3], pv[3];
                float px = (float)x + 0.5f, py = (float)y + 0.5f;
                float ux = px - T.in.jitter_px[0], uy = py - T.in.jitter_px[1];
                float pos[4][2];
                int set, kind, g, rider = 0;
                if (obj_ok) {
                    const uint16_t *op = (const uint16_t *)((const uint8_t *)mo.pData + (size_t)y * mo.RowPitch) + 4 * x;
                    rider = half_f(op[3]) > 0.75f;
                }
                for (k = 0; k < 3; k++) cur[k] = cp[k];
                pos[0][0] = px; pos[0][1] = py;
                pos[1][0] = ux + mx + PR.jprev[0]; pos[1][1] = uy + my + PR.jprev[1];
                pos[2][0] = ux - mx + PR.jprev[0]; pos[2][1] = uy - my + PR.jprev[1];
                pos[3][0] = px + mx; pos[3][1] = py + my;
                for (kind = 0; kind < 4; kind++) {
                    double d;
                    if (!fetch((const uint8_t *)ma.pData, ma.RowPitch, W, H, pos[kind][0], pos[kind][1], pv)) continue;
                    d = (fabs(cur[0] - pv[0]) + fabs(cur[1] - pv[1]) + fabs(cur[2] - pv[2])) / 3.0;
                    for (set = 0; set < 3; set++) {
                        if (set == 1 && !(mx * mx + my * my > 1.0f)) continue;
                        if (set == 2 && !rider) continue;
                        e[set][kind] += d;
                        cnt[set][kind] += 1.0;
                    }
                }
                g = (int)(3u * y / H) * 3 + (int)(3u * x / W);
                gx[g] += mx; gy[g] += my; gn[g] += 1.0;
                mvsum += sqrt((double)mx * mx + (double)my * my);
                mvn += 1.0;
            }
        fprintf(PR.f, "T %llu %ux%u jit %.3f %.3f prev %.3f %.3f |mv| %.2f", n, W, H,
                T.in.jitter_px[0], T.in.jitter_px[1], PR.jprev[0], PR.jprev[1], mvn > 0.0 ? mvsum / mvn : 0.0);
        for (k = 0; k < 3; k++) {
            int kind;
            fprintf(PR.f, " | %s n %.0f", k == 0 ? "all" : k == 1 ? "moving" : "rider", cnt[k][0]);
            for (kind = 0; kind < 4; kind++)
                fprintf(PR.f, " %.2f", cnt[k][kind] > 0.0 ? e[k][kind] / cnt[k][kind] : -1.0);
        }
        fprintf(PR.f, " | grid");
        for (k = 0; k < 9; k++)
            fprintf(PR.f, " %.1f,%.1f", gn[k] > 0.0 ? gx[k] / gn[k] : 0.0, gn[k] > 0.0 ? gy[k] / gn[k] : 0.0);
        fprintf(PR.f, "\n");
        fflush(PR.f);
    }
    if (obj_ok) ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)PR.so, 0);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)PR.sm, 0);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)PR.sb, 0);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)PR.sa, 0);
}
