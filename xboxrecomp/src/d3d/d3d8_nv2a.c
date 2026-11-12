/**
 * NV2A-native draw path.
 *
 * The push-buffer translator (nv2a_pgraph_d3d11.c) runs the title's vertex
 * programs on the CPU and now also turns its register-combiner setup into an
 * HLSL pixel shader (nv2a_psh.c). Neither fits the fixed-function FVF path:
 * the combiners read four float4 texture coordinates, both vertex colours and
 * the fog factor. This file supplies the rest -- a pass-through vertex shader
 * for pre-transformed NV2A vertices with perspective-correct interpolation,
 * a compiled pixel-shader cache keyed by the combiner state, and the draw
 * itself -- while reusing the device's vertex ring, render states, samplers
 * and bound textures. Every shim draw rebinds its own shaders and constant
 * buffers, so nothing set here leaks into them.
 */
#include "../kernel/xbox_perf.h"
#include "../kernel/xbox_logstats.h"
#include "d3d8_internal.h"
#include "d3d8_gpuprof.h"
#include "d3d8_temporal.h"
#include "d3d8_shprewarm.h"
#include "d3d8_pumpflags.h"
#include <d3dcompiler.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

UINT d3d8_UpRingUpload(const void *data, UINT size, ID3D11Buffer **buf);
void d3d8_UpRingReserve(UINT total, UINT pieces);

/* Vertex layout written by nv2a_pgraph_d3d11.c (ProgVertex): screen x, y,
 * z (0..1) and rhw; oD0; oD1; the fog factor; oT0..oT3. */
/* Undo the divide by w so the rasteriser interpolates perspective-correctly.
 * rhw is never 0 and keeps w's sign (nv2a_pgraph_d3d11.c clamps w away
 * from zero), so a vertex behind the eye gets a negative clip w and the
 * rasteriser clips its triangles at the near plane, as xemu relies on. */
#define NV_VS_HEAD \
    "cbuffer NvVS : register(b0) { float4 screen; };\n" \
    "struct VSIn {\n" \
    "    float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n" \
    "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n" \
    "};\n" \
    "struct VSOut {\n" \
    "    float4 pos : SV_POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n" \
    "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n" \
    "};\n" \
    "VSOut main(VSIn i) {\n" \
    "    VSOut o;\n" \
    "    float w = 1.0 / i.pos.w;\n" \
    "    o.pos = float4((i.pos.x / screen.x * 2.0 - 1.0) * w,\n" \
    "                   (1.0 - i.pos.y / screen.y * 2.0) * w, i.pos.z * w, w);\n"
#define NV_VS_TAIL \
    "    o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog;\n" \
    "    o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n" \
    "    return o;\n" \
    "}\n"
static const char g_vs_src[] = NV_VS_HEAD NV_VS_TAIL;
/* Fork: with XBOX_TEMPORAL (d3d8_temporal.h), the same text plus the
 * image's sub-pixel jitter in clip space (screen.zw, NDC; 0 outside the 3D
 * of a race). Without it the shader is the one above, unchanged. */
static const char g_vs_src_jit[] = NV_VS_HEAD "    o.pos.xy += screen.zw * w;\n" NV_VS_TAIL;

static ID3D11VertexShader *g_vs;
static ID3D11InputLayout  *g_layout;
static ID3D11Buffer       *g_vs_cb, *g_ps_cb;
static UINT                g_ps_cb_size;
static int                 g_init_failed;

#define NV_PS_CACHE 2048
/* ps NULL: the key's shader was tried once and failed. */
static struct { uint64_t key; ID3D11PixelShader *ps; } g_ps_cache[NV_PS_CACHE];
static int g_ps_count;
/* Draws skipped because the pixel shader cache is full (it is never
 * emptied: a draw that needs a shader it does not hold is skipped), and
 * draws dropped without an input layout (its cache full, or the layout
 * refused by the device): one line at the first, counted in [PUMP] Caches
 * (XBOX_LOG_STATS=1, map_stats). The title uses a few percent of each
 * bound. */
static unsigned g_ps_full_skips, g_il_lost;

static ID3D11Buffer *make_cb(ID3D11Device *dev, UINT size)
{
    D3D11_BUFFER_DESC bd;
    ID3D11Buffer *b = NULL;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (size + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &b))) return NULL;
    return b;
}

static int nv_init(void)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr;
    static const D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,   0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "FOG",      0, DXGI_FORMAT_R32_FLOAT,          0,  48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  52, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  68, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  84, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 100, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    if (g_vs) return 1;
    if (g_init_failed || !dev) return 0;
    {
        const char *src = d3d8_temporal_wanted() ? g_vs_src_jit : g_vs_src;
        hr = d3d8_shader_compile_logged(src, strlen(src), "vs_nv2a", NULL, "main", "vs_5_0",
                                        0, &blob, &err);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[NV2A-PSH] vertex shader failed: %s\n",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        g_init_failed = 1;
        return 0;
    }
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(blob),
                                         ID3D10Blob_GetBufferSize(blob), NULL, &g_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(dev, el, (UINT)(sizeof el / sizeof el[0]),
                                            ID3D10Blob_GetBufferPointer(blob),
                                            ID3D10Blob_GetBufferSize(blob), &g_layout);
    ID3D10Blob_Release(blob);
    g_vs_cb = make_cb(dev, 16);
    if (FAILED(hr) || !g_vs || !g_layout || !g_vs_cb) {
        fprintf(stderr, "[NV2A-PSH] vertex stage setup failed (hr %08lX)\n", (unsigned long)hr);
        g_init_failed = 1;
        return 0;
    }
    return 1;
}

/* Shader compiles and the time they took, for the frame-rate log. */
volatile long g_nv_compiles = 0;
volatile double g_nv_compile_ms = 0;
static double nv_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER q;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* XBOX_SHADER_CACHE, default 1; =0 previous path: the bytecode of
 * the pump's two compiles (combiner pixel shaders, vertex programs) is kept
 * on disk (d3d8_shcache.c) and read back by a later session instead of
 * calling D3DCompile; the layer's other shaders go through the same cache
 * (d3d8_shcompile.c). */
volatile long g_nv_shc_hits = 0;

/* D3DCompile through the cache; *hit = 1 when the bytecode came from it. */
static HRESULT nv_compile(const char *src, SIZE_T len, const char *name, const char *profile,
                          UINT flags, ID3DBlob **code, ID3DBlob **err, int *hit)
{
    HRESULT hr = d3d8_shader_compile(src, len, name, NULL, "main", profile, flags, code, err, hit);
    if (*hit) g_nv_shc_hits++;
    return hr;
}

/* XBOX_FIX_PUMP_CACHE: exact index from a 64-bit key to a
 * position in a cache table, instead of the linear search done on every
 * draw. Only the first insertion of a key is kept, as the linear search
 * returns the first entry: same result. Power-of-2 capacity, at least twice
 * the indexed table (never full). */
typedef struct { uint64_t key; int pos1; } KIdx;            /* pos1 = position + 1, 0 = free */
static int kidx_find(const KIdx *t, unsigned cap, uint64_t key)
{
    unsigned h = (unsigned)(key ^ (key >> 29) ^ (key >> 47)) & (cap - 1);
    while (t[h].pos1) {
        if (t[h].key == key) return t[h].pos1 - 1;
        h = (h + 1) & (cap - 1);
    }
    return -1;
}
static void kidx_put(KIdx *t, unsigned cap, uint64_t key, int pos)
{
    unsigned h = (unsigned)(key ^ (key >> 29) ^ (key >> 47)) & (cap - 1);
    while (t[h].pos1) {
        if (t[h].key == key) return;                        /* first insertion kept */
        h = (h + 1) & (cap - 1);
    }
    t[h].key = key;
    t[h].pos1 = pos + 1;
}

/* XBOX_FIX_PUMP_ALT=1 (identity test): the pump optimizations are active on
 * one presented frame in two only; during a pause (frozen scene), every
 * captured frame must then be identical. */
static unsigned g_pump_frame;

/* The pump's switches (d3d8_pumpflags.h): each read once from the
 * environment, all worked out here once per presented frame -- the only thing
 * that changes them is the ALT parity of g_pump_frame, which changes at
 * d3d8_pump_frame_tick only -- into g_pump_flags, which every switch reads.
 * Same answers as when each switch worked its own out on every call. */
unsigned g_pump_flags = PF_STALE;

static int env_default_on(const char *name) { const char *e = getenv(name); return !(e && e[0] == '0'); }
static int env_default_off(const char *name) { const char *e = getenv(name); return e && e[0] == '1'; }


static int pf_alt, pf_state, pf_cb, pf_onemap, pf_onemap_alt, pf_cgen, pf_cache, pf_memo2, pf_group, pf_texmemo, pf_srv,
           pf_sobj, pf_smemo, pf_rskip, pf_idx, pf_argb;

/* The word for the current frame, from the switches read once (pf_*) and the
 * parity of g_pump_frame. */
static unsigned pump_flags_of_frame(void)
{
    unsigned f = 0;
    int off = pf_alt && (g_pump_frame & 1u);
    if (off) f |= PF_ALT_OFF;
    if (pf_state && !off) f |= PF_STATE;
    if (pf_cb && !off) f |= PF_CB;
    if (pf_onemap && !(pf_onemap_alt && (g_pump_frame & 1u)) && !off) f |= PF_ONEMAP;
    if (pf_cgen && !off) f |= PF_CGEN;
    if (pf_cache && !off) f |= PF_CACHE;
    if (pf_memo2 && !off) f |= PF_MEMO2;
    if (pf_group && !off) f |= PF_GROUP;
    if (pf_texmemo && (f & PF_CACHE)) f |= PF_TEXMEMO;  /* with the signature cache (nv2a_pgraph_d3d11.c, g_tm) */
    if (pf_srv && (f & PF_STATE)) f |= PF_SRV;          /* with the state mirror's skipping (d3d8_statecache.c) */
    if (pf_sobj && !off) f |= PF_SOBJ;
    if (pf_smemo && !off) f |= PF_SMEMO;
    if (pf_rskip && !off) f |= PF_RSKIP;
    if (pf_idx && !off) f |= PF_IDX;
    if (pf_argb && !off) f |= PF_ARGB;
    return f;
}

unsigned d3d8_pump_flags_refresh(void)
{
    static int read;
    unsigned f;
    if (!read) {
        pf_alt = env_default_off("XBOX_FIX_PUMP_ALT");
        pf_state = env_default_on("XBOX_FIX_PUMP_STATE");      /* default 1 (proven identical) */
        pf_cb = env_default_on("XBOX_FIX_PUMP_CB");            /* default 1 (proven identical) */
        pf_onemap = env_default_on("XBOX_FIX_PUMP_ONEMAP");    /* default 1 (proven identical); 0 = previous path */
        pf_onemap_alt = env_default_off("XBOX_FIX_PUMP_ONEMAP_ALT");
        pf_cgen = env_default_on("XBOX_FIX_PUMP_CGEN");
        pf_cache = env_default_on("XBOX_FIX_PUMP_CACHE");      /* default 1 (proven identical) */
        pf_memo2 = env_default_on("XBOX_FIX_PUMP_MEMO2");
        pf_group = env_default_on("XBOX_FIX_PUMP_GROUP");
        pf_texmemo = env_default_on("XBOX_FIX_PUMP_TEXMEMO");
        pf_srv = env_default_on("XBOX_FIX_PUMP_SRV");
        pf_sobj = env_default_on("XBOX_FIX_PUMP_SOBJ");
        pf_smemo = env_default_on("XBOX_FIX_PUMP_SMEMO");
        pf_rskip = env_default_on("XBOX_FIX_PUMP_RSKIP");
        pf_idx = env_default_on("XBOX_FIX_PUMP_IDX");
        pf_argb = env_default_on("XBOX_FIX_PUMP_ARGB");
        fprintf(stderr, "[PUMP] XBOX_FIX_PUMP_ONEMAP=%d%s\n", pf_onemap, pf_onemap_alt ? " (ALT: one frame in two)" : "");
        fprintf(stderr, "[PUMP] XBOX_FIX_PUMP_MEMO2=%d\n", pf_memo2);
        read = 1;
    }
    f = pump_flags_of_frame();
    g_pump_flags = f;
    return f;
}


static int pump_alt_off(void) { return (d3d8_pump_flags() & PF_ALT_OFF) != 0; }
static void vi_present(void);
/* At the head of host_present, before the post and the Present: the frame
 * ends here (also the vertex/index ring's query of the frame, vi_present). */
void d3d8_pump_frame_tick(void) { vi_present(); g_pump_frame++; d3d8_pump_flags_refresh(); }
int  d3d8_pump_alt_off(void) { return pump_alt_off(); }   /* also used by the post */

int d3d8_pump_state_on(void) { return (d3d8_pump_flags() & PF_STATE) != 0; }
int d3d8_pump_cb_on(void) { return (d3d8_pump_flags() & PF_CB) != 0; }

/* XBOX_FIX_PUMP_ONEMAP (default 1; =0 previous path): one Map per GPU draw for its vertices and indices.
 * Constant attributes (stride 0) that sit close together in guest memory are
 * uploaded as one block through one input slot; vertex groups and indices go
 * to one ring bound as both vertex and index buffer, written with a single
 * Map; the input layout and vertex shader lookups first compare with the
 * previous draw. Same bytes and formats reach the shaders.
 * XBOX_FIX_PUMP_ONEMAP_ALT=1 (identity test): active on one presented frame
 * in two only. XBOX_FIX_PUMP_ALT turns it off with the other optimizations. */
int d3d8_pump_onemap_on(void) { return (d3d8_pump_flags() & PF_ONEMAP) != 0; }

/* XBOX_FIX_PUMP_CGEN, default 1; 0 = previous path: the VS constants
 * of a GPU draw are compared only where the translator wrote a new value since
 * the last draw (its bitmap, Nv2aVshDraw.vdirty) instead of all 3 KB, and the
 * parameters and PS constants already in the constant ring are bound again by
 * offset when their bytes have not changed, without writing them; a draw with
 * nothing new maps nothing. The shaders read the same bytes. Off on the frames
 * XBOX_FIX_PUMP_ALT turns the pump optimizations off. */
int d3d8_pump_cgen_on(void) { return (d3d8_pump_flags() & PF_CGEN) != 0; }

int d3d8_pump_cache_on(void) { return (d3d8_pump_flags() & PF_CACHE) != 0; }

