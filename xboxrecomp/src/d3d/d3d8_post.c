/*
 * d3d8_post -- end-of-frame post-processing chain (fork).
 * See d3d8_post.h for what it does and where it sits in the present.
 *
 * SMAA 1x is Jorge Jimenez et al.'s reference implementation, used
 * unmodified from smaa/SMAA.hlsl (MIT licence, smaa/LICENSE.txt and the
 * notice at the top of the file; provenance in smaa/README.md).
 */
#include "d3d8_internal.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_post.h"
#include "d3d8_gpuprof.h"
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"

/* ── Configuration ─────────────────────────────────────────────────── */

static volatile LONG s_post_on = -1;    /* -1 = XBOX_POST not read yet */
static volatile LONG s_smaa = -1;       /* 0 off, 1..4 = low..ultra; -1 = not read */
static volatile LONG s_split = -1;      /* XBOX_POST_SPLIT; -1 = not read */
static LONG          s_cycle;           /* XBOX_POST_CYCLE=N: diagnostic mode cycle */
static LONG          s_cycle_smaa = 3;  /* the SMAA preset the cycle uses */

static void config_read(void)
{
    if (s_post_on < 0) {
        const char *e = getenv("XBOX_POST");
        InterlockedCompareExchange(&s_post_on, (e && e[0] == '1') ? 1 : 0, -1);
    }
    if (s_smaa < 0) {
        const char *e = getenv("XBOX_SMAA");
        const char *p = getenv("XBOX_SMAA_PRESET");
        LONG v = 0;
        if (e && e[0] == '1') {
            v = 3;                                      /* high, the default */
            if (p && !strcmp(p, "low"))    v = 1;
            if (p && !strcmp(p, "medium")) v = 2;
            if (p && !strcmp(p, "ultra"))  v = 4;
        }
        InterlockedCompareExchange(&s_smaa, v, -1);
        if (v > 0) s_cycle_smaa = v;
    }
    if (s_split < 0) {
        const char *e = getenv("XBOX_POST_SPLIT");
        const char *c = getenv("XBOX_POST_CYCLE");
        s_cycle = (c && atoi(c) > 0) ? atoi(c) : 0;
        /* SMAA implies the split (HUD kept sharp);
         * an explicit XBOX_POST_SPLIT=0 turns it off. */
        InterlockedCompareExchange(&s_split, (e && e[0]) ? (e[0] == '1') : (s_smaa > 0), -1);
    }
}

int d3d8_post_enabled(void)
{
    config_read();
    return s_post_on == 1 || s_smaa > 0;
}

void d3d8_SetPostProcess(int on) { config_read(); InterlockedExchange(&s_post_on, on ? 1 : 0); }
int  d3d8_GetPostProcess(void)   { config_read(); return s_post_on == 1; }
void d3d8_SetSmaa(int preset)    { config_read(); InterlockedExchange(&s_smaa, (preset >= 1 && preset <= 4) ? preset : 0); }
int  d3d8_GetSmaa(void)          { config_read(); return (int)s_smaa; }
void d3d8_SetPostSplit(int on)   { config_read(); InterlockedExchange(&s_split, on ? 1 : 0); }
int  d3d8_GetPostSplit(void)     { config_read(); return s_split == 1; }

int d3d8_post_split_active(void)
{
    config_read();
    return s_split == 1 && d3d8_post_enabled();
}

int d3d8_post_split_wanted(void)
{
    config_read();
    return s_split == 1 || s_cycle > 0;
}

/* XBOX_POST_CYCLE=N (diagnostic): every N presents the next of three
 * modes -- 0 no post, 1 SMAA on the whole image, 2 SMAA on the 3D only
 * (split) -- so one paused screen gives the same frame in all three.
 * Returns the mode of this present, or -1 when the cycle is off. */
int d3d8_post_cycle(unsigned present_no)
{
    int mode;
    config_read();
    if (s_cycle <= 0) return -1;
    mode = (int)((present_no / (unsigned)s_cycle) % 3u);
    InterlockedExchange(&s_smaa, mode == 0 ? 0 : s_cycle_smaa);
    InterlockedExchange(&s_split, mode == 2 ? 1 : 0);
    return mode;
}

/* ── Shader sources ────────────────────────────────────────────────── */

/* The chain's own passes. b0 = { width, height, 1/width, 1/height }. */
static const char s_basic_hlsl[] =
    "cbuffer PostConstants : register(b0) { float4 post_size; };\n"
    "Texture2D t0 : register(t0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "    V o;\n"
    "    o.uv = float2((id << 1) & 2, id & 2);\n"
    "    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    /* Identity: the texel under the pixel, all four channels, by Load -- no
     * filtering, so UNORM8 in, float, UNORM8 out is exact. */
    "float4 identity_ps(V i) : SV_Target { return t0.Load(int3(i.p.xy, 0)); }\n";

/* SMAA, compiled as SMAA_CUSTOM_SL: the porting macros are the reference's
 * HLSL 4.1 ones, with the two samplers bound to explicit registers (s0 linear,
 * s1 point; the reference's effect-framework sampler blocks would be ignored
 * by D3DCompile). Every texture read and write is non-sRGB: edge detection
 * needs gamma-encoded values (SMAA.hlsl, note 5), and the blending happens in
 * gamma space too -- the title's own blending and the MSAA resolve are in
 * gamma space, so this keeps SMAA's edge colours consistent with them. */
