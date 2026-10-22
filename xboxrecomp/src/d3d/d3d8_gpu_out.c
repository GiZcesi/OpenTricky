/* d3d8_gpu_out.c -- final pass with a colour transform (HDR / wide gamut). See d3d8_gpu_out.h.
 *
 * The title's picture is SDR, in sRGB-like gamma, drawn for a TV; a wide-gamut HDR display can show more than that
 * (this one: about DCI-P3). With HDR output (SDL's HDR_EXTENDED_LINEAR swapchain: linear sRGB primaries, 1.0 = SDR white,
 * values above 1 are brighter than white, negative values are colours outside sRGB) the pass:
 *   1. decodes the picture (sRGB transfer) to linear light;
 *   2. gamut: reads the colours as Display P3 -- the matrix P3 -> sRGB, which turns a saturated sRGB orange into the
 *      more saturated orange the panel can reach (greys stay grey: the matrix keeps white); `gamut` mixes between the two;
 *   3. warm: scales the chroma (distance from the luma) of oranges and yellows by up to `warm` more, weighted by hue; the
 *      result is then held inside Display P3 (what the panel can show), so it never asks for more than there is;
 *   4. boost: colourful highlights (bright and saturated: fire, sun, lamps, the game's gold) are lifted towards the
 *      display's headroom; neutral colours, so the white of the HUD and menus, are not touched.
 * A smooth shoulder above white keeps shades that the expansion pushed past 1.0 apart (a clip at the display's headroom merged
 * them: blown highlights). Nothing here clips: the swapchain is float. Without an HDR display the module is not used (the SDR blit is). */
