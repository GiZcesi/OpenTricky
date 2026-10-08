/*
 * NV2A PGRAPH → D3D11 Translator
 *
 * Translates NV2A push buffer methods into D3D8→D3D11 rendering calls.
 * Designed for Xbox static recompilation (xboxrecomp toolkit).
 *
 * Menu rendering profile (captured from xemu):
 *   - INLINE_ARRAY with 5-dword vertices (X, Y, U, V, Color)
 *   - TRIANGLE_STRIP topology
 *   - ~448 vertices per frame (~89 quads)
 *   - Textured 2D elements in 640×480 screen space
 */

#include "../kernel/xbox_perf.h"
extern int d3d8_pump_cache_on(void);
#include <time.h>
#include "nv2a_pgraph_d3d11.h"
#include "nv2a_regs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>

/* D3D8 device — we include the full header for COM vtable access */
#include "../d3d/d3d8_xbox.h"
#include "../d3d/d3d8_gpuprof.h"
#include "nv2a_vsh_cpu.h"
#include "nv2a_psh.h"
#include "nv2a_hdtex.h"
#include "nv2a_btnicons.h"
extern IDirect3DDevice8 *xbox_GetD3DDevice(void);

/* Global.txd texture lookup */
/* Game-specific texture lookup - only available when GAME_HAS_FONT_ATLAS is defined */
#ifdef GAME_HAS_FONT_ATLAS
typedef struct { char name[24]; IDirect3DTexture8 *texture; uint32_t width, height, format; } TXD_Entry;
typedef struct { TXD_Entry entries[512]; int count; } TXD_Dict;
extern TXD_Dict g_global_txd;
extern int g_textures_loaded;
extern IDirect3DTexture8 *txd_find(const TXD_Dict *dict, const char *name);
#else
static int g_textures_loaded = 0;
#endif

/* Font atlas DXT5 data - game-specific, only available in burnout3 */
#ifdef GAME_HAS_FONT_ATLAS
#include "font_atlas_data.h"
#endif

/* Create a D3D8 texture from raw DXT5 data */
static IDirect3DTexture8 *create_dxt5_texture(IDirect3DDevice8 *dev,
    uint32_t width, uint32_t height, const void *dxt5_data, uint32_t data_size)
{
    IDirect3DTexture8 *tex = NULL;
    /* D3DFMT_DXT5 = 0x35545844 ('DXT5') on Xbox, mapped to DXGI_FORMAT_BC3 in our layer.
     * Our d3d8 layer uses format code 0x0F for DXT5. */
    HRESULT hr = dev->lpVtbl->CreateTexture(dev, width, height, 1,
        0 /*Usage*/, 0x0F /*DXT5*/, 0 /*D3DPOOL_DEFAULT*/, &tex);
    if (hr != 0 || !tex) {
        fprintf(stderr, "[PGRAPH-D3D11] Failed to create font atlas texture: hr=0x%08X\n", hr);
        return NULL;
    }

    /* Lock and fill with DXT5 data */
    D3DLOCKED_RECT lr = {0};
    hr = tex->lpVtbl->LockRect(tex, 0, &lr, NULL, 0);
    if (hr == 0 && lr.pBits) {
        memcpy(lr.pBits, dxt5_data, data_size);
        tex->lpVtbl->UnlockRect(tex, 0);
        fprintf(stderr, "[PGRAPH-D3D11] Created font atlas: %ux%u DXT5 (%u bytes)\n",
                width, height, data_size);
    } else {
        fprintf(stderr, "[PGRAPH-D3D11] Failed to lock font atlas: hr=0x%08X\n", hr);
    }
    return tex;
}

/* ══════════════════════════════════════════════════════════════════════
 * NV2A method constants come from nv2a_regs.h.
 *
 * This file used to keep its own copies. Six of them were simply wrong --
 * CLEAR_SURFACE was 0x01D0 rather than 0x1D94, all four clear methods were
 * off, DEPTH_TEST_ENABLE and CULL_FACE_ENABLE named other registers entirely,
 * and TEXTURE_CONTROL0 was 0x1B08 rather than 0x1B0C. Because they shadowed
 * the correct definitions the compiler warned on every one of them and the
 * warnings were ignored, so the title's clears were looked for at addresses it
 * never writes: `clears=0` every run while SET_COLOR_CLEAR_VALUE sat in the
 * histogram. Do not reintroduce local copies.
 * ══════════════════════════════════════════════════════════════════════ */





/* NV2A draw modes → D3D primitive types */
static int nv2a_draw_mode_to_d3d(uint32_t mode) {
    switch (mode) {
        case 1:  return D3DPT_POINTLIST;
        case 2:  return D3DPT_LINELIST;
        case 3:  return D3DPT_LINESTRIP;  /* LINE_LOOP → LINE_STRIP */
        case 4:  return D3DPT_LINESTRIP;
        case 5:  return D3DPT_TRIANGLELIST;
        case 6:  return D3DPT_TRIANGLESTRIP;
        case 7:  return D3DPT_TRIANGLEFAN;
        case 8:  return D3DPT_TRIANGLELIST; /* QUADS → TRI_LIST (needs conversion) */
        default: return D3DPT_TRIANGLELIST;
    }
}