static const char s_smaa_prefix[] =
    "cbuffer PostConstants : register(b0) { float4 post_size; };\n"
    "#define SMAA_RT_METRICS float4(post_size.zw, post_size.xy)\n"
    "#define SMAA_CUSTOM_SL 1\n"
    "SamplerState LinearSampler : register(s0);\n"
    "SamplerState PointSampler : register(s1);\n"
    "#define SMAATexture2D(tex) Texture2D tex\n"
    "#define SMAATexturePass2D(tex) tex\n"
    "#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)\n"
    "#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)\n"
    "#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)\n"
    "#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)\n"
    "#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)\n"
    "#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)\n"
    "#define SMAA_FLATTEN [flatten]\n"
    "#define SMAA_BRANCH [branch]\n"
    "#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)\n";

static const char s_smaa_body[] = {
#embed "smaa/SMAA.hlsl"
    , 0
};

static const char s_smaa_suffix[] =
    "\nTexture2D t0 : register(t0);\n"
    "Texture2D t1 : register(t1);\n"
    "Texture2D t2 : register(t2);\n"
    "void fullscreen(uint id, out float4 p, out float2 uv) {\n"
    "    uv = float2((id << 1) & 2, id & 2);\n"
    "    p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "struct VE { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 off[3] : TEXCOORD1; };\n"
    "VE smaa_edge_vs(uint id : SV_VertexID) {\n"
    "    VE o; fullscreen(id, o.p, o.uv); SMAAEdgeDetectionVS(o.uv, o.off); return o;\n"
    "}\n"
    "float2 smaa_edge_ps(VE i) : SV_Target {\n"
    "    return SMAALumaEdgeDetectionPS(i.uv, i.off, t0);\n"            /* t0 = colour */
    "}\n"
    "struct VW { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1;\n"
    "            float4 off[3] : TEXCOORD2; };\n"
    "VW smaa_weight_vs(uint id : SV_VertexID) {\n"
    "    VW o; fullscreen(id, o.p, o.uv); SMAABlendingWeightCalculationVS(o.uv, o.pix, o.off); return o;\n"
    "}\n"
    "float4 smaa_weight_ps(VW i) : SV_Target {\n"                       /* edges, area, search */
    "    return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.off, t0, t1, t2, float4(0, 0, 0, 0));\n"
    "}\n"
    "struct VN { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 off : TEXCOORD1; };\n"
    "VN smaa_blend_vs(uint id : SV_VertexID) {\n"
    "    VN o; fullscreen(id, o.p, o.uv); SMAANeighborhoodBlendingVS(o.uv, o.off); return o;\n"
    "}\n"
    "float4 smaa_blend_ps(VN i) : SV_Target {\n"                        /* colour, weights */
    "    return SMAANeighborhoodBlendingPS(i.uv, i.off, t0, t1);\n"
    "}\n";

static const char *const s_smaa_preset_define[5] = {
    NULL, "SMAA_PRESET_LOW", "SMAA_PRESET_MEDIUM", "SMAA_PRESET_HIGH", "SMAA_PRESET_ULTRA"
};

/* ── Passes ────────────────────────────────────────────────────────── */

enum { SRC_BASIC, SRC_SMAA };
enum { GRP_IDENTITY, GRP_SMAA };
/* Inputs (t0..t2) and outputs. COLOR is the current image: the chain's input,
 * then the output of the last pass that wrote colour. */
enum { IN_NONE, IN_COLOR, IN_EDGES, IN_WEIGHTS, IN_AREA, IN_SEARCH };
enum { OUT_COLOR, OUT_EDGES, OUT_WEIGHTS, OUT_COUNT };

typedef struct post_pass {
    const char         *name;
    int                 group;          /* decides whether the pass is active */
    int                 src;            /* which source holds its entry points */
    const char         *vs_entry, *ps_entry;
    int                 out;            /* OUT_*: colour ping-pong or an aux target */
    int                 clear;          /* clear the target to 0 first */
    int                 in[3];          /* IN_* bound to t0, t1, t2 */
    ID3D11VertexShader *vs;
    ID3D11PixelShader  *ps;
} post_pass;

/* The chain, in order. */
static post_pass s_passes[] = {
    { "identity",     GRP_IDENTITY, SRC_BASIC, "vs", "identity_ps",
      OUT_COLOR,   0, { IN_COLOR, IN_NONE, IN_NONE }, NULL, NULL },
    { "smaa_edges",   GRP_SMAA, SRC_SMAA, "smaa_edge_vs", "smaa_edge_ps",
      OUT_EDGES,   1, { IN_COLOR, IN_NONE, IN_NONE }, NULL, NULL },
    { "smaa_weights", GRP_SMAA, SRC_SMAA, "smaa_weight_vs", "smaa_weight_ps",
      OUT_WEIGHTS, 1, { IN_EDGES, IN_AREA, IN_SEARCH }, NULL, NULL },
    { "smaa_blend",   GRP_SMAA, SRC_SMAA, "smaa_blend_vs", "smaa_blend_ps",
      OUT_COLOR,   0, { IN_COLOR, IN_WEIGHTS, IN_NONE }, NULL, NULL },
};
#define POST_NPASSES ((int)(sizeof s_passes / sizeof s_passes[0]))

