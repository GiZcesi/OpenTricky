/*
 * AC'97 bus-master DMA.
 *
 * The register model in aci_mmio.c defines CR.RPBM (run bus master) and the
 * write-1-clear mask for SR's BCIS / LVBCI, but nothing ever raised them: there
 * was no DMA engine and no completion signalling. So the title brought the
 * codec up, started the bus master, and waited forever for a buffer-completion
 * that could not arrive -- which is why enabling DSOUND stopped the boot rather
 * than producing sound.
 *
 * This walks the buffer descriptor list the way the hardware does and keeps SR,
 * CIV, PICB and LVI consistent. It deliberately does *not* invoke the title's
 * ISR: delivering an interrupt means running guest code from the APU's host
 * thread, which has no guest stack, and that class of mistake has cost this
 * project real time before. Drivers poll SR as well as taking interrupts, so
 * the state machine alone is worth having first and is safe on its own.
 *
 * Layout, per channel, from the AC'97 spec:
 *
 *   0x00  BDBAR  buffer descriptor list base (32-bit, guest physical)
 *   0x04  CIV    current index value      (8-bit, wraps at 32)
 *   0x05  LVI    last valid index         (8-bit)
 *   0x06  SR     status                   (16-bit)
 *   0x08  PICB   position in current buffer, in samples (16-bit)
 *   0x0A  PIV    prefetched index value   (8-bit)
 *   0x0B  CR     control                  (8-bit)
 *
 * BDL entry, 8 bytes:
 *   +0  buffer address (guest physical)
 *   +4  bits 0..15 length in samples, bit 30 BUP, bit 31 IOC
 */

#include "aci_mmio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Register file and geometry live in aci_mmio.c. */
extern uint8_t *aci_reg_file(void);
extern uint32_t aci_nabm_off(void);
extern uint32_t aci_channel_stride(void);

/* Guest RAM base, set by mcpx_apu_init_standalone. */
extern uint8_t *g_apu_ram_ptr;

#define ACI_CHANNELS        3u      /* PI, PO, MC -- globals start after these */

#define SR_DCH              0x0001u /* DMA controller halted                   */
#define SR_CELV             0x0002u /* current equals last valid               */
#define SR_LVBCI            0x0004u /* last valid buffer completion interrupt  */
#define SR_BCIS             0x0008u /* buffer completion interrupt status      */

#define CR_RPBM             0x01u   /* run/pause bus master                    */
#define CR_LVBIE            0x04u   /* last valid buffer interrupt enable      */
#define CR_IOCE             0x10u   /* interrupt on completion enable          */

#define BDL_IOC             0x80000000u
#define BDL_LEN_MASK        0x0000FFFFu

static int g_aci_dma_trace = -1;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void     wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static uint32_t guest_rd32(uint32_t pa)
{
    /* Guest physical, folded the same way the recompiler folds the RAM
     * mirrors. A descriptor pointing outside RAM is a driver bug, not
     * something to fault the process over. */
    uint32_t off = pa & 0x03FFFFFFu;
    if (!g_apu_ram_ptr) return 0;
    return (uint32_t)g_apu_ram_ptr[off]
         | ((uint32_t)g_apu_ram_ptr[off + 1] << 8)
         | ((uint32_t)g_apu_ram_ptr[off + 2] << 16)
         | ((uint32_t)g_apu_ram_ptr[off + 3] << 24);
}

/* Advance every running channel by `samples`. Returns non-zero if any channel
 * raised an interrupt status bit, so a caller that can deliver one knows to. */
int aci_dma_advance(unsigned samples)
{
    uint8_t *regs = aci_reg_file();
    uint32_t base = aci_nabm_off();
    uint32_t stride = aci_channel_stride();
    unsigned ch;
    int raised = 0;

    if (!regs || !g_apu_ram_ptr || samples == 0) return 0;

    /* Inert unless audio is actually being brought up. This runs on the APU's
     * host thread while the MMIO trap writes the same register file from guest
     * threads, with no lock between them; that race is only worth carrying
     * while working on audio, and the default path must not pay for it.
     * This used to share codec-ready's switch. Codec-ready is now on by
     * default, so the engine needs its own: XBOX_ACI_DMA=1. Nothing is lost
     * while it is off, because no channel has ever set CR.RPBM -- until one
     * does there is nothing here to walk. */
    {
        static int on = -1;
        if (on < 0) {
            const char *e = getenv("XBOX_ACI_DMA");
            on = (e && e[0] == '1') ? 1 : 0;
        }
        if (!on) return 0;
    }

    if (g_aci_dma_trace < 0) {
        const char *e = getenv("XBOX_ACI_DMA_TRACE");
        g_aci_dma_trace = (e && e[0] == '1') ? 1 : 0;
    }

    for (ch = 0; ch < ACI_CHANNELS; ch++) {
        uint8_t *c = regs + base + ch * stride;
        uint8_t  cr = c[0x0B];
        uint16_t sr = rd16(c + 0x06);
        uint16_t picb = rd16(c + 0x08);
        uint32_t bdbar = (uint32_t)c[0x00] | ((uint32_t)c[0x01] << 8)
                       | ((uint32_t)c[0x02] << 16) | ((uint32_t)c[0x03] << 24);
        unsigned left = samples;

        if (!(cr & CR_RPBM) || (sr & SR_DCH) || !bdbar) continue;

        while (left > 0) {
            unsigned n;

            if (picb == 0) {
                /* Load the descriptor CIV points at. */
                uint32_t ent = bdbar + (uint32_t)c[0x04] * 8u;
                uint32_t ctl = guest_rd32(ent + 4);
                picb = (uint16_t)(ctl & BDL_LEN_MASK);
                if (picb == 0) {           /* empty descriptor: nothing to do */
                    sr |= SR_DCH;
                    break;
                }
            }

            n = (left < picb) ? left : picb;
            picb = (uint16_t)(picb - n);
            left -= n;

            if (picb == 0) {
                uint32_t ent = bdbar + (uint32_t)c[0x04] * 8u;
                uint32_t ctl = guest_rd32(ent + 4);

                if ((ctl & BDL_IOC) && (cr & CR_IOCE)) { sr |= SR_BCIS; raised = 1; }

                if (c[0x04] == c[0x05]) {          /* CIV == LVI: end of list */
                    sr |= SR_CELV | SR_DCH;
                    if (cr & CR_LVBIE) { sr |= SR_LVBCI; raised = 1; }
                    if (g_aci_dma_trace)
                        fprintf(stderr, "  [ACI-DMA] ch%u reached LVI %u, halted\n",
                                ch, c[0x05]);
                    break;
                }
                c[0x04] = (uint8_t)((c[0x04] + 1u) & 31u);   /* CIV */
                c[0x0A] = (uint8_t)((c[0x04] + 1u) & 31u);   /* PIV prefetch */
                sr &= (uint16_t)~SR_CELV;
            }
        }

        wr16(c + 0x08, picb);
        wr16(c + 0x06, sr);

        if (g_aci_dma_trace) {
            static unsigned long n = 0;
            if ((++n & 0x3FF) == 1)
                fprintf(stderr, "  [ACI-DMA] ch%u civ=%u lvi=%u picb=%u sr=0x%04X cr=0x%02X\n",
                        ch, c[0x04], c[0x05], picb, sr, cr);
        }
    }

    if (raised && g_aci_dma_trace) {
        fprintf(stderr, "  [ACI-DMA] interrupt status raised\n");
        fflush(stderr);
    }
    return raised;
}
