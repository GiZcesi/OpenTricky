/*
 * d3d8_post -- end-of-frame post-processing chain (fork).
 *
 * The rendered frame (scene_tex, already resolved when anti-aliased) runs
 * through an ordered list of full-screen passes before host_present copies or
 * scales it into the window. Each pass reads the previous pass's output and
 * writes one of two ping-pong render targets at the render resolution, so:
 *
 *   - the post never writes scene_tex: prev_frame_update (the "Master" outfit
 *     chrome reads the previous frame) keeps the title's own image; frame
 *     dumps, hashes and screenshots read what is shown (shown_texture_get in
 *     d3d8_device.c), so an A/B bench of XBOX_POST sees the post's output;
 *   - the letterbox and the scaling of the present are untouched: the post
 *     works at render size and the present treats its output exactly as it
 *     treated scene_tex (1:1 copy or blit_draw);
 *   - video frames (the guest framebuffer) skip the chain.
 *
 * The passes (s_passes in d3d8_post.c), in order:
 *   - identity: a texel copy through a shader, which must leave the image
 *     bit-identical -- the chain's self-check, active with XBOX_POST=1 alone;
 *   - SMAA 1x: edge detection (luma) -> blending weights (AreaTex,
 *     SearchTex) -> neighbourhood blending, the reference implementation in
 *     smaa/. Active with XBOX_SMAA=1, preset XBOX_SMAA_PRESET=low|medium|
 *     high|ultra (default high). It reads the resolved image, so with MSAA on
 *     both apply: MSAA first (at render time), SMAA on the resolved result,
 *     which finds fewer edges left to treat.
 * A pass has its own VS/PS, up to three inputs (t0..t2: the current colour,
 * an auxiliary target or a lookup texture), and writes either the colour
 * ping-pong or an auxiliary target (optionally cleared first); b0 holds
 * { width, height, 1/width, 1/height }, s0 is linear clamp, s1 point clamp.
 * To add an effect: an entry in s_passes, its shaders, and a rule in
 * pass_active().
 *
 * Toggles (environment, read once, or the setters below -- the entries the
 * launcher / window menu will use): XBOX_POST=1, XBOX_SMAA=1. Both off -- the
 * default -- the present path is exactly the one it was: d3d8_post_run is
 * not called.
 *
 * Split 3D / overlay (XBOX_POST_SPLIT=1; ON BY DEFAULT WITH XBOX_SMAA=1 --
 * an explicit XBOX_POST_SPLIT=0 turns it off): the chain then treats the 3D
 * only. Order of one presented image:
 *     3D (groups 0-6) -> FRAME_END marker -> POST -> overlay -> present
 *   - at FRAME_END (the pass_tags marker, read by the translator on the pump
 *     thread; XBOX_POST_SPLIT turns XBOX_PASS_TAGS=emit on by itself), the
 *     scene target holds exactly the 3D of the image: it is resolved, run
 *     through the chain, and the result is written back into the render
 *     target (a copy, or a full-screen draw into every MSAA sample);
 *   - the overlay -- groups >= 7, perspective (FogVolume mist, lens flare)
 *     then orthographic (HUD, menus) -- is then drawn over it by the title,
 *     with its own blending, untouched by the post;
 *   - the present shows the scene target as it is, without running the
 *     chain again.
 *   An image whose FRAME_END was not received (frames of the second render
 *   thread during loads, no markers) falls back to the post at the present,
 *   on the whole image, exactly as without the split. Never twice per image.
 *   Consequence: prev_frame_update (the "Master" chrome reads the previous
 *   image) then copies a 3D that went through the post.
 * XBOX_POST_CYCLE=N (diagnostic): every N presents, the next of the modes
 * no post / SMAA whole image / SMAA split -- the same paused frame in all
 * three, for exact comparisons.
 *
 * XBOX_POST_SELFTEST=1 checks the identity in the same run: every 60th
 * post-processed frame (XBOX_POST_SELFTEST_N samples, default 30) the input
 * and the output are read back and compared byte for byte, and the count of
 * differing pixels is printed as "[POST] selftest ...".
 *
 * XBOX_POST_TIMING=1 prints the GPU time of the chain (timestamp queries),
 * "[POST] gpu <mean> ms mean, <max> ms max over 300 frames".
 *
 * Pump thread only, like everything that touches the D3D11 device.
 */
#ifndef D3D8_POST_H
#define D3D8_POST_H

#ifdef _WIN32
#include <d3d11.h>

/* 1 if the chain should run (XBOX_POST, XBOX_SMAA or the setters). */
int d3d8_post_enabled(void);
void d3d8_SetPostProcess(int on);
int  d3d8_GetPostProcess(void);
/* SMAA preset: 0 off, 1 low, 2 medium, 3 high, 4 ultra (next present). */
void d3d8_SetSmaa(int preset);
int  d3d8_GetSmaa(void);

/* Run the chain on src (w x h, the render size). frame_id identifies the
 * frame: a second call with the same id returns the cached result without
 * drawing again (a capture and the present can both ask for one frame).
 * Returns 1 and the chain's output (owned by the module, not AddRef'd), or 0
 * when the chain is off, empty or failed -- the caller then uses src as is.
 * The D3D11 pipeline state is saved and restored around the passes. */
int d3d8_post_run(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                  ID3D11Texture2D *src_tex, ID3D11ShaderResourceView *src_srv,
                  UINT w, UINT h, unsigned frame_id,
                  ID3D11Texture2D **out_tex, ID3D11ShaderResourceView **out_srv);

/* Split 3D / overlay. active = split on and a post enabled;
 * wanted = split or the diagnostic cycle asked for (registers the phase
 * callback). d3d8_post_cycle: mode of this present (0 none, 1 SMAA,
 * 2 SMAA split), -1 without XBOX_POST_CYCLE. */
void d3d8_SetPostSplit(int on);
int  d3d8_GetPostSplit(void);
int  d3d8_post_split_active(void);
int  d3d8_post_split_wanted(void);
int  d3d8_post_cycle(unsigned present_no);

/* Write src (w x h) into rtv, every sample of a multisampled target, by the
 * identity pass (exact, alpha kept). State saved and restored. */
int d3d8_post_copy_into(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                        ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *src,
                        UINT w, UINT h);

/* Free the render targets and shaders (device teardown). */
void d3d8_post_release(void);
#endif

#endif /* D3D8_POST_H */