/* Identity only as the chain's self-check: with SMAA on it would be a
 * wasted copy. */
static int pass_active(const post_pass *p)
{
    if (p->group == GRP_SMAA) return s_smaa > 0;
    return s_post_on == 1 && s_smaa <= 0;
}

/* ── Resources ─────────────────────────────────────────────────────── */

static struct {
    int                       failed;       /* fixed setup failed: never retried */
    int                       smaa_failed;  /* SMAA shaders failed for smaa_preset */
    int                       smaa_preset;  /* preset the SMAA shaders were built for */
    ID3D11SamplerState       *smp_linear, *smp_point;
    ID3D11RasterizerState    *rs;
    ID3D11Buffer             *cb;
    ID3D11Texture2D          *area_tex, *search_tex;
    ID3D11ShaderResourceView *area_srv, *search_srv;
    UINT                      w, h;         /* size of the targets */
    ID3D11Texture2D          *tex[2];       /* colour ping-pong */
    ID3D11RenderTargetView   *rtv[2];
    ID3D11ShaderResourceView *srv[2];
    ID3D11Texture2D          *aux_tex[OUT_COUNT];   /* [OUT_EDGES], [OUT_WEIGHTS] */
    ID3D11RenderTargetView   *aux_rtv[OUT_COUNT];
    ID3D11ShaderResourceView *aux_srv[OUT_COUNT];
    unsigned                  last_frame;
    int                       last_valid, last_index;
    /* Stencil of SMAA's edges (XBOX_FIX_POST_STENCIL) */
    ID3D11Texture2D          *st_tex;
    ID3D11DepthStencilView   *st_dsv;
    ID3D11DepthStencilState  *st_write, *st_test;
    int                       st_failed;
} P;

/* GPU saving of the chain, identical to the pixel and on by
 * default (0 = the previous path).
 *   XBOX_FIX_POST_STENCIL: the edge pass marks its pixels in a stencil -- its
 *     pixel shader already discards those without an edge -- and the weight
 *     pass runs on them only; elsewhere its target keeps the 0 of its clear,
 *     which is what the shader returns without an edge (SMAA.hlsl, note 9).
 *     Post -5 % on the laptop's iGPU at 1080p (2.87 -> 2.73 ms).
 *   (Writing the last pass straight into the title's target from a copy, to
 *   drop the write-back draw, was measured slower there and left out.) */
static int s_fix_stencil = -1;
static int fix_flag(int *v, const char *name)
{
    if (*v < 0) {
        const char *e = getenv(name);
        *v = !(e && e[0] == '0');
        fprintf(stderr, "[POST] %s=%d\n", name, *v);
    }
    {   /* XBOX_FIX_PUMP_ALT=1 (identity test): off every other image */
        extern int d3d8_pump_alt_off(void);
        return *v && !d3d8_pump_alt_off();
    }
}

static void targets_release(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (P.srv[i]) { ID3D11ShaderResourceView_Release(P.srv[i]); P.srv[i] = NULL; }
        if (P.rtv[i]) { ID3D11RenderTargetView_Release(P.rtv[i]); P.rtv[i] = NULL; }
        if (P.tex[i]) { ID3D11Texture2D_Release(P.tex[i]); P.tex[i] = NULL; }
    }
    for (i = 0; i < OUT_COUNT; i++) {
        if (P.aux_srv[i]) { ID3D11ShaderResourceView_Release(P.aux_srv[i]); P.aux_srv[i] = NULL; }
        if (P.aux_rtv[i]) { ID3D11RenderTargetView_Release(P.aux_rtv[i]); P.aux_rtv[i] = NULL; }
        if (P.aux_tex[i]) { ID3D11Texture2D_Release(P.aux_tex[i]); P.aux_tex[i] = NULL; }
    }
    if (P.st_dsv) { ID3D11DepthStencilView_Release(P.st_dsv); P.st_dsv = NULL; }
    if (P.st_tex) { ID3D11Texture2D_Release(P.st_tex); P.st_tex = NULL; }
    P.w = P.h = 0;
    P.last_valid = 0;
}

static void pass_shaders_release(int src)
{
    int i;
    for (i = 0; i < POST_NPASSES; i++) {
        if (s_passes[i].src != src) continue;
        if (s_passes[i].vs) { ID3D11VertexShader_Release(s_passes[i].vs); s_passes[i].vs = NULL; }
        if (s_passes[i].ps) { ID3D11PixelShader_Release(s_passes[i].ps); s_passes[i].ps = NULL; }
    }
}

