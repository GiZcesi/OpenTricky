/*
 * MCPX APU MMIO Hook - VEH instruction decoder for APU register access
 *
 * Reuses the same x86-64 instruction decoder pattern as nv2a_mmio_hook.c
 * but routes reads/writes through the MCPX APU register handlers.
 */

#include "apu.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

/* Global APU state pointer -- referenced from main.c regardless of which
 * platform's MMIO hook is active, so define it before the #if guard. */
MCPXAPUState *g_apu_state = NULL;

/* The MMIO hook is a Win32-VEH x86-64 instruction decoder. On Linux the
 * equivalent goes through sigaction + ucontext_t (Stage 2 / main.c). For
 * now the whole body is Windows-only so apu_emu links on Debian. */
#if defined(_WIN32)
#include <windows.h>

/* APU MMIO base in Xbox VA space */
#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00080000u  /* 512KB */

/* (g_apu_state is defined above, outside the Win32 guard) */

/* Statistics */
static int g_apu_mmio_read_count = 0;
static int g_apu_mmio_write_count = 0;
static int g_apu_mmio_decode_fail = 0;

/* ============================================================
 * x86-64 register access helpers (same as NV2A hook)
 * ============================================================ */

static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t*)&ctx->Rax;
    case 1:  return (uint64_t*)&ctx->Rcx;
    case 2:  return (uint64_t*)&ctx->Rdx;
    case 3:  return (uint64_t*)&ctx->Rbx;
    case 4:  return (uint64_t*)&ctx->Rsp;
    case 5:  return (uint64_t*)&ctx->Rbp;
    case 6:  return (uint64_t*)&ctx->Rsi;
    case 7:  return (uint64_t*)&ctx->Rdi;
    case 8:  return (uint64_t*)&ctx->R8;
    case 9:  return (uint64_t*)&ctx->R9;
    case 10: return (uint64_t*)&ctx->R10;
    case 11: return (uint64_t*)&ctx->R11;
    case 12: return (uint64_t*)&ctx->R12;
    case 13: return (uint64_t*)&ctx->R13;
    case 14: return (uint64_t*)&ctx->R14;
    case 15: return (uint64_t*)&ctx->R15;
    default: return NULL;
    }
}

static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1; /* SIB */
    if (mod == 0 && (rm & 7) == 5) len += 4; /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* ============================================================
 * Instruction decoder for APU MMIO access
 * ============================================================ */

/* Which APU registers the title actually touches.
 *
 * DSOUND busy-waits on some hardware condition -- KeStallExecutionProcessor is
 * 12% of all kernel calls with it enabled, and the main thread never advances.
 * Counting reads per offset says which register it is polling, which is the
 * one the stubbed DSP has to answer. XBOX_APU_TRACE=1 prints the histogram
 * every few seconds. */
#define APU_HIST_SLOTS 64
static struct { uint32_t off; unsigned n; uint64_t last; } g_apu_hist[APU_HIST_SLOTS];
static int g_apu_hist_used = 0;

static void apu_mmio_note(uint32_t off, uint64_t val, int is_write);
static void apu_mmio_note_read(uint32_t off, uint64_t val)
{ apu_mmio_note(off, val, 0); }
static void apu_mmio_note_write(uint32_t off, uint64_t val)
{ apu_mmio_note(off, val, 1); }
static void apu_mmio_note(uint32_t off, uint64_t val, int is_write)
{
    static int enabled = -1;
    static unsigned long last_report = 0;
    int i;

    if (enabled < 0) {
        const char *e = getenv("XBOX_APU_TRACE");
        enabled = (e && (e[0] == '1' || e[0] == '2')) ? (e[0] - '0') : 0;
    }
    if (!enabled) return;

    /* XBOX_APU_TRACE=2 also logs the first 600 accesses in order. The
     * histogram says which registers were touched and their last value;
     * bring-up questions ("was this ever written with X, and before or after
     * that?") need the sequence. */
    /* Reads of the PIO free-space register (+0x20010) precede every method
     * write and would fill the log on their own, so they are left out. */
    if (enabled == 2 && !(off == 0x20010 && !is_write)) {
        static volatile LONG seq = 0;
        LONG k = InterlockedIncrement(&seq);
        if (k <= 4000) {
            fprintf(stderr, "  [APU-SEQ] %4ld %s +0x%05X = 0x%llX\n", (long)k,
                    is_write ? "W" : "R", off, (unsigned long long)val);
            fflush(stderr);
        }
    }

    for (i = 0; i < g_apu_hist_used; i++) {
        if (g_apu_hist[i].off == (off | (is_write ? 0x80000000u : 0u))) { g_apu_hist[i].n++; g_apu_hist[i].last = val; break; }
    }
    int new_reg = 0;
    if (i == g_apu_hist_used && g_apu_hist_used < APU_HIST_SLOTS) {
        g_apu_hist[g_apu_hist_used].off  = off | (is_write ? 0x80000000u : 0u);
        g_apu_hist[g_apu_hist_used].n    = 1;
        g_apu_hist[g_apu_hist_used].last = val;
        g_apu_hist_used++;
        new_reg = 1;
    }

    /* Report on a 5 s cadence AND whenever a register is seen for the first
     * time. Cadence alone printed once at the first access and never again
     * when the rest arrived as one burst inside five seconds -- a whole
     * DirectSound initialisation read as "one register touched, once"
     *. */
    {
        unsigned long now = (unsigned long)GetTickCount();
        if (new_reg || now - last_report >= 5000) {
            int k;
            last_report = now;
            fprintf(stderr, "  [APU-TRACE] accesses by register:\n");
            for (k = 0; k < g_apu_hist_used; k++)
                fprintf(stderr, "  [APU-TRACE]   %s +0x%05X  x%-8u last=0x%llX\n",
                        (g_apu_hist[k].off & 0x80000000u) ? "W" : "R",
                        g_apu_hist[k].off & 0x7FFFFFFFu, g_apu_hist[k].n,
                        (unsigned long long)g_apu_hist[k].last);
            fflush(stderr);
        }
    }
}

