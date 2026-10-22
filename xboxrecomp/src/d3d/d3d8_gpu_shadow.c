/* d3d8_gpu_shadow.c -- soft shadows on SDL3's GPU API (the macOS backend); the Windows layer's d3d8_softshadow.c, in
 * Metal Shading Language. See that file and d3d8_softshadow.h for the idea:
 *
 * SSX Tricky draws its shadows as stencil shadow volumes, then one full-screen quad with the stencil test and blend
 * ZERO / SRC_ALPHA darkens every marked pixel at once: the mask is binary, so a shadow edge is always hard. Here, just
 * before that quad (the translator asks, d3d8_SoftShadowDraw), 1. a mask pass writes 1 into an R8 target where the quad's
 * own stencil test passes; 2. two separable blur passes at half size, Gaussian weights times a depth term (relative
 * depth difference), so the mask never leaks onto an object in front of or behind the shadowed surface; 3. the scene
 * colour (and alpha) is multiplied by lerp(1, shade, blurred mask). The title's quad is then drawn without colour writes
 * (it still zeroes the stencil). Sigma 2.5 px at 480 lines, scaled with the render height. */
#include "d3d8_gpu_shadow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct C { float4 size; float4 prm; float4 half_size; };   /* size: w h 1/w 1/h; prm: shade taps sigma depth-falloff */\n"
    "constexpr sampler Lin(filter::linear, address::clamp_to_edge);\n"
    "struct VO { float4 pos [[position]]; };\n"
    "vertex VO ss_vs(uint id [[vertex_id]]) {\n"
    "    float2 t = float2((id << 1) & 2, id & 2); VO o;\n"
    "    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); return o;\n"
    "}\n"
    "fragment float4 ss_one(VO i [[stage_in]]) { return float4(1.0); }\n"
    "static int2 full_hi(constant C &c) { return int2(c.size.xy) - 1; }\n"
    "static int2 half_hi(constant C &c) { return int2(c.half_size.xy) - 1; }\n"
    "static float dweight(constant C &c, float z, float z0, float inv) { float dz = (z - z0) * inv; return 1.0 / (1.0 + dz * dz * c.prm.w); }\n"
    "static float gweight(constant C &c, int i) { return exp2(-1.442695 * float(i * i) / (2.0 * c.prm.z * c.prm.z)); }\n"
    "static float depth_at(depth2d<float> d, int2 p) { return d.read(uint2(p)); }\n"
    /* h: half-size output, full-size mask in (one bilinear 2x2 average per tap) */
    "fragment float4 ss_h(VO i [[stage_in]], constant C &c [[buffer(0)]], texture2d<float> src [[texture(0)]], depth2d<float> dep [[texture(1)]]) {\n"
    "    int2 cc = int2(i.pos.xy), f = cc * 2;\n"
    "    int r = int(c.prm.y), k;\n"
    "    float lo = 1.0, top = 0.0, m[33];\n"
    "    for (k = -r; k <= r; k++) {\n"
    "        float2 uv = (float2(f + int2(2 * k, 0)) + 1.0) * c.size.zw;\n"
    "        float v = src.sample(Lin, uv, level(0)).r;\n"
    "        m[k + r] = v; lo = min(lo, v); top = max(top, v);\n"
    "    }\n"
    "    if (top <= 0.0) return float4(0.0);\n"
    "    if (lo >= 1.0) return float4(1.0);\n"
    "    float z0 = depth_at(dep, min(f, full_hi(c))), inv = 1.0 / max(1.0 - z0, 1e-5);\n"
    "    float acc = 0.0, wsum = 0.0;\n"
    "    for (k = -r; k <= r; k++) {\n"
    "        int2 q = clamp(f + int2(2 * k, 0), int2(0, 0), full_hi(c));\n"
    "        float w = gweight(c, k) * dweight(c, depth_at(dep, q), z0, inv);\n"
    "        acc += w * m[k + r]; wsum += w;\n"
    "    }\n"
    "    return float4(acc / wsum);\n"
    "}\n"
    /* v: half size in and out */
    "fragment float4 ss_v(VO i [[stage_in]], constant C &c [[buffer(0)]], texture2d<float> src [[texture(0)]], depth2d<float> dep [[texture(1)]]) {\n"
    "    int2 cc = int2(i.pos.xy);\n"
    "    int r = int(c.prm.y), k;\n"
    "    float lo = 1.0, top = 0.0, m[33];\n"
    "    for (k = -r; k <= r; k++) {\n"
    "        float v = src.read(uint2(clamp(cc + int2(0, k), int2(0, 0), half_hi(c)))).r;\n"
    "        m[k + r] = v; lo = min(lo, v); top = max(top, v);\n"
    "    }\n"
    "    if (top <= 0.0) return float4(0.0);\n"
    "    if (lo >= 1.0) return float4(1.0);\n"
    "    float z0 = depth_at(dep, min(cc * 2, full_hi(c))), inv = 1.0 / max(1.0 - z0, 1e-5);\n"
    "    float acc = 0.0, wsum = 0.0;\n"
    "    for (k = -r; k <= r; k++) {\n"
    "        int2 q = clamp(cc + int2(0, k), int2(0, 0), half_hi(c));\n"
    "        float w = gweight(c, k) * dweight(c, depth_at(dep, min(q * 2, full_hi(c))), z0, inv);\n"
    "        acc += w * m[k + r]; wsum += w;\n"
    "    }\n"
    "    return float4(acc / wsum);\n"
    "}\n"
    /* apply: full size; the 4 half-size texels around, bilinear x depth weights */
    "fragment float4 ss_apply(VO i [[stage_in]], constant C &c [[buffer(0)]], texture2d<float> src [[texture(0)]], depth2d<float> dep [[texture(1)]]) {\n"
    "    float2 hc = i.pos.xy * 0.5 - 0.5;\n"
    "    int2 b = int2(floor(hc));\n"
    "    float2 t = hc - float2(b);\n"
    "    int2 o[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };\n"
    "    float bw[4] = { (1.0 - t.x) * (1.0 - t.y), t.x * (1.0 - t.y), (1.0 - t.x) * t.y, t.x * t.y };\n"
    "    float v[4], lo = 1.0, top = 0.0, mask;\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "        v[k] = src.read(uint2(clamp(b + o[k], int2(0, 0), half_hi(c)))).r;\n"
    "        lo = min(lo, v[k]); top = max(top, v[k]);\n"
    "    }\n"
    "    if (top <= 0.0) return float4(1.0);\n"
    "    if (lo >= 1.0) { mask = 1.0; }\n"
    "    else {\n"
    "        float z0 = depth_at(dep, int2(i.pos.xy)), inv = 1.0 / max(1.0 - z0, 1e-5);\n"
    "        float acc = 0.0, wsum = 1e-6;\n"
    "        for (int k = 0; k < 4; k++) {\n"
    "            int2 q = clamp(b + o[k], int2(0, 0), half_hi(c));\n"
    "            float w = bw[k] * dweight(c, depth_at(dep, min(q * 2, full_hi(c))), z0, inv);\n"
    "            acc += w * v[k]; wsum += w;\n"
    "        }\n"
    "        mask = acc / wsum;\n"
    "    }\n"
    "    float kk = mix(1.0, c.prm.x, mask);\n"
    "    return float4(kk);\n"
    "}\n";

