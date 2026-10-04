/*
 * NV2A register combiners -> HLSL. See nv2a_psh.h.
 *
 * Ported from xemu's hw/xbox/nv2a/pgraph/glsl/psh.c (LGPL-2.1+, espes and the
 * xemu project): the same register decoding, input and output mappings,
 * stage ordering (every read of a stage happens before any of its writes)
 * and final combiner. Not ported: shadow maps, convolution filters, window
 * clipping, colour keys and the per-pixel depth rewrite -- depth comes from
 * the rasteriser -- and textures are all bound as 2D, so cube and 3D lookups
 * fall back to xemu's own 2D remaps.
 */
#include "nv2a_psh.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *buf; int size, len, overflow; } SB;

static void sbf(SB *s, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (s->overflow) return;
    va_start(ap, fmt);
    n = vsnprintf(s->buf + s->len, (size_t)(s->size - s->len), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= s->size - s->len) { s->overflow = 1; return; }
    s->len += n;
}

enum {
    MODE_NONE = 0x00, MODE_PROJECT2D = 0x01, MODE_PROJECT3D = 0x02, MODE_CUBEMAP = 0x03,
    MODE_PASSTHRU = 0x04, MODE_CLIPPLANE = 0x05, MODE_BUMPENVMAP = 0x06,
    MODE_BUMPENVMAP_LUM = 0x07, MODE_BRDF = 0x08, MODE_DOT_ST = 0x09, MODE_DOT_ZW = 0x0a,
    MODE_DOT_RFLCT_DIFF = 0x0b, MODE_DOT_RFLCT_SPEC = 0x0c, MODE_DOT_STR_3D = 0x0d,
    MODE_DOT_STR_CUBE = 0x0e, MODE_DPNDNT_AR = 0x0f, MODE_DPNDNT_GB = 0x10,
    MODE_DOTPRODUCT = 0x11, MODE_DOT_RFLCT_SPEC_CONST = 0x12
};

typedef struct { int reg, mod, chan; } In;
typedef struct { int ab, cd, muxsum, flags, ab_op, cd_op, muxsum_op, mapping; } Out;

typedef struct {
    const Nv2aPshState *st;
    int num_stages, flags, cur_stage;
    int tex_modes[4], input_tex[4], dot_map[4];
    In rgb_in[8][4], alpha_in[8][4];
    Out rgb_out[8], alpha_out[8];
    In fin[7];
    int final_enabled, clamp_sum, inv_v1, inv_r0;
    char varE[2304], varF[2304];
} Ps;

static void parse_input(In *v, int value)
{
    v->reg = value & 0xF;
    v->chan = value & 0x10;
    v->mod = value & 0xE0;
}

static void parse_inputs(uint32_t value, In *abcd)
{
    parse_input(&abcd[3], value & 0xFF);
    parse_input(&abcd[2], (value >> 8) & 0xFF);
    parse_input(&abcd[1], (value >> 16) & 0xFF);
    parse_input(&abcd[0], (value >> 24) & 0xFF);
}

static void parse_output(uint32_t value, Out *o)
{
    int flags = (int)(value >> 12);
    o->cd = value & 0xF;
    o->ab = (value >> 4) & 0xF;
    o->muxsum = (value >> 8) & 0xF;
    o->flags = flags;
    o->cd_op = flags & 1;
    o->ab_op = flags & 2;
    o->muxsum_op = flags & 4;
    o->mapping = flags & 0x38;
}

