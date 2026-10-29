/*
 * d3d8_dither -- the output dither of the fork's effects: a
 * triangular noise of +-1 level of 8 bits, added where an effect writes a
 * smooth gradient into an 8-bit target (the vignette and the speed blur,
 * the bloom's glow, the lights in the haze), so a slow ramp (a corner of
 * the vignette, a wide glow in a night sky) comes out without bands.
 *
 * The image is 8 bits from the 3D on (R8G8B8A8_UNORM, no float target in
 * between): a band is made where an effect's result is rounded, so the
 * noise goes in there, before that rounding; a dither added once at the
 * very end would only add noise to steps already made. The ambient haze
 * dithers its own write already (d3d8_veil.c, ps_up / ps_fuse).
 *
 * The noise is fixed per pixel of the target (an integer hash of its
 * place): no grain moving from one image to the next. Its mean is 0, its
 * weights are not an edge filter: nothing is sharpened. An effect off draws
 * nothing, so nothing is dithered: the title's image (Original Xbox, every
 * effect off) is bit for bit the same.
 *
 *   XBOX_DITHER=0     no dither (default 1: +-1 level; 0..2 scales it)
 */
#ifndef D3D8_DITHER_H
#define D3D8_DITHER_H

/* HLSL: dith_tri(pixel) = a triangular value in (-1, 1), fixed per pixel;
 * dith_add(c, pixel, amp) = c plus amp levels of it, faded to nothing at 0
 * and at 1 (black stays black, white stays white: no lift of the blacks
 * by the half of the noise the rounding would clamp). */
#define D3D8_DITHER_HLSL \
"uint dith_hash(uint s) { s = (s ^ 61u) ^ (s >> 16); s *= 9u; s ^= s >> 4; s *= 0x27d4eb2du; s ^= s >> 15; return s; }\n" \
"float dith_tri(float2 p) {\n" \
"    uint2 q = uint2(p);\n" \
"    uint h = dith_hash(q.x | (q.y << 16));\n" \
"    return (float(h & 0xFFFFu) + float(h >> 16) + 1.0) * (1.0 / 65536.0) - 1.0;\n" \
"}\n" \
"float3 dith_add(float3 c, float2 p, float amp) {\n" \
"    float3 a = saturate(min(c, 1.0 - c) * 255.0) * amp;\n" \
"    return c + dith_tri(p) * a * (1.0 / 255.0);\n" \
"}\n"

/* The dither's amplitude in levels (XBOX_DITHER, read once): 0 off, 1 the
 * default. Pump thread. */
float d3d8_dither_amp(void);

#endif /* D3D8_DITHER_H */