/* NV2A blend factors → D3D blend */
static uint32_t nv2a_blend_to_d3d(uint32_t nv) {
    switch (nv) {
        case 0x0000: return D3DBLEND_ZERO;
        case 0x0001: return D3DBLEND_ONE;
        case 0x0300: return D3DBLEND_SRCCOLOR;
        case 0x0301: return D3DBLEND_INVSRCCOLOR;
        case 0x0302: return D3DBLEND_SRCALPHA;
        case 0x0303: return D3DBLEND_INVSRCALPHA;
        case 0x0304: return D3DBLEND_DESTALPHA;
        case 0x0305: return D3DBLEND_INVDESTALPHA;
        case 0x0306: return D3DBLEND_DESTCOLOR;
        case 0x0307: return D3DBLEND_INVDESTCOLOR;
        case 0x0308: return D3DBLEND_SRCALPHASAT;
        default:     return D3DBLEND_ONE;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Translator State
 * ══════════════════════════════════════════════════════════════════════ */

/* Inline vertex buffer - max 16K vertices per draw */
#define MAX_INLINE_VERTS 16384
#define INLINE_VERT_DWORDS 5  /* X, Y, U, V, Color */

/* RwIm2DVertex-compatible output vertex (28 bytes) */
typedef struct {
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
} OutputVertex;

static struct {
    /* Draw state */
    int in_draw;           /* Between BEGIN and END */
    uint32_t draw_mode;    /* NV2A draw mode (0=end, 6=tristrip, etc.) */
    int d3d_prim_type;     /* Translated D3D prim type */

    /* Inline vertex accumulator */
    uint32_t inline_data[MAX_INLINE_VERTS * INLINE_VERT_DWORDS];
    uint32_t inline_count; /* Number of dwords accumulated */
    uint32_t vert_stride;  /* Dwords per vertex (auto-detected) */

    /* Bound vertex streams (NV2A attribute slots 0..15), filled from
     * SET_VERTEX_DATA_ARRAY_OFFSET / _FORMAT and consumed by DRAW_ARRAYS. */
    struct { uint32_t offset; uint32_t format; } vattr[16];

    /* Per-attribute constant ("inline") values.
     *
     * When an attribute's array is disabled (size == 0) the hardware does not
     * invent a value -- it feeds the attribute's constant register, which the
     * title sets with NV097_SET_VERTEX_DATA*.  This title binds *only*
     * position as an array: the other 48 bytes of its 64-byte vertex are
     * zero, so colour and texcoords come from here and nowhere else.
     * Substituting a guessed default instead painted a white full-screen quad
     * over every frame. */
    float vattr_const[16][4];
    int   vattr_const_init;

    /* Clear state */
    uint32_t clear_color;
    uint32_t clear_rect_h;  /* (width << 16) | x */
    uint32_t clear_rect_v;  /* (height << 16) | y */

    /* Render state cache */
    int depth_test;
    int blend_enable;
    uint32_t blend_sfactor;
    uint32_t blend_dfactor;
    int cull_enable;
    int alpha_test;
    uint32_t color_mask;
    uint32_t depth_func;    /* GL compare enum, 0x200 NEVER .. 0x207 ALWAYS */
    int      depth_mask;
    uint32_t alpha_func;
    uint32_t alpha_ref;

    /* Fog: NV097_SET_FOG_* as xemu's pgraph keeps them. The fog
     * colour parameter is ABGR; fog_param are FOGPARAM0/1 (slot 2 unused). */
    int      fog_enable;
    uint32_t fog_mode, fog_gen, fog_color;
    float    fog_param[3];
    uint32_t spec_fog_cw0, spec_fog_cw1;

    /* Register combiners, as the Xbox D3D runtime writes them. */
    uint32_t comb_alpha_icw[8], comb_alpha_ocw[8];
    uint32_t comb_color_icw[8], comb_color_ocw[8];
    uint32_t comb_factor0[8], comb_factor1[8];
    uint32_t comb_control;          /* 0x1E60: stage count in bits 7:0 */
    uint32_t shader_stage_program;  /* 0x1E70: texture shader mode, 5 bits per stage */
    uint32_t dot_rgbmapping, shader_other_stage_input;
    uint32_t specular_fog_factor[2];
    uint32_t zmin_max_control;      /* 0x1D78: ZCLAMP_EN 7:4, 0 cull / 1 clamp */
    int      zmin_max_seen;
    uint32_t control0;              /* 0x0290: Z_FORMAT 12, Z_PERSPECTIVE 16 */
    int      control0_seen;
    unsigned frame_no;              /* presents seen, for XBOX_NV2A_DRAWLOG */

    /* Viewport */
    float vp_offset[4];
    float vp_scale[4];
    uint32_t surface_clip_h;
    uint32_t surface_clip_v;
    uint32_t surface_color_offset;  /* SET_SURFACE_COLOR_OFFSET (0x0210) */
    uint32_t prev_surface_offset;   /* the surface presented at the last flip */
    int      surface_offset_seen;
    int      frame_complete;        /* set when the render surface flips */
    int      flip_mode;             /* the title flips surfaces: present at flips only */
    unsigned long long last_flip_ms;
    uint32_t draws_since_present;   /* frame has content worth showing */
    /* SET_WINDOW_CLIP_*: type (0 inclusive, 1 exclusive) and the
     * eight rectangles, xmin 11:0 / xmax 27:16 (inclusive) per register. */
    uint32_t wclip_type, wclip_x[8], wclip_y[8];
    int      wclip_seen;
    /* Points: SET_POINT_PARAMS_ENABLE takes the size from the
     * program's oPts.x, otherwise SET_POINT_SIZE / 8; SET_POINT_SMOOTH_ENABLE
     * makes them sprites whose stage-3 coordinate runs 0..1 across the square. */
    int      point_params_enable, point_smooth;
    float    point_params[8];       /* SET_POINT_PARAMS 0x0A30..0x0A4C */
    uint32_t point_size;
    int      specular_enable;       /* SET_SPECULAR_ENABLE: oD1 reaches the combiners */
    /* Stencil, face culling. GL enums as the push buffer sends
     * them; applied to program draws. */
    int      stencil_test;
    uint32_t stencil_func, stencil_ref, stencil_mask_read, stencil_mask_write;
    uint32_t stencil_op[3];         /* fail, zfail, zpass */
    uint32_t cull_face, front_face;
    uint32_t zstencil_clear;        /* SET_ZSTENCIL_CLEAR_VALUE: depth 31:8, stencil 7:0 (D24S8) */
    uint32_t light_control;         /* SET_LIGHT_CONTROL (bit 17: specular alpha) */

    /* Texture state per stage (4 stages) */
    struct {
        uint32_t offset;     /* NV2A VRAM offset (method 0x1B00) */
        uint32_t format;     /* Format register (method 0x1B04) */
        uint32_t control0;   /* Control0 register (method 0x1B0C) */
        uint32_t filter;     /* FILTER (0x1B14): min 23:16, mag 27:24 */
        uint32_t address;    /* ADDRESS (0x1B08): U 3:0, V 11:8 -- 1 wrap, 2 mirror,
                                3 clamp, 4 border (D3D's values), 5 GL clamp */
        uint32_t control1;   /* Control1 register (0x1B10) -- IMAGE_PITCH in 31:16 */
        uint32_t image_rect; /* IMAGE_RECT (0x1B1C) -- linear w/h */
        int enabled;         /* Decoded from control0 bit 30 */
        IDirect3DTexture8 *d3d;   /* Uploaded texture, if any */
        uint32_t d3d_offset;      /* The (offset, format) this was built from, */
        uint32_t d3d_format;      /* so a rebind with the same pair is free */
        uint32_t bump[6];         /* SET_BUMP_ENV_MAT 00 01 10 11, SCALE, OFFSET (0x1B28..0x1B3C, floats) */
    } tex[4];

    /* Cached texture pointers */
    void *menu_texture;           /* IDirect3DTexture8* from Global.txd */
    IDirect3DTexture8 *font_atlas; /* Created from captured DXT5 data */
    int texture_lookup_done;

    /* Stats */
    PgraphD3D11Stats stats;

    /* Chyron scroll */
    float chyron_scroll_offset;  /* Pixels to shift X for chyron text */

    /* Transform state. Everything the vertex-program path needs:
     * program memory and constant memory with their load cursors, the start
     * slot, the execution mode, and the depth range the program's z is scaled
     * into. All of it used to fall into the catch-all ignore list. */
    uint32_t xf_mode;                   /* SET_TRANSFORM_EXECUTION_MODE: 0 fixed, 2 program */
    uint32_t prog[VSHCPU_SLOTS][4];
    uint32_t prog_load, prog_start;
    float    vconst[VSHCPU_CONSTANTS][4];
    uint32_t const_load;
    int      prog_dirty;
    int      prog_len;                  /* decoded length, -1 = no FINAL */
    uint16_t prog_inputs;               /* bit n: the program reads v[n] */
    uint8_t  prog_writes_c;             /* the program writes constant registers */
    uint64_t prog_hash;                 /* of prog_dec[0..prog_len), for the GPU path */
    vshcpu_insn prog_dec[VSHCPU_SLOTS];
    float    clip_min, clip_max;

    /* Vertex indices of the current BEGIN/END, from ARRAY_ELEMENT16/32 and,
     * in program mode, DRAW_ARRAYS -- a long strip arrives as several
     * DRAW_ARRAYS of at most 256 vertices that must be assembled as one. */
    uint32_t *idx;
    uint32_t  idx_count, idx_cap;

    /* Init flag */
    int initialized;
} g_pg;

/* ══════════════════════════════════════════════════════════════════════
 * Float/uint32 conversion
 * ══════════════════════════════════════════════════════════════════════ */
static float u2f(uint32_t u) {
    union { float f; uint32_t i; } x;
    x.i = u;
    return x.f;
}

/* ══════════════════════════════════════════════════════════════════════
 * Initialization
 * ══════════════════════════════════════════════════════════════════════ */

void pgraph_d3d11_init(void)
{
    memset(&g_pg, 0, sizeof(g_pg));
    g_pg.vert_stride = INLINE_VERT_DWORDS;  /* Default: 5 dwords per vertex */
    g_pg.clear_color = 0xFF000000;
    g_pg.color_mask = 0x01010101;
    g_pg.stencil_func = 0x207;          /* ALWAYS */
    g_pg.stencil_mask_read = g_pg.stencil_mask_write = 0xFF;
    g_pg.stencil_op[0] = g_pg.stencil_op[1] = g_pg.stencil_op[2] = 0x1E00;   /* KEEP */
    g_pg.cull_face = 0x405;             /* BACK */
    g_pg.front_face = 0x901;            /* CCW */
    g_pg.zstencil_clear = 0xFFFFFF00u;
    g_pg.clear_rect_h = g_pg.clear_rect_v = 0x0FFF0000u;   /* whole surface until set */
    g_pg.initialized = 1;

    fprintf(stderr, "[PGRAPH-D3D11] Translator initialized\n");
}

void pgraph_d3d11_shutdown(void)
{
    g_pg.initialized = 0;
    fprintf(stderr, "[PGRAPH-D3D11] Translator shut down (draws=%u, verts=%u)\n",
            g_pg.stats.draw_calls, g_pg.stats.vertices_submitted);
}

/* ══════════════════════════════════════════════════════════════════════
 * Draw Submission
 * ══════════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════════
 * Vertex-array draw path (NV097_DRAW_ARRAYS)
 *
 * The translator originally implemented exactly one draw path: SET_BEGIN_END
 * bracketing NV097_INLINE_ARRAY vertices pushed inline, which is the profile
 * captured from the xemu menu trace. The title does not use it. It binds
 * vertex buffers and draws with NV097_DRAW_ARRAYS, so submit_draw() always
 * found inline_count == 0 and returned without drawing. That, and not any
 * fault, is why the draw counter stayed at zero while half a million push
 * buffer dwords went past.
 *
 * Registers involved, per xemu's nv2a_regs.h:
 *   0x1720 + i*4   SET_VERTEX_DATA_ARRAY_OFFSET[i]   RAM address of stream i
 *   0x1760 + i*4   SET_VERTEX_DATA_ARRAY_FORMAT[i]   type:4 size:4 stride:24
 *   0x1810         DRAW_ARRAYS                       start:24 count-1:8
 *
 * Attribute slots follow the NV2A fixed assignment: 0 position, 3 diffuse,
 * 9 texcoord0.
 *
 * Positions are passed through untransformed with rhw defaulted to 1.
 * Geometry the title runs through a vertex program is still in object space
 * and will land in the wrong place until the transform is applied -- that is
 * the next piece. What this unblocks is submission: vertices reaching the
 * device at all.
 * ══════════════════════════════════════════════════════════════════════ */

/* All of these already exist in nv2a_regs.h; these are just short local
 * spellings, so the register file stays the single source of truth. */
#define NV2A_VA_OFFSET_BASE   NV097_SET_VERTEX_DATA_ARRAY_OFFSET
#define NV2A_VA_FORMAT_BASE   NV097_SET_VERTEX_DATA_ARRAY_FORMAT
#define NV2A_VA_SLOTS         16

#define NV2A_ATTR_POSITION    0
#define NV2A_ATTR_DIFFUSE     3
#define NV2A_ATTR_TEXCOORD0   9

#define NV2A_VA_TYPE_UB_D3D   NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D
#define NV2A_VA_TYPE_S1       NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1
#define NV2A_VA_TYPE_F        NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F
#define NV2A_VA_TYPE_UB_OGL   NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL
#define NV2A_VA_TYPE_S32K     NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K

/* Guest RAM window, handed over by the live push buffer consumer. The
 * translator cannot reach into the kernel library for it: xbox_kernel already
 * links xbox_nv2a, so a dependency the other way would be circular. */
static uint8_t *g_pg_mem_base = NULL;
static uint32_t g_pg_mem_size = 0;

void pgraph_d3d11_set_mem_base(void *base, uint32_t size)
{
    g_pg_mem_base = (uint8_t *)base;
    g_pg_mem_size = size;
}

/* XBOX_NV2A_DRAWLOG frame window; also armed live by the diag `drawlog`
 * command (nv2a_drawlog_arm), for screens reached by hand. */
static volatile int g_drawlog_lo = -2, g_drawlog_n = 1;

/* Diag `skipprog <hash>`: drop every draw whose vertex program has that
 * listing hash (the value XBOX_NV2A_DRAWLOG prints as "program %08X") --
 * "what is this program drawing over" without a rebuild. 0 clears it. */
static volatile uint32_t g_skip_prog;
void nv2a_skipprog_set(uint32_t h) { g_skip_prog = h; }

/* In draw-log frames with XBOX_NV2A_PEEK=x,y (0..1 of the frame): read the
 * scene target's pixel back after each draw, clear and flip, to find the
 * step that changes it (the black race intro). */
unsigned d3d8_DebugPeekScene(float fx, float fy);
static void peek_after(const char *what)
{
    static int on = -1;
    static float px, py;
    int f;
    if (on < 0) { const char *e = getenv("XBOX_NV2A_PEEK"); on = e && sscanf(e, "%f,%f", &px, &py) == 2; }
    f = (int)d3d8_PresentSeq();
    if (!on || g_drawlog_lo < 0 || f < g_drawlog_lo || f >= g_drawlog_lo + g_drawlog_n) return;
    fprintf(stderr, "[PEEK] f%d after %s: %08X\n", f, what, d3d8_DebugPeekScene(px, py));
}

void nv2a_drawlog_arm(int frames)
{
    g_drawlog_n = frames > 0 ? frames : 1;
    g_drawlog_lo = (int)d3d8_PresentSeq() + 2;
    fprintf(stderr, "[DRAWLOG] armed frames %d..%d\n", g_drawlog_lo, g_drawlog_lo + g_drawlog_n - 1);
}

/* Resolve an NV2A RAM offset to a host pointer. The Xbox GPU addresses main
 * memory 1:1 and the top nibble carries the memory-space tag, so mask it off.
 * Returns NULL when the span would leave guest RAM -- a stale or uninitialised
 * stream offset must not fault the process. */
static const uint8_t *va_ptr(uint32_t offset, uint32_t need)
{
    uint32_t off = offset & 0x0FFFFFFFu;
    if (!g_pg_mem_base || !g_pg_mem_size || need == 0)
        return NULL;
    if (off >= g_pg_mem_size || need > g_pg_mem_size - off)
        return NULL;
    return g_pg_mem_base + off;
}

/* One component of one attribute for vertex `idx`. Falls back to `dflt` when
 * the attribute is unbound, out of range, or of a type not decoded yet. */
/* The documented default for a generic vertex attribute is (0,0,0,1), which
 * for diffuse is opaque black -- the colour the title's full-screen backdrop
 * quad is actually meant to be. */
static void va_const_init(void)
{
    int i;
    if (g_pg.vattr_const_init) return;
    g_pg.vattr_const_init = 1;
    for (i = 0; i < 16; i++) {
        g_pg.vattr_const[i][0] = 0.0f;
        g_pg.vattr_const[i][1] = 0.0f;
        g_pg.vattr_const[i][2] = 0.0f;
        g_pg.vattr_const[i][3] = 1.0f;
    }
}

static float va_read(int attr, uint32_t idx, uint32_t comp, float dflt)
{
    uint32_t fmt    = g_pg.vattr[attr].format;
    uint32_t type   = fmt & 0x0Fu;
    uint32_t size   = (fmt >> 4) & 0x0Fu;
    uint32_t stride = (fmt >> 8) & 0xFFFFFFu; /* STRIDE is bits 8..31 (cxbx
                                             * NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE
                                             * is 0xFFFFFF00), not 8 bits -- an 8-bit
                                             * mask silently truncates any vertex wider
                                             * than 255 bytes to a bogus stride. */
    const uint8_t *p;

    va_const_init();
    if (size == 0 || stride == 0 || comp >= size)
        return (comp < 4) ? g_pg.vattr_const[attr][comp] : dflt;

    p = va_ptr(g_pg.vattr[attr].offset + idx * stride, stride);
    if (!p)
        return dflt;

    switch (type) {
    case NV2A_VA_TYPE_F: {
        uint32_t bits;
        memcpy(&bits, p + comp * 4, 4);
        return u2f(bits);
    }
    case NV2A_VA_TYPE_UB_D3D:
    case NV2A_VA_TYPE_UB_OGL:
        return (float)p[comp] / 255.0f;
    case NV2A_VA_TYPE_S1: {
        int16_t v;
        memcpy(&v, p + comp * 2, 2);
        return (float)v / 32767.0f;
    }
    case NV2A_VA_TYPE_S32K: {
        int16_t v;
        memcpy(&v, p + comp * 2, 2);
        return (float)v;
    }
    default:
        return dflt;
    }
}

/* Which array slot actually carries diffuse / texcoord0 for this draw?
 *
 * The NV2A's fixed-function slots are fixed -- 0 position, 3 diffuse, 9
 * texcoord0 -- and the translator read those constants directly. This title
 * does not always use them: its 2D geometry binds
 *
 *     attr0  float x4   position
 *     attr1  ub_d3d x4  diffuse
 *     attr2  float x2   texcoord0
 *
 * which is a vertex *declaration*, where the register numbers come from the
 * declaration rather than the fixed-function assignment. Reading slot 3 and
 * slot 9 there finds nothing bound, so every glyph came out with the constant
 * colour (opaque black) and texcoords of 0,0 -- black quads with a single
 * texel, on a black background.
 *
 * Falls back to the canonical slot whenever it is bound, so fixed-function
 * draws are unaffected; otherwise identifies the attribute by its type and
 * width, which are unambiguous for these two (a 4-component unsigned-byte
 * attribute is a colour; a 2-component float attribute is a texture
 * coordinate). */
static int va_slot_diffuse(void)
{
    int i;
    if (((g_pg.vattr[NV2A_ATTR_DIFFUSE].format >> 4) & 0xFu) != 0)
        return NV2A_ATTR_DIFFUSE;
    for (i = 1; i < 16; i++) {
        uint32_t fmt = g_pg.vattr[i].format;
        uint32_t type = fmt & 0x0Fu, size = (fmt >> 4) & 0x0Fu;
        if (size == 4 && (type == NV2A_VA_TYPE_UB_D3D || type == NV2A_VA_TYPE_UB_OGL))
            return i;
    }
    return NV2A_ATTR_DIFFUSE;
}

static int va_slot_texcoord0(void)
{
    int i;
    if (((g_pg.vattr[NV2A_ATTR_TEXCOORD0].format >> 4) & 0xFu) != 0)
        return NV2A_ATTR_TEXCOORD0;
    for (i = 1; i < 16; i++) {
        uint32_t fmt = g_pg.vattr[i].format;
        uint32_t type = fmt & 0x0Fu, size = (fmt >> 4) & 0x0Fu;
        if (size == 2 && type == NV2A_VA_TYPE_F)
            return i;
    }
    return NV2A_ATTR_TEXCOORD0;
}

/* Diffuse colour as a packed D3DCOLOR (ARGB). */
static uint32_t g_ignored_hist[0x2000 >> 2];

/* Diag `ignored`: the dropped-method histogram since the last call, then
 * reset -- "what does this scene send that we ignore". */
void nv2a_ignored_dump(void)
{
    unsigned i, n = 0;
    fprintf(stderr, "[NV2A] dropped methods since last dump:\n");
    for (;;) {
        unsigned best = 0, bi = 0;
        for (i = 0; i < (0x2000 >> 2); i++)
            if (g_ignored_hist[i] > best) { best = g_ignored_hist[i]; bi = i; }
        if (!best || n++ >= 40) break;
        fprintf(stderr, "    0x%04X  x%u\n", bi << 2, best);
        g_ignored_hist[bi] = 0;
    }
    memset(g_ignored_hist, 0, sizeof g_ignored_hist);
    fflush(stderr);
}

void pgraph_d3d11_report_ignored(void)
{
    unsigned i, n = 0;
    const char *e = getenv("XBOX_NV2A_IGNLOG");
    if (!(e && e[0] == '1')) return;
    fprintf(stderr, "[NV2A] dropped methods by frequency:" "\n");
    for (;;) {
        unsigned best = 0, bi = 0;
        for (i = 0; i < (0x2000 >> 2); i++)
            if (g_ignored_hist[i] > best) { best = g_ignored_hist[i]; bi = i; }
        if (!best || n++ >= 30) break;
        fprintf(stderr, "    0x%04X  x%u\n", bi << 2, best);
        g_ignored_hist[bi] = 0;
    }
    fflush(stderr);
}

/* NV097_NO_OPERATION accounting (fork).
 *
 * NOP used to sit in the silently-ignored list. It is counted now, with a
 * histogram of its parameters, because the pass tags of port/src/pass_tags.h
 * will travel as NOP parameters: before emitting anything we need to
 * know what the title itself sends there, and afterwards the count of
 * parameters carrying the tag magic is the proof the markers arrive.
 * Only touched by the method dispatcher, i.e. the pump thread. Printed with
 * the periodic [LIVE-PB] stats (the process is usually killed, so an atexit
 * report alone would never be seen). */
#define NV2A_PASS_TAG_MAGIC 0x5358u     /* PASS_TAG_MAGIC, port/src/pass_tags.h */
#define NOP_HIST_SLOTS 64
static struct {
    unsigned long long total, other, tags, tag_type[4];
    unsigned n;
    struct { uint32_t param; unsigned long long hits; } slot[NOP_HIST_SLOTS];
} g_nop;


/* Race HUD proportions (see nv2a_pgraph_d3d11.h). The title's
 * hooks (port/src/hud_anchor.c) write two HUD tags (anchor x, anchor y)
 * before the records of each element of its race HUD, right after the pass
 * tag of their group; every HUD-phase draw up to the next tag or pass tag is
 * scaled by kx, ky around that anchor (hud_element). Pump thread only. */
static struct {
    float kx, ky;                       /* 0 = off */
    int   on;                           /* the last x tag scales */
    float ax, ay;                       /* the anchor, title pixels of a 640x480 screen */
    unsigned long long tags, draws;
} g_hud;

void pgraph_d3d11_set_hud_scale(float kx, float ky)
{
    int on = kx > 0.0f && ky > 0.0f && (kx < 1.0f || ky < 1.0f);
    g_hud.kx = on ? kx : 0.0f;
    g_hud.ky = on ? ky : 0.0f;
}

/* The 16:9 frame (fork, port/src/hud_anchor.h). Box tags: a record
 * framed in the centred 16:9 (x scaled by k about the centre) or drawn as
 * is; frame tags: the frame is wide (menus framed) or shown whole at 16:9.
 * A frame tag applies to the image the frame's overlay goes into, which
 * holds the 3D of the frame before : the mode of the image is the
 * one of the previous frame tag. Pump thread only. */
static struct {
    float k;                            /* (16/9) / shape, 0 = off */
    float k2d;                          /* (4/3) / shape for framed records, 0 = k */
    float kboxed;                       /* (4/3) / the frame's shape: 0.75 (16:9 frame), 1 (4:3 frame); 0 = 0.75 */
    int   on;                           /* the last box tag frames */
    int   img_wide;                     /* the image being drawn is wide */
    int   next_wide;                    /* the last frame tag */
    int   tagged;                       /* a frame tag since the last image */
    unsigned long long boxed_images, wide_images;
} g_box;

void pgraph_d3d11_set_box(float k)
{
    g_box.k = k > 0.0f && k < 1.0f ? k : 0.0f;
    d3d8_SetPresentBox(g_box.k > 0.0f);             /* the boot and the videos: framed */
}

void pgraph_d3d11_set_box_race2d(float k2d)
{
    g_box.k2d = k2d > 0.0f && k2d < 1.0f ? k2d : 0.0f;
}

void pgraph_d3d11_set_box_frame(float kboxed)
{
    g_box.kboxed = kboxed > 0.0f ? kboxed : 0.75f;
}

/* Panels edge to edge (nv2a_pgraph_d3d11.h, PGRAPH_PANEL_TAG_MAGIC). Pump
 * thread only. */
static struct {
    int   on;                           /* the last tag was a panel's */
    int   wide_only, scale_only;
    float l, r;                         /* the panel's edges, title pixels */
    unsigned long long moved;           /* vertices moved to an edge */
} g_panel;

static void panel_tag_seen(uint32_t p)
{
    if ((p >> 16) == PGRAPH_PANEL_TAG_MAGIC_R) {
        g_panel.r = (float)(p & 0x3FFFu) / 16.0f;
        return;
    }
    g_hud.on = 0;
    g_box.on = 0;
    g_panel.on = 1;
    g_panel.wide_only = (p & 0x8000u) != 0;
    g_panel.scale_only = (p & 0x4000u) != 0;
    g_panel.l = (float)(p & 0x3FFFu) / 16.0f;
    g_panel.r = 1e9f;                   /* until its second tag */
}

static void box_tag_seen(uint32_t p)
{
    switch (p & 3u) {
    case 0: case 1:
        g_box.on = (int)(p & 1u);
        g_hud.on = 0;
        break;
    default:
        g_box.img_wide = g_box.next_wide;
        g_box.next_wide = (p & 3u) == 3u;
        g_box.tagged = 1;
        break;
    }
}

/* Just before an image is presented: framed unless it is a wide one. */
static void box_image_done(void)
{
    int boxed;
    if (!(g_box.k > 0.0f)) return;
    boxed = !(g_box.tagged && g_box.img_wide);
    g_box.tagged = 0;
    if (boxed) g_box.boxed_images++; else g_box.wide_images++;
    d3d8_SetPresentBox(boxed);
}

static void hud_tag_seen(uint32_t p)
{
    g_hud.tags++;
    if ((p >> 16) == PGRAPH_PANEL_TAG_MAGIC || (p >> 16) == PGRAPH_PANEL_TAG_MAGIC_R) {
        panel_tag_seen(p);
        return;
    }
    g_panel.on = 0;
    if ((p >> 16) == PGRAPH_BOX_TAG_MAGIC) {
        box_tag_seen(p);
        return;
    }
    g_box.on = 0;
    if ((p >> 16) == PGRAPH_HUD_TAG_MAGIC) {
        g_hud.on = (p & 0x8000u) != 0;
        g_hud.ax = (float)(p & 0x7FFFu) / 16.0f;
        g_hud.ay = 240.0f;
    } else {
        g_hud.ay = (float)(p & 0x7FFFu) / 16.0f;
    }
}

/* Pass phases (fork).
 *
 * With XBOX_PASS_TAGS=emit the title's own hooks (port/src/pass_tags.c) write
 * NOP markers into the push buffer: FRAME_BEGIN / FRAME_END around
 * SceneRenderer_RenderFrame, and GROUP (view, pass group, ortho) at the start
 * of each run of records. Each marker carries an absolute state, so a lost
 * one costs nothing beyond its own interval. Here they set the current phase,
 * and every draw is counted in the phase it was issued in.
 *
 * Reason by flip, not by RenderFrame: the image presented at a flip is
 *   - the 3D of the PREVIOUS RenderFrame (groups 0-6, ended by FRAME_END),
 *   - then, after the next FRAME_BEGIN, its groups >= 7: a few perspective
 *     ones (clouds, lens flare -- "persp>=7") and the HUD / UI in an
 *     orthographic view ("hud"),
 *   - then the flip, which presents. The Clear and the next 3D go to the
 *     other surface.
 * So "end of the 3D" (where a 3D-only post-process belongs) is the
 * FRAME_END marker, and the HUD of the same image comes after it.
 *
 * Phases: untagged (no marker yet), begin (FRAME_BEGIN, before any group),
 * 3d (group < 7), persp>=7, hud (group >= 7 in an ortho view), end (after
 * FRAME_END -- 0 draws expected; draws here come from frames the second
 * render thread draws without markers).
 * XBOX_PASS_TAGS_TRLOG=1 prints the draws of each phase for every presented
 * image ("[PHASE] img ..."). The order checks count markers out of place
 * (ooo), FRAME_BEGIN without the previous FRAME_END (lost_end) and jumps in
 * the frame number (gaps: frames of the other thread, or lost markers).
 * Pump thread only, like the rest of the translator. */
static const char *const k_phase_name[PGRAPH_PHASE_COUNT] = {
    "untagged", "begin", "3d", "persp>=7", "hud", "end"
};
static struct {
    int      phase;
    uint32_t draw_mark;                     /* stats.draw_calls at the last change */
    uint32_t draws[PGRAPH_PHASE_COUNT];     /* this presented image */
    int      in_frame, have_begin, have_seq, trlog, trlog_read;
    uint32_t last_begin, last_seq;
    unsigned long long tags, images, ooo, lost_end, gaps;
    unsigned long long total[PGRAPH_PHASE_COUNT];
    pgraph_pass_phase_fn cb;
} g_ph;

void pgraph_d3d11_set_pass_phase_callback(pgraph_pass_phase_fn fn) { g_ph.cb = fn; }
int  pgraph_d3d11_pass_phase(void) { return g_ph.phase; }

static void phase_set(int ph, uint32_t tag)
{
    {   /* XBOX_CULLSTAT group (cheap; read only in sampled frames) */
        extern void cullstat_set_group(int ph, uint32_t tag);
        cullstat_set_group(ph, tag);
    }
    uint32_t now = g_pg.stats.draw_calls;
    g_ph.draws[g_ph.phase] += now - g_ph.draw_mark;
    g_ph.draw_mark = now;
    if (g_gpuprof_on)                   /* group of the GROUP markers */
        gpuprof_set_phase(ph, ((tag >> 14) & 3u) == 2u ? (int)((tag >> 5) & 31u) : -1);
    if (ph != g_ph.phase) {
        int old = g_ph.phase;
        g_ph.phase = ph;
        if (g_ph.cb) g_ph.cb(old, ph, tag);
    }
}

static void pass_tag_seen(uint32_t p)
{
    uint32_t f = p & 0x1FFFu;
    g_ph.tags++;
    g_hud.on = 0;                       /* a HUD tag holds until the next pass tag */
    g_box.on = 0;                       /* and a box tag */
    g_panel.on = 0;                     /* and a panel tag */
    switch ((p >> 14) & 3u) {
    case 0:                                         /* FRAME_BEGIN */
        if (g_ph.in_frame) g_ph.lost_end++;
        if (g_ph.have_begin && f != ((g_ph.last_begin + 1u) & 0x1FFFu)) g_ph.gaps++;
        g_ph.last_begin = f;
        g_ph.have_begin = 1;
        g_ph.in_frame = 1;
        g_ph.have_seq = 0;
        phase_set(PGRAPH_PHASE_BEGIN, p);
        break;
    case 1:                                         /* FRAME_END */
        if (!g_ph.in_frame || f != g_ph.last_begin) g_ph.ooo++;
        g_ph.in_frame = 0;
        phase_set(PGRAPH_PHASE_END, p);
        break;
    case 2: {                                       /* GROUP */
        uint32_t seq = p & 31u, grp = (p >> 5) & 31u, ortho = (p >> 13) & 1u;
        if (!g_ph.in_frame) g_ph.ooo++;
        else if (g_ph.have_seq) {
            uint32_t d = (seq - g_ph.last_seq) & 31u;
            if (d == 0 || d > 16u) g_ph.ooo++;     /* rank within the frame went back */
        }
        g_ph.last_seq = seq;
        g_ph.have_seq = 1;
        phase_set(grp < 7u ? PGRAPH_PHASE_3D
                  : ortho ? PGRAPH_PHASE_HUD : PGRAPH_PHASE_AFTER3D_PERSP, p);
        break;
    }
    default:                                        /* ORTHO: not emitted */
        break;
    }
}

/* Just before the title's frame is presented (flip, or clear without flips). */
static void pass_phase_image_done(void)
{
    uint32_t now;
    int i;
    if (!g_ph.tags) return;                         /* no markers: nothing to say */
    now = g_pg.stats.draw_calls;
    g_ph.draws[g_ph.phase] += now - g_ph.draw_mark;
    g_ph.draw_mark = now;
    g_ph.images++;
    for (i = 0; i < PGRAPH_PHASE_COUNT; i++) g_ph.total[i] += g_ph.draws[i];
    if (!g_ph.trlog_read) {
        g_ph.trlog_read = 1;
        const char *e = getenv("XBOX_PASS_TAGS_TRLOG");
        g_ph.trlog = e && e[0] == '1';
    }
    if (g_ph.trlog)
        fprintf(stderr, "[PHASE] img %llu frame %u: 3d %u persp>=7 %u hud %u begin %u end %u"
                " untagged %u | ooo %llu lost_end %llu gaps %llu\n",
                g_ph.images, g_ph.last_begin,
                g_ph.draws[PGRAPH_PHASE_3D], g_ph.draws[PGRAPH_PHASE_AFTER3D_PERSP],
                g_ph.draws[PGRAPH_PHASE_HUD], g_ph.draws[PGRAPH_PHASE_BEGIN],
                g_ph.draws[PGRAPH_PHASE_END], g_ph.draws[PGRAPH_PHASE_UNTAGGED],
                g_ph.ooo, g_ph.lost_end, g_ph.gaps);
    memset(g_ph.draws, 0, sizeof g_ph.draws);
}

/* XBOX_PUMPLAG (port/src/pumplag.c, diagnostic): NOP 0x5D000000 | q before
 * each mesh part, q = 0 after the list. At the marker the port compares the
 * part's constant block with what the title drew it with; a rewritten part's
 * draws are then measured (plag_draw) and the frame saved. */
typedef int (*pgraph_plag_fn)(uint32_t q, uint32_t *cb, const uint8_t **snap);
static struct {
    pgraph_plag_fn check;
    int bad;                        /* the current part's matrix was rewritten */
    uint32_t cb;                    /* its block (guest VA) */
    const uint8_t *snap;            /* the block as the title drew the part */
    const uint8_t *patch;           /* va_fetch4: read the block from snap */
} g_plag;
void pgraph_d3d11_set_plag_check(pgraph_plag_fn fn) { g_plag.check = fn; }
/* Draw fence: the last NOP 0x5C000000 | n translated (XBOX_FIX_CONSTWAIT, pumplag.c). */
volatile uint32_t pgraph_d3d11_fork_fence;

static void nop_note(uint32_t param)
{
    unsigned i;
    g_nop.total++;
    if ((param >> 24) == 0x5Cu) {   /* fork draw fence (port/src/pumplag.c): passed */
        pgraph_d3d11_fork_fence = param & 0xFFFFFFu;
        return;
    }
    if ((param >> 24) == 0x5Du && g_plag.check) {
        uint32_t q = param & 0xFFFFFFu;
        g_plag.bad = q ? g_plag.check(q, &g_plag.cb, &g_plag.snap) == 1 : 0;
        return;
    }
    if ((param >> 16) == NV2A_PASS_TAG_MAGIC) {
        g_nop.tags++;
        g_nop.tag_type[(param >> 14) & 3u]++;
        pass_tag_seen(param);
        return;                     /* tags stay out of the title's histogram */
    }
    if ((param >> 16) == PGRAPH_HUD_TAG_MAGIC || (param >> 16) == PGRAPH_HUD_TAG_MAGIC_Y ||
        (param >> 16) == PGRAPH_BOX_TAG_MAGIC ||    /* 16:9 frame */
        (param >> 16) == PGRAPH_PANEL_TAG_MAGIC || (param >> 16) == PGRAPH_PANEL_TAG_MAGIC_R) {
        hud_tag_seen(param);
        return;
    }
    for (i = 0; i < g_nop.n; i++)
        if (g_nop.slot[i].param == param) { g_nop.slot[i].hits++; return; }
    if (g_nop.n < NOP_HIST_SLOTS) {
        g_nop.slot[g_nop.n].param = param;
        g_nop.slot[g_nop.n].hits = 1;
        g_nop.n++;
    } else {
        g_nop.other++;
    }
}

void pgraph_d3d11_report_nops(void)
{
    static unsigned long long last = ~0ull;
    unsigned i;
    if (g_nop.total == last) return;
    last = g_nop.total;
    fprintf(stderr, "[NV2A-NOP] %llu NOP (0x0100); %u distinct params%s; "
            "pass tags %llu (begin %llu end %llu group %llu ortho %llu)\n",
            g_nop.total, g_nop.n, g_nop.other ? " (table full)" : "",
            g_nop.tags, g_nop.tag_type[0], g_nop.tag_type[1],
            g_nop.tag_type[2], g_nop.tag_type[3]);
    if (g_ph.images) {
        int k;
        fprintf(stderr, "[NV2A-NOP] phases over %llu images (draws):", g_ph.images);
        for (k = 0; k < PGRAPH_PHASE_COUNT; k++)
            fprintf(stderr, " %s %llu", k_phase_name[k], g_ph.total[k]);
        fprintf(stderr, " | ooo %llu lost_end %llu gaps %llu\n", g_ph.ooo, g_ph.lost_end, g_ph.gaps);
    }
    for (i = 0; i < g_nop.n && i < 16; i++)
        fprintf(stderr, "[NV2A-NOP]   param %08X  x%llu\n",
                g_nop.slot[i].param, g_nop.slot[i].hits);
    if (g_nop.n > 16 || g_nop.other)
        fprintf(stderr, "[NV2A-NOP]   ... %u more params, %llu NOP beyond the table\n",
                g_nop.n > 16 ? g_nop.n - 16 : 0, g_nop.other);
}

static uint32_t va_read_color(uint32_t idx)
{
    int slot        = va_slot_diffuse();
    uint32_t fmt    = g_pg.vattr[slot].format;
    uint32_t type   = fmt & 0x0Fu;
    uint32_t size   = (fmt >> 4) & 0x0Fu;
    uint32_t stride = (fmt >> 8) & 0xFFFFFFu; /* STRIDE is bits 8..31 (cxbx
                                             * NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE
                                             * is 0xFFFFFF00), not 8 bits -- an 8-bit
                                             * mask silently truncates any vertex wider
                                             * than 255 bytes to a bogus stride. */
    const uint8_t *p;

    va_const_init();
    if (size == 0 || stride == 0) {
        /* Unbound: the constant register is the value, not white. */
        const float *c = g_pg.vattr_const[slot];
        #define CC8(v) ((uint32_t)((v) <= 0.0f ? 0.0f : ((v) >= 1.0f ? 255.0f : (v) * 255.0f)))
        return (CC8(c[3]) << 24) | (CC8(c[0]) << 16) | (CC8(c[1]) << 8) | CC8(c[2]);
        #undef CC8
    }

    if (type == NV2A_VA_TYPE_F) {
        float r = va_read(slot, idx, 0, 1.0f);
        float g = va_read(slot, idx, 1, 1.0f);
        float b = va_read(slot, idx, 2, 1.0f);
        float a = va_read(slot, idx, 3, 1.0f);
        #define CLAMP8(v) ((uint32_t)((v) <= 0.0f ? 0.0f : ((v) >= 1.0f ? 255.0f : (v) * 255.0f)))
        return (CLAMP8(a) << 24) | (CLAMP8(r) << 16) | (CLAMP8(g) << 8) | CLAMP8(b);
        #undef CLAMP8
    }

    p = va_ptr(g_pg.vattr[slot].offset + idx * stride, stride);
    if (!p)
        return 0xFFFFFFFFu;

    if (type == NV2A_VA_TYPE_UB_D3D) {
        /* Stored B,G,R,A in memory order, which is D3DCOLOR verbatim. */
        uint32_t c;
        memcpy(&c, p, 4);
        return c;
    }
    if (type == NV2A_VA_TYPE_UB_OGL) {
        /* Stored R,G,B,A in memory order. */
        return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
               ((uint32_t)p[1] << 8)  |  (uint32_t)p[2];
    }
    return 0xFFFFFFFFu;
}

/* Render state shared by both draw paths. The inline path still sets this up
 * itself, interleaved with its game-specific texture selection; this is the
 * generic subset the vertex-array path needs. */
/* ══════════════════════════════════════════════════════════════════════
 * Texture upload
 *
 * The title programs SET_TEXTURE_OFFSET / _FORMAT / _CONTROL0 per stage and
 * expects the GPU to sample straight out of guest memory. Until now the draw
 * path bound no texture at all and selected DIFFUSE, so everything rasterised
 * flat white; the inline path had a hardcoded table of VRAM offsets to asset
 * names, which only ever covered the handful of surfaces someone had looked
 * up by hand.
 *
 * Uncompressed Xbox textures are swizzled (Morton order). The mask generation
 * and fill_pattern below follow the nouveau/xemu routine that cxbx-reloaded
 * also carries (reference/cxbx-reloaded/src/devices/video/swizzle.cpp) -- it
 * packs the remaining bits of the larger axis tightly when the texture is not
 * square, which a naive bit-interleave gets wrong.
 *
 * Everything uncompressed is converted to A8R8G8B8 so one upload path covers
 * every source format. DXT blocks are stored linearly on Xbox and map straight
 * onto BC1/2/3, so those pass through untouched.
 * ══════════════════════════════════════════════════════════════════════ */

/* Format register field accessors (cxbx nv2a_regs.h:1431-1498). */
#define TEXFMT_COLOR(f)      (((f) >> 8)  & 0xFFu)
#define TEXFMT_MIPS(f)       (((f) >> 16) & 0x0Fu)
#define TEXFMT_SIZE_U(f)     (((f) >> 20) & 0x0Fu)
#define TEXFMT_SIZE_V(f)     (((f) >> 24) & 0x0Fu)

/* Xbox D3DFMT codes understood by our d3d8 shim. */
#define XFMT_A8R8G8B8  6
/* The *linear* A8R8G8B8 code. This path has already decoded the NV2A swizzle
 * into the locked rect, so the texture has to be declared linear -- the D3D8
 * shim's UnlockRect unswizzles anything whose format is a swizzled Xbox code,
 * and 6 is one. Declaring it as 6 made the data get unswizzled a second time
 * on its way to the GPU, which scrambled every texture in the title. It was
 * invisible to inspection because LockRect hands back the CPU-side copy, taken
 * before that second pass: a dumped font atlas read back perfectly while the
 * screen showed shredded glyphs. */
#define XFMT_LIN_A8R8G8B8 0x12
#define XFMT_DXT1     12
#define XFMT_DXT3     14
#define XFMT_DXT5     15

static void tex_swizzle_masks(unsigned w, unsigned h, uint32_t *mx, uint32_t *my)
{
    uint32_t x = 0, y = 0, bit = 1, mask_bit = 1;
    int done;
    do {
        done = 1;
        if (bit < w) { x |= mask_bit; mask_bit <<= 1; done = 0; }
        if (bit < h) { y |= mask_bit; mask_bit <<= 1; done = 0; }
        bit <<= 1;
    } while (!done);
    *mx = x; *my = y;
}

static uint32_t tex_fill_pattern(uint32_t pattern, uint32_t value)
{
    uint32_t result = 0, bit = 1;
    while (value) {
        if (pattern & bit) {
            result |= (value & 1) ? bit : 0;
            value >>= 1;
        }
        bit <<= 1;
    }
    return result;
}

/* One source texel -> 0xAARRGGBB. Returns 0 for a format not decoded yet. */
static int tex_texel_to_argb(const uint8_t *p, uint32_t color, uint32_t *out)
{
    switch (color) {
    case 0x00: /* SZ_Y8 */
    case 0x13: /* LU_IMAGE_Y8 */
        *out = 0xFF000000u | (p[0] * 0x010101u);
        return 1;
    case 0x19: /* SZ_A8 */
    case 0x1F: /* LU_IMAGE_A8 */
        *out = ((uint32_t)p[0] << 24) | 0x00FFFFFFu;
        return 1;
    case 0x01: /* SZ_AY8 */
    case 0x1B: /* LU_IMAGE_AY8 */
        *out = ((uint32_t)p[0] << 24) | (p[0] * 0x010101u);
        return 1;
    case 0x1A: /* SZ_A8Y8 */
    case 0x20: /* LU_IMAGE_A8Y8 */
        *out = ((uint32_t)p[1] << 24) | (p[0] * 0x010101u);
        return 1;
    case 0x02: /* SZ_A1R5G5B5 */
    case 0x03: /* SZ_X1R5G5B5 */
    case 0x10: /* LU_IMAGE_A1R5G5B5 */
    case 0x1C: /* LU_IMAGE_X1R5G5B5 */
    {
        uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
        uint32_t r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
        uint32_t a = (color == 0x02 || color == 0x10)
                   ? ((v & 0x8000) ? 0xFFu : 0u) : 0xFFu;
        *out = (a << 24) | ((r * 255 / 31) << 16)
             | ((g * 255 / 31) << 8) | (b * 255 / 31);
        return 1;
    }
    case 0x04: /* SZ_A4R4G4B4 */
    case 0x1D: /* LU_IMAGE_A4R4G4B4 */
    {
        uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
        uint32_t a = (v >> 12) & 0xF, r = (v >> 8) & 0xF;
        uint32_t g = (v >> 4) & 0xF, b = v & 0xF;
        *out = ((a * 17) << 24) | ((r * 17) << 16) | ((g * 17) << 8) | (b * 17);
        return 1;
    }
    case 0x05: /* SZ_R5G6B5 */
    case 0x11: /* LU_IMAGE_R5G6B5 */
    {
        uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
        uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
        *out = 0xFF000000u | ((r * 255 / 31) << 16)
             | ((g * 255 / 63) << 8) | (b * 255 / 31);
        return 1;
    }
    case 0x06: /* SZ_A8R8G8B8 */
    case 0x12: /* LU_IMAGE_A8R8G8B8 */
        *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
             | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        return 1;
    case 0x07: /* SZ_X8R8G8B8 */
    case 0x1E: /* LU_IMAGE_X8R8G8B8 */
        *out = 0xFF000000u | (uint32_t)p[0] | ((uint32_t)p[1] << 8)
             | ((uint32_t)p[2] << 16);
        return 1;
    default:
        return 0;
    }
}

static uint32_t tex_bytes_per_texel(uint32_t color)
{
    switch (color) {
    case 0x00: case 0x01: case 0x13: case 0x19: case 0x1B: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1A: case 0x1C: case 0x1D: case 0x20:
        return 2;
    case 0x06: case 0x07: case 0x12: case 0x1E:
        return 4;
    default:
        return 0;
    }
}

/* The SZ_* family is swizzled; LU_IMAGE_* and the DXT codes are linear. */
static int tex_is_swizzled(uint32_t color)
{
    if (color == 0x0C || color == 0x0E || color == 0x0F) return 0;
    return color < 0x10;
}

static int tex_dxt_format(uint32_t color, uint32_t *xfmt, uint32_t *block_bytes)
{
    switch (color) {
    case 0x0C: *xfmt = XFMT_DXT1; *block_bytes = 8;  return 1;
    case 0x0E: *xfmt = XFMT_DXT3; *block_bytes = 16; return 1;
    case 0x0F: *xfmt = XFMT_DXT5; *block_bytes = 16; return 1;
    default:   return 0;
    }
}

/* Build (or reuse) a D3D texture for one stage from guest memory. */
/* Uploaded textures, keyed by the (VRAM offset, format) pair they were built
 * from.
 *
 * This held 16 textures in a linear list. A race scene binds far
 * more than that each frame, so the least recently used one was always the
 * next one needed: every bind re-decoded its texture (swizzle, format
 * conversion, upload) and texture decoding took a fifth of the render thread,
 * which is what the game waits on. Now 2048 entries in 1024 hash buckets;
 * the least recently used entry is evicted only when all are in use. */
#define TC_POOL    2048
#define TC_BUCKETS 1024
typedef struct {
    uint32_t offset, format, sig;
    IDirect3DTexture8 *tex;
    unsigned last_use;
    int next;                   /* bucket chain, or free list */
    uint32_t hd_bytes;          /* GPU bytes of an HD replacement, else 0 */
    uint32_t hd_tick;           /* pump tick of its last use */
    HdJob   *hd_job;            /* its HD copy, being loaded */
} TcEntry;
static TcEntry g_tc[TC_POOL];
static int g_tc_head[TC_BUCKETS];
static int g_tc_free = -1, g_tc_ready = 0;
static unsigned g_texcache_clock, g_tc_uploads, g_tc_live;
static uint64_t g_tc_hd_bytes;  /* live HD replacements, in GPU bytes */
static uint32_t g_sig_gen = 1;  /* pump tick, see tex_sig */

static unsigned tc_bucket(uint32_t offset, uint32_t format)
{
    return ((offset >> 5) * 2654435761u ^ format * 40503u) >> 22 & (TC_BUCKETS - 1);
}

static void tc_init(void)
{
    int i;
    for (i = 0; i < TC_BUCKETS; i++) g_tc_head[i] = -1;
    for (i = 0; i < TC_POOL; i++) g_tc[i].next = i + 1 < TC_POOL ? i + 1 : -1;
    g_tc_free = 0;
    g_tc_ready = 1;
}

/* Unlink entry i from its bucket, release its texture, return it to the pool. */
static void tc_remove_why(int i, int over_budget);
static void tc_remove(int i) { tc_remove_why(i, 0); }
static void tc_remove_why(int i, int over_budget)
{
    unsigned b = tc_bucket(g_tc[i].offset, g_tc[i].format);
    int *link = &g_tc_head[b];
    while (*link >= 0 && *link != i) link = &g_tc[*link].next;
    if (*link == i) *link = g_tc[i].next;
    if (g_tc[i].tex) g_tc[i].tex->lpVtbl->Release(g_tc[i].tex);
    g_tc[i].tex = NULL;
    if (g_tc[i].hd_bytes) {
        g_tc_hd_bytes -= g_tc[i].hd_bytes;
        hdtex_note_evicted(g_tc[i].hd_bytes, over_budget);
        g_tc[i].hd_bytes = 0;
    }
    if (g_tc[i].hd_job) {
        hdtex_cancel(g_tc[i].hd_job);
        g_tc[i].hd_job = NULL;
    }
    g_tc[i].next = g_tc_free;
    g_tc_free = i;
    g_tc_live--;
}

extern int d3d8_pump_cache_on(void);
static uint32_t tex_sig_raw(uint32_t offset, uint32_t size);

/* XBOX_FIX_PUMP_CACHE: the signature of a texture range is
 * computed once per pump round (nv2a_live_pb_tick) instead of on every bind
 * (~2,600 per frame). Exact: the game rewrites a texture already referenced
 * by commands only after a fence, which the pump acknowledges only at the
 * end of its round (ack late); within a round, the content read is the
 * same. Direct-mapped cache, invalidated by generation. */
static struct { uint32_t offset, size, gen, sig; } g_sigc[2048];

void pgraph_d3d11_tick_begin(void) { g_sig_gen++; }

static uint32_t tex_sig(uint32_t offset, uint32_t size)
{
    if (d3d8_pump_cache_on()) {
        unsigned h = ((offset >> 6) ^ (offset >> 17) ^ (size * 0x9E3779B1u)) & 2047u;
        if (g_sigc[h].gen == g_sig_gen && g_sigc[h].offset == offset && g_sigc[h].size == size)
            return g_sigc[h].sig;
        g_sigc[h].offset = offset; g_sigc[h].size = size; g_sigc[h].gen = g_sig_gen;
        return g_sigc[h].sig = tex_sig_raw(offset, size);
    }
    return tex_sig_raw(offset, size);
}

static uint32_t tex_sig_raw(uint32_t offset, uint32_t size)
{
    const uint32_t *p;
    uint32_t h = 2166136261u, i, n, step;
    if (size < 4) return 0;
    if (g_perf_on) perf_count(PC_TEXSIG, 1);
    p = (const uint32_t *)va_ptr(offset, size);
    if (!p) return 0;
    n = size / 4;
    step = n / 97u ? n / 97u : 1u;
    for (i = 0; i < n; i += step)
        h = (h ^ p[i]) * 16777619u;
    return (h ^ p[n - 1]) * 16777619u;
}

/* HD replacements past their GPU budget go, least recently used
 * first -- never one used in this pump tick (it may still be bound; the budget
 * is then exceeded until the next HD texture arrives). */
static void tc_hd_budget(void)
{
    while (g_tc_hd_bytes > hdtex_budget()) {
        int k, victim = -1;
        unsigned oldest = ~0u;
        for (k = 0; k < TC_POOL; k++)
            if (g_tc[k].tex && g_tc[k].hd_bytes && g_tc[k].hd_tick != g_sig_gen &&
                g_tc[k].last_use < oldest) { oldest = g_tc[k].last_use; victim = k; }
        if (victim < 0) break;
        tc_remove_why(victim, 1);
    }
}

static IDirect3DTexture8 *texcache_find(uint32_t offset, uint32_t format, uint32_t sig)
{
    int i;
    if (!g_tc_ready) tc_init();
    for (i = g_tc_head[tc_bucket(offset, format)]; i >= 0; i = g_tc[i].next)
        if (g_tc[i].offset == offset && g_tc[i].format == format) {
            if (g_tc[i].sig != sig) {          /* rewritten: drop it */
                tc_remove(i);
                return NULL;
            }
            g_tc[i].last_use = ++g_texcache_clock;
            g_tc[i].hd_tick = g_sig_gen;
            if (g_tc[i].hd_job) {              /* its HD copy is ready? */
                IDirect3DTexture8 *hd;
                uint32_t bytes;
                if (hdtex_collect(g_tc[i].hd_job, &hd, &bytes)) {
                    g_tc[i].hd_job = NULL;
                    if (hd) {
                        g_tc[i].tex->lpVtbl->Release(g_tc[i].tex);
                        g_tc[i].tex = hd;
                        g_tc[i].hd_bytes = bytes;
                        g_tc_hd_bytes += bytes;
                        tc_hd_budget();
                    }
                }
            }
            return g_tc[i].tex;
        }
    return NULL;
}

static void texcache_put(uint32_t offset, uint32_t format, uint32_t sig, IDirect3DTexture8 *tex,
                         HdJob *hd_job)
{
    unsigned b;
    int i;
    if (!g_tc_ready) tc_init();
    if (g_tc_free < 0) {                        /* full: evict the least recently used */
        int k, victim = 0;
        unsigned oldest = ~0u;
        for (k = 0; k < TC_POOL; k++)
            if (g_tc[k].tex && g_tc[k].last_use < oldest) { oldest = g_tc[k].last_use; victim = k; }
        tc_remove(victim);
    }
    i = g_tc_free;
    g_tc_free = g_tc[i].next;
    b = tc_bucket(offset, format);
    g_tc[i].offset   = offset;
    g_tc[i].format   = format;
    g_tc[i].sig      = sig;
    g_tc[i].tex      = tex;
    g_tc[i].last_use = ++g_texcache_clock;
    g_tc[i].hd_bytes = 0;
    g_tc[i].hd_tick  = g_sig_gen;
    g_tc[i].hd_job   = hd_job;
    g_tc[i].next     = g_tc_head[b];
    g_tc_head[b]     = i;
    g_tc_live++;
    g_tc_uploads++;
    {   /* one line per 600 frames: uploads per frame should fall to ~0 */
        static unsigned last_seq = 0, last_up = 0;
        unsigned seq = d3d8_PresentSeq();
        if (seq - last_seq >= 600) {
            fprintf(stderr, "[TEXCACHE] %u textures kept, %u uploads in the last %u frames\n",
                    g_tc_live, g_tc_uploads - last_up, seq - last_seq);
            last_seq = seq;
            last_up = g_tc_uploads;
        }
    }
}

/* Test only -- XBOX_HD_TEXTURES_ALT=<N>: from present N on, one
 * presented frame in two (odd PresentSeq) binds every texture freshly decoded
 * from the guest bytes, bypassing the texture cache and everything the HD path added
 * to it (no lookup, no HD request, no swap, no budget); the other frames take
 * the normal path. The decode code is fork/main's. In a frozen scene
 * (pump_ident, N after the pause starts) every capture must then be identical
 * whatever its parity: with HD textures off, the cache hands out exactly the
 * guest textures. Within one fresh frame a texture is decoded once (a table
 * of that frame only); fresh textures are released two presents later. */
static int hd_alt_fresh(void)
{
    static long from = -1;
    unsigned seq;
    if (from < 0) { const char *e = getenv("XBOX_HD_TEXTURES_ALT"); from = e && e[0] ? atol(e) : 0; if (from < 0) from = 0; }
    if (!from) return 0;
    seq = d3d8_PresentSeq();
    return seq >= (unsigned)from && (seq & 1u);
}

static struct { uint32_t offset, format, sig; unsigned seq; IDirect3DTexture8 *tex; } g_alt_memo[4096];

static IDirect3DTexture8 *hd_alt_memo_find(uint32_t offset, uint32_t format, uint32_t sig)
{
    unsigned h = (offset * 2654435761u ^ format * 40503u ^ sig) & 4095u, seq = d3d8_PresentSeq();
    if (g_alt_memo[h].seq == seq && g_alt_memo[h].tex && g_alt_memo[h].offset == offset &&
        g_alt_memo[h].format == format && g_alt_memo[h].sig == sig)
        return g_alt_memo[h].tex;
    return NULL;
}

static void hd_alt_memo_put(uint32_t offset, uint32_t format, uint32_t sig, IDirect3DTexture8 *tex)
{
    unsigned h = (offset * 2654435761u ^ format * 40503u ^ sig) & 4095u;
    g_alt_memo[h].offset = offset; g_alt_memo[h].format = format; g_alt_memo[h].sig = sig;
    g_alt_memo[h].seq = d3d8_PresentSeq(); g_alt_memo[h].tex = tex;
}

static struct { IDirect3DTexture8 *tex; unsigned seq; } g_alt_keep[16384];
static int g_alt_n;

static void hd_alt_keep(IDirect3DTexture8 *t)
{
    static unsigned last = ~0u;
    unsigned seq = d3d8_PresentSeq();
    if (seq != last) {                       /* once per frame: drop the old ones */
        int i, k = 0;
        for (i = 0; i < g_alt_n; i++)
            if (seq - g_alt_keep[i].seq >= 2) g_alt_keep[i].tex->lpVtbl->Release(g_alt_keep[i].tex);
            else g_alt_keep[k++] = g_alt_keep[i];
        g_alt_n = k;
        last = seq;
    }
    if (g_alt_n < (int)(sizeof g_alt_keep / sizeof g_alt_keep[0])) {
        g_alt_keep[g_alt_n].tex = t;
        g_alt_keep[g_alt_n].seq = seq;
        g_alt_n++;
    }                                        /* full: kept alive (test only) */
}

/* Bytes in mip level `level` of a w x h texture. */
static uint32_t tex_level_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t level)
{
    uint32_t lw = w >> level, lh = h >> level, xf, bb;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    if (tex_dxt_format(color, &xf, &bb))
        return ((lw + 3) / 4) * ((lh + 3) / 4) * bb;
    return lw * lh * tex_bytes_per_texel(color);
}

static IDirect3DTexture8 *tex_upload_impl(IDirect3DDevice8 *dev, int stage);

/* Timed wrapper for the frame-rate log (XBOX_FPS_LOG). */
extern volatile long g_tex_uploads_log;
extern volatile double g_tex_upload_ms;
static IDirect3DTexture8 *tex_upload(IDirect3DDevice8 *dev, int stage)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER a, b;
    unsigned before = g_tc_uploads;
    IDirect3DTexture8 *t;
    double pt = g_perf_on ? perf_now() : 0.0;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    t = tex_upload_impl(dev, stage);
    if (g_perf_on) perf_add(PZ_TEX, perf_now() - pt);
    if (g_tc_uploads != before) {
        QueryPerformanceCounter(&b);
        g_tex_uploads_log++;
        if (g_perf_on) perf_count(PC_TEXUP, 1);
        g_tex_upload_ms += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    }
    return t;
}