void d3d8_post_release(void)
{
    targets_release();
    pass_shaders_release(SRC_BASIC);
    pass_shaders_release(SRC_SMAA);
    P.smaa_preset = 0;
    P.smaa_failed = 0;
    if (P.smp_linear) { ID3D11SamplerState_Release(P.smp_linear); P.smp_linear = NULL; }
    if (P.smp_point)  { ID3D11SamplerState_Release(P.smp_point); P.smp_point = NULL; }
    if (P.rs)  { ID3D11RasterizerState_Release(P.rs); P.rs = NULL; }
    if (P.cb)  { ID3D11Buffer_Release(P.cb); P.cb = NULL; }
    if (P.area_srv)   { ID3D11ShaderResourceView_Release(P.area_srv); P.area_srv = NULL; }
    if (P.area_tex)   { ID3D11Texture2D_Release(P.area_tex); P.area_tex = NULL; }
    if (P.search_srv) { ID3D11ShaderResourceView_Release(P.search_srv); P.search_srv = NULL; }
    if (P.search_tex) { ID3D11Texture2D_Release(P.search_tex); P.search_tex = NULL; }
    if (P.st_write) { ID3D11DepthStencilState_Release(P.st_write); P.st_write = NULL; }
    if (P.st_test)  { ID3D11DepthStencilState_Release(P.st_test); P.st_test = NULL; }
    P.st_failed = 0;
}

static ID3D10Blob *compile(const char *src, size_t len, const char *name, const char *entry,
                           const char *target, const D3D_SHADER_MACRO *defs)
{
    ID3D10Blob *code = NULL, *err = NULL;
    if (FAILED(D3DCompile(src, len, name, defs, NULL, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        fprintf(stderr, "[POST] shader %s/%s: %s\n", name, entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "compile failed");
        if (err) ID3D10Blob_Release(err);
        if (code) ID3D10Blob_Release(code);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

/* Compile the passes of one source. */
static int pass_shaders_build(ID3D11Device *dev, int src, const char *text, size_t len,
                              const D3D_SHADER_MACRO *defs)
{
    int i;
    for (i = 0; i < POST_NPASSES; i++) {
        post_pass *p = &s_passes[i];
        ID3D10Blob *code;
        if (p->src != src) continue;
        code = compile(text, len, p->name, p->vs_entry, "vs_5_0", defs);
        if (!code) return 0;
        if (FAILED(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), NULL, &p->vs))) { ID3D10Blob_Release(code); return 0; }
        ID3D10Blob_Release(code);
        code = compile(text, len, p->name, p->ps_entry, "ps_5_0", defs);
        if (!code) return 0;
        if (FAILED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), NULL, &p->ps))) { ID3D10Blob_Release(code); return 0; }
        ID3D10Blob_Release(code);
    }
    return 1;
}

static int lookup_texture(ID3D11Device *dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT pitch,
                          const void *bytes, ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = bytes; sd.SysMemPitch = pitch; sd.SysMemSlicePitch = 0;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, &sd, tex)) &&
           SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)*tex, NULL, srv));
}

/* Fixed resources and the chain's own shaders. */
static int base_init(ID3D11Device *dev)
{
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BUFFER_DESC bd;

    if (P.cb) return 1;
    if (P.failed) return 0;
    P.failed = 1;               /* cleared on success */

    if (!pass_shaders_build(dev, SRC_BASIC, s_basic_hlsl, sizeof s_basic_hlsl - 1, NULL))
        goto fail;

    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;  /* SMAA's LinearSampler */
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(dev, &sd, &P.smp_linear))) goto fail;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;         /* SMAA's PointSampler */
    if (FAILED(ID3D11Device_CreateSamplerState(dev, &sd, &P.smp_point))) goto fail;

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &P.rs))) goto fail;

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &P.cb))) goto fail;

    P.failed = 0;
    return 1;
fail:
    fprintf(stderr, "[POST] could not create the post-process chain: presenting without it\n");
    d3d8_post_release();
    P.failed = 1;
    return 0;
}

/* SMAA's shaders for the current preset, and its lookup textures. A preset
 * change rebuilds the shaders; a failure turns SMAA off until it changes. */
static int smaa_init(ID3D11Device *dev, int preset)
{
    D3D_SHADER_MACRO defs[2];
    size_t lp, lb, ls;
    char *text;
    int ok;

    if (P.smaa_preset == preset) return !P.smaa_failed;
    pass_shaders_release(SRC_SMAA);
    P.smaa_preset = preset;
    P.smaa_failed = 1;

    if (!P.area_srv &&
        !lookup_texture(dev, AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM,
                        AREATEX_PITCH, areaTexBytes, &P.area_tex, &P.area_srv)) goto fail;
    if (!P.search_srv &&
        !lookup_texture(dev, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM,
                        SEARCHTEX_PITCH, searchTexBytes, &P.search_tex, &P.search_srv)) goto fail;

    lp = sizeof s_smaa_prefix - 1;
    lb = sizeof s_smaa_body - 1;
    ls = sizeof s_smaa_suffix - 1;
    text = (char *)malloc(lp + lb + ls + 1);
    if (!text) goto fail;
    memcpy(text, s_smaa_prefix, lp);
    memcpy(text + lp, s_smaa_body, lb);
    memcpy(text + lp + lb, s_smaa_suffix, ls);
    text[lp + lb + ls] = 0;
    defs[0].Name = s_smaa_preset_define[preset]; defs[0].Definition = "1";
    defs[1].Name = NULL; defs[1].Definition = NULL;
    ok = pass_shaders_build(dev, SRC_SMAA, text, lp + lb + ls, defs);
    free(text);
    if (!ok) goto fail;

    P.smaa_failed = 0;
    fprintf(stderr, "[POST] SMAA 1x ready (%s)\n", s_smaa_preset_define[preset]);
    return 1;
fail:
    pass_shaders_release(SRC_SMAA);
    fprintf(stderr, "[POST] SMAA unavailable (%s): presenting without it\n",
            s_smaa_preset_define[preset]);
    return 0;
}

