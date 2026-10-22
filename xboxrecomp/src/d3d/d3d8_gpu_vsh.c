/* d3d8_gpu_vsh.c -- NV2A vertex programs as Metal vertex shaders (the macOS GPU backend).
 *
 * The same translation as d3d8_nv2a_vsh.c (HLSL, the Windows layer), instruction for instruction (sources read before
 * either unit writes, a paired ILU writing R1 and the MAC's R1 write dropped, R12 aliasing oPos, 0 * anything = 0 in
 * MUL/MAD, ARL's biased floor, constant reads outside 0..191 giving zero), followed by what vsh_vertex() does to every
 * vertex (1/16-pixel snapping, z over CLIP_MAX, w kept away from zero with its sign, NaN colours to 1, specular and fog
 * rules). A vertex the CPU path would reject is sent to NaN: the rasteriser drops it with its triangle.
 *
 * Differences from the HLSL generator: the shader fetches its attributes itself from one storage buffer holding the raw
 * vertex bytes (a small descriptor per attribute: offset, stride, format), so one shader serves every vertex layout and
 * no input layouts are needed; the draw is indexed, vertex_id being the (rebased) index. Points are not handled (the
 * translator keeps them on the CPU).
 *
 * Buffers: uniform 0 = constants float4 c[192]; uniform 1 = Params (below); storage buffer 0 = the raw vertex bytes
 * (MSL buffer index = number of uniform buffers + 0 = 2).
 */
#include "../nv2a/nv2a_vsh_cpu.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MUX_NONE, MUX_R, MUX_V, MUX_C };

typedef struct { char *p, *end; int overflow; } Sb;

static void sb(Sb *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (b->overflow) return;
    va_start(ap, fmt);
    n = vsnprintf(b->p, (size_t)(b->end - b->p), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= b->end - b->p) { b->overflow = 1; return; }
    b->p += n;
}

static const char *treg(int n)
{
    static const char *names[16] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
                                     "r8", "r9", "r10", "r11", "O0", "r13", "r14", "r15" };
    return names[n & 15];
}

static void mask_str(unsigned mask, char *out)
{
    int k = 0;
    out[k++] = '.';
    if (mask & 8) out[k++] = 'x';
    if (mask & 4) out[k++] = 'y';
    if (mask & 2) out[k++] = 'z';
    if (mask & 1) out[k++] = 'w';
    out[k] = 0;
}

static void src_expr(const vshcpu_insn *i, int k, char *out, size_t n)
{
    static const char comp[4] = { 'x', 'y', 'z', 'w' };
    char base[48], sw[6];
    int j;
    switch (i->src[k].mux) {
    case MUX_R: snprintf(base, sizeof base, "%s", treg(i->src[k].reg)); break;
    case MUX_V: snprintf(base, sizeof base, "v%u", i->vidx & 15); break;
    case MUX_C:
        if (i->a0x) snprintf(base, sizeof base, "cread(c, %u + a0)", i->cidx);
        else        snprintf(base, sizeof base, "cread(c, %u)", i->cidx);
        break;
    default: snprintf(base, sizeof base, "float4(0.0)"); break;
    }
    sw[0] = '.';
    for (j = 0; j < 4; j++) sw[1 + j] = comp[i->src[k].swz[j] & 3];
    sw[5] = 0;
    snprintf(out, n, "%s(%s)%s", i->src[k].neg ? "-" : "", base, sw);
}