static IDirect3DTexture8 *tex_upload_impl(IDirect3DDevice8 *dev, int stage)
{
    uint32_t fmt    = g_pg.tex[stage].format;
    uint32_t offset = g_pg.tex[stage].offset;
    uint32_t color  = TEXFMT_COLOR(fmt);
    uint32_t w      = 1u << TEXFMT_SIZE_U(fmt);
    uint32_t h      = 1u << TEXFMT_SIZE_V(fmt);
    uint32_t xfmt = 0, block_bytes = 0, sig = 0;
    int fresh = hd_alt_fresh();              /* test (XBOX_HD_TEXTURES_ALT) */
    const uint8_t *src;
    IDirect3DTexture8 *tex = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    int linear = !tex_is_swizzled(color) && !tex_dxt_format(color, &xfmt, &block_bytes);

    /* A linear image's size is its IMAGE_RECT; the format's log2 size fields
     * are zero (they decoded as 1x1, so a linear texture was one texel). */
    if (linear && g_pg.tex[stage].image_rect) {
        w = g_pg.tex[stage].image_rect >> 16;
        h = g_pg.tex[stage].image_rect & 0xFFFFu;
    }
    xfmt = 0; block_bytes = 0;

    /* A linear texture at the frame buffer that was just presented is the
     * previous frame, which exists only on the host: SSX's chrome "Master"
     * outfits reflect it. Bind the D3D layer's copy of it; the
     * shader normalises the coordinates by the image rect, so the copy's
     * render resolution does not matter. Not cached -- it changes every frame. */
    if (linear && offset && offset == g_pg.prev_surface_offset) {
        IDirect3DTexture8 *pf = d3d8_PrevFrameTexture();
        if (pf) {
            g_pg.tex[stage].d3d = pf;
            g_pg.tex[stage].d3d_offset = offset;
            g_pg.tex[stage].d3d_format = fmt;
            return pf;
        }
    }

    if (!dev || w == 0 || h == 0 || w > 4096 || h > 4096)
        return NULL;

    /* XBOX_TEX_DUMP=<prefix> writes each uploaded texture as a 24-bit BMP.
     * Whether a garbled glyph is a sampling problem or a decode problem is not
     * answerable from the framebuffer -- the atlas itself has to be looked at. */
    {
        static int want = -1;
        if (want < 0) {
            const char *e = getenv("XBOX_TEX_DUMP");
            want = (e && e[0]) ? 1 : 0;
        }
        (void)want;
    }

    /* Mip levels. The chain sits in memory straight after level
     * 0, each level half the size down to 1 texel (a DXT level is at least
     * one block), each swizzled on its own -- as xemu lays it out. Only
     * level 0 used to be uploaded, which left distant snow and ice shimmering
     * with every texel aliasing. Linear images carry no mips.
     * XBOX_NV2A_MIPS=0 (diagnostic) uploads level 0 only. */
    uint32_t levels = TEXFMT_MIPS(fmt), total = 0, level, dxt;
    dxt = tex_dxt_format(color, &xfmt, &block_bytes);
    {
        static int mips_on = -1;
        uint32_t full = 1, d = w > h ? w : h;
        if (mips_on < 0) { const char *e = getenv("XBOX_NV2A_MIPS"); mips_on = !(e && e[0] == '0'); }
        while (d > 1) { d >>= 1; full++; }
        if (levels < 1) levels = 1;
        if (levels > full) levels = full;
        if (!mips_on || (!dxt && !tex_is_swizzled(color))) levels = 1;
        if (!dxt && tex_bytes_per_texel(color) == 0) levels = 1;
        for (level = 0; level < levels; level++)
            total += tex_level_bytes(color, w, h, level);
        /* A chain that runs off the end of mapped memory is not a chain. */
        if (levels > 1 && !va_ptr(offset, total)) {
            levels = 1;
            total = tex_level_bytes(color, w, h, 0);
        }
    }

    /* Already built for this exact (offset, format) and content? */
    {
        uint32_t sz;
        IDirect3DTexture8 *hit;
        if (dxt)
            sz = total;
        else {
            uint32_t bpt = tex_bytes_per_texel(color);
            uint32_t pitch = tex_is_swizzled(color) ? w * bpt
                           : ((g_pg.tex[stage].control1 >> 16) & 0xFFFFu);
            if (!pitch) pitch = w * bpt;
            sz = tex_is_swizzled(color) ? total : pitch * h;
        }
        sig = tex_sig(offset, sz);
        if (linear) sig ^= g_pg.tex[stage].image_rect * 0x9E3779B1u;   /* same bytes, other shape */
        hit = fresh ? hd_alt_memo_find(offset, fmt, sig) : texcache_find(offset, fmt, sig);
        if (hit) {
            g_pg.tex[stage].d3d = hit;
            g_pg.tex[stage].d3d_offset = offset;
            g_pg.tex[stage].d3d_format = fmt;
            return hit;
        }
    }

    if (dxt) {
        uint32_t off = 0;

        src = va_ptr(offset, total);
        if (!src) return NULL;

        hr = dev->lpVtbl->CreateTexture(dev, w, h, levels, 0, xfmt, 0, &tex);
        if (FAILED(hr) || !tex) return NULL;

        for (level = 0; level < levels; level++) {
            uint32_t n = tex_level_bytes(color, w, h, level);
            memset(&lr, 0, sizeof lr);
            if (SUCCEEDED(tex->lpVtbl->LockRect(tex, level, &lr, NULL, 0)) && lr.pBits) {
                memcpy(lr.pBits, src + off, n);
                tex->lpVtbl->UnlockRect(tex, level);
            }
            off += n;
        }
    } else {
        uint32_t bpt = tex_bytes_per_texel(color);
        uint32_t pitch, need, y, x, off = 0;
        int swizzled = tex_is_swizzled(color);

        if (bpt == 0) return NULL;

        /* Linear textures carry their own pitch; swizzled ones are tightly
         * packed by definition. */
        pitch = swizzled ? (w * bpt)
                         : ((g_pg.tex[stage].control1 >> 16) & 0xFFFFu);
        if (pitch == 0) pitch = w * bpt;

        need = swizzled ? total : (pitch * h);
        src = va_ptr(offset, need);
        if (!src) return NULL;

        hr = dev->lpVtbl->CreateTexture(dev, w, h, levels, 0, XFMT_LIN_A8R8G8B8, 0, &tex);
        if (FAILED(hr) || !tex) return NULL;

        for (level = 0; level < levels; level++) {
            uint32_t lw = w >> level, lh = h >> level;
            uint32_t mask_x = 0, mask_y = 0;
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            if (swizzled)
                tex_swizzle_masks(lw, lh, &mask_x, &mask_y);
            memset(&lr, 0, sizeof lr);
            if (SUCCEEDED(tex->lpVtbl->LockRect(tex, level, &lr, NULL, 0)) && lr.pBits) {
                const uint8_t *ls = src + off;
                for (y = 0; y < lh; y++) {
                    uint32_t *row = (uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch);
                    for (x = 0; x < lw; x++) {
                        const uint8_t *p = swizzled
                            ? ls + bpt * (tex_fill_pattern(mask_x, x)
                                        | tex_fill_pattern(mask_y, y))
                            : ls + y * pitch + x * bpt;
                        uint32_t argb;
                        if (!tex_texel_to_argb(p, color, &argb))
                            argb = 0xFFFF00FFu;   /* undecoded format: obvious magenta */
                        row[x] = argb;
                    }
                }
                tex->lpVtbl->UnlockRect(tex, level);
            }
            off += lw * lh * bpt;
        }
    }

    {
        /* XBOX_TEX_RAW=<dir> (diagnostic): the guest bytes of each
         * decoded texture (whole mip chain, as read) to
         * <dir>/<offset>_c<colour>_<w>x<h>_L<levels>.raw, for an offline
         * decode independent of this one. XBOX_TEX_RAW_COLORS=05,06,0C
         * limits the colour formats; XBOX_TEX_RAW_FROM=<present> starts
         * there (the race, not the menus); at most 2000 writes. */
        static int init;
        static const char *dir;
        static char colors[64];
        static unsigned rn, from;
        if (!init) {
            const char *c = getenv("XBOX_TEX_RAW_COLORS");
            init = 1; dir = getenv("XBOX_TEX_RAW");
            if (getenv("XBOX_TEX_RAW_FROM")) from = (unsigned)atoi(getenv("XBOX_TEX_RAW_FROM"));
            if (c) { char *e; const char *q = c; while (*q) { unsigned long v = strtoul(q, &e, 16); if (e == q) break; if (v < 64) colors[v] = 1; q = *e ? e + 1 : e; } }
            else memset(colors, 1, sizeof colors);
        }
        if (dir && tex && rn < 2000 && d3d8_PresentSeq() >= from && color < 64 && colors[color]) {
            uint32_t n = dxt ? total : (tex_is_swizzled(color) ? total : 0);
            const uint8_t *raw = n ? va_ptr(offset, n) : NULL;
            if (raw) {
                char path[600];
                FILE *f;
                snprintf(path, sizeof path, "%s/%08X_c%02X_%ux%u_L%u.raw", dir, offset, (unsigned)color, w, h, levels);
                if ((f = fopen(path, "wb")) != NULL) { fwrite(raw, 1, n, f); fclose(f); rn++; }
            }
        }
    }
    {
        const char *pre = getenv("XBOX_TEX_DUMP");
        static unsigned dn = 0;
        D3DLOCKED_RECT dr;
        if (pre && tex && dn < 40 &&
            SUCCEEDED(tex->lpVtbl->LockRect(tex, 0, &dr, NULL, 0)) && dr.pBits) {
            char path[512];
            FILE *f;
            unsigned row = (w * 3 + 3) & ~3u, imgsz = row * h, y, x;
            unsigned char hdr[54];
            snprintf(path, sizeof path, "%s%02u_%08X_c%02X_%ux%u.bmp",
                     pre, dn++, offset, (unsigned)color, w, h);
            f = fopen(path, "wb");
            if (f) {
                memset(hdr, 0, sizeof hdr);
                hdr[0] = 'B'; hdr[1] = 'M';
                *(unsigned *)(hdr + 2)  = 54 + imgsz;
                *(unsigned *)(hdr + 10) = 54;
                *(unsigned *)(hdr + 14) = 40;
                *(int *)(hdr + 18) = (int)w;
                *(int *)(hdr + 22) = (int)h;
                *(unsigned short *)(hdr + 26) = 1;
                *(unsigned short *)(hdr + 28) = 24;
                *(unsigned *)(hdr + 34) = imgsz;
                fwrite(hdr, 1, sizeof hdr, f);
                for (y = 0; y < h; y++) {
                    const unsigned char *sp =
                        (const unsigned char *)dr.pBits + (h - 1 - y) * dr.Pitch;
                    unsigned char pad[4] = {0, 0, 0, 0};
                    /* Premultiply by alpha: a font atlas is white RGB with
                     * the glyph shape entirely in the alpha channel, so an
                     * RGB-only dump is a blank white square. */
                    for (x = 0; x < w; x++) {
                        unsigned a = sp[x * 4 + 3];
                        unsigned char px[3];
                        px[0] = (unsigned char)((sp[x * 4 + 0] * a) / 255);
                        px[1] = (unsigned char)((sp[x * 4 + 1] * a) / 255);
                        px[2] = (unsigned char)((sp[x * 4 + 2] * a) / 255);
                        fwrite(px, 1, 3, f);
                    }
                    fwrite(pad, 1, row - w * 3, f);
                }
                fclose(f);
                fprintf(stderr, "  [TEX] %ux%u color %02X from %08X tex=%p -> %s\n",
                        w, h, (unsigned)color, offset, (void *)tex, path);
                fflush(stderr);
            }
            tex->lpVtbl->UnlockRect(tex, 0);
        }
    }

    {   /* XBOX_TEXUP_LOG=1 (diagnostic): every texture decoded, for pairing
         * with a RAM dump (savemem.py) and checking its mip levels offline. */
        static int on = -1;
        if (on < 0) { const char *e = getenv("XBOX_TEXUP_LOG"); on = e && e[0] == '1'; }
        if (on)
            fprintf(stderr, "[TEXUP] off %08X fmt %08X color %02X %ux%u levels %u bytes %u\n",
                    offset, fmt, (unsigned)color, w, h, levels, total);
    }
    {
        /* The guest texture is drawn until its HD copy, if the pack
         * has one (XBOX_HD_TEXTURES), is loaded and swapped in by texcache_find.
         * Linear images (render targets, video frames) never. */
        HdJob *job = NULL;
        if (fresh) {
            hd_alt_keep(tex);                /* not cached: the reference frame */
            hd_alt_memo_put(offset, fmt, sig, tex);
        } else {
            /* Button icons (XBOX_BUTTON_ICONS): a button atlas is swapped at
             * once for its composed copy; nothing else is looked up for it. */
            IDirect3DTexture8 *icons = NULL;
            if (!linear && btnicons_on()) {
                uint32_t n0 = tex_level_bytes(color, w, h, 0);
                icons = btnicons_compose(va_ptr(offset, n0), n0, w, h, color, tex);
                if (icons) {
                    tex->lpVtbl->Release(tex);
                    tex = icons;
                }
            }
            if (!icons && !linear && hdtex_on()) {
                uint32_t n0 = tex_level_bytes(color, w, h, 0);
                job = hdtex_request(va_ptr(offset, n0), n0, w, h);
            }
            texcache_put(offset, fmt, sig, tex, job);
        }
    }
    g_pg.tex[stage].d3d        = tex;
    g_pg.tex[stage].d3d_offset = offset;
    g_pg.tex[stage].d3d_format = fmt;

    {
        static int told = 0;
        if (told < 12) {
            uint32_t dummy_fmt, dummy_bb;
            told++;
            fprintf(stderr, "[PGRAPH-D3D11] texture stage %d: %ux%u color=0x%02X %s "
                            "from 0x%08X\n", stage, w, h, color,
                    tex_dxt_format(color, &dummy_fmt, &dummy_bb) ? "DXT"
                        : (tex_is_swizzled(color) ? "swizzled" : "linear"),
                    offset);
            fflush(stderr);
        }
    }
    return tex;
}

static void apply_draw_state(IDirect3DDevice8 *dev)
{
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SPECULARENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
    /* XBOX_TEST_NOBLEND=1 forces blending off. Every draw here uses
     * SRCALPHA/INVSRCALPHA, so a source alpha of zero makes the draw a no-op
     * and is indistinguishable from "the draw never happened" when all you can
     * see is the frame buffer. This separates the two. Diagnostic; default on
     * (i.e. blending enabled) so behaviour is unchanged unless asked for. */
    {
        static int noblend = -1;
        if (noblend < 0) {
            const char *e = getenv("XBOX_TEST_NOBLEND");
            noblend = (e && e[0] == '1') ? 1 : 0;
        }
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,
                                    noblend ? FALSE : TRUE);
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    /* Sample the title's own texture when it has bound one. Stage 0 is
     * enabled by SET_TEXTURE_CONTROL0 bit 30; without that, or when the
     * format is one we cannot decode yet, fall back to vertex colour so the
     * geometry still shows rather than vanishing. */
    {
        IDirect3DTexture8 *t = g_pg.tex[0].enabled ? tex_upload(dev, 0) : NULL;
        if (t) {
            dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)t);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1, 4 /*COLOROP   = MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2, 2 /*COLORARG1 = TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 3, 0 /*COLORARG2 = DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4, 4 /*ALPHAOP   = MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5, 2 /*ALPHAARG1 = TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 6, 0 /*ALPHAARG2 = DIFFUSE*/);
            /* The title's own address modes (checked against xemu): clamping
             * every texture smeared the wrapped ones. */
            {
                uint32_t au = g_pg.tex[0].address & 0xF, av = (g_pg.tex[0].address >> 8) & 0xF;
                dev->lpVtbl->SetTextureStageState(dev, 0, 13, au >= 1 && au <= 4 ? au : 3 /*ADDRESSU*/);
                dev->lpVtbl->SetTextureStageState(dev, 0, 14, av >= 1 && av <= 4 ? av : 3 /*ADDRESSV*/);
            }
        } else {
            dev->lpVtbl->SetTexture(dev, 0, NULL);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1, 2 /*COLOROP  = SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2, 0 /*COLORARG1 = DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4, 2 /*ALPHAOP  = SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5, 0 /*ALPHAARG1 = DIFFUSE*/);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Vertex-program draws
 *
 * After START skips the attract movie, the title's frontend draws its 3D
 * scene as indexed geometry (ARRAY_ELEMENT16, ~8 M commands in 30 s) in
 * object space, transformed by a vertex program. Neither was implemented:
 * the indices were dropped and positions would have been read as screen
 * coordinates. This path runs the program on the CPU (nv2a_vsh_cpu.c) for
 * each vertex, which yields screen-space positions -- the Xbox D3D runtime
 * folds the viewport transform into every program -- and feeds the existing
 * XYZRHW draw.
 *
 * XBOX_VSH_CPU=0 turns it off; XBOX_VSH_LOG=1 prints each new program.
 * ══════════════════════════════════════════════════════════════════════ */

static struct {
    unsigned prog_draws, prog_verts, prog_tris, culled_w, raw_indexed, decoded;
    unsigned z_neg, z_over, xy_off, w_neg;   /* per report window */
} g_vs;

static void idx_push(uint32_t v)
{
    if (g_pg.idx_count == g_pg.idx_cap) {
        uint32_t cap = g_pg.idx_cap ? g_pg.idx_cap * 2 : 4096;
        uint32_t *n = (uint32_t *)realloc(g_pg.idx, cap * sizeof *n);
        if (!n) return;
        g_pg.idx = n;
        g_pg.idx_cap = cap;
    }
    g_pg.idx[g_pg.idx_count++] = v;
}

/* One attribute as a vertex program reads it: absent components default to
 * (0,0,0,1) and a disabled array reads the attribute's constant register. */
static void va_fetch4(int attr, uint32_t idx, float o[4])
{
    uint32_t fmt    = g_pg.vattr[attr].format;
    uint32_t type   = fmt & 0x0Fu;
    uint32_t size   = (fmt >> 4) & 0x0Fu;
    uint32_t stride = (fmt >> 8) & 0xFFFFFFu;
    const uint8_t *p;
    uint32_t k;

    va_const_init();
    if (size == 0) {
        memcpy(o, g_pg.vattr_const[attr], 4 * sizeof(float));
        return;
    }
    o[0] = o[1] = o[2] = 0.0f;
    o[3] = 1.0f;
    p = va_ptr(g_pg.vattr[attr].offset + idx * stride, 16);
    if (!p)
        return;
    if (g_plag.patch && stride == 0) {  /* XBOX_PUMPLAG: the block as the title drew the part */
        uint32_t off = (g_pg.vattr[attr].offset & 0x0FFFFFFFu) - (g_plag.cb & 0x0FFFFFFFu);
        if (off + 16u <= 0xE0u) p = g_plag.patch + off;
    }
    switch (type) {
    case NV2A_VA_TYPE_F:
        for (k = 0; k < size && k < 4; k++) {
            uint32_t bits;
            memcpy(&bits, p + k * 4, 4);
            o[k] = u2f(bits);
        }
        break;
    case NV2A_VA_TYPE_UB_D3D:
        /* D3DCOLOR byte order (B,G,R,A): the hardware swaps red and blue. */
        for (k = 0; k < size && k < 4; k++)
            o[k] = (float)p[k] / 255.0f;
        if (size >= 3) { float t = o[0]; o[0] = o[2]; o[2] = t; }
        break;
    case NV2A_VA_TYPE_UB_OGL:
        for (k = 0; k < size && k < 4; k++)
            o[k] = (float)p[k] / 255.0f;
        break;
    case NV2A_VA_TYPE_S1:
        for (k = 0; k < size && k < 4; k++) {
            int16_t v;
            memcpy(&v, p + k * 2, 2);
            o[k] = (float)v / 32767.0f;
        }
        break;
    case NV2A_VA_TYPE_S32K:
        for (k = 0; k < size && k < 4; k++) {
            int16_t v;
            memcpy(&v, p + k * 2, 2);
            o[k] = (float)v;
        }
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP: {
        /* Packed normal: x 11 bits, y 11 bits, z 10 bits, all signed. */
        uint32_t v;
        int32_t x, y, z;
        memcpy(&v, p, 4);
        x = (int32_t)(v << 21) >> 21;
        y = (int32_t)(v << 10) >> 21;
        z = (int32_t)v >> 22;
        o[0] = (float)x / 1023.0f;
        o[1] = (float)y / 1023.0f;
        o[2] = (float)z / 511.0f;
        break;
    }
    default:
        break;
    }
}

static int vsh_active(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XBOX_VSH_CPU");
        enabled = !(e && e[0] == '0');
    }
    if (!enabled || g_pg.xf_mode != 2)
        return 0;
    if (g_pg.prog_dirty) {
        g_pg.prog_dirty = 0;
        double pt = g_perf_on ? perf_now() : 0.0;
        g_pg.prog_len = vshcpu_decode((const uint32_t (*)[4])g_pg.prog,
                                      (int)g_pg.prog_start, g_pg.prog_dec);
        if (g_perf_on) perf_add(PZ_VSDEC, perf_now() - pt);
        g_vs.decoded++;
        if (g_perf_on) perf_count(PC_DECODE, 1);
        {
            /* Which inputs the program reads: fetching all 16 attributes per
             * vertex was most of the per-vertex cost, and most programs read
             * three or four. */
            int k, j;
            g_pg.prog_inputs = 0;
            g_pg.prog_writes_c = 0;
            g_pg.prog_hash = 14695981039346656037ull;
            for (k = 0; k < g_pg.prog_len; k++) {
                const uint8_t *b = (const uint8_t *)&g_pg.prog_dec[k];
                size_t q;
                for (j = 0; j < 3; j++)
                    if (g_pg.prog_dec[k].src[j].mux == 2 /* v */)
                        g_pg.prog_inputs |= (uint16_t)(1u << g_pg.prog_dec[k].vidx);
                if (g_pg.prog_dec[k].o_mask && !g_pg.prog_dec[k].orb)
                    g_pg.prog_writes_c = 1;
                for (q = 0; q < sizeof g_pg.prog_dec[k]; q++)
                    g_pg.prog_hash = (g_pg.prog_hash ^ b[q]) * 1099511628211ull;
            }
        }
        {
            static int log = -1;
            if (log < 0) { const char *e = getenv("XBOX_VSH_LOG"); log = e && e[0] == '1'; }
            if (log && g_vs.decoded <= 40) {
                int k;
                fprintf(stderr, "[VSH] program at slot %u: %d instructions\n",
                        g_pg.prog_start, g_pg.prog_len);
                for (k = 0; k < g_pg.prog_len; k++) {
                    char b[200];
                    vshcpu_format(&g_pg.prog_dec[k], b, sizeof b);
                    fprintf(stderr, "[VSH]   %2d %s\n", k, b);
                }
                fflush(stderr);
            }
        }
    }
    return g_pg.prog_len > 0;
}

/* Run the program for one vertex. Returns 0 when the vertex is behind the
 * eye (w <= 0): the hardware clips those, and a pre-transformed draw cannot.
 *
 * Program-path vertex: everything the NV2A pixel pipeline reads --
 * screen position + rhw, oD0, oD1, the fog factor and all four texture
 * coordinates as float4 (projective lookups divide by w). d3d8_nv2a.c's
 * vertex shader consumes exactly this layout. */
typedef struct {
    float x, y, z, rhw;
    float d0[4], d1[4];
    float fog;
    float t[4][4];
} ProgVertex;

/* Fog factor from the program's oFog.x, per xemu's vsh.c for vertex-program
 * mode: linear f = p0 + d*p1 - 1; exp f = p0 + 2^(16*d*p1) - 1.5; exp2
 * f = p0 + 2^(-32*(d*p1)^2) - 1.5; the ABS modes take |d|. With fog off the
 * factor is 1, as in xemu. */
static float vsh_fog_factor(float d)
{
    float f;
    const float p0 = g_pg.fog_param[0], p1 = g_pg.fog_param[1];
    if (!g_pg.fog_enable)
        return 1.0f;
    switch (g_pg.fog_mode) {
    case NV097_SET_FOG_MODE_V_LINEAR_ABS:
        d = fabsf(d);
        /* fallthrough */
    case NV097_SET_FOG_MODE_V_LINEAR:
        if (isinf(d)) d = 0.0f;
        f = p0 + d * p1 - 1.0f;
        break;
    case NV097_SET_FOG_MODE_V_EXP_ABS:
        d = fabsf(d);
        f = p0 + exp2f(d * p1 * 16.0f) - 1.5f;
        break;
    case NV097_SET_FOG_MODE_V_EXP:
        if (isinf(d)) d = 0.0f;
        f = p0 + exp2f(d * p1 * 16.0f) - 1.5f;
        break;
    case NV097_SET_FOG_MODE_V_EXP2:
    case NV097_SET_FOG_MODE_V_EXP2_ABS:
        f = p0 + exp2f(-d * d * p1 * p1 * 32.0f) - 1.5f;
        break;
    default:
        return 1.0f;
    }
    if (!(f == f)) return 1.0f;
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

static int vsh_vertex(uint32_t idx, ProgVertex *ov, float *psize)
{
    float in[16][4], out[VSHCPU_OUT_REGS][4], w;
    int a;
    for (a = 0; a < 16; a++) {
        if (g_pg.prog_inputs & (1u << a))
            va_fetch4(a, idx, in[a]);
        else
            in[a][0] = in[a][1] = in[a][2] = 0.0f, in[a][3] = 1.0f;
    }
    vshcpu_run(g_pg.prog_dec, g_pg.prog_len, (const float (*)[4])in, g_pg.vconst, out);

    w = out[VSHCPU_OUT_POS][3];
    ov->x = truncf(out[VSHCPU_OUT_POS][0] * 16.0f) / 16.0f;
    ov->y = truncf(out[VSHCPU_OUT_POS][1] * 16.0f) / 16.0f;
    ov->z = (g_pg.clip_max > 0.0f) ? out[VSHCPU_OUT_POS][2] / g_pg.clip_max
                                   : out[VSHCPU_OUT_POS][2];
    if (ov->z < 0.0f) g_vs.z_neg++;
    else if (ov->z > 1.0f) g_vs.z_over++;
    if (ov->x < -64.0f || ov->x > 704.0f || ov->y < -64.0f || ov->y > 544.0f) g_vs.xy_off++;
    if (w <= 0.0f) g_vs.w_neg++;
    /* Near-plane clipping, as xemu's vsh-prog.c: keep w's sign,
     * clamp it away from zero and infinity, and let the rasteriser clip in
     * homogeneous space -- d3d8_nv2a.c's VS multiplies x, y, z back by w.
     * Triangles with a vertex behind the eye used to be dropped whole, so
     * any surface running past the camera (floors, the course) lost every
     * triangle that crossed the eye plane. */
    if (w >= 0.0f) { if (w < 5.421011e-20f) w = 5.421011e-20f; if (w > 1.8446744e19f) w = 1.8446744e19f; }
    else           { if (w > -5.421011e-20f) w = -5.421011e-20f; if (w < -1.8446744e19f) w = -1.8446744e19f; }
    ov->rhw = 1.0f / w;
    memcpy(ov->d0, out[VSHCPU_OUT_D0], sizeof ov->d0);
    memcpy(ov->d1, out[VSHCPU_OUT_D1], sizeof ov->d1);
    {
        /* Colours as xemu hands them to the pixel stage: NaN -> 1; oD1 only
         * with SET_SPECULAR_ENABLE (else 0,0,0,1), and its alpha only with
         * SET_LIGHT_CONTROL's ALPHA_FROM_MATERIAL_SPECULAR (else 1). */
        int c;
        for (c = 0; c < 4; c++) {
            if (ov->d0[c] != ov->d0[c]) ov->d0[c] = 1.0f;
            if (ov->d1[c] != ov->d1[c]) ov->d1[c] = 1.0f;
        }
        if (!g_pg.specular_enable) {
            ov->d1[0] = ov->d1[1] = ov->d1[2] = 0.0f;
            ov->d1[3] = 1.0f;
        } else if (!(g_pg.light_control & (1u << 17))) {
            ov->d1[3] = 1.0f;
        }
    }
    ov->fog = vsh_fog_factor(out[VSHCPU_OUT_FOG][0]);
    memcpy(ov->t, out[VSHCPU_OUT_T0], sizeof ov->t);   /* oT0..oT3 are consecutive */
    if (psize)
        *psize = g_pg.point_params_enable ? out[VSHCPU_OUT_PTS][0] * d3d8_PointZoom()
                                          : (g_pg.point_size ? g_pg.point_size / 8.0f : 1.0f);
    /* Only a vertex the rasteriser cannot place is unusable now. */
    return isfinite(ov->x) && isfinite(ov->y) && isfinite(ov->z) && isfinite(ov->rhw);
}

/* Expand an NV2A primitive over `n` vertices into a D3D list, dropping any
 * triangle with a vertex behind the eye. Returns the output vertex count. */
static uint32_t assemble(uint32_t mode, const ProgVertex *v, const uint8_t *ok,
                         uint32_t n, ProgVertex *dst, int *prim)
{
    uint32_t o = 0, i;
    /* XBOX_VSH_KEEPW=1 (diagnostic) keeps triangles with a vertex at w <= 0. */
    static int keepw = -1;
    if (keepw < 0) { const char *e = getenv("XBOX_VSH_KEEPW"); keepw = e && e[0] == '1'; }
    #define TRI(a, b, c) do { \
        if (keepw || (ok[a] && ok[b] && ok[c])) { dst[o++] = v[a]; dst[o++] = v[b]; dst[o++] = v[c]; } \
        else g_vs.culled_w++; } while (0)
    switch (mode) {
    case 1:                                     /* points */
    case 2:                                     /* lines */
    case 3: case 4:                             /* line loop / strip */
        *prim = nv2a_draw_mode_to_d3d(mode);
        for (i = 0; i < n; i++) dst[o++] = v[i];
        return o;
    case 5:                                     /* triangles */
        for (i = 0; i + 2 < n; i += 3) TRI(i, i + 1, i + 2);
        break;
    case 6:                                     /* triangle strip */
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) TRI(i + 1, i, i + 2);
            else       TRI(i, i + 1, i + 2);
        }
        break;
    case 7:                                     /* triangle fan */
    case 10:                                    /* polygon */
        for (i = 1; i + 1 < n; i++) TRI(0, i, i + 1);
        break;
    case 8:                                     /* quads */
        for (i = 0; i + 3 < n; i += 4) { TRI(i, i + 1, i + 2); TRI(i, i + 2, i + 3); }
        break;
    case 9:                                     /* quad strip */
        for (i = 0; i + 3 < n; i += 2) { TRI(i, i + 1, i + 3); TRI(i, i + 3, i + 2); }
        break;
    default:
        break;
    }
    #undef TRI
    *prim = D3DPT_TRIANGLELIST;
    return o;
}