/* Register name; "" for a discarded destination. */
static void get_var(const Ps *ps, int reg, int is_dest, char *out, size_t n)
{
    switch (reg) {
    case 0x0: snprintf(out, n, "%s", is_dest ? "" : "float4(0.0, 0.0, 0.0, 0.0)"); break;
    case 0x1:
        snprintf(out, n, "c0[%d]", ((ps->flags & 0x10) || ps->cur_stage == 8) ? ps->cur_stage : 0);
        break;
    case 0x2:
        snprintf(out, n, "c1[%d]", ((ps->flags & 0x100) || ps->cur_stage == 8) ? ps->cur_stage : 0);
        break;
    case 0x3: snprintf(out, n, "pFog"); break;
    case 0x4: snprintf(out, n, "v0"); break;
    case 0x5: snprintf(out, n, "v1"); break;
    case 0x8: case 0x9: case 0xA: case 0xB: snprintf(out, n, "t%d", reg - 8); break;
    case 0xC: snprintf(out, n, "r0"); break;
    case 0xD: snprintf(out, n, "r1"); break;
    case 0xE:
        snprintf(out, n, ps->clamp_sum ? "saturate(float4(%s.rgb + %s.rgb, 0.0))"
                                       : "float4(%s.rgb + %s.rgb, 0.0)",
                 ps->inv_v1 ? "(1.0 - v1)" : "v1", ps->inv_r0 ? "(1.0 - r0)" : "r0");
        break;
    case 0xF: snprintf(out, n, "float4(%s * %s, 0.0)", ps->varE, ps->varF); break;
    default: snprintf(out, n, "float4(0.0, 0.0, 0.0, 0.0)"); break;
    }
}

static void get_input_var(const Ps *ps, In in, int is_alpha, char *out, size_t n)
{
    char reg[4800];
    get_var(ps, in.reg, 0, reg, sizeof reg);
    if (!is_alpha)
        strncat(reg, in.chan ? ".aaa" : ".rgb", sizeof reg - strlen(reg) - 1);
    else
        strncat(reg, in.chan ? ".a" : ".b", sizeof reg - strlen(reg) - 1);
    switch (in.mod) {
    case 0x00: snprintf(out, n, "max(%s, 0.0)", reg); break;
    case 0x20: snprintf(out, n, "(1.0 - clamp(%s, 0.0, 1.0))", reg); break;
    case 0x40: snprintf(out, n, "(2.0 * max(%s, 0.0) - 1.0)", reg); break;
    case 0x60: snprintf(out, n, "(-2.0 * max(%s, 0.0) + 1.0)", reg); break;
    case 0x80: snprintf(out, n, "(max(%s, 0.0) - 0.5)", reg); break;
    case 0xA0: snprintf(out, n, "(-max(%s, 0.0) + 0.5)", reg); break;
    case 0xC0: snprintf(out, n, "%s", reg); break;
    default:   snprintf(out, n, "-%s", reg); break;
    }
}

static void get_output(const char *e, int mapping, char *out, size_t n)
{
    switch (mapping) {
    case 0x08: snprintf(out, n, "(%s - 0.5)", e); break;
    case 0x10: snprintf(out, n, "(%s * 2.0)", e); break;
    case 0x18: snprintf(out, n, "((%s - 0.5) * 2.0)", e); break;
    case 0x20: snprintf(out, n, "(%s * 4.0)", e); break;
    case 0x30: snprintf(out, n, "(%s / 2.0)", e); break;
    default:   snprintf(out, n, "%s", e); break;
    }
}

/* One channel of one general combiner stage: computations go to `code`, the
 * register writes to `assign` (emitted after both channels have read). */
