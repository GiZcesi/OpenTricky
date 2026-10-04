/**
 * nv2a_vsh_cpu -- NV2A vertex-program interpreter. See nv2a_vsh_cpu.h.
 *
 * Instruction layout (128 bits, dword 0 unused), per the hardware field table
 * that xemu and Cxbx both document:
 *
 *   dword 1: ILU[27:25] MAC[24:21] CONST[20:13] V[12:9]
 *            A.neg[8] A.swz x[7:6] y[5:4] z[3:2] w[1:0]
 *   dword 2: A.R[31:28] A.mux[27:26] B.neg[25] B.swz[24:17] B.R[16:13]
 *            B.mux[12:11] C.neg[10] C.swz[9:2] C.R high[1:0]
 *   dword 3: C.R low[31:30] C.mux[29:28] MAC mask[27:24] OUT.R[23:20]
 *            ILU mask[19:16] O mask[15:12] ORB[11] OUT address[10:3]
 *            OUT mux[2] A0X[1] FINAL[0]
 *
 * The runtime's d3d8_vsh.c decoder uses a different, incorrect layout (its
 * ILU field is four bits wide and its source registers sit in dword 0), so
 * nothing here depends on it.
 *
 * Semantics that are easy to get wrong:
 *  - a MAC and an ILU op in the same slot run in parallel: both read their
 *    sources before either writes;
 *  - when paired, the ILU can only write temp R1, and a MAC write to R1 is
 *    dropped;
 *  - R12 is oPos;
 *  - MUL (and MAD's multiply) gives 0 when either factor is 0, even against
 *    infinity;
 *  - ARL floors with a small bias, because the values it indexes with usually
 *    come from byte attributes normalised to [0,1] and scaled back up.
 */
#include "nv2a_vsh_cpu.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MUX_NONE, MUX_R, MUX_V, MUX_C };

static unsigned fld(const uint32_t *t, int dw, int bit, int len)
{
    return (t[dw] >> bit) & ((1u << len) - 1u);
}

static void swz(const uint32_t *t, int dw, int bit, uint8_t s[4])
{
    s[0] = (uint8_t)fld(t, dw, bit + 6, 2);
    s[1] = (uint8_t)fld(t, dw, bit + 4, 2);
    s[2] = (uint8_t)fld(t, dw, bit + 2, 2);
    s[3] = (uint8_t)fld(t, dw, bit, 2);
}

int vshcpu_decode(const uint32_t prog[VSHCPU_SLOTS][4], int start,
                  vshcpu_insn *out)
{
    int n = 0, s;
    if (start < 0 || start >= VSHCPU_SLOTS)
        return -1;
    for (s = start; s < VSHCPU_SLOTS; s++) {
        const uint32_t *t = prog[s];
        vshcpu_insn *i = &out[n++];
        memset(i, 0, sizeof *i);
        i->ilu  = (uint8_t)fld(t, 1, 25, 3);
        i->mac  = (uint8_t)fld(t, 1, 21, 4);
        i->cidx = (uint8_t)fld(t, 1, 13, 8);
        i->vidx = (uint8_t)fld(t, 1, 9, 4);
        i->src[0].neg = (uint8_t)fld(t, 1, 8, 1);
        swz(t, 1, 0, i->src[0].swz);
        i->src[0].reg = (uint8_t)fld(t, 2, 28, 4);
        i->src[0].mux = (uint8_t)fld(t, 2, 26, 2);
        i->src[1].neg = (uint8_t)fld(t, 2, 25, 1);
        swz(t, 2, 17, i->src[1].swz);
        i->src[1].reg = (uint8_t)fld(t, 2, 13, 4);
        i->src[1].mux = (uint8_t)fld(t, 2, 11, 2);
        i->src[2].neg = (uint8_t)fld(t, 2, 10, 1);
        swz(t, 2, 2, i->src[2].swz);
        i->src[2].reg = (uint8_t)((fld(t, 2, 0, 2) << 2) | fld(t, 3, 30, 2));
        i->src[2].mux = (uint8_t)fld(t, 3, 28, 2);
        i->mac_mask = (uint8_t)fld(t, 3, 24, 4);
        i->out_r    = (uint8_t)fld(t, 3, 20, 4);
        i->ilu_mask = (uint8_t)fld(t, 3, 16, 4);
        i->o_mask   = (uint8_t)fld(t, 3, 12, 4);
        i->orb      = (uint8_t)fld(t, 3, 11, 1);
        i->out_addr = (uint8_t)fld(t, 3, 3, 8);
        i->out_mux  = (uint8_t)fld(t, 3, 2, 1);
        i->a0x      = (uint8_t)fld(t, 3, 1, 1);
        i->final    = (uint8_t)fld(t, 3, 0, 1);
        if (i->final)
            return n;
    }
    return -1;
}

