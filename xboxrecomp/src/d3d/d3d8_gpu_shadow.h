/* d3d8_gpu_shadow.h -- soft edges for the title's stencil shadows (macOS GPU backend). See d3d8_gpu_shadow.c. */
#ifndef D3D8_GPU_SHADOW_H
#define D3D8_GPU_SHADOW_H
#include <SDL3/SDL.h>

/* Source of the shadow shaders (malloc'd). */
char *gpu_shadow_source(void);

/* The three passes of d3d8_softshadow.h on `rt`, inside `cmd` (no render pass open): mask (the title's stencil test, cmp
 * a D3DCMP_* 1..8, ref, rmask), depth-weighted blur at half size, apply (rt *= lerp(1, shade, mask)) inside the pixel
 * rectangle x0,y0..x1,y1 (exclusive end). `ds` is the depth-stencil texture of the scene (sampled for depth, attached for
 * the stencil test). Returns 1 if it ran. */
int gpu_shadow_run(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *rt, SDL_GPUTextureFormat rt_fmt,
                   SDL_GPUTexture *ds, SDL_GPUTextureFormat ds_fmt, unsigned w, unsigned h,
                   float shade, unsigned cmp, unsigned ref, unsigned rmask, int x0, int y0, int x1, int y1);

#endif
