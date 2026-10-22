/* Guest-window test (MACOS_PORT_PLAN.md 0.3): runs the real xbox_MemoryLayoutInit
 * on a blank XBE and checks, through the same MEM32() macros the translated code
 * uses, that the RAM mirrors alias, the uncached/write-combined aliases resolve
 * to RAM, and the gap and GPU aperture are plain readable/writable memory. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "recomp_types.h"
#include "kernel/xbox_memory_layout.h"
#include "platform/mmio_trap.h"
#include <stdbool.h>

/* device installers (apu/aci_mmio.h, apu/apu.h, nv2a/nv2a_mmio_hook.h) */
extern bool aci_mmio_install(void *mem_base);
extern bool apu_mmio_install(uint8_t *mem_base);
extern bool nv2a_dac_trap_install(uint8_t *mem_base);

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    char dumpdir[] = "/tmp/ot_fbdump_XXXXXX";
    if (mkdtemp(dumpdir)) setenv("XBOX_DUMP_FB", dumpdir, 1);   /* read once, at the first present */
    static uint8_t xbe[0x1000];
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) { printf("FAIL: xbox_MemoryLayoutInit\n"); return 1; }
    g_xbox_mem_offset = xbox_GetMemoryOffset();

    const uint32_t size   = (uint32_t)xbox_GetMemorySize();
    const int      slots  = xbox_GetMirrorSlotCount();
    printf("RAM %u MB, %d mirror slots, base %p\n", size >> 20, slots, xbox_GetMemoryBase());
    CHECK(slots >= 20, "mirror slot count %d", slots);

    /* mirrors alias RAM: write through the base, read through every slot */
    const uint32_t va = 0x00500000u;
    MEM32(va) = 0xA5A5C3C3u;
    int checked = 0;
    for (int m = 1; m <= slots; m++) {
        uint32_t a = va + (uint32_t)m * size;
        /* XBOX_PTR folds these two windows onto RAM itself (uncached 0x80000000,
         * write-combined 0xF0000000), so they never reach the mirror views. */
        if ((a & 0xFC000000u) == 0x80000000u || (a >= 0xF0000000u && a < 0xF4000000u)) continue;
        CHECK(MEM32(a) == 0xA5A5C3C3u, "mirror %d read", m);
        checked++;
    }
    printf("%d mirror slots read back (others fall in the uncached/WC alias windows)\n", checked);
    /* ... and the other way round, and at sub-word sizes */
    MEM32(va + 5u * size) = 0x11223344u;
    CHECK(MEM32(va) == 0x11223344u, "write via mirror 5 visible at base");
    MEM8(va + 9u * size + 1) = 0xEE;
    CHECK(MEM8(va + 1) == 0xEE && MEM16(va) == 0xEE44u, "byte/half through mirror 9");
    MEM16(va + 20u * size + 2) = 0xBEEF;   /* mirror 20: a 64-bit-address-sized offset */
    CHECK(MEM32(va) == 0xBEEFEE44u, "half through mirror 20");

    /* uncached (0x80000000) and write-combined (0xF0000000) aliases are RAM */
    CHECK(MEM32(0x80000000u + va) == MEM32(va), "uncached alias");
    MEM32(0xF0000000u + va + 4) = 0xCAFEF00Du;
    CHECK(MEM32(va + 4) == 0xCAFEF00Du, "write-combined alias write");

    /* the gap above the last mirror and the GPU aperture are plain memory */
    uint32_t gap = (uint32_t)(slots + 1) * size + 0x1000u;
    CHECK(gap < 0xFD000000u, "gap exists (0x%08X)", gap);
    MEM32(gap) = 0x01020304u;
    CHECK(MEM32(gap) == 0x01020304u, "gap read/write");
    MEM32(0xFDF00000u) = 0x0BADF00Du;
    CHECK(MEM32(0xFDF00000u) == 0x0BADF00Du, "GPU aperture read/write");
    CHECK(MEM32(0xFFFFFF00u) == 0 || 1, "top of address space readable");

    /* device registers: protected pages whose loads and stores the MMIO trap emulates.
     * Every access below goes through MEM*(), i.e. the same code the translated game runs. */
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    CHECK(aci_mmio_install(mem), "ACI trap installed");
    CHECK(apu_mmio_install(mem), "APU trap installed");
    CHECK(nv2a_dac_trap_install(mem), "DAC trap installed");
    uint64_t t0 = ot_mmio_trap_count();

    /* ACI: bus-master channels reset halted (status register 0x106, bit 0 = DCH) */
    CHECK((MEM16(0xFEC00000u + 0x106) & 1) == 1, "ACI channel 0 reads as halted after reset");
    MEM8(0xFEC00000u + 0x10B) = 0x00;            /* control register write must not crash */
    /* ... and the rest of its 16 KB host page is plain memory, not the device */
    MEM32(0xFEC01000u) = 0x13572468u;
    CHECK(MEM32(0xFEC01000u) == 0x13572468u, "memory next to the ACI window behaves like RAM");
    /* APU: registers are reachable; the DSOUND poll reads this one */
    uint32_t apu_reg = MEM32(0xFE820010u);
    printf("APU 0xFE820010 reads 0x%08X\n", apu_reg);
    MEM32(0xFE800000u + 0x2000) = 0;
    /* DAC palette: VGA index register written and read back through the device */
    MEM8(0xFD681000u + 0x3C8) = 0x5A;
    CHECK(MEM8(0xFD681000u + 0x3C8) == 0x5A, "DAC write index reads back");
    MEM8(0xFD681000u + 0x3C6) = 0xFF;
    CHECK(MEM8(0xFD681000u + 0x3C6) == 0xFF, "DAC pixel mask reads back");
    CHECK(ot_mmio_trap_count() - t0 >= 8, "device accesses went through the trap (%llu)",
          (unsigned long long)(ot_mmio_trap_count() - t0));

    /* ---- null backend: drive the pump with a hand-made push buffer ----------- */
    {
        const uint32_t ctx = 0x00780000u, ring = 0x00800000u, ring_len = 0x10000u, reports = 0x00300000u;
        uint32_t wp = ring;
        MEM32(0x001776C0u) = ctx;                    /* the channel context pointer the pump reads */
        MEM32(ctx + 0x00) = wp;                      /* write pointer */
        MEM32(ctx + 0x04) = ring + ring_len;         /* limit */
        MEM32(ctx + 0x10) = ring;                    /* buffer base */
        MEM32(ctx + 0x14) = ring + ring_len;         /* buffer end */
        usleep(50 * 1000);                           /* let the pump see an empty ring and bring the backend up */

        extern unsigned d3d8_PresentSeq(void);
        unsigned seq0 = d3d8_PresentSeq();
        const int frames = 20;
        int reports_ok = 0;
        for (int i = 0; i < frames; i++) {
            uint32_t surf = 0x00100000u + (uint32_t)(i & 1) * 0x00400000u;
            uint32_t rep  = reports + (uint32_t)i * 16u;
            MEM32(rep + 12) = 0xFFFFFFFFu;           /* "pending", as the title sets it */
            #define EMIT(m, p) do { MEM32(wp) = (1u << 18) | (m); MEM32(wp + 4) = (p); wp += 8; } while (0)
            EMIT(0x0100u, 0x5C000000u | (uint32_t)(i + 1));   /* NV097_NO_OPERATION: the guest's draw fence */
            EMIT(0x0210u, surf);                     /* NV097_SET_SURFACE_COLOR_OFFSET: a flip from the second frame on */
            EMIT(0x17FCu, 5u);                       /* NV097_SET_BEGIN_END begin ... */
            EMIT(0x17FCu, 0u);                       /* ... end: one draw */
            EMIT(0x17D0u, (1u << 24) | rep);         /* NV097_GET_REPORT, zpass */
            __atomic_store_n((volatile uint32_t *)((uint8_t *)mem + ctx), wp, __ATOMIC_RELEASE);
            for (int w = 0; w < 2000 && MEM32(rep + 12) != 0; w++) usleep(1000);
            if (MEM32(rep + 12) == 0 && MEM32(rep + 8) == 256u) reports_ok++;
        }
        usleep(50 * 1000);
        extern volatile uint32_t pgraph_d3d11_fork_fence;
        CHECK(pgraph_d3d11_fork_fence == (uint32_t)frames, "draw fence reached the last NOP (%u)", pgraph_d3d11_fork_fence);
        CHECK(reports_ok == frames, "every GET_REPORT completed with the visible count (%d/%d)", reports_ok, frames);
        CHECK(d3d8_PresentSeq() - seq0 == (unsigned)frames - 1, "one present per surface flip with draws (%u for %d frames)",
              d3d8_PresentSeq() - seq0, frames);
    }

    /* ---- guest framebuffer dump: what the movie player writes through LockRect ----
     * Two buffers, as the title double-buffers its video: a present must show the one
     * that is not being written right now and never an older frame (the choice is
     * ported from the real D3D layer). A is seeded, then B is rewritten, then both idle. */
    {
        extern void d3d8_SetGuestFramebuffer(const void *src, unsigned pitch, unsigned w, unsigned h);
        extern void d3d8_SetGuestFramebufferAlt(const void *alt);
        extern void d3d8_NoteGuestFramebufferLock(void);
        extern void d3d8_PresentFrame(void);
        static uint8_t fa[16 * 64], fb[16 * 64];
        #define FILL(buf, r, g, b) do { for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) { \
            uint8_t *px = (buf) + y * 64 + x * 4; px[0] = (b); px[1] = (g); px[2] = (r); px[3] = 0; } } while (0)
        FILL(fa, 200, 60, 50);                 /* X8R8G8B8 in memory: B G R X */
        FILL(fb, 10, 20, 30);
        d3d8_SetGuestFramebuffer(fa, 64, 16, 8);
        d3d8_SetGuestFramebufferAlt(fb);

        unsigned want[4][3] = { {200, 60, 50}, {200, 60, 50}, {111, 112, 113}, {0} };
        const char *what[3] = { "seeding present shows A", "B being rewritten: A is held, B is not shown", "both idle: the newer B is shown" };
        for (int step = 0; step < 3; step++) {
            if (step == 1) FILL(fb, 111, 112, 113);
            d3d8_NoteGuestFramebufferLock();
            d3d8_PresentFrame();
            char path[256]; snprintf(path, sizeof path, "%s/fb_%05d.ppm", dumpdir, step);
            FILE *f = fopen(path, "rb");
            uint8_t px[3] = {0}; int ok = 0;
            if (f) {
                char magic[3] = {0}; unsigned w = 0, h = 0, mx = 0;
                if (fscanf(f, "%2s %u %u %u", magic, &w, &h, &mx) == 4 && fgetc(f) == '\n') {
                    fseek(f, 3 * (3 * 16 + 5), SEEK_CUR);          /* pixel (5, 3) */
                    ok = fread(px, 1, 3, f) == 3 && w == 16 && h == 8 && mx == 255 && magic[0] == 'P' && magic[1] == '6';
                }
                fclose(f);
            }
            CHECK(ok && px[0] == want[step][0] && px[1] == want[step][1] && px[2] == want[step][2],
                  "framebuffer dump %d (%s): pixel (5,3) = %u,%u,%u, want %u,%u,%u", step, what[step],
                  px[0], px[1], px[2], want[step][0], want[step][1], want[step][2]);
        }
    }

    /* ---- null audio sink: the APU thread must run at the sample rate, not flat out ---- */
    {
        extern uint64_t xa2_null_frames_submitted(void);
        extern int xa2_queued(void);
        uint64_t s0 = xa2_null_frames_submitted();
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        int qmax = 0;
        for (int i = 0; i < 40; i++) { usleep(50 * 1000); int q = xa2_queued(); if (q > qmax) qmax = q; }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        double audio = (double)(xa2_null_frames_submitted() - s0) / 48000.0;
        printf("audio: %.2f s of sound mixed in %.2f s of wall time (queue peaked at %d buffers)\n", audio, secs, qmax);
        CHECK(audio > secs * 0.8 && audio < secs * 1.2, "APU mixes at real time (%.2f s per %.2f s)", audio, secs);
        CHECK(qmax <= 24, "queue stays bounded (%d)", qmax);
    }

    printf("%d passed, %d failed\n", g_pass, g_fail);
    fflush(stdout);
    _exit(g_fail ? 1 : 0);   /* skip shutdown: pump threads are still running */
}
