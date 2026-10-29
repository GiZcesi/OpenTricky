/*
 * d3d8_rscale -- render scale of the 3D (fork).
 *
 * [Display] RenderScale / XBOX_RENDER_SCALE, in percent of the render size:
 * 50, 67, 75, 85, 100 (the default), 125, 150, 175 or 200. Only the 3D of
 * an image is drawn at that size; the HUD, the menus and the text stay at
 * the render size, as in a modern game.
 *
 * How (d3d8_device.c, "Render scale"): in a race the title marks its passes
 * (pass_tags, XBOX_PASS_TAGS=emit, turned on by a scale other than 100). The
 * 3D of the image being built -- every draw from the flip up to the first
 * orthographic group (>= 7, the HUD) -- goes to a second scene target of the
 * scaled size, with its own depth. At that first HUD group the device brings
 * the 3D to the render size here (d3d8_rscale_resample, then the depth by
 * d3d8_rscale_depth_copy) and hands the title the render-size target for the
 * rest of the image. An image without markers (menus, loading screens,
 * videos) is drawn at the render size from start to end.
 *
 * Filters, never sharpened (no negative lobe anywhere):
 *   - above 100 %: each output pixel is the average of the scene texels under
 *     it, each weighted by the area it covers (a box filter; at 2:1 exactly
 *     the mean of 2x2, any ratio up to 3:1 handled);
 *   - below 100 %: the hardware's bilinear filter (weights 0..1).
 * Both average the stored values, as the hardware's MSAA resolve does.
 *
 * At 100 nothing in this file runs and no target is made: the image is the
 * title's, bit for bit, as without the setting.
 */
#ifndef D3D8_RSCALE_H
#define D3D8_RSCALE_H

#include <d3d11.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XBOX_RENDER_SCALE read once and snapped to the nearest step (50..200);
 * 100 when unset or unreadable. */
int  d3d8_rscale_wanted(void);

/* Live change (diagnostics port, picture captures): the scale snapped to a
 * step; the renderer applies it between two images (generation changes). */
void d3d8_rscale_set(int pct);
int  d3d8_rscale_generation(void);

/* The steps, ascending (n written to *count). */
const int *d3d8_rscale_steps(int *count);

/* Compiles the shaders (once). 0 = unavailable: the caller keeps 100 %. */
int  d3d8_rscale_init(ID3D11Device *dev);

/* src (sw x sh, single-sampled colour) into dst (dw x dh, which may be
 * multisampled: every sample gets the value), all four channels -- alpha
 * kept for a later destination-alpha blend. Box when the source is larger,
 * bilinear when smaller. State saved and restored. */
int  d3d8_rscale_resample(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                          ID3D11ShaderResourceView *src, UINT sw, UINT sh,
                          ID3D11RenderTargetView *dst, UINT dw, UINT dh);

/* The depth of the scaled 3D into dsv (dw x dh): the nearest source sample
 * (sample 0 when src_ms), written whole (depth test always, write on); the
 * stencil of dsv is cleared to 0 first -- a shader cannot write it.
 * src: R24_UNORM_X8_TYPELESS view. State saved and restored. */
int  d3d8_rscale_depth_copy(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                            ID3D11ShaderResourceView *src, int src_ms, UINT sw, UINT sh,
                            ID3D11DepthStencilView *dsv, UINT dw, UINT dh);

void d3d8_rscale_release(void);

#ifdef __cplusplus
}
#endif
#endif
