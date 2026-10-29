/*
 * d3d8_haze_halo -- a light effect of the renderer that this source tree does
 * not hold: the device still calls these, and each one (d3d8_extras_off.c)
 * answers "off", so nothing is drawn.
 */
#ifndef D3D8_HAZE_HALO_H
#define D3D8_HAZE_HALO_H

#ifdef _WIN32
#include <d3d11.h>

int  d3d8_haze_halo_composite(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                              ID3D11ShaderResourceView *lights, ID3D11ShaderResourceView *halos,
                              ID3D11ShaderResourceView *ez, ID3D11ShaderResourceView *depth,
                              ID3D11RenderTargetView *rtv, UINT lw, UINT lh, UINT dw, UINT dh);
void d3d8_haze_halo_release(void);

#endif /* _WIN32 */
#endif /* D3D8_HAZE_HALO_H */
