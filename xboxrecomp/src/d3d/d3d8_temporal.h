/*
 * d3d8_temporal -- the inputs of a temporal anti-aliasing / super-resolution (fork).
 *
 * XBOX_TEMPORAL=1 (hidden, read once at start; off by default and with the
 * Original Xbox preset). Off: nothing here runs, nothing is created, no
 * shader text changes -- the image is the title's, bit for bit.
 *
 * On, for each image of a race (an image that follows one carrying pass
 * markers, as the render scale does), this module gives what DLSS / DLAA
 * and FSR 3.1 take as input, and nothing else (no resolve of its own yet):
 *
 *   jitter: the 3D of the image -- from the present to its FRAME_END marker,
 *     groups 0-6 -- is drawn shifted by a sub-pixel offset of the 3D target,
 *     the Halton (2, 3) sequence, 8 phases (8 x (100 / scale)^2 below 100 %,
 *     at most 64). Added in clip space after the title's 1/16 pixel snap, in
 *     the three places that place a vertex on the screen (the vertex
 *     programs, their CPU path's pass-through shader, the point sprites'
 *     geometry shader). Never the overlay (groups >= 7: mist, lens flare,
 *     halos, HUD), drawn after FRAME_END: it is neither jittered nor taken
 *     in. The 2D of the fixed-function path (RwIm2D) is not jittered;
 *
 *   mip bias: while the jitter is on, every sampler of the title gets
 *     log2(scale) - 1 more (the bias both super-resolution passes ask for), or
 *     XBOX_TEMPORAL_MIPBIAS;
 *
 *   motion vectors (RG16F, the 3D target's size): uv of the previous image
 *     minus uv of this one, without jitter (prev = cur + mv, the convention
 *     of both super-resolution passes). The camera's: each pixel taken back into the view
 *     by its depth and this image's camera (the one it was rendered with,
 *     noted at its FRAME_END), then through the previous image's. The
 *     riders' (CPU-skinned meshes, in world space, program A09F1173): each
 *     of their draws is matched with the same draw of the previous image
 *     (same program, textures and vertex count, the nearest), and moves
 *     rigidly by the difference of the two centroids of its world positions;
 *     drawn a second time, same vertex shader, depth EQUAL, writes off, into
 *     an object target that carries that displacement. A rider draw with no
 *     match (a first image, a respawn) gets the camera's vector and a high
 *     reactive value;
 *
 *   reactive mask (R8): the blended draws of the effects groups (default
 *     groups 2 and 6: the board's spray and trail, sparks, pick-ups; their
 *     vertices are CPU-written and have no motion of their own) drawn a
 *     second time, same shaders and blending, depth writes off, into an
 *     accumulation target; the mask is its brightest channel (at most 0.9);
 *
 *   history reset: no camera, a camera cut (more than 60 degrees or 400
 *     units between two images), a multi-view race.
 *
 * At FRAME_END the target switches: with a render scale, the 3D is brought
 * to the output size there (and no longer at the first HUD group), so that
 * everything drawn after FRAME_END is at the output size -- the place where
 * a super-resolution pass will put its output.
 *
 * XBOX_TEMPORAL_VIEW=mv | reactive | object (diagnostic): the motion vectors
 * (colour = direction, brightness = length, full at 8 pixels), the reactive
 * mask (red) or the riders' object target (green matched, magenta not)
 * drawn over the 3D at FRAME_END, after the post, before the overlay.
 * XBOX_TEMPORAL_LOG=1: one line every 300 images.
 *
 * Pump thread only, like everything that touches the D3D11 device.
 */
#ifndef D3D8_TEMPORAL_H
#define D3D8_TEMPORAL_H

#ifdef _WIN32
#include <d3d11.h>

/* XBOX_TEMPORAL=1, read once: shaders, depth view and markers are set up. */
int  d3d8_temporal_wanted(void);
/* Wanted and not failed. */
int  d3d8_temporal_active(void);

