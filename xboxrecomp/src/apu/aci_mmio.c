/*
 * MCPX ACI (AC'97 Codec Interface) MMIO emulation.
 *
 * Layout, per xemu's hw/xbox/mcpx/aci.c: a 0x1000 MMIO region at Xbox VA
 * 0xFEC00000, with the AC'97 Native Audio Mixer at offset 0x000 and the Native
 * Audio Bus Master block aliased in at 0x100. Each of the four bus-master
 * channels occupies 16 bytes:
 *
 *   +0x00 BDBAR   +0x04 CIV   +0x05 LVI   +0x06 SR
 *   +0x08 PICB    +0x0A PIV   +0x0B CR
 *
 * WHY THIS EXISTS
 * ---------------
 * SSX's DSOUND resets a channel the standard AC'97 way: set CR.RR, then read
 * the register back and wait for the hardware to clear it. The title's loop
 * (VA 0x0017E9AC, inside sub_0017E97A) is three instructions:
 *
 *     mov BYTE PTR [chan+0x0B], 0x2   ; CR |= RR
 *     mov cl, BYTE PTR [chan+0x0B]    ; read back -- ONCE
 *     and cl, 0x2
 *     jne $-2                         ; spin on the cached byte
 *
 * It never re-reads memory, so RR must already be clear by the time that
 * single read executes. Real AC'97 clears RR as a side effect of the write,
 * which is why the loop falls straight through on hardware. With the aperture
 * backed by ordinary RAM the 2 stayed 2 and the game's main thread never left
 * audio init -- confirmed by attaching to the idle process and finding thread 1
 * parked there under the whole DSOUND call chain.
 *
 * A polling pump cannot fix this: the read happens a few instructions after the
 * write, and the loop then spins on a cached value, so there is no window for
 * an asynchronous fixup to win. The semantics have to be applied *during* the
 * write, which means trapping the access.
 */

#include "aci_mmio.h"
#include <stdio.h>
#include <string.h>

/* Register file. Reads and writes land here; the side effects below are what
 * distinguish this from the plain RAM it replaces. */
static uint8_t g_aci_regs[XBOX_ACI_MMIO_SIZE];

static int g_aci_reads, g_aci_writes, g_aci_decode_fail;

#define ACI_NABM_OFF        0x100u
/* AC'97 global registers, NABM-relative. GLOB_STA.PCR is the codec-ready bit
 * DSOUND waits on: it resets the AC-link through GLOB_CNT, then polls PCR
 * every 20 us for 1000 tries (0x0017E6E9). With the register file backed by
 * plain RAM the bit never appeared, the poll timed out after 20 ms, audio init
 * returned failure and the title's main thread never left it -- which is why
 * enabling DSOUND collapsed drawing to zero. There is a working XAudio2 codec
 * behind this, so reporting it ready is the truth. */
#define ACI_GLOB_CNT_OFF    (ACI_NABM_OFF + 0x2Cu)
#define ACI_GLOB_STA_OFF    (ACI_NABM_OFF + 0x30u)
#define ACI_GLOB_STA_PCR    0x00000100u   /* Primary Codec Ready */
#define ACI_GLOB_CNT_COLD_RESET 0x00000002u /* 0 = AC-link held in reset */

/* The Xbox MCPX has a fourth bus-master channel -- SPDIF out -- at NABM+0x70,
 * past the three desktop AC'97 ones and past the global registers. The title
 * resets it exactly like the others: write CR.RR, read back once, spin while
 * the bit is still set. With this offset outside every channel range the write
 * fell through to the generic store, RR stayed set, and the read-back spun
 * forever -- 20,000 hits on a probe at 0x0017E9AC, and the main thread never
 * left audio init. */
#define ACI_SPDIF_OFF       (ACI_NABM_OFF + 0x70u)