static int target_create(ID3D11Device *dev, UINT w, UINT h, DXGI_FORMAT fmt,
                         ID3D11Texture2D **tex, ID3D11RenderTargetView **rtv,
                         ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, tex)) &&
           SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)*tex, NULL, rtv)) &&
           SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)*tex, NULL, srv));
}

/* The stencil of the edges, at the render size, and its two
 * states (write 1 where the edge pass keeps a pixel; test == 1). Depth off. */
static int stencil_ensure(ID3D11Device *dev, UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_DEPTH_STENCIL_DESC dd;
    if (P.st_failed) return 0;
    if (P.st_dsv && P.st_write && P.st_test) return 1;
    P.st_failed = 1;                            /* cleared on success */
    if (!P.st_write) {
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
        dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
        dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dd.BackFace = dd.FrontFace;
        if (FAILED(ID3D11Device_CreateDepthStencilState(dev, &dd, &P.st_write))) return 0;
        dd.StencilWriteMask = 0;
        dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        dd.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
        dd.BackFace = dd.FrontFace;
        if (FAILED(ID3D11Device_CreateDepthStencilState(dev, &dd, &P.st_test))) return 0;
    }
    if (!P.st_dsv) {
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &P.st_tex)) ||
            FAILED(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)P.st_tex, NULL, &P.st_dsv)))
            return 0;
    }
    P.st_failed = 0;
    return 1;
}

/* Targets at the render size. Colour ping-pong in the scene target's format
 * (R8G8B8A8_UNORM, no sRGB view: no implicit conversion); SMAA's edges
 * (R8G8) and blending weights (R8G8B8A8) beside them. Recreated when the
 * render size changes, and released with everything else when the
 * anti-aliasing changes (d3d8_device.c). */
static int targets_ensure(ID3D11Device *dev, UINT w, UINT h)
{
    int i;
    if (P.tex[0] && P.w == w && P.h == h) return 1;
    targets_release();
    for (i = 0; i < 2; i++)
        if (!target_create(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, &P.tex[i], &P.rtv[i], &P.srv[i]))
            goto fail;
    if (!target_create(dev, w, h, DXGI_FORMAT_R8G8_UNORM, &P.aux_tex[OUT_EDGES],
                       &P.aux_rtv[OUT_EDGES], &P.aux_srv[OUT_EDGES]) ||
        !target_create(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, &P.aux_tex[OUT_WEIGHTS],
                       &P.aux_rtv[OUT_WEIGHTS], &P.aux_srv[OUT_WEIGHTS]))
        goto fail;
    P.w = w;
    P.h = h;
    return 1;
fail:
    targets_release();
    return 0;
}

/* ── Pipeline state save / restore (the blit_draw pattern) ─────────── */

typedef struct {
    ID3D11RenderTargetView   *rtv;
    ID3D11DepthStencilView   *dsv;
    D3D11_VIEWPORT            vp;
    UINT                      nvp;
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps;
    ID3D11InputLayout        *il;
    D3D11_PRIMITIVE_TOPOLOGY  topo;
    ID3D11ShaderResourceView *srv[3];
    ID3D11SamplerState       *smp[2];
    ID3D11Buffer             *pscb, *vscb;
    ID3D11BlendState         *bs;
    FLOAT                     bf[4];
    UINT                      sm;
    ID3D11DepthStencilState  *ds;
    UINT                      sref;
    ID3D11RasterizerState    *rs;
} post_saved;

static void state_save(ID3D11DeviceContext *ctx, post_saved *o)
{
    memset(o, 0, sizeof *o);
    o->nvp = 1;
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &o->rtv, &o->dsv);
    ID3D11DeviceContext_RSGetViewports(ctx, &o->nvp, &o->vp);
    ID3D11DeviceContext_VSGetShader(ctx, &o->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(ctx, &o->ps, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(ctx, &o->il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(ctx, &o->topo);
    ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 3, o->srv);
    ID3D11DeviceContext_PSGetSamplers(ctx, 0, 2, o->smp);
    ID3D11DeviceContext_PSGetConstantBuffers(ctx, 0, 1, &o->pscb);
    ID3D11DeviceContext_VSGetConstantBuffers(ctx, 0, 1, &o->vscb);
    ID3D11DeviceContext_OMGetBlendState(ctx, &o->bs, o->bf, &o->sm);
    ID3D11DeviceContext_OMGetDepthStencilState(ctx, &o->ds, &o->sref);
    ID3D11DeviceContext_RSGetState(ctx, &o->rs);
}

static void state_restore(ID3D11DeviceContext *ctx, post_saved *o)
{
    int i;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &o->rtv, o->dsv);
    if (o->nvp) ID3D11DeviceContext_RSSetViewports(ctx, 1, &o->vp);
    ID3D11DeviceContext_VSSetShader(ctx, o->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, o->ps, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, o->il);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, o->topo);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, o->srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, o->smp);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &o->pscb);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &o->vscb);
    ID3D11DeviceContext_OMSetBlendState(ctx, o->bs, o->bf, o->sm);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, o->ds, o->sref);
    ID3D11DeviceContext_RSSetState(ctx, o->rs);

    if (o->rtv) ID3D11RenderTargetView_Release(o->rtv);
    if (o->dsv) ID3D11DepthStencilView_Release(o->dsv);
    if (o->vs)  ID3D11VertexShader_Release(o->vs);
    if (o->ps)  ID3D11PixelShader_Release(o->ps);
    if (o->il)  ID3D11InputLayout_Release(o->il);
    for (i = 0; i < 3; i++) if (o->srv[i]) ID3D11ShaderResourceView_Release(o->srv[i]);
    for (i = 0; i < 2; i++) if (o->smp[i]) ID3D11SamplerState_Release(o->smp[i]);
    if (o->pscb) ID3D11Buffer_Release(o->pscb);
    if (o->vscb) ID3D11Buffer_Release(o->vscb);
    if (o->bs)  ID3D11BlendState_Release(o->bs);
    if (o->ds)  ID3D11DepthStencilState_Release(o->ds);
    if (o->rs)  ID3D11RasterizerState_Release(o->rs);
}