/* The game side: the camera an image was rendered with, by the frame number
 * of its FRAME_END marker (modulo 8192): cam[0..15] the view (D3D rows),
 * [16] tan of half the vertical angle, [17] aspect, [24] 1 when valid,
 * [25] [26] near and far planes (0 = unknown). 0 when not noted. */
void d3d8_temporal_set_frame_source(int (*fn)(unsigned frame, float *cam));

/* Device, at the end of a present: the next image is jittered when the image
 * just shown carried markers (marked); w x h is the target its 3D is drawn
 * into, pct the render scale. */
void d3d8_temporal_next_image(int marked, UINT w, UINT h, UINT pct);
/* The jitter of the draw being issued, in NDC (x right, y up); 0 when none. */
int  d3d8_temporal_jitter_ndc(float *jx, float *jy);
/* The mip bias added to the title's while the jitter is on; else 0. */
float d3d8_temporal_mip_bias(void);
/* Device, at the HUD phase: the jitter off (an image without FRAME_END). */
void d3d8_temporal_jitter_off(void);
/* Device: this image's 3D was brought to the output size at FRAME_END (the
 * render scale); until the next image. The translator then leaves the
 * lights of the perspective groups to the title (d3d8_xbox.h). */
void d3d8_temporal_set_moved(void);
int  d3d8_temporal_moved(void);

/* Draw mirrors (the marks: d3d8_xbox.h). 1 = the draw was mirrored into the
 * reactive accumulation or the riders' object target: end it with
 * mirror_end; 0 = nothing to do (off, not the 3D target, failed). */
int  d3d8_temporal_mirror_begin(ID3D11DeviceContext *ctx);
void d3d8_temporal_mirror_end(ID3D11DeviceContext *ctx);

/* FRAME_END of an image (pump thread, the 3D target still bound, before the
 * post): the jitter off, the camera of the image (by the marker's tag), the
 * motion vectors and the reactive mask built from depth (w x h, the bound
 * 3D target's). State saved and restored. */
void d3d8_temporal_frame_end(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                             ID3D11ShaderResourceView *depth_srv, ID3D11Texture2D *depth_tex,
                             UINT w, UINT h, unsigned tag);
/* After the post at FRAME_END: XBOX_TEMPORAL_VIEW drawn over rtv (w x h). */
void d3d8_temporal_show(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                        ID3D11RenderTargetView *rtv, UINT w, UINT h);

/* This image's inputs, valid from its FRAME_END to the present, for the
 * super-resolution passes to come -- the same set for DLSS / DLAA (NGX: colour, depth,
 * motion vectors with their scale, jitter offset, reset; the reactive mask
 * unused or as its "bias current colour" mask) and for FSR 3.1 (the same,
 * plus the reactive mask). The colour is the bound 3D target at FRAME_END
 * (the device's). mv and reactive are owned by the module; depth is the
 * device's view (single- or multi-sampled: depth_ms). */
typedef struct {
    ID3D11ShaderResourceView *mv;        /* RG16F, uv: prev = cur + mv, no jitter;
                                          * x w, h for pixels (DLSS mvScale / FSR scale) */
    ID3D11ShaderResourceView *reactive;  /* R8 */
    ID3D11ShaderResourceView *depth;     /* R24 of the 3D target, z not inverted */
    int   depth_ms;
    UINT  w, h;                          /* the 3D target's size */
    float jitter_px[2];                  /* this image's offset, pixels (x right, y down) */
    int   phases;                        /* length of the jitter sequence */
    int   reset;                         /* history to be dropped */
} d3d8_temporal_inputs;
int  d3d8_temporal_get(d3d8_temporal_inputs *o);

/* XBOX_TEMPORAL_PROBE=<file> (diagnostic): the previous image warped onto
 * this one by the vectors, every 30th image (d3d8_temporal.c). The device
 * calls probe at FRAME_END with the resolved 3D colour, before the post.
 * XBOX_TEMPORAL_NEAR / _FAR replace the planes of the depth (diagnostic). */
int  d3d8_temporal_probing(void);
void d3d8_temporal_probe(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *color);

void d3d8_temporal_release(void);
#endif

#endif /* D3D8_TEMPORAL_H */
