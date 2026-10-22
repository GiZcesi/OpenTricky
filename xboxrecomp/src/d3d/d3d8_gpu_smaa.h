/* d3d8_gpu_smaa.h -- SMAA 1x for the macOS GPU backend (SDL3 GPU API, Metal). See d3d8_gpu_smaa.c. */
#ifndef D3D8_GPU_SMAA_H
#define D3D8_GPU_SMAA_H
#include <SDL3/SDL.h>

/* The MSL source for a render size and preset (1 low, 2 medium, 3 high, 4 ultra): the porting prefix, the converted
 * reference (d3d8_gpu_smaa_msl.c) and the entry points. malloc'd; the caller frees it. */
char *gpu_smaa_source(unsigned w, unsigned h, int preset);

/* Runs edge detection, blending weights and neighbourhood blending on `color` (w x h, format fmt, must allow sampling)
 * inside `cmd`. Returns the result texture (same size and format, owned by the module, valid until the next call) or
 * NULL if SMAA is unavailable (a shader failed: said once on stderr). No render pass may be open on `cmd`. */
SDL_GPUTexture *gpu_smaa_run(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *color,
                             unsigned w, unsigned h, SDL_GPUTextureFormat fmt, int preset);

#endif