/* IEEE semantics matter here (0 * inf, 1 / 0, NaN tests): safe math where the compiler has the pragma. */
static const char g_prelude[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Attr { uint off; uint stride; uint fmt; uint size; };   /* fmt: 0 f32, 1 ub (BGRA), 2 ub (RGBA), 3 snorm16, 4 sint16, 5 cmp, 6 constant; +0x100: w = 1 */\n"
    "struct Params {\n"
    "    float4 screen;      /* width, height, CLIP_MAX, oPts scale */\n"
    "    float4 fogp;        /* mode (0 off, 1 linear, 2 linear abs, 3 exp, 4 exp abs, 5 exp2), p0, p1 */\n"
    "    float4 flags;       /* specular enable, alpha from specular */\n"
    "    float4 vattr[16];   /* attributes with no array */\n"
    "    float4 ptparm;\n"
    "    float4 hud;         /* HUD element: x scale, x anchor, y scale, y anchor; x scale < 0: a panel's ends, z w = its edges */\n"
    "    Attr at[16];\n"
    "};\n"
    "struct VSOut { float4 pos [[position]]; float4 d0; float4 d1; float fog; float4 t0; float4 t1; float4 t2; float4 t3; };\n"
    "static bool nv_isnan(float x) { return (as_type<uint>(x) & 0x7FFFFFFFu) > 0x7F800000u; }\n"
    "static bool nv_isinf(float x) { return (as_type<uint>(x) & 0x7FFFFFFFu) == 0x7F800000u; }\n"
    "static bool nv_isfinite(float x) { return (as_type<uint>(x) & 0x7F800000u) != 0x7F800000u; }\n"
    "static float4 cread(constant float4 *c, int n) { return (n >= 0 && n < 192) ? c[n] : float4(0.0); }\n"
    "static float4 mul0(float4 a, float4 b) {\n"
    "    float4 p = a * b;\n"
    "    return float4((a.x == 0.0 || b.x == 0.0) ? 0.0 : p.x, (a.y == 0.0 || b.y == 0.0) ? 0.0 : p.y,\n"
    "                  (a.z == 0.0 || b.z == 0.0) ? 0.0 : p.z, (a.w == 0.0 || b.w == 0.0) ? 0.0 : p.w);\n"
    "}\n"
    "static float clamp_away(float t) {\n"
    "    if (t > 0.0 || (t == 0.0 && as_type<uint>(t) == 0u)) return clamp(t, 5.421011e-20, 1.8446744e+19);\n"
    "    return clamp(t, -1.8446744e+19, -5.421011e-20);\n"
    "}\n"
    "static float fogf(constant Params &P, float d) {\n"
    "    float f, m = P.fogp.x;\n"
    "    if (m == 0.0) return 1.0;\n"
    "    if (m == 2.0 || m == 4.0) d = abs(d);\n"
    "    if ((m == 1.0 || m == 3.0) && nv_isinf(d)) d = 0.0;\n"
    "    if (m <= 2.0) f = P.fogp.y + d * P.fogp.z - 1.0;\n"
    "    else if (m <= 4.0) f = P.fogp.y + exp2(d * P.fogp.z * 16.0) - 1.5;\n"
    "    else f = P.fogp.y + exp2(-d * d * P.fogp.z * P.fogp.z * 32.0) - 1.5;\n"
    "    if (nv_isnan(f)) return 1.0;\n"
    "    return saturate(f);\n"
    "}\n"
    "static float4 nan1(float4 v) { return float4(nv_isnan(v.x) ? 1.0 : v.x, nv_isnan(v.y) ? 1.0 : v.y,\n"
    "                                             nv_isnan(v.z) ? 1.0 : v.z, nv_isnan(v.w) ? 1.0 : v.w); }\n"
    /* one attribute of vertex `vid`, as the vertex program reads it: absent components (0, 0, 0, 1) */
    "static float4 fetch(device const uchar *raw, constant Attr &a, uint vid, float4 cv) {\n"
    "    uint f = a.fmt & 0xFFu, p = a.off + vid * a.stride, k;\n"
    "    float4 v = float4(0.0, 0.0, 0.0, 1.0);\n"
    "    if (f == 6u) return cv;\n"
    "    if (f == 0u) { for (k = 0; k < a.size && k < 4u; k++) v[k] = *(device const float *)(raw + p + 4u * k); }\n"
    "    else if (f == 1u || f == 2u) {\n"
    "        for (k = 0; k < a.size && k < 4u; k++) v[k] = float(raw[p + k]) / 255.0;\n"
    "        if (f == 1u && a.size >= 3u) { float t = v.x; v.x = v.z; v.z = t; }\n"
    "    }\n"
    "    else if (f == 3u) { for (k = 0; k < a.size && k < 4u; k++) v[k] = max(float(*(device const short *)(raw + p + 2u * k)) / 32767.0, -1.0); }\n"
    "    else if (f == 4u) { for (k = 0; k < a.size && k < 4u; k++) v[k] = float(*(device const short *)(raw + p + 2u * k)); }\n"
    "    else if (f == 5u) {\n"
    "        uint u = *(device const uint *)(raw + p);\n"
    "        v = float4(float(as_type<int>(u << 21) >> 21) / 1023.0, float(as_type<int>(u << 10) >> 21) / 1023.0, float(as_type<int>(u) >> 22) / 511.0, 1.0);\n"
    "    }\n"
    "    if (a.fmt & 0x100u) v.w = 1.0;\n"
    "    return v;\n"
    "}\n";

int nv2a_vsh_msl(const vshcpu_insn *p, int n, unsigned short inputs, char *buf, int cap)
{
    Sb b = { buf, buf + cap, 0 };
    int k, a;

    sb(&b, "#pragma METAL fp math_mode(safe)\n");
    sb(&b, "%s", g_prelude);
    sb(&b, "vertex VSOut vs_prog(uint vid [[vertex_id]], constant float4 *c [[buffer(0)]], constant Params &P [[buffer(1)]],\n"
           "                     device const uchar *raw [[buffer(2)]]) {\n");
    for (a = 0; a < 16; a++) {
        if (!(inputs & (1u << a))) sb(&b, "    float4 v%d = float4(0.0, 0.0, 0.0, 1.0);\n", a);
        else sb(&b, "    float4 v%d = fetch(raw, P.at[%d], vid, P.vattr[%d]);\n", a, a, a);
    }
    sb(&b, "    float4 r0 = float4(0.0), r1 = float4(0.0), r2 = float4(0.0), r3 = float4(0.0), r4 = float4(0.0), r5 = float4(0.0),\n"
           "           r6 = float4(0.0), r7 = float4(0.0), r8 = float4(0.0), r9 = float4(0.0), r10 = float4(0.0), r11 = float4(0.0),\n"
           "           r13 = float4(0.0), r14 = float4(0.0), r15 = float4(0.0);\n"
           "    float4 O0 = float4(0.0, 0.0, 0.0, 1.0), O1 = O0, O2 = O0, O3 = O0, O4 = O0, O5 = O0, O6 = O0,\n"
           "           O7 = O0, O8 = O0, O9 = O0, O10 = O0, O11 = O0, O12 = O0, O13 = O0, O14 = O0, O15 = O0;\n"
           "    float4 A, B, C, m, u;\n"
           "    int a0 = 0, na0;\n");

    for (k = 0; k < n; k++) {
        const vshcpu_insn *i = &p[k];
        int paired = i->mac != MAC_NOP && i->ilu != ILU_NOP;
        char e[96], mk[6];
        if (i->mac == MAC_NOP && i->ilu == ILU_NOP) {
            if (i->final) break;
            continue;
        }
        sb(&b, "    /* %d */ na0 = a0;\n", k);
        src_expr(i, 0, e, sizeof e); sb(&b, "    A = %s;\n", e);
        src_expr(i, 1, e, sizeof e); sb(&b, "    B = %s;\n", e);
        src_expr(i, 2, e, sizeof e); sb(&b, "    C = %s;\n", e);
        switch (i->mac) {
        case MAC_MOV: sb(&b, "    m = A;\n"); break;
        case MAC_MUL: sb(&b, "    m = mul0(A, B);\n"); break;
        case MAC_ADD: sb(&b, "    m = A + C;\n"); break;
        case MAC_MAD: sb(&b, "    m = mul0(A, B) + C;\n"); break;
        case MAC_DP3: sb(&b, "    m = float4(A.x * B.x + A.y * B.y + A.z * B.z);\n"); break;
        case MAC_DPH: sb(&b, "    m = float4(A.x * B.x + A.y * B.y + A.z * B.z + B.w);\n"); break;
        case MAC_DP4: sb(&b, "    m = float4(A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w);\n"); break;
        case MAC_DST: sb(&b, "    m = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case MAC_MIN: sb(&b, "    m = float4(A.x < B.x ? A.x : B.x, A.y < B.y ? A.y : B.y, A.z < B.z ? A.z : B.z, A.w < B.w ? A.w : B.w);\n"); break;
        case MAC_MAX: sb(&b, "    m = float4(A.x > B.x ? A.x : B.x, A.y > B.y ? A.y : B.y, A.z > B.z ? A.z : B.z, A.w > B.w ? A.w : B.w);\n"); break;
        case MAC_SLT: sb(&b, "    m = float4(A.x < B.x ? 1.0 : 0.0, A.y < B.y ? 1.0 : 0.0, A.z < B.z ? 1.0 : 0.0, A.w < B.w ? 1.0 : 0.0);\n"); break;
        case MAC_SGE: sb(&b, "    m = float4(A.x >= B.x ? 1.0 : 0.0, A.y >= B.y ? 1.0 : 0.0, A.z >= B.z ? 1.0 : 0.0, A.w >= B.w ? 1.0 : 0.0);\n"); break;
        case MAC_ARL: sb(&b, "    m = float4(0.0); na0 = (int)floor(A.x + 0.001);\n"); break;
        default:      sb(&b, "    m = float4(0.0);\n"); break;
        }
        switch (i->ilu) {
        case ILU_MOV: sb(&b, "    u = C;\n"); break;
        case ILU_RCP: sb(&b, "    u = float4(1.0 / C.x);\n"); break;
        case ILU_RCC: sb(&b, "    u = float4(clamp_away(1.0 / C.x));\n"); break;
        case ILU_RSQ: sb(&b, "    u = float4(C.x == 0.0 ? as_type<float>(0x7F800000u) : (nv_isinf(C.x) ? 0.0 : 1.0 / sqrt(abs(C.x))));\n"); break;
        case ILU_EXP: sb(&b, "    u = float4(exp2(floor(C.x)), C.x - floor(C.x), exp2(C.x), 1.0);\n"); break;
        case ILU_LOG:
            sb(&b, "    { float x = abs(C.x);\n"
                   "      if (x == 0.0) u = float4(as_type<float>(0xFF800000u), 1.0, as_type<float>(0xFF800000u), 1.0);\n"
                   "      else { float e = floor(log2(x)); u = float4(e, x / exp2(e), log2(x), 1.0); } }\n");
            break;
        case ILU_LIT:
            sb(&b, "    { float x = max(C.x, 0.0), y = max(C.y, 0.0), w = clamp(C.w, -(128.0 - 1.0 / 256.0), 128.0 - 1.0 / 256.0);\n"
                   "      u = float4(1.0, x, x > 0.0 ? exp2(w * log2(y)) : 0.0, 1.0); }\n");
            break;
        default: sb(&b, "    u = float4(0.0);\n"); break;
        }
        if (i->mac != MAC_NOP && i->mac != MAC_ARL) {
            unsigned mask = i->mac_mask;
            if (paired && i->out_r == 1) mask = 0;
            if (mask) { mask_str(mask, mk); sb(&b, "    %s%s = m%s;\n", treg(i->out_r), mk, mk); }
        }
        if (i->ilu != ILU_NOP && i->ilu_mask) {
            int reg = paired ? 1 : i->out_r;
            mask_str(i->ilu_mask, mk);
            sb(&b, "    %s%s = u%s;\n", treg(reg), mk, mk);
        }
        if (i->o_mask && i->orb) {
            const char *v = NULL;
            if (i->out_mux == 0 && i->mac != MAC_NOP && i->mac != MAC_ARL) v = "m";
            if (i->out_mux == 1 && i->ilu != ILU_NOP) v = "u";
            if (v) {
                int o = i->out_addr & 0xF;
                if (o == 5) {
                    int comp = (i->o_mask & 8) ? 0 : (i->o_mask & 4) ? 1 : (i->o_mask & 2) ? 2 : 3;
                    sb(&b, "    O5.x = %s.%c;\n", v, "xyzw"[comp]);
                } else {
                    mask_str(i->o_mask, mk);
                    sb(&b, "    O%d%s = %s%s;\n", o, mk, v, mk);
                }
            }
        }
        sb(&b, "    a0 = na0;\n");
        if (i->final) break;
    }

    sb(&b,
       "    VSOut o;\n"
       "    float x = trunc(O0.x * 16.0) / 16.0, y = trunc(O0.y * 16.0) / 16.0;\n"
       "    if (P.hud.x > 0.0) { x = P.hud.y + (x - P.hud.y) * P.hud.x; y = P.hud.w + (y - P.hud.w) * P.hud.z; }\n"
       "    else if (P.hud.x < 0.0) x = x <= P.hud.z ? 0.0 : (x >= P.hud.w ? P.screen.x : P.hud.y - (x - P.hud.y) * P.hud.x);\n"
       "    float z = P.screen.z > 0.0 ? O0.z / P.screen.z : O0.z;\n"
       "    float w = O0.w;\n"
       "    if (w >= 0.0) w = clamp(w, 5.421011e-20, 1.8446744e19); else w = clamp(w, -1.8446744e19, -5.421011e-20);\n"
       "    float rhw = 1.0 / w, wc = 1.0 / rhw;\n"
       "    o.pos = float4((x / P.screen.x * 2.0 - 1.0) * wc, (1.0 - y / P.screen.y * 2.0) * wc, z * wc, wc);\n"
       "    if (!(nv_isfinite(x) && nv_isfinite(y) && nv_isfinite(z) && nv_isfinite(rhw))) o.pos = float4(as_type<float>(0x7FC00000u));\n"
       "    o.d0 = nan1(O3);\n"
       "    o.d1 = nan1(O4);\n"
       "    if (P.flags.x == 0.0) o.d1 = float4(0.0, 0.0, 0.0, 1.0);\n"
       "    else if (P.flags.y == 0.0) o.d1.w = 1.0;\n"
       "    o.fog = fogf(P, O5.x);\n"
       "    o.t0 = O9; o.t1 = O10; o.t2 = O11; o.t3 = O12;\n"
       "    return o;\n"
       "}\n");
    return b.overflow ? -1 : (int)(b.p - buf);
}
