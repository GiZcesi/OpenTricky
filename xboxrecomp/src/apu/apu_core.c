/*
 * MCPX APU Core - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "fpconv.h"

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;
extern MCPXAPUState *g_state;

/* See apu_shim.h: an APU write into the title's image. */
void apu_phys_write_check(hwaddr addr, uint32_t val, int width)
{
    static volatile LONG n = 0;
    LONG k = InterlockedIncrement(&n);
    if (k <= 8) {
        MCPXAPUState *d = g_state;
        void *fr[8];
        USHORT nf = CaptureStackBackTrace(0, 8, fr, NULL);
        uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
        USHORT i;
        fprintf(stderr, "[APU] write into the image: phys 0x%08llX (masked 0x%08X) <- 0x%08X/%d;"
                        " FENADDR=0x%08X VPVADDR=0x%08X VPSGEADDR=0x%08X VPSSLADDR=0x%08X"
                        " FEMEMADDR=0x%08X FECV=0x%X; host:",
                (unsigned long long)addr, (unsigned)(addr & 0x03FFFFFF), val, width,
                d ? d->regs[NV_PAPU_FENADDR] : 0, d ? d->regs[NV_PAPU_VPVADDR] : 0,
                d ? d->regs[NV_PAPU_VPSGEADDR] : 0, d ? d->regs[NV_PAPU_VPSSLADDR] : 0,
                d ? d->regs[NV_PAPU_FEMEMADDR] : 0, d ? d->regs[NV_PAPU_FECV] : 0);
        for (i = 0; i < nf; i++)
            fprintf(stderr, " 0x%llX", (unsigned long long)((uintptr_t)fr[i] - base + 0x140000000ull));
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;
volatile long g_vp_voice_frames = 0;  /* incremented in apu_vp.c voice_process */
volatile long g_vp_idle_seen = 0;     /* voices found in a list but not ACTIVE */  /* 0 = audio enabled */

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * IRQ handling (stubbed - no PCI bus in standalone)
 * ============================================================ */

static void update_irq(MCPXAPUState *d)
{
    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        /* In standalone mode we don't raise a PCI IRQ; the game's kernel
         * stub will poll ISTS directly or we'll signal via a flag. */
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

/* The front end trapped (apu_vp.c, SE2FE_IDLE_VOICE) and waits for the
 * title's interrupt routine. Set there, taken by XBOX_FIX_BOOTHANG (port/src),
 * which raises the line below and runs the routine on a guest thread. */
volatile long g_apu_fe_trap_pending = 0;

/* What the IRQ line would show the routine when it reads ISTS:
 * FETINTSTS for a trap, GINTSTS when IEN lets it through. Returns ISTS. */
uint32_t mcpx_apu_irq_raise(void)
{
    extern MCPXAPUState *g_state;
    if (!g_state) return 0;
    update_irq(g_state);
    return qatomic_read(&g_state->regs[NV_PAPU_ISTS]);
}

uint32_t mcpx_apu_reg(uint32_t addr)
{
    extern MCPXAPUState *g_state;
    return (g_state && addr < sizeof(g_state->regs) / sizeof(g_state->regs[0]))
               ? qatomic_read(&g_state->regs[addr]) : 0;
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100);
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)r);
     */
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)val);
     */
    (void)size;

    switch (addr) {
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        {   /* XBOX_BOOTDIAG=1: every change of the two control registers */
            static int diag = -1, shown = 0;
            if (diag < 0) {
                const char *e = getenv("XBOX_BOOTDIAG");
                diag = e && e[0] == '1';
            }
            if (diag && d->regs[addr] != (uint32_t)val && shown++ < 200)
                fprintf(stderr, "[APUDIAG] t %lu write %s %08X -> %08X\n",
                        (unsigned long)GetTickCount(),
                        addr == NV_PAPU_FECTL ? "FECTL" : "SECTL",
                        (unsigned)d->regs[addr], (uint32_t)val);
        }
        qatomic_set(&d->regs[addr], (uint32_t)val);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            /* The memory the APU writes into is named by these base
             * registers; report each value the guest programs, since a base
             * outside RAM is exactly what folds an APU write onto the image. */
            switch (addr) {
            case NV_PAPU_FENADDR: case NV_PAPU_FEMEMADDR:
            case NV_PAPU_VPVADDR: case NV_PAPU_VPSGEADDR: case NV_PAPU_VPSSLADDR:
            case NV_PAPU_GPSADDR: case NV_PAPU_GPFADDR:
            case NV_PAPU_EPSADDR: case NV_PAPU_EPFADDR:
                if (d->regs[addr] != (uint32_t)val) {
                    static int shown = 0;
                    if (shown++ < 48) {
                        fprintf(stderr, "[APU] base register 0x%04X <- 0x%08X\n",
                                (unsigned)addr, (uint32_t)val);
                        fflush(stderr);
                    }
                }
                break;
            default:
                break;
            }
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* On Linux, waveOut* are inert stubs from win32_compat.h: the APU's
 * waveOut fallback path stays inactive and never produces audio. */

/* Ring of waveOut buffers for double-buffering */
#define WAVEOUT_NUM_BUFS 4
#define WAVEOUT_BUF_SAMPLES 2048  /* ~42.7ms at 48kHz, matches 8-frame delivery rate */
#define MIXER_FRAME_SAMPLES 256  /* Internal mixing frame size (matches frame_buf) */

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;

    /* Try XAudio2 first (lower latency) */
    if (xa2_init()) {
        fprintf(stderr, "[APU] Using XAudio2 audio backend\n");
        return;
    }
    fprintf(stderr, "[APU] XAudio2 unavailable, falling back to waveOut\n");

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

/*
 * Build one 256-sample monitor block: the APU's own output for the last eight
 * 32-sample sub-frames, with the software mixer and the test tone on top.
 *
 * monitor.frame_buf is where the VP -> DSP pipeline leaves its output
 * (apu_dsp.c writes each sub-frame's slice; at the VP monitor point apu_vp.c
 * accumulates into it). This used to be cleared before rendering, and only
 * the test tone and the HLE software mixer were sent to the host -- so every
 * sample the emulated APU produced was discarded, and a title that drives the
 * APU through its own statically linked DirectSound (SSX Tricky does) stayed
 * silent however far its audio got.
 */
static void monitor_render_block(MCPXAPUState *d,
                                 int16_t out[MIXER_FRAME_SAMPLES][2])
{
    int16_t extra[MIXER_FRAME_SAMPLES][2];
    int i, c;

    memset(extra, 0, sizeof(extra));
    if (!g_audio_muted) {
        if (g_test_tone.active) {
            for (i = 0; i < MIXER_FRAME_SAMPLES; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                extra[i][0] = s;
                extra[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }
        mixer_render(extra, MIXER_FRAME_SAMPLES);
    }

    for (i = 0; i < MIXER_FRAME_SAMPLES; i++) {
        for (c = 0; c < 2; c++) {
            int v = g_audio_muted ? 0
                  : (int)d->monitor.frame_buf[i][c] + (int)extra[i][c];
            out[i][c] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
    }

    /* The VP monitor point accumulates with +=, so the next block has to
     * start from silence. */
    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}

/* Hand one full buffer to waveOut, waiting (bounded) for the slot to free. */
static void waveout_submit(MCPXAPUState *d, int16_t src[][2])
{
    int idx = g_waveout.next_buf;
    WAVEHDR *hdr = &g_waveout.hdrs[idx];
    int wait_loops = 0;

    while (!(hdr->dwFlags & WHDR_DONE) && (hdr->dwFlags & WHDR_INQUEUE)) {
        qemu_mutex_unlock(&d->lock);
        Sleep(1);
        qemu_mutex_lock(&d->lock);
        if (++wait_loops > 50) break;
    }

    memcpy(g_waveout.bufs[idx], src, WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t));
    hdr->dwFlags &= ~WHDR_DONE;
    waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR));

    g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
    g_waveout.frames_written++;
}

/*
 * Called once per 32-sample EP sub-frame; every eighth call ships a 256-sample
 * block. Blocks are gathered into sink-sized buffers (1024 samples for
 * XAudio2, WAVEOUT_BUF_SAMPLES for waveOut) so the sink sees one buffer per
 * real-time interval instead of the old code's four-times-real-time bursts.
 */
void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    static int16_t acc[WAVEOUT_BUF_SAMPLES][2];
    static int acc_n = 0;
    int16_t block[MIXER_FRAME_SAMPLES][2];
    int want, i;

    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    if (xa2_is_active())
        want = xa2_get_buffer_size();
    else if (g_waveout.initialized)
        want = WAVEOUT_BUF_SAMPLES;
    else
        return;
    if (want <= 0 || want > WAVEOUT_BUF_SAMPLES)
        want = WAVEOUT_BUF_SAMPLES;

    monitor_render_block(d, block);

    for (i = 0; i < MIXER_FRAME_SAMPLES; i++) {
        acc[acc_n][0] = block[i][0];
        acc[acc_n][1] = block[i][1];
        if (++acc_n < want)
            continue;
        acc_n = 0;
        if (xa2_is_active())
            xa2_submit_samples((const int16_t *)acc, want);
        else
            waveout_submit(d, acc);
    }
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

/* XBOX_FIX_AUDIOMIX (default 1): pace the VP one 32-sample frame per
 * 0.667 ms of real time, like the hardware, instead of in bursts.
 *
 * SSX Tricky's EA mixer does not stream: it plays six looping 2,400-sample
 * buffers (one per speaker bin, voices 65..70) and rewrites them on a 10 ms
 * thread tick, keeping only 512..1,023 samples (10..21 ms) ahead of each
 * voice's play cursor. Fed "until xa2_target_queued() buffers wait", the frame
 * thread ran 8 frames at a time and several such blocks back to back whenever
 * XAudio2 drained a 10 ms quantum, so the cursor jumped up to ~0.4 ms per
 * frame -- 5..20x real time -- and overtook the mixer: the VP then played the
 * previous lap of the buffer, and every frame where it crossed back into fresh
 * data was a step in the waveform, the clicks heard with many sounds playing.
 * Measured: ~10,000 stale runs per 100 s of race, clicks on the APU
 * frame boundary (phase 0 mod 32).
 *
 * Here the frame thread runs at raised priority (the APU is hardware on the
 * console: it is never starved) and the queue only trims the pace, from a
 * smoothed level so XAudio2's quantum steps do not show: +2 % slower when
 * ahead, up to 1.5x real time to win back a preemption -- the mixer's 10..21 ms
 * lead per 10 ms tick tolerates that, an instant burst it does not. A burst is
 * kept for the start and for emergencies (queue down to a quarter of its
 * target), where an underrun would be worse. XBOX_FIX_AUDIOMIX=0: old pacing. */
static int fix_audiomix(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XBOX_FIX_AUDIOMIX");
        on = !(e && e[0] == '0');
        fprintf(stderr, "[APU] XBOX_FIX_AUDIOMIX=%d (VP paced %s)\n", on,
                on ? "one frame per 0.667 ms" : "in 8-frame bursts");
    }
    return on;
}

