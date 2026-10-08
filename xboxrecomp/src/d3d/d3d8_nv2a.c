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
#include "d3d8_internal.h"
#include "d3d8_gpuprof.h"
#include <d3dcompiler.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

UINT d3d8_UpRingUpload(const void *data, UINT size, ID3D11Buffer **buf);
void d3d8_UpRingReserve(UINT total, UINT pieces);

/* Vertex layout written by nv2a_pgraph_d3d11.c (ProgVertex): screen x, y,
 * z (0..1) and rhw; oD0; oD1; the fog factor; oT0..oT3. */
static const char g_vs_src[] =
    "cbuffer NvVS : register(b0) { float4 screen; };\n"
    "struct VSIn {\n"
    "    float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n"
    "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
    "};\n"
    "struct VSOut {\n"
    "    float4 pos : SV_POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n"
    "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
    "};\n"
    "VSOut main(VSIn i) {\n"
    "    VSOut o;\n"
    /* Undo the divide by w so the rasteriser interpolates perspective-correctly. */
    /* rhw is never 0 and keeps w's sign (nv2a_pgraph_d3d11.c clamps w away
     * from zero), so a vertex behind the eye gets a negative clip w and the
     * rasteriser clips its triangles at the near plane, as xemu relies on. */
    "    float w = 1.0 / i.pos.w;\n"
    "    o.pos = float4((i.pos.x / screen.x * 2.0 - 1.0) * w,\n"
    "                   (1.0 - i.pos.y / screen.y * 2.0) * w, i.pos.z * w, w);\n"
    "    o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog;\n"
    "    o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
    "    return o;\n"
    "}\n";

static ID3D11VertexShader *g_vs;
static ID3D11InputLayout  *g_layout;
static ID3D11Buffer       *g_vs_cb, *g_ps_cb;
static UINT                g_ps_cb_size;
static int                 g_init_failed;

#define NV_PS_CACHE 2048
static struct { uint64_t key; ID3D11PixelShader *ps; } g_ps_cache[NV_PS_CACHE];
static int g_ps_count;

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
    hr = D3DCompile(g_vs_src, strlen(g_vs_src), "vs_nv2a", NULL, NULL, "main", "vs_5_0",
                    0, 0, &blob, &err);
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
static int pump_alt_off(void)
{
    static int alt = -1;
    if (alt < 0) { const char *e = getenv("XBOX_FIX_PUMP_ALT"); alt = e && e[0] == '1'; }
    return alt && (g_pump_frame & 1u);
}
void d3d8_pump_frame_tick(void) { g_pump_frame++; }
int  d3d8_pump_alt_off(void) { return pump_alt_off(); }   /* also used by the post */

int d3d8_pump_state_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_PUMP_STATE"); on = !(e && e[0] == '0');   /* default 1 (proven identical) */ }
    return on && !pump_alt_off();
}

int d3d8_pump_cb_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_PUMP_CB"); on = !(e && e[0] == '0');   /* default 1 (proven identical) */ }
    return on && !pump_alt_off();
}

/* XBOX_FIX_PUMP_ONEMAP (default 1; =0 previous path): one Map per GPU draw for its vertices and indices.
 * Constant attributes (stride 0) that sit close together in guest memory are
 * uploaded as one block through one input slot; vertex groups and indices go
 * to one ring bound as both vertex and index buffer, written with a single
 * Map; the input layout and vertex shader lookups first compare with the
 * previous draw. Same bytes and formats reach the shaders.
 * XBOX_FIX_PUMP_ONEMAP_ALT=1 (identity test): active on one presented frame
 * in two only. XBOX_FIX_PUMP_ALT turns it off with the other optimizations. */
int d3d8_pump_onemap_on(void)
{
    static int on = -1, alt = -1;
    if (on < 0) {
        const char *e = getenv("XBOX_FIX_PUMP_ONEMAP");
        on = !(e && e[0] == '0');       /* default 1 (proven identical); 0 = previous path */
        e = getenv("XBOX_FIX_PUMP_ONEMAP_ALT");
        alt = e && e[0] == '1';
        fprintf(stderr, "[PUMP] XBOX_FIX_PUMP_ONEMAP=%d%s\n", on, alt ? " (ALT: one frame in two)" : "");
    }
    return on && !(alt && (g_pump_frame & 1u)) && !pump_alt_off();
}

