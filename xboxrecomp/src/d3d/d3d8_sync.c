/*
 * d3d8_sync.c -- presentation sync, see d3d8_sync.h.
 */
#define COBJMACROS
#include "d3d8_sync.h"
#include <dxgi1_3.h>
#include <dxgi1_5.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int    s_mode = -1;
static int    s_flip;                   /* chain created with the flip model */
static int    s_tearing;                /* ALLOW_TEARING accepted by the system */
static int    s_latency = 2;
static int    s_legacy_vsync;           /* XBOX_VSYNC=1 (legacy) */
static UINT   s_flags;                  /* chain creation flags */
static HANDLE s_wait;                   /* latency object (flip model) */
static HWND   s_hwnd;
static volatile LONG s_hz;              /* refresh rate of the window's display */
static double s_qf;

static const char *const k_mode_name[] = { "legacy", "off", "vsync", "adaptive" };

static double now_ms(void)
{
    LARGE_INTEGER t;
    if (!s_qf) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_qf = (double)f.QuadPart; }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / s_qf;
}

int d3d8_sync_mode(void)
{
    if (s_mode < 0) {
        const char *e = getenv("XBOX_SYNC");
        int i;
        s_mode = D3D8_SYNC_LEGACY;
        if (e && *e) {
            for (i = 0; i < 4; i++)
                if (!_stricmp(e, k_mode_name[i])) s_mode = i;
            if (s_mode == D3D8_SYNC_LEGACY && _stricmp(e, "legacy"))
                fprintf(stderr, "D3D8: XBOX_SYNC=%s unknown (legacy | off | vsync | adaptive): legacy\n", e);
        }
        e = getenv("XBOX_SYNC_LATENCY");
        if (e && *e) {
            int v = atoi(e);
            s_latency = v < 1 ? 1 : v > 3 ? 3 : v;
        }
        e = getenv("XBOX_VSYNC");
        s_legacy_vsync = e && e[0] == '1';
    }
    return s_mode;
}

int d3d8_monitor_hz(HWND hwnd)
{
    MONITORINFOEXW mi;
    DEVMODEW dm;
    HMONITOR m;
    POINT o = { 0, 0 };
    m = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
             : MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!m || !GetMonitorInfoW(m, (MONITORINFO *)&mi)) return 0;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return dm.dmDisplayFrequency > 1 ? (int)dm.dmDisplayFrequency : 0;   /* 0 / 1 = hardware default */
}

/* ALLOW_TEARING: Windows 10 1607+ and a driver that allows it. */
static int tearing_supported(void)
{
    typedef HRESULT (WINAPI *CreateFactory1)(REFIID, void **);
    static const GUID iid_f1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8,0x29, 0x25,0x3c,0x83,0xd1,0xb3,0x87 } };
    static const GUID iid_f5 = { 0x7632e1f5, 0xee65, 0x4dca, { 0x87,0xfd, 0x84,0xcd,0x75,0xf8,0x83,0x8d } };
    HMODULE dx = GetModuleHandleA("dxgi.dll");
    CreateFactory1 create;
    IDXGIFactory1 *f1 = NULL;
    IDXGIFactory5 *f5 = NULL;
    BOOL ok = FALSE;
    if (!dx) dx = LoadLibraryA("dxgi.dll");
    create = dx ? (CreateFactory1)(void *)GetProcAddress(dx, "CreateDXGIFactory1") : NULL;
    if (!create || FAILED(create(&iid_f1, (void **)&f1)) || !f1) return 0;
    if (SUCCEEDED(IDXGIFactory1_QueryInterface(f1, &iid_f5, (void **)&f5)) && f5) {
        if (FAILED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &ok, sizeof ok)))
            ok = FALSE;
        IDXGIFactory5_Release(f5);
    }
    IDXGIFactory1_Release(f1);
    return ok ? 1 : 0;
}