typedef struct { float v[4]; } v4;

static v4 fetch(const vshcpu_insn *i, int k, const float in[16][4],
                float c[VSHCPU_CONSTANTS][4], float r[16][4],
                float out[VSHCPU_OUT_REGS][4], int a0)
{
    const float *s = NULL;
    static const float zero[4] = { 0, 0, 0, 0 };
    v4 x;
    int j;
    switch (i->src[k].mux) {
    case MUX_R:
        s = (i->src[k].reg == 12) ? out[VSHCPU_OUT_POS] : r[i->src[k].reg];
        break;
    case MUX_V:
        s = in[i->vidx];
        break;
    case MUX_C: {
        int ci = i->cidx + (i->a0x ? a0 : 0);
        s = (ci >= 0 && ci < VSHCPU_CONSTANTS) ? c[ci] : zero;
        break;
    }
    default:
        s = zero;
        break;
    }
    for (j = 0; j < 4; j++) {
        x.v[j] = s[i->src[k].swz[j]];
        if (i->src[k].neg) x.v[j] = -x.v[j];
    }
    return x;
}

static float mul0(float a, float b)
{
    return (a == 0.0f || b == 0.0f) ? 0.0f : a * b;
}

static float clamp_away(float t)
{
    /* The hardware keeps reciprocals out of zero and infinity. */
    if (t > 0.0f || (t == 0.0f && !signbit(t))) {
        if (t < 5.421011e-20f) t = 5.421011e-20f;
        if (t > 1.8446744e+19f) t = 1.8446744e+19f;
    } else {
        if (t > -5.421011e-20f) t = -5.421011e-20f;
        if (t < -1.8446744e+19f) t = -1.8446744e+19f;
    }
    return t;
}

static void store(float *d, const v4 *s, unsigned mask)
{
    if (mask & 8) d[0] = s->v[0];
    if (mask & 4) d[1] = s->v[1];
    if (mask & 2) d[2] = s->v[2];
    if (mask & 1) d[3] = s->v[3];
}