static void stage_code(Ps *ps, const In *in, const Out *o, int is_alpha, SB *code, SB *assign)
{
    static char a[5000], b[5000], c[5000], d[5000], ab[10100], cd[10100], abm[10200], cdm[10200];
    static char mux[20500], muxm[20600];
    char abd[5000], cdd[5000], msd[5000];
    const char *wm = is_alpha ? "a" : "rgb";
    const char *cast = is_alpha ? "" : "(float3)";

    get_input_var(ps, in[0], is_alpha, a, sizeof a);
    get_input_var(ps, in[1], is_alpha, b, sizeof b);
    get_input_var(ps, in[2], is_alpha, c, sizeof c);
    get_input_var(ps, in[3], is_alpha, d, sizeof d);
    snprintf(ab, sizeof ab, o->ab_op ? "dot(%s, %s)" : "(%s * %s)", a, b);
    snprintf(cd, sizeof cd, o->cd_op ? "dot(%s, %s)" : "(%s * %s)", c, d);
    get_output(ab, o->mapping, abm, sizeof abm);
    get_output(cd, o->mapping, cdm, sizeof cdm);
    get_var(ps, o->ab, 1, abd, sizeof abd);
    get_var(ps, o->cd, 1, cdd, sizeof cdd);
    get_var(ps, o->muxsum, 1, msd, sizeof msd);

    if (abd[0]) sbf(code, "ab.%s = clamp(%s(%s), -1.0, 1.0);\n", wm, cast, abm);
    if (cdd[0]) sbf(code, "cd.%s = clamp(%s(%s), -1.0, 1.0);\n", wm, cast, cdm);
    if (msd[0]) {
        if (!o->muxsum_op)
            snprintf(mux, sizeof mux, "(%s + %s)", ab, cd);
        else
            snprintf(mux, sizeof mux, "((%s) ? %s(%s) : %s(%s))",
                     (ps->flags & 0x1) ? "r0.a >= 0.5" : "(((uint)(r0.a * 255.0)) & 1u) == 1u",
                     cast, cd, cast, ab);
        get_output(mux, o->mapping, muxm, sizeof muxm);
        sbf(code, "mux_sum.%s = clamp(%s(%s), -1.0, 1.0);\n", wm, cast, muxm);
    }
    if (abd[0]) {
        sbf(assign, "%s.%s = ab.%s;\n", abd, wm, wm);
        if (!is_alpha && (o->flags & 0x80)) sbf(assign, "%s.a = ab.b;\n", abd);
    }
    if (cdd[0]) {
        sbf(assign, "%s.%s = cd.%s;\n", cdd, wm, wm);
        if (!is_alpha && (o->flags & 0x40)) sbf(assign, "%s.a = cd.b;\n", cdd);
    }
    if (msd[0]) sbf(assign, "%s.%s = mux_sum.%s;\n", msd, wm, wm);
}

static const char *const dotmap_funcs[8] = {
    "dotmap_zero_to_one", "dotmap_minus1_to_1_d3d", "dotmap_minus1_to_1_gl",
    "dotmap_minus1_to_1", "dotmap_hilo_1", "dotmap_hilo_hemisphere_d3d",
    "dotmap_hilo_hemisphere_gl", "dotmap_hilo_hemisphere",
};

static const char preamble[] =
    "cbuffer PshConsts : register(b0) {\n"
    "    float4 c0[9];\n"
    "    float4 c1[9];\n"
    "    float4 fog_color;\n"
    "    float4 tex_size[4];\n"
    "    float4 alpha_ref;\n"
    "    float4 clip_region[8];\n"
    "};\n"
    "struct PSIn {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 d0  : COLOR0;\n"
    "    float4 d1  : COLOR1;\n"
    "    float  fog : FOG;\n"
    "    float4 t0  : TEXCOORD0;\n"
    "    float4 t1  : TEXCOORD1;\n"
    "    float4 t2  : TEXCOORD2;\n"
    "    float4 t3  : TEXCOORD3;\n"
    "};\n"
    "float sign1(float x) { x *= 255.0; return (x - 128.0) / 127.0; }\n"
    "float sign2(float x) { x *= 255.0; return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5; }\n"
    "float sign3(float x) { x *= 255.0; return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0; }\n"
    "float3 dotmap_zero_to_one(float4 c) { return c.rgb; }\n"
    "float3 dotmap_minus1_to_1_d3d(float4 c) { return float3(sign1(c.r), sign1(c.g), sign1(c.b)); }\n"
    "float3 dotmap_minus1_to_1_gl(float4 c) { return float3(sign2(c.r), sign2(c.g), sign2(c.b)); }\n"
    "float3 dotmap_minus1_to_1(float4 c) { return float3(sign3(c.r), sign3(c.g), sign3(c.b)); }\n"
    "float3 dotmap_hilo_1(float4 c) {\n"
    "    uint hi = ((uint)(c.a * 255.0) << 8) | (uint)(c.r * 255.0);\n"
    "    uint lo = ((uint)(c.g * 255.0) << 8) | (uint)(c.b * 255.0);\n"
    "    return float3(hi / 65535.0, lo / 65535.0, 1.0);\n"
    "}\n"
    "float3 dotmap_hilo_hemisphere_d3d(float4 c) { return c.rgb; }\n"
    "float3 dotmap_hilo_hemisphere_gl(float4 c) { return c.rgb; }\n"
    "float3 dotmap_hilo_hemisphere(float4 c) { return c.rgb; }\n"
    "float2 remapCubeTo2D(float3 t) {\n"
    "    float3 a = abs(t);\n"
    "    float2 uv;\n"
    "    if (a.x > a.y && a.x > a.z) uv = (t.x > 0.0 ? float2(-t.z, t.y) : float2(t.z, t.y)) / a.x;\n"
    "    else if (a.y > a.x && a.y > a.z) uv = (t.y > 0.0 ? float2(t.x, -t.z) : float2(t.x, t.z)) / a.y;\n"
    "    else uv = (t.z > 0.0 ? float2(t.x, t.y) : float2(-t.x, t.y)) / a.z;\n"
    "    return uv;\n"
    "}\n";