static bool apu_decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    if (!g_apu_state) return false;

    int prefix_len = 0;
    int has_66 = 0;
    int rex = 0, has_rex = 0;

    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) { has_66 = 1; prefix_len++; }
        else if (b == 0xF2 || b == 0xF3) { prefix_len++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix_len++; }
        else break;
    }

    int rex_w = has_rex && (rex & 0x08);
    int rex_r = has_rex && (rex & 0x04);
    int rex_b = has_rex && (rex & 0x01);

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4;
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    /* MOV r/m, r (write: 88/89) */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        apu_mmio_note_write(mmio_offset, (uint64_t)val);
        return true;
    }

    /* MOV r/m, imm32 (write: C7 /0) */
    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint32_t imm = *(uint32_t *)(opcode + 1 + modrm_len);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + 4;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r/m8, imm8 (write: C6 /0) */
    if (opcode[0] == 0xC6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r, r/m (read: 8A/8B) */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t *dst = ctx_reg64(ctx, reg);
        if (access_size == 1) *dst = (*dst & ~0xFFULL) | (val & 0xFF);
        else if (access_size == 2) *dst = (*dst & ~0xFFFFULL) | (val & 0xFFFF);
        else if (access_size == 4) *dst = val & 0xFFFFFFFF;
        else *dst = val;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        apu_mmio_note_read(mmio_offset, val);
        return true;
    }

    /* MOVZX r32, r/m8 (0F B6) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, 1);
        *ctx_reg64(ctx, reg) = val & 0xFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m16 (0F B7) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        access_size = 2;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, 2);
        *ctx_reg64(ctx, reg) = val & 0xFFFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* TEST r/m, r (84/85) - read */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val & reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* CMP r/m, r (38/39) */
    if (opcode[0] == 0x39 || opcode[0] == 0x38) {
        if (opcode[0] == 0x38) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }
        uint64_t result = mem_val - reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (mem_val < reg_val) ctx->EFlags |= 0x0001;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* CMP r/m, imm (80 /7, 81 /7, 83 /7). GCC 16 compiles the DSOUND
     * poll `cmp [0xFE820010], 4; jb` (sub_0017D660) to `cmp dword [mem], 3; ja`,
     * which fell through to "decode fail" and crashed the boot. */
    if ((opcode[0] == 0x80 || opcode[0] == 0x81 || opcode[0] == 0x83)
        && ((opcode[1] >> 3) & 7) == 7) {
        if (opcode[0] == 0x80) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        const uint8_t *immp = opcode + 1 + modrm_len;
        int imm_len;
        uint64_t imm_val;
        if (opcode[0] == 0x81) {
            if (access_size == 2) { imm_val = (uint64_t)(int64_t)*(const int16_t *)immp; imm_len = 2; }
            else { imm_val = (uint64_t)(int64_t)*(const int32_t *)immp; imm_len = 4; }
        } else {
            imm_val = (uint64_t)(int64_t)*(const int8_t *)immp; imm_len = 1;
        }
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            imm_val &= (1ULL << (access_size * 8)) - 1;
        }
        uint64_t result = mem_val - imm_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if ((result & (access_size == 8 ? ~0ULL : ((1ULL << (access_size * 8)) - 1))) == 0)
            ctx->EFlags |= 0x0040;
        if (mem_val < imm_val) ctx->EFlags |= 0x0001;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* OR r/m, r (08/09) */
    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, mem_val | reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* AND r/m, r (20/21) */
    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, mem_val & reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* Unrecognized */
    g_apu_mmio_decode_fail++;
    if (g_apu_mmio_decode_fail <= 20) {
        fprintf(stderr, "[APU] MMIO decode fail at RIP=%p offset=0x%X: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, mmio_offset, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API (called from VEH in main.c)
 * ============================================================ */

bool apu_mmio_install(uint8_t *mem_base)
{
    DWORD old = 0;
    void *page = mem_base + APU_MMIO_BASE;

    if (!g_apu_state) {
        g_apu_state = mcpx_apu_init_standalone(mem_base);
        if (!g_apu_state) {
            fprintf(stderr, "  [APU] init failed -- aperture left as plain RAM\n");
            fflush(stderr);
            return false;
        }
    }

    if (!VirtualProtect(page, APU_MMIO_SIZE, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "  [APU] could not guard the aperture (error %lu) -- "
                        "registers stay plain RAM\n", GetLastError());
        fflush(stderr);
        return false;
    }
    fprintf(stderr, "  [APU] MCPX aperture trapped at Xbox VA 0x%08X (%u bytes)\n",
            APU_MMIO_BASE, APU_MMIO_SIZE);
    fflush(stderr);
    return true;
}

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - APU_MMIO_BASE;
    return apu_decode_and_handle(ctx, mmio_offset, is_write);
}

#endif /* _WIN32 */