char *gpu_shadow_source(void)
{
    char *s = (char *)malloc(sizeof k_msl);
    if (s) memcpy(s, k_msl, sizeof k_msl);
    return s;
}

enum { P_MASK_BASE = 0 };       /* mask pipelines are per (cmp, rmask): a few */
typedef struct { unsigned cmp, rmask; SDL_GPUGraphicsPipeline *p; } MaskPipe;

static struct {
    int failed, ready;
    unsigned w, h;
    SDL_GPUShader *vs, *ps_one, *ps_h, *ps_v, *ps_apply;
    SDL_GPUGraphicsPipeline *p_h, *p_v, *p_apply;
    MaskPipe mask[8]; int nmask;
    SDL_GPUTexture *mask_tex, *h1, *h2;
    SDL_GPUSampler *lin;
    SDL_GPUTextureFormat rt_fmt, ds_fmt;
} S;

static SDL_GPUShader *mk(SDL_GPUDevice *dev, const char *src, const char *entry, SDL_GPUShaderStage st, unsigned samplers, unsigned ubo)
{
    SDL_GPUShaderCreateInfo ci;
    SDL_GPUShader *s;
    memset(&ci, 0, sizeof ci);
    ci.code = (const Uint8 *)src; ci.code_size = strlen(src) + 1; ci.format = SDL_GPU_SHADERFORMAT_MSL;
    ci.entrypoint = entry; ci.stage = st; ci.num_samplers = samplers; ci.num_uniform_buffers = ubo;
    s = SDL_CreateGPUShader(dev, &ci);
    if (!s) fprintf(stderr, "[GPU] soft shadow shader %s failed: %s\n", entry, SDL_GetError());
    return s;
}

static SDL_GPUTexture *tex(SDL_GPUDevice *dev, unsigned w, unsigned h)
{
    SDL_GPUTextureCreateInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM; ti.width = w; ti.height = h;
    ti.layer_count_or_depth = 1; ti.num_levels = 1; ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    return SDL_CreateGPUTexture(dev, &ti);
}