/* ── Self-test ─────────────────────────────────────────────────────── */

/* XBOX_POST_SELFTEST=1: with the identity as the only pass, the chain must
 * leave every byte unchanged. Reads the chain's input and output back on
 * sampled frames and counts the pixels whose RGBA differs. Diagnostic only --
 * the readback stalls the pump. Skipped while a real effect (SMAA) is on. */
static void selftest(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                     ID3D11Texture2D *src, ID3D11Texture2D *out, unsigned frame_id)
{
    static int on = -1;
    static unsigned left = 0, calls = 0, runs = 0, bad_runs = 0;
    static unsigned long long total_px = 0, total_diff = 0;
    ID3D11Texture2D *sa = NULL, *sb = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE ma, mb;
    unsigned long long diff = 0;
    unsigned maxd = 0, x, y;

    if (on < 0) {
        const char *e = getenv("XBOX_POST_SELFTEST");
        const char *n = getenv("XBOX_POST_SELFTEST_N");
        on = e && e[0] == '1';
        left = (n && atoi(n) > 0) ? (unsigned)atoi(n) : 30u;
    }
    if (!on || !left || s_smaa > 0 || (calls++ % 60u) != 0) return;
    left--;

    ID3D11Texture2D_GetDesc(src, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &sa)) ||
        FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &sb))) goto done;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)sa, (ID3D11Resource *)src);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)sb, (ID3D11Resource *)out);
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)sa, 0, D3D11_MAP_READ, 0, &ma)))
        goto done;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)sb, 0, D3D11_MAP_READ, 0, &mb))) {
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)sa, 0);
        goto done;
    }
    for (y = 0; y < td.Height; y++) {
        const unsigned char *a = (const unsigned char *)ma.pData + (size_t)y * ma.RowPitch;
        const unsigned char *b = (const unsigned char *)mb.pData + (size_t)y * mb.RowPitch;
        for (x = 0; x < td.Width; x++) {
            unsigned c, px = 0;
            for (c = 0; c < 4; c++) {
                unsigned d = (unsigned)abs((int)a[x * 4 + c] - (int)b[x * 4 + c]);
                if (d) { px = 1; if (d > maxd) maxd = d; }
            }
            diff += px;
        }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)sb, 0);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)sa, 0);
    runs++;
    total_px += (unsigned long long)td.Width * td.Height;
    total_diff += diff;
    if (diff) bad_runs++;
    fprintf(stderr, "[POST] selftest #%u frame %u %ux%u: %llu pixel(s) differ (max channel delta %u)"
            " -- total %llu of %llu over %u frames, %u with a difference\n",
            runs, frame_id, td.Width, td.Height, diff, maxd, total_diff, total_px,
            runs, bad_runs);
done:
    if (sa) ID3D11Texture2D_Release(sa);
    if (sb) ID3D11Texture2D_Release(sb);
}

/* ── GPU timing ────────────────────────────────────────────────────── */

/* XBOX_POST_TIMING=1: GPU time of the chain and of each pass, by timestamp
 * queries between the passes. Results are read a few frames later without
 * waiting (ring of 4), averaged and printed every 300 frames as
 * "[POST] gpu ...". The frame rate of a loaded laptop says little about a
 * sub-millisecond pass; this does. */
#define POST_TQ 4
static struct {
    int on;
    ID3D11Query *dj[POST_TQ], *ts[POST_TQ][POST_NPASSES + 1];
    int pass_of[POST_TQ][POST_NPASSES];     /* pass index ending at ts[k+1] */
    int count[POST_TQ];                     /* passes timed in the slot */
    int pending[POST_TQ];
    unsigned slot;
    double sum_ms, max_ms, pass_ms[POST_NPASSES];
    unsigned n;
} T;
static int s_timing_read;              /* XBOX_POST_TIMING read */