/* XBOX_FIX_PUMP_MEMO2 (default 1; =0 one entry, the previous path): the
 * memos in front of the vertex shader, input layout and pixel shader key
 * lookups keep the last two lookups instead of the last one, so that two
 * kinds of draws that take turns -- a terrain patch and its seam blend, the
 * same program with one instruction, one input and alpha_t0z more -- each
 * find theirs. The memos only spare a lookup: the caches behind them only
 * grow, so a remembered entry is the one the lookup returns. Off with the
 * other optimizations on the XBOX_FIX_PUMP_ALT frames. */
int d3d8_pump_memo2_on(void) { return (d3d8_pump_flags() & PF_MEMO2) != 0; }

static KIdx g_ps_idx[NV_PS_CACHE * 2];

/* The key's position in the cache, or -1. */
static int ps_slot(unsigned long long key)
{
    int i;
    if (d3d8_pump_cache_on())
        return kidx_find(g_ps_idx, NV_PS_CACHE * 2, key);
    for (i = 0; i < g_ps_count; i++)
        if (g_ps_cache[i].key == key)
            return i;
    return -1;
}

static ID3D11PixelShader *ps_find(unsigned long long key)
{
    int i = ps_slot(key);
    return i >= 0 ? g_ps_cache[i].ps : NULL;
}

/* The compiler arguments of the pump's two shaders, shared with
 * the prewarm thread (d3d8_shprewarm.c): same HLSL, same bytecode. */
#define NV_PS_NAME    "ps_nv2a"
#define NV_PS_PROFILE "ps_5_0"
#define NV_PS_FLAGS   D3DCOMPILE_OPTIMIZATION_LEVEL3
#define NV_VS_NAME    "vs_nv2a_prog"
#define NV_VS_PROFILE "vs_5_0"
#define NV_VS_FLAGS   D3DCOMPILE_IEEE_STRICTNESS   /* IEEE strictness keeps the NaN and infinity rules of the CPU path */

/* One [SHC] line for each shader the pump needs and does not
 * have yet (a few dozen per session; capped): where it came from and what
 * it cost the pump. "prewarmed" = built ahead by d3d8_shprewarm.c. */
static void nv_pump_log(const char *what, unsigned long long key, const char *how, double ms)
{
    static int lines;
    if (++lines <= 512)
        fprintf(stderr, "[SHC] pump %s %016llX: %s, %.1f ms (t+%.0f ms)\n", what, key, how, ms,
                d3d8_ms_since_start());
    else if (lines == 513)
        fprintf(stderr, "[SHC] (no more lines for the pump's shaders)\n");
}

static void ps_put(unsigned long long key, ID3D11PixelShader *ps)
{
    g_ps_cache[g_ps_count].key = key;
    g_ps_cache[g_ps_count].ps = ps;
    kidx_put(g_ps_idx, NV_PS_CACHE * 2, key, g_ps_count);
    g_ps_count++;
}

/* 1: compiled; 0: never tried; -1: tried and failed. */
int d3d8_nv2a_has_ps(unsigned long long key)
{
    int i = ps_slot(key);
    ID3D11PixelShader *ps;
    if (i >= 0) return g_ps_cache[i].ps ? 1 : -1;
    /* Built ahead by the prewarm thread. */
    if (g_ps_count >= NV_PS_CACHE || !(ps = (ID3D11PixelShader *)shpw_take_ps(key))) return 0;
    ps_put(key, ps);
    nv_pump_log("ps", key, "prewarmed", 0.0);
    return 1;
}

/* The prewarm thread's pixel shader (ID3D11PixelShader *, NULL on
 * failure); the pump's own build below uses the same arguments. */
void *d3d8_nv2a_build_ps(const char *hlsl, int len, int *hit)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr;
    *hit = 0;
    if (!dev) return NULL;
    hr = d3d8_shader_compile(hlsl, (SIZE_T)len, NV_PS_NAME, NULL, "main", NV_PS_PROFILE, NV_PS_FLAGS,
                             &blob, &err, hit);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr) || !blob) return NULL;
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(blob),
                                        ID3D10Blob_GetBufferSize(blob), NULL, &ps);
    ID3D10Blob_Release(blob);
    return SUCCEEDED(hr) ? ps : NULL;
}

/* A key whose shader could not be generated, compiled or
 * created is kept as failed (NULL), so that it is tried once: without it,
 * each of its draws generated and compiled it again (3 to 18 ms each), as
 * a shader refused by another HLSL compiler (Wine's) would be. Its draws
 * are skipped as before. No device yet is not a failure. */
int d3d8_nv2a_add_ps(unsigned long long key, const char *hlsl, int len)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr;
    static int failures;
    double t0 = nv_now_ms();
    int hit = 0;

    if (!dev) return 0;
    if (g_ps_count >= NV_PS_CACHE) {
        if (!g_ps_full_skips++)
            fprintf(stderr, "[NV2A-PSH] pixel shader cache full (%d shaders): a draw that needs a new one "
                    "is skipped (counted in [PUMP] Caches)\n", NV_PS_CACHE);
        return 0;
    }
    if (len <= 0) { ps_put(key, NULL); return 0; }
    {
        hr = nv_compile(hlsl, (SIZE_T)len, NV_PS_NAME, NV_PS_PROFILE,
                        NV_PS_FLAGS, &blob, &err, &hit);
        if (!hit) {
            g_nv_compiles++;
            if (g_perf_on) perf_count(PC_COMPILE, 1);
        }
        g_nv_compile_ms += nv_now_ms() - t0;
    }
    if (FAILED(hr)) {
        if (failures++ < 4)
            fprintf(stderr, "[NV2A-PSH] pixel shader %016llX failed: %s\n--- source ---\n%s\n",
                    (unsigned long long)key,
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", hlsl);
        if (err) ID3D10Blob_Release(err);
        ps_put(key, NULL);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(blob),
                                        ID3D10Blob_GetBufferSize(blob), NULL, &ps);
    ID3D10Blob_Release(blob);
    if (FAILED(hr) || !ps) { ps_put(key, NULL); return 0; }
    ps_put(key, ps);
    nv_pump_log("ps", key, hit ? "from the cache" : "compiled", nv_now_ms() - t0);
    return 1;
}

/* Buffer Maps of this file, counted for XBOX_PERF (Maps per GPU draw),
 * timed under XBOX_PERF_MAPTIME, and under XBOX_LOG_STATS for the slow Maps
 * of map_stats. Same calls as before otherwise. */
static unsigned g_nv_maps;
static void map_note(ID3D11Resource *r, D3D11_MAP how, unsigned long long cyc);

static HRESULT nv_map(ID3D11DeviceContext *ctx, ID3D11Resource *r, D3D11_MAP how, D3D11_MAPPED_SUBRESOURCE *m)
{
    unsigned long long t;
    HRESULT hr;
    g_nv_maps++;
    if (!g_perf_maptime && !xbox_log_stats()) return ID3D11DeviceContext_Map(ctx, r, 0, how, 0, m);
    t = __rdtsc();
    hr = ID3D11DeviceContext_Map(ctx, r, 0, how, 0, m);
    t = __rdtsc() - t;
    if (g_perf_maptime) g_perf_map_cyc += t;
    if (xbox_log_stats()) map_note(r, how, t);
    return hr;
}

static void nv_unmap(ID3D11DeviceContext *ctx, ID3D11Resource *r)
{
    unsigned long long t;
    if (!g_perf_maptime) { ID3D11DeviceContext_Unmap(ctx, r, 0); return; }
    t = __rdtsc();
    ID3D11DeviceContext_Unmap(ctx, r, 0);
    g_perf_map_cyc += __rdtsc() - t;
    g_perf_map_pairs++;
}

/* XBOX_FIX_PUMP_CB: a single constant ring for the GPU draws.
 * Each draw writes its constants there (VS only if they changed, parameters,
 * PS) with one Map NO_OVERWRITE, and binds them by offset
 * (VSSetConstantBuffers1 / PSSetConstantBuffers1), instead of three
 * Map DISCARD on three small buffers. Same bytes for the shaders.
 * Needs the 11.1 runtime and the ConstantBufferOffsetting and
 * MapNoOverwriteOnDynamicConstantBuffer options; otherwise, the old path. */
#define CBR_SIZE (4u * 1024u * 1024u)
static ID3D11Buffer *g_cbr;
static ID3D11DeviceContext1 *g_ctx1;
static UINT g_cbr_off, g_cbr_vsc_off;
static unsigned g_cbr_gen;              /* +1 on each DISCARD: the VS constants already written are lost */
static unsigned g_cbr_vsc_gen;

extern int d3d8_pump_cb_on(void);

static int cbr_ready(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    static int ok = -1;
    if (ok >= 0) return ok;
    ok = 0;
    {
        D3D11_FEATURE_DATA_D3D11_OPTIONS o;
        D3D11_BUFFER_DESC bd;
        memset(&o, 0, sizeof o);
        if (FAILED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o))
            || !o.ConstantBufferOffsetting || !o.MapNoOverwriteOnDynamicConstantBuffer) {
            fprintf(stderr, "[PUMP] constant ring unavailable (11.1 options): old path\n");
            return 0;
        }
        if (FAILED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&g_ctx1)))
            return 0;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = CBR_SIZE;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &g_cbr))) return 0;
        g_cbr_off = CBR_SIZE;           /* first Map = DISCARD */
        ok = 1;
        fprintf(stderr, "[PUMP] constant ring active (%u KB)\n", CBR_SIZE / 1024u);
    }
    return ok;
}

#define CBR_ALIGN(n) (((n) + 255u) & ~255u)     /* offsets in multiples of 16 constants */

/* Writes the constants of a draw and binds them; the VS constants are copied
 * only if they changed (or if the ring wrapped since). */
static UINT g_cbr_par_first, g_cbr_par_num;

/* The parameters and PS constants last written to the ring (bytes,
 * offset, ring generation), for XBOX_FIX_PUMP_CGEN. */
static struct { uint8_t b[512]; UINT size, off; unsigned gen; int ok; } g_cbr_last[2];

static int cbr_same(int k, const void *p, UINT size)
{
    return g_cbr_last[k].ok && g_cbr_last[k].gen == g_cbr_gen && g_cbr_last[k].size == size &&
           !memcmp(g_cbr_last[k].b, p, size);
}

static void cbr_keep(int k, const void *p, UINT size, UINT off)
{
    g_cbr_last[k].ok = size <= sizeof g_cbr_last[k].b;
    if (!g_cbr_last[k].ok) return;
    memcpy(g_cbr_last[k].b, p, size);
    g_cbr_last[k].size = size;
    g_cbr_last[k].off = off;
    g_cbr_last[k].gen = g_cbr_gen;
}

static int cbr_draw(ID3D11DeviceContext *ctx, const void *vsc, int vsc_changed, UINT vsc_size,
                    const void *par, UINT par_size, const void *psc, UINT psc_size)
{
    D3D11_MAPPED_SUBRESOURCE m;
    UINT need, a_vsc = CBR_ALIGN(vsc_size), a_par = CBR_ALIGN(par_size), a_psc = CBR_ALIGN(psc_size);
    UINT o_par = 0, o_psc = 0, first[2], num[2];   /* always set below (0 only quiets the compiler) */
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    ID3D11Buffer *bufs[2];
    int write_vsc = vsc_changed || g_cbr_vsc_gen != g_cbr_gen;
    int reuse = d3d8_pump_cgen_on();
    int write_par = !(reuse && cbr_same(0, par, par_size));
    int write_psc = !(reuse && cbr_same(1, psc, psc_size));
    need = (write_vsc ? a_vsc : 0) + (write_par ? a_par : 0) + (write_psc ? a_psc : 0);
    if (g_cbr_off + need > CBR_SIZE) {
        how = D3D11_MAP_WRITE_DISCARD;
        g_cbr_off = 0;
        g_cbr_gen++;
        write_vsc = write_par = write_psc = 1;
        need = a_vsc + a_par + a_psc;
    }
    if (need) {
        if (FAILED(nv_map(ctx, (ID3D11Resource *)g_cbr, how, &m))) return 0;
        if (write_vsc) {
            memcpy((uint8_t *)m.pData + g_cbr_off, vsc, vsc_size);
            g_cbr_vsc_off = g_cbr_off;
            g_cbr_vsc_gen = g_cbr_gen;
            g_cbr_off += a_vsc;
        }
        if (write_par) {
            o_par = g_cbr_off; memcpy((uint8_t *)m.pData + o_par, par, par_size); g_cbr_off += a_par;
            cbr_keep(0, par, par_size, o_par);
        }
        if (write_psc) {
            o_psc = g_cbr_off; memcpy((uint8_t *)m.pData + o_psc, psc, psc_size); g_cbr_off += a_psc;
            cbr_keep(1, psc, psc_size, o_psc);
        }
        nv_unmap(ctx, (ID3D11Resource *)g_cbr);
    }
    if (!write_par) o_par = g_cbr_last[0].off;   /* the same bytes, already in the ring */
    if (!write_psc) o_psc = g_cbr_last[1].off;
    if (g_perf_detail && !need) perf_count(PC_CBSKIP, 1);   /* only where it reuses: XBOX_PERF_DETAIL */
    bufs[0] = bufs[1] = g_cbr;
    first[0] = g_cbr_vsc_off / 16u; num[0] = a_vsc / 16u;
    first[1] = o_par / 16u;         num[1] = a_par / 16u;
    g_cbr_par_first = first[1]; g_cbr_par_num = num[1];   /* the points' GS reads it too */
    ID3D11DeviceContext1_VSSetConstantBuffers1(g_ctx1, 0, 2, bufs, first, num);
    first[0] = o_psc / 16u; num[0] = a_psc / 16u;
    ID3D11DeviceContext1_PSSetConstantBuffers1(g_ctx1, 0, 1, bufs, first, num);
    return 1;
}

static void cb_write(ID3D11DeviceContext *ctx, ID3D11Buffer *b, const void *data, UINT size)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(nv_map(ctx, (ID3D11Resource *)b, D3D11_MAP_WRITE_DISCARD, &m))) {
        memcpy(m.pData, data, size);
        nv_unmap(ctx, (ID3D11Resource *)b);
    }
}