#include "d3d8_gpu_out.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct U { float4 p; };   /* gamut, warm, boost, headroom */\n"
    "constexpr sampler Lin(filter::linear, address::clamp_to_edge);\n"
    "struct VO { float4 pos [[position]]; float2 uv; };\n"
    "vertex VO out_vs(uint id [[vertex_id]]) {\n"
    "    VO o; o.uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); return o;\n"
    "}\n"
    "static float3 srgb_to_linear(float3 c) {\n"
    "    float3 lo = c / 12.92, hi = pow((c + 0.055) / 1.055, float3(2.4));\n"
    "    return select(hi, lo, c <= 0.04045);\n"
    "}\n"
    /* Display P3 -> sRGB primaries, D65, linear */
    "constant float3x3 P3_TO_SRGB = float3x3(\n"
    "    float3( 1.2249401, -0.0420569, -0.0196376),\n"
    "    float3(-0.2249404,  1.0420571, -0.0786361),\n"
    "    float3( 0.0000000,  0.0000000,  1.0982735));\n"
    "constant float3x3 SRGB_TO_P3 = float3x3(\n"
    "    float3(0.8224621, 0.0331941, 0.0170827),\n"
    "    float3(0.1775380, 0.9668058, 0.0723974),\n"
    "    float3(0.0000000, 0.0000000, 0.9105199));\n"
    "static float hue_weight(float3 c) {   /* 1 at orange-yellow (about 45 degrees), 0 outside 10..80 */\n"
    "    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b)), d = mx - mn;\n"
    "    if (d < 1e-4 || mx < 1e-3) return 0.0;\n"
    "    float h;\n"
    "    if (mx == c.r) h = fmod((c.g - c.b) / d + 6.0, 6.0);\n"
    "    else if (mx == c.g) h = (c.b - c.r) / d + 2.0;\n"
    "    else h = (c.r - c.g) / d + 4.0;\n"
    "    h *= 60.0;\n"
    "    return clamp(1.0 - abs(h - 45.0) / 35.0, 0.0, 1.0) * clamp(d / max(mx, 1e-4) * 2.0, 0.0, 1.0);\n"
    "}\n"
    "static float3 linear_to_srgb(float3 c) {\n"
    "    c = max(c, float3(0.0));\n"
    "    float3 lo = c * 12.92, hi = 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055;\n"
    "    return select(hi, lo, c <= 0.0031308);\n"
    "}\n"
    /* The 3D picture gets less of the gamut expansion than the HUD and menus: this pass runs on it (in the scene target, at
     * FRAME_END) and moves its colours part of the way back, c' = mix(c, P3->sRGB inverse * c, u.p.x), so that the later
     * matrix of out_ps gives mix(c, P3_TO_SRGB * c, 1 - u.p.x) exactly (in linear light; sRGB colours stay inside [0, 1]). */
    "fragment float4 comp_ps(VO i [[stage_in]], constant U &u [[buffer(0)]], texture2d<float> src [[texture(0)]]) {\n"
    "    float4 s = src.read(uint2(i.pos.xy));\n"
    "    float3 lin = srgb_to_linear(s.rgb);\n"
    "    float3 c = mix(lin, SRGB_TO_P3 * lin, u.p.x);\n"
    "    return float4(linear_to_srgb(c), s.a);\n"
    "}\n"
    "fragment float4 out_ps(VO i [[stage_in]], constant U &u [[buffer(0)]], texture2d<float> src [[texture(0)]]) {\n"
    "    float4 s = src.sample(Lin, i.uv);\n"
    "    float3 lin = srgb_to_linear(s.rgb);\n"
    "    float3 c = mix(lin, P3_TO_SRGB * lin, u.p.x);\n"
    "    float y = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
    "    c = float3(y) + (c - float3(y)) * (1.0 + u.p.y * hue_weight(lin));\n"
    "    c = P3_TO_SRGB * max(SRGB_TO_P3 * c, float3(0.0));\n"        /* never beyond what a P3 panel can show */
    "    float mx = max(c.r, max(c.g, c.b)), mn = max(min(c.r, min(c.g, c.b)), 0.0);\n"
    "    float sat = (mx - mn) / max(mx, 1e-4);\n"
    "    float lift = smoothstep(0.5, 1.0, mx) * sat;\n"
    "    c *= 1.0 + max(u.p.w - 1.0, 0.0) * u.p.z * lift;\n"
    /* A shoulder above white instead of a clip: the largest channel is rolled off smoothly into the display's headroom (slope 1
     * at white, then flatter, never above the headroom), so shades that the expansion pushed over 1.0 stay apart. */
    "    float hr = max(u.p.w, 1.0), top = max(c.r, max(c.g, c.b)), room = hr - 1.0;\n"
    "    if (top > 1.0) {\n"
    "        float t2 = room > 1e-3 ? 1.0 + room * (1.0 - exp(-(top - 1.0) / room)) : 1.0;\n"
    "        c *= t2 / top;\n"
    "    }\n"
    "    return float4(c, 1.0);\n"
    "}\n"
    /* The SDR upscaler: Catmull-Rom bicubic (five bilinear fetches), then a mild contrast-adaptive sharpen from the four neighbours
     * (a Laplacian, kept between the neighbourhood's own min and max so it cannot ring). No colour change: gamma in, gamma out. */
    "struct UU { float4 p; };   /* source width, source height, sharpness, 0 */\n"
    "static float4 cr5(texture2d<float> t, float2 uv, float2 sz) {\n"
    "    float2 pos = uv * sz, c = floor(pos - 0.5) + 0.5, f = pos - c;\n"
    "    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
    "    float2 s12 = w1 + w2, off12 = w2 / s12;\n"
    "    float2 t0 = (c - 1.0) / sz, t12 = (c + off12) / sz, t3 = (c + 2.0) / sz;\n"
    "    float4 r = t.sample(Lin, float2(t12.x, t0.y)) * (s12.x * w0.y) + t.sample(Lin, float2(t0.x, t12.y)) * (w0.x * s12.y)\n"
    "             + t.sample(Lin, t12) * (s12.x * s12.y) + t.sample(Lin, float2(t3.x, t12.y)) * (w3.x * s12.y)\n"
    "             + t.sample(Lin, float2(t12.x, t3.y)) * (s12.x * w3.y);\n"
    "    float wsum = s12.x * w0.y + w0.x * s12.y + s12.x * s12.y + w3.x * s12.y + s12.x * w3.y;\n"
    "    return r / wsum;\n"
    "}\n"
    /* Edge-adaptive upscaling in the manner of FSR 1's EASU (an original implementation of the idea): the local gradient of a 4x4 patch gives an edge
     * direction and strength; the reconstruction kernel (a Lanczos-2 shaped weight) is stretched along the edge, so edges stay straight and sharp instead of
     * stair-stepping, and the result is held inside the range of the four nearest texels (no ringing). */
    "static float ea_luma(float3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
    "static float3 ea_tap(texture2d<float> t, int2 p, int2 hi) { return t.read(uint2(clamp(p, int2(0), hi))).rgb; }\n"
    "static float4 easu(texture2d<float> t, float2 uv, float2 sz) {\n"
    "    int2 hi = int2(sz) - 1;\n"
    "    float2 pos = uv * sz - 0.5, bf = floor(pos), f = pos - bf; int2 b = int2(bf);\n"
    "    float3 c[16]; float l[16];\n"
    "    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { c[y * 4 + x] = ea_tap(t, b + int2(x - 1, y - 1), hi); l[y * 4 + x] = ea_luma(c[y * 4 + x]); }\n"
    "    float wt[4] = { (1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y };\n"
    "    float gx = 0.0, gy = 0.0;\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "        int ix = 1 + (k & 1), iy = 1 + (k >> 1);\n"
    "        gx += wt[k] * (l[iy * 4 + ix + 1] - l[iy * 4 + ix - 1]);\n"
    "        gy += wt[k] * (l[(iy + 1) * 4 + ix] - l[(iy - 1) * 4 + ix]);\n"
    "    }\n"
    "    float g2 = gx * gx + gy * gy;\n"
    "    float2 dir = g2 > 1e-6 ? float2(gx, gy) * rsqrt(g2) : float2(1.0, 0.0);\n"
    "    float2 perp = float2(-dir.y, dir.x);\n"
    "    float str = saturate(sqrt(g2) * 2.0);\n"
    "    float along = mix(1.0, 0.45, str);          /* distances along the edge count less: the kernel reaches further along it */\n"
    "    float3 acc = float3(0.0); float ws = 0.0;\n"
    "    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {\n"
    "        float2 d = float2(float(x - 1), float(y - 1)) - f;\n"
    "        float2 q = float2(dot(d, dir), dot(d, perp) * along);\n"
    "        float d2 = min(dot(q, q), 4.0);\n"
    "        float a = 0.4 * d2 - 1.0, bq = 0.25 * d2 - 1.0;\n"
    "        float w = (1.5625 * a * a - 0.5625) * bq * bq;\n"
    "        acc += c[y * 4 + x] * w; ws += w;\n"
    "    }\n"
    "    float3 r = acc / max(ws, 1e-4);\n"
    "    float3 lo = min(min(c[5], c[6]), min(c[9], c[10])), hi4 = max(max(c[5], c[6]), max(c[9], c[10]));\n"
    "    return float4(clamp(r, lo, hi4), 1.0);\n"
    "}\n"
    "fragment float4 up_ps(VO i [[stage_in]], constant UU &u [[buffer(0)]], texture2d<float> src [[texture(0)]]) {\n"
    "    float2 sz = u.p.xy, px = 1.0 / sz;\n"
    "    if (u.p.w > 0.5) {   /* edge-adaptive */\n"
    "        float4 e = easu(src, i.uv, sz);\n"
    "        if (u.p.z > 0.0) {   /* the same mild unsharp as below, against the plain bilinear reading */\n"
    "            float3 bl = src.sample(Lin, i.uv).rgb;\n"
    "            e.rgb = saturate(e.rgb + (e.rgb - bl) * (0.5 * u.p.z));\n"
    "        }\n"
    "        return float4(saturate(e.rgb), 1.0);\n"
    "    }\n"
    "    float4 c = cr5(src, i.uv, sz);\n"
    "    if (u.p.z > 0.0) {\n"
    "        float3 n = src.sample(Lin, i.uv + float2(0.0, -px.y)).rgb, s = src.sample(Lin, i.uv + float2(0.0, px.y)).rgb;\n"
    "        float3 w = src.sample(Lin, i.uv + float2(-px.x, 0.0)).rgb, e = src.sample(Lin, i.uv + float2(px.x, 0.0)).rgb;\n"
    "        float3 lo = min(min(n, s), min(w, e)), hi = max(max(n, s), max(w, e));\n"
    "        float3 lap = c.rgb * 4.0 - (n + s + w + e);\n"
    "        c.rgb = clamp(c.rgb + lap * (0.25 * u.p.z), min(lo, c.rgb), max(hi, c.rgb));\n"
    "    }\n"
    "    return float4(saturate(c.rgb), 1.0);\n"
    "}\n";