static SDL_GPUGraphicsPipeline *pipe(SDL_GPUDevice *dev, SDL_GPUShader *fs, SDL_GPUTextureFormat fmt, int blend,
                                     const SDL_GPUDepthStencilState *dss, SDL_GPUTextureFormat ds_fmt)
{
    SDL_GPUGraphicsPipelineCreateInfo pi;
    SDL_GPUColorTargetDescription ct;
    memset(&pi, 0, sizeof pi); memset(&ct, 0, sizeof ct);
    pi.vertex_shader = S.vs; pi.fragment_shader = fs;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ct.format = fmt;
    ct.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
    ct.blend_state.enable_color_write_mask = true;
    if (blend) {   /* dest = dest * src: the scene darkened by lerp(1, shade, mask), alpha as the title's quad did */
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO; ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_COLOR;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO; ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        ct.blend_state.color_blend_op = ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    pi.target_info.color_target_descriptions = &ct; pi.target_info.num_color_targets = 1;
    if (dss) { pi.depth_stencil_state = *dss; pi.target_info.depth_stencil_format = ds_fmt; pi.target_info.has_depth_stencil_target = true; }
    return SDL_CreateGPUGraphicsPipeline(dev, &pi);
}

static SDL_GPUCompareOp cmp_op(unsigned c)
{
    switch (c) {
    case 1: return SDL_GPU_COMPAREOP_NEVER;   case 2: return SDL_GPU_COMPAREOP_LESS;
    case 3: return SDL_GPU_COMPAREOP_EQUAL;   case 4: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case 5: return SDL_GPU_COMPAREOP_GREATER; case 6: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case 7: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    default: return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

static SDL_GPUGraphicsPipeline *mask_pipe(SDL_GPUDevice *dev, unsigned cmp, unsigned rmask)
{
    SDL_GPUDepthStencilState dss;
    int i;
    for (i = 0; i < S.nmask; i++) if (S.mask[i].cmp == cmp && S.mask[i].rmask == (rmask & 0xFF)) return S.mask[i].p;
    if (S.nmask >= 8) return NULL;
    memset(&dss, 0, sizeof dss);
    dss.enable_stencil_test = true;
    dss.compare_mask = (Uint8)rmask; dss.write_mask = 0;
    dss.front_stencil_state.compare_op = cmp_op(cmp);
    dss.front_stencil_state.fail_op = dss.front_stencil_state.pass_op = dss.front_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
    dss.back_stencil_state = dss.front_stencil_state;
    S.mask[S.nmask].p = pipe(dev, S.ps_one, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 0, &dss, S.ds_fmt);
    S.mask[S.nmask].cmp = cmp; S.mask[S.nmask].rmask = rmask & 0xFF;
    return S.mask[S.nmask++].p;
}

static int setup(SDL_GPUDevice *dev, unsigned w, unsigned h, SDL_GPUTextureFormat rt_fmt, SDL_GPUTextureFormat ds_fmt)
{
    char *src;
    if (!S.ready) {
        SDL_GPUSamplerCreateInfo si;
        src = gpu_shadow_source();
        if (!src) return 0;
        S.vs = mk(dev, src, "ss_vs", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        S.ps_one = mk(dev, src, "ss_one", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
        S.ps_h = mk(dev, src, "ss_h", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
        S.ps_v = mk(dev, src, "ss_v", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
        S.ps_apply = mk(dev, src, "ss_apply", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
        free(src);
        if (!S.vs || !S.ps_one || !S.ps_h || !S.ps_v || !S.ps_apply) return 0;
        S.rt_fmt = rt_fmt; S.ds_fmt = ds_fmt;
        S.p_h = pipe(dev, S.ps_h, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 0, NULL, 0);
        S.p_v = pipe(dev, S.ps_v, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 0, NULL, 0);
        S.p_apply = pipe(dev, S.ps_apply, rt_fmt, 1, NULL, 0);
        memset(&si, 0, sizeof si);
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        S.lin = SDL_CreateGPUSampler(dev, &si);
        if (!S.p_h || !S.p_v || !S.p_apply || !S.lin) { fprintf(stderr, "[GPU] soft shadow pipelines failed: %s\n", SDL_GetError()); return 0; }
        S.ready = 1;
    }
    if (S.w != w || S.h != h) {
        if (S.mask_tex) SDL_ReleaseGPUTexture(dev, S.mask_tex);
        if (S.h1) SDL_ReleaseGPUTexture(dev, S.h1);
        if (S.h2) SDL_ReleaseGPUTexture(dev, S.h2);
        S.mask_tex = tex(dev, w, h); S.h1 = tex(dev, (w + 1) / 2, (h + 1) / 2); S.h2 = tex(dev, (w + 1) / 2, (h + 1) / 2);
        S.w = w; S.h = h;
        if (!S.mask_tex || !S.h1 || !S.h2) { fprintf(stderr, "[GPU] soft shadow targets failed: %s\n", SDL_GetError()); return 0; }
    }
    return 1;
}

static void run_pass(SDL_GPUCommandBuffer *cmd, SDL_GPUGraphicsPipeline *p, SDL_GPUTexture *target, int load_color, SDL_GPUTexture *ds,
                     const SDL_Rect *scissor, Uint8 stencil_ref, const float *cb, SDL_GPUTexture *in0, SDL_GPUTexture *in1,
                     SDL_GPUSampler *smp)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUDepthStencilTargetInfo dt;
    SDL_GPURenderPass *rp;
    memset(&ct, 0, sizeof ct); memset(&dt, 0, sizeof dt);
    ct.texture = target; ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.load_op = load_color ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    if (ds) {
        dt.texture = ds; dt.load_op = SDL_GPU_LOADOP_LOAD; dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = SDL_GPU_LOADOP_LOAD; dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
    }
    rp = SDL_BeginGPURenderPass(cmd, &ct, 1, ds ? &dt : NULL);
    SDL_BindGPUGraphicsPipeline(rp, p);
    SDL_SetGPUScissor(rp, scissor);
    SDL_SetGPUStencilReference(rp, stencil_ref);
    if (cb) SDL_PushGPUFragmentUniformData(cmd, 0, cb, 12 * sizeof(float));
    if (in0) {
        SDL_GPUTextureSamplerBinding b[2];
        int n = 1;
        b[0].texture = in0; b[0].sampler = smp;
        if (in1) { b[1].texture = in1; b[1].sampler = smp; n = 2; }
        SDL_BindGPUFragmentSamplers(rp, 0, b, (Uint32)n);
    }
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
}

int gpu_shadow_run(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *rt, SDL_GPUTextureFormat rt_fmt,
                   SDL_GPUTexture *ds, SDL_GPUTextureFormat ds_fmt, unsigned w, unsigned h,
                   float shade, unsigned cmp, unsigned ref, unsigned rmask, int x0, int y0, int x1, int y1)
{
    float cb[12], sigma;
    int taps;
    SDL_Rect rect, full, hfull;
    SDL_GPUGraphicsPipeline *mp;
    if (S.failed || !dev || !rt || !ds || x1 <= x0 || y1 <= y0) return 0;
    if (!setup(dev, w, h, rt_fmt, ds_fmt)) { S.failed = 1; return 0; }
    mp = mask_pipe(dev, cmp, rmask);
    if (!mp) return 0;
    sigma = 1.25f * (float)h / 480.0f;
    if (sigma < 0.75f) sigma = 0.75f;
    taps = (int)(sigma * 1.5f + 0.5f);
    if (taps < 1) taps = 1;
    if (taps > 16) taps = 16;
    cb[0] = (float)w; cb[1] = (float)h; cb[2] = 1.0f / (float)w; cb[3] = 1.0f / (float)h;
    cb[4] = shade; cb[5] = (float)taps; cb[6] = sigma; cb[7] = 1600.0f;
    cb[8] = (float)((w + 1) / 2); cb[9] = (float)((h + 1) / 2); cb[10] = cb[11] = 0.0f;
    rect.x = x0; rect.y = y0; rect.w = x1 - x0; rect.h = y1 - y0;
    full.x = 0; full.y = 0; full.w = (int)w; full.h = (int)h;
    hfull.x = 0; hfull.y = 0; hfull.w = (int)((w + 1) / 2); hfull.h = (int)((h + 1) / 2);
    {   /* OT_SS_STAGES (test): bit 1 mask, 2 blur, 4 apply -- to see where the time goes */
        static int st = -1;
        if (st < 0) { const char *e = getenv("OT_SS_STAGES"); st = e ? atoi(e) : 7; }
        if (st & 1) run_pass(cmd, mp, S.mask_tex, 0, ds, &rect, (Uint8)ref, NULL, NULL, NULL, NULL);          /* 1. mask */
        if (st & 2) {
            run_pass(cmd, S.p_h, S.h1, 0, NULL, &hfull, 0, cb, S.mask_tex, ds, S.lin);                        /* 2. blur h */
            run_pass(cmd, S.p_v, S.h2, 0, NULL, &hfull, 0, cb, S.h1, ds, S.lin);                              /*    blur v */
        }
        if (st & 4) run_pass(cmd, S.p_apply, rt, 1, NULL, &rect, 0, cb, S.h2, ds, S.lin);                     /* 3. apply */
    }
    (void)full;
    return 1;
}