/* The rasteriser state for a draw's raster bits: bit 0 depth clip, bits 2:1
 * cull (0 none, 1 front, 2 back), bit 3 front face counter-clockwise (D3D11
 * sense); bit 4 here is the window-clip scissor. NV2A SET_ZMIN_MAX_CONTROL
 * in clamp mode clamps a pixel beyond the depth range instead of dropping it
 * (xemu discards only in cull mode); D3D11 clips at the far plane unless
 * DepthClipEnable is off, which deleted the character-select riders: their z
 * lands just past 2^24-1. */
static void nv_set_raster(ID3D11DeviceContext *ctx, ID3D11Device *dev, int raster)
{
    static ID3D11RasterizerState *rs[32];
    int k = (raster & 0x0F) | (d3d8_WindowClipOn() ? 0x10 : 0);
    if (!rs[k]) {
        D3D11_RASTERIZER_DESC rd;
        int cull = (k >> 1) & 3;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = cull == 1 ? D3D11_CULL_FRONT : cull == 2 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
        rd.FrontCounterClockwise = (k & 8) ? TRUE : FALSE;
        rd.DepthClipEnable = (k & 1) ? TRUE : FALSE;
        rd.ScissorEnable = (k & 0x10) ? TRUE : FALSE;   /* rectangle set by d3d8_states_apply */
        ID3D11Device_CreateRasterizerState(dev, &rd, &rs[k]);
    }
    if (rs[k]) ID3D11DeviceContext_RSSetState(ctx, rs[k]);
}

/* Every draw as usual (d3d8_nv2a_vsh.h): 0, and never the two passes. */
int  d3d8_ghost_mode(void) { return 0; }
void d3d8_ghost_pass(void *ctx, int pass) { (void)ctx; (void)pass; }

HRESULT d3d8_nv2a_draw(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                       unsigned long long ps_key, const void *ps_consts, UINT ps_consts_size,
                       int raster)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    int lr;                             /* fork: the lights (d3d8_lights.h) */
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3D11PixelShader *ps = ps_find(ps_key);
    ID3D11Buffer *vb = NULL;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    UINT nv, off;
    float screen[4];

    if (!ctx || !dev || !ps || !nv_init()) return E_FAIL;
    switch (prim) {
    case D3DPT_TRIANGLELIST: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; nv = prim_count * 3; break;
    case D3DPT_LINELIST:     topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;     nv = prim_count * 2; break;
    case D3DPT_LINESTRIP:    topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;    nv = prim_count + 1; break;
    case D3DPT_POINTLIST:    topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;    nv = prim_count;     break;
    default: return E_INVALIDARG;
    }
    off = d3d8_UpRingUpload(verts, nv * stride, &vb);
    if (off == (UINT)-1 || !vb) return E_OUTOFMEMORY;

    if (!g_ps_cb || g_ps_cb_size < ps_consts_size) {
        if (g_ps_cb) ID3D11Buffer_Release(g_ps_cb);
        g_ps_cb = make_cb(dev, ps_consts_size);
        g_ps_cb_size = g_ps_cb ? ps_consts_size : 0;
        if (!g_ps_cb) return E_OUTOFMEMORY;
    }
    screen[0] = (float)d3d8_GetBackbufferWidth();
    screen[1] = (float)d3d8_GetBackbufferHeight();
    if (screen[0] <= 0.0f) screen[0] = 640.0f;
    if (screen[1] <= 0.0f) screen[1] = 480.0f;
    screen[2] = screen[3] = 0.0f;
    d3d8_temporal_jitter_ndc(&screen[2], &screen[3]);      /* fork: 0 when off */
    cb_write(ctx, g_vs_cb, screen, sizeof screen);
    cb_write(ctx, g_ps_cb, ps_consts, ps_consts_size);

    d3d8_states_apply();                 /* blend, depth, raster, samplers */
    /* NV2A SET_ZMIN_MAX_CONTROL: in clamp mode a pixel beyond the depth
     * range is clamped, not dropped (xemu discards only in cull mode).
     * D3D11 clips at the far plane unless DepthClipEnable is off, which
     * deleted the character-select riders: their z lands just past 2^24-1. */
    nv_set_raster(ctx, dev, raster);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, topo);
    ID3D11DeviceContext_VSSetShader(ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_vs_cb);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_ps_cb);
    {
        int gm = d3d8_ghost_mode();
        if (gm == 1) return S_OK;
        if (g_gpuprof_on) gpuprof_draw_begin();
        if (gm == 2) {
            d3d8_ghost_pass(ctx, 0);
            ID3D11DeviceContext_Draw(ctx, nv, 0);
            d3d8_ghost_pass(ctx, 1);
            ID3D11DeviceContext_Draw(ctx, nv, 0);
            d3d8_ghost_pass(ctx, 2);
        } else {
            lr = g_d3d8_light_mark ? d3d8_lights_pre(ctx) : 0;     /* fork: the lights (d3d8_lights.h) */
            ID3D11DeviceContext_Draw(ctx, nv, 0);
            if (lr == 2) {
                if (d3d8_lights_z_begin(ctx)) { ID3D11DeviceContext_Draw(ctx, nv, 0); d3d8_lights_z_end(ctx); }
                d3d8_lights_mirror_end(ctx);
            } else if (lr == 1 && d3d8_lights_mirror_begin(ctx)) {
                ID3D11DeviceContext_Draw(ctx, nv, 0);
                if (d3d8_lights_z_begin(ctx)) { ID3D11DeviceContext_Draw(ctx, nv, 0); d3d8_lights_z_end(ctx); }
                d3d8_lights_mirror_end(ctx);
            }
            if (g_d3d8_temporal_mark && lr != 2 && d3d8_temporal_mirror_begin(ctx)) {   /* fork: temporal */
                ID3D11DeviceContext_Draw(ctx, nv, 0);
                d3d8_temporal_mirror_end(ctx);
            }
        }
        if (g_gpuprof_on) gpuprof_draw_end();
    }
    return S_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * Vertex programs on the GPU -- see d3d8_nv2a_vsh.c.
 * ══════════════════════════════════════════════════════════════════════ */
#include "d3d8_nv2a_vsh.h"

#define VSH_CACHE 512
static struct { uint64_t key; ID3D11VertexShader *vs; ID3D10Blob *code; } g_vsh[VSH_CACHE];
static int g_nvsh;
#define IL_CACHE 1024
static struct { uint64_t key; ID3D11InputLayout *il; } g_il[IL_CACHE];
static int g_nil;
static KIdx g_vsh_idx[VSH_CACHE * 2], g_il_idx[IL_CACHE * 2];
static ID3D11Buffer *g_vsc_cb, *g_vsp_cb, *g_ib_ring;
static UINT g_ib_off;
static float g_vsc_last[VSHCPU_CONSTANTS][4];
static int g_vsc_valid;
#define IB_RING_SIZE (16u * 1024u * 1024u)

static uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

/* XBOX_NV2A_HLSL_DUMP=<existing folder> (debug, off by default): each
 * generated shader source is also written there as <name>_<key>.hlsl (at
 * most 256 files), to try it with another HLSL compiler. */
static void nv_hlsl_dump(const char *name, uint64_t key, const char *src, int len)
{
    static const char *dir;
    static int count;
    char path[600];
    FILE *f;
    if (!dir) { dir = getenv("XBOX_NV2A_HLSL_DUMP"); if (!dir) dir = ""; }
    if (!dir[0] || count >= 256) return;
    count++;
    snprintf(path, sizeof path, "%s/%s_%016llX.hlsl", dir, name, (unsigned long long)key);
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(src, 1, (size_t)len, f);
    fclose(f);
}

/* The shader for this program and these input kinds, compiled on first use.
 * A program that fails to compile is remembered, so it is not retried. */
static int vsh_lookup(const Nv2aVshDraw *d, const uint8_t kind[16]);
static struct { uint64_t hash; uint8_t kind[16]; int pts, vi; } g_vsh_last[2] = { { 0, {0}, 0, -1 }, { 0, {0}, 0, -1 } };
static int g_vsh_mru;                   /* the entry of the last lookup; the other is replaced next (MEMO2) */
static unsigned long long g_vsh_memo2_hits, g_il_memo2_hits;     /* found in the older entry (XBOX_SUBST_TIME) */

static int vsh_memo_is(int w, const Nv2aVshDraw *d, const uint8_t kind[16], int pts)
{
    return g_vsh_last[w].vi >= 0 && g_vsh_last[w].hash == d->prog_hash && g_vsh_last[w].pts == pts &&
           !memcmp(g_vsh_last[w].kind, kind, sizeof g_vsh_last[w].kind);
}

static int vsh_get(const Nv2aVshDraw *d)
{
    uint8_t kind[16];
    int a, i, w, pts = d->topology == D3DPT_POINTLIST, two = d3d8_pump_memo2_on();

    for (a = 0; a < 16; a++) kind[a] = (d->inputs & (1u << a)) ? d->attr[a].kind : 0xFF;
    /* Same program and input kinds as the previous lookup -- with
     * XBOX_FIX_PUMP_MEMO2, as one of the last two: same entry (each draw asks
     * twice, and consecutive draws often share the program or take turns). */
    if (d3d8_pump_onemap_on()) {
        w = two ? g_vsh_mru : 0;
        if (vsh_memo_is(w, d, kind, pts)) return g_vsh_last[w].vi;
        if (two && vsh_memo_is(w ^ 1, d, kind, pts)) { g_vsh_mru = w ^ 1; g_vsh_memo2_hits++; return g_vsh_last[w ^ 1].vi; }
    }
    i = vsh_lookup(d, kind);
    w = two ? g_vsh_mru ^ 1 : 0;        /* the older entry (one entry: the only one) */
    g_vsh_mru = w;
    g_vsh_last[w].vi = i;
    g_vsh_last[w].hash = d->prog_hash;
    g_vsh_last[w].pts = pts;
    memcpy(g_vsh_last[w].kind, kind, sizeof kind);
    return i;
}

/* XBOX_SUBST_TIME (nv2a_pgraph_d3d11.c): lookups found in the older memo entry so far */
void d3d8_nv2a_memo2_hits(unsigned long long *vsh, unsigned long long *il)
{
    *vsh = g_vsh_memo2_hits;
    *il = g_il_memo2_hits;
}

/* The key of a vertex program in g_vsh: the program, the kind of each input
 * and the point flag. Also the prewarm thread's. */
static uint64_t vsh_key(uint64_t prog_hash, const uint8_t kind[16], int pts)
{
    uint64_t key = fnv64(prog_hash, kind, 16);
    if (pts) key = fnv64(key, "pts", 3);
    return key;
}

/* A vertex-program recipe for the prewarm thread
 * (d3d8_shprewarm.h): this head, then prog_len decoded instructions. */
typedef struct {
    uint64_t prog_hash;
    uint16_t inputs;
    uint8_t  pts, prog_len;
    uint8_t  kind[16];
} VshRecipe;

static void vsh_note(const Nv2aVshDraw *d, const uint8_t kind[16], int pts)
{
    uint8_t rec[sizeof(VshRecipe) + VSHCPU_SLOTS * sizeof(vshcpu_insn)];
    VshRecipe h;
    if (d->prog_len <= 0 || d->prog_len > VSHCPU_SLOTS) return;
    memset(&h, 0, sizeof h);
    h.prog_hash = d->prog_hash;
    h.inputs = d->inputs;
    h.pts = (uint8_t)(pts != 0);
    h.prog_len = (uint8_t)d->prog_len;
    memcpy(h.kind, kind, 16);
    memcpy(rec, &h, sizeof h);
    memcpy(rec + sizeof h, d->prog, (size_t)d->prog_len * sizeof(vshcpu_insn));
    shpw_note(SHPW_VS, rec, (int)(sizeof h + (size_t)d->prog_len * sizeof(vshcpu_insn)));
}

int d3d8_nv2a_vs_recipe_hlsl(const void *rec, int size, uint64_t *key, char *buf, int cap)
{
    VshRecipe h;
    vshcpu_insn prog[VSHCPU_SLOTS];
    if (size < (int)sizeof h) return -1;
    memcpy(&h, rec, sizeof h);
    if (!h.prog_len || h.prog_len > VSHCPU_SLOTS
        || size != (int)(sizeof h + (size_t)h.prog_len * sizeof(vshcpu_insn))) return -1;
    memcpy(prog, (const uint8_t *)rec + sizeof h, (size_t)h.prog_len * sizeof(vshcpu_insn));
    *key = vsh_key(h.prog_hash, h.kind, h.pts);
    return nv2a_vsh_hlsl(prog, h.prog_len, h.inputs, h.kind, h.pts, buf, cap);
}

/* A vertex recipe rebuilt from a program's instruction words (the
 * shader prewarm thread's given list, d3d8_shprewarm.c): decoded and hashed
 * as vsh_active (nv2a_pgraph_d3d11.c) does for a drawn one, plus the blend
 * overlay's instruction if t0z (as the terrain seams' draws). A program that
 * writes constant registers runs on the CPU there (no shader): none. */
int d3d8_nv2a_vs_recipe_make(const uint32_t (*words)[4], int count, int t0z, const uint8_t kind[16],
                             int pts, void *out, int cap, uint64_t *key)
{
    uint32_t mem[VSHCPU_SLOTS][4];
    vshcpu_insn dec[VSHCPU_SLOTS];
    VshRecipe h;
    int n, wc, size;
    if (count <= 0 || count > VSHCPU_SLOTS) return -1;
    memset(mem, 0, sizeof mem);
    memcpy(mem, words, (size_t)count * sizeof mem[0]);
    n = vshcpu_decode((const uint32_t (*)[4])mem, 0, dec);
    if (n != count) return -1;                                 /* FINAL is the last word given */
    memset(&h, 0, sizeof h);
    h.prog_hash = vshcpu_digest(dec, n, &h.inputs, &wc);
    if (wc) return -1;
    if (t0z) {                                                 /* as the substituted draws' overlay */
        if (n >= VSHCPU_SLOTS || (h.inputs & 0x8000u)) return -1;
        vshcpu_t0z_append(dec, n, &h.inputs, &h.prog_hash);
        n++;
    }
    h.pts = (uint8_t)(pts != 0);
    h.prog_len = (uint8_t)n;
    memcpy(h.kind, kind, 16);
    size = (int)(sizeof h + (size_t)n * sizeof(vshcpu_insn));
    if (size > cap) return -1;
    memcpy(out, &h, sizeof h);
    memcpy((uint8_t *)out + sizeof h, dec, (size_t)n * sizeof(vshcpu_insn));
    *key = vsh_key(h.prog_hash, h.kind, h.pts);
    return size;
}

