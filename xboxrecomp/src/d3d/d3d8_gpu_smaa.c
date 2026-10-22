/* d3d8_gpu_smaa.c -- SMAA 1x on SDL3's GPU API (the macOS backend): the reference implementation's three passes, edge
 * detection (luma) -> blending weights (AreaTex, SearchTex) -> neighbourhood blending, as in the Windows layer
 * (d3d8_post.c). The shader is the reference SMAA.hlsl converted to Metal Shading Language by smaa/make_msl.py; the
 * porting macros below are those of SMAA_CUSTOM_SL. The render size is compiled in (SMAA_RT_METRICS), so the shaders are
 * rebuilt when the size or preset changes. Everything is non-sRGB: edge detection needs gamma-encoded values. */
#include "d3d8_gpu_smaa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char SMAA_MSL_BODY[];

#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"

static const char *const k_preset[5] = { NULL, "SMAA_PRESET_LOW", "SMAA_PRESET_MEDIUM", "SMAA_PRESET_HIGH", "SMAA_PRESET_ULTRA" };

static const char k_prefix[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#define SMAA_CUSTOM_SL 1\n"
    "constexpr sampler LinearSampler(filter::linear, address::clamp_to_edge);\n"
    "constexpr sampler PointSampler(filter::nearest, address::clamp_to_edge);\n"
    "#define SMAATexture2D(tex) texture2d<float> tex\n"
    "#define SMAATexturePass2D(tex) tex\n"
    "#define SMAASampleLevelZero(tex, coord) tex.sample(LinearSampler, coord, level(0))\n"
    "#define SMAASampleLevelZeroPoint(tex, coord) tex.sample(PointSampler, coord, level(0))\n"
    "#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.sample(LinearSampler, coord, level(0), offset)\n"
    "#define SMAASample(tex, coord) tex.sample(LinearSampler, coord)\n"
    "#define SMAASamplePoint(tex, coord) tex.sample(PointSampler, coord)\n"
    "#define SMAASampleOffset(tex, coord, offset) tex.sample(LinearSampler, coord, offset)\n"
    "#define SMAA_FLATTEN\n"
    "#define SMAA_BRANCH\n"
    "#define mad(a, b, c) fma(a, b, c)\n"
    "#define lerp(a, b, t) mix(a, b, t)\n"
    "#define round(x) rint(x)\n"
    "#define discard discard_fragment()\n";

static const char k_suffix[] =
    "struct VO { float4 pos [[position]]; float2 uv; float2 pix; float4 o0; float4 o1; float4 o2; };\n"
    "static VO fullscreen(uint id) {\n"
    "    VO o; o.uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "    o.pix = float2(0.0); o.o0 = o.o1 = o.o2 = float4(0.0);\n"
    "    return o;\n"
    "}\n"
    "vertex VO smaa_edge_vs(uint id [[vertex_id]]) {\n"
    "    VO o = fullscreen(id); float4 off[3]; SMAAEdgeDetectionVS(o.uv, off); o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2]; return o;\n"
    "}\n"
    "fragment float2 smaa_edge_ps(VO i [[stage_in]], texture2d<float> t0 [[texture(0)]]) {\n"
    "    float4 off[3] = { i.o0, i.o1, i.o2 };\n"
    "    return SMAALumaEdgeDetectionPS(i.uv, off, t0);\n"
    "}\n"
    "vertex VO smaa_weight_vs(uint id [[vertex_id]]) {\n"
    "    VO o = fullscreen(id); float4 off[3]; SMAABlendingWeightCalculationVS(o.uv, o.pix, off); o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2]; return o;\n"
    "}\n"
    "fragment float4 smaa_weight_ps(VO i [[stage_in]], texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]],\n"
    "                               texture2d<float> t2 [[texture(2)]]) {\n"
    "    float4 off[3] = { i.o0, i.o1, i.o2 };\n"
    "    return SMAABlendingWeightCalculationPS(i.uv, i.pix, off, t0, t1, t2, float4(0.0));\n"
    "}\n"
    "vertex VO smaa_blend_vs(uint id [[vertex_id]]) {\n"
    "    VO o = fullscreen(id); float4 off; SMAANeighborhoodBlendingVS(o.uv, off); o.o0 = off; return o;\n"
    "}\n"
    "fragment float4 smaa_blend_ps(VO i [[stage_in]], texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]]) {\n"
    "    return SMAANeighborhoodBlendingPS(i.uv, i.o0, t0, t1);\n"
    "}\n";

char *gpu_smaa_source(unsigned w, unsigned h, int preset)
{
    char head[256];
    size_t n;
    char *s;
    if (preset < 1 || preset > 4) preset = 3;
    snprintf(head, sizeof head, "#define %s 1\n#define SMAA_RT_METRICS float4(%.9g, %.9g, %.1f, %.1f)\n",
             k_preset[preset], 1.0 / (double)w, 1.0 / (double)h, (double)w, (double)h);
    n = strlen(k_prefix) + strlen(head) + strlen(SMAA_MSL_BODY) + strlen(k_suffix) + 1;
    s = (char *)malloc(n);
    if (!s) return NULL;
    snprintf(s, n, "%s%s%s%s", k_prefix, head, SMAA_MSL_BODY, k_suffix);
    return s;
}

static struct {
    int failed, built_preset;
    unsigned w, h;
    SDL_GPUTextureFormat fmt;
    SDL_GPUShader *vs[3], *ps[3];
    SDL_GPUGraphicsPipeline *pipe[3];
    SDL_GPUTexture *edges, *weights, *out, *area, *search;
    SDL_GPUSampler *lin, *pt;
    int tex_uploaded;
} S;

static void release_all(SDL_GPUDevice *dev)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (S.pipe[i]) SDL_ReleaseGPUGraphicsPipeline(dev, S.pipe[i]);
        if (S.vs[i]) SDL_ReleaseGPUShader(dev, S.vs[i]);
        if (S.ps[i]) SDL_ReleaseGPUShader(dev, S.ps[i]);
        S.pipe[i] = NULL; S.vs[i] = S.ps[i] = NULL;
    }
    if (S.edges) SDL_ReleaseGPUTexture(dev, S.edges);
    if (S.weights) SDL_ReleaseGPUTexture(dev, S.weights);
    if (S.out) SDL_ReleaseGPUTexture(dev, S.out);
    S.edges = S.weights = S.out = NULL;
}

