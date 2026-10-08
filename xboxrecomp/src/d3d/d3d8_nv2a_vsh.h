/**
 * NV2A vertex programs on the GPU. See d3d8_nv2a_vsh.c for the
 * HLSL generator and d3d8_nv2a.c for the draw.
 */
#ifndef D3D8_NV2A_VSH_H
#define D3D8_NV2A_VSH_H

#include <stdint.h>
#include "../nv2a/nv2a_vsh_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How a program input reaches the shader. */
enum {
    NV2A_VSH_IN_FLOAT = 0,   /* float4 from a float / UNORM / SNORM element */
    NV2A_VSH_IN_INT   = 1,   /* int4 (S32K: shorts taken as integers) */
    NV2A_VSH_IN_CMP   = 2,   /* one uint: the packed 11:11:10 normal */
    NV2A_VSH_IN_CONST = 3,   /* no array: the attribute's constant value */
    NV2A_VSH_IN_W1    = 0x10 /* flag: a 3-component element read as 4; w is 1 */
};

/* points != 0: the shader also hands the point's centre and half
 * size to the point-sprite geometry shader (d3d8_nv2a.c). */
int nv2a_vsh_hlsl(const vshcpu_insn *p, int n, uint16_t inputs, const uint8_t kind[16],
                  int points, char *buf, int cap);

/* One program draw, as the push-buffer translator hands it over. */
typedef struct {
    const vshcpu_insn *prog;
    int                prog_len;
    uint64_t           prog_hash;       /* identifies prog[0..prog_len) */
    uint16_t           inputs;          /* bit n: the program reads v[n] */
    struct {
        uint8_t         kind;           /* NV2A_VSH_IN_* */
        uint32_t        dxgi_format;    /* DXGI_FORMAT of the element */
        uint32_t        stride;
        const uint8_t  *base;           /* host address of vertex `lo`'s element */
        uint32_t        bytes;          /* readable bytes from base (whole range) */
    } attr[16];
    /* Guest memory as one flat host range [mem_lo, mem_hi): attribute bases
     * inside it point straight at guest memory (others at per-draw copies). */
    const uint8_t     *mem_lo, *mem_hi;
    const float      (*attr_const)[4];  /* values of attributes with no array */
    const float      (*vconst)[4];      /* the 192 program constants */
    float              screen_w, screen_h, clip_max;
    int                fog_mode;        /* 0 off, 1 linear, 2 linear abs, 3 exp, 4 exp abs, 5 exp2 */
    float              fog_p0, fog_p1;
    int                specular, spec_alpha;
    int                topology;        /* D3DPT_TRIANGLELIST / LINELIST / LINESTRIP / POINTLIST */
    /* Points: size from oPts.x (SET_POINT_PARAMS_ENABLE) or the
     * fixed size; SET_POINT_SMOOTH_ENABLE puts the sprite coordinate in t3. */
    int                point_params;
    float              point_size;      /* SET_POINT_SIZE / 8, or 1 */
    int                point_smooth;
    float              point_kx;        /* half-width factor (round sprites), 1 = square */
    float              point_zoom;      /* oPts scale (narrowed field of view), 1 = none */
    /* Race HUD element: x' = ax + (x - ax) * kx, y' = ay + (y - ay) * ky,
     * title pixels ; kx 0 = none. */
    float              hud_kx, hud_ax, hud_ky, hud_ay;
    const uint32_t    *indices;         /* rebased: 0 = vertex `lo` */
    uint32_t           nindices;
    unsigned long long ps_key;
    const void        *ps_consts;
    unsigned           ps_consts_size;
    int                raster;          /* as d3d8_nv2a_draw */
} Nv2aVshDraw;

/* 1 if drawn; 0 if this draw cannot go through the GPU path (the caller then
 * runs the program on the CPU). */
int d3d8_nv2a_draw_program_gpu(const Nv2aVshDraw *d);

/* XBOX_FIX_POINTS_GPU (default 1): program draws in point mode
 * (the particles) take the GPU path, their squares built by a geometry
 * shader; 0 keeps them on the CPU interpreter. */
int d3d8_points_gpu_on(void);

/* XBOX_POINTS_CHECK=1 (diagnostic): the next point draw's GPU output
 * is compared with these CPU squares (ProgVertex, six per point). */
int  d3d8_points_check_on(void);
void d3d8_nv2a_points_expect(const void *verts, unsigned n, unsigned stride);

#ifdef __cplusplus
}
#endif
#endif