/* The prewarm thread's vertex program (shader and bytecode, for
 * the input layouts); the pump's own build below uses the same arguments. */
int d3d8_nv2a_build_vs(const char *src, int len, void **vs, void **code, int *hit)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3D10Blob *c = NULL, *err = NULL;
    ID3D11VertexShader *v = NULL;
    HRESULT hr;
    *hit = 0;
    if (!dev) return 0;
    hr = d3d8_shader_compile(src, (SIZE_T)len, NV_VS_NAME, NULL, "main", NV_VS_PROFILE, NV_VS_FLAGS,
                             &c, &err, hit);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr) || !c) return 0;
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(c),
                                         ID3D10Blob_GetBufferSize(c), NULL, &v);
    if (FAILED(hr) || !v) { ID3D10Blob_Release(c); return 0; }
    *vs = v;
    *code = c;
    return 1;
}

static int vsh_lookup(const Nv2aVshDraw *d, const uint8_t kind[16])
{
    static char src[262144];
    ID3D11Device *dev = d3d8_GetD3D11Device();
    uint64_t key;
    ID3D10Blob *code = NULL, *err = NULL;
    HRESULT hr;
    int i, len, hit = 0, pts = d->topology == D3DPT_POINTLIST;
    double t0;

    key = vsh_key(d->prog_hash, kind, pts);
    if (d3d8_pump_cache_on()) {
        i = kidx_find(g_vsh_idx, VSH_CACHE * 2, key);
        if (i >= 0) return g_vsh[i].vs ? i : -1;
    } else
    for (i = 0; i < g_nvsh; i++)
        if (g_vsh[i].key == key) return g_vsh[i].vs ? i : -1;
    if (g_nvsh >= VSH_CACHE || !dev) return -1;
    i = g_nvsh++;
    g_vsh[i].key = key;
    kidx_put(g_vsh_idx, VSH_CACHE * 2, key, i);
    g_vsh[i].vs = NULL;
    g_vsh[i].code = NULL;
    {   /* built ahead by the prewarm thread. */
        void *pv = NULL, *pc = NULL;
        if (shpw_take_vs(key, &pv, &pc)) {
            g_vsh[i].vs = (ID3D11VertexShader *)pv;
            g_vsh[i].code = (ID3D10Blob *)pc;
            nv_pump_log("vs", key, "prewarmed", 0.0);
            return i;
        }
    }
    t0 = nv_now_ms();
    len = nv2a_vsh_hlsl(d->prog, d->prog_len, d->inputs, kind, pts, src, (int)sizeof src);
    if (len <= 0) return -1;
    nv_hlsl_dump("vs_nv2a_prog", key, src, len);
    {
        double tc = nv_now_ms();
        hr = nv_compile(src, (SIZE_T)len, NV_VS_NAME, NV_VS_PROFILE,
                        NV_VS_FLAGS, &code, &err, &hit);
        if (!hit) {
            g_nv_compiles++;
            if (g_perf_on) perf_count(PC_COMPILE, 1);
        }
        g_nv_compile_ms += nv_now_ms() - tc;
    }
    if (FAILED(hr)) {
        static int told;
        if (told++ < 4)
            fprintf(stderr, "[NV2A-VSH] program %016llX did not compile: %s\n--- source ---\n%s\n",
                    (unsigned long long)key, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", src);
        if (err) ID3D10Blob_Release(err);
        return -1;
    }
    if (err) ID3D10Blob_Release(err);
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &g_vsh[i].vs);
    if (FAILED(hr)) { ID3D10Blob_Release(code); g_vsh[i].vs = NULL; return -1; }
    g_vsh[i].code = code;
    nv_pump_log("vs", key, hit ? "from the cache" : "compiled", nv_now_ms() - t0);
    vsh_note(d, kind, pts);
    {
        static int count;
        if (++count <= 64 || (count % 64) == 0)
            fprintf(stderr, "[NV2A-VSH] compiled vertex program %d (%d instructions)\n",
                    count, d->prog_len);
    }
    return i;
}

static ID3D11InputLayout *il_lookup(int vi, const D3D11_INPUT_ELEMENT_DESC *el, int n);
static struct { int vi, n; D3D11_INPUT_ELEMENT_DESC el[16]; ID3D11InputLayout *il; } g_il_last[2];
static int g_il_mru;                    /* as g_vsh_mru */

static int il_memo_is(int w, int vi, const D3D11_INPUT_ELEMENT_DESC *el, int n)
{
    return g_il_last[w].il && g_il_last[w].vi == vi && g_il_last[w].n == n &&
           !memcmp(g_il_last[w].el, el, (size_t)n * sizeof *el);
}

/* The input layout for this shader and these elements; the same elements
 * as the previous draw (XBOX_FIX_PUMP_MEMO2: as one of the last two) give
 * the same layout without hashing them again. */
static ID3D11InputLayout *il_get(int vi, const D3D11_INPUT_ELEMENT_DESC *el, int n)
{
    ID3D11InputLayout *il;
    int w, two = d3d8_pump_memo2_on();
    if (d3d8_pump_onemap_on()) {
        w = two ? g_il_mru : 0;
        if (il_memo_is(w, vi, el, n)) return g_il_last[w].il;
        if (two && il_memo_is(w ^ 1, vi, el, n)) { g_il_mru = w ^ 1; g_il_memo2_hits++; return g_il_last[w ^ 1].il; }
    }
    il = il_lookup(vi, el, n);
    w = two ? g_il_mru ^ 1 : 0;
    g_il_mru = w;
    g_il_last[w].il = il;
    g_il_last[w].vi = vi;
    g_il_last[w].n = n;
    memcpy(g_il_last[w].el, el, (size_t)n * sizeof *el);
    return il;
}

static ID3D11InputLayout *il_lookup(int vi, const D3D11_INPUT_ELEMENT_DESC *el, int n)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    uint64_t key = fnv64(14695981039346656037ull, &vi, sizeof vi);
    int i;
    for (i = 0; i < n; i++) {
        key = fnv64(key, &el[i].SemanticIndex, sizeof el[i].SemanticIndex);
        key = fnv64(key, &el[i].Format, sizeof el[i].Format);
        key = fnv64(key, &el[i].InputSlot, sizeof el[i].InputSlot);
        key = fnv64(key, &el[i].AlignedByteOffset, sizeof el[i].AlignedByteOffset);
    }
    if (d3d8_pump_cache_on()) {
        i = kidx_find(g_il_idx, IL_CACHE * 2, key);
        if (i >= 0) return g_il[i].il;
    } else
    for (i = 0; i < g_nil; i++)
        if (g_il[i].key == key) return g_il[i].il;
    if (!dev) return NULL;
    if (g_nil >= IL_CACHE) {
        static int told;
        if (!told++)
            fprintf(stderr, "[NV2A-VSH] input layout cache full (%d layouts): a draw that needs a new one "
                    "is dropped (counted in [PUMP] Caches)\n", IL_CACHE);
        return NULL;
    }
    i = g_nil++;
    g_il[i].key = key;
    kidx_put(g_il_idx, IL_CACHE * 2, key, i);
    g_il[i].il = NULL;
    {
        HRESULT hr = ID3D11Device_CreateInputLayout(dev, el, (UINT)n,
                                                    ID3D10Blob_GetBufferPointer(g_vsh[vi].code),
                                                    ID3D10Blob_GetBufferSize(g_vsh[vi].code), &g_il[i].il);
        static int told;
        if ((FAILED(hr) || !g_il[i].il) && told++ < 4)       /* kept NULL: its draws are dropped */
            fprintf(stderr, "[NV2A-VSH] input layout refused (hr %08lX, vertex program %d, %d elements): "
                    "its draws are dropped (counted in [PUMP] Caches)\n", (unsigned long)hr, vi, n);
    }
    return g_il[i].il;
}

/* Index ring: as the vertex ring, discarding when it wraps. */
static UINT ib_upload(ID3D11DeviceContext *ctx, const uint32_t *idx, UINT n)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT size = n * 4u, off;
    if (!g_ib_ring) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = IB_RING_SIZE;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &bd, NULL, &g_ib_ring)))
            return (UINT)-1;
    }
    if (size > IB_RING_SIZE) return (UINT)-1;
    if (g_ib_off + size > IB_RING_SIZE) { g_ib_off = 0; how = D3D11_MAP_WRITE_DISCARD; }
    if (FAILED(nv_map(ctx, (ID3D11Resource *)g_ib_ring, how, &m)))
        return (UINT)-1;
    off = g_ib_off;
    memcpy((uint8_t *)m.pData + off, idx, size);
    nv_unmap(ctx, (ID3D11Resource *)g_ib_ring);
    g_ib_off = (off + size + 15u) & ~15u;
    return off;
}

/* XBOX_FIX_PUMP_ONEMAP: one ring for the vertex groups and the indices of a
 * draw (bound as both vertex and index buffer), written by one Map once the
 * whole size is known -- a wrap can never fall between two uploads of one
 * draw. NO_OVERWRITE otherwise, as the other rings.
 *
 * XBOX_FIX_PUMP_RING (default 1): the ring wraps without DISCARD. A DISCARD
 * of the whole 48 MB cost about 1.5 to 2.5 ms on the frame that wrapped, one
 * frame in 9 to 16 (indicative: a development build), most likely the driver
 * handing out new memory for all of it (renaming); DXVK also counts the 48 MB
 * against its MaxMemoryPerSubmission and submits at once.
 * Only the draws of d3d8_nv2a_draw_program_gpu read the ring, each right
 * after its own write: every other draw binds its own vertex buffer in the
 * one slot its input layout reads (slot 0), or draws without one, and the
 * only other indexed draws (the tuning panel's) bind their own index buffer. So once an
 * event query ended at a write position has completed, the GPU has finished
 * with every byte below that position. A query is ended at each presented
 * frame, in d3d8_pump_frame_tick (head of host_present, before the post and
 * the Present; under DXVK an End may submit the commands waiting, and it ends
 * the open render pass, as the post's first target change would). A write
 * over bytes of the previous lap is a NO_OVERWRITE once a query past them
 * has completed and reads TRUE; otherwise (GPU far behind, frame bigger than
 * the ring), a DISCARD and a new lap, as before. In play, only queries a lap
 * old are read: long completed. Positions run across laps (lap * size +
 * offset), so a completed query covers the older laps too. The shaders read
 * the same bytes as before (the offsets of a lap begun by a DISCARD after a
 * failed check start again at 0); the CPU never waits for the GPU. A failed
 * Map is followed by a new lap.
 * XBOX_FIX_PUMP_RING=0: the previous path, a DISCARD at each wrap (a failed
 * Map is followed by a new lap there too).
 * XBOX_FIX_PUMP_RING_ALT=1 (identity test): the switch is read when a lap
 * begins, so the laps begun on an even presented frame wrap without DISCARD
 * and the others with one. XBOX_FIX_PUMP_ALT turns it off with the other
 * pump optimizations.
 * XBOX_FIX_PUMP_RING_MB=n (development builds, 1 to 48): a ring of n MB, to
 * wrap often. A draw larger than the ring is not drawn: counted, logged once.
 * XBOX_LOG_STATS=1: wraps, DISCARDs, slow Maps in the log (map_stats; every
 * Map timed). XBOX_PERF_CSV: a V line per presented frame (perf_ring_frame),
 * counts only. A lap begins at each wrap and at each DISCARD for lack of a
 * completed query. If the device were ever created again, the queries, their
 * queue and the lap state would have to go with g_vi_ring. */
#define VI_RING_SIZE (48u * 1024u * 1024u)
#define VI_MARKS 16
static ID3D11Buffer *g_vi_ring;
static UINT g_vi_off, g_vi_size = VI_RING_SIZE;
static int g_vi_on = -1, g_vi_alt;
static int g_vi_shared;                 /* this lap writes over the previous one, in the same memory */
static unsigned long long g_vi_lap;     /* laps since the first Map (lap 0) */
static unsigned long long g_vi_safe;    /* the GPU has finished with every position below */
static unsigned long long g_vi_marked;  /* position at the last query */
static ID3D11Query *g_vi_q[VI_MARKS];   /* queries in flight, the oldest at g_vi_qhead */
static unsigned long long g_vi_qpos[VI_MARKS];
static unsigned g_vi_qhead, g_vi_qn;
static unsigned g_vi_wraps, g_vi_discards, g_vi_fallbacks, g_vi_refused;   /* map_stats, V line */

static void vi_cfg(void)
{
    const char *e = getenv("XBOX_FIX_PUMP_RING");
    g_vi_on = !(e && e[0] == '0');
    e = getenv("XBOX_FIX_PUMP_RING_ALT");
    g_vi_alt = e && e[0] == '1';
    fprintf(stderr, "[PUMP] XBOX_FIX_PUMP_RING=%d%s: vertex/index ring of %u MB, %s\n", g_vi_on,
            g_vi_alt ? " (ALT: the laps begun on even frames)" : "", g_vi_size >> 20,
            g_vi_on ? "wrapping without DISCARD once the GPU has finished" : "DISCARD at each wrap");
}

/* The switch, read when a lap begins. The XBOX_FIX_PUMP_ALT term never turns
 * it off with the present code (ONEMAP, the ring's only user, is off on the
 * frames it turns off); it is kept so that the ring follows the family. */
static int vi_fix_now(void)
{
    return g_vi_on > 0 && !(g_vi_alt && (g_pump_frame & 1u)) && !pump_alt_off();
}

/* The oldest query in flight, read without flushing and without waiting:
 * 1 if it has completed (S_OK and TRUE) and is taken off. */
static int vi_pop(ID3D11DeviceContext *ctx)
{
    unsigned k = g_vi_qhead;
    BOOL done = FALSE;
    if (!g_vi_qn || ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)g_vi_q[k], &done, sizeof done,
                                                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done)
        return 0;
    if (g_vi_qpos[k] > g_vi_safe) g_vi_safe = g_vi_qpos[k];
    g_vi_qhead = (k + 1) % VI_MARKS;
    g_vi_qn--;
    return 1;
}

/* 1 if a completed query shows that the GPU has finished with every position
 * below `need`. */
static int vi_gpu_done(ID3D11DeviceContext *ctx, unsigned long long need)
{
    while (g_vi_safe < need && vi_pop(ctx)) {}
    return g_vi_safe >= need;
}