/* NOT SET, deliberately --
 *
 * Reporting PCR does exactly what it should to DSOUND: the 2000-iteration
 * spin at 0x0017E6E9 disappears and the AC-link reset takes its success path
 * for the first time. But it also collapses drawing to zero in the DEFAULT
 * build, with DSOUND still denied and the audio streaming thread denied too --
 * so something else reads GLOB_STA and changes behaviour once the codec claims
 * to be ready, and that path is not yet understood.
 *
 * Setting this bit is almost certainly correct and will be needed for audio.
 * It stays off until the downstream blocker is found, because a working
 * picture is worth more than an audio path that stops the game booting. */
#define ACI_NABM_CHANNELS   4
#define ACI_CHANNEL_STRIDE  0x10u

#define ACI_CR_RR           0x02u   /* Reset Registers -- hardware self-clears */
#define ACI_CR_RPBM         0x01u   /* Run/Pause Bus Master                    */
#define ACI_CR_DONT_CLEAR   0x1Cu   /* IOCE | FEIE | LVBIE survive a reset     */
#define ACI_SR_DCH          0x01u   /* DMA Controller Halted                   */
#define ACI_SR_WCLEAR_MASK  0x1Cu   /* FIFOE | BCIS | LVBCI are write-1-clear  */

/* ---- device semantics ---------------------------------- */

/* Mirror QEMU's ac97 reset_bm_regs(): clear the bus-master registers, park the
 * channel with DCH raised, and drop everything from CR except the interrupt
 * enables -- which is what clears RR and releases the title's spin. */
static void aci_reset_channel(uint32_t chan)
{
    uint8_t cr = g_aci_regs[chan + 0x0B];

    g_aci_regs[chan + 0x00] = 0;
    g_aci_regs[chan + 0x01] = 0;
    g_aci_regs[chan + 0x02] = 0;
    g_aci_regs[chan + 0x03] = 0;   /* BDBAR */
    g_aci_regs[chan + 0x04] = 0;   /* CIV   */
    g_aci_regs[chan + 0x05] = 0;   /* LVI   */
    g_aci_regs[chan + 0x06] = ACI_SR_DCH;
    g_aci_regs[chan + 0x07] = 0;   /* SR is 16-bit */
    g_aci_regs[chan + 0x08] = 0;
    g_aci_regs[chan + 0x09] = 0;   /* PICB */
    g_aci_regs[chan + 0x0A] = 0;   /* PIV  */
    g_aci_regs[chan + 0x0B] = (uint8_t)(cr & ACI_CR_DONT_CLEAR);
}

/* Codec-ready is correct behaviour and is now ON by default. It was gated for a
 * long time because raising it hung the boot -- but that hang was never the
 * codec. The Xbox MCPX puts a fourth, SPDIF bus-master channel at NABM+0x70,
 * outside the three-channel PC AC'97 layout this file modelled; every access to
 * it fell through to the raw-register path, so the driver's reset of that
 * channel never completed and it spun ~20,000 times at 0x0017E9AC waiting for
 * CR.RR to self-clear. With the SPDIF channel decoded the spin ends after 6
 * reads and the boot is indistinguishable from codec-ready being off
 * (4 runs each: 7633..7889 draws on, 7587..7954 off).
 *
 * XBOX_ACI_PCR=0 forces it back off, for bisecting. */
static int aci_pcr_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XBOX_ACI_PCR");
        on = (e && e[0] == '0') ? 0 : 1;
    }
    return on;
}

static uint64_t aci_read(uint32_t off, unsigned size)
{
    uint64_t v = 0;
    unsigned i;
    if (off + size > XBOX_ACI_MMIO_SIZE) return 0;
    for (i = 0; i < size; i++)
        v |= (uint64_t)g_aci_regs[off + i] << (8 * i);
    g_aci_reads++;
    return v;
}

