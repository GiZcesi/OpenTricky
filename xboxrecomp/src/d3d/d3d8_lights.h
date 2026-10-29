/*
 * d3d8_lights -- the lights' own targets of an effect that this source tree
 * does not hold: the device and the translator still call these, and each
 * one (d3d8_extras_off.c) answers "off", so nothing is drawn.
 */
#ifndef D3D8_LIGHTS_H
#define D3D8_LIGHTS_H

#ifdef _WIN32
#include <d3d11.h>

int  d3d8_lights_wanted(void);
int  d3d8_lights_active(void);
int  d3d8_lights_pre(ID3D11DeviceContext *ctx);
int  d3d8_lights_mirror_begin(ID3D11DeviceContext *ctx);
void d3d8_lights_mirror_end(ID3D11DeviceContext *ctx);
int  d3d8_lights_take(ID3D11DeviceContext *ctx, UINT w, UINT h, ID3D11ShaderResourceView **srv2);
int  d3d8_lights_z_begin(ID3D11DeviceContext *ctx);
void d3d8_lights_z_end(ID3D11DeviceContext *ctx);
int  d3d8_lights_pending(void);
void d3d8_lights_release(void);

#endif /* _WIN32 */
#endif /* D3D8_LIGHTS_H */