/* At each presented frame: a query ended at the write position, if it moved.
 * All slots in flight: the oldest (16 frames back) is read first; if it has
 * not completed, this frame is covered by a later query. */
static void vi_mark(ID3D11DeviceContext *ctx)
{
    unsigned long long pos = g_vi_lap * g_vi_size + g_vi_off;
    unsigned k;
    if (pos == g_vi_marked) return;
    if (g_vi_qn == VI_MARKS && !vi_pop(ctx)) return;
    k = (g_vi_qhead + g_vi_qn) % VI_MARKS;
    if (!g_vi_q[k]) {
        D3D11_QUERY_DESC qd;
        qd.Query = D3D11_QUERY_EVENT;
        qd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(d3d8_GetD3D11Device(), &qd, &g_vi_q[k]))) { g_vi_q[k] = NULL; return; }
    }
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)g_vi_q[k]);
    g_vi_qpos[k] = pos;
    g_vi_qn++;
    g_vi_marked = pos;
}

static void map_stats(void);

/* d3d8_pump_frame_tick, at each presented frame: the statistics of the frame
 * (map_stats, V line of XBOX_PERF_CSV), then the ring's query. */
static void vi_present(void)
{
    ID3D11DeviceContext *ctx;
    map_stats();
    if (g_perf_on) {
        static unsigned w0, d0, f0, r0;
        perf_ring_frame(g_vi_wraps - w0, g_vi_discards - d0, g_vi_fallbacks - f0, g_vi_refused - r0, g_vi_shared);
        w0 = g_vi_wraps; d0 = g_vi_discards; f0 = g_vi_fallbacks; r0 = g_vi_refused;
    }
    if (g_vi_ring && g_vi_on > 0 && (ctx = d3d8_GetD3D11Context()) != NULL) vi_mark(ctx);
}

static uint8_t *vi_ring_map(ID3D11DeviceContext *ctx, UINT size, UINT *off)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (!g_vi_ring) {
        D3D11_BUFFER_DESC bd;
        if (g_vi_on < 0) vi_cfg();
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = g_vi_size;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &bd, NULL, &g_vi_ring)))
            return NULL;
        g_vi_off = g_vi_size;           /* first Map = DISCARD, lap 0 */
        g_vi_lap = ~0ull;
    }
    if (size > g_vi_size) {             /* only with XBOX_FIX_PUMP_RING_MB: the draw is lost */
        if (!g_vi_refused++)
            fprintf(stderr, "[PUMP] a draw of %u bytes is larger than the vertex/index ring (%u MB): not drawn\n",
                    size, g_vi_size >> 20);
        return NULL;
    }
    if (g_vi_off + size > g_vi_size) {  /* a new lap */
        g_vi_off = 0;
        g_vi_lap++;
        g_vi_wraps++;
        g_vi_shared = g_vi_lap > 0 && vi_fix_now();
        if (!g_vi_shared) how = D3D11_MAP_WRITE_DISCARD;
    }
    /* A lap in the memory of the previous one checks each write, whatever the
     * switch says now: the bytes it replaces were written one lap earlier.
     * No completed query past them: a DISCARD and a new lap, as before. */
    if (g_vi_shared && !vi_gpu_done(ctx, (g_vi_lap - 1) * g_vi_size + g_vi_off + size)) {
        g_vi_off = 0;
        g_vi_lap++;
        g_vi_shared = 0;
        g_vi_fallbacks++;
        how = D3D11_MAP_WRITE_DISCARD;
    }
    if (how == D3D11_MAP_WRITE_DISCARD) {
        /* What the DISCARD hands out holds nothing of the earlier laps; if the
         * Map fails, the next Map begins a new lap all the same. */
        g_vi_safe = g_vi_lap * g_vi_size;
        g_vi_discards++;
    }
    if (FAILED(nv_map(ctx, (ID3D11Resource *)g_vi_ring, how, &m))) {
        g_vi_off = g_vi_size;           /* the next Map begins a new lap: never over bytes in an unknown state */
        return NULL;
    }
    *off = g_vi_off;
    g_vi_off = (g_vi_off + size + 15u) & ~15u;
    return (uint8_t *)m.pData + *off;
}

/* XBOX_LOG_STATS=1: every buffer Map of this file is timed (nv_map); a Map of
 * more than 100 us is slow (a DISCARD of the whole vertex/index ring took
 * about 1.5 to 2.5 ms on a development build). Every 10 s of presented
 * frames, one line: the wraps and DISCARDs of the vertex/index ring, its slow
 * and longest Maps, at a new lap and elsewhere, the draws larger than the
 * ring; the same for the other Maps (the constant ring, whose wraps are each
 * a DISCARD). With XBOX_FIX_PUMP_RING=0, the previous path's. Test tools read
 * this line: its fields keep their wording, new ones go after them. A second
 * line, [PUMP] Caches, gives how full the shader and input layout caches are
 * and the draws lost to them since the start. */
typedef struct { unsigned n, slow; unsigned long long top; } MapTimes;
static MapTimes g_mt[4];                /* vertex/index ring: [0] in a lap, [1] a new lap; others: [2], [3] DISCARD */
static unsigned long long g_mt_slow = ~0ull;    /* 100 us in rdtsc cycles, once measured */

static void map_note(ID3D11Resource *r, D3D11_MAP how, unsigned long long cyc)
{
    MapTimes *t = r == (ID3D11Resource *)g_vi_ring ? &g_mt[g_vi_off == 0 ? 1 : 0]
                                                   : &g_mt[how == D3D11_MAP_WRITE_DISCARD ? 3 : 2];
    t->n++;
    if (cyc > g_mt_slow) t->slow++;
    if (cyc > t->top) t->top = cyc;
}

static void map_stats(void)
{
    static LARGE_INTEGER f, t0, tw;
    static unsigned long long c0;
    static unsigned frames, w0, d0, fb0, r0, cg0;
    LARGE_INTEGER now;
    double us, k;
    if (!xbox_log_stats()) return;
    QueryPerformanceCounter(&now);
    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f);
        t0 = tw = now;
        c0 = __rdtsc();
        w0 = g_vi_wraps; d0 = g_vi_discards; fb0 = g_vi_fallbacks; r0 = g_vi_refused; cg0 = g_cbr_gen;
        return;
    }
    frames++;
    us = (double)(now.QuadPart - t0.QuadPart) * 1e6 / (double)f.QuadPart;
    if (us < 1e5) return;               /* the rdtsc rate, measured over 0.1 s at least */
    k = (double)(__rdtsc() - c0) / us;
    g_mt_slow = (unsigned long long)(100.0 * k);
    if (now.QuadPart - tw.QuadPart < 10 * f.QuadPart) return;
    fprintf(stderr, "[PUMP] Maps, last %.1f s, %u frames: vertex/index ring (%u MB) %u wraps, %u DISCARD "
            "(%u for lack of a completed query); slow (> 100 us) %u in a lap, %u at a new lap; longest %.0f us, "
            "%.0f at a new lap; %u draws larger than the ring | other Maps: %u constant ring wraps; slow %u, "
            "%u DISCARD; longest %.0f us, %.0f DISCARD | since start: %u wraps, %u DISCARD (%u for lack of a query), "
            "%u draws larger than the ring\n",
            (double)(now.QuadPart - tw.QuadPart) / (double)f.QuadPart, frames, g_vi_size >> 20,
            g_vi_wraps - w0, g_vi_discards - d0, g_vi_fallbacks - fb0, g_mt[0].slow, g_mt[1].slow,
            (double)g_mt[0].top / k, (double)g_mt[1].top / k, g_vi_refused - r0, g_cbr_gen - cg0, g_mt[2].slow,
            g_mt[3].slow, (double)g_mt[2].top / k, (double)g_mt[3].top / k, g_vi_wraps, g_vi_discards,
            g_vi_fallbacks, g_vi_refused);
    fprintf(stderr, "[PUMP] Caches: pixel shaders %d / %d, vertex programs %d / %d, input layouts %d / %d | "
            "since start: %u draws skipped for a full pixel shader cache, %u dropped without an input layout\n",
            g_ps_count, NV_PS_CACHE, g_nvsh, VSH_CACHE, g_nil, IL_CACHE, g_ps_full_skips, g_il_lost);
    xbox_log_flush();
    memset(g_mt, 0, sizeof g_mt);
    tw = now;
    frames = 0;
    w0 = g_vi_wraps; d0 = g_vi_discards; fb0 = g_vi_fallbacks; r0 = g_vi_refused; cg0 = g_cbr_gen;
}

/* Point sprites on the GPU. The CPU path turned each point into
 * a square of its size around the snapped centre, in title pixels -- corners
 * (-h,-h) (+h,-h) (-h,+h) (+h,+h), triangles 0 1 2 and 2 1 3 -- with the
 * sprite coordinate in t3 under SET_POINT_SMOOTH_ENABLE, and dropped a point
 * whose vertex could not be placed. This geometry shader does the same with
 * the same arithmetic: a strip of those four corners rasterises as those two
 * triangles. */
/* NaN read from the bits, as in the vertex programs (d3d8_nv2a_vsh.c). */
#define NV_GS_CB \
    "cbuffer Params : register(b1) {\n" \
    "    float4 screen; float4 fogp; float4 flags; float4 vattr[16]; float4 ptparm;\n"
#define NV_GS_BODY_A \
    "};\n" \
    "struct VSOut {\n" \
    "    float4 pos : SV_POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n" \
    "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n" \
    "    float4 pt : PTS;\n" \
    "};\n" \
    "struct GSOut {\n" \
    "    float4 pos : SV_POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n" \
    "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n" \
    "};\n" \
    "bool nv_isnan(float x) { return (asuint(x) & 0x7FFFFFFF) > 0x7F800000; }\n" \
    "[maxvertexcount(4)]\n" \
    "void main(point VSOut p[1], inout TriangleStream<GSOut> s) {\n" \
    "    VSOut v = p[0];\n" \
    "    if (nv_isnan(v.pos.w)) return;\n" \
    "    float h = v.pt.z, wc = v.pos.w;\n" \
    "    for (int c = 0; c < 4; c++) {\n" \
    "        GSOut o;\n" \
    "        float x = v.pt.x + ((c & 1) ? h : -h) * ptparm.w, y = v.pt.y + ((c & 2) ? h : -h);\n" \
    "        o.pos = float4((x / screen.x * 2.0 - 1.0) * wc, (1.0 - y / screen.y * 2.0) * wc, v.pos.z, wc);\n"
#define NV_GS_BODY_B \
    "        o.d0 = v.d0; o.d1 = v.d1; o.fog = v.fog;\n" \
    "        o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2;\n" \
    "        o.t3 = ptparm.z != 0 ? float4((c & 1) ? 1 : 0, (c & 2) ? 1 : 0, 1, 1) : v.t3;\n" \
    "        s.Append(o);\n" \
    "    }\n" \
    "}\n"
static const char g_gs_src[] = NV_GS_CB NV_GS_BODY_A NV_GS_BODY_B;
/* Fork: with XBOX_TEMPORAL, the image's jitter (Params.jit, NDC) on every
 * corner, as the vertex programs add it (d3d8_nv2a_vsh.c). The stream-out
 * check (XBOX_POINTS_CHECK) keeps the plain shader. */
static const char g_gs_src_jit[] = NV_GS_CB "    float4 hud; float4 jit;\n" NV_GS_BODY_A
    "        o.pos.xy += jit.xy * wc;\n" NV_GS_BODY_B;
static ID3D11GeometryShader *g_gs;
static int g_gs_failed;

static int gs_init(void)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr;
    if (g_gs) return 1;
    if (g_gs_failed || !dev) return 0;
    {
        const char *src = d3d8_temporal_wanted() ? g_gs_src_jit : g_gs_src;
        nv_hlsl_dump("gs_nv2a_points", 0, src, (int)strlen(src));
        hr = d3d8_shader_compile_logged(src, strlen(src), "gs_nv2a_points", NULL, "main",
                                        "gs_5_0", D3DCOMPILE_IEEE_STRICTNESS, &blob, &err);
    }
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateGeometryShader(dev, ID3D10Blob_GetBufferPointer(blob),
                                               ID3D10Blob_GetBufferSize(blob), NULL, &g_gs);
    if (FAILED(hr) || !g_gs) {
        fprintf(stderr, "[NV2A-VSH] point geometry shader failed (hr %08lX): %s\n", (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        g_gs_failed = 1;
    }
    if (blob) ID3D10Blob_Release(blob);
    if (err) ID3D10Blob_Release(err);
    return g_gs != NULL;
}

/* The point geometry shader, compiled by the shader prewarm
 * thread (d3d8_shprewarm.c) into the disk cache with gs_init's arguments, so
 * its first point draw -- the particles, sometimes first seen in a race --
 * reads it back instead of compiling it. */
void d3d8_nv2a_prewarm_fixed(void)
{
    ID3DBlob *blob = NULL, *err = NULL;
    const char *src;
    int hit = 0;
    if (!d3d8_points_gpu_on()) return;
    src = d3d8_temporal_wanted() ? g_gs_src_jit : g_gs_src;
    if (SUCCEEDED(d3d8_shader_compile(src, strlen(src), "gs_nv2a_points", NULL, "main", "gs_5_0",
                                      D3DCOMPILE_IEEE_STRICTNESS, &blob, &err, &hit)) && blob)
        ID3D10Blob_Release(blob);
    if (err) ID3D10Blob_Release(err);
}

/* XBOX_POINTS_CHECK=1 (diagnostic): for each point draw on the
 * GPU, the translator also builds the CPU squares (points_squares) and hands
 * them over; the geometry shader's output is captured by stream output and
 * compared vertex by vertex -- position in pixels, colours, fog, texture
 * coordinates, number of vertices. Stalls on every point draw. */
static const float *g_pc_ref;
static unsigned g_pc_n, g_pc_stride;
static ID3D11GeometryShader *g_gs_so;
static ID3D11Buffer *g_so_buf, *g_so_stage;
static ID3D11Query *g_so_q;
#define PC_FLOATS 29                    /* pos, d0, d1, fog, t0..t3 */
#define PC_MAXV   (6u * 20000u)
static struct {
    unsigned long long draws, verts, exact, count_bad, skipped;
    double dpix, dattr;                 /* largest |difference| seen */
    unsigned long long pix_over;        /* vertices more than 1/256 px apart */
    unsigned long long rotated;
    unsigned long long visible;         /* in front of the eye, near the screen */         /* matched to another vertex of the triangle */
} g_pcs;

int d3d8_points_check_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_POINTS_CHECK"); on = e && e[0] == '1'; }
    return on;
}