void vshcpu_run(const vshcpu_insn *p, int n, const float in[16][4],
                float c[VSHCPU_CONSTANTS][4], float out[VSHCPU_OUT_REGS][4])
{
    float r[16][4];
    int a0 = 0, k, j;
    memset(r, 0, sizeof r);
    for (j = 0; j < VSHCPU_OUT_REGS; j++) {
        out[j][0] = out[j][1] = out[j][2] = 0.0f;
        out[j][3] = 1.0f;
    }

    for (k = 0; k < n; k++) {
        const vshcpu_insn *i = &p[k];
        v4 A, B, C, m, u;
        int paired = i->mac != MAC_NOP && i->ilu != ILU_NOP;
        int new_a0 = a0;

        if (i->mac == MAC_NOP && i->ilu == ILU_NOP)
            continue;
        A = fetch(i, 0, in, c, r, out, a0);
        B = fetch(i, 1, in, c, r, out, a0);
        C = fetch(i, 2, in, c, r, out, a0);
        memset(&m, 0, sizeof m);
        memset(&u, 0, sizeof u);

        switch (i->mac) {
        case MAC_MOV: m = A; break;
        case MAC_MUL: for (j = 0; j < 4; j++) m.v[j] = mul0(A.v[j], B.v[j]); break;
        case MAC_ADD: for (j = 0; j < 4; j++) m.v[j] = A.v[j] + C.v[j]; break;
        case MAC_MAD: for (j = 0; j < 4; j++) m.v[j] = mul0(A.v[j], B.v[j]) + C.v[j]; break;
        case MAC_DP3: {
            float d = A.v[0] * B.v[0] + A.v[1] * B.v[1] + A.v[2] * B.v[2];
            m.v[0] = m.v[1] = m.v[2] = m.v[3] = d;
            break;
        }
        case MAC_DPH: {
            float d = A.v[0] * B.v[0] + A.v[1] * B.v[1] + A.v[2] * B.v[2] + B.v[3];
            m.v[0] = m.v[1] = m.v[2] = m.v[3] = d;
            break;
        }
        case MAC_DP4: {
            float d = A.v[0] * B.v[0] + A.v[1] * B.v[1] + A.v[2] * B.v[2] + A.v[3] * B.v[3];
            m.v[0] = m.v[1] = m.v[2] = m.v[3] = d;
            break;
        }
        case MAC_DST:
            m.v[0] = 1.0f; m.v[1] = A.v[1] * B.v[1]; m.v[2] = A.v[2]; m.v[3] = B.v[3];
            break;
        case MAC_MIN: for (j = 0; j < 4; j++) m.v[j] = A.v[j] < B.v[j] ? A.v[j] : B.v[j]; break;
        case MAC_MAX: for (j = 0; j < 4; j++) m.v[j] = A.v[j] > B.v[j] ? A.v[j] : B.v[j]; break;
        case MAC_SLT: for (j = 0; j < 4; j++) m.v[j] = A.v[j] < B.v[j] ? 1.0f : 0.0f; break;
        case MAC_SGE: for (j = 0; j < 4; j++) m.v[j] = A.v[j] >= B.v[j] ? 1.0f : 0.0f; break;
        case MAC_ARL: new_a0 = (int)floorf(A.v[0] + 0.001f); break;
        default: break;
        }

        switch (i->ilu) {
        case ILU_MOV: u = C; break;
        case ILU_RCP: {
            float t = 1.0f / C.v[0];
            u.v[0] = u.v[1] = u.v[2] = u.v[3] = t;
            break;
        }
        case ILU_RCC: {
            float t = clamp_away(1.0f / C.v[0]);
            u.v[0] = u.v[1] = u.v[2] = u.v[3] = t;
            break;
        }
        case ILU_RSQ: {
            float x = C.v[0], t;
            if (x == 0.0f) t = INFINITY;
            else if (isinf(x)) t = 0.0f;
            else t = 1.0f / sqrtf(fabsf(x));
            u.v[0] = u.v[1] = u.v[2] = u.v[3] = t;
            break;
        }
        case ILU_EXP: {
            float x = C.v[0], f = floorf(x);
            u.v[0] = exp2f(f); u.v[1] = x - f; u.v[2] = exp2f(x); u.v[3] = 1.0f;
            break;
        }
        case ILU_LOG: {
            float x = fabsf(C.v[0]);
            if (x == 0.0f) {
                u.v[0] = -INFINITY; u.v[1] = 1.0f; u.v[2] = -INFINITY; u.v[3] = 1.0f;
            } else {
                float e = floorf(log2f(x));
                u.v[0] = e; u.v[1] = x / exp2f(e); u.v[2] = log2f(x); u.v[3] = 1.0f;
            }
            break;
        }
        case ILU_LIT: {
            float x = C.v[0] > 0.0f ? C.v[0] : 0.0f;
            float y = C.v[1] > 0.0f ? C.v[1] : 0.0f;
            float w = C.v[3], eps = 1.0f / 256.0f;
            if (w < -(128.0f - eps)) w = -(128.0f - eps);
            if (w > 128.0f - eps) w = 128.0f - eps;
            u.v[0] = 1.0f; u.v[1] = x;
            u.v[2] = (x > 0.0f) ? exp2f(w * log2f(y)) : 0.0f;
            u.v[3] = 1.0f;
            break;
        }
        default: break;
        }

        /* Writes, all after every read above. */
        if (i->mac != MAC_NOP && i->mac != MAC_ARL) {
            unsigned mask = i->mac_mask;
            if (paired && i->out_r == 1) mask = 0;
            if (mask)
                store(i->out_r == 12 ? out[VSHCPU_OUT_POS] : r[i->out_r], &m, mask);
        }
        if (i->ilu != ILU_NOP && i->ilu_mask) {
            int reg = paired ? 1 : i->out_r;
            store(reg == 12 ? out[VSHCPU_OUT_POS] : r[reg], &u, i->ilu_mask);
        }
        if (i->o_mask) {
            const v4 *v = NULL;
            if (i->out_mux == 0 && i->mac != MAC_NOP && i->mac != MAC_ARL) v = &m;
            if (i->out_mux == 1 && i->ilu != ILU_NOP) v = &u;
            if (v) {
                if (i->orb) {
                    int o = i->out_addr & 0xF;
                    if (o == VSHCPU_OUT_FOG) {
                        /* oFog is scalar: whichever component the mask names
                         * lands in .x. */
                        int comp = (i->o_mask & 8) ? 0 : (i->o_mask & 4) ? 1
                                 : (i->o_mask & 2) ? 2 : 3;
                        out[o][0] = v->v[comp];
                    } else if (o < VSHCPU_OUT_REGS) {
                        store(out[o], v, i->o_mask);
                    }
                } else if (i->out_addr < VSHCPU_CONSTANTS) {
                    store(c[i->out_addr], v, i->o_mask);
                }
            }
        }
        a0 = new_a0;
        if (i->final)
            break;
    }
}

