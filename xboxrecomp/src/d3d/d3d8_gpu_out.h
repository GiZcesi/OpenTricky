/* d3d8_gpu_out.h -- the final pass of the macOS GPU backend: the picture to the window (or any target) through a
 * colour shader, for HDR / wide-gamut output. See d3d8_gpu_out.c. */
#ifndef D3D8_GPU_OUT_H
#define D3D8_GPU_OUT_H
#include <SDL3/SDL.h>

typedef struct {
    float gamut;        /* 0..1: how far the picture is read as Display P3 instead of sRGB (more saturated, 0 = off) */
    float warm;         /* extra saturation of oranges and yellows (0 = none, 0.25 = a quarter more chroma at the peak) */
    float boost;        /* 0..1: how much of the display's HDR headroom colourful highlights may use */
    float headroom;     /* the display's HDR headroom above SDR white (1 = none): from the window; capped by the caller */
} GpuOutParams;

/* The headroom the colour pass may use: what the display reports, capped at `peak` (OT_HDR_PEAK), never below 1 (no headroom)
 * whatever the two values are (a peak below 1, zero or NaN, a report of 0 or NaN all give 1). */
float gpu_out_effective_headroom(float reported, float peak);

/* The shader source (malloc'd). */
char *gpu_out_source(void);

/* Draws `src` (any 2D sampled texture, gamma encoded like the title's picture) into `dst` (format dst_fmt) inside the
 * rectangle `dest` (the rest of dst is cleared to black), with the colour transform of `p`. No render pass may be open.
 * Returns 1 if drawn. */
int gpu_out_draw(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, SDL_GPUTexture *dst,
                 SDL_GPUTextureFormat dst_fmt, const SDL_Rect *dest, const GpuOutParams *p);

/* The scene pass: src -> dst (same size and format fmt), colours moved back by `amount`: 0 unchanged, 1 the gamut matrix of
 * gpu_out_draw (gamut = 1) fully undone, so that matrix then gives the picture its original colours; in between, the
 * picture ends with the fraction 1 - amount of the expansion. Used on the 3D alone, so it gets less of it than the HUD. */
int gpu_out_scene_comp(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, SDL_GPUTexture *dst,
                       SDL_GPUTextureFormat fmt, float amount);

/* The SDR upscaler: Catmull-Rom bicubic plus a mild anti-ringing sharpen (0..1), `src` into the rectangle `dest` of `dst`. */
int gpu_out_upscale(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, int src_w, int src_h, SDL_GPUTexture *dst,
                    SDL_GPUTextureFormat dst_fmt, const SDL_Rect *dest, float sharpness, int edge);   /* edge: 1 = edge-adaptive (FSR 1 style), 0 = bicubic */

#endif