int d3d8_pump_cache_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_PUMP_CACHE"); on = !(e && e[0] == '0');   /* default 1 (proven identical) */ }
    return on && !pump_alt_off();
}

static KIdx g_ps_idx[NV_PS_CACHE * 2];

static ID3D11PixelShader *ps_find(unsigned long long key)
{
    int i;
    if (d3d8_pump_cache_on()) {
        i = kidx_find(g_ps_idx, NV_PS_CACHE * 2, key);
        return i >= 0 ? g_ps_cache[i].ps : NULL;
    }
    for (i = 0; i < g_ps_count; i++)
        if (g_ps_cache[i].key == key)
            return g_ps_cache[i].ps;
    return NULL;
}

int d3d8_nv2a_has_ps(unsigned long long key)
{
    return ps_find(key) != NULL;
}

int d3d8_nv2a_add_ps(unsigned long long key, const char *hlsl, int len)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr;
    static int failures;

    if (!dev || g_ps_count >= NV_PS_CACHE) return 0;
    {
        double t0 = nv_now_ms();
        hr = D3DCompile(hlsl, (SIZE_T)len, "ps_nv2a", NULL, NULL, "main", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
        g_nv_compiles++;
        if (g_perf_on) perf_count(PC_COMPILE, 1);
        g_nv_compile_ms += nv_now_ms() - t0;
    }
    if (FAILED(hr)) {
        if (failures++ < 4)
            fprintf(stderr, "[NV2A-PSH] pixel shader %016llX failed: %s\n--- source ---\n%s\n",
                    (unsigned long long)key,
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", hlsl);
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(blob),
                                        ID3D10Blob_GetBufferSize(blob), NULL, &ps);
    ID3D10Blob_Release(blob);
    if (FAILED(hr) || !ps) return 0;
    g_ps_cache[g_ps_count].key = key;
    g_ps_cache[g_ps_count].ps = ps;
    kidx_put(g_ps_idx, NV_PS_CACHE * 2, key, g_ps_count);
    g_ps_count++;
    return 1;
}

/* Buffer Maps of this file, counted for XBOX_PERF (Maps per GPU draw) and
 * timed under XBOX_PERF_MAPTIME. Same calls as before otherwise. */
static unsigned g_nv_maps;

static HRESULT nv_map(ID3D11DeviceContext *ctx, ID3D11Resource *r, D3D11_MAP how, D3D11_MAPPED_SUBRESOURCE *m)
{
    unsigned long long t;
    HRESULT hr;
    g_nv_maps++;
    if (!g_perf_maptime) return ID3D11DeviceContext_Map(ctx, r, 0, how, 0, m);
    t = __rdtsc();
    hr = ID3D11DeviceContext_Map(ctx, r, 0, how, 0, m);
    g_perf_map_cyc += __rdtsc() - t;
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

static int cbr_draw(ID3D11DeviceContext *ctx, const void *vsc, int vsc_changed, UINT vsc_size,
                    const void *par, UINT par_size, const void *psc, UINT psc_size)
{
    D3D11_MAPPED_SUBRESOURCE m;
    UINT need, a_vsc = CBR_ALIGN(vsc_size), a_par = CBR_ALIGN(par_size), a_psc = CBR_ALIGN(psc_size);
    UINT o_par, o_psc, first[2], num[2];
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    ID3D11Buffer *bufs[2];
    int write_vsc = vsc_changed || g_cbr_vsc_gen != g_cbr_gen;
    need = (write_vsc ? a_vsc : 0) + a_par + a_psc;
    if (g_cbr_off + need > CBR_SIZE) {
        how = D3D11_MAP_WRITE_DISCARD;
        g_cbr_off = 0;
        g_cbr_gen++;
        write_vsc = 1;
        need = a_vsc + a_par + a_psc;
    }
    if (FAILED(nv_map(ctx, (ID3D11Resource *)g_cbr, how, &m))) return 0;
    if (write_vsc) {
        memcpy((uint8_t *)m.pData + g_cbr_off, vsc, vsc_size);
        g_cbr_vsc_off = g_cbr_off;
        g_cbr_vsc_gen = g_cbr_gen;
        g_cbr_off += a_vsc;
    }
    o_par = g_cbr_off; memcpy((uint8_t *)m.pData + o_par, par, par_size); g_cbr_off += a_par;
    o_psc = g_cbr_off; memcpy((uint8_t *)m.pData + o_psc, psc, psc_size); g_cbr_off += a_psc;
    nv_unmap(ctx, (ID3D11Resource *)g_cbr);
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

HRESULT d3d8_nv2a_draw(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                       unsigned long long ps_key, const void *ps_consts, UINT ps_consts_size,
                       int raster)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
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
    if (g_gpuprof_on) gpuprof_draw_begin();
    ID3D11DeviceContext_Draw(ctx, nv, 0);
    if (g_gpuprof_on) gpuprof_draw_end();
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

/* The shader for this program and these input kinds, compiled on first use.
 * A program that fails to compile is remembered, so it is not retried. */
static int vsh_lookup(const Nv2aVshDraw *d, const uint8_t kind[16]);
static struct { uint64_t hash; uint8_t kind[16]; int pts, vi; } g_vsh_last = { 0, {0}, 0, -1 };

static int vsh_get(const Nv2aVshDraw *d)
{
    uint8_t kind[16];
    int a, i;

    for (a = 0; a < 16; a++) kind[a] = (d->inputs & (1u << a)) ? d->attr[a].kind : 0xFF;
    /* Same program and input kinds as the previous lookup: same entry (each
     * draw asks twice, and consecutive draws often share the program). */
    if (d3d8_pump_onemap_on() && g_vsh_last.vi >= 0 && g_vsh_last.hash == d->prog_hash &&
        g_vsh_last.pts == (d->topology == D3DPT_POINTLIST) && !memcmp(g_vsh_last.kind, kind, sizeof kind))
        return g_vsh_last.vi;
    i = vsh_lookup(d, kind);
    g_vsh_last.vi = i;
    g_vsh_last.hash = d->prog_hash;
    g_vsh_last.pts = d->topology == D3DPT_POINTLIST;
    memcpy(g_vsh_last.kind, kind, sizeof kind);
    return i;
}

static int vsh_lookup(const Nv2aVshDraw *d, const uint8_t kind[16])
{
    static char src[262144];
    ID3D11Device *dev = d3d8_GetD3D11Device();
    uint64_t key = d->prog_hash;
    ID3D10Blob *code = NULL, *err = NULL;
    HRESULT hr;
    int i, len;

    key = fnv64(key, kind, 16);
    if (d->topology == D3DPT_POINTLIST) key = fnv64(key, "pts", 3);
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
    len = nv2a_vsh_hlsl(d->prog, d->prog_len, d->inputs, kind, d->topology == D3DPT_POINTLIST,
                        src, (int)sizeof src);
    if (len <= 0) return -1;
    /* IEEE strictness keeps the isnan/isfinite tests the CPU path relies on. */
    {
        double t0 = nv_now_ms();
        hr = D3DCompile(src, (SIZE_T)len, "vs_nv2a_prog", NULL, NULL, "main", "vs_5_0",
                        D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &err);
        g_nv_compiles++;
        if (g_perf_on) perf_count(PC_COMPILE, 1);
        g_nv_compile_ms += nv_now_ms() - t0;
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
    {
        static int count;
        if (++count <= 64 || (count % 64) == 0)
            fprintf(stderr, "[NV2A-VSH] compiled vertex program %d (%d instructions)\n",
                    count, d->prog_len);
    }
    return i;
}

static ID3D11InputLayout *il_lookup(int vi, const D3D11_INPUT_ELEMENT_DESC *el, int n);
static struct { int vi, n; D3D11_INPUT_ELEMENT_DESC el[16]; ID3D11InputLayout *il; } g_il_last;

/* The input layout for this shader and these elements; the same elements
 * as the previous draw give the same layout without hashing them again. */
static ID3D11InputLayout *il_get(int vi, const D3D11_INPUT_ELEMENT_DESC *el, int n)
{
    ID3D11InputLayout *il;
    if (d3d8_pump_onemap_on() && g_il_last.il && g_il_last.vi == vi && g_il_last.n == n &&
        !memcmp(g_il_last.el, el, (size_t)n * sizeof *el))
        return g_il_last.il;
    il = il_lookup(vi, el, n);
    g_il_last.il = il;
    g_il_last.vi = vi;
    g_il_last.n = n;
    memcpy(g_il_last.el, el, (size_t)n * sizeof *el);
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
    if (g_nil >= IL_CACHE || !dev) return NULL;
    i = g_nil++;
    g_il[i].key = key;
    kidx_put(g_il_idx, IL_CACHE * 2, key, i);
    g_il[i].il = NULL;
    ID3D11Device_CreateInputLayout(dev, el, (UINT)n,
                                   ID3D10Blob_GetBufferPointer(g_vsh[vi].code),
                                   ID3D10Blob_GetBufferSize(g_vsh[vi].code), &g_il[i].il);
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
 * whole size is known -- a wrap (DISCARD) can never fall between two uploads
 * of one draw. NO_OVERWRITE otherwise, as the other rings. */
#define VI_RING_SIZE (48u * 1024u * 1024u)
static ID3D11Buffer *g_vi_ring;
static UINT g_vi_off;

static uint8_t *vi_ring_map(ID3D11DeviceContext *ctx, UINT size, UINT *off)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (!g_vi_ring) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = VI_RING_SIZE;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &bd, NULL, &g_vi_ring)))
            return NULL;
        g_vi_off = VI_RING_SIZE;        /* first Map = DISCARD */
    }
    if (size > VI_RING_SIZE) return NULL;
    if (g_vi_off + size > VI_RING_SIZE) { g_vi_off = 0; how = D3D11_MAP_WRITE_DISCARD; }
    if (FAILED(nv_map(ctx, (ID3D11Resource *)g_vi_ring, how, &m)))
        return NULL;
    *off = g_vi_off;
    g_vi_off = (g_vi_off + size + 15u) & ~15u;
    return (uint8_t *)m.pData + *off;
}