void vshcpu_format(const vshcpu_insn *i, char *buf, int len)
{
    static const char *mac[] = { "nop", "mov", "mul", "add", "mad", "dp3", "dph",
                                 "dp4", "dst", "min", "max", "slt", "sge", "arl",
                                 "?14", "?15" };
    static const char *ilu[] = { "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit" };
    char s[3][24];
    int k;
    for (k = 0; k < 3; k++) {
        const char *sw = "xyzw";
        char reg[12];
        switch (i->src[k].mux) {
        case MUX_R: snprintf(reg, sizeof reg, "r%u", i->src[k].reg); break;
        case MUX_V: snprintf(reg, sizeof reg, "v%u", i->vidx); break;
        case MUX_C: snprintf(reg, sizeof reg, "c%s%u", i->a0x ? "[a0]+" : "", i->cidx); break;
        default: snprintf(reg, sizeof reg, "-"); break;
        }
        snprintf(s[k], sizeof s[k], "%s%s.%c%c%c%c", i->src[k].neg ? "-" : "", reg,
                 sw[i->src[k].swz[0]], sw[i->src[k].swz[1]],
                 sw[i->src[k].swz[2]], sw[i->src[k].swz[3]]);
    }
    snprintf(buf, len, "%s r%u/%X  %s r%s/%X  | A=%s B=%s C=%s | out %s%u/%X mux=%s%s",
             mac[i->mac & 15], i->out_r, i->mac_mask, ilu[i->ilu & 7],
             (i->mac != MAC_NOP && i->ilu != ILU_NOP) ? "1" : "*", i->ilu_mask,
             s[0], s[1], s[2], i->orb ? "o" : "c", i->out_addr, i->o_mask,
             i->out_mux ? "ilu" : "mac", i->final ? " FINAL" : "");
}
