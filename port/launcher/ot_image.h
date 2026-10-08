/*
 * ot_image.h -- small RGBA image toolkit for the OpenTricky launcher.
 *
 * Portable C99, no dependency on the game or on any platform API. Pixels are
 * 32-bit 0xAARRGGBB, straight (not premultiplied) alpha, rows top-down.
 * Filtering works internally on premultiplied floats so transparent texels
 * never bleed their colour into the opaque ones.
 */
#ifndef OT_IMAGE_H
#define OT_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int       w, h;
    uint32_t *px;
} OtImage;

bool ot_image_alloc(OtImage *im, int w, int h);      /* cleared to 0 */
void ot_image_free(OtImage *im);
bool ot_image_copy(const OtImage *src, OtImage *dst);

/* Resample the source rectangle [sx0,sx1) x [sy0,sy1) (texel units, may be
 * fractional) to dw x dh with a Lanczos-3 filter, widened when shrinking. */
bool ot_image_resample(const OtImage *src, float sx0, float sy0, float sx1, float sy1,
                       int dw, int dh, OtImage *dst);
bool ot_image_resize(const OtImage *src, int dw, int dh, OtImage *dst);

/* Gaussian blur in place (standard deviation in pixels; <= 0: no-op). */
bool ot_image_blur(OtImage *im, float sigma);

/* Coverage masks (0..255, w * h bytes, caller frees), antialiased with 4 x 4
 * samples per pixel. Rectangle edges are in pixel-corner coordinates. */
uint8_t *ot_mask_round_rect(int w, int h, float x0, float y0, float x1, float y1, float radius);
uint8_t *ot_mask_ellipse(int w, int h, float x0, float y0, float x1, float y1);

/* Multiply the image's alpha by a mask of the same size. */
void ot_image_apply_mask(OtImage *im, const uint8_t *mask);

/* Draw `src` over `dst` at (x, y) ("source over"). */
void ot_image_blend(OtImage *dst, const OtImage *src, int x, int y);

/* ── DDS textures (texture replacement packs) ─────────────────────────
 * Formats: BC1, BC2, BC3, BC7 (legacy FourCC or DX10 header) and 32-bit
 * RGBA / BGRA. `min_w`: decode the smallest mip level at least that wide
 * (0 = the full-size level). */
bool ot_dds_info(const uint8_t *d, size_t n, int *w, int *h, int *mips);
bool ot_dds_decode(const uint8_t *d, size_t n, int min_w, OtImage *out);

/* Block decoders, exposed for tests: one 4 x 4 block into 16 pixels. */
void ot_decode_bc1(const uint8_t *b, uint32_t out[16], bool punch_alpha);
void ot_decode_bc2(const uint8_t *b, uint32_t out[16]);
void ot_decode_bc3(const uint8_t *b, uint32_t out[16]);
void ot_decode_bc7(const uint8_t *b, uint32_t out[16]);

#ifdef __cplusplus
}
#endif

#endif /* OT_IMAGE_H */