/* NV2A zpass pixel counter and reports (fork).
 *
 * The title's D3D brackets each visibility test (LensFX's sun, MeshDrawMode_
 * VisibilityTestedMesh) with CLEAR_REPORT_VALUE(1) + SET_ZPASS_PIXEL_COUNT_
 * ENABLE(1) ... ENABLE(0) + GET_REPORT((addr & 0x2FFFFFF) | 1 << 24), having
 * set the 16-byte report's status word (+0xC) to 0xFFFFFFFF; the GPU then
 * writes {timestamp, value, status 0} at that physical address (reports live
 * in MmAllocateContiguousMemoryEx(.., 0xFFFFFF, ..) pages, below 16 MB, so
 * the DMA offset is the physical address). D3DDevice_GetVisibilityTestResult
 * (0x169010) reads status/value; Render_ReadVisibilityTestResult spins up to
 * 0x100000 times while the status reads "pending". These methods used to be
 * ignored: the report never landed, every test spun to its limit and read
 * 0, and the lens flare never showed.
 *
 * Each ENABLE(1)..ENABLE(0) span is one D3D11 occlusion query; GET_REPORT
 * queues a report job over the spans counted since the last clear; the pump
 * polls jobs without blocking and writes value then status when all spans
 * are done. Counts are scaled back to title pixels. XBOX_FIX_OCCLUSION=0
 * restores the old behaviour (methods ignored). */
#define OCC_SPANS 16
#define OCC_JOBS  64
typedef struct { int h; int refs; int ready; unsigned long long n; } OccSpan;
static OccSpan  g_occ_span[256];
static int      g_occ_open = -1;                 /* span being counted */
static int      g_occ_cur[OCC_SPANS], g_occ_ncur;/* spans since the last clear */
static struct { uint32_t addr; int span[OCC_SPANS]; int n; LARGE_INTEGER t0; } g_occ_job[OCC_JOBS];
static int      g_occ_njob;
static unsigned long long g_occ_reports, g_occ_lat_us, g_occ_dropped;

static int occ_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_OCCLUSION"); on = !(e && e[0] == '0'); }
    return on;
}

static void occ_unref(int s)
{
    if (s < 0) return;
    if (--g_occ_span[s].refs <= 0) {
        d3d8_OcclusionRelease(g_occ_span[s].h);
        g_occ_span[s].h = -1; g_occ_span[s].refs = 0;
    }
}

static void occ_clear(void)
{
    int i;
    for (i = 0; i < g_occ_ncur; i++) occ_unref(g_occ_cur[i]);
    g_occ_ncur = 0;
}

static void occ_enable(int on)
{
    if (on && g_occ_open < 0) {
        int h = d3d8_OcclusionBegin(), s;
        if (h < 0) { g_occ_dropped++; return; }
        s = h;                                    /* one span slot per query handle */
        g_occ_span[s].h = h; g_occ_span[s].refs = 1; g_occ_span[s].ready = 0; g_occ_span[s].n = 0;
        g_occ_open = s;
    } else if (!on && g_occ_open >= 0) {
        d3d8_OcclusionEnd(g_occ_span[g_occ_open].h);
        if (g_occ_ncur < OCC_SPANS) g_occ_cur[g_occ_ncur++] = g_occ_open;
        else { occ_unref(g_occ_open); g_occ_dropped++; }
        g_occ_open = -1;
    }
}

static void occ_get_report(uint32_t param)
{
    int i, j;
    {   /* XBOX_OCCL_LOG=1 (diagnostic): the first reports requested */
        static int left = -1;
        if (left < 0) { const char *e = getenv("XBOX_OCCL_LOG"); left = (e && e[0] == '1') ? 40 : 0; }
        if (left > 0) {
            left--;
            fprintf(stderr, "[OCCL] GET_REPORT %08X spans %d open %d jobs %d status %08X\n", param, g_occ_ncur,
                    g_occ_open, g_occ_njob,
                    (g_pg_mem_base && (param & 0xFFFFFF) + 16 <= g_pg_mem_size)
                    ? *(uint32_t *)(g_pg_mem_base + (param & 0xFFFFFF) + 12) : 0xDEADDEADu);
        }
    }
    if ((param >> 24) != 1) return;                   /* only the zpass pixel count */
    if (g_occ_njob >= OCC_JOBS) { g_occ_dropped++; return; }
    j = g_occ_njob++;
    g_occ_job[j].addr = param & 0x00FFFFFFu;
    g_occ_job[j].n = g_occ_ncur;
    for (i = 0; i < g_occ_ncur; i++) { g_occ_job[j].span[i] = g_occ_cur[i]; g_occ_span[g_occ_cur[i]].refs++; }
    QueryPerformanceCounter(&g_occ_job[j].t0);
    {   /* Fork: the title spins on this report until the pump writes it;
         * send the queries to the GPU now rather than at the next poll.
         * XBOX_FIX_PUMP_OCCFLUSH=0: no flush here (the old latency). */
        static int fl = -1;
        extern void d3d8_OcclusionFlush(void);
        extern int d3d8_pump_alt_off(void);
        if (fl < 0) { const char *e = getenv("XBOX_FIX_PUMP_OCCFLUSH"); fl = !(e && e[0] == '0'); }
        if (fl && !d3d8_pump_alt_off()) d3d8_OcclusionFlush();
    }
}

/* Reports the title is (or will be) waiting for: the pump does not sleep
 * while there is one (XBOX_FIX_PUMP_OCCWAIT, xbox_memory_layout.c). */
int pgraph_d3d11_reports_pending(void) { return g_occ_njob; }

/* Pump thread, every tick: write the reports whose spans are all counted. */
void pgraph_d3d11_poll_reports(void)
{
    int j = 0, i;
    while (j < g_occ_njob) {
        unsigned long long sum = 0;
        int done = 1;
        for (i = 0; i < g_occ_job[j].n; i++) {
            OccSpan *sp = &g_occ_span[g_occ_job[j].span[i]];
            if (!sp->ready) {
                int r = d3d8_OcclusionPoll(sp->h, &sp->n);
                if (r == 0) { done = 0; break; }
                sp->ready = 1;
                if (r < 0) sp->n = 0;
            }
            sum += sp->n;
        }
        if (!done) { j++; continue; }
        {
            uint8_t *p = (g_pg_mem_base && g_occ_job[j].addr + 16 <= g_pg_mem_size)
                       ? g_pg_mem_base + g_occ_job[j].addr : NULL;
            float scale = d3d8_OcclusionScale();
            uint32_t v = (uint32_t)((double)sum / (scale > 0.0f ? scale : 1.0f) + 0.5);
            LARGE_INTEGER t1, f;
            QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
            if (p) {
                uint64_t ts = (uint64_t)t1.QuadPart;
                memcpy(p, &ts, 8);
                *(volatile uint32_t *)(p + 8) = v;
                MemoryBarrier();
                *(volatile uint32_t *)(p + 12) = 0;          /* status: done */
            }
            {   /* XBOX_OCCL_LOG=1 (diagnostic): a sample of the values written */
                static int logv = -1;
                if (logv < 0) { const char *e = getenv("XBOX_OCCL_LOG"); logv = e && e[0] == '1'; }
                if (logv && (g_occ_reports < 60 || g_occ_reports % 50 == 0))
                    fprintf(stderr, "[OCCL] report #%llu at %08X = %u px (%llu samples / %.2f)\n",
                            g_occ_reports, g_occ_job[j].addr, v, sum, scale);
            }
            g_occ_reports++;
            g_occ_lat_us += (unsigned long long)((t1.QuadPart - g_occ_job[j].t0.QuadPart) * 1000000 / f.QuadPart);
        }
        for (i = 0; i < g_occ_job[j].n; i++) occ_unref(g_occ_job[j].span[i]);
        g_occ_job[j] = g_occ_job[--g_occ_njob];
    }
}

void pgraph_d3d11_report_occlusion(void)
{
    static unsigned long long last;
    if (g_occ_reports == last) return;
    last = g_occ_reports;
    fprintf(stderr, "[OCCL] %llu reports written, mean latency %.2f ms, %d pending, %llu dropped\n",
            g_occ_reports, g_occ_reports ? (double)g_occ_lat_us / 1000.0 / (double)g_occ_reports : 0.0,
            g_occ_njob, g_occ_dropped);
}

static void drawlog_program_draw(const uint32_t *indices, uint32_t v0, uint32_t count)
{
    static unsigned seq;
    float in[16][4], out[VSHCPU_OUT_REGS][4];
    int a, st, nst = (int)(g_pg.comb_control & 0xFF);
    {
        static int dry2 = -1;
        if (dry2 < 0) { const char *e = getenv("XBOX_NV2A_DRAWLOG_DRY"); dry2 = e && atoi(e) == 2; }
        if (dry2) { Sleep(1); return; }
    }
    for (a = 0; a < 16; a++) va_fetch4(a, v0, in[a]);
    vshcpu_run(g_pg.prog_dec, g_pg.prog_len, (const float (*)[4])in, g_pg.vconst, out);
    {   /* XBOX_NV2A_DRAWLOG_DRY=1: stop here (the CPU work, no output);
         * =2: skip the CPU work too and only sleep 1 ms per draw. Separates
         * a side effect of the log from the time it takes. */
        static int dry = -1;
        if (dry < 0) { const char *e = getenv("XBOX_NV2A_DRAWLOG_DRY"); dry = e ? atoi(e) : 0; }
        if (dry == 1) return;
    }
    fprintf(stderr, "[DRAW]   depth func %X clip %g..%g vp z scale %g off %g\n", g_pg.depth_func,
            g_pg.clip_min, g_pg.clip_max, g_pg.vconst[0x3A][2], g_pg.vconst[0x3B][2]);
    fprintf(stderr, "[DRAW] f%u #%u mode %u n %u prog@%u blend %d %X/%X alphatest %d z %d/%d "
            "stages %d prog %08X cw0 %08X cw1 %08X\n",
            d3d8_PresentSeq(), seq++, g_pg.draw_mode, count, g_pg.prog_start, g_pg.blend_enable,
            g_pg.blend_sfactor, g_pg.blend_dfactor, g_pg.alpha_test, g_pg.depth_test,
            g_pg.depth_mask, nst, g_pg.shader_stage_program, g_pg.spec_fog_cw0, g_pg.spec_fog_cw1);
    for (st = 0; st < 4; st++) {
        uint32_t f = g_pg.tex[st].format;
        if (!g_pg.tex[st].enabled) continue;
        fprintf(stderr, "[DRAW]   t%d off %08X fmt %08X (color %02X %ux%u mips %u cube %u dim %u) "
                "addr %08X  oT%d %g %g %g %g\n", st, g_pg.tex[st].offset, f, TEXFMT_COLOR(f),
                1u << TEXFMT_SIZE_U(f), 1u << TEXFMT_SIZE_V(f), TEXFMT_MIPS(f), (f >> 2) & 1,
                (f >> 4) & 0xF, g_pg.tex[st].address, st, out[VSHCPU_OUT_T0 + st][0],
                out[VSHCPU_OUT_T0 + st][1], out[VSHCPU_OUT_T0 + st][2], out[VSHCPU_OUT_T0 + st][3]);
    }
    for (st = 0; st < nst && st < 8; st++)
        fprintf(stderr, "[DRAW]   s%d rgb %08X->%08X alpha %08X->%08X k %08X %08X\n", st,
                g_pg.comb_color_icw[st], g_pg.comb_color_ocw[st], g_pg.comb_alpha_icw[st],
                g_pg.comb_alpha_ocw[st], g_pg.comb_factor0[st], g_pg.comb_factor1[st]);
    {
        /* Input-position bounding box over the draw's vertices (first 4096),
         * and how many sit absurdly far out: CPU-skinned meshes arrive in
         * world space, so a bad bone shows up here as a spike. */
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f }, p[4];
        uint32_t k, far_out = 0, nv = count < 4096 ? count : 4096;
        int c;
        for (k = 0; k < nv; k++) {
            va_fetch4(0, indices ? indices[k] : v0 + k, p);
            for (c = 0; c < 3; c++) {
                if (p[c] < mn[c]) mn[c] = p[c];
                if (p[c] > mx[c]) mx[c] = p[c];
            }
            if (fabsf(p[0]) > 1e5f || fabsf(p[1]) > 1e5f || fabsf(p[2]) > 1e5f || p[0] != p[0])
                far_out++;
        }
        fprintf(stderr, "[DRAW]   v0 box %.1f..%.1f %.1f..%.1f %.1f..%.1f far %u/%u\n",
                mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], far_out, nv);
    }
    for (a = 0; a < 16; a++)
        if (g_pg.vattr[a].format & 0xF0u)
            fprintf(stderr, "[DRAW]   v%-2d fmt %08X off %08X = %g %g %g %g\n", a,
                    g_pg.vattr[a].format, g_pg.vattr[a].offset,
                    in[a][0], in[a][1], in[a][2], in[a][3]);
    {
        /* XBOX_NV2A_DRAWLOG_VERTS=N: the first N vertices' attributes and
         * program texture outputs, one line each. */
        static int nverts = -1;
        uint32_t k;
        if (nverts < 0) { const char *e = getenv("XBOX_NV2A_DRAWLOG_VERTS"); nverts = e ? atoi(e) : 0; }
        for (k = 0; k < (uint32_t)nverts && k < count; k++) {
            float vin[16][4], vout[VSHCPU_OUT_REGS][4];
            uint32_t vi = indices ? indices[k] : v0 + k;
            for (a = 0; a < 16; a++) va_fetch4(a, vi, vin[a]);
            vshcpu_run(g_pg.prog_dec, g_pg.prog_len, (const float (*)[4])vin, g_pg.vconst, vout);
            fprintf(stderr, "[VERT] %4u #%-5u", k, vi);
            for (a = 0; a < 16; a++)
                if ((g_pg.vattr[a].format & 0xF0u) || k == 0)   /* first vertex: constant registers too */
                    fprintf(stderr, " v%d%s(%g %g %g %g)", a, (g_pg.vattr[a].format & 0xF0u) ? "" : "c",
                            vin[a][0], vin[a][1], vin[a][2], vin[a][3]);
            if (k == 0) {
                /* XBOX_NV2A_DRAWLOG_CONSTS=LO-HI: those constant registers too
                 * (default 98-99, the fog colour and parameters). */
                static int clo = -1, chi = -1;
                int ci;
                if (clo < 0) {
                    const char *e = getenv("XBOX_NV2A_DRAWLOG_CONSTS");
                    clo = 98; chi = 99;
                    if (e) sscanf(e, "%d-%d", &clo, &chi);
                }
                for (ci = clo; ci <= chi && ci < 192; ci++)
                    fprintf(stderr, " c%d(%g %g %g %g)", ci, g_pg.vconst[ci][0], g_pg.vconst[ci][1],
                            g_pg.vconst[ci][2], g_pg.vconst[ci][3]);
            }
            for (a = 0; a < 4; a++)
                fprintf(stderr, " oT%d(%g %g %g %g)", a, vout[VSHCPU_OUT_T0 + a][0],
                        vout[VSHCPU_OUT_T0 + a][1], vout[VSHCPU_OUT_T0 + a][2], vout[VSHCPU_OUT_T0 + a][3]);
            fprintf(stderr, " oPos(%g %g %g %g) oD0(%g %g %g %g) oPts %g",
                    vout[VSHCPU_OUT_POS][0], vout[VSHCPU_OUT_POS][1], vout[VSHCPU_OUT_POS][2], vout[VSHCPU_OUT_POS][3],
                    vout[VSHCPU_OUT_D0][0], vout[VSHCPU_OUT_D0][1], vout[VSHCPU_OUT_D0][2], vout[VSHCPU_OUT_D0][3],
                    vout[VSHCPU_OUT_PTS][0]);
            fprintf(stderr, "\n");
        }
    }
    {
        /* The program listing, once per distinct program. */
        static uint32_t seen[64];
        static int nseen;
        uint32_t h = 2166136261u;
        int k;
        for (k = 0; k < g_pg.prog_len; k++) {
            h = (h ^ g_pg.prog[(g_pg.prog_start + k) % VSHCPU_SLOTS][1]) * 16777619u;
            h = (h ^ g_pg.prog[(g_pg.prog_start + k) % VSHCPU_SLOTS][2]) * 16777619u;
            h = (h ^ g_pg.prog[(g_pg.prog_start + k) % VSHCPU_SLOTS][3]) * 16777619u;
        }
        for (k = 0; k < nseen && seen[k] != h; k++) ;
        fprintf(stderr, "[DRAW]   program %08X len %d inputs %04X\n", h, g_pg.prog_len, g_pg.prog_inputs);
        if (k == nseen && nseen < 64) {
            seen[nseen++] = h;
            for (k = 0; k < g_pg.prog_len; k++) {
                char b[200];
                vshcpu_format(&g_pg.prog_dec[k], b, sizeof b);
                fprintf(stderr, "[DRAW]     %2d %s\n", k, b);
            }
        }
    }
    fprintf(stderr, "[DRAW]   oD0 %.3g %.3g %.3g %.3g oD1 %.3g %.3g %.3g %.3g oFog %.3g oPts %.4g pos %.1f %.1f %.4f %.3g\n",
            out[VSHCPU_OUT_D0][0], out[VSHCPU_OUT_D0][1], out[VSHCPU_OUT_D0][2], out[VSHCPU_OUT_D0][3],
            out[VSHCPU_OUT_D1][0], out[VSHCPU_OUT_D1][1], out[VSHCPU_OUT_D1][2], out[VSHCPU_OUT_D1][3],
            out[VSHCPU_OUT_FOG][0], out[VSHCPU_OUT_PTS][0], out[VSHCPU_OUT_POS][0], out[VSHCPU_OUT_POS][1],
            out[VSHCPU_OUT_POS][2], out[VSHCPU_OUT_POS][3]);
}

/* Window clip -> scissor. One inclusive rectangle -- all eight
 * slots equal, which is what a write to slot 0 produces -- becomes a D3D11
 * scissor for every draw; the whole surface or no clip at all turns it off.
 * Any other shape (several rectangles, exclusive) is left to the pixel-shader
 * test in draw_program (wclip_in_shader). XBOX_NV2A_WCLIP=0 disables both. */
static int wclip_in_shader;

static void wclip_update(void)
{
    static int on = -1;
    int k, same = 1;
    uint32_t x0, x1, y0, y1;
    if (on < 0) { const char *e = getenv("XBOX_NV2A_WCLIP"); on = !(e && e[0] == '0'); }
    wclip_in_shader = 0;
    if (!on || !g_pg.wclip_seen) { d3d8_SetWindowClip(0, 0, 0, 0, 0); return; }
    for (k = 1; k < 8; k++)
        if (g_pg.wclip_x[k] != g_pg.wclip_x[0] || g_pg.wclip_y[k] != g_pg.wclip_y[0]) same = 0;
    x0 = g_pg.wclip_x[0] & 0xFFF; x1 = ((g_pg.wclip_x[0] >> 16) & 0xFFF) + 1;
    y0 = g_pg.wclip_y[0] & 0xFFF; y1 = ((g_pg.wclip_y[0] >> 16) & 0xFFF) + 1;
    if (g_pg.wclip_type == 0 && same) {
        if (x0 == 0 && y0 == 0 && x1 >= 640 && y1 >= 480)
            d3d8_SetWindowClip(0, 0, 0, 0, 0);
        else
            d3d8_SetWindowClip(1, (long)x0, (long)y0, (long)x1, (long)y1);
    } else {
        d3d8_SetWindowClip(0, 0, 0, 0, 0);
        wclip_in_shader = 1;
    }
}

/* Draw the current BEGIN/END's vertex list through the vertex program.
 * `indices` NULL means `count` consecutive vertices from `start`. */
/* ── Vertex programs on the GPU ─────────────────────────────
 *
 * The CPU interpreter below was two thirds of the render thread in a race,
 * and the game thread waits on the render thread (D3D_BlockOnTime), so it set
 * the frame rate. gpu_prepare() describes a program draw for
 * d3d8_nv2a_draw_program_gpu(), which runs the program as a generated HLSL
 * vertex shader (d3d8_nv2a_vsh.c) on the title's own vertex data. Anything it
 * cannot express stays on the CPU path: point sprites (expanded here),
 * programs that write constant registers, attribute formats or alignments
 * the input assembler cannot read. XBOX_VSH_GPU=0 keeps everything on the
 * CPU, for comparison. */
#include <dxgiformat.h>
#include "../d3d/d3d8_nv2a_vsh.h"
int d3d8_nv2a_vsh_ready(const Nv2aVshDraw *d);
unsigned int d3d8_GetBackbufferWidth(void);    /* d3d8_internal.h: the title's 640x480 */
unsigned int d3d8_GetBackbufferHeight(void);

/* DXGI format, shader-input kind and element bytes for an NV2A array format. */
static int gpu_attr_format(uint32_t type, uint32_t size, uint32_t *fmt, uint8_t *kind, uint32_t *bytes)
{
    static const uint32_t f32[5]  = { 0, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
                                      DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT };
    static const uint32_t ubd[5]  = { 0, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM,
                                      DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };
    static const uint32_t ubo[5]  = { 0, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM,
                                      DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM };
    static const uint32_t s1[5]   = { 0, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16G16_SNORM,
                                      DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM };
    static const uint32_t s32k[5] = { 0, DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16G16_SINT,
                                      DXGI_FORMAT_R16G16B16A16_SINT, DXGI_FORMAT_R16G16B16A16_SINT };
    if (type == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP) {
        *fmt = DXGI_FORMAT_R32_UINT; *kind = NV2A_VSH_IN_CMP; *bytes = 4;
        return 1;
    }
    if (size < 1 || size > 4) return 0;
    *kind = (uint8_t)(size == 3 && type != NV2A_VA_TYPE_F ? NV2A_VSH_IN_W1 : 0);
    switch (type) {
    case NV2A_VA_TYPE_F:      *fmt = f32[size]; *bytes = 4 * size; break;
    case NV2A_VA_TYPE_UB_D3D: *fmt = ubd[size]; *bytes = size == 3 ? 4 : size; break;
    case NV2A_VA_TYPE_UB_OGL: *fmt = ubo[size]; *bytes = size == 3 ? 4 : size; break;
    case NV2A_VA_TYPE_S1:     *fmt = s1[size];  *bytes = size == 3 ? 8 : 2 * size; break;
    case NV2A_VA_TYPE_S32K:   *fmt = s32k[size]; *bytes = size == 3 ? 8 : 2 * size;
                              *kind |= NV2A_VSH_IN_INT; break;
    default: return 0;
    }
    return 1;
}

/* Why program draws stayed on the CPU: off, points, constant writes, mode,
 * format, alignment, unmapped range, no shader; and how many went to the GPU. */
static unsigned g_gpu_fb[8], g_gpu_ok;
#define GPU_NO(r) do { g_gpu_fb[r]++; return 0; } while (0)

/* The sprite is square in the title's pixels, which the
 * host stretches across a wider shape (sx > sy): x 1.33 at 16:9 --
 * what a widescreen TV did to the console's frame too -- 1.79 at
 * 21:9, 2.67 at 32:9. Round sprites scale the half-width by sy/sx.
 * Default: round beyond 16:9 only (no console ever showed those), as
 * on the console at 4:3 and 16:9; XBOX_ROUND_POINTS=1/0 forces it.
 * One function for the CPU squares and the geometry shader. */
static float points_kx(void)
{
    static int round_pts = -1;
    float kx = 1.0f;
    if (round_pts < 0) {
        const char *e = getenv("XBOX_ROUND_POINTS");
        round_pts = e ? e[0] == '1' : d3d8_HostAspect() > 16.0 / 9.0 + 0.01;
    }
    if (round_pts) {
        float sx, sy;
        d3d8_GetGuestScale(&sx, &sy);
        if (sx > 0.0f) kx = sy / sx;
    }
    {
        static float told = 1.0f;
        if (kx != told) { told = kx; fprintf(stderr, "[POINTS] half-width factor %.4f\n", kx); }
    }
    return kx;
}

static int gpu_prepare(Nv2aVshDraw *d, const uint32_t *indices, uint32_t start, uint32_t count)
{
    static int on = -1;
    static uint32_t *ib = NULL, ib_cap = 0;
    uint32_t lo, hi, i, a, n = 0, need;
    uint32_t mode = g_pg.draw_mode;

    if (on < 0) { const char *e = getenv("XBOX_VSH_GPU"); on = !(e && e[0] == '0'); }
    if (!on || g_pg.prog_len <= 0 || count == 0) GPU_NO(0);
    if (mode == 1 && !d3d8_points_gpu_on()) GPU_NO(1);      /* points on the GPU */
    if (g_pg.prog_writes_c) GPU_NO(2);

    memset(d, 0, sizeof *d);
    if (indices) {
        lo = hi = indices[0];
        for (i = 1; i < count; i++) {
            if (indices[i] < lo) lo = indices[i];
            if (indices[i] > hi) hi = indices[i];
        }
    } else {
        lo = start;
        hi = start + count - 1;
    }

    /* The index list, rebased to lo, in the primitive form assemble() builds. */
    need = count * 6u + 8u;
    if (need > ib_cap) {
        uint32_t *nb = (uint32_t *)realloc(ib, need * sizeof *nb);
        if (!nb) GPU_NO(0);
        ib = nb;
        ib_cap = need;
    }
    #define IX(k) ((indices ? indices[k] : start + (k)) - lo)
    switch (mode) {
    case 1:                                     /* points: squares built by the GS */
    case 2: case 3: case 4:                     /* lines, loop, strip */
        for (i = 0; i < count; i++) ib[n++] = IX(i);
        d->topology = nv2a_draw_mode_to_d3d(mode);
        break;
    case 5:
        for (i = 0; i + 2 < count; i += 3) { ib[n++] = IX(i); ib[n++] = IX(i + 1); ib[n++] = IX(i + 2); }
        break;
    case 6:
        for (i = 0; i + 2 < count; i++) {
            if (i & 1) { ib[n++] = IX(i + 1); ib[n++] = IX(i); }
            else       { ib[n++] = IX(i);     ib[n++] = IX(i + 1); }
            ib[n++] = IX(i + 2);
        }
        break;
    case 7: case 10:
        for (i = 1; i + 1 < count; i++) { ib[n++] = IX(0); ib[n++] = IX(i); ib[n++] = IX(i + 1); }
        break;
    case 8:
        for (i = 0; i + 3 < count; i += 4) {
            ib[n++] = IX(i); ib[n++] = IX(i + 1); ib[n++] = IX(i + 2);
            ib[n++] = IX(i); ib[n++] = IX(i + 2); ib[n++] = IX(i + 3);
        }
        break;
    case 9:
        for (i = 0; i + 3 < count; i += 2) {
            ib[n++] = IX(i); ib[n++] = IX(i + 1); ib[n++] = IX(i + 3);
            ib[n++] = IX(i); ib[n++] = IX(i + 3); ib[n++] = IX(i + 2);
        }
        break;
    default:
        GPU_NO(3);
    }
    #undef IX
    if (mode >= 5) d->topology = D3DPT_TRIANGLELIST;
    if (n == 0) GPU_NO(3);
    d->indices = ib;
    d->nindices = n;

    va_const_init();
    for (a = 0; a < 16; a++) {
        uint32_t fmt = g_pg.vattr[a].format, type = fmt & 0x0Fu, size = (fmt >> 4) & 0x0Fu;
        uint32_t stride = (fmt >> 8) & 0xFFFFFFu, dx = 0, bytes = 0, total;
        uint8_t kind = 0;
        if (!(g_pg.prog_inputs & (1u << a))) continue;
        if (size == 0) { d->attr[a].kind = NV2A_VSH_IN_CONST; continue; }
        if (!gpu_attr_format(type, size, &dx, &kind, &bytes)) GPU_NO(4);
        /* The input assembler reads elements at 4-byte-aligned offsets. An
         * array that is not aligned (packed shorts, byte offsets) is copied
         * for this draw into a tight, aligned one -- far cheaper than running
         * the whole draw on the CPU. */
        if ((g_pg.vattr[a].offset & 3u) || (stride & 3u)) {
            static uint8_t *rep[16];
            static uint32_t rep_cap[16];
            uint32_t es = (bytes + 3u) & ~3u, nv = stride ? hi - lo + 1 : 1, v;
            const uint8_t *src = va_ptr(g_pg.vattr[a].offset + lo * stride, (hi - lo) * stride + bytes);
            if (!src) GPU_NO(6);
            if (nv * es > rep_cap[a]) {
                uint8_t *nb = (uint8_t *)realloc(rep[a], nv * es);
                if (!nb) GPU_NO(5);
                rep[a] = nb;
                rep_cap[a] = nv * es;
            }
            for (v = 0; v < nv; v++) memcpy(rep[a] + v * es, src + v * stride, bytes);
            d->attr[a].base = rep[a];
            d->attr[a].kind = kind;
            d->attr[a].dxgi_format = dx;
            d->attr[a].stride = stride ? es : 0;
            d->attr[a].bytes = nv * es;
            g_gpu_fb[5]++;                      /* counted, but drawn on the GPU */
            continue;
        }
        total = (hi - lo) * stride + bytes;
        d->attr[a].base = va_ptr(g_pg.vattr[a].offset + lo * stride, total);
        if (!d->attr[a].base) GPU_NO(6);
        d->attr[a].kind = kind;
        d->attr[a].dxgi_format = dx;
        d->attr[a].stride = stride;
        d->attr[a].bytes = total;
    }
    d->mem_lo = g_pg_mem_base;
    d->mem_hi = g_pg_mem_base ? g_pg_mem_base + g_pg_mem_size : NULL;
    d->prog = g_pg.prog_dec;
    d->prog_len = g_pg.prog_len;
    d->prog_hash = g_pg.prog_hash;
    d->inputs = g_pg.prog_inputs;
    d->attr_const = (const float (*)[4])g_pg.vattr_const;
    d->vconst = (const float (*)[4])g_pg.vconst;
    d->screen_w = (float)d3d8_GetBackbufferWidth();
    d->screen_h = (float)d3d8_GetBackbufferHeight();
    if (d->screen_w <= 0.0f) d->screen_w = 640.0f;
    if (d->screen_h <= 0.0f) d->screen_h = 480.0f;
    d->clip_max = g_pg.clip_max;
    d->fog_mode = 0;
    if (g_pg.fog_enable) {
        switch (g_pg.fog_mode) {
        case NV097_SET_FOG_MODE_V_LINEAR:     d->fog_mode = 1; break;
        case NV097_SET_FOG_MODE_V_LINEAR_ABS: d->fog_mode = 2; break;
        case NV097_SET_FOG_MODE_V_EXP:        d->fog_mode = 3; break;
        case NV097_SET_FOG_MODE_V_EXP_ABS:    d->fog_mode = 4; break;
        case NV097_SET_FOG_MODE_V_EXP2:
        case NV097_SET_FOG_MODE_V_EXP2_ABS:   d->fog_mode = 5; break;
        default:                              d->fog_mode = 0; break;
        }
    }
    d->fog_p0 = g_pg.fog_param[0];
    d->fog_p1 = g_pg.fog_param[1];
    d->specular = g_pg.specular_enable ? 1 : 0;
    d->spec_alpha = (g_pg.light_control & (1u << 17)) ? 1 : 0;
    d->point_params = g_pg.point_params_enable ? 1 : 0;
    d->point_size = g_pg.point_size ? g_pg.point_size / 8.0f : 1.0f;
    d->point_smooth = g_pg.point_smooth ? 1 : 0;
    d->point_kx = g_pg.draw_mode == 1 ? points_kx() : 1.0f;
    d->point_zoom = d3d8_PointZoom();
    d->hud_kx = d->hud_ax = d->hud_ky = d->hud_ay = 0.0f;
    if (!d3d8_nv2a_vsh_ready(d)) GPU_NO(7);
    g_gpu_ok++;
    return 1;
}