static void throttle_realtime(MCPXAPUState *d)
{
    static double due_us = 0.0, q_avg = -1.0;
    const double frame_us = 1000000.0 * NUM_SAMPLES_PER_FRAME / 48000.0;
    int tgt = xa2_target_queued();
    int q = xa2_queued();
    double now = (double)qemu_clock_get_us(QEMU_CLOCK_REALTIME), adj;

    if (q_avg < 0.0) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        q_avg = q;
    }
    q_avg += (q - q_avg) * (1.0 / 64.0);       /* ~43 ms at 1,500 frames/s */

    if (q * 4 <= tgt || due_us == 0.0) {
        /* start, or the queue is nearly dry: fill now, re-anchor the clock */
        due_us = now;
        if (q * 4 <= tgt) return;
    }
    /* Preempted for a while: do not repay the lost time in one burst (that
     * is the bug); drop it and let the queue level speed the pace up. */
    if (now - due_us > 2.0 * frame_us)
        due_us = now - 2.0 * frame_us;
    while (!d->pause_requested) {
        now = (double)qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        if (now >= due_us) break;
        if (xa2_queued() * 4 <= tgt) break;
        qemu_cond_timedwait(&d->cond, &d->lock, 1);
    }
    adj = (q_avg - tgt) / tgt;
    if (adj > 0.02) adj = 0.02;      /* ahead: slow down gently */
    if (adj < -0.33) adj = -0.33;    /* behind: up to 1.5x real time */
    due_us += frame_us * (1.0 + adj);
}

