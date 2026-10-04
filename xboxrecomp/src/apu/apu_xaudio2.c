/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   256    /* ~5.3 ms per submission; see XA2_TARGET_QUEUED */
/* The frame thread keeps XA2_TARGET_QUEUED of these filled (see xa2_queued
 * and throttle() in apu_core.c), so there is room for jitter on both sides.
 * With three, and a submit that dropped the buffer whenever all three were
 * queued, every pacing wobble became a click. */
#define XA2_NUM_BUFS      24
static unsigned long g_xa2_underruns = 0, g_xa2_drops = 0;

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static uint64_t                g_xa2_samples_sent = 0;
/* Queue latency over the current report window: samples submitted
 * and not yet played, sampled at every submit. */
static uint64_t                g_xa2_lat_sum = 0, g_xa2_lat_n = 0, g_xa2_lat_max = 0;

int xa2_target_queued(void)
{
    static int target = 0;
    if (!target) {
        const char *e = getenv("XBOX_AUDIO_QUEUE");
        int n = (e && *e) ? atoi(e) : XA2_TARGET_QUEUED;
        if (n < XA2_QUEUE_MIN) n = XA2_QUEUE_MIN;
        if (n > XA2_QUEUE_MAX) n = XA2_QUEUE_MAX;
        target = n;
        fprintf(stderr, "[XA2] queue target %d buffers (~%.1f ms)%s\n", target,
                target * XA2_BUF_SAMPLES * 1000.0 / XA2_SAMPLE_RATE,
                (e && *e) ? " from XBOX_AUDIO_QUEUE" : "");
    }
    return target;
}

int xa2_init(void)
{
    HRESULT hr;
    WAVEFORMATEX wfx = { 0 };

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != (HRESULT)0x80010106 /* RPC_E_CHANGED_MODE */ && hr != S_FALSE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }

    /* XAudio2 ships as a COM DLL and MinGW provides the header but no import
     * library, so resolve the entry point at runtime. 2.9 is present on
     * Windows 10 and later; 2.8 covers Windows 8. */
    {
        typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT32, UINT32);
        static const char *const dlls[] = { "xaudio2_9.dll", "xaudio2_8.dll" };
        PFN_XAudio2Create create = NULL;
        HMODULE lib = NULL;
        size_t i;

        for (i = 0; i < sizeof(dlls) / sizeof(dlls[0]) && !create; i++) {
            lib = LoadLibraryA(dlls[i]);
            if (!lib) continue;
            create = (PFN_XAudio2Create)(void *)GetProcAddress(lib, "XAudio2Create");
            if (!create) { FreeLibrary(lib); lib = NULL; }
        }
        if (!create) {
            fprintf(stderr, "[XA2] no XAudio2 runtime found -- audio output disabled\n");
            return 0;
        }
        hr = create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    }
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        return 0;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
        return 0;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
        return 0;
    }

    IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    g_xa2_samples_sent = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_xa2_initialized) return;

    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Buffers the device still has to play -- the clock the APU frame thread
 * paces itself by. */