static SDL_GPUTexture *make_tex(SDL_GPUDevice *dev, unsigned w, unsigned h, SDL_GPUTextureFormat fmt, int target)
{
    SDL_GPUTextureCreateInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fmt; ti.width = w; ti.height = h;
    ti.layer_count_or_depth = 1; ti.num_levels = 1; ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (target ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
    return SDL_CreateGPUTexture(dev, &ti);
}

static void upload_lookup(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *t, const void *data, unsigned w, unsigned h, unsigned bpp)
{
    SDL_GPUTransferBufferCreateInfo bi; SDL_GPUTransferBuffer *tb; void *map;
    SDL_GPUCopyPass *cp; SDL_GPUTextureTransferInfo src; SDL_GPUTextureRegion dst;
    memset(&bi, 0, sizeof bi); bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; bi.size = w * h * bpp;
    tb = SDL_CreateGPUTransferBuffer(dev, &bi);
    map = tb ? SDL_MapGPUTransferBuffer(dev, tb, false) : NULL;
    if (map) {
        memcpy(map, data, (size_t)w * h * bpp);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        cp = SDL_BeginGPUCopyPass(cmd);
        memset(&src, 0, sizeof src); memset(&dst, 0, sizeof dst);
        src.transfer_buffer = tb; src.pixels_per_row = w; src.rows_per_layer = h;
        dst.texture = t; dst.w = w; dst.h = h; dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
    }
    if (tb) SDL_ReleaseGPUTransferBuffer(dev, tb);
}

static int build(SDL_GPUDevice *dev, unsigned w, unsigned h, SDL_GPUTextureFormat fmt, int preset)
{
    static const char *vs_name[3] = { "smaa_edge_vs", "smaa_weight_vs", "smaa_blend_vs" };
    static const char *ps_name[3] = { "smaa_edge_ps", "smaa_weight_ps", "smaa_blend_ps" };
    static const int nsamp[3] = { 1, 3, 2 };
    SDL_GPUTextureFormat tf[3];
    char *src;
    int i;
    release_all(dev);
    src = gpu_smaa_source(w, h, preset);
    if (!src) return 0;
    tf[0] = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM; tf[1] = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; tf[2] = fmt;
    for (i = 0; i < 3; i++) {
        SDL_GPUShaderCreateInfo ci;
        SDL_GPUGraphicsPipelineCreateInfo pi;
        SDL_GPUColorTargetDescription ct;
        memset(&ci, 0, sizeof ci);
        ci.code = (const Uint8 *)src; ci.code_size = strlen(src) + 1; ci.format = SDL_GPU_SHADERFORMAT_MSL;
        ci.entrypoint = vs_name[i]; ci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        S.vs[i] = SDL_CreateGPUShader(dev, &ci);
        ci.entrypoint = ps_name[i]; ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; ci.num_samplers = (Uint32)nsamp[i];
        S.ps[i] = SDL_CreateGPUShader(dev, &ci);
        if (!S.vs[i] || !S.ps[i]) {
            fprintf(stderr, "[GPU] SMAA shader %d failed: %s\n", i, SDL_GetError());
            if (getenv("OT_GPU_PSH_DUMP_FAILED")) fprintf(stderr, "----\n%s\n----\n", src);
            free(src);
            return 0;
        }
        memset(&pi, 0, sizeof pi); memset(&ct, 0, sizeof ct);
        pi.vertex_shader = S.vs[i]; pi.fragment_shader = S.ps[i];
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        ct.format = tf[i]; ct.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
        ct.blend_state.enable_color_write_mask = true;
        pi.target_info.color_target_descriptions = &ct; pi.target_info.num_color_targets = 1;
        S.pipe[i] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if (!S.pipe[i]) { fprintf(stderr, "[GPU] SMAA pipeline %d failed: %s\n", i, SDL_GetError()); free(src); return 0; }
    }
    free(src);
    S.edges = make_tex(dev, w, h, tf[0], 1);
    S.weights = make_tex(dev, w, h, tf[1], 1);
    S.out = make_tex(dev, w, h, fmt, 1);
    if (!S.edges || !S.weights || !S.out) { fprintf(stderr, "[GPU] SMAA targets failed: %s\n", SDL_GetError()); return 0; }
    S.w = w; S.h = h; S.fmt = fmt; S.built_preset = preset;
    fprintf(stderr, "[GPU] SMAA %s at %ux%u ready\n", k_preset[preset] + 12, w, h);
    return 1;
}

static void pass(SDL_GPUCommandBuffer *cmd, int i, SDL_GPUTexture *target, SDL_GPUTexture *const *in, int nin, int clear)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUTextureSamplerBinding b[3];
    SDL_GPURenderPass *rp;
    int k;
    memset(&ct, 0, sizeof ct);
    ct.texture = target; ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_DONT_CARE;
    rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(rp, S.pipe[i]);
    for (k = 0; k < nin; k++) {
        b[k].texture = in[k];
        b[k].sampler = (in[k] == S.search) ? S.pt : S.lin;
    }
    SDL_BindGPUFragmentSamplers(rp, 0, b, (Uint32)nin);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
}