static void aci_write(uint32_t off, uint64_t val, unsigned size)
{
    unsigned i;
    if (off + size > XBOX_ACI_MMIO_SIZE) return;
    g_aci_writes++;


    /* AC'97 global registers. These live at NABM+0x2C and NABM+0x30, i.e.
     * immediately after the three bus-master channels -- NOT inside them. The
     * channel test below used ACI_NABM_CHANNELS * ACI_CHANNEL_STRIDE, which
     * with 4 channels covers 0x100..0x13F and swallows both of them, so
     * GLOB_CNT was being handled as channel 2 register 0x0C and GLOB_STA as
     * channel 3's BDBAR. They are handled here, first, and the channel test
     * below is bounded by ACI_GLOB_CNT_OFF instead of a channel count. */
    if (off >= ACI_GLOB_CNT_OFF && off < ACI_GLOB_CNT_OFF + 4) {
        /* GLOB_CNT: bit 1 is the AC-link cold reset, released by the driver to
         * bring the link up. A real codec answers by raising GLOB_STA.PCR a
         * moment later; DSOUND polls for exactly that (0x0017E6E9) and gives up
         * after 20 ms. Raising PCR unconditionally at power-on instead breaks
         * the boot -- something reads GLOB_STA before the link is up and takes
         * a different path -- so it is raised here, in response to the reset,
         * which is also what the hardware does. */
        uint32_t before = (uint32_t)g_aci_regs[ACI_GLOB_CNT_OFF]
                        | ((uint32_t)g_aci_regs[ACI_GLOB_CNT_OFF + 1] << 8);
        uint32_t after;
        for (i = 0; i < size; i++)
            g_aci_regs[off + i] = (uint8_t)((val >> (8 * i)) & 0xFF);
        after = (uint32_t)g_aci_regs[ACI_GLOB_CNT_OFF]
              | ((uint32_t)g_aci_regs[ACI_GLOB_CNT_OFF + 1] << 8);

        if ((after & ACI_GLOB_CNT_COLD_RESET) &&
            !(before & ACI_GLOB_CNT_COLD_RESET) && aci_pcr_enabled()) {
            g_aci_regs[ACI_GLOB_STA_OFF + 1] |=
                (uint8_t)((ACI_GLOB_STA_PCR >> 8) & 0xFF);
            fprintf(stderr, "  [ACI] AC-link cold reset released -- "
                    "primary codec ready\n");
            fflush(stderr);
        }
        return;
    }
    if (off >= ACI_GLOB_STA_OFF && off < ACI_GLOB_STA_OFF + 4) {
        /* GLOB_STA's interrupt bits are write-1-clear; PCR is read-only status
         * and must survive a driver clearing them. */
        for (i = 0; i < size; i++) {
            uint8_t keep = (off + i == ACI_GLOB_STA_OFF + 1)
                         ? (uint8_t)(g_aci_regs[off + i] & ((ACI_GLOB_STA_PCR >> 8) & 0xFF))
                         : 0u;
            g_aci_regs[off + i] = (uint8_t)(((val >> (8 * i)) & 0xFF) | keep);
        }
        return;
    }

    /* Bus-master registers carry the side effects: the three AC'97 channels
     * below the globals, plus the Xbox's SPDIF channel above them. */
    if ((off >= ACI_NABM_OFF && off < ACI_GLOB_CNT_OFF) ||
        (off >= ACI_SPDIF_OFF && off < ACI_SPDIF_OFF + ACI_CHANNEL_STRIDE)) {
        uint32_t rel  = off - ACI_NABM_OFF;
        uint32_t chan = ACI_NABM_OFF + (rel / ACI_CHANNEL_STRIDE) * ACI_CHANNEL_STRIDE;
        uint32_t creg = rel % ACI_CHANNEL_STRIDE;

        if (creg == 0x0B) {                       /* CR */
            uint8_t v = (uint8_t)(val & 0xFF);
            if (v & ACI_CR_RR) {
                g_aci_regs[off] = v;
                aci_reset_channel(chan);
            } else {
                g_aci_regs[off] = (uint8_t)(v & 0x1F);
                /* Nothing is DMA-ing yet, so a channel told to stop reports
                 * itself halted immediately rather than never completing. */
                if (!(v & ACI_CR_RPBM))
                    g_aci_regs[chan + 0x06] |= ACI_SR_DCH;
            }
            return;
        }
        if (creg == 0x06) {                       /* SR: write-1-to-clear */
            uint8_t cur = g_aci_regs[off];
            g_aci_regs[off] = (uint8_t)(cur & ~((uint8_t)val & ACI_SR_WCLEAR_MASK));
            return;
        }
    }

    for (i = 0; i < size; i++)
        g_aci_regs[off + i] = (uint8_t)(val >> (8 * i));
}

