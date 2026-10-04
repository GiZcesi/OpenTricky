/*
 * NV2A register combiners -> HLSL.
 *
 * The title's 3D scenes are shaded by the NV2A pixel pipeline: up to four
 * texture-shader stages (SET_SHADER_STAGE_PROGRAM) feeding up to eight
 * general register-combiner stages and the final combiner. The frontend's ice
 * cave is bump mapped -- stage 0 dots a normal map with a light vector packed
 * into oD0 -- so drawing oD0 as a colour painted it green and pink.
 *
 * This is a port of xemu's hw/xbox/nv2a/pgraph/glsl/psh.c (LGPL, espes and
 * the xemu project) from GLSL to HLSL, driven by the push-buffer registers.
 */
#ifndef NV2A_PSH_H
#define NV2A_PSH_H

#include <stdint.h>

typedef struct {
    uint32_t combiner_control;       /* 0x1E60: stages 7:0, flags 15:8 */
    uint32_t shader_stage_program;   /* 0x1E70: 5 bits per texture stage */
    uint32_t other_stage_input;      /* 0x1E78: dot mappings + input textures */
    uint32_t final0, final1;         /* SPECULAR_FOG_CW0 / CW1 */
    uint32_t rgb_in[8], rgb_out[8], alpha_in[8], alpha_out[8];
    uint8_t  tex_enabled[4];
    uint8_t  tex_dim[4];             /* 2 or 3 */
    uint8_t  tex_cube[4];
    uint8_t  tex_rect[4];            /* linear image: texel-unit coordinates */
    uint8_t  alphakill[4];
    uint8_t  alpha_test;
    uint8_t  alpha_func;             /* 0 NEVER .. 7 ALWAYS (GL enum - 0x200) */
    uint8_t  simple;                 /* XBOX_NV2A_PSH=0: t0 * v0, the old look */
    uint8_t  show;                   /* XBOX_NV2A_PSH_SHOW: 1+ index into show_regs (diagnostic) */
    uint8_t  window_clip;            /* 0 off, 1 inclusive, 2 exclusive (SET_WINDOW_CLIP_*) */
} Nv2aPshState;

/* Pixel-shader constant buffer; matches the cbuffer the generator emits. */
typedef struct {
    float c0[9][4];                  /* COMBINER_FACTOR0[i]; [8] final combiner */
    float c1[9][4];                  /* COMBINER_FACTOR1[i]; [8] final combiner */
    float fog_color[4];
    float tex_size[4][4];            /* texel size of each stage, for rect textures */
    float alpha_ref[4];              /* x: 0..255 */
    float clip_region[8][4];         /* window-clip rectangles: xmin, ymin, xmax, ymax (exclusive) */
} Nv2aPshConsts;

uint64_t nv2a_psh_key(const Nv2aPshState *st);
/* Writes HLSL for `st` into buf; returns its length, or -1 if it did not fit. */
int nv2a_psh_generate(const Nv2aPshState *st, char *buf, int size);

#endif
