/*
 * d3d8_veil -- a distance effect of the renderer that this source tree does
 * not hold: the device still calls these at a few places, and each one
 * (d3d8_extras_off.c) answers "off", so nothing is drawn.
 */
#ifndef D3D8_VEIL_H
#define D3D8_VEIL_H

#include <d3d11.h>
#include <stdint.h>

typedef struct {
    float limit;
    float cell;
    float win[4];
    float radius;
    float tick;
    float track;
} d3d8_veil_cull;

typedef struct {
    float k[16];
    float eye[3];
    float clear_depth;
    float near_z, far_z;
    float fog_rgb[3];
    float fog_rho;
    int   have_world;
    float kw[16];
    float eye_w[3];
    int   have_cull;
    d3d8_veil_cull cull;
} d3d8_veil_frame;

int  d3d8_veil_wanted(void);
int  d3d8_veil_on(void);
int  d3d8_veil_amb_on(void);
int  d3d8_veil_probe(void);
int  d3d8_veil_wants_bg(void);
int  d3d8_veil_snapshot(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv);
int  d3d8_veil_terrain_depth(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *depth);
void d3d8_veil_prepare(ID3D11Device *dev);
void d3d8_veil_pretargets(ID3D11Device *dev, ID3D11RenderTargetView *rtv);
int  d3d8_veil_run(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                   ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *depth,
                   UINT w, UINT h, UINT msaa, const d3d8_veil_frame *f);
void d3d8_VeilSetCullFn(int (*fn)(float *out));
int  d3d8_VeilCull(d3d8_veil_cull *out);

#endif /* D3D8_VEIL_H */