float gpu_out_effective_headroom(float reported, float peak)
{
    float hr = reported, cap = peak;
    if (!(cap >= 1.0f)) cap = 1.0f;        /* also catches NaN and 0 (a non-numeric OT_HDR_PEAK) */
    if (!(hr >= 1.0f)) hr = 1.0f;          /* also NaN */
    return hr > cap ? cap : hr;
}

char *gpu_out_source(void)
{
    char *s = (char *)malloc(sizeof k_msl);
    if (s) memcpy(s, k_msl, sizeof k_msl);
    return s;
}

static struct {
    int failed;
    SDL_GPUShader *vs, *ps, *ps_comp, *ps_up;
    SDL_GPUSampler *smp;
    SDL_GPUGraphicsPipeline *pipe[2], *pipe_comp[2], *pipe_up[2];
    SDL_GPUTextureFormat fmt[2];
    int n;
} S;

static SDL_GPUGraphicsPipeline *pipe_for(SDL_GPUDevice *dev, SDL_GPUTextureFormat fmt, int comp)      /* comp: 0 colour pass, 1 scene pass, 2 upscaler */
{
    int i;
    for (i = 0; i < S.n; i++) if (S.fmt[i] == fmt) return comp == 2 ? S.pipe_up[i] : comp ? S.pipe_comp[i] : S.pipe[i];
    if (S.n >= 2) return NULL;
    {
        SDL_GPUGraphicsPipelineCreateInfo pi; SDL_GPUColorTargetDescription ct;
        memset(&pi, 0, sizeof pi); memset(&ct, 0, sizeof ct);
        pi.vertex_shader = S.vs; pi.fragment_shader = S.ps;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        ct.format = fmt;
        ct.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
        ct.blend_state.enable_color_write_mask = true;
        pi.target_info.color_target_descriptions = &ct; pi.target_info.num_color_targets = 1;
        S.pipe[S.n] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        pi.fragment_shader = S.ps_comp;
        S.pipe_comp[S.n] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        pi.fragment_shader = S.ps_up;
        S.pipe_up[S.n] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if (!S.pipe[S.n] || !S.pipe_comp[S.n] || !S.pipe_up[S.n]) { fprintf(stderr, "[GPU] colour pass pipeline failed: %s\n", SDL_GetError()); return NULL; }
        S.fmt[S.n] = fmt;
        S.n++;
        return comp == 2 ? S.pipe_up[S.n - 1] : comp ? S.pipe_comp[S.n - 1] : S.pipe[S.n - 1];
    }
}