/* Point sprites on the GPU. The CPU path turned each point into
 * a square of its size around the snapped centre, in title pixels -- corners
 * (-h,-h) (+h,-h) (-h,+h) (+h,+h), triangles 0 1 2 and 2 1 3 -- with the
 * sprite coordinate in t3 under SET_POINT_SMOOTH_ENABLE, and dropped a point
 * whose vertex could not be placed. This geometry shader does the same with
 * the same arithmetic: a strip of those four corners rasterises as those two
 * triangles. */
static const char g_gs_src[] =
    "cbuffer Params : register(b1) {\n"
    "    float4 screen; float4 fogp; float4 flags; float4 vattr[16]; float4 ptparm;\n"
    "};\n"
    "struct VSOut {\n"
    "    float4 pos : SV_POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n"
    "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
    "    float4 pt : PTS;\n"
    "};\n"
    "struct GSOut {\n"
    "    float4 pos : SV_POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n"
    "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
    "};\n"
    "[maxvertexcount(4)]\n"
    "void main(point VSOut p[1], inout TriangleStream<GSOut> s) {\n"
    "    VSOut v = p[0];\n"
    "    if (isnan(v.pos.w)) return;\n"
    "    float h = v.pt.z, wc = v.pos.w;\n"
    "    for (int c = 0; c < 4; c++) {\n"
    "        GSOut o;\n"
    "        float x = v.pt.x + ((c & 1) ? h : -h) * ptparm.w, y = v.pt.y + ((c & 2) ? h : -h);\n"
    "        o.pos = float4((x / screen.x * 2.0 - 1.0) * wc, (1.0 - y / screen.y * 2.0) * wc, v.pos.z, wc);\n"
    "        o.d0 = v.d0; o.d1 = v.d1; o.fog = v.fog;\n"
    "        o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2;\n"
    "        o.t3 = ptparm.z != 0 ? float4((c & 1) ? 1 : 0, (c & 2) ? 1 : 0, 1, 1) : v.t3;\n"
    "        s.Append(o);\n"
    "    }\n"
    "}\n";