/* Normalised 2D coordinate expression for stage i from `coord` (a float2
 * expression): rect (linear) textures are addressed in texels. */
static void norm2(const Ps *ps, int i, const char *coord, char *out, size_t n)
{
    if (ps->st->tex_rect[i])
        snprintf(out, n, "((%s) / tex_size[%d].xy)", coord, i);
    else
        snprintf(out, n, "(%s)", coord);
}

static int stage_samples(int mode)
{
    switch (mode) {
    case MODE_NONE: case MODE_PASSTHRU: case MODE_CLIPPLANE: case MODE_BRDF:
    case MODE_DOT_ZW: case MODE_DOTPRODUCT: case MODE_DOT_RFLCT_SPEC_CONST:
        return 0;
    default:
        return 1;
    }
}

static void tex_code(Ps *ps, int i, SB *decl, SB *v)
{
    const Nv2aPshState *st = ps->st;
    int mode = ps->tex_modes[i], in = ps->input_tex[i] & 3;
    const char *dm = dotmap_funcs[ps->dot_map[i] & 7];
    char co[256], nrm[320];

    if (stage_samples(mode))
        sbf(decl, "Texture2D tex%d : register(t%d);\nSamplerState smp%d : register(s%d);\n",
            i, i, i, i);
    switch (mode) {
    case MODE_NONE:
        sbf(v, "float4 t%d = float4(0.0, 0.0, 0.0, 1.0);\n", i);
        break;
    case MODE_PROJECT2D:
    case MODE_PROJECT3D:                 /* no 3D textures: project the xy plane */
        if (st->tex_cube[i]) {
            snprintf(co, sizeof co, "remapCubeTo2D(float3(1.0, pT%d.y / pT%d.w, -pT%d.x / pT%d.w))",
                     i, i, i, i);
            sbf(v, "float4 t%d = tex%d.Sample(smp%d, %s);\n", i, i, i, co);
        } else {
            snprintf(co, sizeof co, "pT%d.xy", i);
            norm2(ps, i, co, nrm, sizeof nrm);
            sbf(v, "float4 t%d = tex%d.Sample(smp%d, %s / pT%d.w);\n", i, i, i, nrm, i);
        }
        break;
    case MODE_CUBEMAP:
        sbf(v, "float4 t%d = tex%d.Sample(smp%d, remapCubeTo2D(pT%d.xyz));\n", i, i, i, i);
        break;
    case MODE_PASSTHRU:
        sbf(v, "float4 t%d = pT%d;\n", i, i);
        break;
    case MODE_CLIPPLANE:
        sbf(v, "float4 t%d = float4(0.0, 0.0, 0.0, 0.0);\n"
               "if (pT%d.x < 0.0 || pT%d.y < 0.0 || pT%d.z < 0.0 || pT%d.w < 0.0) discard;\n",
            i, i, i, i, i);
        break;
    case MODE_BUMPENVMAP:
    case MODE_BUMPENVMAP_LUM:            /* bump matrix not tracked yet: unperturbed */
        snprintf(co, sizeof co, "pT%d.xy", i);
        norm2(ps, i, co, nrm, sizeof nrm);
        sbf(v, "float4 t%d = tex%d.Sample(smp%d, %s);\n", i, i, i, nrm);
        break;
    case MODE_DOT_ST:
        sbf(v, "float dot%d = dot(pT%d.xyz, %s(t%d));\n", i, i, dm, in);
        snprintf(co, sizeof co, "float2(dot%d, dot%d)", i - 1, i);
        norm2(ps, i, co, nrm, sizeof nrm);
        sbf(v, "float4 t%d = tex%d.Sample(smp%d, %s);\n", i, i, i, nrm);
        break;
    case MODE_DOT_ZW:
    case MODE_DOTPRODUCT:
        sbf(v, "float dot%d = dot(pT%d.xyz, %s(t%d));\nfloat4 t%d = float4(0.0, 0.0, 0.0, 0.0);\n",
            i, i, dm, in, i);
        break;
    case MODE_DOT_RFLCT_DIFF:
        sbf(v, "float dot%d = dot(pT%d.xyz, %s(t%d));\n", i, i, dm, in);
        sbf(v, "float dot%d_n = dot(pT%d.xyz, %s(t%d));\n", i, i + 1,
            dotmap_funcs[ps->dot_map[(i + 1) & 3] & 7], ps->input_tex[(i + 1) & 3] & 3);
        sbf(v, "float4 t%d = tex%d.Sample(smp%d, remapCubeTo2D(float3(dot%d, dot%d, dot%d_n)));\n",
            i, i, i, i - 1, i, i);
        break;
    case MODE_DOT_RFLCT_SPEC:
        sbf(v, "float dot%d = dot(pT%d.xyz, %s(t%d));\n", i, i, dm, in);
        sbf(v, "float3 n_%d = float3(dot%d, dot%d, dot%d);\n", i, i - 2, i - 1, i);
        sbf(v, "float3 e_%d = float3(pT%d.w, pT%d.w, pT%d.w);\n", i, i - 2, i - 1, i);
        sbf(v, "float3 rv_%d = 2.0 * n_%d * dot(n_%d, e_%d) / dot(n_%d, n_%d) - e_%d;\n",
            i, i, i, i, i, i, i);
        sbf(v, "float4 t%d = tex%d.Sample(smp%d, remapCubeTo2D(rv_%d));\n", i, i, i, i);
        break;
    case MODE_DOT_STR_3D:
    case MODE_DOT_STR_CUBE:
        sbf(v, "float dot%d = dot(pT%d.xyz, %s(t%d));\n", i, i, dm, in);
        sbf(v, "float4 t%d = tex%d.Sample(smp%d, remapCubeTo2D(float3(dot%d, dot%d, dot%d)));\n",
            i, i, i, i - 2, i - 1, i);
        break;
    case MODE_DPNDNT_AR:
        sbf(v, "float4 t%d = tex%d.Sample(smp%d, t%d.ar);\n", i, i, i, in);
        break;
    case MODE_DPNDNT_GB:
        sbf(v, "float4 t%d = tex%d.Sample(smp%d, t%d.gb);\n", i, i, i, in);
        break;
    default:                             /* BRDF, DOT_RFLCT_SPEC_CONST: unimplemented in xemu too */
        sbf(v, "float4 t%d = float4(0.0, 0.0, 0.0, 0.0);\n", i);
        break;
    }
    if (stage_samples(mode) && st->alphakill[i])
        sbf(v, "if (t%d.a == 0.0) discard;\n", i);
}