static void throttle(MCPXAPUState *d)
{
    if (xa2_is_active() && fix_audiomix()) {
        throttle_realtime(d);
        d->next_frame_time_us = 0;
        return;
    }

    if (d->ep_frame_div % 8) {
        return;
    }

    /* With XAudio2 live the sound device is the clock: produce until
     * xa2_target_queued() buffers (~43 ms) are waiting, then wait for it to
     * drain one. The wall-clock pacing below ran at 1,499.4 frames/s against
     * the device's 1,500, rounded every wait to whole milliseconds and threw
     * time away when behind, so the queue alternately ran dry and overflowed
     * -- the "slight noise" heard in part 179. */
    if (xa2_is_active()) {
        while (!d->pause_requested && xa2_queued() >= xa2_target_queued())
            qemu_cond_timedwait(&d->cond, &d->lock, 2);
        d->next_frame_time_us = 0;
        return;
    }

    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    if (d->next_frame_time_us == 0 ||
        now_us - d->next_frame_time_us > EP_FRAME_US) {
        d->next_frame_time_us = now_us;
    }

    while (!d->pause_requested) {
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t remaining_ms = (d->next_frame_time_us - now_us) / 1000;
        if (remaining_ms > 0) {
            qemu_cond_timedwait(&d->cond, &d->lock, (int)remaining_ms);
        } else {
            break;
        }
    }
    d->next_frame_time_us += EP_FRAME_US;

    d->sleep_acc_us += (int)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - now_us);
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    mcpx_apu_vp_frame(d, mixbins);

    /* Where does sound stop? Report, every 5 s once a voice has been mixed:
     * how many pipeline frames ran, how many voice-frames the VP mixed, and
     * the peak in mixbins 0/1 (what the DSP bypass sends on). Silence at
     * XAudio2 with a non-zero peak here is an output problem; a zero peak
     * with voices mixed is a sample-fetch or routing problem. */
    {
        static unsigned long frames = 0, last = 0;
        static float peak = 0.0f;
        int k;
        frames++;
        for (k = 0; k < NUM_SAMPLES_PER_FRAME; k++) {
            float a = mixbins[0][k] < 0 ? -mixbins[0][k] : mixbins[0][k];
            float b = mixbins[1][k] < 0 ? -mixbins[1][k] : mixbins[1][k];
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        {
            unsigned long now = (unsigned long)GetTickCount();
            if (now - last >= 5000) {
                last = now;
                fprintf(stderr, "[VP] %lu pipeline frames, %ld voice-frames mixed, "
                        "mixbin 0/1 peak %.4f; list heads 2D=%04X 3D=%04X MP=%04X, "
                        "idle-in-list %ld\n", frames, g_vp_voice_frames, peak,
                        (unsigned)d->regs[NV_PAPU_TVL2D], (unsigned)d->regs[NV_PAPU_TVL3D],
                        (unsigned)d->regs[NV_PAPU_TVLMP], g_vp_idle_seen);
                {
                    extern void apu_vp_voicelog_dump(MCPXAPUState *d);
                    apu_vp_voicelog_dump(d);
                }
                fflush(stderr);
            }
        }
    }

    mcpx_apu_dsp_frame(d, mixbins);
    mcpx_apu_monitor_frame(d);

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

extern int aci_dma_advance(unsigned samples);

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    qemu_mutex_lock(&d->lock);

    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        /* Always run the audio output loop — the software mixer and test tone
         * need continuous frame delivery regardless of APU register state.
         * The VP/DSP pipeline (se_frame) only runs when registers allow it. */
        throttle(d);

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        bool apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED);

        /* Advance the AC'97 bus-master DMA in step with the audio frame.
         * Without this the descriptor list never moves and a driver that has
         * started the bus master waits forever for a buffer completion --
         * see aci_dma.c. 32 samples is this thread's frame size. */
        aci_dma_advance(32);

        /* XBOX_BOOTDIAG=1: once a second, the APU state that decides
         * between the full pipeline and the lightweight path -- the boot hang
         * on the EA logo shows one full frame, then nothing. */
        {
            static int diag = -1;
            static unsigned long full = 0, light = 0, last = 0;
            if (diag < 0) {
                const char *e = getenv("XBOX_BOOTDIAG");
                diag = e && e[0] == '1';
            }
            if (diag) {
                unsigned long now = (unsigned long)GetTickCount();
                if (apu_active && !g_test_tone.active) full++; else light++;
                if (now - last >= 1000) {
                    last = now;
                    fprintf(stderr, "[APUDIAG] t %lu full %lu light %lu xcntmode %d fectl %08X "
                            "sectl %08X tone %d mixers %ld queued %d\n", now, full, light,
                            xcntmode, (unsigned)fectl,
                            (unsigned)qatomic_read(&d->regs[NV_PAPU_SECTL]),
                            (int)g_test_tone.active, (long)g_mixer_active_count,
                            xa2_is_active() ? xa2_queued() : -1);
                    fflush(stderr);
                }
            }
        }

        if (apu_active && !g_test_tone.active) {
            /* Full pipeline: VP voices → DSP → monitor → waveOut */
            se_frame(d);
        } else {
            /* Lightweight: just monitor frame (test tone + software mixer) */
            mcpx_apu_monitor_frame(d);
            d->ep_frame_div++;
        }
    }

    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    if (d->gp.dsp) {
        memset((void *)d->gp.dsp->core.pram_opcache, 0,
               sizeof(d->gp.dsp->core.pram_opcache));
    }
    if (d->ep.dsp) {
        memset((void *)d->ep.dsp->core.pram_opcache, 0,
               sizeof(d->ep.dsp->core.pram_opcache));
    }
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Init DSP (GP/EP - stubbed) */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Init monitor (waveOut output) */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    mcpx_apu_wait_for_idle(d);

    /* Run the frame thread from the start. xemu resumes it when the VM enters
     * RUN_STATE_RUNNING, and the pipeline's actual work is gated separately by
     * SECTL.XCNTMODE and FECTL (see mcpx_apu_frame_thread). This port has no
     * VM state change, so the only things that ever resumed it were the test
     * tone and the HLE software mixer -- and a title whose own statically
     * linked DirectSound drives the hardware (SSX Tricky) started voices on a
     * voice processor that never ran a single frame.
     *
     * RUNS BY DEFAULT since part 178; XBOX_APU_RUN=0 holds it. It was held in
     * part 177 because running it let EA's stream mixer write PCM from guest
     * address 0 across .text, the kernel thunk table and .data -- which looked
     * like a heap overlap. It was a chain of translation defects in the mixer
     * and its codecs: a jcc taking flags from the wrong block (0x184C5), a
     * collapsed x87 fadd and a missing carry in the converter (0x00019040),
     * and untranslated codec callbacks whose ICALL misses shifted the mixer's
     * stack frame by 20 bytes (0x00019950, 0x0001A990, 0x00012D30,
     * 0x00012D00). All fixed; */
    {
        const char *e = getenv("XBOX_APU_RUN");
        if (!(e && e[0] == '0'))
            mcpx_apu_resume(d);
    }
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    qemu_mutex_lock(&d->lock);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    qemu_thread_join(&d->apu_thread);
    mcpx_apu_vp_finalize(d);
    mcpx_apu_monitor_finalize(d);

    free(d);
    g_state = NULL;
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    }
    /* GP (0x30000) and EP (0x50000) regions ignored for now */
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    }
    return 0;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    if (!d) return;
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
}

/* ============================================================
 * APU Test Tone - Direct waveOut sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that waveOut
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Make sure waveOut is running - enable SECTL and resume APU thread */
    qemu_mutex_lock(&d->lock);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers to waveOut
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    g_mixer_voices[slot].active = 0;
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || v->pcm_bytes == 0) return;
    v->looping = looping;
    v->play_offset = 0;
    v->active = 1;
    InterlockedIncrement((volatile LONG *)&g_mixer_active_count);

    /* Wake up APU thread if it was paused */
    extern MCPXAPUState *g_state;
    if (g_state) {
        qemu_mutex_lock(&g_state->lock);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * play_offset is stored as a 16.16 fixed-point source frame position. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint32_t inc = (uint32_t)(((uint64_t)voice->sample_rate << 16) / 48000);
        uint32_t pos = voice->play_offset; /* 16.16 fixed-point */
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos = 0;
                    src_frame = 0;
                } else {
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = 0;
            } else if (voice->active) {
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
}