int xa2_queued(void)
{
    XAUDIO2_VOICE_STATE state;
    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (int)state.BuffersQueued;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, 0);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) { g_xa2_drops++; return 0; }
    if (g_xa2_samples_sent >= state.SamplesPlayed) {
        uint64_t q = g_xa2_samples_sent - state.SamplesPlayed;
        g_xa2_lat_sum += q;
        g_xa2_lat_n++;
        if (q > g_xa2_lat_max) g_xa2_lat_max = q;
    }
    if (state.BuffersQueued == 0 && g_xa2_frames_written > XA2_NUM_BUFS)
        g_xa2_underruns++;          /* the device ran dry before this arrived */

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);

    {   /* XBOX_AUDIO_WAV=<path> (diagnostic): append exactly what is
         * submitted to a 48 kHz 16-bit WAV, header patched as it grows, so a
         * run's sound can be inspected rather than described. */
        static FILE *wf = NULL;
        static int init = 0;
        static uint32_t bytes = 0;
        if (!init) {
            const char *p = getenv("XBOX_AUDIO_WAV");
            init = 1;
            if (p && *p) {
                wf = fopen(p, "wb");
                if (wf) {
                    uint8_t h[44] = {0};
                    fwrite(h, 1, 44, wf);
                }
            }
        }
        if (wf) {
            uint32_t n = (uint32_t)xbuf.AudioBytes;
            fwrite(xbuf.pAudioData, 1, n, wf);
            bytes += n;
            if ((g_xa2_frames_written & 31) == 0) {
                long pos = ftell(wf);
                uint32_t rate = 48000, byterate = 48000 * XA2_CHANNELS * 2, v;
                uint16_t s;
                fseek(wf, 0, SEEK_SET);
                fwrite("RIFF", 1, 4, wf); v = 36 + bytes; fwrite(&v, 4, 1, wf);
                fwrite("WAVEfmt ", 1, 8, wf); v = 16; fwrite(&v, 4, 1, wf);
                s = 1; fwrite(&s, 2, 1, wf); s = XA2_CHANNELS; fwrite(&s, 2, 1, wf);
                fwrite(&rate, 4, 1, wf); fwrite(&byterate, 4, 1, wf);
                s = XA2_CHANNELS * 2; fwrite(&s, 2, 1, wf); s = 16; fwrite(&s, 2, 1, wf);
                fwrite("data", 1, 4, wf); fwrite(&bytes, 4, 1, wf);
                fseek(wf, pos, SEEK_SET);
                fflush(wf);
            }
        }
    }

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    g_xa2_samples_sent += (uint64_t)copy_samples;

    /* Say whether what reaches the speakers is sound or silence. A buffer
     * count alone cannot tell -- the pipeline submits zeros whenever no voice
     * is playing, so "frames written" rises either way. Reported every 5 s
     * while anything non-silent has been heard, and once when output first
     * stops being silent. */
    {
        static int peak = 0, announced = 0;
        static unsigned long loud = 0, last_tick = 0;
        int k, bpeak = 0;
        for (k = 0; k < copy_samples * XA2_CHANNELS; k++) {
            int v = samples[k] < 0 ? -samples[k] : samples[k];
            if (v > bpeak) bpeak = v;
        }
        if (bpeak > 64) loud++;
        if (bpeak > peak) peak = bpeak;
        if (loud && !announced) {
            announced = 1;
            fprintf(stderr, "[XA2] first non-silent buffer (peak %d) after %d buffers\n",
                    bpeak, g_xa2_frames_written);
            fflush(stderr);
        }
        if (loud) {
            unsigned long now = GetTickCount();
            if (now - last_tick >= 5000) {
                last_tick = now;
                XAUDIO2_PERFORMANCE_DATA perf;
                memset(&perf, 0, sizeof(perf));
                IXAudio2_GetPerformanceData(g_xa2, &perf);
                fprintf(stderr, "[XA2] %d buffers sent, %lu non-silent, peak %d, "
                        "%lu underruns, %lu dropped\n",
                        g_xa2_frames_written, loud, peak, g_xa2_underruns, g_xa2_drops);
                /* Queue target, measured queue latency over the last
                 * window (mean / max, submitted but unplayed), the engine's
                 * own device latency and its glitch count. */
                fprintf(stderr, "[XA2] queue %d: latency mean %.1f ms max %.1f ms, "
                        "device %.1f ms, glitches %u\n", xa2_target_queued(),
                        g_xa2_lat_n ? (double)g_xa2_lat_sum / (double)g_xa2_lat_n * 1000.0 / XA2_SAMPLE_RATE : 0.0,
                        (double)g_xa2_lat_max * 1000.0 / XA2_SAMPLE_RATE,
                        perf.CurrentLatencyInSamples * 1000.0 / XA2_SAMPLE_RATE,
                        (unsigned)perf.GlitchesSinceEngineStarted);
                g_xa2_lat_sum = g_xa2_lat_n = g_xa2_lat_max = 0;
                fflush(stderr);
            }
        }
    }
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- POSIX stubs (no audio output yet) */

int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
int  xa2_get_buffer_size(void)                        { return 0; }
int  xa2_target_queued(void)                          { return XA2_TARGET_QUEUED; }

#endif /* _WIN32 */
