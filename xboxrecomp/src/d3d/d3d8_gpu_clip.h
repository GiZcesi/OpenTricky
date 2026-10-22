/* The window-clip scissor of a draw, as pure arithmetic (tested by macos/clip_test). The rectangle is in title pixels with an
 * EXCLUSIVE end (as the D3D11 layer uses it); it is scaled to a target of tw x th (title size TITLE_W x TITLE_H) and clamped to it.
 * Returns 0 if nothing of it is inside the target. */
#ifndef D3D8_GPU_CLIP_H
#define D3D8_GPU_CLIP_H

static inline int gpu_clip_rect(const long sc[4], unsigned tw, unsigned th, unsigned title_w, unsigned title_h,
                                int *rx, int *ry, int *rw, int *rh)
{
    double gsx = (double)tw / (double)title_w, gsy = (double)th / (double)title_h;
    double x0 = (double)sc[0] * gsx, y0 = (double)sc[1] * gsy, x1 = (double)sc[2] * gsx, y1 = (double)sc[3] * gsy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (double)tw) x1 = (double)tw;
    if (y1 > (double)th) y1 = (double)th;
    if (!(x1 > x0) || !(y1 > y0)) return 0;
    *rx = (int)x0; *ry = (int)y0; *rw = (int)x1 - *rx; *rh = (int)y1 - *ry;
    return *rw > 0 && *rh > 0;
}
#endif
