/**
 * NV2A vertex programs as HLSL vertex shaders.
 *
 * The title transforms nearly all of its 3D geometry with vertex programs.
 * Until now they ran on the CPU, one vertex at a time, in nv2a_vsh_cpu.c --
 * and that interpreter was two thirds of the render thread, which the game
 * thread waits on (D3D_BlockOnTime spins until the GPU's fence catches up):
 * the race ran at 17-27 frames a second. This file turns a decoded program
 * into an HLSL vertex shader so the GPU runs it instead, as xemu does.
 *
 * The generated code follows vshcpu_run() instruction for instruction --
 * sources read before either unit writes, a paired ILU writing R1 and the
 * MAC's R1 write dropped, R12 aliasing oPos, 0 * anything = 0 in MUL/MAD,
 * ARL's biased floor, constant reads outside 0..191 giving zero -- and then
 * applies what vsh_vertex() and the pass-through shader in d3d8_nv2a.c do to
 * every vertex: 1/16-pixel snapping, z over CLIP_MAX, w kept away from zero
 * with its sign, NaN colours to 1, the specular and fog rules. A vertex the
 * CPU path would reject (non-finite) is sent to NaN, which the rasteriser
 * culls with its triangle, as the CPU path drops the triangle.
 */
#include "d3d8_internal.h"
#include "d3d8_nv2a_vsh.h"
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

/* Temp register n as HLSL: R12 is oPos (output register 0). */
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
        if (i->a0x) snprintf(base, sizeof base, "cread(%u + a0)", i->cidx);
        else        snprintf(base, sizeof base, "cread(%u)", i->cidx);
        break;
    default: snprintf(base, sizeof base, "float4(0, 0, 0, 0)"); break;
    }
    sw[0] = '.';
    for (j = 0; j < 4; j++) sw[1 + j] = comp[i->src[k].swz[j] & 3];
    sw[5] = 0;
    snprintf(out, n, "%s(%s)%s", i->src[k].neg ? "-" : "", base, sw);
}

static const char g_prelude[] =
    "cbuffer Consts : register(b0) { float4 c[192]; };\n"
    "cbuffer Params : register(b1) {\n"
    "    float4 screen;      /* width, height, CLIP_MAX, oPts scale */\n"
    "    float4 fogp;        /* mode (0 off, 1 linear, 2 linear abs, 3 exp, 4 exp abs, 5 exp2), p0, p1 */\n"
    "    float4 flags;       /* specular enable, alpha from specular */\n"
    "    float4 vattr[16];   /* attributes with no array */\n"
    "    float4 ptparm;       /* size from oPts, fixed size, smooth */\n"
    "    float4 hud;          /* HUD element: x scale, x anchor, y scale, y anchor; x scale < 0: a panel's ends, z w = its edges */\n"
    "};\n"
    "struct VSOut {\n"
    "    float4 pos : SV_POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n"
    "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
    "#ifdef NV_POINTS\n"
    "    float4 pt : PTS;    /* centre x, y in title pixels, half size */\n"
    "#endif\n"
    "};\n"
    "float4 cread(int n) { return (n >= 0 && n < 192) ? c[n] : float4(0, 0, 0, 0); }\n"
    "float4 mul0(float4 a, float4 b) {\n"
    "    float4 p = a * b;\n"
    "    return float4(a.x == 0 || b.x == 0 ? 0 : p.x, a.y == 0 || b.y == 0 ? 0 : p.y,\n"
    "                  a.z == 0 || b.z == 0 ? 0 : p.z, a.w == 0 || b.w == 0 ? 0 : p.w);\n"
    "}\n"
    "float clamp_away(float t) {\n"
    "    if (t > 0 || (t == 0 && asuint(t) == 0)) return clamp(t, 5.421011e-20, 1.8446744e+19);\n"
    "    return clamp(t, -1.8446744e+19, -5.421011e-20);\n"
    "}\n"
    "float fogf(float d) {\n"
    "    float f, m = fogp.x;\n"
    "    if (m == 0) return 1;\n"
    "    if (m == 2 || m == 4) d = abs(d);\n"
    "    if ((m == 1 || m == 3) && isinf(d)) d = 0;\n"
    "    if (m <= 2) f = fogp.y + d * fogp.z - 1.0;\n"
    "    else if (m <= 4) f = fogp.y + exp2(d * fogp.z * 16.0) - 1.5;\n"
    "    else f = fogp.y + exp2(-d * d * fogp.z * fogp.z * 32.0) - 1.5;\n"
    "    if (isnan(f)) return 1;\n"
    "    return saturate(f);\n"
    "}\n"
    "float4 nan1(float4 v) { return float4(isnan(v.x) ? 1 : v.x, isnan(v.y) ? 1 : v.y,\n"
    "                                      isnan(v.z) ? 1 : v.z, isnan(v.w) ? 1 : v.w); }\n";

