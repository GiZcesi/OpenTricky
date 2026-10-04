/**
 * nv2a_vsh_cpu -- run NV2A vertex-program microcode on the CPU.
 *
 * The D3D11 translator draws pre-transformed (XYZRHW) vertices. Titles that
 * run their geometry through a vertex program -- SSX Tricky's 3D frontend and
 * every course -- hand the GPU object-space vertices plus a program, so the
 * program has to run somewhere before the vertices mean anything on screen.
 * This runs it per vertex; the result is already in screen space because the
 * Xbox D3D runtime appends the viewport transform to every program it builds.
 */
#ifndef NV2A_VSH_CPU_H
#define NV2A_VSH_CPU_H

#include <stdint.h>

#define VSHCPU_SLOTS      136   /* program memory, 4 dwords per slot */
#define VSHCPU_CONSTANTS  192

/* Output register numbers, as the hardware's OUT_ADDRESS field names them. */
enum {
    VSHCPU_OUT_POS = 0, VSHCPU_OUT_D0 = 3, VSHCPU_OUT_D1 = 4, VSHCPU_OUT_FOG = 5,
    VSHCPU_OUT_PTS = 6, VSHCPU_OUT_B0 = 7, VSHCPU_OUT_B1 = 8, VSHCPU_OUT_T0 = 9,
    VSHCPU_OUT_REGS = 16
};

typedef struct {
    uint8_t mac, ilu;            /* opcodes */
    uint8_t cidx, vidx;          /* shared constant / input register index */
    struct { uint8_t mux, reg, neg, swz[4]; } src[3];   /* A, B, C */
    uint8_t mac_mask, ilu_mask;  /* temp-register write masks (8=x .. 1=w) */
    uint8_t out_r;               /* temp register written */
    uint8_t o_mask, orb, out_addr, out_mux;  /* output / constant write */
    uint8_t a0x, final;
} vshcpu_insn;

/* Decode from `start` through the instruction flagged FINAL. Returns the
 * number of instructions, or -1 if no FINAL appears before the end of
 * program memory (the program was never loaded, or `start` is wrong). */
int vshcpu_decode(const uint32_t prog[VSHCPU_SLOTS][4], int start,
                  vshcpu_insn *out);

/* Run a decoded program for one vertex. `c` is writable: the hardware lets a
 * program store to constant memory. */
void vshcpu_run(const vshcpu_insn *p, int n, const float in[16][4],
                float c[VSHCPU_CONSTANTS][4], float out[VSHCPU_OUT_REGS][4]);

/* One-line disassembly of an instruction, for logs. */
void vshcpu_format(const vshcpu_insn *i, char *buf, int len);

#endif
