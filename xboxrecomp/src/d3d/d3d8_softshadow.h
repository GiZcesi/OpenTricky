/*
 * d3d8_softshadow -- soft edges for the title's stencil shadows (fork).
 *
 * SSX Tricky draws its shadows as stencil shadow volumes ("z-pass": front
 * faces INCR, back faces DECR, colour untouched), then one full-screen quad
 * with the stencil test LEQUAL 1, blend ZERO / SRC_ALPHA and a diffuse alpha of
 * 0.65 darkens every marked pixel at once. The mask is binary, so a
 * shadow edge is always hard, and a rider's own volume shades it along the
 * edges of its polygons (the faceted afro).
 *
 * With XBOX_SOFT_SHADOWS=1 the translator calls d3d8_SoftShadowDraw just
 * before that quad, and the quad is then drawn with its colour writes off
 * (it still zeroes the stencil, as before). Here, at that point of the frame:
 *   1. mask: a full-screen triangle with the quad's own stencil test writes 1
 *      into an R8 target (same size and sample count as the depth buffer);
 *      multisampled, it is resolved -- the mask gets antialiased edges;
 *   2. blur, at half size: two separable passes (horizontal, vertical) whose weights are a
 *      Gaussian of the distance times a depth term (relative depth
 *      difference, ~5 %), so the mask never leaks onto an object in front of
 *      or behind the shadowed surface; radius proportional to the render
 *      height (2.5 px at 480 lines);
 *   3. apply: the scene colour (and alpha, as the quad did) is multiplied by
 *      lerp(1, shade, blurred mask): the inside of a shadow keeps exactly the
 *      title's darkness, the edge fades over a few pixels.
 * Mid-frame, before the overlay: the HUD, the mist and later geometry are
 * drawn afterwards, exactly as with the title's quad. Pipeline state is saved
 * and restored around the passes (the d3d8_post pattern).
 *
 * Off (the default) nothing here runs and the depth buffer is created as
 * before: the image is the title's, bit for bit.
 */
#ifndef D3D8_SOFTSHADOW_H
#define D3D8_SOFTSHADOW_H

#include <d3d11.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XBOX_SOFT_SHADOWS=1, read once. */
int d3d8_softshadow_enabled(void);

/* The three passes. depth is a shader view of the depth buffer bound as dsv
 * (R24_UNORM_X8_TYPELESS, multisampled when msaa > 1). cmp is a
 * D3D11_COMPARISON_FUNC, ref / rmask the stencil reference and read mask of
 * the title's quad, shade its alpha, rect the quad's extent in render-target
 * pixels (the mask and the darkening stay inside it). Returns 1 when the shadow was applied
 * (the caller then keeps the quad from writing colour), 0 to fall back. */
int d3d8_softshadow_run(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                        ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv,
                        ID3D11ShaderResourceView *depth, UINT w, UINT h, UINT msaa,
                        float shade, UINT cmp, UINT ref, UINT rmask, const RECT *rect);

#ifdef __cplusplus
}
#endif
#endif
