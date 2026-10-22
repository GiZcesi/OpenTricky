/* Numeric check of the HDR / wide-gamut colour pass: known colours through the shader into an RGBA16F target, read back.
 *   cc -I/opt/homebrew/include -I../../xboxrecomp/src/d3d hdr_test.c ../../xboxrecomp/src/d3d/d3d8_gpu_out.c \
 *      -L/opt/homebrew/lib -lSDL3 -o hdr_test && ./hdr_test
 * Exit status 0 = every check held. No window needed. */
#include "d3d8_gpu_out.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name; unsigned char r, g, b; } Patch;
static float half_to_float(unsigned short h)
{
    unsigned s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m + 1024), (int)e - 25);
    return s ? -v : v;
}

int main(void)
{
    static const Patch P[] = { { "white", 255, 255, 255 }, { "grey", 128, 128, 128 }, { "black", 0, 0, 0 }, { "orange", 255, 128, 0 },
                               { "yellow", 255, 220, 0 }, { "red", 255, 0, 0 }, { "green", 0, 255, 0 }, { "blue", 0, 0, 255 },
                               { "dim orange", 128, 64, 0 }, { "sky", 80, 140, 220 } };
    enum { N = sizeof P / sizeof P[0], W = 4 };
    SDL_GPUDevice *dev;
    SDL_GPUTexture *src, *dst;
    SDL_GPUTextureCreateInfo ti;
    SDL_GPUTransferBufferCreateInfo bi;
    SDL_GPUTransferBuffer *up, *down;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *cp;
    SDL_GPUTextureTransferInfo ts;
    SDL_GPUTextureRegion tr;
    SDL_GPUFence *fence;
    unsigned char *px;
    unsigned short *out;
    GpuOutParams p;
    SDL_Rect all;
    float res[N][3], head = 1.2f;
    int i, bad = 0, mode;

    if (!SDL_Init(SDL_INIT_VIDEO)) return 2;
    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, NULL);
    if (!dev) { fprintf(stderr, "no device\n"); return 2; }
    memset(&ti, 0, sizeof ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ti.width = N * W; ti.height = 1;
    ti.layer_count_or_depth = 1; ti.num_levels = 1; ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    src = SDL_CreateGPUTexture(dev, &ti);
    ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    dst = SDL_CreateGPUTexture(dev, &ti);
    memset(&bi, 0, sizeof bi); bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; bi.size = N * W * 4;
    up = SDL_CreateGPUTransferBuffer(dev, &bi);
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; bi.size = N * W * 8;
    down = SDL_CreateGPUTransferBuffer(dev, &bi);
    px = (unsigned char *)SDL_MapGPUTransferBuffer(dev, up, false);
    for (i = 0; i < N * W; i++) { px[i * 4] = P[i / W].r; px[i * 4 + 1] = P[i / W].g; px[i * 4 + 2] = P[i / W].b; px[i * 4 + 3] = 255; }
    SDL_UnmapGPUTransferBuffer(dev, up);
    all.x = 0; all.y = 0; all.w = N * W; all.h = 1;

    for (mode = 0; mode < 3; mode++) {
        /* mode 0: everything off (must reproduce linear light); 1: gamut only; 2: the defaults */
        p.gamut = mode >= 1 ? 1.0f : 0.0f; p.warm = mode == 2 ? 0.2f : 0.0f; p.boost = mode == 2 ? 1.0f : 0.0f; p.headroom = head;
        cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (mode == 0) {
            cp = SDL_BeginGPUCopyPass(cmd);
            memset(&ts, 0, sizeof ts); memset(&tr, 0, sizeof tr);
            ts.transfer_buffer = up; ts.pixels_per_row = N * W; ts.rows_per_layer = 1;
            tr.texture = src; tr.w = N * W; tr.h = 1; tr.d = 1;
            SDL_UploadToGPUTexture(cp, &ts, &tr, false);
            SDL_EndGPUCopyPass(cp);
        }
        if (!gpu_out_draw(dev, cmd, src, dst, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, &all, &p)) { fprintf(stderr, "draw failed\n"); return 2; }
        cp = SDL_BeginGPUCopyPass(cmd);
        memset(&ts, 0, sizeof ts); memset(&tr, 0, sizeof tr);
        ts.transfer_buffer = down; ts.pixels_per_row = N * W; ts.rows_per_layer = 1;
        tr.texture = dst; tr.w = N * W; tr.h = 1; tr.d = 1;
        SDL_DownloadFromGPUTexture(cp, &tr, &ts);
        SDL_EndGPUCopyPass(cp);
        fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(dev, true, &fence, 1); SDL_ReleaseGPUFence(dev, fence);
        out = (unsigned short *)SDL_MapGPUTransferBuffer(dev, down, false);
        for (i = 0; i < N; i++) {
            int k = i * W + 2;   /* a texel well inside the patch */
            res[i][0] = half_to_float(out[k * 4]); res[i][1] = half_to_float(out[k * 4 + 1]); res[i][2] = half_to_float(out[k * 4 + 2]);
        }
        SDL_UnmapGPUTransferBuffer(dev, down);
        printf("mode %d (%s)\n", mode, mode == 0 ? "plain: linear light" : mode == 1 ? "gamut" : "gamut + warm + boost, headroom 1.2");
        for (i = 0; i < N; i++) printf("  %-11s %3d %3d %3d -> %8.4f %8.4f %8.4f\n", P[i].name, P[i].r, P[i].g, P[i].b, res[i][0], res[i][1], res[i][2]);
        if (mode == 0) {   /* sRGB 128 -> 0.2158 linear, 255 -> 1 */
            if (!(fabsf(res[0][0] - 1.0f) <= 0.002f) || !(fabsf(res[1][0] - 0.2158f) <= 0.002f) || !(res[2][0] <= 0.0005f)) { bad++; printf("  FAIL: plain decode\n"); }
        }
        if (mode >= 1) {
            if (!(fabsf(res[0][0] - 1.0f) <= 0.002f) || !(fabsf(res[0][1] - 1.0f) <= 0.002f) || !(fabsf(res[0][2] - 1.0f) <= 0.002f)) { bad++; printf("  FAIL: white moved\n"); }
            if (!(fabsf(res[1][0] - res[1][2]) <= 0.002f)) { bad++; printf("  FAIL: grey not neutral\n"); }
            if (!(res[3][2] < 0.0f && res[3][0] > 1.0f)) { bad++; printf("  FAIL: orange not outside sRGB (expected R > 1, B < 0)\n"); }
        }
        if (mode == 2) {
            if (!(fabsf(res[0][0] - 1.0f) <= 0.002f)) { bad++; printf("  FAIL: white boosted\n"); }
            if (!(fabsf(res[1][0] - res[1][1]) <= 0.002f)) { bad++; printf("  FAIL: grey changed\n"); }
            if (res[3][0] > 1.0f * 1.2f * 1.5f) { bad++; printf("  FAIL: orange above the allowed headroom\n"); }
            for (i = 0; i < N; i++) {   /* every colour inside Display P3 (its coordinates >= 0), before the boost scales them */
                float r = res[i][0], g = res[i][1], b = res[i][2];
                float pr = 0.8224621f * r + 0.1775380f * g, pg = 0.0331941f * r + 0.9668058f * g, pb = 0.0170827f * r + 0.0723974f * g + 0.9105199f * b;
                if (pr < -0.001f || pg < -0.001f || pb < -0.001f) { bad++; printf("  FAIL: %s outside Display P3 (%.3f %.3f %.3f)\n", P[i].name, pr, pg, pb); }
            }
        }
    }
    {   /* the scene pass: comp(amount = 1 - k) then the gamut matrix must equal the gamut matrix at strength k */
        SDL_GPUTexture *mid;
        float direct[N][3];
        const float k = 0.45f;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        mid = SDL_CreateGPUTexture(dev, &ti);
        for (mode = 0; mode < 2; mode++) {
            cmd = SDL_AcquireGPUCommandBuffer(dev);
            if (mode == 0) {   /* direct: gamut strength k on the original */
                p.gamut = k; p.warm = 0; p.boost = 0; p.headroom = 1.0f;
                gpu_out_draw(dev, cmd, src, dst, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, &all, &p);
            } else {           /* scene pass, then the full matrix */
                p.gamut = 1.0f; p.warm = 0; p.boost = 0; p.headroom = 1.0f;
                gpu_out_scene_comp(dev, cmd, src, mid, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1.0f - k);
                gpu_out_draw(dev, cmd, mid, dst, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, &all, &p);
            }
            cp = SDL_BeginGPUCopyPass(cmd);
            memset(&ts, 0, sizeof ts); memset(&tr, 0, sizeof tr);
            ts.transfer_buffer = down; ts.pixels_per_row = N * W; ts.rows_per_layer = 1;
            tr.texture = dst; tr.w = N * W; tr.h = 1; tr.d = 1;
            SDL_DownloadFromGPUTexture(cp, &tr, &ts);
            SDL_EndGPUCopyPass(cp);
            fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            SDL_WaitForGPUFences(dev, true, &fence, 1); SDL_ReleaseGPUFence(dev, fence);
            out = (unsigned short *)SDL_MapGPUTransferBuffer(dev, down, false);
            for (i = 0; i < N; i++) {
                int kk = i * W + 2, c;
                for (c = 0; c < 3; c++) {
                    float v = half_to_float(out[kk * 4 + c]);
                    if (mode == 0) direct[i][c] = v; else res[i][c] = v;
                }
            }
            SDL_UnmapGPUTransferBuffer(dev, down);
        }
        printf("scene pass (net gamut strength %.2f), via comp + matrix vs direct\n", k);
        for (i = 0; i < N; i++) {
            float d = fmaxf(fabsf(res[i][0] - direct[i][0]), fmaxf(fabsf(res[i][1] - direct[i][1]), fabsf(res[i][2] - direct[i][2])));
            printf("  %-11s via %7.4f %7.4f %7.4f  direct %7.4f %7.4f %7.4f  max diff %.4f\n", P[i].name, res[i][0], res[i][1], res[i][2],
                   direct[i][0], direct[i][1], direct[i][2], d);
            if (!(d <= 0.012f)) { bad++; printf("  FAIL: scene pass does not compose to the direct result\n"); }
        }
    }
    {   /* highlight gradations: a ramp of shades of a saturated colour, 0.6 .. 1.0 of full, through the default settings must keep
         * its steps apart (a hard clip at the display's headroom merged the brightest ones) */
        enum { R = 16, FAM = 4 };
        static const unsigned char fam[FAM][3] = { { 255, 128, 0 }, { 255, 220, 0 }, { 255, 40, 20 }, { 240, 200, 120 } };
        SDL_GPUTexture *rs, *rd;
        SDL_GPUTransferBuffer *rup, *rdown;
        int f, k;
        float py[FAM][R], base[FAM][R];
        ti.type = SDL_GPU_TEXTURETYPE_2D; ti.width = R * FAM; ti.height = 1;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; rs = SDL_CreateGPUTexture(dev, &ti);
        ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER; rd = SDL_CreateGPUTexture(dev, &ti);
        memset(&bi, 0, sizeof bi); bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; bi.size = R * FAM * 4; rup = SDL_CreateGPUTransferBuffer(dev, &bi);
        bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; bi.size = R * FAM * 8; rdown = SDL_CreateGPUTransferBuffer(dev, &bi);
        px = (unsigned char *)SDL_MapGPUTransferBuffer(dev, rup, false);
        for (f = 0; f < FAM; f++)
            for (k = 0; k < R; k++) {
                float sh = 0.6f + 0.4f * (float)k / (float)(R - 1);
                unsigned char *q = px + (f * R + k) * 4;
                q[0] = (unsigned char)(fam[f][0] * sh + 0.5f); q[1] = (unsigned char)(fam[f][1] * sh + 0.5f); q[2] = (unsigned char)(fam[f][2] * sh + 0.5f); q[3] = 255;
            }
        SDL_UnmapGPUTransferBuffer(dev, rup);
        all.x = 0; all.y = 0; all.w = R * FAM; all.h = 1;
        for (mode = 0; mode < 2; mode++) {   /* 0: plain linear light, 1: the defaults of the game (see d3d8_gpu.c hdr_params) */
            p.gamut = mode ? 1.0f : 0.0f; p.warm = mode ? 0.1f : 0.0f; p.boost = mode ? 0.35f : 0.0f; p.headroom = 1.5f;   /* the game caps the headroom it uses at OT_HDR_PEAK (1.5) */
            cmd = SDL_AcquireGPUCommandBuffer(dev);
            if (mode == 0) {
                cp = SDL_BeginGPUCopyPass(cmd);
                memset(&ts, 0, sizeof ts); memset(&tr, 0, sizeof tr);
                ts.transfer_buffer = rup; ts.pixels_per_row = R * FAM; ts.rows_per_layer = 1;
                tr.texture = rs; tr.w = R * FAM; tr.h = 1; tr.d = 1;
                SDL_UploadToGPUTexture(cp, &ts, &tr, false);
                SDL_EndGPUCopyPass(cp);
            }
            gpu_out_draw(dev, cmd, rs, rd, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, &all, &p);
            cp = SDL_BeginGPUCopyPass(cmd);
            memset(&ts, 0, sizeof ts); memset(&tr, 0, sizeof tr);
            ts.transfer_buffer = rdown; ts.pixels_per_row = R * FAM; ts.rows_per_layer = 1;
            tr.texture = rd; tr.w = R * FAM; tr.h = 1; tr.d = 1;
            SDL_DownloadFromGPUTexture(cp, &tr, &ts);
            SDL_EndGPUCopyPass(cp);
            fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            SDL_WaitForGPUFences(dev, true, &fence, 1); SDL_ReleaseGPUFence(dev, fence);
            out = (unsigned short *)SDL_MapGPUTransferBuffer(dev, rdown, false);
            for (f = 0; f < FAM; f++)
                for (k = 0; k < R; k++) {
                    int o = (f * R + k) * 4;   /* the channel that is largest in the source, as displayed: luma of the output */
                    float y = 0.2126f * half_to_float(out[o]) + 0.7152f * half_to_float(out[o + 1]) + 0.0722f * half_to_float(out[o + 2]);
                    float mx = fmaxf(half_to_float(out[o]), fmaxf(half_to_float(out[o + 1]), half_to_float(out[o + 2])));
                    if (mode == 0) base[f][k] = mx; else py[f][k] = mx;
                    (void)y;
                }
            SDL_UnmapGPUTransferBuffer(dev, rdown);
        }
        printf("highlight ramps (largest channel; plain -> defaults), steps of 1/15 of the range\n");
        for (f = 0; f < FAM; f++) {
            float minstep = 1e9f, basestep = 1e9f;
            for (k = 1; k < R; k++) { minstep = fminf(minstep, py[f][k] - py[f][k - 1]); basestep = fminf(basestep, base[f][k] - base[f][k - 1]); }
            printf("  family %d: top %.3f -> %.3f   smallest step %.4f (plain %.4f)\n", f, base[f][R - 1], py[f][R - 1], minstep, basestep);
            if (!(minstep > 0.0f)) { bad++; printf("  FAIL: two shades merged\n"); }
            if (!(minstep >= 0.30f * basestep)) { bad++; printf("  FAIL: highlight steps squeezed below 30%% of the plain ones\n"); }
        }
    }
    {   /* the headroom clamp the game applies (d3d8_gpu.c hdr_params uses this function) */
        static const float cases[][3] = {   /* reported, peak, expected */
            { 3.59f, 1.5f, 1.5f }, { 1.2f, 1.5f, 1.2f }, { 0.8f, 1.5f, 1.0f }, { 3.0f, 0.5f, 1.0f }, { 3.0f, 0.0f, 1.0f },
            { 3.0f, 1.0f, 1.0f }, { 0.0f, 4.0f, 1.0f }, { 2.0f, 4.0f, 2.0f } };
        int c;
        for (c = 0; c < (int)(sizeof cases / sizeof cases[0]); c++) {
            float g = gpu_out_effective_headroom(cases[c][0], cases[c][1]);
            if (!(fabsf(g - cases[c][2]) <= 1e-6f)) { bad++; printf("  FAIL: headroom(reported %.2f, peak %.2f) = %.3f, expected %.3f\n", cases[c][0], cases[c][1], g, cases[c][2]); }
        }
        if (gpu_out_effective_headroom(NAN, 1.5f) != 1.0f || gpu_out_effective_headroom(2.0f, NAN) != 1.0f) { bad++; printf("  FAIL: NaN headroom or peak\n"); }
        printf("headroom clamp: %d cases checked\n", (int)(sizeof cases / sizeof cases[0]) + 2);
    }
    printf("%s\n", bad ? "FAILED" : "all checks held");
    return bad != 0;
}
