/* Compile-only check of the SMAA Metal shaders (all presets): no window needed.
 *   cc -I/opt/homebrew/include -I../../xboxrecomp/src/d3d smaa_compile.c ../../xboxrecomp/src/d3d/d3d8_gpu_smaa.c \
 *      ../../xboxrecomp/src/d3d/d3d8_gpu_smaa_msl.c -L/opt/homebrew/lib -lSDL3 -o smaa_compile && ./smaa_compile
 * Exit status 0 = every entry point of every preset compiled. */
#include "d3d8_gpu_smaa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    static const char *names[6] = { "smaa_edge_vs", "smaa_edge_ps", "smaa_weight_vs", "smaa_weight_ps", "smaa_blend_vs", "smaa_blend_ps" };
    SDL_GPUDevice *dev;
    int p, i, bad = 0;
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 2; }
    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, NULL);
    if (!dev) { fprintf(stderr, "no GPU device: %s\n", SDL_GetError()); return 2; }
    for (p = 1; p <= 4; p++) {
        char *src = gpu_smaa_source(1920, 1080, p);
        for (i = 0; i < 6; i++) {
            SDL_GPUShaderCreateInfo ci; SDL_GPUShader *s;
            memset(&ci, 0, sizeof ci);
            ci.code = (const Uint8 *)src; ci.code_size = strlen(src) + 1; ci.format = SDL_GPU_SHADERFORMAT_MSL;
            ci.entrypoint = names[i]; ci.stage = (i & 1) ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
            ci.num_samplers = (i & 1) ? (i == 3 ? 3 : i == 5 ? 2 : 1) : 0;
            s = SDL_CreateGPUShader(dev, &ci);
            if (!s) { bad++; fprintf(stderr, "preset %d %s: %s\n", p, names[i], SDL_GetError()); }
            else SDL_ReleaseGPUShader(dev, s);
        }
        free(src);
    }
    printf("%s\n", bad ? "FAILED" : "all compiled");
    SDL_DestroyGPUDevice(dev);
    return bad != 0;
}