void d3d8_nv2a_points_expect(const void *verts, unsigned n, unsigned stride)
{
    g_pc_ref = (const float *)verts;
    g_pc_n = n;
    g_pc_stride = stride;
}

static int pc_init(void)
{
    static int failed;
    ID3D11Device *dev = d3d8_GetD3D11Device();
    if (g_gs_so) return 1;
    if (failed || !dev) return 0;
    failed = 1;
    {
        static const D3D11_SO_DECLARATION_ENTRY so[] = {
            { 0, "SV_POSITION", 0, 0, 4, 0 }, { 0, "COLOR", 0, 0, 4, 0 }, { 0, "COLOR", 1, 0, 4, 0 },
            { 0, "FOG", 0, 0, 1, 0 }, { 0, "TEXCOORD", 0, 0, 4, 0 }, { 0, "TEXCOORD", 1, 0, 4, 0 },
            { 0, "TEXCOORD", 2, 0, 4, 0 }, { 0, "TEXCOORD", 3, 0, 4, 0 },
        };
        UINT stride = PC_FLOATS * 4;
        ID3DBlob *blob = NULL, *err = NULL;
        D3D11_BUFFER_DESC bd;
        D3D11_QUERY_DESC qd;
        HRESULT hr = d3d8_shader_compile_logged(g_gs_src, strlen(g_gs_src), "gs_nv2a_points", NULL,
                                                "main", "gs_5_0", D3DCOMPILE_IEEE_STRICTNESS,
                                                &blob, &err);
        if (err) ID3D10Blob_Release(err);
        if (FAILED(hr)) return 0;
        hr = ID3D11Device_CreateGeometryShaderWithStreamOutput(dev, ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob), so, 8, &stride, 1, 0, NULL, &g_gs_so);
        ID3D10Blob_Release(blob);
        if (FAILED(hr)) return 0;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = PC_MAXV * PC_FLOATS * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &g_so_buf))) return 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &g_so_stage))) return 0;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_SO_STATISTICS;
        if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &g_so_q))) return 0;
    }
    failed = 0;
    fprintf(stderr, "[PTSCHECK] CPU / GPU point comparison active\n");
    return 1;
}

static void pc_compare(ID3D11DeviceContext *ctx, float sw, float sh)
{
    D3D11_QUERY_DATA_SO_STATISTICS st;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned v, k, nv;
    if (g_pc_n > PC_MAXV) { g_pcs.skipped++; return; }
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)g_so_q, &st, sizeof st, 0) == S_FALSE)
        ;
    nv = (unsigned)st.NumPrimitivesWritten * 3u;
    g_pcs.draws++;
    if (nv != g_pc_n) {
        if (g_pcs.count_bad++ < 4)
            fprintf(stderr, "[PTSCHECK] draw %llu: %u vertices GPU, %u CPU\n", g_pcs.draws, nv, g_pc_n);
        if (nv > g_pc_n) nv = g_pc_n;
    }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)g_so_stage, (ID3D11Resource *)g_so_buf);
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_so_stage, 0, D3D11_MAP_READ, 0, &m))) return;
    for (v = 0; v < nv; v++) {
        const float *g = (const float *)m.pData + (size_t)v * PC_FLOATS;
        const float *c;
        float w, ref[PC_FLOATS];
        double gx, gy, dp, da = 0.0;
        /* Stream output may start a triangle at another of its vertices
         * (same triangle, same winding): match within the triangle. */
        {
            unsigned t0 = v - v % 3u, j, best = v;
            double bd = 1e30;
            gx = ((double)g[0] / g[3] + 1.0) * 0.5 * sw;
            gy = (1.0 - (double)g[1] / g[3]) * 0.5 * sh;
            for (j = t0; j < t0 + 3u && j < g_pc_n; j++) {
                const float *cj = (const float *)((const uint8_t *)g_pc_ref + (size_t)j * g_pc_stride);
                double d = fabs(gx - cj[0]) + fabs(gy - cj[1]);
                if (d < bd) { bd = d; best = j; }
            }
            if (best != v) g_pcs.rotated++;
            c = (const float *)((const uint8_t *)g_pc_ref + (size_t)best * g_pc_stride);
        }
        w = 1.0f / c[3];
        /* The CPU path's clip position, as its pass-through vertex shader makes it. */
        ref[0] = (c[0] / sw * 2.0f - 1.0f) * w;
        ref[1] = (1.0f - c[1] / sh * 2.0f) * w;
        ref[2] = c[2] * w;
        ref[3] = w;
        memcpy(ref + 4, c + 4, (PC_FLOATS - 4) * sizeof(float));
        gx = ((double)g[0] / g[3] + 1.0) * 0.5 * sw;
        gy = (1.0 - (double)g[1] / g[3]) * 0.5 * sh;
        dp = fabs(gx - c[0]) > fabs(gy - c[1]) ? fabs(gx - c[0]) : fabs(gy - c[1]);
        for (k = 4; k < PC_FLOATS; k++) {
            double d = fabs((double)g[k] - ref[k]);
            if (d > da) da = d;
        }
        /* Pixel and attribute gaps are measured on the vertices the rasteriser
         * can show: in front of the eye, within a 64-pixel guard band. Behind
         * the eye or far outside, a 1-ulp change of a clamped w is billions
         * of "pixels" and says nothing about the image. */
        if (c[3] > 0.0f && c[0] > -64.0f && c[0] < sw + 64.0f && c[1] > -64.0f && c[1] < sh + 64.0f) {
            g_pcs.visible++;
            if (dp > g_pcs.dpix) g_pcs.dpix = dp;
            if (da > g_pcs.dattr) g_pcs.dattr = da;
            if ((dp > 1.0 / 256.0 || da > 1.0 / 512.0) && g_pcs.pix_over++ < 4) {
                fprintf(stderr, "[PTSCHECK] visible gap %.4g px, attributes %.4g: GPU", dp, da);
                for (k = 0; k < PC_FLOATS; k++) fprintf(stderr, " %.9g", g[k]);
                fprintf(stderr, "\n[PTSCHECK]   CPU x %.9g y %.9g rhw %.9g, attributes", c[0], c[1], c[3]);
                for (k = 4; k < PC_FLOATS; k++) fprintf(stderr, " %.9g", ref[k]);
                fprintf(stderr, "\n");
            } else if (dp > 1.0 / 256.0 || da > 1.0 / 512.0) {
                g_pcs.pix_over++;
            }
        }
        if (!memcmp(g, ref, sizeof ref)) g_pcs.exact++;
        else if (g_pcs.verts - g_pcs.exact < 3) {
            fprintf(stderr, "[PTSCHECK] different vertex: GPU");
            for (k = 0; k < PC_FLOATS; k++) fprintf(stderr, " %.9g", g[k]);
            fprintf(stderr, "\n[PTSCHECK]                   CPU");
            for (k = 0; k < PC_FLOATS; k++) fprintf(stderr, " %.9g", ref[k]);
            fprintf(stderr, "\n");
        }
        g_pcs.verts++;
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_so_stage, 0);
    if ((g_pcs.draws % 100) == 1)
        fprintf(stderr, "[PTSCHECK] %llu draws, %llu vertices: %llu bit identical, max gap %.3g px "
                "over %llu visible (%llu > 1/256 px or attribute > 1/512), attributes %.3g; order in the triangle %llu; different vertex count %llu, skipped %llu\n",
                g_pcs.draws, g_pcs.verts, g_pcs.exact, g_pcs.dpix, g_pcs.visible, g_pcs.pix_over, g_pcs.dattr, g_pcs.rotated,
                g_pcs.count_bad, g_pcs.skipped);
}

int d3d8_points_gpu_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_POINTS_GPU"); on = !(e && e[0] == '0'); }
    return on && !pump_alt_off();
}

int d3d8_nv2a_vsh_ready(const Nv2aVshDraw *d)
{
    return nv_init() && (d->topology != D3DPT_POINTLIST || gs_init()) && vsh_get(d) >= 0;
}

/* Largest block of constant attributes uploaded together (ONEMAP); far
 * below the 2048-byte limit of AlignedByteOffset. */
#define ZS_WINDOW 256u

/* ── Stable ties inside one mesh (XBOX_ZTIE, raster bit 0x40) ────────────
 * nv2a_pgraph_d3d11.c marks the draws of meshes whose own faces overlap in
 * one plane (Garibaldi's stand trusses). Drawn in one pass, the face that
 * shows there is decided by rounding and flips as the camera moves. Here,
 * three passes with the same buffers and shaders:
 *   1. depth only, the title's test: the nearest depth of the mesh;
 *   2. colour, LESS_EQUAL against it with the depth pulled T steps nearer
 *      (XBOX_ZTIE_STEPS, 8, plus XBOX_ZTIE_SLOPE x the slope per title pixel,
 *      0.5), no depth write, and stencil bit 0x80 as "first come": among the
 *      faces within T of the nearest, the first one in the mesh's order wins,
 *      always the same one (output order follows primitive order);
 *   3. stencil bit 0x80 back to 0 on the mesh (no colour, no depth).
 * Away from ties only one face is within T of the nearest: the same image.
 * Used only when the title's own state has depth test and write on and its
 * stencil off; the lights' and temporal mirrors after the draw see one draw
 * (d3d8_nv2a_draw_program_gpu). */
#define ZTIE_BIT 0x80u
static struct { D3D11_DEPTH_STENCIL_DESC d; ID3D11DepthStencilState *p2, *p3; } g_zt_ds[16];
static struct { ID3D11RasterizerState *orig, *bias; float scale; } g_zt_rs[16];
static ID3D11BlendState *g_zt_nocolor;
static int   g_zt_steps = -1;
static float g_zt_slope = 0.5f;
static unsigned g_zt_frames_draws;
int g_d3d8_zcost;
unsigned long long g_d3d8_zc[4];

static void zt_params(void)
{
    const char *e;
    if (g_zt_steps >= 0) return;
    e = getenv("XBOX_ZTIE_STEPS");
    g_zt_steps = e && *e ? atoi(e) : 8;
    if (g_zt_steps < 0) g_zt_steps = 0;
    e = getenv("XBOX_ZTIE_SLOPE");
    if (e && *e) g_zt_slope = (float)atof(e);
}

/* Passes 2 and 3 for the title's depth-stencil state (cached). */
static int zt_ds(ID3D11Device *dev, const D3D11_DEPTH_STENCIL_DESC *t,
                 ID3D11DepthStencilState **p2, ID3D11DepthStencilState **p3)
{
    int i;
    for (i = 0; i < 16; i++)
        if (g_zt_ds[i].p2 && !memcmp(&g_zt_ds[i].d, t, sizeof *t)) { *p2 = g_zt_ds[i].p2; *p3 = g_zt_ds[i].p3; return 1; }
    for (i = 0; i < 16; i++)
        if (!g_zt_ds[i].p2) {
            D3D11_DEPTH_STENCIL_DESC a = *t, b;
            a.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            a.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            a.StencilEnable = TRUE;
            a.StencilReadMask = ZTIE_BIT;
            a.StencilWriteMask = ZTIE_BIT;
            a.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;          /* bit still 0 (ref 0) */
            a.FrontFace.StencilPassOp = D3D11_STENCIL_OP_INVERT;       /* set it (only bit 0x80 written) */
            a.FrontFace.StencilFailOp = a.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            a.BackFace = a.FrontFace;
            memset(&b, 0, sizeof b);
            b.DepthEnable = FALSE;
            b.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            b.DepthFunc = D3D11_COMPARISON_ALWAYS;
            b.StencilEnable = TRUE;
            b.StencilReadMask = ZTIE_BIT;
            b.StencilWriteMask = ZTIE_BIT;
            b.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
            b.FrontFace.StencilPassOp = D3D11_STENCIL_OP_ZERO;
            b.FrontFace.StencilFailOp = b.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            b.BackFace = b.FrontFace;
            if (FAILED(ID3D11Device_CreateDepthStencilState(dev, &a, &g_zt_ds[i].p2))) return 0;
            if (FAILED(ID3D11Device_CreateDepthStencilState(dev, &b, &g_zt_ds[i].p3))) {
                ID3D11DepthStencilState_Release(g_zt_ds[i].p2); g_zt_ds[i].p2 = NULL; return 0;
            }
            g_zt_ds[i].d = *t;
            *p2 = g_zt_ds[i].p2; *p3 = g_zt_ds[i].p3;
            return 1;
        }
    return 0;
}

/* The current rasteriser state with the pass-2 depth bias (cached). */
static ID3D11RasterizerState *zt_rs(ID3D11Device *dev, ID3D11RasterizerState *orig)
{
    float sx = 1.0f, sy = 1.0f;
    int i;
    d3d8_GetGuestScale(&sx, &sy);
    for (i = 0; i < 16; i++)
        if (g_zt_rs[i].orig == orig && g_zt_rs[i].scale == sx) return g_zt_rs[i].bias;
    for (i = 0; i < 16; i++)
        if (!g_zt_rs[i].orig || i == 15) {
            D3D11_RASTERIZER_DESC rd;
            if (g_zt_rs[i].bias) { ID3D11RasterizerState_Release(g_zt_rs[i].bias); g_zt_rs[i].bias = NULL; }
            ID3D11RasterizerState_GetDesc(orig, &rd);
            rd.DepthBias = -g_zt_steps;
            rd.SlopeScaledDepthBias = -g_zt_slope * sx;   /* per title pixel, at any render size */
            /* At most 3 T: a face seen nearly edge-on (huge slope) must not
             * bring faces far behind it into the band (seen on the snowcat). */
            rd.DepthBiasClamp = -3.0f * (float)(g_zt_steps > 0 ? g_zt_steps : 1) / 16777215.0f;
            if (FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &g_zt_rs[i].bias))) { g_zt_rs[i].orig = NULL; return NULL; }
            g_zt_rs[i].orig = orig;                        /* cached by pointer: the states live as long as the device */
            g_zt_rs[i].scale = sx;
            return g_zt_rs[i].bias;
        }
    return NULL;
}