uint64_t nv2a_psh_key(const Nv2aPshState *st)
{
    const uint8_t *p = (const uint8_t *)st;
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;
    for (i = 0; i < sizeof *st; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

int nv2a_psh_generate(const Nv2aPshState *st, char *buf, int size)
{
    static Ps ps;
    static char declbuf[4096], varsbuf[16384], codebuf[65536], asgbuf[16384];
    SB out = { buf, size, 0, 0 }, decl = { declbuf, sizeof declbuf, 0, 0 };
    SB vars = { varsbuf, sizeof varsbuf, 0, 0 }, code = { codebuf, sizeof codebuf, 0, 0 };
    int i;

    memset(&ps, 0, sizeof ps);
    ps.st = st;
    ps.num_stages = (int)(st->combiner_control & 0xFF);
    if (ps.num_stages > 8) ps.num_stages = 8;
    ps.flags = (int)(st->combiner_control >> 8);
    {
        /* Stage modes come from SHADER_STAGE_PROGRAM alone, as in xemu
         * (psh.c, tex_modes): a mode that samples nothing (PASSTHRU, DOT
         * products...) needs no bound texture. The UBERBOARD's stage 1 is
         * PASSTHRU with texture 1 disabled (its normal, (0.5, 0.5, 1)); it
         * was forced to NONE, so the board's dot products went black.
         * Only sampling modes still need the texture. XBOX_FIX_TEXPASSTHRU=0
         * restores the old gating. */
        static int fix = -1;
        if (fix < 0) { const char *e = getenv("XBOX_FIX_TEXPASSTHRU"); fix = !(e && e[0] == '0'); }
        for (i = 0; i < 4; i++) {
            int m = (int)((st->shader_stage_program >> (i * 5)) & 0x1F);
            if (!st->tex_enabled[i] && (!fix || stage_samples(m)))
                m = MODE_NONE;
            ps.tex_modes[i] = m;
        }
    }
    ps.dot_map[1] = (int)(st->other_stage_input & 0xF);
    ps.dot_map[2] = (int)((st->other_stage_input >> 4) & 0xF);
    ps.dot_map[3] = (int)((st->other_stage_input >> 8) & 0xF);
    ps.input_tex[1] = 0;
    ps.input_tex[2] = (int)((st->other_stage_input >> 16) & 0xF);
    ps.input_tex[3] = (int)((st->other_stage_input >> 20) & 0xF);

    sbf(&vars, "float4 pT0 = i.t0, pT1 = i.t1, pT2 = i.t2, pT3 = i.t3;\n");
    sbf(&vars, "float4 v0 = saturate(i.d0), v1 = saturate(i.d1);\n");
    sbf(&vars, "float4 pFog = float4(fog_color.rgb, saturate(i.fog));\n");

    if (st->simple) {
        ps.tex_modes[0] = st->tex_enabled[0] ? MODE_PROJECT2D : MODE_NONE;
        tex_code(&ps, 0, &decl, &vars);
        sbf(&code, "float4 fragColor = %s;\n", st->tex_enabled[0] ? "t0 * v0" : "v0");
    } else {
        for (i = 0; i < 4; i++)
            tex_code(&ps, i, &decl, &vars);
        sbf(&vars, "float4 r0 = float4(0.0, 0.0, 0.0, %s);\n",
            ps.tex_modes[0] != MODE_NONE ? "t0.a" : "1.0");
        sbf(&vars, "float4 r1 = float4(0.0, 0.0, 0.0, 0.0);\n");
        sbf(&vars, "float4 ab = r1, cd = r1, mux_sum = r1;\n");

        for (i = 0; i < ps.num_stages; i++) {
            SB asg = { asgbuf, sizeof asgbuf, 0, 0 };
            parse_inputs(st->rgb_in[i], ps.rgb_in[i]);
            parse_inputs(st->alpha_in[i], ps.alpha_in[i]);
            parse_output(st->rgb_out[i], &ps.rgb_out[i]);
            parse_output(st->alpha_out[i], &ps.alpha_out[i]);
            ps.cur_stage = i;
            sbf(&code, "// stage %d\n", i);
            stage_code(&ps, ps.rgb_in[i], &ps.rgb_out[i], 0, &code, &asg);
            stage_code(&ps, ps.alpha_in[i], &ps.alpha_out[i], 1, &code, &asg);
            if (asg.overflow) code.overflow = 1;
            sbf(&code, "%s", asgbuf);
        }

        ps.final_enabled = st->final0 || st->final1;
        if (ps.final_enabled) {
            static char a[5000], b[5000], c[5000], d[5000], g[5000];
            In blank;
            parse_inputs(st->final0, &ps.fin[0]);              /* A B C D */
            {
                In efg[4];
                parse_inputs(st->final1, efg);                 /* E F G - */
                ps.fin[4] = efg[0]; ps.fin[5] = efg[1]; ps.fin[6] = efg[2];
                blank = efg[3];
                (void)blank;
            }
            ps.clamp_sum = (st->final1 & 0x80) != 0;
            ps.inv_v1 = (st->final1 & 0x40) != 0;
            ps.inv_r0 = (st->final1 & 0x20) != 0;
            ps.cur_stage = 8;
            get_input_var(&ps, ps.fin[4], 0, ps.varE, sizeof ps.varE);
            get_input_var(&ps, ps.fin[5], 0, ps.varF, sizeof ps.varF);
            get_input_var(&ps, ps.fin[0], 0, a, sizeof a);
            get_input_var(&ps, ps.fin[1], 0, b, sizeof b);
            get_input_var(&ps, ps.fin[2], 0, c, sizeof c);
            get_input_var(&ps, ps.fin[3], 0, d, sizeof d);
            get_input_var(&ps, ps.fin[6], 1, g, sizeof g);
            sbf(&code, "// final combiner\nfloat4 fragColor;\n");
            sbf(&code, "fragColor.rgb = %s + lerp((float3)(%s), (float3)(%s), (float3)(%s));\n",
                d, c, b, a);
            sbf(&code, "fragColor.a = %s;\n", g);
        } else {
            sbf(&code, "float4 fragColor = r0;\n");
        }
    }

    if (st->show) {
        /* XBOX_NV2A_PSH_SHOW (diagnostic): output one register instead. */
        static const char *const regs[] = { "t0", "t1", "t2", "t3", "v0", "v1", "pFog", "r0", "r1" };
        int k = st->show - 1;
        /* r0/r1 exist only in the full combiner, not in the simple shader. */
        if (k >= 0 && k < 9 && (k < 7 || !st->simple))
            sbf(&code, "fragColor = float4(%s.rgb, 1.0);\n", regs[k]);
    }
    if (st->alpha_test && st->alpha_func != 7) {
        static const char *const ops[8] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
        if (st->alpha_func == 0)
            sbf(&code, "discard;\n");
        else
            sbf(&code, "if (!((int)round(saturate(fragColor.a) * 255.0) %s (int)alpha_ref.x)) discard;\n",
                ops[st->alpha_func & 7]);
    }

    sbf(&out, "%s%s", preamble, declbuf);
    sbf(&out, "float4 main(PSIn i) : SV_TARGET {\n");
    if (st->window_clip) {
        /* Window clip, as xemu's psh.c: up to eight rectangles in surface
         * pixels. Inclusive keeps a pixel inside any of them, exclusive
         * drops it. The 3D menus clip their scene to the inside of the UI
         * frame; without this it ran to the screen edges. */
        sbf(&out, "{ float2 wc = i.pos.xy - 0.5; bool hit = false;\n"
                  "  [unroll] for (int k = 0; k < 8; k++)\n"
                  "    if (all(wc >= clip_region[k].xy) && all(wc < clip_region[k].zw)) hit = true;\n"
                  "  if (%shit) discard; }\n", st->window_clip == 2 ? "" : "!");
    }
    sbf(&out, "%s%sreturn fragColor;\n}\n", varsbuf, codebuf);
    if (out.overflow || decl.overflow || vars.overflow || code.overflow)
        return -1;
    return out.len;
}