/* Builds the shaders and the sampler shared by the passes of this module (once). */
static int ensure_shared(SDL_GPUDevice *dev)
{
    char *code; SDL_GPUShaderCreateInfo ci; SDL_GPUSamplerCreateInfo si;
    if (S.vs) return 1;
    code = gpu_out_source();
    if (!code) { S.failed = 1; return 0; }
    memset(&ci, 0, sizeof ci);
    ci.code = (const Uint8 *)code; ci.code_size = sizeof k_msl; ci.format = SDL_GPU_SHADERFORMAT_MSL;
    ci.entrypoint = "out_vs"; ci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    S.vs = SDL_CreateGPUShader(dev, &ci);
    ci.entrypoint = "out_ps"; ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; ci.num_samplers = 1; ci.num_uniform_buffers = 1;
    S.ps = SDL_CreateGPUShader(dev, &ci);
    ci.entrypoint = "comp_ps";
    S.ps_comp = SDL_CreateGPUShader(dev, &ci);
    ci.entrypoint = "up_ps";
    S.ps_up = SDL_CreateGPUShader(dev, &ci);
    free(code);
    memset(&si, 0, sizeof si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    S.smp = SDL_CreateGPUSampler(dev, &si);
    if (!S.vs || !S.ps || !S.ps_comp || !S.ps_up || !S.smp) { fprintf(stderr, "[GPU] colour pass shaders failed: %s\n", SDL_GetError()); S.failed = 1; return 0; }
    return 1;
}

int gpu_out_draw(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, SDL_GPUTexture *dst,
                 SDL_GPUTextureFormat dst_fmt, const SDL_Rect *dest, const GpuOutParams *p)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUTextureSamplerBinding b;
    SDL_GPURenderPass *rp;
    SDL_GPUViewport vp;
    SDL_GPUGraphicsPipeline *pl;
    float u[4];
    if (S.failed || !dev || !src || !dst) return 0;
    if (!S.vs) {
        char *code = gpu_out_source();
        SDL_GPUShaderCreateInfo ci;
        SDL_GPUSamplerCreateInfo si;
        if (!code) { S.failed = 1; return 0; }
        memset(&ci, 0, sizeof ci);
        ci.code = (const Uint8 *)code; ci.code_size = sizeof k_msl; ci.format = SDL_GPU_SHADERFORMAT_MSL;
        ci.entrypoint = "out_vs"; ci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        S.vs = SDL_CreateGPUShader(dev, &ci);
        ci.entrypoint = "out_ps"; ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; ci.num_samplers = 1; ci.num_uniform_buffers = 1;
        S.ps = SDL_CreateGPUShader(dev, &ci);
        ci.entrypoint = "comp_ps";
        S.ps_comp = SDL_CreateGPUShader(dev, &ci);
        ci.entrypoint = "up_ps";
        S.ps_up = SDL_CreateGPUShader(dev, &ci);
        free(code);
        memset(&si, 0, sizeof si);
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        S.smp = SDL_CreateGPUSampler(dev, &si);
        if (!S.vs || !S.ps || !S.ps_comp || !S.ps_up || !S.smp) { fprintf(stderr, "[GPU] colour pass shaders failed: %s\n", SDL_GetError()); S.failed = 1; return 0; }
    }
    pl = pipe_for(dev, dst_fmt, 0);
    if (!pl) { S.failed = 1; return 0; }
    memset(&ct, 0, sizeof ct);
    ct.texture = dst; ct.load_op = SDL_GPU_LOADOP_CLEAR; ct.store_op = SDL_GPU_STOREOP_STORE;
    rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(rp, pl);
    vp.x = (float)dest->x; vp.y = (float)dest->y; vp.w = (float)dest->w; vp.h = (float)dest->h; vp.min_depth = 0.0f; vp.max_depth = 1.0f;
    SDL_SetGPUViewport(rp, &vp);
    u[0] = p->gamut; u[1] = p->warm; u[2] = p->boost; u[3] = p->headroom;
    SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof u);
    b.texture = src; b.sampler = S.smp;
    SDL_BindGPUFragmentSamplers(rp, 0, &b, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
    return 1;
}