/* Accessors for the DMA engine in aci_dma.c, which walks the same register
 * file the trap maintains. */
uint8_t *aci_reg_file(void)        { return g_aci_regs; }
uint32_t aci_nabm_off(void)        { return ACI_NABM_OFF; }
uint32_t aci_channel_stride(void)  { return ACI_CHANNEL_STRIDE; }

void aci_mmio_report(void)
{
    fprintf(stderr, "  [ACI] %d reads, %d writes, %d undecoded\n",
            g_aci_reads, g_aci_writes, g_aci_decode_fail);
    fflush(stderr);
}

/* ---- instruction decode (same shape as apu_mmio_hook.c) - */

#ifdef _WIN32

static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t*)&ctx->Rax;   case 1:  return (uint64_t*)&ctx->Rcx;
    case 2:  return (uint64_t*)&ctx->Rdx;   case 3:  return (uint64_t*)&ctx->Rbx;
    case 4:  return (uint64_t*)&ctx->Rsp;   case 5:  return (uint64_t*)&ctx->Rbp;
    case 6:  return (uint64_t*)&ctx->Rsi;   case 7:  return (uint64_t*)&ctx->Rdi;
    case 8:  return (uint64_t*)&ctx->R8;    case 9:  return (uint64_t*)&ctx->R9;
    case 10: return (uint64_t*)&ctx->R10;   case 11: return (uint64_t*)&ctx->R11;
    case 12: return (uint64_t*)&ctx->R12;   case 13: return (uint64_t*)&ctx->R13;
    case 14: return (uint64_t*)&ctx->R14;   case 15: return (uint64_t*)&ctx->R15;
    default: return NULL;
    }
}

static int decode_modrm_len(const uint8_t *m, int has_rex_b)
{
    uint8_t modrm = m[0];
    int mod = (modrm >> 6) & 3, rm = modrm & 7;
    int len = 1;
    (void)has_rex_b;
    if (mod != 3 && rm == 4) len++;                       /* SIB */
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    else if (mod == 0) {
        if (rm == 5) len += 4;                            /* RIP-relative */
        else if (rm == 4 && (m[1] & 7) == 5) len += 4;    /* SIB with disp32 */
    }
    return len;
}