static ID3D11GeometryShader *g_gs;
static int g_gs_failed;

static int gs_init(void)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr;
    if (g_gs) return 1;
    if (g_gs_failed || !dev) return 0;
    hr = D3DCompile(g_gs_src, strlen(g_gs_src), "gs_nv2a_points", NULL, NULL, "main", "gs_5_0",
                    D3DCOMPILE_IEEE_STRICTNESS, 0, &blob, &err);
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
        HRESULT hr = D3DCompile(g_gs_src, strlen(g_gs_src), "gs_nv2a_points", NULL, NULL, "main", "gs_5_0",
                                D3DCOMPILE_IEEE_STRICTNESS, 0, &blob, &err);
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

int d3d8_nv2a_draw_program_gpu(const Nv2aVshDraw *d)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3D11PixelShader *ps = ps_find(d->ps_key);
    D3D11_INPUT_ELEMENT_DESC el[16];
    struct { const uint8_t *base; UINT stride, size; } grp[16];
    ID3D11Buffer *vbs[16], *cbs[2], *ib_buf;
    UINT strides[16], offs[16], ib_off;
    ID3D11InputLayout *il;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    int vi, a, g, ngrp = 0, nel = 0;
    uint16_t done = 0;
    struct { float screen[4], fog[4], flags[4], vattr[16][4], point[4], hud[4]; } params;
    double pt = 0.0;                    /* XBOX_PERF sub-zones */
    int use_cbr, vsc_changed = 0;       /* constant ring */
    static int g_cbr_was;
    int points = d->topology == D3DPT_POINTLIST;
    unsigned maps0 = g_nv_maps;         /* XBOX_PERF: Maps of this draw */
    int onemap = d3d8_pump_onemap_on();

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
    for (;;) {
        int best = -1;
        for (a = 0; a < 16; a++)
            if ((d->inputs & (1u << a)) && !(done & (1u << a)) &&
                (d->attr[a].kind & 0x0F) != NV2A_VSH_IN_CONST &&
                (best < 0 || d->attr[a].base < d->attr[best].base))
                best = a;
        if (best < 0) break;
        done |= (uint16_t)(1u << best);
        a = best;
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
        if (nel && !il) return 0;
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
        if (nel && !il) return 0;
        ib_off = ib_upload(ctx, d->indices, d->nindices);
        if (ib_off == (UINT)-1) return 0;
        ib_buf = g_ib_ring;
    }
    if (g_perf_on) { double t = perf_now(); perf_add(PZ_DUP, t - pt); pt = t; }

    use_cbr = d3d8_pump_cb_on() && cbr_ready(dev, ctx);
    if (!g_vsc_valid || memcmp(g_vsc_last, d->vconst, sizeof g_vsc_last)) {
        memcpy(g_vsc_last, d->vconst, sizeof g_vsc_last);
        if (!use_cbr) cb_write(ctx, g_vsc_cb, g_vsc_last, sizeof g_vsc_last);
        vsc_changed = 1;
        g_vsc_valid = 1;
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

    d3d8_states_apply();
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
        ID3D11DeviceContext_DrawIndexed(ctx, d->nindices, 0, 0);
        if (g_gpuprof_on) gpuprof_draw_end();
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
    if (g_gpuprof_on) gpuprof_draw_begin();
    ID3D11DeviceContext_DrawIndexed(ctx, d->nindices, 0, 0);
    if (g_gpuprof_on) gpuprof_draw_end();
    if (g_perf_on) { perf_add(PZ_DDRAW, perf_now() - pt); perf_gpu_draw(g_nv_maps - maps0, (unsigned)ngrp); }
    return 1;
}