/* The scene pass: src -> dst (same size, fmt), colours moved back by `amount` (0 = unchanged, 1 = out_ps's gamut matrix
 * fully undone). Both textures must be GPU-side targets/samplers; no render pass open. */
int gpu_out_scene_comp(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, SDL_GPUTexture *dst,
                       SDL_GPUTextureFormat fmt, float amount)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUTextureSamplerBinding b;
    SDL_GPURenderPass *rp;
    SDL_GPUGraphicsPipeline *pl;
    float u[4] = { amount, 0, 0, 0 };
    GpuOutParams dummy = { 0, 0, 0, 1 };
    SDL_Rect none = { 0, 0, 1, 1 };
    if (S.failed || !dev || !src || !dst) return 0;
    if (!S.vs) {   /* build the shaders and the sampler through a no-op first use of the module */
        (void)dummy; (void)none;
        {   char *code = gpu_out_source();
            SDL_GPUShaderCreateInfo ci; SDL_GPUSamplerCreateInfo si;
            if (!code) { S.failed = 1; return 0; }
            memset(&ci, 0, sizeof ci);
            ci.code = (const Uint8 *)code; ci.code_size = sizeof k_msl; ci.format = SDL_GPU_SHADERFORMAT_MSL;
            ci.entrypoint = "out_vs"; ci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
            S.vs = SDL_CreateGPUShader(dev, &ci);
            ci.entrypoint = "out_ps"; ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; ci.num_samplers = 1; ci.num_uniform_buffers = 1;
            S.ps = SDL_CreateGPUShader(dev, &ci);
            ci.entrypoint = "comp_ps";
            S.ps_comp = SDL_CreateGPUShader(dev, &ci);
            ci.entrypoint = "up_ps";
            S.ps_up = SDL_CreateGPUShader(dev, &ci);
            free(code);
            memset(&si, 0, sizeof si);
            si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
            si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            S.smp = SDL_CreateGPUSampler(dev, &si);
            if (!S.vs || !S.ps || !S.ps_comp || !S.ps_up || !S.smp) { fprintf(stderr, "[GPU] colour pass shaders failed: %s\n", SDL_GetError()); S.failed = 1; return 0; }
        }
    }
    pl = pipe_for(dev, fmt, 1);
    if (!pl) { S.failed = 1; return 0; }
    memset(&ct, 0, sizeof ct);
    ct.texture = dst; ct.load_op = SDL_GPU_LOADOP_DONT_CARE; ct.store_op = SDL_GPU_STOREOP_STORE;
    rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(rp, pl);
    SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof u);
    b.texture = src; b.sampler = S.smp;
    SDL_BindGPUFragmentSamplers(rp, 0, &b, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
    return 1;
}