void d3d8_sync_swap_desc(DXGI_SWAP_CHAIN_DESC *scd)
{
    if (d3d8_sync_mode() == D3D8_SYNC_LEGACY) {
        scd->SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        scd->Flags = 0;
        s_flip = 0;
        s_flags = 0;
        return;
    }
    /* Flip model: 2 buffers at least, no multisampling (already the case:
     * anti-aliasing is done in the scene target). */
    s_tearing = (s_mode == D3D8_SYNC_OFF || s_mode == D3D8_SYNC_ADAPTIVE) && tearing_supported();
    scd->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd->BufferCount = 3;
    scd->SampleDesc.Count = 1;
    scd->SampleDesc.Quality = 0;
    scd->Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
               | (s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    s_flip = 1;
    s_flags = scd->Flags;
}

void d3d8_sync_fallback(DXGI_SWAP_CHAIN_DESC *scd, HRESULT why)
{
    fprintf(stderr, "D3D8: flip model chain refused (0x%08lX): back to the original presentation\n",
            (unsigned long)why);
    s_mode = D3D8_SYNC_LEGACY;
    scd->BufferCount = 1;
    d3d8_sync_swap_desc(scd);
}

UINT d3d8_sync_swap_flags(void) { return s_flags; }

void d3d8_sync_window_changed(void)
{
    if (s_hwnd) InterlockedExchange(&s_hz, d3d8_monitor_hz(s_hwnd));
}

/* ── mesures (XBOX_PACING_LOG / XBOX_PACING_CSV) ─────────────────── */

#define HBIN    0.05                    /* ms per bin of the global histogram */
#define HMAX    5000                    /* 250 ms */
#define WMAX    16384                   /* intervals of a 10 s window */
static struct {
    int    on;
    FILE  *csv;
    double t0, last, win_t0;
    /* global */
    unsigned long long n, slow, tears, waits_to;
    double sum, sum2, wait_sum, max;
    unsigned hist[HMAX + 1];
    /* window */
    double w[WMAX];
    unsigned wn;
    /* display (flip model): refreshes between two new frames */
    UINT pc0, pr0;
    unsigned long long refr[5], multi, stats_err;
} M;

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double hist_pct(double p)
{
    unsigned long long k = 0, want = (unsigned long long)(p * (double)M.n);
    int i;
    for (i = 0; i <= HMAX; i++) {
        k += M.hist[i];
        if (k > want) return (i + 0.5) * HBIN;
    }
    return HMAX * HBIN;
}

static void report_global(const char *when)
{
    double mean, sd;
    unsigned long long shown;
    if (!M.n) return;
    mean = M.sum / (double)M.n;
    sd = M.sum2 / (double)M.n - mean * mean;
    sd = sd > 0 ? sqrt(sd) : 0.0;
    shown = M.refr[1] + M.refr[2] + M.refr[3] + M.refr[4];
    fprintf(stderr, "[PACING] %s: mode %s%s, display %ld Hz, %llu frames, mean interval %.2f ms "
            "(%.1f fps), std dev %.2f, p50 %.2f, p95 %.2f, p99 %.2f, max %.1f; "
            "> 1.5x mean: %llu (%.2f %%); latency wait %.2f ms/frame (%llu timeouts); adaptive tears %llu\n",
            when, k_mode_name[s_mode < 0 ? 0 : s_mode], s_tearing ? "+tearing" : "", (long)s_hz,
            M.n, mean, mean > 0 ? 1000.0 / mean : 0.0, sd, hist_pct(0.50), hist_pct(0.95),
            hist_pct(0.99), M.max, M.slow, 100.0 * (double)M.slow / (double)M.n,
            M.wait_sum / (double)M.n, M.waits_to, M.tears);
    if (shown)
        fprintf(stderr, "[PACING] %s: display (DXGI statistics): new frame after 1 refresh %llu, "
                "2: %llu, 3: %llu, 4+: %llu; several frames between two readings %llu; failed readings %llu\n",
                when, M.refr[1], M.refr[2], M.refr[3], M.refr[4], M.multi, M.stats_err);
}

static void report_window(void)
{
    double mean = 0, sd = 0, p50, p99;
    unsigned i, n = M.wn;
    if (n < 2) return;
    for (i = 0; i < n; i++) mean += M.w[i];
    mean /= n;
    for (i = 0; i < n; i++) sd += (M.w[i] - mean) * (M.w[i] - mean);
    sd = sqrt(sd / n);
    qsort(M.w, n, sizeof M.w[0], cmp_d);
    p50 = M.w[n / 2];
    p99 = M.w[(unsigned)(0.99 * (n - 1))];
    fprintf(stderr, "[PACING] t=%.0f s: %u frames, %.1f fps, mean interval %.2f ms, std dev %.2f, p50 %.2f, p99 %.2f, max %.1f\n",
            (M.last - M.t0) / 1000.0, n, 1000.0 / mean, mean, sd, p50, p99, M.w[n - 1]);
}

static void pacing_atexit(void) { d3d8_sync_report(); }

void d3d8_sync_report(void)
{
    static LONG done;
    if (!M.on || InterlockedExchange(&done, 1)) return;
    report_global("summary");
    if (M.csv) fflush(M.csv);
}

static void pacing_init(void)
{
    const char *e = getenv("XBOX_PACING_LOG"), *c = getenv("XBOX_PACING_CSV");
    M.on = (e && e[0] == '1') || (c && *c);
    if (!M.on) return;
    if (c && *c) {
        M.csv = fopen(c, "w");
        if (M.csv) fprintf(M.csv, "# t_ms,itv_ms,wait_ms,present_ms,sync,flags,present_count,present_refresh,sync_refresh,sync_qpc_ms,hz\n");
    }
    M.t0 = M.win_t0 = now_ms();
    atexit(pacing_atexit);
}

static void pacing_note(IDXGISwapChain *sc, double t_end, double wait_ms, double pres_ms, UINT sync, UINT flags)
{
    DXGI_FRAME_STATISTICS fs;
    int have = 0;
    double itv;
    if (M.last > 0) {
        int b;
        itv = t_end - M.last;
        M.n++;
        M.sum += itv; M.sum2 += itv * itv;
        M.wait_sum += wait_ms;
        if (itv > M.max) M.max = itv;
        b = (int)(itv / HBIN);
        M.hist[b < 0 ? 0 : b > HMAX ? HMAX : b]++;
        if (M.n > 30 && itv > 1.5 * (M.sum / (double)M.n)) M.slow++;
        if (M.wn < WMAX) M.w[M.wn++] = itv;
    } else itv = 0.0;
    M.last = t_end;
    memset(&fs, 0, sizeof fs);
    if (s_flip) {
        HRESULT hr = IDXGISwapChain_GetFrameStatistics(sc, &fs);
        if (SUCCEEDED(hr)) {
            have = 1;
            if (M.pc0 && fs.PresentCount != M.pc0) {
                UINT dp = fs.PresentCount - M.pc0, dr = fs.PresentRefreshCount - M.pr0;
                if (dp == 1) M.refr[dr >= 4 ? 4 : dr]++;
                else M.multi++;
            }
            if (fs.PresentCount != M.pc0) { M.pc0 = fs.PresentCount; M.pr0 = fs.PresentRefreshCount; }
        } else M.stats_err++;
    }
    if (M.csv)
        fprintf(M.csv, "%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%.3f,%ld\n", t_end - M.t0, itv, wait_ms, pres_ms,
                sync, flags, have ? fs.PresentCount : 0, have ? fs.PresentRefreshCount : 0,
                have ? fs.SyncRefreshCount : 0,
                have && fs.SyncQPCTime.QuadPart ? (double)fs.SyncQPCTime.QuadPart * 1000.0 / s_qf - M.t0 : 0.0,
                (long)s_hz);
    if (t_end - M.win_t0 >= 10000.0) {
        if (getenv("XBOX_PACING_LOG")) report_window();
        if (M.csv) fflush(M.csv);
        M.wn = 0;
        M.win_t0 = t_end;
    }
}

void d3d8_sync_created(IDXGISwapChain *sc, HWND hwnd)
{
    s_hwnd = hwnd;
    InterlockedExchange(&s_hz, d3d8_monitor_hz(hwnd));
    if (s_flip) {
        static const GUID iid_sc2 = { 0xa8be2ac4, 0x199f, 0x4946, { 0xb3,0x31, 0x79,0x59,0x9f,0xb9,0x8d,0xe7 } };
        IDXGISwapChain2 *sc2 = NULL;
        if (SUCCEEDED(IDXGISwapChain_QueryInterface(sc, &iid_sc2, (void **)&sc2)) && sc2) {
            IDXGISwapChain2_SetMaximumFrameLatency(sc2, (UINT)s_latency);
            s_wait = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
            IDXGISwapChain2_Release(sc2);
        }
    }
    now_ms();
    pacing_init();
    fprintf(stderr, "D3D8: presentation %s (%s%s), display %ld Hz%s\n", k_mode_name[s_mode],
            s_flip ? "flip model, 3 buffers" : "DXGI_SWAP_EFFECT_DISCARD",
            s_flip ? (s_tearing ? ", tearing allowed" : ", no tearing") : (s_legacy_vsync ? ", XBOX_VSYNC=1" : ""),
            (long)s_hz, s_wait ? "" : (s_flip ? ", no latency object" : ""));
    if (s_flip && s_wait) fprintf(stderr, "D3D8: presentation queue: %d frame(s) at most\n", s_latency);
}

HRESULT d3d8_sync_present(IDXGISwapChain *sc, double *dxgi_ms)
{
    static double last_arrive, arrive_avg;
    double t0, t1, t2, wait_ms = 0.0;
    UINT sync, flags = 0;
    HRESULT hr;

    t0 = now_ms();
    if (s_wait) {
        /* A frame may enter the queue. Bounded timeout: a hidden window or a
         * display turned off must not freeze the presenting thread. */
        if (WaitForSingleObjectEx(s_wait, 100, TRUE) == WAIT_TIMEOUT) M.waits_to++;
    }
    t1 = now_ms();
    wait_ms = t1 - t0;
    switch (s_mode) {
    case D3D8_SYNC_OFF:
        sync = 0;
        flags = s_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        break;
    case D3D8_SYNC_VSYNC:
        sync = 1;
        break;
    case D3D8_SYNC_ADAPTIVE: {
        long hz = s_hz > 0 ? s_hz : 60;
        double period = 1000.0 / (double)hz;
        /* Arrival pace of the frames (running average over ~8, from the
         * arrival at Present, without our wait): below the refresh rate, a
         * late frame is shown at once (tearing) rather than at the next
         * refresh. Above it, VSync. */
        double d = last_arrive > 0 ? t0 - last_arrive : period;
        arrive_avg = arrive_avg > 0 ? arrive_avg * 0.875 + d * 0.125 : d;
        if (arrive_avg > 1.05 * period && d > period) {
            sync = 0;
            flags = s_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
            M.tears++;
        } else sync = 1;
        break;
    }
    default:
        sync = s_legacy_vsync ? 1 : 0;
        break;
    }
    hr = IDXGISwapChain_Present(sc, sync, flags);
    t2 = now_ms();
    last_arrive = t0;
    if (dxgi_ms) *dxgi_ms = t2 - t1;
    if (M.on) pacing_note(sc, t2, wait_ms, t2 - t1, sync, flags);
    return hr;
}

void d3d8_sync_release(void)
{
    if (s_wait) { CloseHandle(s_wait); s_wait = NULL; }
}