bool aci_hook_handle_mmio(PCONTEXT ctx, uint32_t fault_xbox_va, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    uint32_t off = fault_xbox_va - XBOX_ACI_MMIO_BASE;
    int prefix_len = 0, has_66 = 0, rex = 0, has_rex = 0;
    int rex_w, rex_r, rex_b, access_size;
    const uint8_t *opcode;

    (void)is_write;
    if (off >= XBOX_ACI_MMIO_SIZE) return false;

    for (;;) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) { has_66 = 1; prefix_len++; }
        else if (b == 0xF2 || b == 0xF3) { prefix_len++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix_len++; }
        else break;
    }
    rex_w = has_rex && (rex & 0x08);
    rex_r = has_rex && (rex & 0x04);
    rex_b = has_rex && (rex & 0x01);
    opcode = ip + prefix_len;
    access_size = has_66 ? 2 : (rex_w ? 8 : 4);

    /* MOV r/m, r  (88/89) */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        int len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        if (opcode[0] == 0x88) access_size = 1;
        aci_write(off, *ctx_reg64(ctx, reg), (unsigned)access_size);
        ctx->Rip += prefix_len + 1 + len;
        return true;
    }
    /* MOV r/m32, imm32 (C7 /0) */
    if (opcode[0] == 0xC7) {
        int len = decode_modrm_len(opcode + 1, rex_b);
        uint32_t imm = *(const uint32_t *)(opcode + 1 + len);
        aci_write(off, imm, (unsigned)access_size);
        ctx->Rip += prefix_len + 1 + len + 4;
        return true;
    }
    /* MOV r/m8, imm8 (C6 /0) -- this is the CR write the title spins on */
    if (opcode[0] == 0xC6) {
        int len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = opcode[1 + len];
        aci_write(off, imm, 1);
        ctx->Rip += prefix_len + 1 + len + 1;
        return true;
    }
    /* MOV r, r/m (8A/8B) -- and this is the read-back */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        int len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val;
        uint64_t *dst;
        if (opcode[0] == 0x8A) access_size = 1;
        val = aci_read(off, (unsigned)access_size);
        dst = ctx_reg64(ctx, reg);
        if (access_size == 1)      *dst = (*dst & ~0xFFULL)   | (val & 0xFF);
        else if (access_size == 2) *dst = (*dst & ~0xFFFFULL) | (val & 0xFFFF);
        else if (access_size == 4) *dst = val & 0xFFFFFFFFULL;
        else                       *dst = val;
        ctx->Rip += prefix_len + 1 + len;
        return true;
    }
    /* MOVZX r32, r/m8 / r/m16 (0F B6 / 0F B7) */
    if (opcode[0] == 0x0F && (opcode[1] == 0xB6 || opcode[1] == 0xB7)) {
        unsigned sz = (opcode[1] == 0xB6) ? 1u : 2u;
        int len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        *ctx_reg64(ctx, reg) = aci_read(off, sz) & (sz == 1 ? 0xFFu : 0xFFFFu);
        ctx->Rip += prefix_len + 2 + len;
        return true;
    }

    g_aci_decode_fail++;
    if (g_aci_decode_fail <= 8) {
        fprintf(stderr, "  [ACI] undecoded access at VA 0x%08X, opcode %02X %02X %02X\n",
                fault_xbox_va, opcode[0], opcode[1], opcode[2]);
        fflush(stderr);
    }
    return false;
}

bool aci_mmio_install(void *mem_base)
{
    DWORD old = 0;
    void *page = (uint8_t *)mem_base + XBOX_ACI_MMIO_BASE;
    int ch;

    memset(g_aci_regs, 0, sizeof(g_aci_regs));
    /* PCR is deliberately NOT set here. On real hardware the codec is not
     * ready at power-on -- it becomes ready a moment after the driver releases
     * the AC-link cold reset, and aci_write() raises it there. Forcing it
     * ready before the link is up let code that samples GLOB_STA early take a
     * path the hardware would never give it. */
    /* Park every bus-master channel halted, which is the post-reset state. */
    for (ch = 0; ch < ACI_NABM_CHANNELS; ch++)
        g_aci_regs[ACI_NABM_OFF + ch * ACI_CHANNEL_STRIDE + 0x06] = ACI_SR_DCH;

    if (!VirtualProtect(page, XBOX_ACI_MMIO_SIZE, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "  [ACI] could not guard the aperture (error %lu) -- "
                        "audio registers stay plain RAM\n", GetLastError());
        fflush(stderr);
        return false;
    }
    fprintf(stderr, "  [ACI] AC'97 aperture trapped at Xbox VA 0x%08X (%u bytes)\n",
            XBOX_ACI_MMIO_BASE, XBOX_ACI_MMIO_SIZE);
    fflush(stderr);
    return true;
}

#else
bool aci_mmio_install(void *mem_base) { (void)mem_base; return false; }
#endif /* _WIN32 */