/* The CPU squares of `count` points: six vertices per
 * point into dst, the points whose vertex cannot be placed dropped. Also
 * the reference of the XBOX_POINTS_CHECK comparison. */
static uint32_t points_squares(const ProgVertex *vb, const uint8_t *okb, const float *psb,
                               uint32_t count, ProgVertex *tb)
{
    uint32_t i;
    uint32_t o = 0;
    float kx = points_kx();             /* shared with the GPU path */
    for (i = 0; i < count; i++) {
        ProgVertex q[4];
        float h = psb[i];
        int c;
        {
            static int keepw = -1;   /* XBOX_VSH_KEEPW=1 (diagnostic), as in assemble() */
            if (keepw < 0) { const char *e = getenv("XBOX_VSH_KEEPW"); keepw = e && e[0] == '1'; }
            if (!okb[i] && !keepw) { g_vs.culled_w++; continue; }
        }
        if (!(h >= 1.0f)) h = 1.0f;
        if (h > 2048.0f) h = 2048.0f;
        h *= 0.5f;
        for (c = 0; c < 4; c++) {
            q[c] = vb[i];
            q[c].x = vb[i].x + ((c & 1) ? h : -h) * kx;
            q[c].y = vb[i].y + ((c & 2) ? h : -h);
            if (g_pg.point_smooth) {
                q[c].t[3][0] = (c & 1) ? 1.0f : 0.0f;
                q[c].t[3][1] = (c & 2) ? 1.0f : 0.0f;
                q[c].t[3][2] = 1.0f;
                q[c].t[3][3] = 1.0f;
            }
        }
        tb[o++] = q[0]; tb[o++] = q[1]; tb[o++] = q[2];
        tb[o++] = q[2]; tb[o++] = q[1]; tb[o++] = q[3];
    }
    return o;
}

/* The scale of the current HUD element, in the draw's pixels; 0 when
 * the draw is not part of one. */
static int hud_element(float *kx, float *ax, float *ky, float *ay)
{
    float sw = (float)d3d8_GetBackbufferWidth(), sh = (float)d3d8_GetBackbufferHeight();
    if (sw <= 0.0f) sw = 640.0f;
    if (sh <= 0.0f) sh = 480.0f;
    /* With the 16:9 frame, the mode of the image (its 3D is the frame
     * before's); without it (16:9, 16:10) only the 2D changes: the mode of
     * the frame being drawn (its frame tag comes first). */
    int wide = g_box.k > 0.0f ? g_box.img_wide : g_box.next_wide;
    if (g_panel.on && g_ph.phase == PGRAPH_PHASE_HUD) {
        /* a panel: x at the 4:3 proportions of the image (k2d; in a
         * framed image, kboxed: 0.75 when it is shown at 16:9, 1 when at
         * 4:3), its ends to the screen's edges (the frame's, framed).
         * kx < 0 says so: then ky, ay are the left and right edges, in the
         * draw's pixels (hud_vertex, and the vertex shader). */
        int boxed = g_box.k > 0.0f && !wide;
        float k = boxed ? (g_box.kboxed > 0.0f ? g_box.kboxed : 0.75f) : g_box.k2d;
        if (g_panel.wide_only && !wide) return 0;
        if (!(k > 0.0f && k < 1.0f)) {
            /* 4:3 (no race 2D, no frame), or an image already at 4:3 in
             * its frame: x as drawn, only the ends move */
            if (g_panel.scale_only || (!boxed && (g_box.k > 0.0f || g_box.k2d > 0.0f))) return 0;
            k = 1.0f;
        }
        *ax = 0.5f * sw;
        if (g_panel.scale_only) {
            *kx = k;
            *ky = 1.0f;
            *ay = 0.0f;
        } else {
            *kx = -k;
            *ky = g_panel.l * sw / 640.0f;
            *ay = g_panel.r * sw / 640.0f;
        }
        g_hud.draws++;
        return 1;
    }
    if (g_box.on && wide && (g_box.k2d > 0.0f || g_box.k > 0.0f) &&
        g_ph.phase == PGRAPH_PHASE_HUD) {
        /* framed: the title's 4:3 proportions, else the centred 16:9 */
        *kx = g_box.k2d > 0.0f ? g_box.k2d : g_box.k;
        *ky = 1.0f;
        *ax = 0.5f * sw;
        *ay = 0.0f;
        g_hud.draws++;
        return 1;
    }
    if (!(g_hud.on && g_hud.kx > 0.0f && g_ph.phase == PGRAPH_PHASE_HUD)) return 0;
    *kx = g_hud.kx;
    *ky = g_hud.ky;
    *ax = g_hud.ax * sw / 640.0f;
    *ay = g_hud.ay * sh / 480.0f;
    g_hud.draws++;
    return 1;
}

/* One vertex through hud_element's scale; kx < 0: a panel's end piece, the
 * vertices past its edges (ky, ay) to the screen's (0, width sw). Keep in
 * step with the vertex shader (d3d8_nv2a_vsh.c). */
static void hud_vertex(float *x, float *y, float kx, float ax, float ky, float ay)
{
    if (kx < 0.0f) {
        float sw = (float)d3d8_GetBackbufferWidth();
        if (sw <= 0.0f) sw = 640.0f;
        *x = *x <= ky ? 0.0f : *x >= ay ? sw : ax + (*x - ax) * -kx;
        return;
    }
    *x = ax + (*x - ax) * kx;
    *y = ay + (*y - ay) * ky;
}

/* A panel's end pieces on the CPU path (triangle list): x at the 4:3
 * proportions, and the vertices past the panel's edges to the screen's.
 * A piece is one quad holding a straight run and half of the bar's curve
 * (texture across the whole quad): moving its outer vertex alone would
 * stretch the curve. So the moved vertex takes the attributes (texture
 * coordinates, colours, fog) of the triangle's plane at its new place: the
 * texture keeps its 4:3 density and only runs on past the old end (the
 * straight run, with the texture clamped). */
static int panel_snap_list(ProgVertex *v, uint32_t n, float k, float ax, float el, float er)
{
    static ProgVertex *src;
    static uint32_t cap;
    float sw = (float)d3d8_GetBackbufferWidth();
    uint32_t t, j;
    int moved = 0;
    if (sw <= 0.0f) sw = 640.0f;
    if (n > cap) {
        ProgVertex *a = (ProgVertex *)realloc(src, n * sizeof *a);
        if (!a) return 0;
        src = a;
        cap = n;
    }
    for (j = 0; j < n; j++) {
        src[j] = v[j];
        src[j].x = ax + (v[j].x - ax) * k;      /* the 4:3 place of every vertex */
        v[j].x = src[j].x;
    }
    for (t = 0; t + 2 < n; t += 3) {
        const ProgVertex *a = &src[t], *b = &src[t + 1], *c = &src[t + 2];
        float d = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
        for (j = t; j < t + 3; j++) {
            float ox = ax + (src[j].x - ax) / k, nx, dx;
            const float *pa, *pb, *pc;
            float *out;
            int m;
            if (ox <= el) nx = 0.0f;
            else if (ox >= er) nx = sw;
            else continue;
            dx = nx - src[j].x;
            v[j].x = nx;
            moved++;
            if (!(d > 1e-6f || d < -1e-6f)) continue;
            /* d(attribute)/dx of the plane through a, b, c */
#define PANEL_EXTRA(field, cnt)             for (m = 0, pa = a->field, pb = b->field, pc = c->field, out = v[j].field; m < (cnt); m++)                 out[m] = src[j].field[m] + dx * (((pb[m] - pa[m]) * (c->y - a->y) - (pc[m] - pa[m]) * (b->y - a->y)) / d);
            PANEL_EXTRA(t[0], 4)
            PANEL_EXTRA(t[1], 4)
            PANEL_EXTRA(t[2], 4)
            PANEL_EXTRA(t[3], 4)
            PANEL_EXTRA(d0, 4)
            PANEL_EXTRA(d1, 4)
#undef PANEL_EXTRA
        }
    }
    return moved;
}

/* ── XBOX_CULLSTAT=N (diagnostic): which draws reach the screen ─────
 * Every N-th presented frame, each program draw is run through the CPU
 * vertex program (all its vertices) and classified: fully outside the
 * title frame (every vertex on the same outer side, or all behind the eye),
 * sub-pixel (screen box under 2x2 title pixels), or visible. One [CULL]
 * line per sampled frame: draws / vertices per class and per render group
 * (g0..g6 3D, 7 persp >= 7, 8 HUD, 9 other). Off by default; the sampled
 * frames are much slower (CPU transform of every vertex). */
static int g_cull_n = -1;
static int g_cull_grp = 9;
void cullstat_set_group(int ph, uint32_t tag)
{
    if (ph == 2) { if (((tag >> 14) & 3u) == 2u) { int g = (int)((tag >> 5) & 31u); g_cull_grp = g > 6 ? 6 : g; } }
    else g_cull_grp = ph == 3 ? 7 : ph == 4 ? 8 : 9;
}
static unsigned g_cull_frame = 0xFFFFFFFFu;
static struct { unsigned d[10][3]; unsigned long long v[10][3]; } g_cs;
static void cullstat_flush(unsigned f)
{
    int g, c;
    unsigned td[3] = {0, 0, 0};
    unsigned long long tv[3] = {0, 0, 0};
    char buf[1024];
    int o = 0;
    for (g = 0; g < 10; g++) for (c = 0; c < 3; c++) { td[c] += g_cs.d[g][c]; tv[c] += g_cs.v[g][c]; }
    if (td[0] + td[1] + td[2] == 0) return;
    for (g = 0; g < 10; g++)
        if (g_cs.d[g][0] + g_cs.d[g][1] + g_cs.d[g][2])
            o += snprintf(buf + o, sizeof buf - (size_t)o, " g%d %u/%u/%u", g,
                          g_cs.d[g][0], g_cs.d[g][1], g_cs.d[g][2]);
    fprintf(stderr, "[CULL] f%u draws vis/out/tiny %u/%u/%u verts %llu/%llu/%llu |%s\n", f,
            td[0], td[1], td[2], tv[0], tv[1], tv[2], buf);
    memset(&g_cs, 0, sizeof g_cs);
}
/* XBOX_PUMPLAG: a draw of a part whose matrix was rewritten. Its screen box
 * as drawn (the block now) and as the title meant it (the block when it drew
 * the part), both run through the CPU vertex program; the frame is saved
 * (XBOX_PUMPLAG_SHOTS). [PLAGD] lines, in title pixels (sw x sh). */
static void plag_box(const uint32_t *indices, uint32_t start, uint32_t count, float b[4], unsigned *front)
{
    uint32_t i;
    b[0] = b[1] = 1e30f; b[2] = b[3] = -1e30f; *front = 0;
    for (i = 0; i < count; i++) {
        float in[16][4], out[VSHCPU_OUT_REGS][4], x, y, w;
        uint32_t idx = indices ? indices[i] : start + i;
        int a;
        for (a = 0; a < 16; a++) {
            if (g_pg.prog_inputs & (1u << a)) va_fetch4(a, idx, in[a]);
            else in[a][0] = in[a][1] = in[a][2] = 0.0f, in[a][3] = 1.0f;
        }
        vshcpu_run(g_pg.prog_dec, g_pg.prog_len, (const float (*)[4])in, g_pg.vconst, out);
        x = out[VSHCPU_OUT_POS][0]; y = out[VSHCPU_OUT_POS][1]; w = out[VSHCPU_OUT_POS][3];
        if (!(w > 0.0f)) continue;
        (*front)++;
        if (x < b[0]) b[0] = x;
        if (y < b[1]) b[1] = y;
        if (x > b[2]) b[2] = x;
        if (y > b[3]) b[3] = y;
    }
}
static void plag_draw(const uint32_t *indices, uint32_t start, uint32_t count, float sw, float sh)
{
    static unsigned printed;
    extern void d3d8_PlagShot(void);
    float d[4], e[4];
    unsigned fd, fe;
    plag_box(indices, start, count, d, &fd);
    g_plag.patch = g_plag.snap;
    plag_box(indices, start, count, e, &fe);
    g_plag.patch = NULL;
    if (printed++ < 2000u)
        fprintf(stderr, "[PLAGD] f%u block %08X count %u drawn x %.1f..%.1f y %.1f..%.1f (%u in front) "
                "meant x %.1f..%.1f y %.1f..%.1f (%u in front) frame %.0fx%.0f\n",
                d3d8_PresentSeq(), g_plag.cb, count, d[0], d[2], d[1], d[3], fd, e[0], e[2], e[1], e[3], fe, sw, sh);
    d3d8_PlagShot();
}

static void cullstat_draw(const uint32_t *indices, uint32_t start, uint32_t count, float sw, float sh)
{
    unsigned f = d3d8_PresentSeq();
    uint32_t i;
    unsigned and_code = 0xFFu;
    float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
    int cls, front = 0;
    if (g_cull_n < 0) { const char *e = getenv("XBOX_CULLSTAT"); g_cull_n = e ? atoi(e) : 0; }
    if (g_cull_n <= 0) return;
    if (f != g_cull_frame) { if (g_cull_frame != 0xFFFFFFFFu) cullstat_flush(g_cull_frame); g_cull_frame = f; }
    if (f % (unsigned)g_cull_n) return;
    for (i = 0; i < count; i++) {
        float in[16][4], out[VSHCPU_OUT_REGS][4], x, y, z, w;
        uint32_t idx = indices ? indices[i] : start + i;
        unsigned code = 0;
        int a;
        for (a = 0; a < 16; a++) {
            if (g_pg.prog_inputs & (1u << a)) va_fetch4(a, idx, in[a]);
            else in[a][0] = in[a][1] = in[a][2] = 0.0f, in[a][3] = 1.0f;
        }
        vshcpu_run(g_pg.prog_dec, g_pg.prog_len, (const float (*)[4])in, g_pg.vconst, out);
        x = out[VSHCPU_OUT_POS][0]; y = out[VSHCPU_OUT_POS][1];
        z = out[VSHCPU_OUT_POS][2]; w = out[VSHCPU_OUT_POS][3];
        if (!(w > 0.0f)) code = 16;
        else {
            if (x < 0.0f) code |= 1;
            if (x > sw) code |= 2;
            if (y < 0.0f) code |= 4;
            if (y > sh) code |= 8;
            if (g_pg.clip_max > 0.0f && z > g_pg.clip_max) code |= 32;
            if (x < mnx) mnx = x;
            if (x > mxx) mxx = x;
            if (y < mny) mny = y;
            if (y > mxy) mxy = y;
            front = 1;
        }
        and_code &= code;
        if (!and_code && i > 64 && front && (mxx - mnx) * (mxy - mny) >= 4.0f) break;   /* visible: no need to finish */
    }
    if (and_code) cls = 1;
    else if (front && (mxx - mnx) < 2.0f && (mxy - mny) < 2.0f && i >= count) cls = 2;
    else cls = 0;
    g_cs.d[g_cull_grp][cls]++;
    g_cs.v[g_cull_grp][cls] += count;
}