int nv2a_vsh_hlsl(const vshcpu_insn *p, int n, uint16_t inputs, const uint8_t kind[16],
                  int points, char *buf, int cap)
{
    Sb b = { buf, buf + cap, 0 };
    int k, a;

    if (points) sb(&b, "#define NV_POINTS 1\n");
    sb(&b, "%s", g_prelude);
    sb(&b, "struct VSIn {\n");
    for (a = 0; a < 16; a++) {
        if (!(inputs & (1u << a)) || (kind[a] & 0x0F) == NV2A_VSH_IN_CONST) continue;
        switch (kind[a] & 0x0F) {
        case NV2A_VSH_IN_INT:  sb(&b, "    int4 a%d : V%d;\n", a, a); break;
        case NV2A_VSH_IN_CMP:  sb(&b, "    uint a%d : V%d;\n", a, a); break;
        default:               sb(&b, "    float4 a%d : V%d;\n", a, a); break;
        }
    }
    sb(&b, "    uint vid : SV_VertexID;\n};\n");

    sb(&b, "VSOut main(VSIn i) {\n");
    for (a = 0; a < 16; a++) {
        if (!(inputs & (1u << a))) {
            sb(&b, "    float4 v%d = float4(0, 0, 0, 1);\n", a);
            continue;
        }
        switch (kind[a] & 0x0F) {
        case NV2A_VSH_IN_CONST: sb(&b, "    float4 v%d = vattr[%d];\n", a, a); break;
        case NV2A_VSH_IN_INT:   sb(&b, "    float4 v%d = float4(i.a%d);\n", a, a); break;
        case NV2A_VSH_IN_CMP:
            /* Packed normal: x 11 bits, y 11 bits, z 10 bits, all signed. */
            sb(&b, "    float4 v%d = float4(float(asint(i.a%d << 21) >> 21) / 1023.0,\n"
                   "        float(asint(i.a%d << 10) >> 21) / 1023.0, float(asint(i.a%d) >> 22) / 511.0, 1);\n",
               a, a, a, a);
            break;
        default: sb(&b, "    float4 v%d = i.a%d;\n", a, a); break;
        }
        if (kind[a] & NV2A_VSH_IN_W1) sb(&b, "    v%d.w = 1;\n", a);
    }
    sb(&b, "    float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0,\n"
           "           r8 = 0, r9 = 0, r10 = 0, r11 = 0, r13 = 0, r14 = 0, r15 = 0;\n"
           "    float4 O0 = float4(0, 0, 0, 1), O1 = O0, O2 = O0, O3 = O0, O4 = O0, O5 = O0, O6 = O0,\n"
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
        case MAC_DP3: sb(&b, "    m = (A.x * B.x + A.y * B.y + A.z * B.z).xxxx;\n"); break;
        case MAC_DPH: sb(&b, "    m = (A.x * B.x + A.y * B.y + A.z * B.z + B.w).xxxx;\n"); break;
        case MAC_DP4: sb(&b, "    m = (A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w).xxxx;\n"); break;
        case MAC_DST: sb(&b, "    m = float4(1, A.y * B.y, A.z, B.w);\n"); break;
        case MAC_MIN: sb(&b, "    m = float4(A.x < B.x ? A.x : B.x, A.y < B.y ? A.y : B.y, A.z < B.z ? A.z : B.z, A.w < B.w ? A.w : B.w);\n"); break;
        case MAC_MAX: sb(&b, "    m = float4(A.x > B.x ? A.x : B.x, A.y > B.y ? A.y : B.y, A.z > B.z ? A.z : B.z, A.w > B.w ? A.w : B.w);\n"); break;
        case MAC_SLT: sb(&b, "    m = float4(A.x < B.x ? 1 : 0, A.y < B.y ? 1 : 0, A.z < B.z ? 1 : 0, A.w < B.w ? 1 : 0);\n"); break;
        case MAC_SGE: sb(&b, "    m = float4(A.x >= B.x ? 1 : 0, A.y >= B.y ? 1 : 0, A.z >= B.z ? 1 : 0, A.w >= B.w ? 1 : 0);\n"); break;
        case MAC_ARL: sb(&b, "    m = 0; na0 = (int)floor(A.x + 0.001);\n"); break;
        default:      sb(&b, "    m = 0;\n"); break;
        }
        switch (i->ilu) {
        case ILU_MOV: sb(&b, "    u = C;\n"); break;
        case ILU_RCP: sb(&b, "    u = (1.0 / C.x).xxxx;\n"); break;
        case ILU_RCC: sb(&b, "    u = clamp_away(1.0 / C.x).xxxx;\n"); break;
        case ILU_RSQ: sb(&b, "    u = (C.x == 0 ? asfloat(0x7F800000) : isinf(C.x) ? 0 : 1.0 / sqrt(abs(C.x))).xxxx;\n"); break;
        case ILU_EXP: sb(&b, "    u = float4(exp2(floor(C.x)), C.x - floor(C.x), exp2(C.x), 1);\n"); break;
        case ILU_LOG:
            sb(&b, "    { float x = abs(C.x);\n"
                   "      if (x == 0) u = float4(asfloat(0xFF800000), 1, asfloat(0xFF800000), 1);\n"
                   "      else { float e = floor(log2(x)); u = float4(e, x / exp2(e), log2(x), 1); } }\n");
            break;
        case ILU_LIT:
            sb(&b, "    { float x = max(C.x, 0), y = max(C.y, 0), w = clamp(C.w, -(128.0 - 1.0 / 256.0), 128.0 - 1.0 / 256.0);\n"
                   "      u = float4(1, x, x > 0 ? exp2(w * log2(y)) : 0, 1); }\n");
            break;
        default: sb(&b, "    u = 0;\n"); break;
        }
        /* Writes, after every read above (vshcpu_run's order). */
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
                if (o == 5) {                 /* oFog is scalar: the first masked component */
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

    /* Per-vertex rules of vsh_vertex() and the pass-through shader. */
    sb(&b,
       "    VSOut o;\n"
       "    float x = trunc(O0.x * 16.0) / 16.0, y = trunc(O0.y * 16.0) / 16.0;\n"
       "    if (hud.x > 0) { x = hud.y + (x - hud.y) * hud.x; y = hud.w + (y - hud.w) * hud.z; }\n"
       "    else if (hud.x < 0) x = x <= hud.z ? 0 : x >= hud.w ? screen.x : hud.y - (x - hud.y) * hud.x;\n"
       "    float z = screen.z > 0 ? O0.z / screen.z : O0.z;\n"
       "    float w = O0.w;\n"
       "    if (w >= 0) w = clamp(w, 5.421011e-20, 1.8446744e19); else w = clamp(w, -1.8446744e19, -5.421011e-20);\n"
       "    float rhw = 1.0 / w, wc = 1.0 / rhw;\n"
       "    o.pos = float4((x / screen.x * 2.0 - 1.0) * wc, (1.0 - y / screen.y * 2.0) * wc, z * wc, wc);\n"
       "    if (!(isfinite(x) && isfinite(y) && isfinite(z) && isfinite(rhw))) o.pos = asfloat(0x7FC00000).xxxx;\n"
       "    o.d0 = nan1(O3);\n"
       "    o.d1 = nan1(O4);\n"
       "    if (flags.x == 0) o.d1 = float4(0, 0, 0, 1);\n"
       "    else if (flags.y == 0) o.d1.w = 1;\n"
       "    o.fog = fogf(O5.x);\n"
       "    o.t0 = O9; o.t1 = O10; o.t2 = O11; o.t3 = O12;\n"
       /* Point size as draw_program() takes it: oPts.x or the fixed size,
        * at least 1 pixel (NaN too), at most 2048. */
       "#ifdef NV_POINTS\n"
       "    float s = ptparm.x != 0 ? O6.x * screen.w : ptparm.y;\n"
       "    if (!(s >= 1.0)) s = 1.0;\n"
       "    if (s > 2048.0) s = 2048.0;\n"
       "    o.pt = float4(x, y, s * 0.5, 0);\n"
       "#endif\n"
       "    return o;\n"
       "}\n");
    return b.overflow ? -1 : (int)(b.p - buf);
}