static void timing_collect(ID3D11DeviceContext *ctx)
{
    unsigned i;
    int k;
    for (i = 0; i < POST_TQ; i++) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d;
        UINT64 t[POST_NPASSES + 1];
        int ok = 1;
        if (!T.pending[i]) continue;
        if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)T.dj[i], &d, sizeof d,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        for (k = 0; k <= T.count[i] && ok; k++)
            ok = ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)T.ts[i][k], &t[k],
                                             sizeof t[k], D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!ok) continue;
        T.pending[i] = 0;
        if (d.Disjoint || !d.Frequency || t[T.count[i]] < t[0]) continue;
        {
            double ms = (double)(t[T.count[i]] - t[0]) * 1000.0 / (double)d.Frequency;
            T.sum_ms += ms;
            if (ms > T.max_ms) T.max_ms = ms;
            for (k = 0; k < T.count[i]; k++)
                if (t[k + 1] >= t[k])
                    T.pass_ms[T.pass_of[i][k]] += (double)(t[k + 1] - t[k]) * 1000.0
                                                  / (double)d.Frequency;
            if (++T.n == 300) {
                char per[256];
                int j, len = 0;
                per[0] = 0;
                for (j = 0; j < POST_NPASSES; j++)
                    if (T.pass_ms[j] > 0.0 && len < (int)sizeof per - 40)
                        len += snprintf(per + len, sizeof per - (size_t)len, " %s %.3f",
                                        s_passes[j].name, T.pass_ms[j] / T.n);
                fprintf(stderr, "[POST] gpu %.3f ms mean, %.3f ms max over %u frames;%s\n",
                        T.sum_ms / T.n, T.max_ms, T.n, per);
                T.sum_ms = T.max_ms = 0.0;
                for (j = 0; j < POST_NPASSES; j++) T.pass_ms[j] = 0.0;
                T.n = 0;
            }
        }
    }
}

/* Returns the slot to mark with timing_mark / timing_end, or -1 when off. */
static int timing_begin(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    unsigned s;
    int k;
    if (!s_timing_read) {
        const char *e = getenv("XBOX_POST_TIMING");
        s_timing_read = 1;
        T.on = e && e[0] == '1';
    }
    if (!T.on) return -1;
    timing_collect(ctx);
    s = T.slot;
    if (T.pending[s]) return -1;            /* GPU this far behind: skip a sample */
    if (!T.dj[s]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &T.dj[s]))) { T.on = 0; return -1; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        for (k = 0; k <= POST_NPASSES; k++)
            if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &T.ts[s][k]))) { T.on = 0; return -1; }
    }
    T.count[s] = 0;
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)T.dj[s]);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)T.ts[s][0]);
    return (int)s;
}

static void timing_mark(ID3D11DeviceContext *ctx, int s, int pass)
{
    if (s < 0) return;
    T.pass_of[s][T.count[s]] = pass;
    T.count[s]++;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)T.ts[s][T.count[s]]);
}

static void timing_end(ID3D11DeviceContext *ctx, int s)
{
    if (s < 0) return;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)T.dj[s]);
    T.pending[s] = T.count[s] > 0;
    T.slot = (T.slot + 1) % POST_TQ;
}

/* ── Copy into a render target ──────────────────────────────── */

/* GPU time of the split write-back, with XBOX_POST_TIMING=1 (its own ring:
 * the copy runs after the chain's timed region). Printed every 300 copies. */
static struct {
    ID3D11Query *dj[POST_TQ], *t0[POST_TQ], *t1[POST_TQ];
    int pending[POST_TQ];
    unsigned slot, n;
    double sum_ms;
} TW;

static void wb_collect(ID3D11DeviceContext *ctx)
{
    unsigned i;
    for (i = 0; i < POST_TQ; i++) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d;
        UINT64 a, b;
        if (!TW.pending[i]) continue;
        if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)TW.dj[i], &d, sizeof d,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)TW.t0[i], &a, sizeof a,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)TW.t1[i], &b, sizeof b,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        TW.pending[i] = 0;
        if (d.Disjoint || !d.Frequency || b < a) continue;
        TW.sum_ms += (double)(b - a) * 1000.0 / (double)d.Frequency;
        if (++TW.n == 300) {
            fprintf(stderr, "[POST] gpu write-back %.3f ms mean over %u copies\n",
                    TW.sum_ms / TW.n, TW.n);
            TW.sum_ms = 0.0;
            TW.n = 0;
        }
    }
}

static int wb_begin(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    unsigned s;
    if (!s_timing_read) {
        const char *e = getenv("XBOX_POST_TIMING");
        s_timing_read = 1;
        T.on = e && e[0] == '1';
    }
    if (!T.on) return -1;
    wb_collect(ctx);
    s = TW.slot;
    if (TW.pending[s]) return -1;
    if (!TW.dj[s]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &TW.dj[s]))) return -1;
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &TW.t0[s])) ||
            FAILED(ID3D11Device_CreateQuery(dev, &qd, &TW.t1[s]))) return -1;
    }
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)TW.dj[s]);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)TW.t0[s]);
    return (int)s;
}

static void wb_end(ID3D11DeviceContext *ctx, int s)
{
    if (s < 0) return;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)TW.t1[s]);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)TW.dj[s]);
    TW.pending[s] = 1;
    TW.slot = (TW.slot + 1) % POST_TQ;
}