static void draw_program(const uint32_t *indices, uint32_t start, uint32_t count)
{
    static ProgVertex *vb = NULL, *tb = NULL;
    static uint8_t *okb = NULL;
    static float *psb = NULL;               /* point sizes, for mode 1 */
    static uint32_t cap = 0;
    IDirect3DDevice8 *dev;
    uint32_t i, n;
    int prim = D3DPT_TRIANGLELIST;
    int points = g_pg.draw_mode == 1;
    Nv2aVshDraw gd;
    int gpu = 0;
    double pt0 = g_perf_on ? perf_now() : 0.0;

    if (count == 0)
        return;
    if (count > cap) {
        uint32_t c = count < 4096 ? 4096 : count;
        ProgVertex *a = (ProgVertex *)realloc(vb, c * sizeof *a);
        ProgVertex *b;
        uint8_t *k;
        if (!a) return;
        vb = a;
        b = (ProgVertex *)realloc(tb, 6u * c * sizeof *b);
        if (!b) return;
        tb = b;
        k = (uint8_t *)realloc(okb, c);
        if (!k) return;
        okb = k;
        {
            float *f = (float *)realloc(psb, c * sizeof *f);
            if (!f) return;
            psb = f;
        }
        cap = c;
    }
    {
        /* XBOX_NV2A_PICK needs the CPU-transformed vertices: take the CPU
         * path, but only in the frames XBOX_NV2A_DRAWLOG selects, so the
         * run keeps GPU timing up to the frame being examined. */
        /* The window is g_drawlog_lo/n, so the diag `drawlog` command arms
         * it live as well. */
        static int pick_cpu = -1;
        int f = (int)d3d8_PresentSeq();
        if (pick_cpu < 0) pick_cpu = getenv("XBOX_NV2A_PICK") != NULL;
        gpu = !(pick_cpu && g_drawlog_lo >= 0 && f >= g_drawlog_lo && f < g_drawlog_lo + g_drawlog_n)
              && !(g_panel.on && !g_panel.scale_only && g_ph.phase == PGRAPH_PHASE_HUD)  /* panel ends: CPU */
              && gpu_prepare(&gd, indices, start, count);
    }
    if (gpu) {
        n = gd.nindices;
        prim = gd.topology;
    } else if (indices && points) {
        /* Indexed points: no reuse to exploit, and each needs its size. */
        for (i = 0; i < count; i++)
            okb[i] = (uint8_t)vsh_vertex(indices[i], &vb[i], &psb[i]);
    } else if (indices) {
        /* An indexed list references each vertex several times; run the
         * program once per distinct index. Stamped slots avoid clearing the
         * cache between draws. Ranges wider than the cache fall back to one
         * run per reference. */
        #define VCACHE 65536u
        static uint32_t *stamp = NULL, now = 0;
        static ProgVertex *cv = NULL;
        static uint8_t *cok = NULL;
        uint32_t lo = indices[0], hi = indices[0];
        for (i = 1; i < count; i++) {
            if (indices[i] < lo) lo = indices[i];
            if (indices[i] > hi) hi = indices[i];
        }
        if (!stamp) {
            stamp = (uint32_t *)calloc(VCACHE, sizeof *stamp);
            cv = (ProgVertex *)malloc(VCACHE * sizeof *cv);
            cok = (uint8_t *)malloc(VCACHE);
        }
        if (stamp && cv && cok && hi - lo < VCACHE) {
            if (++now == 0) { memset(stamp, 0, VCACHE * sizeof *stamp); now = 1; }
            for (i = 0; i < count; i++) {
                uint32_t k = indices[i] - lo;
                if (stamp[k] != now) {
                    cok[k] = (uint8_t)vsh_vertex(indices[i], &cv[k], NULL);
                    stamp[k] = now;
                }
                vb[i] = cv[k];
                okb[i] = cok[k];
            }
        } else {
            for (i = 0; i < count; i++)
                okb[i] = (uint8_t)vsh_vertex(indices[i], &vb[i], NULL);
        }
        #undef VCACHE
    } else {
        for (i = 0; i < count; i++)
            okb[i] = (uint8_t)vsh_vertex(start + i, &vb[i], points ? &psb[i] : NULL);
    }
    /* XBOX_VSH_DUMPDRAW=N (diagnostic): for up to N large draws (>= 64
     * vertices, sampled every 2000th program draw), print the attribute
     * formats, the first vertex's inputs and the program's outputs. */
    {
        static int dumps = -1;
        if (dumps < 0) { const char *e = getenv("XBOX_VSH_DUMPDRAW"); dumps = e ? atoi(e) : 0; }
        if (dumps > 0 && count >= 64 && (g_vs.prog_draws % 2000) == 0) {
            float in[16][4], out[VSHCPU_OUT_REGS][4];
            uint32_t v0 = indices ? indices[0] : start;
            int a;
            dumps--;
            for (a = 0; a < 16; a++) va_fetch4(a, v0, in[a]);
            vshcpu_run(g_pg.prog_dec, g_pg.prog_len, (const float (*)[4])in, g_pg.vconst, out);
            fprintf(stderr, "[VSHDUMP] draw %u mode %u count %u idx0 %u prog@%u len %d inputs %04X\n",
                    g_vs.prog_draws, g_pg.draw_mode, count, v0, g_pg.prog_start, g_pg.prog_len,
                    g_pg.prog_inputs);
            for (a = 0; a < 16; a++)
                if (g_pg.vattr[a].format & 0xF0u)
                    fprintf(stderr, "[VSHDUMP]   v%-2d fmt %08X off %08X = %g %g %g %g\n", a,
                            g_pg.vattr[a].format, g_pg.vattr[a].offset,
                            in[a][0], in[a][1], in[a][2], in[a][3]);
            fprintf(stderr, "[VSHDUMP]   oPos %g %g %g %g  oD0 %g %g %g %g  oT0 %g %g\n",
                    out[0][0], out[0][1], out[0][2], out[0][3], out[3][0], out[3][1], out[3][2],
                    out[3][3], out[9][0], out[9][1]);
            for (a = 0; a < g_pg.prog_len && a < 40; a++) {
                char b[200];
                vshcpu_format(&g_pg.prog_dec[a], b, sizeof b);
                fprintf(stderr, "[VSHDUMP]   %2d %s\n", a, b);
            }
            fflush(stderr);
        }
    }
    /* XBOX_NV2A_DRAWLOG=F[:N] (diagnostic): describe every program draw of
     * frames F..F+N-1 (numbered like the XBOX_D3D_DUMP frames) -- texture stages, combiners,
     * blend, and the first vertex's program outputs. */
    {
        int lo, n_frames;
        if (g_drawlog_lo == -2) {
            const char *e = getenv("XBOX_NV2A_DRAWLOG");
            g_drawlog_lo = e ? atoi(e) : -1;
            if (e && strchr(e, ':')) g_drawlog_n = atoi(strchr(e, ':') + 1);
        }
        lo = g_drawlog_lo; n_frames = g_drawlog_n;
        static int only_mode = -2;
        if (only_mode == -2) {   /* XBOX_NV2A_DRAWLOG_MODE=N: only draws of that primitive mode */
            const char *e = getenv("XBOX_NV2A_DRAWLOG_MODE");
            only_mode = e ? atoi(e) : -1;
        }
        if (lo >= 0 && (int)d3d8_PresentSeq() >= lo && (int)d3d8_PresentSeq() < lo + n_frames &&
            (only_mode < 0 || (int)g_pg.draw_mode == only_mode))
            drawlog_program_draw(indices, indices ? indices[0] : start, count);
    }
    {
        /* XBOX_NV2A_SKIPTEX=<hex offset> (diagnostic): drop program draws
         * whose stage 0 samples that texture offset -- "what is under it". */
        static int init = 0;
        static uint32_t skip_off = 0;
        if (!init) { const char *e = getenv("XBOX_NV2A_SKIPTEX"); init = 1; if (e) skip_off = (uint32_t)strtoul(e, NULL, 16); }
        if (skip_off && g_pg.tex[0].enabled && g_pg.tex[0].offset == skip_off)
            return;
    }
    if (g_skip_prog) {
        uint32_t h = 2166136261u;
        int k;
        for (k = 0; k < g_pg.prog_len; k++) {
            h = (h ^ g_pg.prog[(g_pg.prog_start + k) % VSHCPU_SLOTS][1]) * 16777619u;
            h = (h ^ g_pg.prog[(g_pg.prog_start + k) % VSHCPU_SLOTS][2]) * 16777619u;
            h = (h ^ g_pg.prog[(g_pg.prog_start + k) % VSHCPU_SLOTS][3]) * 16777619u;
        }
        if (h == g_skip_prog) return;
    }
    if (gpu) {
        /* n and prim come from gpu_prepare; the vertices never touch the CPU. */
    } else if (points) {
        /* Points become screen-aligned squares of the program's point size
         *. D3D11 rasterises points as single pixels, so the
         * title's additive glow sprites -- the light pool under the rider in
         * character select, the haze in the cave -- had vanished. With
         * SET_POINT_SMOOTH_ENABLE stage 3 samples the sprite coordinate,
         * 0..1 from the top-left corner (xemu: gl_PointCoord). */
        n = points_squares(vb, okb, psb, count, tb);
        prim = D3DPT_TRIANGLELIST;
    } else {
        n = assemble(g_pg.draw_mode, vb, okb, count, tb, &prim);
    }
    {
        /* XBOX_NV2A_PRIMLOG=1 (diagnostic): every 5 s, the non-triangle
         * program draws -- mode, texture, blend, point size -- grouped. */
        static int on = -1;
        static struct { uint32_t key[4]; unsigned n, verts; float smin, smax, ssum; float pp[8]; } g[64];
        static int ng;
        static DWORD last;
        if (on < 0) { const char *e = getenv("XBOX_NV2A_PRIMLOG"); on = e && e[0] == '1'; }
        if (on && g_pg.draw_mode >= 1 && g_pg.draw_mode <= 4) {
            uint32_t key[4];
            int k;
            key[0] = g_pg.draw_mode | (g_pg.point_params_enable << 8) | (g_pg.point_smooth << 9);
            key[1] = g_pg.tex[3].enabled ? g_pg.tex[3].offset : g_pg.tex[0].enabled ? g_pg.tex[0].offset : 0;
            key[2] = g_pg.tex[3].enabled ? g_pg.tex[3].format : g_pg.tex[0].enabled ? g_pg.tex[0].format : 0;
            key[3] = (g_pg.blend_enable ? 0x80000000u : 0) | (g_pg.blend_sfactor << 16) | g_pg.blend_dfactor;
            for (k = 0; k < ng; k++)
                if (!memcmp(g[k].key, key, sizeof key)) break;
            if (k == ng && ng < 64) {
                memset(&g[ng], 0, sizeof g[ng]);
                memcpy(g[ng].key, key, sizeof key); g[ng].smin = 1e30f; g[ng].smax = -1e30f;
                memcpy(g[ng].pp, g_pg.point_params, sizeof g[ng].pp);
                ng++;
            }
            if (k < ng) {
                uint32_t v;
                g[k].n++; g[k].verts += count;
                for (v = 0; points && !gpu && v < count; v++) {
                    float s = psb[v];
                    if (s < g[k].smin) g[k].smin = s;
                    if (s > g[k].smax) g[k].smax = s;
                    g[k].ssum += s;
                }
            }
        }
        if (on && GetTickCount() - last > 5000) {
            int k;
            last = GetTickCount();
            fprintf(stderr, "[PRIMLOG] present %u, %d groups\n", d3d8_PresentSeq(), ng);
            for (k = 0; k < ng; k++)
                fprintf(stderr, "[PRIMLOG]   mode %u params %u smooth %u tex %08X fmt %08X blend %08X: %u draws %u verts size %.2f..%.2f\n",
                        g[k].key[0] & 0xFF, (g[k].key[0] >> 8) & 1, (g[k].key[0] >> 9) & 1, g[k].key[1], g[k].key[2],
                        g[k].key[3], g[k].n, g[k].verts, g[k].smin, g[k].smax);
            for (k = 0; k < ng; k++)
                fprintf(stderr, "[PRIMLOG]   group %d: mean size %.2f, point params %g %g %g %g %g %g %g %g, POINT_SIZE %u\n",
                        k, g[k].verts ? g[k].ssum / g[k].verts : 0.0f, g[k].pp[0], g[k].pp[1], g[k].pp[2],
                        g[k].pp[3], g[k].pp[4], g[k].pp[5], g[k].pp[6], g[k].pp[7], g_pg.point_size);
            ng = 0;
        }
    }
    {
        /* XBOX_NV2A_PICK=x,y (diagnostic): name every program draw whose
         * assembled triangles cover pixel (x,y), in the frames XBOX_NV2A_DRAWLOG
         * selects (all frames if it is unset) -- "which draw made this
         * pixel", with its interpolated oT0..oT3 and colours there. */
        static int pick = -1;
        static float px, py;
        if (pick < 0) {
            const char *e = getenv("XBOX_NV2A_PICK");
            pick = e && sscanf(e, "%f,%f", &px, &py) == 2;
        }
        if (pick && !gpu && prim == D3DPT_TRIANGLELIST) {
            const char *dl = getenv("XBOX_NV2A_DRAWLOG");
            int lo = dl ? atoi(dl) : 0, nf = (dl && strchr(dl, ':')) ? atoi(strchr(dl, ':') + 1) : 1;
            int f = (int)d3d8_PresentSeq();
            if (!dl || (f >= lo && f < lo + nf)) {
                uint32_t t;
                for (t = 0; t + 2 < n; t += 3) {
                    const ProgVertex *a = &tb[t], *b = &tb[t + 1], *c = &tb[t + 2];
                    float d = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
                    float l1, l2, l0;
                    if (d == 0.0f) continue;
                    l1 = ((px - a->x) * (c->y - a->y) - (c->x - a->x) * (py - a->y)) / d;
                    l2 = ((b->x - a->x) * (py - a->y) - (px - a->x) * (b->y - a->y)) / d;
                    l0 = 1.0f - l1 - l2;
                    if (l0 < 0.0f || l1 < 0.0f || l2 < 0.0f) continue;
                    fprintf(stderr, "[PICK] f%d draw %u mode %u n %u z %.5f blend %d %X/%X ztest %d/%d stages %d prog %08X "
                            "t0 %08X t1 %08X t2 %08X t3 %08X | oT0 %.3f %.3f oT1 %.3f %.3f oT2 %.3f %.3f oT3 %.3f %.3f %.3f %.3f "
                            "d0 %.2f %.2f %.2f %.2f d1 %.2f %.2f %.2f %.2f\n",
                            f, g_vs.prog_draws, g_pg.draw_mode, count, l0 * a->z + l1 * b->z + l2 * c->z,
                            g_pg.blend_enable, g_pg.blend_sfactor, g_pg.blend_dfactor, g_pg.depth_test, g_pg.depth_mask,
                            (int)(g_pg.comb_control & 0xFF), g_pg.shader_stage_program,
                            g_pg.tex[0].enabled ? g_pg.tex[0].offset : 0, g_pg.tex[1].enabled ? g_pg.tex[1].offset : 0,
                            g_pg.tex[2].enabled ? g_pg.tex[2].offset : 0, g_pg.tex[3].enabled ? g_pg.tex[3].offset : 0,
                            l0 * a->t[0][0] + l1 * b->t[0][0] + l2 * c->t[0][0], l0 * a->t[0][1] + l1 * b->t[0][1] + l2 * c->t[0][1],
                            l0 * a->t[1][0] + l1 * b->t[1][0] + l2 * c->t[1][0], l0 * a->t[1][1] + l1 * b->t[1][1] + l2 * c->t[1][1],
                            l0 * a->t[2][0] + l1 * b->t[2][0] + l2 * c->t[2][0], l0 * a->t[2][1] + l1 * b->t[2][1] + l2 * c->t[2][1],
                            l0 * a->t[3][0] + l1 * b->t[3][0] + l2 * c->t[3][0], l0 * a->t[3][1] + l1 * b->t[3][1] + l2 * c->t[3][1],
                            l0 * a->t[3][2] + l1 * b->t[3][2] + l2 * c->t[3][2], l0 * a->t[3][3] + l1 * b->t[3][3] + l2 * c->t[3][3],
                            a->d0[0], a->d0[1], a->d0[2], a->d0[3], a->d1[0], a->d1[1], a->d1[2], a->d1[3]);
                    fprintf(stderr, "[PICK]    stencil %d func %X ref %02X rmask %02X wmask %02X ops %X/%X/%X  cmask %08X  cull %d face %X front %X  zfunc %X  atest %d func %X ref %02X\n",
                            g_pg.stencil_test, g_pg.stencil_func, g_pg.stencil_ref, g_pg.stencil_mask_read,
                            g_pg.stencil_mask_write, g_pg.stencil_op[0], g_pg.stencil_op[1], g_pg.stencil_op[2],
                            g_pg.color_mask, g_pg.cull_enable, g_pg.cull_face, g_pg.front_face, g_pg.depth_func,
                            g_pg.alpha_test, g_pg.alpha_func, g_pg.alpha_ref & 0xFF);
                    break;
                }
            }
        }
    }
    g_vs.prog_draws++;
    g_vs.prog_verts += count;
    if (n == 0)
        return;

    dev = xbox_GetD3DDevice();
    if (!dev)
        return;
    apply_draw_state(dev);
    {
        /* apply_draw_state forces the 2D path's settings -- no depth, blend
         * always on. 3D geometry needs the title's own: opaque meshes whose
         * program leaves diffuse alpha at 0 vanish under forced blending, and
         * without depth the scene draws in submission order. GL compare and
         * blend enums map onto D3D's by offset / table. Alpha test runs in
         * the generated pixel shader. */
        uint32_t zf = g_pg.depth_func >= 0x200 && g_pg.depth_func <= 0x207
                    ? g_pg.depth_func - 0x200 + 1 : D3DCMP_LESSEQUAL;
        {
            /* XBOX_VSH_NODEPTH=1 (diagnostic) turns the depth test off. */
            static int nodepth = -1;
            if (nodepth < 0) { const char *e = getenv("XBOX_VSH_NODEPTH"); nodepth = e && e[0] == '1'; }
            dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, (g_pg.depth_test && !nodepth) ? TRUE : FALSE);
        }
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZFUNC, zf);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, g_pg.depth_mask ? TRUE : FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, g_pg.blend_enable ? TRUE : FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, nv2a_blend_to_d3d(g_pg.blend_sfactor));
        dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, nv2a_blend_to_d3d(g_pg.blend_dfactor));
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_SPECULARENABLE, FALSE);
        {
            /* Stencil and colour mask. Character select marks
             * the spotlight's volume in the stencil buffer (quads drawn with
             * blend ZERO/ONE, depth test on, depth write off) and then darkens
             * everything outside it with a full-screen ZERO/SRC_ALPHA quad;
             * without the stencil test the light pool was darkened too.
             * GL compare enums map onto D3D's by offset, ops by table. */
            /* D3D8 stencil ops: KEEP 1 ZERO 2 REPLACE 3 INCRSAT 4 DECRSAT 5 INVERT 6
             * INCR 7 DECR 8; colour write bits R 1 G 2 B 4 A 8. */
            static const struct { uint32_t nv; DWORD d3d; } ops[] = {
                { 0x1E00, 1 }, { 0x0000, 2 },
                { 0x1E01, 3 }, { 0x1E02, 4 },
                { 0x1E03, 5 }, { 0x150A, 6 },
                { 0x8507, 7 }, { 0x8508, 8 },
            };
            static const D3DRENDERSTATETYPE op_rs[3] = { D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS };
            uint32_t m = g_pg.color_mask;
            int k, j;
            dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILENABLE, g_pg.stencil_test ? TRUE : FALSE);
            if (g_pg.stencil_test) {
                dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILFUNC,
                    g_pg.stencil_func >= 0x200 && g_pg.stencil_func <= 0x207
                        ? g_pg.stencil_func - 0x200 + 1 : D3DCMP_ALWAYS);
                dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILREF, g_pg.stencil_ref);
                dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILMASK, g_pg.stencil_mask_read);
                dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILWRITEMASK, g_pg.stencil_mask_write);
                for (k = 0; k < 3; k++) {
                    DWORD v = 1;
                    for (j = 0; j < (int)(sizeof ops / sizeof ops[0]); j++)
                        if (ops[j].nv == g_pg.stencil_op[k]) v = ops[j].d3d;
                    dev->lpVtbl->SetRenderState(dev, op_rs[k], v);
                }
            }
            dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE,
                ((m >> 16) & 1 ? 1u : 0) | ((m >> 8) & 1 ? 2u : 0) |
                (m & 1 ? 4u : 0) | ((m >> 24) & 1 ? 8u : 0));
            /* The shadow quad -- a full-surface quad, stencil
             * tested, blend ZERO / SRC_ALPHA. With XBOX_SOFT_SHADOWS=1 the
             * device applies a soft version of it first; the quad then only
             * zeroes the stencil. Its darkness is the alpha of its diffuse
             * colour (the program run on the CPU for its four vertices, which
             * also give its extent: the soft passes stay inside it). */
            if (g_pg.stencil_test && g_pg.blend_enable && g_pg.blend_sfactor == 0 &&
                g_pg.blend_dfactor == 0x302 && count == 4 && d3d8_SoftShadowOn()) {
                static unsigned long long seen;
                ProgVertex q;
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, shade = 0.0f;
                uint32_t sw = g_pg.surface_clip_h >> 16, sh = g_pg.surface_clip_v >> 16, v;
                int ok = 1;
                for (v = 0; v < 4 && ok; v++) {
                    ok = vsh_vertex(indices ? indices[v] : start + v, &q, NULL);
                    if (q.x < x0) x0 = q.x;
                    if (q.x > x1) x1 = q.x;
                    if (q.y < y0) y0 = q.y;
                    if (q.y > y1) y1 = q.y;
                    if (v == 0) shade = q.d0[3];
                }
                if (ok && sw && sh && x1 > x0 && y1 > y0) {
                    if (shade < 0.0f) shade = 0.0f;
                    if (shade > 1.0f) shade = 1.0f;
                    if (seen++ < 4)
                        fprintf(stderr, "[SOFTSHADOW] quad %.0f,%.0f..%.0f,%.0f of %ux%u, shade %.3f\n",
                                x0, y0, x1, y1, sw, sh, shade);
                    if (d3d8_SoftShadowDraw(shade, g_pg.stencil_func >= 0x200 && g_pg.stencil_func <= 0x207
                                                       ? g_pg.stencil_func - 0x200 + 1 : D3DCMP_ALWAYS,
                                            g_pg.stencil_ref, g_pg.stencil_mask_read,
                                            x0 / (float)sw, y0 / (float)sh, x1 / (float)sw, y1 / (float)sh))
                        dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0);
                }
            }
        }
    }

    /* The NV2A pixel pipeline itself -- all four texture stages
     * and the register combiners, as HLSL from nv2a_psh.c. XBOX_NV2A_PSH=0
     * falls back to t0 * oD0 (the look before this part). */
    {
        static int psh_on = -1;
        static char hlsl[98304];
        static unsigned failures;
        Nv2aPshState ps;
        Nv2aPshConsts pc;
        uint64_t key;
        int st, nst = (int)(g_pg.comb_control & 0xFF);

        if (psh_on < 0) { const char *e = getenv("XBOX_NV2A_PSH"); psh_on = !(e && e[0] == '0'); }
        memset(&ps, 0, sizeof ps);
        memset(&pc, 0, sizeof pc);
        if (nst > 8) nst = 8;
        ps.combiner_control = g_pg.comb_control;
        ps.shader_stage_program = g_pg.shader_stage_program;
        ps.other_stage_input = g_pg.shader_other_stage_input;
        ps.final0 = g_pg.spec_fog_cw0;
        ps.final1 = g_pg.spec_fog_cw1;
        for (st = 0; st < nst; st++) {
            ps.rgb_in[st] = g_pg.comb_color_icw[st];
            ps.rgb_out[st] = g_pg.comb_color_ocw[st];
            ps.alpha_in[st] = g_pg.comb_alpha_icw[st];
            ps.alpha_out[st] = g_pg.comb_alpha_ocw[st];
        }
        for (st = 0; st < 4; st++) {
            uint32_t f = g_pg.tex[st].format, color = TEXFMT_COLOR(f), xf, bb;
            IDirect3DTexture8 *t = g_pg.tex[st].enabled ? tex_upload(dev, st) : NULL;
            dev->lpVtbl->SetTexture(dev, st, (IDirect3DBaseTexture8 *)t);
            if (!g_pg.tex[st].enabled)
                continue;
            {
                uint32_t au = g_pg.tex[st].address & 0xF, av = (g_pg.tex[st].address >> 8) & 0xF;
                uint32_t mag = (g_pg.tex[st].filter >> 24) & 0xF, min = (g_pg.tex[st].filter >> 16) & 0xFF;
                dev->lpVtbl->SetTextureStageState(dev, st, D3DTSS_ADDRESSU, au >= 1 && au <= 4 ? au : 3);
                dev->lpVtbl->SetTextureStageState(dev, st, D3DTSS_ADDRESSV, av >= 1 && av <= 4 ? av : 3);
                /* NV2A min filters (xemu's table): 1 nearest, 2 linear,
                 * 3 nearest/mip nearest, 4 linear/mip nearest, 5 nearest/
                 * mip linear, 6 linear/mip linear. The mip half is
                 * honoured now that the chain is uploaded, with the LOD bias
                 * (TEXTURE_FILTER 12:0, signed 1/256ths), the most-detailed
                 * level clamp (CONTROL0 29:18, 1/256ths) and the texture's
                 * own anisotropy (CONTROL0 5:4 = 1, 2, 4, 8). */
                uint32_t c0 = g_pg.tex[st].control0;
                int32_t bias = (int32_t)(g_pg.tex[st].filter & 0x1FFFu);
                union { float f; DWORD u; } fb;
                if (bias & 0x1000) bias -= 0x2000;
                fb.f = (float)bias / 256.0f;
                /* The PS2 game's buttons (XBOX_BUTTON_ICONS=ps2): their atlas
                 * is sampled nearest neighbour, mip levels included, so that
                 * its pixels show as they are (nv2a_btnicons.h). */
                int pt = btnicons_point(t);
                dev->lpVtbl->SetTextureStageState(dev, st, D3DTSS_MAGFILTER, (pt || mag == 1) ? 1 : 2);
                dev->lpVtbl->SetTextureStageState(dev, st, D3DTSS_MINFILTER,
                                                  (pt || min == 1 || min == 3 || min == 5) ? 1 : 2);
                dev->lpVtbl->SetTextureStageState(dev, st, D3DTSS_MIPFILTER,
                                                  (min == 3 || min == 4) ? 1 : (min == 5 || min == 6) ? (pt ? 1 : 2) : 0);
                dev->lpVtbl->SetTextureStageState(dev, st, D3DTSS_MIPMAPLODBIAS, fb.u);
                dev->lpVtbl->SetTextureStageState(dev, st, D3DTSS_MAXMIPLEVEL, ((c0 >> 18) & 0xFFFu) >> 8);
                dev->lpVtbl->SetTextureStageState(dev, st, D3DTSS_MAXANISOTROPY, 1u << ((c0 >> 4) & 3u));
            }
            ps.tex_enabled[st] = 1;
            ps.tex_dim[st] = (uint8_t)((f >> 4) & 0xF);
            ps.tex_cube[st] = (uint8_t)((f >> 2) & 1);
            ps.tex_rect[st] = (uint8_t)(!tex_is_swizzled(color) && !tex_dxt_format(color, &xf, &bb));
            ps.alphakill[st] = (uint8_t)((g_pg.tex[st].control0 >> 2) & 1);
            pc.tex_size[st][0] = (float)(1u << TEXFMT_SIZE_U(f));
            pc.tex_size[st][1] = (float)(1u << TEXFMT_SIZE_V(f));
            if (ps.tex_rect[st] && g_pg.tex[st].image_rect) {
                /* Linear images: IMAGE_RECT width 31:16, height 15:0. */
                pc.tex_size[st][0] = (float)(g_pg.tex[st].image_rect >> 16);
                pc.tex_size[st][1] = (float)(g_pg.tex[st].image_rect & 0xFFFF);
            }
        }
        {
            /* XBOX_NV2A_TEXSTATS=1 (diagnostic): every 5 s, how many draws
             * used each texture-shader mode, per stage, with the bound
             * texture's cube flag and dimensionality. */
            static int on = -1;
            static unsigned cnt[4][32][2][4], fmtcnt[64][2];
            static DWORD last;
            if (on < 0) { const char *e = getenv("XBOX_NV2A_TEXSTATS"); on = e && e[0] == '1'; }
            if (on) {
                DWORD now = GetTickCount();
                for (st = 0; st < 4; st++) {
                    uint32_t m = (g_pg.shader_stage_program >> (st * 5)) & 0x1F;
                    if (m && g_pg.tex[st].enabled) {
                        cnt[st][m][(g_pg.tex[st].format >> 2) & 1][((g_pg.tex[st].format >> 4) & 0xF) & 3]++;
                        fmtcnt[TEXFMT_COLOR(g_pg.tex[st].format) & 63][TEXFMT_MIPS(g_pg.tex[st].format) > 1]++;
                    }
                }
                if (now - last > 5000) {
                    int m, c, dm;
                    last = now;
                    fprintf(stderr, "[TEXSTATS] present %u:", d3d8_PresentSeq());
                    for (st = 0; st < 4; st++)
                        for (m = 0; m < 32; m++)
                            for (c = 0; c < 2; c++)
                                for (dm = 0; dm < 4; dm++)
                                    if (cnt[st][m][c][dm])
                                        fprintf(stderr, " s%d:m%02X%s/d%d=%u", st, m, c ? "cube" : "",
                                                dm, cnt[st][m][c][dm]);
                    fprintf(stderr, " | color:");
                    for (m = 0; m < 64; m++)
                        for (c = 0; c < 2; c++)
                            if (fmtcnt[m][c]) fprintf(stderr, " %02X%s=%u", m, c ? "+mips" : "", fmtcnt[m][c]);
                    memset(fmtcnt, 0, sizeof fmtcnt);
                    fprintf(stderr, "\n");
                    memset(cnt, 0, sizeof cnt);
                }
            }
        }
        {
            /* XBOX_NV2A_PSHINV=<file> (diagnostic): inventory of the
             * pixel pipeline per window of XBOX_NV2A_PSHINV_EVERY presents
             * (default 120). One line per distinct (stage program, other
             * stage input, final combiner, combiner count, per-stage
             * enabled/colour format/signed bits/cube) with draws, vertices,
             * the first draw's texture offsets and, for stages in
             * BUMPENVMAP(_LUM), the bump registers. */
            typedef struct {
                uint32_t prog, osi, f0, f1, cc, tf[4], off[4], bump[4][6];
                unsigned draws, verts, used;
            } InvE;
            static int on = -1;
            static FILE *out;
            static unsigned every = 120, win_lo;
            static InvE tab[1024];
            if (on < 0) {
                const char *e = getenv("XBOX_NV2A_PSHINV"), *n = getenv("XBOX_NV2A_PSHINV_EVERY");
                on = 0;
                if (e && e[0] && (out = fopen(e, "w")) != NULL) on = 1;
                if (n && atoi(n) > 0) every = (unsigned)atoi(n);
                win_lo = d3d8_PresentSeq();
            }
            if (on) {
                unsigned seq = d3d8_PresentSeq(), h, k, j;
                uint32_t tf[4];
                if (seq >= win_lo + every) {
                    int any = 0;
                    for (k = 0; k < 1024; k++) {
                        InvE *x = &tab[k];
                        if (!x->used) continue;
                        if (!any) { fprintf(out, "W %u %u\n", win_lo, seq); any = 1; }
                        fprintf(out, "E prog=%08X osi=%08X f0=%08X f1=%08X cc=%08X", x->prog, x->osi, x->f0, x->f1, x->cc);
                        for (j = 0; j < 4; j++)
                            fprintf(out, " s%u=%X:%08X", j, x->tf[j], x->off[j]);
                        fprintf(out, " draws=%u verts=%u", x->draws, x->verts);
                        for (j = 1; j < 4; j++) {
                            uint32_t m = (x->prog >> (j * 5)) & 0x1F;
                            if (m == 6 || m == 7) {
                                float b[6];
                                memcpy(b, x->bump[j], sizeof b);
                                fprintf(out, " bump%u=[%g %g %g %g] s=%g o=%g", j, b[0], b[1], b[2], b[3], b[4], b[5]);
                            }
                        }
                        fprintf(out, "\n");
                    }
                    fflush(out);
                    memset(tab, 0, sizeof tab);
                    win_lo = seq;
                }
                /* tf: bit 31 enabled, 30 cube, 29:28 dim&3, 27:24 signed bits (FILTER 31:28), 5:0 colour format */
                for (j = 0; j < 4; j++) {
                    uint32_t f = g_pg.tex[j].format;
                    tf[j] = g_pg.tex[j].enabled
                        ? (0x80000000u | (((f >> 2) & 1) << 30) | ((((f >> 4) & 0xF) & 3) << 28)
                           | (((g_pg.tex[j].filter >> 28) & 0xF) << 24) | (TEXFMT_COLOR(f) & 0x3F))
                        : 0;
                }
                h = (g_pg.shader_stage_program * 2654435761u) ^ (g_pg.spec_fog_cw0 * 40503u) ^ g_pg.spec_fog_cw1
                    ^ g_pg.shader_other_stage_input ^ (g_pg.comb_control << 7) ^ (tf[0] * 31u) ^ (tf[1] * 131u)
                    ^ (tf[2] * 1031u) ^ (tf[3] * 8191u);
                for (k = 0; k < 1024; k++) {
                    InvE *x = &tab[(h + k) & 1023];
                    if (!x->used) {
                        x->used = 1;
                        x->prog = g_pg.shader_stage_program; x->osi = g_pg.shader_other_stage_input;
                        x->f0 = g_pg.spec_fog_cw0; x->f1 = g_pg.spec_fog_cw1; x->cc = g_pg.comb_control;
                        for (j = 0; j < 4; j++) {
                            x->tf[j] = tf[j];
                            x->off[j] = tf[j] ? g_pg.tex[j].offset : 0;
                            memcpy(x->bump[j], g_pg.tex[j].bump, sizeof x->bump[j]);
                        }
                    }
                    if (x->prog == g_pg.shader_stage_program && x->osi == g_pg.shader_other_stage_input
                        && x->f0 == g_pg.spec_fog_cw0 && x->f1 == g_pg.spec_fog_cw1 && x->cc == g_pg.comb_control
                        && !memcmp(x->tf, tf, sizeof tf)) {
                        x->draws++;
                        x->verts += count;
                        break;
                    }
                }
            }
        }
        ps.alpha_test = (uint8_t)g_pg.alpha_test;
        ps.alpha_func = (uint8_t)(g_pg.alpha_func >= 0x200 && g_pg.alpha_func <= 0x207
                                  ? g_pg.alpha_func - 0x200 : 7);
        ps.simple = (uint8_t)!psh_on;
        {
            static int show = -1;
            if (show < 0) {
                static const char *const names[] = { "t0", "t1", "t2", "t3", "v0", "v1", "fog", "r0", "r1" };
                const char *e = getenv("XBOX_NV2A_PSH_SHOW");
                int k;
                show = 0;
                for (k = 0; e && k < 9; k++)
                    if (!strcmp(e, names[k])) show = k + 1;
            }
            ps.show = (uint8_t)show;
        }
        {
            /* XBOX_NV2A_WCLIP=0 (diagnostic) ignores the window clip. */
            static int wclip_on = -1;
            int k;
            if (wclip_on < 0) { const char *e = getenv("XBOX_NV2A_WCLIP"); wclip_on = !(e && e[0] == '0'); }
            if (wclip_on && g_pg.wclip_seen && wclip_in_shader) {
                /* The test compares SV_Position, which is in scene-target
                 * pixels; the regions are in the title's. */
                float sx, sy;
                d3d8_GetGuestScale(&sx, &sy);
                ps.window_clip = (uint8_t)(g_pg.wclip_type ? 2 : 1);
                for (k = 0; k < 8; k++) {
                    pc.clip_region[k][0] = (float)(g_pg.wclip_x[k] & 0xFFF) * sx;
                    pc.clip_region[k][1] = (float)(g_pg.wclip_y[k] & 0xFFF) * sy;
                    pc.clip_region[k][2] = (float)(((g_pg.wclip_x[k] >> 16) & 0xFFF) + 1) * sx;
                    pc.clip_region[k][3] = (float)(((g_pg.wclip_y[k] >> 16) & 0xFFF) + 1) * sy;
                }
            }
        }

        #define ARGB4(dst, v) do { (dst)[0] = (((v) >> 16) & 0xFF) / 255.0f; \
            (dst)[1] = (((v) >> 8) & 0xFF) / 255.0f; (dst)[2] = ((v) & 0xFF) / 255.0f; \
            (dst)[3] = (((v) >> 24) & 0xFF) / 255.0f; } while (0)
        for (st = 0; st < 8; st++) {
            ARGB4(pc.c0[st], g_pg.comb_factor0[st]);
            ARGB4(pc.c1[st], g_pg.comb_factor1[st]);
        }
        ARGB4(pc.c0[8], g_pg.specular_fog_factor[0]);
        ARGB4(pc.c1[8], g_pg.specular_fog_factor[1]);
        #undef ARGB4
        pc.fog_color[0] = (g_pg.fog_color & 0xFF) / 255.0f;          /* ABGR */
        pc.fog_color[1] = ((g_pg.fog_color >> 8) & 0xFF) / 255.0f;
        pc.fog_color[2] = ((g_pg.fog_color >> 16) & 0xFF) / 255.0f;
        pc.fog_color[3] = ((g_pg.fog_color >> 24) & 0xFF) / 255.0f;
        pc.alpha_ref[0] = (float)(g_pg.alpha_ref & 0xFF);

        double ptk = g_perf_on ? perf_now() : 0.0;
        if (d3d8_pump_cache_on()) {
            /* Same state byte for byte as the previous draw =
             * same key (the key is a hash of these bytes). */
            static Nv2aPshState last;
            static uint64_t last_key;
            static int have;
            if (have && !memcmp(&ps, &last, sizeof ps))
                key = last_key;
            else {
                key = nv2a_psh_key(&ps);
                last = ps; last_key = key; have = 1;
            }
        } else
            key = nv2a_psh_key(&ps);
        if (g_perf_on) perf_add(PZ_PSH, perf_now() - ptk);
        if (!d3d8_nv2a_has_ps(key)) {
            int len = nv2a_psh_generate(&ps, hlsl, (int)sizeof hlsl);
            {
                /* XBOX_NV2A_PSH_DUMP=<dir> (diagnostic): write each new pixel
                 * shader's HLSL as <dir>/psh_<key>.hlsl. */
                const char *dir = getenv("XBOX_NV2A_PSH_DUMP");
                if (dir && len > 0) {
                    char path[600];
                    FILE *f;
                    snprintf(path, sizeof path, "%s/psh_%016llX.hlsl", dir, (unsigned long long)key);
                    f = fopen(path, "wb");
                    if (f) { fwrite(hlsl, 1, (size_t)len, f); fclose(f); }
                }
            }
            if (len <= 0 || !d3d8_nv2a_add_ps(key, hlsl, len)) {
                if (failures++ < 8)
                    fprintf(stderr, "[NV2A-PSH] no shader for combiners %08X stages %d prog %08X "
                            "(generate %d); draw skipped\n", g_pg.comb_control, nst,
                            g_pg.shader_stage_program, len);
                return;
            }
        }

        dev->lpVtbl->BeginScene(dev);
        {
            /* Bounded batches; a whole course section can be one strip. */
            uint32_t per = (prim == D3DPT_TRIANGLELIST) ? 3u : 1u;
            /* Rasteriser flags for d3d8_nv2a_draw: depth clip from
             * SET_ZMIN_MAX_CONTROL; face culling from SET_CULL_FACE_ENABLE /
             * _FACE / SET_FRONT_FACE, done by the rasteriser after near-plane
             * clipping. xemu hands NV2A screen coordinates to GL as window
             * coordinates, so NV2A's CCW front face is a triangle that is
             * clockwise on the render target: D3D11's FrontCounterClockwise
             * FALSE. Point sprites are never culled. XBOX_NV2A_CULL=0
             * (diagnostic) turns culling off. */
            static int cull_on = -1;
            int raster = ((g_pg.zmin_max_control >> 4) & 0xF) == 0 ? 1 : 0;
            if (cull_on < 0) { const char *e = getenv("XBOX_NV2A_CULL"); cull_on = !(e && e[0] == '0'); }
            if (cull_on && g_pg.cull_enable && !points && prim == D3DPT_TRIANGLELIST) {
                if (g_pg.cull_face == 0x408) {                    /* FRONT_AND_BACK */
                    dev->lpVtbl->EndScene(dev);
                    return;
                }
                raster |= (g_pg.cull_face == 0x404 ? 1 : 2) << 1;
                {
                    /* NV2A's screen space is y-down like the D3D11
                     * render target, so a CW front face is CW there too:
                     * FrontCounterClockwise only for CCW (0x901). The old
                     * mapping (CCW for 0x900) culled the wrong side; the title
                     * enables culling almost only for the FogVolume spheres
                     * (front 0x900, cull FRONT), whose far side carries the
                     * alpha, so the menu fog curtains and the course mist
                     * vanished. XBOX_FIX_CULLWIND=0 restores the old mapping. */
                    static int fix = -1;
                    if (fix < 0) { const char *e = getenv("XBOX_FIX_CULLWIND"); fix = !(e && e[0] == '0'); }
                    if (fix ? g_pg.front_face == 0x901 : g_pg.front_face == 0x900) raster |= 8;
                }
            }
            {
                /* Race HUD element in its own proportions. */
                float kx, ax, ky, ay;
                if (hud_element(&kx, &ax, &ky, &ay)) {
                    if (gpu) { gd.hud_kx = kx; gd.hud_ax = ax; gd.hud_ky = ky; gd.hud_ay = ay; }
                    else if (kx < 0.0f && prim == D3DPT_TRIANGLELIST && !points)
                        g_panel.moved += (unsigned long long)panel_snap_list(tb, n, -kx, ax, ky, ay);
                    else for (i = 0; i < n; i++)
                        hud_vertex(&tb[i].x, &tb[i].y, kx, ax, ky, ay);
                }
            }
            if (gpu && !points) cullstat_draw(indices, start, count, gd.screen_w, gd.screen_h);
            if (gpu && g_plag.bad && !points) plag_draw(indices, start, count, gd.screen_w, gd.screen_h);
            if (gpu) {
                gd.ps_key = key;
                gd.ps_consts = &pc;
                gd.ps_consts_size = sizeof pc;
                gd.raster = raster;
                if (points && d3d8_points_check_on()) {
                    /* XBOX_POINTS_CHECK: the CPU squares, as reference. */
                    for (i = 0; i < count; i++)
                        okb[i] = (uint8_t)vsh_vertex(indices ? indices[i] : start + i, &vb[i], &psb[i]);
                    d3d8_nv2a_points_expect(tb, points_squares(vb, okb, psb, count, tb), sizeof(ProgVertex));
                }
                if (d3d8_nv2a_draw_program_gpu(&gd) && prim == D3DPT_TRIANGLELIST)
                    g_vs.prog_tris += n / 3;
            } else {
            uint32_t chunk = 18000u - (18000u % per), off;
            for (off = 0; off < n; off += chunk) {
                uint32_t m = (n - off < chunk) ? n - off : chunk;
                uint32_t pcount = 0;
                switch (prim) {
                case D3DPT_TRIANGLELIST: pcount = m / 3; g_vs.prog_tris += pcount; break;
                case D3DPT_LINELIST:     pcount = m / 2; break;
                case D3DPT_LINESTRIP:    pcount = m >= 2 ? m - 1 : 0; break;
                default:                 pcount = m; break;
                }
                if (pcount)
                    d3d8_nv2a_draw((D3DPRIMITIVETYPE)prim, pcount, tb + off, sizeof(ProgVertex),
                                   key, &pc, sizeof pc,
                                   raster);
            }
            }
        }
    }
    peek_after("program draw");
    if (g_perf_on) {
        perf_add(gpu ? PZ_PROGGPU : PZ_PROGCPU, perf_now() - pt0);
        if (!gpu) { perf_count(PC_CPUVTX, count); perf_count(PC_CPUDRAW, 1); }
    }
    g_pg.stats.draw_calls++;
    g_pg.stats.vertices_submitted += count;
    g_pg.draws_since_present++;

    if ((g_vs.prog_draws % 5000) == 1) {
        fprintf(stderr, "[VSH] %u program draws, %u verts, %u tris, %u tris behind the eye, "
                "%u programs decoded, %u fixed-mode indexed draws; last v0=(%.1f, %.1f, %.4f, rhw %.4g)\n",
                g_vs.prog_draws, g_vs.prog_verts, g_vs.prog_tris, g_vs.culled_w,
                g_vs.decoded, g_vs.raw_indexed, vb[0].x, vb[0].y, vb[0].z, vb[0].rhw);
        fprintf(stderr, "[VSH]   window: z<0 %u, z>1 %u, xy off-screen %u, w<=0 %u; clip %g..%g, "
                "vpscl %g %g %g %g, vpoff %g %g %g %g\n",
                g_vs.z_neg, g_vs.z_over, g_vs.xy_off, g_vs.w_neg, g_pg.clip_min, g_pg.clip_max,
                g_pg.vconst[0x3A][0], g_pg.vconst[0x3A][1], g_pg.vconst[0x3A][2], g_pg.vconst[0x3A][3],
                g_pg.vconst[0x3B][0], g_pg.vconst[0x3B][1], g_pg.vconst[0x3B][2], g_pg.vconst[0x3B][3]);
        fprintf(stderr, "[VSH]   gpu %u, cpu: off %u points %u cwrite %u mode %u format %u (repacked %u) range %u shader %u\n",
                g_gpu_ok, g_gpu_fb[0], g_gpu_fb[1], g_gpu_fb[2], g_gpu_fb[3], g_gpu_fb[4],
                g_gpu_fb[5], g_gpu_fb[6], g_gpu_fb[7]);
        g_vs.z_neg = g_vs.z_over = g_vs.xy_off = g_vs.w_neg = 0;
        fflush(stderr);
    }
}

/* The same for the title's pre-transformed vertices (screen pixels). */
static void hud_scale_out(OutputVertex *out, uint32_t n)
{
    float kx, ax, ky, ay;
    uint32_t i;
    if (!hud_element(&kx, &ax, &ky, &ay)) return;
    for (i = 0; i < n; i++)
        hud_vertex(&out[i].x, &out[i].y, kx, ax, ky, ay);
}

