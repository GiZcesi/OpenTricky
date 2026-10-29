/*
 * d3d8_extras_off.c -- the D3D side of the extras when they are not built
 * (port/extras.cmake, off by default): the distance veil and
 * ambient haze (d3d8_veil.h), the lights in the haze (d3d8_haze_halo.h), the
 * speed blur and vignette (d3d8_speedfx.h), the bloom (d3d8_bloom.h) and the
 * lights' emission targets it reads (d3d8_lights.h).
 *
 * The rest of the layer still calls them at a few places (the device, the
 * post chain, the translator, the game's start). Here
 * each of those calls does nothing and answers what the real one answers
 * with its effect off -- 0: not wanted, not on, nothing drawn -- so the
 * image is the one of the effects turned off, bit for bit. With the extras
 * the real sources are built instead and this file is empty.
 */
#include "d3d8_veil.h"
#include "d3d8_haze_halo.h"
#include "d3d8_speedfx.h"
#include "d3d8_bloom.h"
#include "d3d8_lights.h"

/* The translator's light marks (d3d8_xbox.h): never set without the bloom. */
int g_d3d8_light_mark;
unsigned long long g_d3d8_light_tex_key;

/* ── distance veil, ambient haze ── */
int  d3d8_veil_wanted(void) { return 0; }
int  d3d8_veil_on(void) { return 0; }
int  d3d8_veil_amb_on(void) { return 0; }
int  d3d8_veil_probe(void) { return 0; }
int  d3d8_veil_wants_bg(void) { return 0; }
int  d3d8_veil_snapshot(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv)
{ (void)dev; (void)ctx; (void)rtv; return 0; }
int  d3d8_veil_terrain_depth(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *depth)
{ (void)dev; (void)ctx; (void)depth; return 0; }
void d3d8_veil_prepare(ID3D11Device *dev) { (void)dev; }
void d3d8_veil_pretargets(ID3D11Device *dev, ID3D11RenderTargetView *rtv) { (void)dev; (void)rtv; }
int  d3d8_veil_run(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                   ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *depth,
                   UINT w, UINT h, UINT msaa, const d3d8_veil_frame *f)
{ (void)dev; (void)ctx; (void)rtv; (void)depth; (void)w; (void)h; (void)msaa; (void)f; return 0; }
void d3d8_VeilSetCullFn(int (*fn)(float *out)) { (void)fn; }
int  d3d8_VeilCull(d3d8_veil_cull *out) { (void)out; return 0; }

/* ── lights in the haze ── */
int  d3d8_haze_halo_composite(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                              ID3D11ShaderResourceView *lights, ID3D11ShaderResourceView *halos,
                              ID3D11ShaderResourceView *ez, ID3D11ShaderResourceView *depth,
                              ID3D11RenderTargetView *rtv, UINT lw, UINT lh, UINT dw, UINT dh)
{
    (void)dev; (void)ctx; (void)lights; (void)halos; (void)ez; (void)depth; (void)rtv;
    (void)lw; (void)lh; (void)dw; (void)dh;
    return 0;
}
void d3d8_haze_halo_release(void) {}

/* ── speed blur, vignette ── */
void d3d8_vignette_image_done(void) {}
int  d3d8_speedfx_wanted(void) { return 0; }
int  d3d8_speedfx_active(void) { return 0; }
void d3d8_speedfx_set_tag(unsigned tag) { (void)tag; }
int  d3d8_speedfx_run(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                      ID3D11ShaderResourceView *src, UINT w, UINT h,
                      ID3D11RenderTargetView *dst)
{ (void)dev; (void)ctx; (void)src; (void)w; (void)h; (void)dst; return 0; }
void d3d8_speedfx_set_depth(ID3D11ShaderResourceView *srv, ID3D11Texture2D *tex) { (void)srv; (void)tex; }
void d3d8_speedfx_release(void) {}

/* ── bloom ── */
int  d3d8_bloom_wanted(void) { return 0; }
int  d3d8_bloom_composite(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                          ID3D11ShaderResourceView *lights, ID3D11ShaderResourceView *halos,
                          ID3D11ShaderResourceView *ez, ID3D11ShaderResourceView *depth,
                          ID3D11RenderTargetView *rtv, UINT w, UINT h)
{ (void)dev; (void)ctx; (void)lights; (void)halos; (void)ez; (void)depth; (void)rtv; (void)w; (void)h; return 0; }
void d3d8_bloom_release(void) {}

/* ── the lights' emission targets (the bloom's source) ── */
int  d3d8_lights_wanted(void) { return 0; }
int  d3d8_lights_active(void) { return 0; }
int  d3d8_lights_pre(ID3D11DeviceContext *ctx) { (void)ctx; return 0; }
int  d3d8_lights_mirror_begin(ID3D11DeviceContext *ctx) { (void)ctx; return 0; }
void d3d8_lights_mirror_end(ID3D11DeviceContext *ctx) { (void)ctx; }
int  d3d8_lights_take(ID3D11DeviceContext *ctx, UINT w, UINT h, ID3D11ShaderResourceView **srv2)
{ (void)ctx; (void)w; (void)h; (void)srv2; return 0; }
int  d3d8_lights_z_begin(ID3D11DeviceContext *ctx) { (void)ctx; return 0; }
void d3d8_lights_z_end(ID3D11DeviceContext *ctx) { (void)ctx; }
int  d3d8_lights_pending(void) { return 0; }
void d3d8_lights_release(void) {}

