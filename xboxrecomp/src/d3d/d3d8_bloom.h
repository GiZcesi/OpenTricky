/*
 * d3d8_bloom -- a post effect that this source tree does not hold: the
 * device and the post chain still call these, and each one
 * (d3d8_extras_off.c) answers "off", so nothing is drawn.
 */
#ifndef D3D8_BLOOM_H
#define D3D8_BLOOM_H

#ifdef _WIN32
#include <d3d11.h>

int  d3d8_bloom_wanted(void);
int  d3d8_bloom_composite(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                          ID3D11ShaderResourceView *lights, ID3D11ShaderResourceView *halos,
                          ID3D11ShaderResourceView *ez, ID3D11ShaderResourceView *depth,
                          ID3D11RenderTargetView *rtv, UINT w, UINT h);
void d3d8_bloom_release(void);

#endif /* _WIN32 */
#endif /* D3D8_BLOOM_H */