/* Gather `count` vertices starting at `start` from the bound streams and hand
 * them to the same DrawPrimitiveUP path the inline route uses. */
static void draw_arrays(uint32_t start, uint32_t count)
{
    IDirect3DDevice8 *dev;
    OutputVertex *out;
    uint32_t i, prim_count = 0;
    int prim = g_pg.d3d_prim_type;

    if (count == 0 || g_pg.vattr[NV2A_ATTR_POSITION].format == 0) {
        static int told = 0;
        if (!told) {
            told = 1;
            fprintf(stderr, "[PGRAPH-D3D11] DRAW_ARRAYS skipped: position attribute has "
                    "no format (offset=0x%08X). The title binds stream addresses but "
                    "never writes SET_VERTEX_DATA_ARRAY_FORMAT.\n",
                    g_pg.vattr[NV2A_ATTR_POSITION].offset);
            fflush(stderr);
        }
        return;
    }
    if (count > MAX_INLINE_VERTS)
        count = MAX_INLINE_VERTS;

    switch (prim) {
    case D3DPT_TRIANGLELIST:  prim_count = count / 3; break;
    case D3DPT_TRIANGLESTRIP: prim_count = (count >= 3) ? count - 2 : 0; break;
    case D3DPT_TRIANGLEFAN:   prim_count = (count >= 3) ? count - 2 : 0; break;
    case D3DPT_LINELIST:      prim_count = count / 2; break;
    case D3DPT_LINESTRIP:     prim_count = (count >= 2) ? count - 1 : 0; break;
    case D3DPT_POINTLIST:     prim_count = count; break;
    default:                  prim_count = 0; break;
    }
    if (prim_count == 0)
        return;

    dev = xbox_GetD3DDevice();
    if (!dev)
        return;

    out = (OutputVertex *)_alloca(count * sizeof(OutputVertex));
    {
    int tc = va_slot_texcoord0();
    for (i = 0; i < count; i++) {
        uint32_t v = start + i;
        out[i].x     = va_read(NV2A_ATTR_POSITION,  v, 0, 0.0f);
        out[i].y     = va_read(NV2A_ATTR_POSITION,  v, 1, 0.0f);
        out[i].z     = va_read(NV2A_ATTR_POSITION,  v, 2, 0.0f);
        out[i].rhw   = va_read(NV2A_ATTR_POSITION,  v, 3, 1.0f);
        out[i].color = va_read_color(v);
        out[i].u     = va_read(tc, v, 0, 0.0f);
        out[i].v     = va_read(tc, v, 1, 0.0f);
        if (out[i].rhw == 0.0f)
            out[i].rhw = 1.0f;
    }
    }

    apply_draw_state(dev);
    /* XBOX_D3D_VTXLOG=1 dumps the geometry actually handed to the rasteriser.
     * The title settles into exactly one 4-vertex draw per frame, and the
     * screen changes from white to black while that draw keeps being issued --
     * so the change is in the quad's own data, which a draw count cannot show. */
    {
        static int vlog = -1;
        if (vlog < 0) {
            const char *e = getenv("XBOX_D3D_VTXLOG");
            vlog = (e && e[0] == '1') ? 1 : 0;
        }
        if (vlog && count <= 8) {
            IDirect3DBaseTexture8 *dbg_tex = NULL;
            uint32_t vi;
            dev->lpVtbl->GetTexture(dev, 0, &dbg_tex);
            fprintf(stderr, "[draw %5u] prim=%d nv=%u tex=%p\n",
                    g_pg.stats.draw_calls, prim, count,
                    (void *)dbg_tex);
            fprintf(stderr, "    tex0 en=%d off=%08X fmt=%08X d3d=%p"
                    " (%ux%u)\n",
                    g_pg.tex[0].enabled, g_pg.tex[0].offset, g_pg.tex[0].format,
                    (void *)g_pg.tex[0].d3d,
                    1u << TEXFMT_SIZE_U(g_pg.tex[0].format),
                    1u << TEXFMT_SIZE_V(g_pg.tex[0].format));
            for (vi = 0; vi < 16; vi++)
                if (g_pg.vattr[vi].format)
                    fprintf(stderr, "    attr%-2u fmt=%08X off=%08X"
                            " (type=%u size=%u stride=%u)\n", vi,
                            g_pg.vattr[vi].format, g_pg.vattr[vi].offset,
                            g_pg.vattr[vi].format & 0xF,
                            (g_pg.vattr[vi].format >> 4) & 0xF,
                            (g_pg.vattr[vi].format >> 8) & 0xFFFFFF);
            /* The whole 64-byte vertex, not just the 16 bytes the position
             * array claims.  Every attribute reports stride=64 while only
             * position reports a non-zero size, so if colour and texcoords
             * really are in the buffer they are sitting in the other 48
             * bytes and only the format writes were lost. */
            {
                uint32_t st = (g_pg.vattr[0].format >> 8) & 0xFFFFFFu;
                uint32_t vj, k;
                for (vj = 0; vj < count && vj < 4; vj++) {
                    const uint8_t *raw = va_ptr(g_pg.vattr[0].offset + (start + vj) * st, st);
                    if (!raw) break;
                    fprintf(stderr, "    raw%u:", vj);
                    for (k = 0; k < st && k < 64; k++)
                        fprintf(stderr, "%s%02X", (k % 4) ? "" : " ", raw[k]);
                    fprintf(stderr, "\n");
                }
            }
            for (vi = 0; vi < count; vi++)
                fprintf(stderr, "    v%u xyzw=%.1f,%.1f,%.3f,%.3f c=%08X uv=%.3f,%.3f\n",
                        vi, out[vi].x, out[vi].y, out[vi].z, out[vi].rhw,
                        out[vi].color, out[vi].u, out[vi].v);
            fflush(stderr);
        }
    }

    /* XBOX_SKIP_BLACK_QUAD: diagnostic only. The frontend ends each frame with
     * an opaque black full-screen quad over SRC_ALPHA/INV_SRC_ALPHA blending,
     * which hides everything drawn before it. Dropping it answers whether the
     * menu underneath is actually being rasterised. Not a fix -- the fade is
     * supposed to ramp, and this just proves what is behind it. */
    {
        static int skip_black = -1;
        if (skip_black < 0) skip_black = getenv("XBOX_SKIP_BLACK_QUAD") ? 1 : 0;
        if (skip_black && count == 4 && out[0].color == 0xFF000000u
            && out[0].x == 0.0f && out[0].y == 0.0f
            && out[3].x >= 639.0f && out[3].y >= 479.0f) {
            g_pg.stats.draw_calls++;
            return;
        }
    }

    hud_scale_out(out, count);
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->DrawPrimitiveUP(dev, (D3DPRIMITIVETYPE)prim, prim_count,
                                 out, sizeof(OutputVertex));

    g_pg.stats.draw_calls++;
    g_pg.stats.vertices_submitted += count;
    g_pg.draws_since_present++;

    /* Draws-per-second, so "the menu is drawn every frame" can be checked
     * against the clock rather than against a total. */
    {
        static clock_t t0 = 0, last = 0;
        static unsigned mark = 0;
        static int on = -1;
        clock_t now;
        if (on < 0) on = getenv("XBOX_DRAWRATE_LOG") ? 1 : 0;
        now = on ? clock() : 0;
        if (on && !t0) { t0 = now; last = now; }
        if (on && now - last >= CLOCKS_PER_SEC) {
            fprintf(stderr, "[DRAWRATE] t=%.1fs draws=%u (+%u this second)\n",
                    (double)(now - t0) / (double)CLOCKS_PER_SEC,
                    g_pg.stats.draw_calls, g_pg.stats.draw_calls - mark);
            fflush(stderr);
            mark = g_pg.stats.draw_calls;
            last = now;
        }
    }

    /* Pre-transformed vertices are clipped on z outside [0,1] whatever the
     * depth test says, so a bad z silently removes the draw. Count them. */
    {
        static unsigned bad_z = 0, tot_z = 0;
        static int on = -1;
        unsigned vi2;
        int bad = 0;
        if (on < 0) on = getenv("XBOX_ZCLIP_LOG") ? 1 : 0;
        for (vi2 = 0; on && vi2 < count; vi2++)
            if (!(out[vi2].z >= 0.0f && out[vi2].z <= 1.0f)) bad = 1;
        if (on) tot_z++;
        if (bad) bad_z++;
        if (on && (tot_z % 2000) == 0)
            fprintf(stderr, "[ZCLIP] %u of %u draws have z outside [0,1]\n",
                    bad_z, tot_z), fflush(stderr);
    }

    if (g_pg.stats.draw_calls <= 8 || (g_pg.stats.draw_calls % 2000) == 0) {
        fprintf(stderr,
                "[PGRAPH-D3D11] DrawArrays #%u: start=%u count=%u prim=%d prims=%u "
                "posfmt=0x%08X posoff=0x%08X v0=(%.2f, %.2f, %.2f, %.2f) c0=0x%08X\n",
                g_pg.stats.draw_calls, start, count, prim, prim_count,
                g_pg.vattr[NV2A_ATTR_POSITION].format,
                g_pg.vattr[NV2A_ATTR_POSITION].offset,
                out[0].x, out[0].y, out[0].z, out[0].rhw, out[0].color);
        fflush(stderr);
        {
            /* Dump the stream bytes as they are at this instant. The decode is
             * confirmed correct against the raw command stream, so whatever is
             * wrong is in the memory the offset points at, not in the parse. */
            uint32_t st = (g_pg.vattr[NV2A_ATTR_POSITION].format >> 8) & 0xFFFFFFu;
            uint32_t vi;
            /* All four vertices, not just the first: the decode is confirmed
             * correct against the raw command stream, so if the quad is
             * degenerate the numbers themselves have to show it. */
            for (vi = 0; vi < count && vi < 4; vi++) {
                const uint8_t *raw = va_ptr(g_pg.vattr[NV2A_ATTR_POSITION].offset
                                            + vi * st, 16);
                float f[4];
                if (!raw) break;
                memcpy(f, raw, sizeof f);
                fprintf(stderr, "[PGRAPH-D3D11]   v%u = (%.3f, %.3f, %.3f, %.3f)\n",
                        vi, f[0], f[1], f[2], f[3]);
            }
            fflush(stderr);
        }
    }
}

static void submit_draw(void)
{
    if (g_pg.inline_count == 0 || g_pg.vert_stride == 0)
        return;

    uint32_t num_verts = g_pg.inline_count / g_pg.vert_stride;
    if (num_verts < 3)
        return;
    {   /* In the XBOX_NV2A_DRAWLOG / diag `drawlog` frames: inline-array draws
         * too, which the program draw log never saw. */
        int f = (int)d3d8_PresentSeq();
        if (g_drawlog_lo >= 0 && f >= g_drawlog_lo && f < g_drawlog_lo + g_drawlog_n) {
            const uint32_t *v = g_pg.inline_data;
            fprintf(stderr, "[DRAW] f%d INLINE mode %u n %u stride %u xf %u vsh %d blend %d %X/%X z %d | v0 %08X %08X %08X %08X %08X %08X\n",
                    f, g_pg.draw_mode, num_verts, g_pg.vert_stride, g_pg.xf_mode, vsh_active(),
                    g_pg.blend_enable, g_pg.blend_sfactor, g_pg.blend_dfactor, g_pg.depth_test,
                    v[0], v[1], v[2], v[3], v[4], g_pg.vert_stride > 5 ? v[5] : 0);
        }
    }

    const uint32_t *src = g_pg.inline_data;
    int actual_prim_type = g_pg.d3d_prim_type;
    uint32_t out_vert_count = num_verts;

    /* Handle QUADS (mode 8): convert to triangle list (6 verts per quad) */
    int is_quads = (g_pg.draw_mode == 8);
    uint32_t num_quads = is_quads ? (num_verts / 4) : 0;
    if (is_quads) {
        out_vert_count = num_quads * 6;  /* 2 triangles per quad */
        actual_prim_type = D3DPT_TRIANGLELIST;
    }

    /* Calculate primitive count */
    uint32_t prim_count = 0;
    switch (actual_prim_type) {
        case D3DPT_TRIANGLELIST:  prim_count = out_vert_count / 3; break;
        case D3DPT_TRIANGLESTRIP: prim_count = out_vert_count - 2; break;
        case D3DPT_TRIANGLEFAN:   prim_count = out_vert_count - 2; break;
        case D3DPT_LINELIST:      prim_count = out_vert_count / 2; break;
        case D3DPT_LINESTRIP:     prim_count = out_vert_count - 1; break;
        default: prim_count = out_vert_count / 3; break;
    }
    if (prim_count == 0)
        return;

    /* Convert inline vertices to OutputVertex (28 bytes) */
    OutputVertex *out = (OutputVertex *)_alloca(out_vert_count * sizeof(OutputVertex));

    /* Helper to convert one inline vertex */
    #define CONVERT_VERT(dst_idx, src_idx) do { \
        uint32_t _b = (src_idx) * g_pg.vert_stride; \
        out[dst_idx].x     = u2f(src[_b + 0]); \
        out[dst_idx].y     = u2f(src[_b + 1]); \
        out[dst_idx].z     = 0.0f; \
        out[dst_idx].rhw   = 1.0f; \
        out[dst_idx].u     = u2f(src[_b + 2]); \
        out[dst_idx].v     = u2f(src[_b + 3]); \
        out[dst_idx].color = src[_b + 4]; \
    } while(0)

    if (is_quads) {
        /* Convert quads (v0,v1,v2,v3) → two triangles (v0,v1,v2), (v0,v2,v3) */
        uint32_t out_idx = 0;
        for (uint32_t q = 0; q < num_quads; q++) {
            uint32_t qi = q * 4;
            CONVERT_VERT(out_idx + 0, qi + 0);  /* tri 1: v0 */
            CONVERT_VERT(out_idx + 1, qi + 1);  /* tri 1: v1 */
            CONVERT_VERT(out_idx + 2, qi + 2);  /* tri 1: v2 */
            CONVERT_VERT(out_idx + 3, qi + 0);  /* tri 2: v0 */
            CONVERT_VERT(out_idx + 4, qi + 2);  /* tri 2: v2 */
            CONVERT_VERT(out_idx + 5, qi + 3);  /* tri 2: v3 */
            out_idx += 6;
        }
    } else {
        for (uint32_t i = 0; i < num_verts; i++) {
            CONVERT_VERT(i, i);
        }
    }
    #undef CONVERT_VERT

    /* Chyron scroll: shift X for vertices in the chyron Y band (366-382).
     * Simple continuous scroll — no per-vertex wrapping to avoid artifacts
     * from split triangle-strip quads spanning the screen. */
    if (g_pg.chyron_scroll_offset != 0.0f && out_vert_count >= 6) {
        /* Check if this draw is in the chyron band */
        int is_chyron = 1;
        for (uint32_t i = 0; i < (out_vert_count < 8 ? out_vert_count : 8); i++) {
            if (out[i].y < 360.0f || out[i].y > 390.0f) {
                is_chyron = 0;
                break;
            }
        }
        if (is_chyron) {
            /* Find the total text width */
            float min_x = 9999.0f, max_x = -9999.0f;
            for (uint32_t i = 0; i < out_vert_count; i++) {
                if (out[i].x < min_x) min_x = out[i].x;
                if (out[i].x > max_x) max_x = out[i].x;
            }
            float text_width = max_x - min_x;

            /* Scroll loops: text slides left, then resets to start position.
             * Total cycle = text scrolls fully off-left + re-enters from right. */
            float cycle = text_width + 640.0f;
            float scroll = fmodf(g_pg.chyron_scroll_offset, cycle);

            /* Apply uniform shift to ALL vertices (no per-vertex wrap) */
            for (uint32_t i = 0; i < out_vert_count; i++) {
                out[i].x -= scroll;
            }
        }
    }

    /* Log first few draws' vertex positions (once) */
    if (g_pg.stats.draw_calls < 3 && num_verts >= 3) {
        fprintf(stderr, "[PGRAPH-D3D11] Draw verts (mode=%u, %u in → %u out):\n",
                g_pg.draw_mode, num_verts, out_vert_count);
        uint32_t show = num_verts < 8 ? num_verts : 8;
        for (uint32_t i = 0; i < show; i++) {
            uint32_t b = i * g_pg.vert_stride;
            fprintf(stderr, "  [%u] pos=(%.1f, %.1f) uv=(%.3f, %.3f) color=0x%08X\n",
                    i, u2f(src[b+0]), u2f(src[b+1]), u2f(src[b+2]), u2f(src[b+3]), src[b+4]);
        }
    }

    /* Get D3D8 device */
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();
    if (!dev) return;

    /* Set up 2D render state — always enable alpha for menu transparency */
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SPECULARENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
    /* XBOX_TEST_NOBLEND=1 forces blending off. Every draw here uses
     * SRCALPHA/INVSRCALPHA, so a source alpha of zero makes the draw a no-op
     * and is indistinguishable from "the draw never happened" when all you can
     * see is the frame buffer. This separates the two. Diagnostic; default on
     * (i.e. blending enabled) so behaviour is unchanged unless asked for. */
    {
        static int noblend = -1;
        if (noblend < 0) {
            const char *e = getenv("XBOX_TEST_NOBLEND");
            noblend = (e && e[0] == '1') ? 1 : 0;
        }
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,
                                    noblend ? FALSE : TRUE);
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    /* Set FVF for pre-transformed 2D with texture */
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    /* Bind texture based on NV2A VRAM offset.
     * Game-specific texture mapping is handled via GAME_HAS_FONT_ATLAS
     * compile flag. Generic path uses vertex color only. */
#ifdef GAME_HAS_FONT_ATLAS
    if (g_textures_loaded) {
        if (!g_pg.texture_lookup_done) {
            g_pg.texture_lookup_done = 1;
            fprintf(stderr, "[PGRAPH-D3D11] Texture lookup init (global_txd has %d textures)\n",
                    g_global_txd.count);
            for (int ti = 0; ti < g_global_txd.count; ti++) {
                fprintf(stderr, "    [%3d] %-24s %3ux%-3u fmt=0x%X\n",
                        ti, g_global_txd.entries[ti].name,
                        g_global_txd.entries[ti].width,
                        g_global_txd.entries[ti].height,
                        g_global_txd.entries[ti].format);
            }
        }

        IDirect3DTexture8 *tex = NULL;
        uint32_t vram_off = g_pg.tex[0].offset;
        switch (vram_off) {
            case 0x03C1ED00: tex = txd_find(&g_global_txd, "B3Logo"); break;
            case 0x03C24700: tex = txd_find(&g_global_txd, "bg"); break;
            case 0x03C24B80: tex = txd_find(&g_global_txd, "big_curve"); break;
            case 0x03C7BE00: tex = txd_find(&g_global_txd, "Buttons"); break;
            case 0x03C95700: tex = txd_find(&g_global_txd, "dpad"); break;
            case 0x03C95980: tex = txd_find(&g_global_txd, "FE"); break;
            case 0x03CA1A80: tex = txd_find(&g_global_txd, "small_curve"); break;
            case 0x03D57000: tex = txd_find(&g_global_txd, "box_curve"); break;
            case 0x03CB9200: tex = txd_find(&g_global_txd, "grid"); break;
            case 0x02EC0400:
                dev->lpVtbl->EndScene(dev);
                g_pg.inline_count = 0;
                return;
            case 0x021C4100:
                if (!g_pg.font_atlas) {
                    g_pg.font_atlas = create_dxt5_texture(dev,
                        FONT_ATLAS_WIDTH, FONT_ATLAS_HEIGHT,
                        font_atlas_dxt5, FONT_ATLAS_SIZE);
                }
                tex = g_pg.font_atlas;
                break;
            case 0: tex = NULL; break;
            default: tex = NULL; break;
        }

        if (tex) {
            dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*COLOROP*/, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 3 /*COLORARG2*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 6 /*ALPHAARG2*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 13 /*ADDRESSU*/, 3 /*CLAMP*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 14 /*ADDRESSV*/, 3 /*CLAMP*/);
        } else {
            /* No texture — use vertex color only */
            dev->lpVtbl->SetTexture(dev, 0, NULL);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*COLOROP*/, 2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/, 2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, 0 /*DIFFUSE*/);
        }
    } else {
        /* No texture for this draw -- take the vertex colour, exactly as the
         * sibling branches above do.
         *
         * These four calls are not optional. dev_SetTexture(stage, NULL) sets
         * COLOROP to D3DTOP_DISABLE, and the pixel shader's stage loop bails on
         * the first stage whose COLOROP is <= D3DTOP_DISABLE -- so leaving them
         * out made every draw on this path return input.diffuse untouched.
         * Measured before the fix: the frame was pure white with blending
         * forced off, and invisible with it on. */
        dev->lpVtbl->SetTexture(dev, 0, NULL);
        dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*COLOROP*/,   2 /*SELECTARG1*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, 0 /*DIFFUSE*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/,   2 /*SELECTARG1*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, 0 /*DIFFUSE*/);
    }
#else
    /* Bind the texture the title actually set, exactly as apply_draw_state
     * does.  This used to be an unconditional SetTexture(0, NULL) with a
     * "use vertex color only" comment, which meant the translator never bound
     * a texture at all: every draw came out as flat vertex colour, so nothing
     * textured -- no UI art, no glyphs, no video -- could ever appear no matter
     * what the rest of the pipeline did.  The real texture lookup was sitting
     * in the branch above, behind GAME_HAS_FONT_ATLAS, which is defined
     * nowhere. */
    {
        IDirect3DTexture8 *t = g_pg.tex[0].enabled ? tex_upload(dev, 0) : NULL;
        if (t) {
            dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)t);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*COLOROP*/,   4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 3 /*COLORARG2*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/,   4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 6 /*ALPHAARG2*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 13 /*ADDRESSU*/, 3 /*CLAMP*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 14 /*ADDRESSV*/, 3 /*CLAMP*/);
        } else {
            dev->lpVtbl->SetTexture(dev, 0, NULL);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*COLOROP*/,   2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/,   2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, 0 /*DIFFUSE*/);
        }
    }