/* The title's draw (n indices), with stable ties when `raster` has 0x40: the
 * first nsub indices (the triangles that overlap in one plane) in three
 * passes, then the others as the title draws them. */
/* Stable decals over the previous draw (raster bit 0x80, marked by
 * nv2a_pgraph_d3d11.c): the first nsub indices -- the triangles lying on the
 * previous draw -- with the title's states but the depth pulled the same few
 * steps nearer as ZTIE's pass 2 (XBOX_ZTIE_STEPS / _SLOPE, at most 3 T), so
 * they win their ties with it; then the others as the title draws them. One
 * pass each: depth test and write as the title's. */
/* The decals' own pull, smaller than ZTIE's: a decal only has to beat
 * rounding (0-4 steps seen on Merqury's windows), and a larger pull lets its
 * edges show through the frame around it (seen at 640x480 with 8 steps +
 * 0.5 x slope). XBOX_ZDECAL_STEPS (4) and XBOX_ZDECAL_SLOPE (0.25),
 * diagnostics only; at most 3 x the steps. */
static struct { ID3D11RasterizerState *orig, *bias; float scale; } g_zd_rs[16];
static int   g_zd_steps = -1;
static float g_zd_slope = 0.25f;

static ID3D11RasterizerState *zd_rs(ID3D11Device *dev, ID3D11RasterizerState *orig)
{
    float sx = 1.0f, sy = 1.0f;
    int i;
    if (g_zd_steps < 0) {
        const char *e = getenv("XBOX_ZDECAL_STEPS");
        g_zd_steps = e && *e ? atoi(e) : 4;
        if (g_zd_steps < 0) g_zd_steps = 0;
        e = getenv("XBOX_ZDECAL_SLOPE");
        if (e && *e) g_zd_slope = (float)atof(e);
    }
    d3d8_GetGuestScale(&sx, &sy);
    for (i = 0; i < 16; i++)
        if (g_zd_rs[i].orig == orig && g_zd_rs[i].scale == sx) return g_zd_rs[i].bias;
    for (i = 0; i < 16; i++)
        if (!g_zd_rs[i].orig || i == 15) {
            D3D11_RASTERIZER_DESC rd;
            if (g_zd_rs[i].bias) { ID3D11RasterizerState_Release(g_zd_rs[i].bias); g_zd_rs[i].bias = NULL; }
            ID3D11RasterizerState_GetDesc(orig, &rd);
            rd.DepthBias = -g_zd_steps;
            rd.SlopeScaledDepthBias = -g_zd_slope * sx;
            rd.DepthBiasClamp = -3.0f * (float)(g_zd_steps > 0 ? g_zd_steps : 1) / 16777215.0f;
            if (FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &g_zd_rs[i].bias))) { g_zd_rs[i].orig = NULL; return NULL; }
            g_zd_rs[i].orig = orig;
            g_zd_rs[i].scale = sx;
            return g_zd_rs[i].bias;
        }
    return NULL;
}

static void zd_draw_indexed(ID3D11DeviceContext *ctx, ID3D11Device *dev, UINT n, UINT nsub)
{
    ID3D11RasterizerState *ors = NULL, *brs;
    ID3D11DeviceContext_RSGetState(ctx, &ors);
    if (!ors || !(brs = zd_rs(dev, ors))) {
        ID3D11DeviceContext_DrawIndexed(ctx, n, 0, 0);
    } else {
        ID3D11DeviceContext_RSSetState(ctx, brs);
        ID3D11DeviceContext_DrawIndexed(ctx, nsub, 0, 0);
        ID3D11DeviceContext_RSSetState(ctx, ors);
        if (n > nsub) ID3D11DeviceContext_DrawIndexed(ctx, n - nsub, nsub, 0);
    }
    if (ors) ID3D11RasterizerState_Release(ors);
}

/* A mirror of the title's draw (the lights' emission and emitters' depth,
 * the temporal mask): drawn as the title's was. A stable decal's first nsub
 * triangles wrote their depth pulled nearer (zd_draw_indexed); unpulled,
 * their mirror lost its LESS_EQUAL test against that depth, and a sign drawn
 * as a decal on its frame glowed only on the images ZDECAL left it alone. */
/* Diagnostics port (set zdmirror 0): the mirrors unpulled, as before -- the
 * comparison on one held image. */
static int s_zd_mirror = 1;
void d3d8_nv2a_set_zd_mirror(int on) { s_zd_mirror = on ? 1 : 0; }

static void mirror_draw_indexed(ID3D11DeviceContext *ctx, ID3D11Device *dev, const Nv2aVshDraw *d)
{
    if (s_zd_mirror && (d->raster & 0x80) && d->zdecal_nsub && d->zdecal_nsub <= d->nindices)
        zd_draw_indexed(ctx, dev, d->nindices, d->zdecal_nsub);
    else
        ID3D11DeviceContext_DrawIndexed(ctx, d->nindices, 0, 0);
}

static void zt_draw_indexed(ID3D11DeviceContext *ctx, ID3D11Device *dev, int raster, UINT n, UINT nsub)
{
    ID3D11DepthStencilState *ods = NULL, *p2 = NULL, *p3 = NULL;
    ID3D11BlendState *obs = NULL;
    ID3D11RasterizerState *ors = NULL, *brs;
    D3D11_DEPTH_STENCIL_DESC dd;
    FLOAT bf[4];
    UINT oref = 0, omask = 0;
    if ((raster & 0x80) && nsub && nsub <= n) { zd_draw_indexed(ctx, dev, n, nsub); return; }
    if (!(raster & 0x40) || !nsub || nsub > n) { ID3D11DeviceContext_DrawIndexed(ctx, n, 0, 0); return; }
    zt_params();
    ID3D11DeviceContext_OMGetDepthStencilState(ctx, &ods, &oref);
    ID3D11DeviceContext_OMGetBlendState(ctx, &obs, bf, &omask);
    ID3D11DeviceContext_RSGetState(ctx, &ors);
    if (ods) ID3D11DepthStencilState_GetDesc(ods, &dd);
    if (!g_zt_nocolor) {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].RenderTargetWriteMask = 0;
        ID3D11Device_CreateBlendState(dev, &bd, &g_zt_nocolor);
    }
    if (!ods || !ors || !g_zt_nocolor || !dd.DepthEnable || dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ALL ||
        dd.StencilEnable || !zt_ds(dev, &dd, &p2, &p3) || !(brs = zt_rs(dev, ors))) {
        ID3D11DeviceContext_DrawIndexed(ctx, n, 0, 0);      /* not for this state: as the title draws it */
    } else {
        ID3D11DeviceContext_OMSetBlendState(ctx, g_zt_nocolor, bf, omask);           /* 1. depth */
        ID3D11DeviceContext_DrawIndexed(ctx, nsub, 0, 0);
        ID3D11DeviceContext_OMSetBlendState(ctx, obs, bf, omask);                    /* 2. colour */
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, p2, 0);
        ID3D11DeviceContext_RSSetState(ctx, brs);
        ID3D11DeviceContext_DrawIndexed(ctx, nsub, 0, 0);
        ID3D11DeviceContext_OMSetBlendState(ctx, g_zt_nocolor, bf, omask);           /* 3. bit back to 0 */
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, p3, 0);
        ID3D11DeviceContext_RSSetState(ctx, ors);
        ID3D11DeviceContext_DrawIndexed(ctx, nsub, 0, 0);
        ID3D11DeviceContext_OMSetBlendState(ctx, obs, bf, omask);                    /* the title's states back */
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, ods, oref);
        if (n > nsub) ID3D11DeviceContext_DrawIndexed(ctx, n - nsub, nsub, 0);   /* the others */
        g_zt_frames_draws++;
    }
    if (ods) ID3D11DepthStencilState_Release(ods);
    if (obs) ID3D11BlendState_Release(obs);
    if (ors) ID3D11RasterizerState_Release(ors);
}


/* The order in which a GPU draw's streams are grouped: its array attributes
 * (inputs read by the program, not constant) by increasing address, the
 * lower attribute number first at equal addresses. ord[] receives them in
 * that order; returns how many.
 *
 * stream_order_scan is the previous form: one scan of the 16 attributes per
 * attribute taken (16 x (n + 1) tests with data-dependent branches, about
 * 0.1 us per draw). stream_order takes the set bits only, each inserted after
 * every element whose address is lower or equal: a stable sort on the
 * address over the attribute numbers taken in increasing order, so the same
 * sequence, hence the same groups, elements and input slots. */
static int stream_order_scan(const Nv2aVshDraw *d, int ord[16])
{
    uint16_t done = 0;
    int a, n = 0;
    for (;;) {
        int best = -1;
        for (a = 0; a < 16; a++)
            if ((d->inputs & (1u << a)) && !(done & (1u << a)) &&
                (d->attr[a].kind & 0x0F) != NV2A_VSH_IN_CONST &&
                (best < 0 || d->attr[a].base < d->attr[best].base))
                best = a;
        if (best < 0) break;
        done |= (uint16_t)(1u << best);
        ord[n++] = best;
    }
    return n;
}

static int stream_order(const Nv2aVshDraw *d, int ord[16])
{
    unsigned m = d->inputs;
    int n = 0;
    while (m) {
        int a = __builtin_ctz(m), k;
        const uint8_t *b = d->attr[a].base;
        m &= m - 1u;
        if ((d->attr[a].kind & 0x0F) == NV2A_VSH_IN_CONST) continue;
        for (k = n++; k > 0 && d->attr[ord[k - 1]].base > b; k--) ord[k] = ord[k - 1];
        ord[k] = a;
    }
    return n;
}

/* XBOX_FIX_PUMP_GROUP (default 1; =0 the previous scan): stream_order
 * above. Off with the other optimizations on the XBOX_FIX_PUMP_ALT frames. */
static int pump_group_on(void) { return (d3d8_pump_flags() & PF_GROUP) != 0; }