/* Writes src (w x h) into rtv -- every sample when rtv is multisampled -- by
 * the identity pass (texel Load, all four channels: exact, alpha kept for a
 * later destination-alpha blend). State saved and restored. */
int d3d8_post_copy_into(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                        ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *src,
                        UINT w, UINT h)
{
    post_saved saved;
    ID3D11ShaderResourceView *none[3] = { NULL, NULL, NULL };
    D3D11_VIEWPORT vp;
    post_pass *id = &s_passes[0];               /* "identity" */
    int tq;
    if (!dev || !ctx || !rtv || !src || !base_init(dev) || !id->vs || !id->ps) return 0;
    tq = wb_begin(dev, ctx);
    state_save(ctx, &saved);
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (FLOAT)w; vp.Height = (FLOAT)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_RSSetState(ctx, P.rs);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, id->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, id->ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &src);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, none);
    state_restore(ctx, &saved);
    wb_end(ctx, tq);
    return 1;
}

/* ── The chain ─────────────────────────────────────────────────────── */

int d3d8_post_run(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                  ID3D11Texture2D *src_tex, ID3D11ShaderResourceView *src_srv,
                  UINT w, UINT h, unsigned frame_id,
                  ID3D11Texture2D **out_tex, ID3D11ShaderResourceView **out_srv)
{
    static const FLOAT zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    post_saved saved;
    ID3D11ShaderResourceView *color, *none[3] = { NULL, NULL, NULL };
    ID3D11SamplerState *smp[2];
    D3D11_VIEWPORT vp;
    FLOAT size[4];
    int i, cur = -1, tq, smaa, stencil;

    if (!d3d8_post_enabled() || !dev || !ctx || !src_tex || !src_srv || !w || !h)
        return 0;
    smaa = (int)s_smaa;
    if (P.last_valid && P.last_frame == frame_id && P.w == w && P.h == h) {
        *out_tex = P.tex[P.last_index];
        *out_srv = P.srv[P.last_index];
        return 1;
    }
    if (!base_init(dev) || !targets_ensure(dev, w, h)) return 0;
    if (smaa > 0 && !smaa_init(dev, smaa)) smaa = 0;
    stencil = smaa > 0 && fix_flag(&s_fix_stencil, "XBOX_FIX_POST_STENCIL") && stencil_ensure(dev, w, h);
    gpuprof_note(stencil ? 1u : 0u);    /* XBOX_GPUPROF: A/B per frame (XBOX_FIX_PUMP_ALT=1) */

    tq = timing_begin(dev, ctx);
    state_save(ctx, &saved);

    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (FLOAT)w; vp.Height = (FLOAT)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    size[0] = (FLOAT)w; size[1] = (FLOAT)h;
    size[2] = 1.0f / (FLOAT)w; size[3] = 1.0f / (FLOAT)h;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)P.cb, 0, NULL, size, 0, 0);

    smp[0] = P.smp_linear;
    smp[1] = P.smp_point;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, smp);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &P.cb);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &P.cb);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_RSSetState(ctx, P.rs);

    color = src_srv;
    for (i = 0; i < POST_NPASSES; i++) {
        post_pass *p = &s_passes[i];
        ID3D11RenderTargetView *rtv;
        ID3D11ShaderResourceView *in[3];
        int dst = -1, k;
        if (!pass_active(p) || (p->group == GRP_SMAA && smaa <= 0) || !p->vs || !p->ps)
            continue;
        if (p->out == OUT_COLOR) {
            dst = cur == 0 ? 1 : 0;     /* never the texture being read */
            rtv = P.rtv[dst];
        } else {
            rtv = P.aux_rtv[p->out];
        }
        for (k = 0; k < 3; k++) {
            switch (p->in[k]) {
            case IN_COLOR:   in[k] = color; break;
            case IN_EDGES:   in[k] = P.aux_srv[OUT_EDGES]; break;
            case IN_WEIGHTS: in[k] = P.aux_srv[OUT_WEIGHTS]; break;
            case IN_AREA:    in[k] = P.area_srv; break;
            case IN_SEARCH:  in[k] = P.search_srv; break;
            default:         in[k] = NULL; break;
            }
        }
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, none);
        if (p->clear) ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, zero);
        if (stencil && p->out == OUT_EDGES) {
            ID3D11DeviceContext_ClearDepthStencilView(ctx, P.st_dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, P.st_dsv);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, P.st_write, 1);
        } else if (stencil && p->out == OUT_WEIGHTS) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, P.st_dsv);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, P.st_test, 1);
        } else {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            if (stencil) ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
        }
        ID3D11DeviceContext_VSSetShader(ctx, p->vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, p->ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, in);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        timing_mark(ctx, tq, i);
        if (p->out == OUT_COLOR) {
            cur = dst;
            color = P.srv[dst];
        }
    }
    /* Unbind the sources before anything renders into them again. */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, none);
    state_restore(ctx, &saved);
    timing_end(ctx, tq);

    if (cur < 0) return 0;              /* no pass wrote colour */
    P.last_frame = frame_id;
    P.last_index = cur;
    P.last_valid = 1;
    selftest(dev, ctx, src_tex, P.tex[cur], frame_id);
    *out_tex = P.tex[cur];
    *out_srv = P.srv[cur];
    return 1;
}