/* The SDR upscaler (see up_ps): `src` (src_w x src_h, gamma encoded) into the rectangle `dest` of `dst` (rest cleared to black), bicubic with
 * `sharpness` (0 = none .. 1). No render pass may be open. Returns 1 if drawn (0: the caller falls back to a plain blit). */
int gpu_out_upscale(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, int src_w, int src_h, SDL_GPUTexture *dst,
                    SDL_GPUTextureFormat dst_fmt, const SDL_Rect *dest, float sharpness, int edge)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUTextureSamplerBinding b;
    SDL_GPURenderPass *rp;
    SDL_GPUViewport vp;
    SDL_GPUGraphicsPipeline *pl;
    float u[4];
    if (S.failed || !dev || !src || !dst || src_w <= 0 || src_h <= 0) return 0;
    if (!ensure_shared(dev)) return 0;
    pl = pipe_for(dev, dst_fmt, 2);
    if (!pl) return 0;
    memset(&ct, 0, sizeof ct);
    ct.texture = dst; ct.load_op = SDL_GPU_LOADOP_CLEAR; ct.store_op = SDL_GPU_STOREOP_STORE;
    rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(rp, pl);
    vp.x = (float)dest->x; vp.y = (float)dest->y; vp.w = (float)dest->w; vp.h = (float)dest->h; vp.min_depth = 0.0f; vp.max_depth = 1.0f;
    SDL_SetGPUViewport(rp, &vp);
    u[0] = (float)src_w; u[1] = (float)src_h; u[2] = sharpness; u[3] = edge ? 1.0f : 0.0f;
    SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof u);
    b.texture = src; b.sampler = S.smp;
    SDL_BindGPUFragmentSamplers(rp, 0, &b, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
    return 1;
}