#endif

    /* Begin scene if needed */
    dev->lpVtbl->BeginScene(dev);

    /* Draw */
    hud_scale_out(out, out_vert_count);
    dev->lpVtbl->DrawPrimitiveUP(dev, (D3DPRIMITIVETYPE)g_pg.d3d_prim_type,
                                  prim_count, out, sizeof(OutputVertex));

    g_pg.stats.draw_calls++;
    g_pg.stats.vertices_submitted += num_verts;
    g_pg.draws_since_present++;

    if (g_pg.stats.draw_calls <= 5 || (g_pg.stats.draw_calls % 1000) == 0) {
        fprintf(stderr, "[PGRAPH-D3D11] Draw #%u: %u verts, prim=%d, prims=%u\n",
                g_pg.stats.draw_calls, num_verts, g_pg.d3d_prim_type, prim_count);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Method Handler
 * ══════════════════════════════════════════════════════════════════════ */

int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    if (!g_pg.initialized)
        return 0;
    {   /* XBOX_NV2A_SUBCHLOG=N (diagnostic): the first N methods sent to a
         * subchannel other than 0 -- 2D objects (surface copies, blits) that
         * this 3D translator would otherwise read as NV097 methods. */
        static int left = -1;
        if (left < 0) { const char *e = getenv("XBOX_NV2A_SUBCHLOG"); left = e ? atoi(e) : 0; }
        if (left > 0 && subchannel != 0) {
            left--;
            fprintf(stderr, "[SUBCH] %d method %04X param %08X present %u\n",
                    subchannel, method, param, d3d8_PresentSeq());
        }
    }

    /* Vertex stream binding. These are strided ranges rather than single
     * methods, so they are matched ahead of the switch. Both were previously
     * swallowed by the catch-all ignore list at the bottom of this function,
     * which is why no stream ever reached the draw path. */
    if (method >= NV2A_VA_OFFSET_BASE &&
        method <  NV2A_VA_OFFSET_BASE + NV2A_VA_SLOTS * 4) {
        {
            uint32_t slot = (method - NV2A_VA_OFFSET_BASE) >> 2;
            static int logged = 0;
            g_pg.vattr[slot].offset = param;
            if (logged < 8) {
                logged++;
                fprintf(stderr, "[PGRAPH-D3D11] bind attr %u offset=0x%08X "
                        "(format currently 0x%08X)\n", slot, param, g_pg.vattr[slot].format);
                fflush(stderr);
            }
        }
        g_pg.stats.methods_handled++;
        return 1;
    }
    if (method >= NV2A_VA_FORMAT_BASE &&
        method <  NV2A_VA_FORMAT_BASE + NV2A_VA_SLOTS * 4) {
        {
            uint32_t slot = (method - NV2A_VA_FORMAT_BASE) >> 2;
            static int logged = 0;
            g_pg.vattr[slot].format = param;
            if (logged < 8) {
                logged++;
                fprintf(stderr, "[PGRAPH-D3D11] attr %u format=0x%08X "
                        "(type %u size %u stride %u)\n", slot, param,
                        param & 0xF, (param >> 4) & 0xF, (param >> 8) & 0xFFFFFF);
                fflush(stderr);
            }
        }
        g_pg.stats.methods_handled++;
        return 1;
    }

    g_pg.stats.methods_handled++;

    switch (method) {

    /* ── Draw Begin/End ── */
    case NV097_SET_BEGIN_END:
        if (param == 0) {
            /* END: submit accumulated vertices */
            if (g_pg.idx_count) {
                if (vsh_active())
                    draw_program(g_pg.idx, 0, g_pg.idx_count);
                else
                    g_vs.raw_indexed++;   /* fixed-function indexed: no transform yet */
                g_pg.idx_count = 0;
            }
            if (g_pg.in_draw) {
                submit_draw();
                g_pg.in_draw = 0;
            }
        } else {
            /* BEGIN: start new draw */
            g_pg.in_draw = 1;
            g_pg.draw_mode = param;
            g_pg.d3d_prim_type = nv2a_draw_mode_to_d3d(param);
            g_pg.inline_count = 0;
            g_pg.idx_count = 0;
        }
        return 1;

    /* ── Vertex Array Draw ── */
    case NV097_DRAW_ARRAYS: {
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        if (vsh_active()) {
            /* Collected and drawn at END, so a strip split over several
             * DRAW_ARRAYS stays one strip. */
            uint32_t k;
            for (k = 0; k < count; k++)
                idx_push(start + k);
        } else {
            draw_arrays(start, count);
        }
        return 1;
    }

    /* ── Indexed vertices: two 16-bit indices per word, low half first ── */
    case NV097_ARRAY_ELEMENT16:
        idx_push(param & 0xFFFFu);
        idx_push(param >> 16);
        return 1;
    case NV097_ARRAY_ELEMENT32:
        idx_push(param);
        return 1;

    /* ── Transform state ── */
    case NV097_SET_TRANSFORM_EXECUTION_MODE:
        g_pg.xf_mode = param & NV097_SET_TRANSFORM_EXECUTION_MODE_MODE;
        return 1;
    case NV097_SET_TRANSFORM_PROGRAM_LOAD:
        g_pg.prog_load = param;
        return 1;
    case NV097_SET_TRANSFORM_PROGRAM_START:
        if (g_pg.prog_start != param) {
            g_pg.prog_start = param;
            g_pg.prog_dirty = 1;
        }
        return 1;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD:
        g_pg.const_load = param;
        return 1;
    case NV097_SET_TRANSFORM_PROGRAM ... NV097_SET_TRANSFORM_PROGRAM + 0x7C: {
        /* One component per method slot; the load pointer advances after
         * the fourth. */
        uint32_t slot = (method - NV097_SET_TRANSFORM_PROGRAM) / 4;
        if (g_pg.prog_load < VSHCPU_SLOTS) {
            g_pg.prog[g_pg.prog_load][slot % 4] = param;
            g_pg.prog_dirty = 1;
            if (slot % 4 == 3)
                g_pg.prog_load++;
        }
        return 1;
    }
    case NV097_SET_TRANSFORM_CONSTANT ... NV097_SET_TRANSFORM_CONSTANT + 0x7C: {
        uint32_t slot = (method - NV097_SET_TRANSFORM_CONSTANT) / 4;
        if (g_pg.const_load < VSHCPU_CONSTANTS) {
            g_pg.vconst[g_pg.const_load][slot % 4] = u2f(param);
            if (slot % 4 == 3)
                g_pg.const_load++;
        }
        return 1;
    }
    case NV097_SET_CLIP_MIN:
        g_pg.clip_min = u2f(param);
        return 1;
    case NV097_SET_CLIP_MAX:
        g_pg.clip_max = u2f(param);
        return 1;

    /* ── Inline Vertex Data ── */
    case NV097_INLINE_ARRAY:
        if (g_pg.in_draw && g_pg.inline_count < MAX_INLINE_VERTS * INLINE_VERT_DWORDS) {
            g_pg.inline_data[g_pg.inline_count++] = param;
        }
        return 1;

    /* ── Constant ("inline") vertex attributes ──
     *
     * These feed any attribute whose array is disabled.  Dropping them meant
     * every unbound attribute fell back to a hardcoded guess: diffuse became
     * white, so the title's backdrop quad covered the screen in white, and
     * texcoords became 0,0.  Semantics follow the NV2A model in
     * reference/cxbx-reloaded EmuNV2A_PGRAPH.cpp. */
    case NV097_SET_VERTEX_DATA4UB ... NV097_SET_VERTEX_DATA4UB + 0x3C: {
        uint32_t a = (method - NV097_SET_VERTEX_DATA4UB) / 4;
        va_const_init();
        g_pg.vattr_const[a][0] = (float)( param        & 0xFF) / 255.0f;
        g_pg.vattr_const[a][1] = (float)((param >>  8) & 0xFF) / 255.0f;
        g_pg.vattr_const[a][2] = (float)((param >> 16) & 0xFF) / 255.0f;
        g_pg.vattr_const[a][3] = (float)((param >> 24) & 0xFF) / 255.0f;
        return 1;
    }

    case NV097_SET_VERTEX_DATA4F_M ... NV097_SET_VERTEX_DATA4F_M + 0xFC: {
        uint32_t i = (method - NV097_SET_VERTEX_DATA4F_M) / 4;
        va_const_init();
        g_pg.vattr_const[i / 4][i % 4] = u2f(param);
        return 1;
    }

    case NV097_SET_VERTEX_DATA2F_M ... NV097_SET_VERTEX_DATA2F_M + 0x7C: {
        uint32_t i = (method - NV097_SET_VERTEX_DATA2F_M) / 4;
        uint32_t a = i / 2;
        va_const_init();
        g_pg.vattr_const[a][i % 2] = u2f(param);
        g_pg.vattr_const[a][2] = 0.0f;
        g_pg.vattr_const[a][3] = 1.0f;
        return 1;
    }

    case NV097_SET_VERTEX_DATA4S_M ... NV097_SET_VERTEX_DATA4S_M + 0x7C: {
        uint32_t i = (method - NV097_SET_VERTEX_DATA4S_M) / 4;
        uint32_t a = i / 2, part = i % 2;
        va_const_init();
        g_pg.vattr_const[a][part * 2 + 0] =
            ((float)(int16_t)( param        & 0xFFFF) * 2.0f + 1.0f) / 65535.0f;
        g_pg.vattr_const[a][part * 2 + 1] =
            ((float)(int16_t)((param >> 16) & 0xFFFF) * 2.0f + 1.0f) / 65535.0f;
        return 1;
    }

    case NV097_SET_VERTEX_DATA2S ... NV097_SET_VERTEX_DATA2S + 0x3C: {
        uint32_t a = (method - NV097_SET_VERTEX_DATA2S) / 4;
        va_const_init();
        g_pg.vattr_const[a][0] = (float)(int16_t)( param        & 0xFFFF);
        g_pg.vattr_const[a][1] = (float)(int16_t)((param >> 16) & 0xFFFF);
        g_pg.vattr_const[a][2] = 0.0f;
        g_pg.vattr_const[a][3] = 1.0f;
        return 1;
    }

    /* ── Clear ── */
    case NV097_SET_COLOR_CLEAR_VALUE:
        g_pg.clear_color = param;
        return 1;

    case NV097_SET_CLEAR_RECT_HORIZONTAL:
        g_pg.clear_rect_h = param;
        return 1;

    case NV097_SET_CLEAR_RECT_VERTICAL:
        g_pg.clear_rect_v = param;
        return 1;

    case NV097_SET_STENCIL_TEST_ENABLE: g_pg.stencil_test = param != 0; return 1;
    case NV097_SET_STENCIL_MASK:        g_pg.stencil_mask_write = param & 0xFF; return 1;
    case NV097_SET_STENCIL_FUNC:        g_pg.stencil_func = param; return 1;
    case NV097_SET_STENCIL_FUNC_REF:    g_pg.stencil_ref = param & 0xFF; return 1;
    case NV097_SET_STENCIL_FUNC_MASK:   g_pg.stencil_mask_read = param & 0xFF; return 1;
    case NV097_SET_STENCIL_OP_FAIL:     g_pg.stencil_op[0] = param; return 1;
    case NV097_SET_STENCIL_OP_ZFAIL:    g_pg.stencil_op[1] = param; return 1;
    case NV097_SET_STENCIL_OP_ZPASS:    g_pg.stencil_op[2] = param; return 1;
    case NV097_SET_CULL_FACE:           g_pg.cull_face = param; return 1;
    case NV097_SET_FRONT_FACE:          g_pg.front_face = param; return 1;
    case NV097_SET_ZSTENCIL_CLEAR_VALUE: {
        static int left = -1;
        if (left < 0) { const char *e = getenv("XBOX_NV2A_SURFLOG"); left = e ? atoi(e) : 0; }
        if (left > 0 && param != g_pg.zstencil_clear) {
            left--;
            fprintf(stderr, "[SURF] zstencil clear = %08X (present %u)\n", param, d3d8_PresentSeq());
        }
        g_pg.zstencil_clear = param;
        return 1;
    }
    case NV097_SET_SPECULAR_ENABLE:
    case NV097_SET_LIGHT_CONTROL: {
        /* XBOX_NV2A_SURFLOG also logs changes of these two. */
        static int left = -1;
        uint32_t old = method == NV097_SET_SPECULAR_ENABLE ? (uint32_t)g_pg.specular_enable : g_pg.light_control;
        if (method == NV097_SET_SPECULAR_ENABLE) g_pg.specular_enable = param != 0;
        else g_pg.light_control = param;
        if (left < 0) { const char *e = getenv("XBOX_NV2A_SURFLOG"); left = e ? atoi(e) : 0; }
        if (left > 0 && old != (method == NV097_SET_SPECULAR_ENABLE ? (uint32_t)(param != 0) : param)) {
            left--;
            fprintf(stderr, "[SURF] %s = %08X (present %u)\n",
                    method == NV097_SET_SPECULAR_ENABLE ? "specular enable" : "light control", param, d3d8_PresentSeq());
        }
        return 1;
    }
    case NV097_SET_POINT_PARAMS_ENABLE:
        g_pg.point_params_enable = param != 0;
        return 1;
    case NV097_SET_POINT_SMOOTH_ENABLE:
        g_pg.point_smooth = param != 0;
        return 1;
    case NV097_SET_POINT_SIZE:
        if (param <= 0x1FF) g_pg.point_size = param;
        return 1;
    case NV097_SET_POINT_PARAMS + 0x00: case NV097_SET_POINT_PARAMS + 0x04:
    case NV097_SET_POINT_PARAMS + 0x08: case NV097_SET_POINT_PARAMS + 0x0C:
    case NV097_SET_POINT_PARAMS + 0x10: case NV097_SET_POINT_PARAMS + 0x14:
    case NV097_SET_POINT_PARAMS + 0x18: case NV097_SET_POINT_PARAMS + 0x1C:
        memcpy(&g_pg.point_params[(method - NV097_SET_POINT_PARAMS) / 4], &param, 4);
        return 1;

    case NV097_SET_WINDOW_CLIP_TYPE:
        g_pg.wclip_type = param & 1;
        wclip_update();
        return 1;

    case NV097_SET_WINDOW_CLIP_HORIZONTAL ... NV097_SET_WINDOW_CLIP_HORIZONTAL + 0x1C:
    case NV097_SET_WINDOW_CLIP_VERTICAL ... NV097_SET_WINDOW_CLIP_VERTICAL + 0x1C: {
        /* As xemu: writing slot k fills slots k..7. */
        int vert = method >= NV097_SET_WINDOW_CLIP_VERTICAL;
        uint32_t k = (method - (vert ? NV097_SET_WINDOW_CLIP_VERTICAL
                                     : NV097_SET_WINDOW_CLIP_HORIZONTAL)) / 4;
        uint32_t old = vert ? g_pg.wclip_y[k] : g_pg.wclip_x[k];
        for (; k < 8; k++) {
            if (vert) g_pg.wclip_y[k] = param;
            else      g_pg.wclip_x[k] = param;
        }
        g_pg.wclip_seen = 1;
        wclip_update();
        {
            static int left = -1;
            if (left < 0) { const char *e = getenv("XBOX_NV2A_SURFLOG"); left = e ? atoi(e) : 0; }
            if (left > 0 && param != old) {
                left--;
                fprintf(stderr, "[SURF] window clip %s[%u] = %u..%u (type %u)\n", vert ? "y" : "x",
                        (method - (vert ? NV097_SET_WINDOW_CLIP_VERTICAL : NV097_SET_WINDOW_CLIP_HORIZONTAL)) / 4,
                        param & 0xFFF, (param >> 16) & 0xFFF, g_pg.wclip_type);
            }
        }
        return 1;
    }

    case NV097_CLEAR_REPORT_VALUE:          /* zpass counter and reports */
        if (!occ_enabled()) return 0;
        if (param == NV097_CLEAR_REPORT_VALUE_TYPE_ZPASS_PIXEL_CNT) occ_clear();
        return 1;
    case NV097_SET_ZPASS_PIXEL_COUNT_ENABLE:
        if (!occ_enabled()) return 0;
        occ_enable(param != 0);
        return 1;
    case NV097_GET_REPORT:
        if (!occ_enabled()) return 0;
        occ_get_report(param);
        return 1;

    case NV097_SET_SURFACE_COLOR_OFFSET:
        /* The title double-buffers by pointing the render surface at the other
         * buffer each frame, which is the only frame boundary it gives us: it
         * emits no NV097_FLIP_STALL and writes no CRTC register we can see.
         * A change here means the frame just finished is complete, so it is
         * the moment to present. Presenting on a 16 ms timer instead landed
         * between the frame's clear and its draw, so what reached the screen
         * was a freshly cleared buffer -- black -- every time. */
        {   /* XBOX_FLIP_LOG=1 (diagnostic): every surface change, timestamped */
            static int flog = -1;
            if (flog < 0) { const char *e = getenv("XBOX_FLIP_LOG"); flog = e && e[0] == '1'; }
            if (flog && param != g_pg.surface_color_offset) {
                LARGE_INTEGER q, f;
                QueryPerformanceCounter(&q);
                QueryPerformanceFrequency(&f);
                fprintf(stderr, "[FLIP] t=%.1f %08X -> %08X draws %u\n",
                        (double)q.QuadPart * 1000.0 / (double)f.QuadPart,
                        g_pg.surface_color_offset, param, g_pg.draws_since_present);
            }
        }
        peek_after("frame (flip)");
        if (g_pg.surface_offset_seen && param != g_pg.surface_color_offset) {
            g_pg.frame_complete = 1;
            g_pg.prev_surface_offset = g_pg.surface_color_offset;
            /* A surface flip is the title's real frame boundary --
             * present here, and only here, once flips are seen. Presenting on
             * every clear (and on the pump's 16 ms fallback) showed each
             * render pass as its own frame: the 3D menus clear depth between
             * the background, the set and the UI, so character select
             * alternated between a lone ice cave and a half-drawn platform. */
            g_pg.flip_mode = 1;
            g_pg.last_flip_ms = GetTickCount64();
            if (g_pg.draws_since_present > 0) {
                g_pg.draws_since_present = 0;
                g_pg.frame_no++;
                pass_phase_image_done();
                box_image_done();
                d3d8_PresentFrame();
            }
        }
        {
            /* XBOX_NV2A_SURFLOG=N (diagnostic): the first N surface changes,
             * with the draws issued to the previous surface. */
            static int left = -1;
            if (left < 0) { const char *e = getenv("XBOX_NV2A_SURFLOG"); left = e ? atoi(e) : 0; }
            if (left > 0 && param != g_pg.surface_color_offset) {
                left--;
                fprintf(stderr, "[SURF] color %08X -> %08X after %u draws (clip h %08X v %08X) present %u\n",
                        g_pg.surface_color_offset, param, g_pg.draws_since_present,
                        g_pg.surface_clip_h, g_pg.surface_clip_v, d3d8_PresentSeq());
            }
        }
        g_pg.surface_color_offset = param;
        g_pg.surface_offset_seen = 1;
        return 1;

    case NV097_CLEAR_SURFACE:
    {
        IDirect3DDevice8 *dev = xbox_GetD3DDevice();

        /* A clear starts a new frame, so whatever was drawn before it is a
         * finished frame -- present it now, before wiping the buffer.
         *
         * This has to happen here rather than in the pump's tick, because the
         * tick presents whatever state the surface is in when the batch runs
         * out, and that landed between a frame's clear and its draw every
         * time: a cleared black buffer was what reached the screen while the
         * title's geometry was being submitted correctly. The surface-offset
         * flip would be the more faithful signal, but this title does not
         * change it. */
        {
            /* Per-frame trace.  The screen is a single flat colour in every
             * captured frame, so the question is whether that colour is the
             * clear value, and whether the draws that are issued reach it.
             * Counting draws alone cannot answer either. */
            static int trace = -1;
            static unsigned fno = 0;
            if (trace < 0) {
                const char *e = getenv("XBOX_D3D_FRAMELOG");
                trace = (e && e[0] == '1') ? 1 : 0;
            }
            if (trace) {
                fprintf(stderr,
                        "[frame %5u] clear=%08X flags=%02X rect %08X %08X draws=%u dc=%u vtx=%u\n",
                        fno++, g_pg.clear_color, param & 0xFF,
                        g_pg.clear_rect_h, g_pg.clear_rect_v,
                        g_pg.draws_since_present, g_pg.stats.draw_calls,
                        g_pg.stats.vertices_submitted);
                fflush(stderr);
            }
        }
        {
            static unsigned rc = 0;
            if (++rc == 400) pgraph_d3d11_report_ignored();
        }
        if (g_pg.draws_since_present > 0 && !pgraph_d3d11_flipping()) {
            g_pg.draws_since_present = 0;
            g_pg.frame_no++;
            pass_phase_image_done();
            box_image_done();
            d3d8_PresentFrame();
        }
        if (dev) {
            uint32_t flags = 0;
            if (param & 0xF0) flags |= 1;  /* D3DCLEAR_TARGET */
            if (param & 0x01) flags |= 2;  /* D3DCLEAR_ZBUFFER */
            if (param & 0x02) flags |= 4;  /* D3DCLEAR_STENCIL */
            /* Only the clear rectangle (NV_PGRAPH_CLEARRECTX/Y: min in bits
             * 0..11, max in 16..27, inclusive) -- see d3d8_ClearRect. */
            d3d8_ClearRect(flags, g_pg.clear_color, 1.0f, 0,
                           g_pg.clear_rect_h & 0xFFF, g_pg.clear_rect_v & 0xFFF,
                           (g_pg.clear_rect_h >> 16) & 0xFFF, (g_pg.clear_rect_v >> 16) & 0xFFF);
        }
        peek_after("clear");
        g_pg.stats.clears++;
        return 1;
    }

    /* ── Render State ── */
    case NV097_SET_DEPTH_TEST_ENABLE:
        g_pg.depth_test = param ? 1 : 0;
        return 1;

    case NV097_SET_BLEND_ENABLE:
        g_pg.blend_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_BLEND_FUNC_SFACTOR:
        g_pg.blend_sfactor = param;
        return 1;

    case NV097_SET_BLEND_FUNC_DFACTOR:
        g_pg.blend_dfactor = param;
        return 1;

    case NV097_SET_CULL_FACE_ENABLE:
        g_pg.cull_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_ALPHA_TEST_ENABLE:
        g_pg.alpha_test = param ? 1 : 0;
        return 1;

    case NV097_SET_DEPTH_FUNC:
        g_pg.depth_func = param;
        return 1;

    case NV097_SET_DEPTH_MASK:
        g_pg.depth_mask = param ? 1 : 0;
        return 1;

    case NV097_SET_ALPHA_FUNC:
        g_pg.alpha_func = param;
        return 1;

    case NV097_SET_ALPHA_REF:
        g_pg.alpha_ref = param;
        return 1;

    case NV097_SET_FOG_MODE:
        g_pg.fog_mode = param;
        return 1;
    case NV097_SET_FOG_GEN_MODE:
        g_pg.fog_gen = param;
        return 1;
    case NV097_SET_FOG_ENABLE:
        g_pg.fog_enable = param ? 1 : 0;
        return 1;
    case NV097_SET_FOG_COLOR:
        g_pg.fog_color = param;
        return 1;
    case NV097_SET_FOG_PARAMS: case NV097_SET_FOG_PARAMS + 4: case NV097_SET_FOG_PARAMS + 8:
        g_pg.fog_param[(method - NV097_SET_FOG_PARAMS) / 4] = u2f(param);
        return 1;
    case NV097_SET_COMBINER_ALPHA_ICW ... NV097_SET_COMBINER_ALPHA_ICW + 0x1C:
        g_pg.comb_alpha_icw[(method - NV097_SET_COMBINER_ALPHA_ICW) / 4] = param;
        return 1;
    case NV097_SET_COMBINER_ALPHA_OCW ... NV097_SET_COMBINER_ALPHA_OCW + 0x1C:
        g_pg.comb_alpha_ocw[(method - NV097_SET_COMBINER_ALPHA_OCW) / 4] = param;
        return 1;
    case NV097_SET_COMBINER_COLOR_ICW ... NV097_SET_COMBINER_COLOR_ICW + 0x1C:
        g_pg.comb_color_icw[(method - NV097_SET_COMBINER_COLOR_ICW) / 4] = param;
        return 1;
    case NV097_SET_COMBINER_COLOR_OCW ... NV097_SET_COMBINER_COLOR_OCW + 0x1C:
        g_pg.comb_color_ocw[(method - NV097_SET_COMBINER_COLOR_OCW) / 4] = param;
        return 1;
    case NV097_SET_COMBINER_FACTOR0 ... NV097_SET_COMBINER_FACTOR0 + 0x1C:
        g_pg.comb_factor0[(method - NV097_SET_COMBINER_FACTOR0) / 4] = param;
        return 1;
    case NV097_SET_COMBINER_FACTOR1 ... NV097_SET_COMBINER_FACTOR1 + 0x1C:
        g_pg.comb_factor1[(method - NV097_SET_COMBINER_FACTOR1) / 4] = param;
        return 1;
    case NV097_SET_CONTROL0:
        /* Z_FORMAT (bit 12): float depth; Z_PERSPECTIVE_ENABLE (bit 16):
         * the depth test uses w (a w-buffer) instead of z. */
        {
            static int told = 0;
            if ((!g_pg.control0_seen || g_pg.control0 != param) && told < 16) {
                told++;
                fprintf(stderr, "[PGRAPH-D3D11] CONTROL0 %08X (z %s, %s) at present %u%c", param,
                        (param >> 12) & 1 ? "float" : "fixed",
                        (param >> 16) & 1 ? "w-buffer" : "z-buffer", d3d8_PresentSeq(), 10);
            }
        }
        g_pg.control0 = param;
        g_pg.control0_seen = 1;
        return 1;
    case NV097_SET_ZMIN_MAX_CONTROL:
        if (!g_pg.zmin_max_seen || g_pg.zmin_max_control != param)
            fprintf(stderr, "[PGRAPH-D3D11] ZMIN_MAX_CONTROL %08X (z %s)%c", param,
                    ((param >> 4) & 0xF) ? "clamp" : "cull", 10);
        g_pg.zmin_max_control = param;
        g_pg.zmin_max_seen = 1;
        return 1;
    case NV097_SET_COMBINER_CONTROL:
        g_pg.comb_control = param;
        return 1;
    case NV097_SET_SHADER_STAGE_PROGRAM:
        g_pg.shader_stage_program = param;
        return 1;
    case NV097_SET_DOT_RGBMAPPING:
        g_pg.dot_rgbmapping = param;
        return 1;
    case NV097_SET_SHADER_OTHER_STAGE_INPUT:
        g_pg.shader_other_stage_input = param;
        return 1;
    case NV097_SET_SPECULAR_FOG_FACTOR: case NV097_SET_SPECULAR_FOG_FACTOR + 4:
        g_pg.specular_fog_factor[(method - NV097_SET_SPECULAR_FOG_FACTOR) / 4] = param;
        return 1;
    case NV097_SET_COMBINER_SPECULAR_FOG_CW0:
        g_pg.spec_fog_cw0 = param;
        return 1;
    case NV097_SET_COMBINER_SPECULAR_FOG_CW1:
        g_pg.spec_fog_cw1 = param;
        return 1;

    case NV097_SET_COLOR_MASK:
        g_pg.color_mask = param;
        return 1;

    case NV097_SET_SHADE_MODE:
        /* 1=flat, 2=gouraud — we always use gouraud */
        return 1;

    /* ── Viewport ── */
    case NV097_SET_VIEWPORT_OFFSET:
    case NV097_SET_VIEWPORT_OFFSET + 4:
    case NV097_SET_VIEWPORT_OFFSET + 8:
    case NV097_SET_VIEWPORT_OFFSET + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_OFFSET) / 4;
        g_pg.vp_offset[idx] = u2f(param);
        /* The hardware keeps the viewport in transform-constant memory
         * (XFCTX_VPOFF = c[0x3B], D3D's c[-37]); the screen-space tail the
         * Xbox D3D runtime appends to every vertex program reads it there. */
        g_pg.vconst[0x3B][idx] = u2f(param);
        return 1;
    }

    case NV097_SET_VIEWPORT_SCALE:
    case NV097_SET_VIEWPORT_SCALE + 4:
    case NV097_SET_VIEWPORT_SCALE + 8:
    case NV097_SET_VIEWPORT_SCALE + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_SCALE) / 4;
        g_pg.vp_scale[idx] = u2f(param);
        g_pg.vconst[0x3A][idx] = u2f(param);   /* XFCTX_VPSCL, D3D's c[-38] */
        return 1;
    }

    /* Fixed-function matrices also live in transform-constant memory, at the
     * XFCTX slots xemu's nv2a_regs.h lists: composite c[0..3], projection
     * c[4..7], model-view n at c[8 + 8n], its inverse at c[12 + 8n]. */
    case NV097_SET_COMPOSITE_MATRIX ... NV097_SET_COMPOSITE_MATRIX + 0x3C: {
        uint32_t slot = (method - NV097_SET_COMPOSITE_MATRIX) / 4;
        g_pg.vconst[0x00 + slot / 4][slot % 4] = u2f(param);
        return 1;
    }
    case NV097_SET_PROJECTION_MATRIX ... NV097_SET_PROJECTION_MATRIX + 0x3C: {
        uint32_t slot = (method - NV097_SET_PROJECTION_MATRIX) / 4;
        g_pg.vconst[0x04 + slot / 4][slot % 4] = u2f(param);
        return 1;
    }
    case NV097_SET_MODEL_VIEW_MATRIX ... NV097_SET_MODEL_VIEW_MATRIX + 0xFC: {
        uint32_t slot = (method - NV097_SET_MODEL_VIEW_MATRIX) / 4;
        g_pg.vconst[0x08 + (slot / 16) * 8 + (slot % 16) / 4][slot % 4] = u2f(param);
        return 1;
    }
    case NV097_SET_INVERSE_MODEL_VIEW_MATRIX ... NV097_SET_INVERSE_MODEL_VIEW_MATRIX + 0xFC: {
        uint32_t slot = (method - NV097_SET_INVERSE_MODEL_VIEW_MATRIX) / 4;
        g_pg.vconst[0x0C + (slot / 16) * 8 + (slot % 16) / 4][slot % 4] = u2f(param);
        return 1;
    }

    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        g_pg.surface_clip_h = param;
        return 1;

    case NV097_SET_SURFACE_CLIP_VERTICAL:
        g_pg.surface_clip_v = param;
        return 1;

    /* ── Texture state tracking (4 stages, 0x40 stride) ── */
    case NV097_SET_TEXTURE_OFFSET:
    case NV097_SET_TEXTURE_OFFSET + 0x40:
    case NV097_SET_TEXTURE_OFFSET + 0x80:
    case NV097_SET_TEXTURE_OFFSET + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_OFFSET) / 0x40;
        g_pg.tex[stage].offset = param;
        return 1;
    }
    case NV097_SET_TEXTURE_FORMAT:
    case NV097_SET_TEXTURE_FORMAT + 0x40:
    case NV097_SET_TEXTURE_FORMAT + 0x80:
    case NV097_SET_TEXTURE_FORMAT + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_FORMAT) / 0x40;
        g_pg.tex[stage].format = param;
        return 1;
    }
    case NV097_SET_TEXTURE_CONTROL1:
    case NV097_SET_TEXTURE_CONTROL1 + 0x40:
    case NV097_SET_TEXTURE_CONTROL1 + 0x80:
    case NV097_SET_TEXTURE_CONTROL1 + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_CONTROL1) / 0x40;
        g_pg.tex[stage].control1 = param;
        return 1;
    }
    case NV097_SET_TEXTURE_IMAGE_RECT:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0x40:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0x80:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_IMAGE_RECT) / 0x40;
        g_pg.tex[stage].image_rect = param;
        return 1;
    }
    case NV097_SET_TEXTURE_FILTER:
    case NV097_SET_TEXTURE_FILTER + 0x40:
    case NV097_SET_TEXTURE_FILTER + 0x80:
    case NV097_SET_TEXTURE_FILTER + 0xC0:
        g_pg.tex[(method - NV097_SET_TEXTURE_FILTER) / 0x40].filter = param;
        return 1;
    case NV097_SET_TEXTURE_ADDRESS:
    case NV097_SET_TEXTURE_ADDRESS + 0x40:
    case NV097_SET_TEXTURE_ADDRESS + 0x80:
    case NV097_SET_TEXTURE_ADDRESS + 0xC0:
        g_pg.tex[(method - NV097_SET_TEXTURE_ADDRESS) / 0x40].address = param;
        return 1;
    case NV097_SET_TEXTURE_CONTROL0:
    case NV097_SET_TEXTURE_CONTROL0 + 0x40:
    case NV097_SET_TEXTURE_CONTROL0 + 0x80:
    case NV097_SET_TEXTURE_CONTROL0 + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_CONTROL0) / 0x40;
        g_pg.tex[stage].control0 = param;
        g_pg.tex[stage].enabled = (param >> 30) & 1;
        return 1;
    }

    /* SET_TEXTURE_SET_BUMP_ENV_MAT / _SCALE / _OFFSET: kept per
     * stage for BUMPENVMAP(_LUM); stage 0 has none on the hardware. */
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x40:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x44:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x48:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x4C:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x50:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x54:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x80:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x84:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x88:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x8C:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x90:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x94:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0xC0:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0xC4:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0xC8:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0xCC:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0xD0:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0xD4:
    {
        uint32_t r = method - NV097_SET_TEXTURE_SET_BUMP_ENV_MAT;
        g_pg.tex[r / 0x40].bump[(r % 0x40) / 4] = param;
        return 1;
    }

    /* Counted, never acted on (see nop_note). */
    case NV097_NO_OPERATION:
        nop_note(param);
        return 1;

    default:
        /* Check if it's in a known range we can safely ignore */
        if ((method >= 0x0B80 && method < 0x0C00) ||  /* Transform program */
            (method >= 0x0E00 && method < 0x1000) ||  /* Transform constants */
            (method >= 0x1680 && method < 0x1720) ||  /* Vertex attrib misc */
            (method >= 0x17A0 && method < 0x17C0) ||  /* Vertex array reserved */
            (method >= 0x1B00 && method < 0x1C00) ||  /* Texture registers */
            (method >= 0x1D60 && method < 0x1EA0) ||  /* Combiners */
            method == 0x0180 ||                        /* SET_OBJECT */
            method == 0x0394 ||                        /* TRANSFORM_EXECUTION_MODE */
            method == 0x0398 ||                        /* TRANSFORM_PROGRAM_CXT_WRITE_EN */
            method == 0x039C ||                        /* TRANSFORM_PROGRAM_LOAD */
            method == 0x01E0 ||                        /* SHADER_STAGE_PROGRAM */
            method == 0x0108 || method == 0x010C ||    /* FLIP_READ/WRITE */
            method == 0x0110 || method == 0x0114 ||    /* FLIP_MODULO/INCREMENT */
            method == 0x0118)                          /* FLIP_STALL */
        {
            return 1;  /* Silently handled (ignored but acknowledged) */
        }

        /* XBOX_NV2A_IGNLOG=1 histograms which methods are being dropped, so
         * the ones that matter can be picked out of the volume instead of
         * guessed at.  Printed at exit by pgraph_d3d11_report_ignored(). */
        if (method < 0x2000) {
            g_ignored_hist[method >> 2]++;
        }
        g_pg.stats.methods_ignored++;
        return 0;  /* Truly unhandled */
    }
}

void pgraph_d3d11_flush(void)
{
    if (g_pg.in_draw) {
        submit_draw();
        g_pg.in_draw = 0;
    }
    g_pg.stats.frames++;
}

void pgraph_d3d11_set_chyron_scroll(uint32_t pixels)
{
    g_pg.chyron_scroll_offset = (float)pixels;
}

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out)
{
    if (out) *out = g_pg.stats;
}

/* Draws translated so far, read from another thread (the freeze watchdog,
 * port crashreport.c): it only looks for a change. */
uint32_t pgraph_d3d11_draw_count(void)
{
    return *(volatile uint32_t *)&g_pg.stats.draw_calls;
}

/* Present the frame.
 *
 * SSX never emits NV097_FLIP_STALL or NV097_FLIP_INCREMENT_WRITE -- the
 * push-buffer histogram has neither, across 386 distinct methods. It flips the
 * way the hardware does: by moving the CRTC scanout base, i.e. a write to
 * NV_PCRTC_START. pcrtc_write() calls this whenever that base actually changes.
 *
 * Until now nothing called Present at all, so every draw the translator issued
 * accumulated in a back buffer that was never shown.
 */
void pgraph_d3d11_present(uint32_t crtc_start)
{
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();
    static uint32_t frames = 0;

    if (!dev)
        return;

    /* Close the scene the draw paths opened. BeginScene/EndScene are bookkeeping
     * in our shim, but keeping them paired means a stricter backend still works. */
    dev->lpVtbl->EndScene(dev);
    dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);

    frames++;
    if (frames <= 4 || (frames % 120) == 0) {
        fprintf(stderr, "[PGRAPH-D3D11] present #%u (CRTC start=0x%08X): "
                "%u draws, %u vertices so far\n",
                frames, crtc_start, g_pg.stats.draw_calls,
                g_pg.stats.vertices_submitted);
        fflush(stderr);
    }
}

/* Draw a known-good quad, for the XBOX_TEST_QUAD diagnostic.
 *
 * Everything about the title's own draws has checked out -- a full-screen
 * 640x480 screen-space quad, opaque white, depth off, cull none, solid fill,
 * a 640x480 viewport, a bound RTV and a compiled VS/PS pair -- and the back
 * buffer still reads 0x000000. This draws geometry that cannot be wrong
 * through the same device, so a black result indicts the device path and a
 * magenta one indicts the data feeding it.
 */
void pgraph_d3d11_test_quad(void)
{
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();
    OutputVertex q[4];
    int i;

    if (!dev)
        return;

    for (i = 0; i < 4; i++) {
        q[i].x     = (i & 1) ? 600.0f : 40.0f;
        q[i].y     = (i & 2) ? 440.0f : 40.0f;
        q[i].z     = 0.0f;
        q[i].rhw   = 1.0f;
        q[i].color = 0xFFFF00FFu;   /* opaque magenta */
        q[i].u     = 0.0f;
        q[i].v     = 0.0f;
    }

    apply_draw_state(dev);
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(OutputVertex));
    dev->lpVtbl->EndScene(dev);
}

/* True while the title is flipping render surfaces (one seen in the last
 * 250 ms): presents then belong to the flips, not to clears or timers. */
int pgraph_d3d11_flipping(void)
{
    return g_pg.flip_mode && GetTickCount64() - g_pg.last_flip_ms < 250
        && !d3d8_GuestFramebufferActive();   /* video frames need timed presents */
}

/* Draws translated since the last present (XBOX_FLIP_LOG timelines). */
unsigned pgraph_d3d11_draws_pending(void)
{
    return g_pg.draws_since_present;
}

int pgraph_d3d11_take_frame_complete(void)
{
    int v = g_pg.frame_complete;
    g_pg.frame_complete = 0;
    return v;
}

/* ---------------------------------------------------------------------------
 * Guest framebuffer presentation.
 *
 * The title's video player does not draw through the push buffer at all: it
 * calls the Xbox D3D8 that is statically linked into the XBE, locks the back
 * buffer, and writes decoded MPEG pixels straight into guest video memory
 * (measured: pBits 0xF3BA0000 / 0xF3CCC000 alternating, pitch 0x0A00, a 576-
 * wide region at x=0x20, y=0x10). On real hardware the GPU scans that memory
 * out. Here the host swap chain is a separate D3D11 surface, so those pixels
 * had nowhere to go and the intro video could never appear no matter how well
 * it decoded.
 *
 * This lifts the guest framebuffer into a texture and draws it as a full-screen
 * quad through the same device the translator uses -- the path proven to work,
 * since the loading-screen text renders through it.
 *
 * XBOX_GUEST_FB=<hex VA> enables it. The address is explicit for now because
 * nothing in the push buffer announces the scanout base: the NV2A PCRTC start
 * register reads back zero, so there is no register to key off yet.
 * ------------------------------------------------------------------------ */
void d3d8_SetGuestFramebuffer(const void *src, unsigned pitch,
                              unsigned w, unsigned h);
void d3d8_SetGuestFramebufferAlt(const void *alt);
uint32_t g_guest_fb_alt_va = 0;

void pgraph_d3d11_present_guest_fb(uint32_t va, uint32_t pitch,
                                   uint32_t w, uint32_t h)
{
    const uint8_t *src;

    if (!w || !h)
        return;
    src = va_ptr(va, pitch * h);
    if (!src) {
        static int told = 0;
        if (!told) {
            told = 1;
            fprintf(stderr, "  [FB] guest framebuffer 0x%08X (%ux%u pitch %u) "
                    "is outside guest RAM -- not presenting\n", va, w, h, pitch);
            fflush(stderr);
        }
        return;
    }
    d3d8_SetGuestFramebuffer(src, pitch, w, h);
    /* Register the other half of the flip too, when one was given. */
    if (g_guest_fb_alt_va) {
        const uint8_t *alt = va_ptr(g_guest_fb_alt_va, pitch * h);
        if (alt) d3d8_SetGuestFramebufferAlt(alt);
    }
}

/* Cumulative counters for XBOX_PERF (read at Present). */
void pgraph_d3d11_perf_counts(unsigned *draws, unsigned *methods, unsigned *verts)
{
    *draws = (unsigned)g_pg.stats.draw_calls;
    *methods = (unsigned)g_pg.stats.methods_handled;
    *verts = (unsigned)g_pg.stats.vertices_submitted;
}