int d3d8_nv2a_draw_program_gpu(const Nv2aVshDraw *d)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    int lr;                             /* fork: the lights (d3d8_lights.h) */
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3D11PixelShader *ps = ps_find(d->ps_key);
    D3D11_INPUT_ELEMENT_DESC el[16];
    struct { const uint8_t *base; UINT stride, size; } grp[16];
    ID3D11Buffer *vbs[16], *cbs[2], *ib_buf;
    UINT strides[16], offs[16], ib_off;
    ID3D11InputLayout *il;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    int vi, a, g, ngrp = 0, nel = 0;
    int ord[16], nord, oi;              /* the streams in grouping order (stream_order) */
    struct { float screen[4], fog[4], flags[4], vattr[16][4], point[4], hud[4], jit[4]; } params;
    double pt = 0.0;                    /* XBOX_PERF sub-zones */
    int use_cbr, vsc_changed = 0;       /* constant ring */
    static int g_cbr_was;
    int points = d->topology == D3DPT_POINTLIST;
    unsigned maps0 = g_nv_maps;         /* XBOX_PERF: Maps of this draw */
    int onemap = d3d8_pump_onemap_on();
    double pd = g_perf_detail ? perf_now() : 0.0;   /* XBOX_PERF_DETAIL, before the uploads */

    if (!ctx || !dev || !ps || !d->nindices || !nv_init()) return 0;
    switch (d->topology) {
    case D3DPT_TRIANGLELIST: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case D3DPT_LINELIST:     topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;     break;
    case D3DPT_LINESTRIP:    topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;    break;
    case D3DPT_POINTLIST:    topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;    break;
    default: return 0;
    }
    if (points && !gs_init()) return 0;
    if (!g_vsc_cb) g_vsc_cb = make_cb(dev, sizeof g_vsc_last);
    if (!g_vsp_cb) g_vsp_cb = make_cb(dev, sizeof params);
    if (!g_vsc_cb || !g_vsp_cb) return 0;
    vi = vsh_get(d);
    if (vi < 0) return 0;

    /* Streams: attributes interleaved in one array (same stride, within one
     * vertex of each other) share an upload and an input slot. Taken in
     * address order so each group starts at its lowest element. */
    nord = pump_group_on() ? stream_order(d, ord) : stream_order_scan(d, ord);
    for (oi = 0; oi < nord; oi++) {
        a = ord[oi];
        for (g = 0; g < ngrp; g++) {
            if (grp[g].stride == d->attr[a].stride && grp[g].stride &&
                d->attr[a].base >= grp[g].base &&
                (UINT)(d->attr[a].base - grp[g].base) < grp[g].stride)
                break;
            /* ONEMAP: a constant attribute (stride 0) joins a constant group
             * that starts at most ZS_WINDOW bytes before its end, both read
             * straight from guest memory (so the bytes between them are
             * mapped too). Same element, same format, at its offset in the
             * block: the shader reads the same bytes. */
            if (onemap && !grp[g].stride && !d->attr[a].stride &&
                d->attr[a].base >= grp[g].base && d->mem_lo &&
                grp[g].base >= d->mem_lo && d->attr[a].base + d->attr[a].bytes <= d->mem_hi &&
                (UINT)(d->attr[a].base - grp[g].base) + d->attr[a].bytes <= ZS_WINDOW)
                break;
        }
        if (g == ngrp) {
            grp[g].base = d->attr[a].base;
            grp[g].stride = d->attr[a].stride;
            grp[g].size = 0;
            ngrp++;
        }
        {
            UINT off = (UINT)(d->attr[a].base - grp[g].base);
            if (off + d->attr[a].bytes > grp[g].size) grp[g].size = off + d->attr[a].bytes;
            el[nel].SemanticName = "V";
            el[nel].SemanticIndex = (UINT)a;
            el[nel].Format = (DXGI_FORMAT)d->attr[a].dxgi_format;
            el[nel].InputSlot = (UINT)g;
            el[nel].AlignedByteOffset = off;
            el[nel].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            el[nel].InstanceDataStepRate = 0;
            nel++;
        }
    }
    if (g_perf_on) pt = perf_now();
    if (g_perf_detail) perf_add(PZ_DPRE, pt - pd);
    if (onemap) {                       /* every group, then the indices, in one Map */
        UINT rel[16], total = 0, ib_rel, base;
        uint8_t *w;
        for (g = 0; g < ngrp; g++) {
            rel[g] = total;
            total = (total + grp[g].size + 15u) & ~15u;
        }
        ib_rel = total;
        total += d->nindices * 4u;
        w = vi_ring_map(ctx, total, &base);
        if (!w) return 0;
        for (g = 0; g < ngrp; g++) {
            memcpy(w + rel[g], grp[g].base, grp[g].size);
            vbs[g] = g_vi_ring;
            offs[g] = base + rel[g];
            strides[g] = grp[g].stride;
        }
        memcpy(w + ib_rel, d->indices, d->nindices * 4u);
        nv_unmap(ctx, (ID3D11Resource *)g_vi_ring);
        ib_buf = g_vi_ring;
        ib_off = base + ib_rel;
        il = nel ? il_get(vi, el, nel) : NULL;
        if (nel && !il) { g_il_lost++; return 0; }
    } else {
        {
            UINT total = 0;
            for (g = 0; g < ngrp; g++) total += grp[g].size;
            d3d8_UpRingReserve(total, (UINT)ngrp);
        }
        for (g = 0; g < ngrp; g++) {
            offs[g] = d3d8_UpRingUpload(grp[g].base, grp[g].size, &vbs[g]);
            g_nv_maps++;
            if (offs[g] == (UINT)-1 || !vbs[g]) return 0;
            strides[g] = grp[g].stride;
        }
        il = nel ? il_get(vi, el, nel) : NULL;
        if (nel && !il) { g_il_lost++; return 0; }
        ib_off = ib_upload(ctx, d->indices, d->nindices);
        if (ib_off == (UINT)-1) return 0;
        ib_buf = g_ib_ring;
    }
    if (g_perf_on) { double t = perf_now(); perf_add(PZ_DUP, t - pt); pt = t; }

    use_cbr = d3d8_pump_cb_on() && cbr_ready(dev, ctx);
    if (g_perf_on) {                    /* how often the 3 KB compare could be skipped */
        static uint32_t last_gen;
        static int have;
        if (have && g_vsc_valid && d->vgen == last_gen) perf_count(PC_VSC_GEN, 1);
        last_gen = d->vgen;
        have = 1;
    }
    if (g_vsc_valid && d->vdirty && d3d8_pump_cgen_on()) {
        /* Only the constants written with a new value since the last
         * GPU draw can differ from the copy; all the others are unchanged. */
        int w, changed = 0;
        unsigned ncmp = 0;              /* counted once per draw, as the whole-block compare below is */
        for (w = 0; w < (VSHCPU_CONSTANTS + 31) / 32; w++) {
            uint32_t mk = d->vdirty[w];
            while (mk) {
                int c = w * 32 + __builtin_ctz(mk);
                mk &= mk - 1u;
                ncmp++;
                if (memcmp(g_vsc_last[c], d->vconst[c], sizeof g_vsc_last[c])) {
                    memcpy(g_vsc_last[c], d->vconst[c], sizeof g_vsc_last[c]);
                    changed = 1;
                }
            }
            d->vdirty[w] = 0;
        }
        if (g_perf_on) perf_count(PC_VSC_CMP, ncmp);
        if (changed) {
            if (!use_cbr) cb_write(ctx, g_vsc_cb, g_vsc_last, sizeof g_vsc_last);
            vsc_changed = 1;
            if (g_perf_on) perf_count(PC_VSC_CHG, 1);
        } else if (g_perf_on) perf_count(PC_VSC_EQ, 1);
    } else {
        if (g_perf_on && g_vsc_valid) perf_count(PC_VSC_CMP, VSHCPU_CONSTANTS);   /* the whole block (at most) */
        if (!g_vsc_valid || memcmp(g_vsc_last, d->vconst, sizeof g_vsc_last)) {
            memcpy(g_vsc_last, d->vconst, sizeof g_vsc_last);
            if (!use_cbr) cb_write(ctx, g_vsc_cb, g_vsc_last, sizeof g_vsc_last);
            vsc_changed = 1;
            g_vsc_valid = 1;
            if (g_perf_on) perf_count(PC_VSC_CHG, 1);
        } else if (g_perf_on) perf_count(PC_VSC_EQ, 1);
        /* The copy now equals the constants: nothing is new */
        if (d->vdirty) memset(d->vdirty, 0, ((VSHCPU_CONSTANTS + 31) / 32) * sizeof *d->vdirty);
    }
    if (use_cbr != g_cbr_was) {         /* path change: rewrite everything */
        if (!use_cbr) cb_write(ctx, g_vsc_cb, g_vsc_last, sizeof g_vsc_last);
        vsc_changed = 1;
        g_cbr_was = use_cbr;
    }
    memset(&params, 0, sizeof params);
    params.screen[0] = d->screen_w;
    params.screen[1] = d->screen_h;
    params.screen[2] = d->clip_max;
    params.screen[3] = d->point_zoom;
    params.fog[0] = (float)d->fog_mode;
    params.fog[1] = d->fog_p0;
    params.fog[2] = d->fog_p1;
    params.flags[0] = d->specular ? 1.0f : 0.0f;
    params.flags[1] = d->spec_alpha ? 1.0f : 0.0f;
    params.point[0] = d->point_params ? 1.0f : 0.0f;
    params.point[1] = d->point_size;
    params.point[2] = d->point_smooth ? 1.0f : 0.0f;
    params.point[3] = d->point_kx;
    params.hud[0] = d->hud_kx;
    params.hud[1] = d->hud_ax;
    params.hud[2] = d->hud_ky;
    params.hud[3] = d->hud_ay;
    d3d8_temporal_jitter_ndc(&params.jit[0], &params.jit[1]);   /* fork: 0 when off */
    if (d->attr_const) memcpy(params.vattr, d->attr_const, sizeof params.vattr);
    if (!use_cbr) cb_write(ctx, g_vsp_cb, &params, sizeof params);

    if (!g_ps_cb || g_ps_cb_size < d->ps_consts_size) {
        if (g_ps_cb) ID3D11Buffer_Release(g_ps_cb);
        g_ps_cb = make_cb(dev, d->ps_consts_size);
        g_ps_cb_size = g_ps_cb ? d->ps_consts_size : 0;
        if (!g_ps_cb) return 0;
    }
    if (use_cbr) {
        if (!cbr_draw(ctx, g_vsc_last, vsc_changed, sizeof g_vsc_last, &params, sizeof params,
                      d->ps_consts, d->ps_consts_size))
            return 0;
    } else
        cb_write(ctx, g_ps_cb, d->ps_consts, d->ps_consts_size);
    if (g_perf_on) { double t = perf_now(); perf_add(PZ_DCB, t - pt); pt = t; }

    d3d8_states_apply_nv2a();            /* its raster state is the next call's */
    nv_set_raster(ctx, dev, d->raster);
    if (ngrp) ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, (UINT)ngrp, vbs, strides, offs);
    ID3D11DeviceContext_IASetInputLayout(ctx, il);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib_buf, DXGI_FORMAT_R32_UINT, ib_off);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, topo);
    ID3D11DeviceContext_VSSetShader(ctx, g_vsh[vi].vs, NULL, 0);
    if (!use_cbr) {
        cbs[0] = g_vsc_cb;
        cbs[1] = g_vsp_cb;
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 2, cbs);
    }
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    if (!use_cbr) ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_ps_cb);
    if (points) {                       /* point squares */
        int chk = g_pc_ref && pc_init();
        ID3D11DeviceContext_GSSetShader(ctx, chk ? g_gs_so : g_gs, NULL, 0);
        if (use_cbr)                    /* Params: the same block as the VS */
            ID3D11DeviceContext1_GSSetConstantBuffers1(g_ctx1, 1, 1, &g_cbr, &g_cbr_par_first, &g_cbr_par_num);
        else
            ID3D11DeviceContext_GSSetConstantBuffers(ctx, 1, 1, &g_vsp_cb);
        if (g_perf_on) { double t = perf_now(); perf_add(PZ_DSTATE, t - pt); pt = t; }
        if (chk) {
            UINT zero = 0;
            ID3D11DeviceContext_SOSetTargets(ctx, 1, &g_so_buf, &zero);
            ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)g_so_q);
        }
        if (g_gpuprof_on) gpuprof_draw_begin();
        lr = !chk && g_d3d8_light_mark ? d3d8_lights_pre(ctx) : 0;     /* fork: the lights */
        ID3D11DeviceContext_DrawIndexed(ctx, d->nindices, 0, 0);
        if (g_gpuprof_on) gpuprof_draw_end();
        if (lr == 2) {
            if (d3d8_lights_z_begin(ctx)) { ID3D11DeviceContext_DrawIndexed(ctx, d->nindices, 0, 0); d3d8_lights_z_end(ctx); }
            d3d8_lights_mirror_end(ctx);
        } else if (lr == 1 && d3d8_lights_mirror_begin(ctx)) {
            ID3D11DeviceContext_DrawIndexed(ctx, d->nindices, 0, 0);
            if (d3d8_lights_z_begin(ctx)) { ID3D11DeviceContext_DrawIndexed(ctx, d->nindices, 0, 0); d3d8_lights_z_end(ctx); }
            d3d8_lights_mirror_end(ctx);
        }
        if (!chk && g_d3d8_temporal_mark && lr != 2 && d3d8_temporal_mirror_begin(ctx)) {   /* fork: temporal */
            ID3D11DeviceContext_DrawIndexed(ctx, d->nindices, 0, 0);
            d3d8_temporal_mirror_end(ctx);
        }
        if (chk) {
            ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)g_so_q);
            ID3D11DeviceContext_SOSetTargets(ctx, 0, NULL, NULL);
            pc_compare(ctx, d->screen_w, d->screen_h);
        }
        g_pc_ref = NULL;
        /* Every other draw of the device runs without a geometry shader. */
        ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
        if (g_perf_on) { perf_add(PZ_DDRAW, perf_now() - pt); perf_gpu_draw(g_nv_maps - maps0, (unsigned)ngrp); }
        return 1;
    }
    if (g_perf_on) { double t = perf_now(); perf_add(PZ_DSTATE, t - pt); pt = t; }
    if (g_perf_detail) {    /* same bound state as the previous such draw (a merge could join them);
                             * ~8 KB copied and compared per draw: XBOX_PERF_DETAIL only */
        static struct {
            DWORD rs[256], tss[4][32];
            void *tex[4];
            int vi, nel, raster, topo;
            ID3D11PixelShader *ps;
            ID3D11InputLayout *il;
            D3D11_INPUT_ELEMENT_DESC el[16];
            UINT strides[16];
            uint32_t vgen;
            float par[88];
            uint8_t psc[512];
        } sg[2];
        static int cur, have;
        int k, n = cur ^ 1;
        const DWORD *rs = d3d8_GetRenderStates();
        memset(&sg[n], 0, sizeof sg[n]);
        if (rs) memcpy(sg[n].rs, rs, sizeof sg[n].rs);
        for (k = 0; k < 4; k++) {
            const DWORD *t = d3d8_GetTSS((DWORD)k);
            if (t) memcpy(sg[n].tss[k], t, sizeof sg[n].tss[k]);
            sg[n].tex[k] = d3d8_CurTexture(k);
        }
        sg[n].vi = vi; sg[n].nel = nel; sg[n].raster = d->raster; sg[n].topo = (int)topo;
        sg[n].ps = ps; sg[n].il = il;
        memcpy(sg[n].el, el, (size_t)nel * sizeof el[0]);
        for (k = 0; k < ngrp; k++) sg[n].strides[k] = strides[k];
        sg[n].vgen = d->vgen;
        memcpy(sg[n].par, &params, sizeof params < sizeof sg[n].par ? sizeof params : sizeof sg[n].par);
        if (d->ps_consts_size <= sizeof sg[n].psc) memcpy(sg[n].psc, d->ps_consts, d->ps_consts_size);
        if (have && !memcmp(&sg[0], &sg[1], sizeof sg[0]) && !(d->raster & 0xC0) &&
            !g_d3d8_light_mark && !g_d3d8_temporal_mark)
            perf_count(PC_MERGES, 1);
        cur = n;
        have = 1;
    }
    {
        int gm = d3d8_ghost_mode();
        if (gm == 1) return 1;
        if (g_gpuprof_on) gpuprof_draw_begin();
        if (gm == 2) {
            d3d8_ghost_pass(ctx, 0);
            ID3D11DeviceContext_DrawIndexed(ctx, d->nindices, 0, 0);
            d3d8_ghost_pass(ctx, 1);
            ID3D11DeviceContext_DrawIndexed(ctx, d->nindices, 0, 0);
            d3d8_ghost_pass(ctx, 2);
        } else {
            lr = g_d3d8_light_mark ? d3d8_lights_pre(ctx) : 0;     /* fork: the lights (d3d8_lights.h) */
            if (g_d3d8_zcost) {
                LARGE_INTEGER q0, q1;
                int split = (d->raster & 0xC0) && ((d->raster & 0x80) ? d->zdecal_nsub : d->ztie_nsub);
                QueryPerformanceCounter(&q0);
                zt_draw_indexed(ctx, dev, d->raster, d->nindices, (d->raster & 0x80) ? d->zdecal_nsub : d->ztie_nsub);
                QueryPerformanceCounter(&q1);
                g_d3d8_zc[split ? 2 : 0] += (unsigned long long)(q1.QuadPart - q0.QuadPart);
                g_d3d8_zc[split ? 3 : 1]++;
            } else
            zt_draw_indexed(ctx, dev, d->raster, d->nindices, (d->raster & 0x80) ? d->zdecal_nsub : d->ztie_nsub);   /* stable ties (XBOX_ZTIE) / decals */
            if (lr == 2) {
                if (d3d8_lights_z_begin(ctx)) { mirror_draw_indexed(ctx, dev, d); d3d8_lights_z_end(ctx); }
                d3d8_lights_mirror_end(ctx);
            } else if (lr == 1 && d3d8_lights_mirror_begin(ctx)) {
                mirror_draw_indexed(ctx, dev, d);
                if (d3d8_lights_z_begin(ctx)) { mirror_draw_indexed(ctx, dev, d); d3d8_lights_z_end(ctx); }
                d3d8_lights_mirror_end(ctx);
            }
            if (g_d3d8_temporal_mark && lr != 2 && d3d8_temporal_mirror_begin(ctx)) {   /* fork: temporal */
                mirror_draw_indexed(ctx, dev, d);
                d3d8_temporal_mirror_end(ctx);
            }
        }
        if (g_gpuprof_on) gpuprof_draw_end();
    }
    if (g_perf_on) { perf_add(PZ_DDRAW, perf_now() - pt); perf_gpu_draw(g_nv_maps - maps0, (unsigned)ngrp); }
    return 1;
}
