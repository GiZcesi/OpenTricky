/*
 * d3d8_speedfx -- a post effect that this source tree does not hold: the
 * device and the post chain still call these, and each one
 * (d3d8_extras_off.c) answers "off", so nothing is drawn.
 */
#ifndef D3D8_SPEEDFX_H
#define D3D8_SPEEDFX_H

#ifdef _WIN32
#include <d3d11.h>

void d3d8_vignette_image_done(void);
int  d3d8_speedfx_wanted(void);
int  d3d8_speedfx_active(void);
void d3d8_speedfx_set_tag(unsigned tag);
int  d3d8_speedfx_run(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                      ID3D11ShaderResourceView *src, UINT w, UINT h,
                      ID3D11RenderTargetView *dst);
void d3d8_speedfx_set_depth(ID3D11ShaderResourceView *srv, ID3D11Texture2D *tex);
void d3d8_speedfx_release(void);

#endif /* _WIN32 */
#endif /* D3D8_SPEEDFX_H */