SDL_GPUTexture *gpu_smaa_run(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *color,
                             unsigned w, unsigned h, SDL_GPUTextureFormat fmt, int preset)
{
    SDL_GPUTexture *in[3];
    if (S.failed || !dev || !color || preset < 1) return NULL;
    if (!S.lin) {
        SDL_GPUSamplerCreateInfo si;
        memset(&si, 0, sizeof si);
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        S.lin = SDL_CreateGPUSampler(dev, &si);
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
        S.pt = SDL_CreateGPUSampler(dev, &si);
        S.area = make_tex(dev, AREATEX_WIDTH, AREATEX_HEIGHT, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, 0);
        S.search = make_tex(dev, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 0);
        if (!S.lin || !S.pt || !S.area || !S.search) { S.failed = 1; fprintf(stderr, "[GPU] SMAA resources failed: %s\n", SDL_GetError()); return NULL; }
    }
    if (!S.tex_uploaded) {
        upload_lookup(dev, cmd, S.area, areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT, 2);
        upload_lookup(dev, cmd, S.search, searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1);
        S.tex_uploaded = 1;
    }
    if (!S.pipe[0] || S.w != w || S.h != h || S.fmt != fmt || S.built_preset != preset) {
        if (!build(dev, w, h, fmt, preset)) { S.failed = 1; release_all(dev); return NULL; }
    }
    in[0] = color;
    pass(cmd, 0, S.edges, in, 1, 1);
    in[0] = S.edges; in[1] = S.area; in[2] = S.search;
    pass(cmd, 1, S.weights, in, 3, 1);
    in[0] = color; in[1] = S.weights;
    pass(cmd, 2, S.out, in, 2, 0);
    return S.out;
}
