/*
 * d3d8_sync.c -- presentation sync, see d3d8_sync.h.
 */
#define COBJMACROS
#include "d3d8_sync.h"
#include "../kernel/xbox_logstats.h"
void sc_ForgetAll(void);                /* d3d8_statecache.c */
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
static void (*s_before_present)(void);  /* O-B: see d3d8_sync_set_before_present */
static HWND   s_hwnd;
static volatile LONG s_hz;              /* refresh rate of the window's display */
static volatile LONG s_pace_fps;        /* frame rate limit (0: none), see d3d8_sync_set_pace */
static volatile LONG s_adaptive_tears;  /* Adaptive: the game is slower than its pace, late frames shown at once */
/* How the frames reach the display, from the media statistics of the frames
 * the display took (presentation_mode_note): a DXGI_FRAME_PRESENTATION_MODE,
 * -1 unknown. The presenting thread writes it, any thread reads it. */
static volatile LONG s_comp_mode = -1;
static volatile LONG s_comp_again;      /* any thread: something changed, read it again from the frames presented after */
static struct {                         /* presenting thread only */
    IDXGISwapChainMedia *m;             /* the chain's media statistics (queried once; refused: again 2 s later) */
    double read, qi_retry;              /* last reading; no media interface: next try (ms) */
    UINT mark, seen, lpc;               /* present counts: when a change is taken (frames presented before count as
                                           before it); of the last reading; at it */
    int marked;
    double changed;                     /* when the last change was taken (ms) */
    double found;                       /* how long after it the mode was known (ms, logged; < 0: none) */
    double us;                          /* time spent reading (logged) */
    unsigned long n;
    double logged;                      /* last line written (ms): one a second at most */
    unsigned long skipped;              /* changes not written since */
    int pending;                        /* a change not written yet */
} CM = { .found = -1.0 };
static double s_qf;
/* Render gate, see d3d8_sync_gate. */
static int    s_gate;                   /* XBOX_RENDER_GATE: frames allowed ahead of a render (0: off) */
static volatile LONG s_gate_live;       /* the retire thread runs: the pump no longer waits */
static volatile LONG s_presented, s_retired, s_qadj, s_gate_stop;
static HANDLE s_gate_thread;
static unsigned long long s_gate_resync, s_gate_dn;
static long long s_gate_dsum;           /* statistics' queue - counted queue, summed (diagnostic) */
/* Present grid, see pgrid_wait. */
static struct { int on; double margin, anchor, last_slot; unsigned long long held, late, restart; } G;
static void (*s_fc_shift)(double ms);   /* the frame timer is on an exact grid (port, frame_clock.c); moves it */

void d3d8_sync_set_frame_clock(void (*shift)(double ms)) { s_fc_shift = shift; }
static void pgrid_init(void);
static void pgrid_wait(IDXGISwapChain *sc, double t, int video);

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

void d3d8_sync_set_pace(int fps)
{
    InterlockedExchange(&s_pace_fps, fps > 0 && fps <= 1000 ? fps : 0);
}

/* Whether the frame of the render about to start will reach the screen as
 * soon as it is presented (fps_cap: its image then shows the render's start,
 * else its slot). That takes:
 * - a sync interval of 0: VSync off; Adaptive gives it to the frames that come
 *   late while its running average says the game is slower than its pace, and
 *   this answers for every render of that phase (one that comes on time waits
 *   for its refresh, but then its start is its slot);
 * - tearing allowed (ALLOW_TEARING): without it a frame waits for the next
 *   refresh all the same, the newest one being shown;
 * - the frames flipped on their own, which the media statistics call an
 *   overlay plane (an independent flip, too): measured in full screen, they
 *   then reach the screen at every point of the refresh. A frame the desktop
 *   composes waits for its next refresh.
 * While the way the frames reach the display is unknown (no statistics yet, a
 * change they do not show yet, frames presented and none taken since the last
 * reading) or not an overlay plane (a mode they do not specify, a composition
 * failure: known for the reading's pace, waiting for this answer), a frame
 * counts as waiting. The legacy chain (DISCARD) gives no statistics: the desktop
 * composes it on Windows 10, but Windows 11 may flip it and let it tear
 * ("Optimizations for windowed games"); not knowing, it counts as waiting. */
int d3d8_sync_immediate(void)
{
    switch (d3d8_sync_mode()) {
    case D3D8_SYNC_OFF:      break;
    case D3D8_SYNC_ADAPTIVE: if (!s_adaptive_tears) return 0; break;
    default:                 return 0;
    }
    return s_tearing && s_comp_mode == DXGI_FRAME_PRESENTATION_MODE_OVERLAY;
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

/* ── exact refresh rate ────────────────────────────────────────────
 * Windows lists a display's rate as an integer: 240 for a signal at
 * 239.98 Hz, 60 (or 59) for 59.94. A frame rate limit set to that integer
 * produced frames slightly faster than the display: the DXGI queue filled
 * up and stayed full (+2.5 to +3 ms between the start of a render and the
 * screen at 240 Hz). The exact rate is the signal's vertical sync
 * frequency, a rational, from QueryDisplayConfig (Windows 7+; Wine has it
 * too). Not found, or far from the integer rate: the integer rate. */
static double qdc_rate(const WCHAR *dev)
{
    DISPLAYCONFIG_PATH_INFO *p = NULL;
    DISPLAYCONFIG_MODE_INFO *mo = NULL;
    UINT32 np = 0, nm = 0, i;
    double r = 0.0;
    int tries;
    for (tries = 0; tries < 3; tries++) {       /* the layout may change between the two calls */
        LONG e;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return 0.0;
        p = (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof *p);
        mo = (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof *mo);
        if (!p || !mo) { free(p); free(mo); return 0.0; }
        e = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, p, &nm, mo, NULL);
        if (e == ERROR_SUCCESS) break;
        free(p); free(mo); p = NULL; mo = NULL;
        if (e != ERROR_INSUFFICIENT_BUFFER) return 0.0;
    }
    if (!p) return 0.0;
    for (i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
        DISPLAYCONFIG_RATIONAL q;
        UINT32 k;
        memset(&sn, 0, sizeof sn);
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof sn;
        sn.header.adapterId = p[i].sourceInfo.adapterId;
        sn.header.id = p[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, dev)) continue;
        /* the signal's timing first (exact), else the path's rate */
        q = p[i].targetInfo.refreshRate;
        k = p[i].targetInfo.modeInfoIdx;
        if (k != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && k < nm && mo[k].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET
            && mo[k].targetMode.targetVideoSignalInfo.vSyncFreq.Numerator
            && mo[k].targetMode.targetVideoSignalInfo.vSyncFreq.Denominator)
            q = mo[k].targetMode.targetVideoSignalInfo.vSyncFreq;
        if (q.Numerator && q.Denominator) r = (double)q.Numerator / (double)q.Denominator;
        break;
    }
    free(p);
    free(mo);
    return r;
}

double d3d8_monitor_rate(HWND hwnd, const char **src)
{
    MONITORINFOEXW mi;
    DEVMODEW dm;
    HMONITOR m;
    POINT o = { 0, 0 };
    const char *e = NULL;
    double hz = 0.0, r;
    if (src) *src = "unknown";
    if (e && *e) {                              /* test: a rate, the display untouched */
        /* "A" or "A@T:B" (B from T seconds after the first read on: a mode
         * change, read again on the next WM_DISPLAYCHANGE); A, B = Hz or N/D */
        static double t_first;
        char *end;
        double v = strtod(e, &end), now = now_ms();
        if (*end == '/') { double d = strtod(end + 1, &end); v = d > 0.0 ? v / d : 0.0; }
        if (t_first <= 0.0) t_first = now;
        if (*end == '@') {
            double t = strtod(end + 1, &end);
            if (*end == ':' && now - t_first >= t * 1000.0) {
                v = strtod(end + 1, &end);
                if (*end == '/') { double d = strtod(end + 1, &end); v = d > 0.0 ? v / d : 0.0; }
            }
        }
        if (v >= 24.0 && v <= 1000.0) { if (src) *src = "XBOX_TEST_DISPLAY_RATE (test)"; return v; }
    }
    m = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
             : MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!m || !GetMonitorInfoW(m, (MONITORINFO *)&mi)) return 0.0;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        hz = (double)dm.dmDisplayFrequency;
        if (src) *src = "integer rate";
    }
    r = qdc_rate(mi.szDevice);
    /* the integer is rounded (239.98 -> 240) or cut (59.94 -> 59): within 1.5 Hz */
    if (r >= 24.0 && r <= 1000.0 && (hz <= 0.0 || fabs(r - hz) <= 1.5)) {
        if (src) *src = "display signal";
        return r;
    }
    return hz;
}

/* The exact rate of the window's display, as last read (mHz), and a count
 * of its changes: the frame rate limit follows it (fps_cap.c). */
static volatile LONG s_rate_mhz, s_rate_gen;
static volatile LONG_PTR s_rate_mon;            /* HMONITOR it was read on */

static void rate_update(HWND hwnd, const char *why)
{
    const char *src;
    double r = d3d8_monitor_rate(hwnd, &src);
    LONG v = (LONG)(r * 1000.0 + 0.5);
    HMONITOR m = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : NULL;
    InterlockedExchangePointer((PVOID volatile *)&s_rate_mon, (PVOID)m);
    if (v != s_rate_mhz) {
        InterlockedExchange(&s_rate_mhz, v);
        InterlockedIncrement(&s_rate_gen);
        fprintf(stderr, "D3D8: display refresh %.3f Hz (%s; %s)\n", r, src, why);
        fflush(stderr);
    }
}

double d3d8_display_rate(LONG *gen)
{
    if (gen) *gen = s_rate_gen;
    return (double)s_rate_mhz / 1000.0;
}

void d3d8_sync_display_changed(int moved)
{
    HWND h = s_hwnd;
    if (!h) return;
    /* WM_MOVE: only when the window has gone over to another display */
    if (moved && (LONG_PTR)MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) == s_rate_mon) return;
    InterlockedExchange(&s_comp_again, 1);      /* how the frames reach this display: read again */
    InterlockedExchange(&s_hz, d3d8_monitor_hz(h));
    rate_update(h, moved ? "window on another display" : "display mode changed");
}

/* ── XBOX_TEST_DISPLAY_HZ (test only, see d3d8_sync.h) ───────────── */
static volatile LONG s_test_hz_on;       /* 1 while our mode is set */
static WCHAR s_test_hz_dev[CCHDEVICENAME];

void d3d8_test_display_hz_restore(void)
{
    LONG r;
    if (!InterlockedExchange(&s_test_hz_on, 0)) return;
    /* NULL mode, no flags: back to the mode saved in the registry (the desktop's). */
    r = ChangeDisplaySettingsExW(s_test_hz_dev, NULL, NULL, 0, NULL);
    fprintf(stderr, "D3D8: XBOX_TEST_DISPLAY_HZ: desktop mode restored (%ld)\n", (long)r);
    fflush(stderr);
}

static void test_display_hz_atexit(void) { d3d8_test_display_hz_restore(); }

void d3d8_test_display_hz_apply(void)
{
    const char *e = NULL;                               /* never the desktop's mode */
    MONITORINFOEXW mi;
    DEVMODEW cur, dm;
    POINT o = { 0, 0 };
    HMONITOR m;
    long hz;
    LONG r;
    static int done;
    if (done || !e || !*e) return;
    done = 1;
    hz = strtol(e, NULL, 10);
    if (hz < 24 || hz > 1000) { fprintf(stderr, "D3D8: XBOX_TEST_DISPLAY_HZ=%s invalid: ignored\n", e); return; }
    m = MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);   /* the game window opens there (fps_cap_init) */
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    memset(&cur, 0, sizeof cur);
    cur.dmSize = sizeof cur;
    if (!m || !GetMonitorInfoW(m, (MONITORINFO *)&mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) {
        fprintf(stderr, "D3D8: XBOX_TEST_DISPLAY_HZ: display not found: ignored\n");
        return;
    }
    if ((long)cur.dmDisplayFrequency == hz) {
        fprintf(stderr, "D3D8: XBOX_TEST_DISPLAY_HZ=%ld: display already at %ld Hz\n", hz, hz);
        return;
    }
    dm = cur;
    dm.dmDisplayFrequency = (DWORD)hz;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    r = ChangeDisplaySettingsExW(mi.szDevice, &dm, NULL, CDS_TEST, NULL);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        fprintf(stderr, "D3D8: XBOX_TEST_DISPLAY_HZ=%ld: %lux%lu at %ld Hz refused by the display (%ld): ignored\n",
                hz, cur.dmPelsWidth, cur.dmPelsHeight, hz, (long)r);
        return;
    }
    memcpy(s_test_hz_dev, mi.szDevice, sizeof s_test_hz_dev);
    /* CDS_FULLSCREEN: a temporary mode, never written to the registry. */
    r = ChangeDisplaySettingsExW(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        fprintf(stderr, "D3D8: XBOX_TEST_DISPLAY_HZ=%ld: mode change failed (%ld): ignored\n", hz, (long)r);
        return;
    }
    InterlockedExchange(&s_test_hz_on, 1);
    atexit(test_display_hz_atexit);
    fprintf(stderr, "D3D8: XBOX_TEST_DISPLAY_HZ (TEST): %lux%lu %lu Hz -> %ld Hz until the game exits\n",
            cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency, hz);
    fflush(stderr);
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
    /* window <-> full screen: how the frames reach the display is read again */
    InterlockedExchange(&s_comp_again, 1);
    if (!s_hwnd) return;
    InterlockedExchange(&s_hz, d3d8_monitor_hz(s_hwnd));
    rate_update(s_hwnd, "window resized");
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

static void gpu_fence_summary(void);

void d3d8_sync_report(void)
{
    static LONG done;
    gpu_fence_summary();
    if (!M.on || InterlockedExchange(&done, 1)) return;
    report_global("summary");
    if (G.on)
        fprintf(stderr, "D3D8: present grid: %llu frames held to their slot, %llu late, %llu restarts\n",
                G.held, G.late, G.restart);
    if (M.csv) fflush(M.csv);
}

static void pacing_init(void)
{
    const char *e = getenv("XBOX_PACING_LOG"), *c = getenv("XBOX_PACING_CSV");
    M.on = (e && e[0] == '1') || (c && *c);
    if (!M.on) return;
    if (c && *c) {
        M.csv = fopen(c, "w");
        if (M.csv) fprintf(M.csv, "# t_ms,itv_ms,wait_ms,present_ms,sync,flags,present_count,present_refresh,sync_refresh,sync_qpc_ms,hz,last_present\n");
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
    if (M.csv) {
        /* last_present: the PresentCount DXGI gave this very Present, the
         * same key as present_count in the frame statistics: each row's frame is
         * then found on screen exactly (no index offset to guess). */
        UINT lpc = 0;
        if (s_flip && FAILED(IDXGISwapChain_GetLastPresentCount(sc, &lpc))) lpc = 0;
        fprintf(M.csv, "%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%.3f,%ld,%u\n", t_end - M.t0, itv, wait_ms, pres_ms,
                sync, flags, have ? fs.PresentCount : 0, have ? fs.PresentRefreshCount : 0,
                have ? fs.SyncRefreshCount : 0,
                have && fs.SyncQPCTime.QuadPart ? (double)fs.SyncQPCTime.QuadPart * 1000.0 / s_qf - M.t0 : 0.0,
                (long)s_hz, lpc);
    }
    if (t_end - M.win_t0 >= 10000.0) {
        if (getenv("XBOX_PACING_LOG")) report_window();
        if (M.csv) fflush(M.csv);
        M.wn = 0;
        M.win_t0 = t_end;
    }
}

/* ── render gate (XBOX_RENDER_GATE=1|2, off by default) ──────────────
 * Without it, the pump waits on the latency object just before Present:
 * when the DXGI queue is full, the game has already started (and frozen
 * the content of) the next frame, which then waits in the queue. With it,
 * the game's thread waits before it starts a render (fps_cap.c,
 * render_gate) until at most N frames are still on their way to the
 * screen (in the push buffer + in the DXGI queue): the content is taken as
 * late as the screen allows. The pump no longer waits; a thread takes the
 * latency object's signals instead (one per frame that left the queue),
 * and the count is checked against the DXGI frame statistics after every
 * Present (a lost signal must not tighten the gate for good). */
static DWORD WINAPI gate_retire_thread(LPVOID arg)
{
    HANDLE w = (HANDLE)arg;
    while (!s_gate_stop)
        if (WaitForSingleObjectEx(w, 50, FALSE) == WAIT_OBJECT_0) InterlockedIncrement(&s_retired);
    return 0;
}

int d3d8_sync_gate(void) { return s_gate_live ? s_gate : 0; }

/* Frames that left the queue so far (the latency object's signals). */
long d3d8_sync_retired(void) { return s_retired; }

void d3d8_sync_gate_stats(double *stats_minus_count, unsigned long long *resync)
{
    *stats_minus_count = s_gate_dn ? (double)s_gate_dsum / (double)s_gate_dn : 0.0;
    *resync = s_gate_resync;
}

int d3d8_sync_queued(void)
{
    LONG q = s_presented + s_qadj - s_retired + s_latency;   /* the object starts with s_latency signals */
    return q < 0 ? 0 : (int)q;
}

/* Pump, after a Present (gate on): the counted queue is never more than what
 * the frame statistics show (those may lag a refresh behind: an upper bound). */
static void gate_check(IDXGISwapChain *sc)
{
    DXGI_FRAME_STATISTICS fs;
    UINT lpc = 0;
    LONG cq = s_presented + s_qadj - s_retired + s_latency, sq;
    if (cq < 0) { InterlockedExchangeAdd(&s_qadj, -cq); cq = 0; }
    memset(&fs, 0, sizeof fs);
    if (FAILED(IDXGISwapChain_GetLastPresentCount(sc, &lpc)) || FAILED(IDXGISwapChain_GetFrameStatistics(sc, &fs))
        || !fs.PresentCount || lpc < fs.PresentCount)
        return;
    sq = (LONG)(lpc - fs.PresentCount);
    s_gate_dsum += sq - cq;
    s_gate_dn++;
    if (cq > sq) {
        InterlockedExchangeAdd(&s_qadj, sq - cq);
        s_gate_resync++;
    }
}

static void gate_start(void)
{
    if (!s_gate || !s_flip || !s_wait || s_gate_thread) return;
    s_gate_stop = 0;
    s_gate_thread = CreateThread(NULL, 0, gate_retire_thread, s_wait, 0, NULL);
    if (!s_gate_thread) { fprintf(stderr, "D3D8: XBOX_RENDER_GATE: no thread, gate off\n"); return; }
    SetThreadPriority(s_gate_thread, THREAD_PRIORITY_HIGHEST);
    InterlockedExchange(&s_gate_live, 1);
    fprintf(stderr, "D3D8: XBOX_RENDER_GATE=%d: the game waits before each render until at most %d frame(s) are on "
            "their way to the screen; no wait before Present\n", s_gate, s_gate);
}

void d3d8_sync_created(IDXGISwapChain *sc, HWND hwnd)
{
    s_hwnd = hwnd;
    InterlockedExchange(&s_hz, d3d8_monitor_hz(hwnd));
    rate_update(hwnd, "game window");
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
    pgrid_init();
    if (G.on) fprintf(stderr, "D3D8: XBOX_PRESENT_GRID=%d: 60 fps Presents on a steady grid, margin %.1f ms%s "
                      "(only at the 60 fps limit with VSync / Adaptive on a display above 60 Hz)\n", G.on, G.margin,
                      G.on == 2 ? ", GPU waited for, kept off the vertical blank"
                      : G.on == 3 ? " at least, adaptive (GPU end p95 + 0.5, up to 10 ms), GPU waited for, kept off the vertical blank" : "");
    fprintf(stderr, "D3D8: presentation %s (%s%s), display %ld Hz%s\n", k_mode_name[s_mode],
            s_flip ? "flip model, 3 buffers" : "DXGI_SWAP_EFFECT_DISCARD",
            s_flip ? (s_tearing ? ", tearing allowed" : ", no tearing") : (s_legacy_vsync ? ", XBOX_VSYNC=1" : ""),
            (long)s_hz, s_wait ? "" : (s_flip ? ", no latency object" : ""));
    if (s_flip && s_wait) fprintf(stderr, "D3D8: presentation queue: %d frame(s) at most\n", s_latency);
    gate_start();
}

/* Adaptive: an arrival gap of this length or more is a load or a stall, not
 * a pace (no frame for more than 3 steps of the game's 60 Hz logic); the
 * average covers
 * about this much time at the target pace; tearing starts when the average
 * interval is this much longer than the target (below ~91 % of its rate). */
#define ADAPTIVE_GAP_MS   60.0
#define ADAPTIVE_AVG_MS   500.0
#define ADAPTIVE_SLOW     1.10

/* Present grid (mode 1, XBOX_PRESENT_GRID=1[:margin ms]; mode 3 is the default).
 * At the 60 fps limit the game's own frame timer re-arms itself with whole
 * milliseconds: its frames come 14 to 19 ms apart (16.667 on average). On a
 * display above 60 Hz each one then reaches the screen after 3, 4 or 5
 * refreshes at 240 Hz (20 to 34 % of frames not on 4). With the grid,
 * the Present waits for a steady 60 Hz slot that follows the game's own
 * frames (anchor nudged towards their mean, so it never drifts away), plus a
 * margin that covers their spread: every frame is shown for the same time.
 * The game's timer and logic are untouched; only the Present is held (by
 * the margin, ~3 ms, plus the frame's earliness). */
#define PGRID_PERIOD      (1000.0 / 60.0)

/* Mode 2 (XBOX_PRESENT_GRID=2[:margin ms[:guard ms]], off by default):
 * the grid of mode 1, plus what mode 1 leaves to chance (Presents
 * regular to 0.02 ms, still 10-17 % of frames on 3 or 5 refreshes at
 * 240 Hz, at every phase against the refresh):
 *  - the frame's GPU work is waited for (D3D11 event query) before the
 *    hold: a frame finished at a varying time after its Present reached the
 *    screen at a varying refresh;
 *  - the vertical blank is predicted from the DXGI frame statistics (last
 *    blank's QPC time and count, period measured over >= 1 s of blanks): a
 *    Present that would fall less than `guard` before a blank (the flip may
 *    or may not catch it) waits until just after it, so consecutive frames
 *    stay on the same side. The phase of the game's frames against the
 *    blanks drifts slowly (60 against 239.98 / 4 Hz here: one refresh in
 *    ~50 s), so this moves frames by one refresh once per crossing only.
 * Cost: the presenting thread waits for the GPU (the game waits for that
 * thread; at 60 fps the GPU frame fits well within 16.7 ms), plus up to
 * `guard` + 0.3 ms in the guard band. With a variable refresh (G-Sync /
 * FreeSync) the blank prediction is meaningless: the guard is skipped when
 * the measured period strays from the display's rate. */
#include <d3d11.h>
#include <d3d11_4.h>
static struct {
    int tried, have_q;
    ID3D11DeviceContext *ctx;
    ID3D11Query *q;
    double guard, vq0, vper;
    UINT vc0;
    unsigned long long gpu_frames, gpu_slow, guarded, vbl_ok;
    double gpu_ms;
    double gd[128], mcur;           /* mode 3: GPU end - slot over the last 128 frames (~2 s), margin in use */
    int gn, gi;
    double mwarm;                   /* a video frame's margin before the first full computation */
    int warm_done;                  /* the first full computation made (the 128th frame; V is never reset) */
} V;

/* Mode 3 ("2b", XBOX_PRESENT_GRID=3[:min margin ms[:guard ms]], the
 * default: 3:3:2.5, see pgrid_init): mode 2
 * with a margin that follows the GPU. Mode 2's fixed 3 ms was shorter than
 * the GPU work on this PC (~4.9 ms after the frame's arrival, PC alone):
 * 79 to 92 % of frames came after their slot + margin and left at once, off
 * the grid. Here the margin = p95 of (GPU end - slot) over the last
 * ~2 s + 0.5 ms, within [margin, 10] ms (margin: 3 by default): the frame
 * is finished before it is sent, and sent on the grid. */
static double pgrid_margin3(double d, int video)
{
    double tmp[128], p;
    int i, j, n;
    V.gd[V.gi] = d;
    V.gi = (V.gi + 1) & 127;
    if (V.gn < 128) V.gn++;
    n = V.gn;
    if (n < 30) { V.mcur = G.margin; return V.mcur; }
    /* Sticky: a margin that moved every frame moved the Present across the
     * guard band's edge and back (3 / 5 refresh pairs). Recomputed
     * every 128 frames (~2 s): taken when higher, lowered only by more than
     * 1 ms. */
    if (V.mcur > 0.0 && V.gi != 0) {
        /* Until the first full computation (the 128th frame), the margin
         * stays at its floor; a video frame then follows the GPU at once
         * (the boot video's GPU slows down within its first seconds: at the
         * floor, frames rode the deadline, 1/3 refresh pairs). The p95 of the
         * samples so far + 0.5, raised only; game frames keep V.mcur. */
        if (video && !V.warm_done) {
            for (i = 0; i < n; i++) {
                double x = V.gd[i];
                for (j = i; j > 0 && tmp[j - 1] > x; j--) tmp[j] = tmp[j - 1];
                tmp[j] = x;
            }
            p = tmp[(int)(0.95 * (n - 1))] + 0.5;
            if (p < G.margin) p = G.margin;
            if (p > 10.0) p = 10.0;
            if (p > V.mwarm) V.mwarm = p;
            return V.mwarm > V.mcur ? V.mwarm : V.mcur;
        }
        return V.mcur;
    }
    V.warm_done = 1;
    for (i = 0; i < n; i++) {                 /* insertion sort, 128 values at most */
        double x = V.gd[i];
        for (j = i; j > 0 && tmp[j - 1] > x; j--) tmp[j] = tmp[j - 1];
        tmp[j] = x;
    }
    p = tmp[(int)(0.95 * (n - 1))] + 0.5;
    if (p < G.margin) p = G.margin;
    if (p > 10.0) p = 10.0;
    if (V.mcur <= 0.0 || p > V.mcur || p < V.mcur - 1.0) V.mcur = p;
    return V.mcur;
}

/* The wait for the frame's GPU work sleeps (M1): it used to turn on
 * YieldProcessor + Sleep(0) for the whole GPU time of every frame (S60 and
 * the grid, the defaults at 60 fps), a core busy for 3 to 10 ms a frame.
 *  - A D3D11 fence (ID3D11Device5, Windows 10 1703+; DXVK has it): when the
 *    usual GPU time is 3 ms or more, Signal after the frame's commands,
 *    SetEventOnCompletion and WaitForSingleObject until 2 ms before it (the
 *    event wakes the thread sooner if the GPU is done), then the event query
 *    is turned on to the end exactly as before. A shorter GPU time is waited
 *    for exactly as before (turned on whole: the old cost, under 3 ms).
 *  - Without one: the event query as before, polled with Sleep(1) while the
 *    wait is more than 2.5 ms short of the usual GPU time, then turned on for
 *    the end (Sleep(1) may last up to ~2 ms). The usual time is a running
 *    mean of the waits of every 8th frame, turned on from the start: a mean
 *    of the slept waits would grow with the sleeps' lateness (measured: 2.0
 *    against 0.9 ms) and push the grid's margin up.
 * Both stop at 25 ms, as before (never block a stalled frame for long).
 *
 * The fence's event resets itself only when a wait takes its signal. When the
 * query is seen done before the fence's signal reaches the event (the event
 * is set by the system, or by a thread of DXVK, a little after the GPU's
 * end), that signal stays set after the frame, and the next frame's wait
 * returned at once: its whole GPU time was turned on. The event is now
 * emptied before it is armed, and a wake while the fence is not at the
 * frame's value (an earlier frame's signal, come during the wait) waits
 * again for the rest of the time. XBOX_FIX_FENCEWAIT=0: as before
 * (comparison). Counted: the first such wake is logged, the totals every
 * 10 s with XBOX_LOG_STATS=1 and once when the window closes. */
static struct {
    int tried, ok, spin, test_sleep;
    int fix;                        /* XBOX_FIX_FENCEWAIT, default 1 */
    ID3D11DeviceContext4 *ctx4;
    ID3D11Fence *fence;
    HANDLE ev;
    UINT64 value;
    double mean;                    /* running mean of the turned-on waits (ms), for the query's polling */
    unsigned n;                     /* frames waited with the query */
    unsigned long long armed;       /* waits on the event */
    unsigned long long old;         /* of them, ended by an earlier frame's signal (the fence not at this one) */
    unsigned long long again;       /* waits made again after one (the fix) */
    double old_ms;                  /* without the fix: time turned after such a wake */
    double t_log;                   /* the last periodic line */
    int said;                       /* the first such wake was logged */
} F;

static const IID k_iid_device5 = { 0x8ffde202, 0xa0e7, 0x45df, { 0x9e, 0x01, 0xe8, 0x37, 0x80, 0x1b, 0x5e, 0xa0 } };
static const IID k_iid_context4 = { 0x917600da, 0xf58c, 0x4c33, { 0x98, 0xd8, 0x3e, 0x15, 0xb3, 0x90, 0xfa, 0x24 } };
static const IID k_iid_fence = { 0xaffde9d1, 0x1df7, 0x4bb7, { 0x8a, 0x34, 0x0f, 0x46, 0x25, 0x1d, 0xab, 0x80 } };

static void gpu_fence_init(ID3D11Device *dev)
{
    ID3D11Device5 *d5 = NULL;
    {
        const char *e = getenv("XBOX_FIX_FENCEWAIT");
        F.fix = !(e && e[0] == '0');
        if (!F.fix) fprintf(stderr, "D3D8: XBOX_FIX_FENCEWAIT=0: the fence's event as before (not emptied before a wait)\n");
    }
    if (FAILED(ID3D11Device_QueryInterface(dev, &k_iid_device5, (void **)&d5)) || !d5) d5 = NULL;
    /* No flags: (D3D11_FENCE_FLAG)0, not D3D11_FENCE_FLAG_NONE, which
     * MinGW's headers define as 1 (Microsoft's as 0); DXVK logs that 1 as an
     * unsupported flag. */
    if (d5 && SUCCEEDED(ID3D11DeviceContext_QueryInterface(V.ctx, &k_iid_context4, (void **)&F.ctx4)) && F.ctx4 &&
        SUCCEEDED(ID3D11Device5_CreateFence(d5, 0, (D3D11_FENCE_FLAG)0, &k_iid_fence, (void **)&F.fence)) && F.fence &&
        (F.ev = CreateEventW(NULL, FALSE, FALSE, NULL)) != NULL) {
        F.ok = 1;
    } else {
        if (F.fence) { ID3D11Fence_Release(F.fence); F.fence = NULL; }
        if (F.ctx4) { ID3D11DeviceContext4_Release(F.ctx4); F.ctx4 = NULL; }
    }
    if (d5) ID3D11Device5_Release(d5);
    fprintf(stderr, "D3D8: GPU wait: %s\n", F.ok ? "fence (asleep to 2 ms before the usual GPU end when it is 3 ms or more)"
                                                 : "event query, Sleep(1) polling (no D3D11 fence)");
}

static void gpu_fence_report(const char *when)
{
    fprintf(stderr, "D3D8: GPU wait (%s): %llu waits on the fence's event, %llu ended by an earlier frame's signal, "
            "%llu waited on again, %.1f ms turned after one\n", when, F.armed, F.old, F.again, F.old_ms);
}

/* Once, when the window closes (d3d8_sync_report). */
static void gpu_fence_summary(void)
{
    static LONG done;
    if (F.armed && !InterlockedExchange(&done, 1)) gpu_fence_report("session");
}

/* The wait on the fence's event was ended by a signal while the fence is not
 * at this frame's value: an earlier frame's. With the fix, waited on again
 * until the value or the wait's limit (`lim` ms from t0), and 0 is returned;
 * without it (as before), the time it was seen: the rest is turned on. */
static double gpu_fence_old_signal(double t0, DWORD lim)
{
    double t = now_ms();
    F.old++;
    if (!F.said) {
        F.said = 1;
        fprintf(stderr, "D3D8: GPU wait: the fence's event was left set by an earlier frame (wait %llu): %s\n", F.armed,
                F.fix ? "waited on again" : "the rest of the GPU time is turned on (XBOX_FIX_FENCEWAIT=0)");
        xbox_log_flush();
    }
    if (!F.fix) return t;
    for (;;) {
        double left = (double)lim - (t - t0);
        if (left < 1.0) break;
        F.again++;
        if (WaitForSingleObject(F.ev, (DWORD)left) != WAIT_OBJECT_0) break;
        if (ID3D11Fence_GetCompletedValue(F.fence) >= F.value) break;
        F.old++;
        t = now_ms();
    }
    return 0.0;
}

static void pgrid_gpu_wait(IDXGISwapChain *sc)
{
    double t0, el, t_old = 0.0, t_end;
    int armed = 0;
    if (!V.tried) {
        ID3D11Device *dev = NULL;
        D3D11_QUERY_DESC qd;
        V.tried = 1;
        qd.Query = D3D11_QUERY_EVENT;
        qd.MiscFlags = 0;
        if (SUCCEEDED(IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void **)&dev)) && dev) {
            ID3D11Device_GetImmediateContext(dev, &V.ctx);
            if (V.ctx && SUCCEEDED(ID3D11Device_CreateQuery(dev, &qd, &V.q)) && V.q) V.have_q = 1;
            if (V.ctx) gpu_fence_init(dev);
            ID3D11Device_Release(dev);
        }
        if (!V.have_q && !F.ok) fprintf(stderr, "D3D8: XBOX_PRESENT_GRID=2: no GPU event query, the GPU is not waited for\n");
    }
    if (!V.have_q) return;
    if (F.ok && (F.n & 7u) && (F.mean >= 3.0 || F.test_sleep)) {
        /* asleep on the fence's event until 2 ms before the usual end (sooner if the GPU is done),
         * then the query turned on as before: a wake by the event alone comes a little after the
         * GPU's end (offmode not settled, 1.35 against 0.96 %) */
        t0 = now_ms();
        F.value++;
        ID3D11DeviceContext4_Signal(F.ctx4, F.fence, F.value);
        ID3D11DeviceContext_Flush(V.ctx);
        if (ID3D11Fence_GetCompletedValue(F.fence) < F.value) {
            if (F.fix) ResetEvent(F.ev);        /* a signal left by the previous frame must not end this wait */
            if (FAILED(ID3D11Fence_SetEventOnCompletion(F.fence, F.value, F.ev))) {
                F.ok = 0;                       /* not again: Sleep(1) polling from now on */
                fprintf(stderr, "D3D8: GPU wait: SetEventOnCompletion failed, Sleep(1) polling from now on\n");
            } else {
                DWORD lim = F.test_sleep ? 25u : (DWORD)(F.mean - 2.0);
                armed = 1;
                F.armed++;
                if (WaitForSingleObject(F.ev, lim) == WAIT_OBJECT_0 && ID3D11Fence_GetCompletedValue(F.fence) < F.value)
                    t_old = gpu_fence_old_signal(t0, lim);
            }
        }
        ID3D11DeviceContext_End(V.ctx, (ID3D11Asynchronous *)V.q);
    } else {
        ID3D11DeviceContext_End(V.ctx, (ID3D11Asynchronous *)V.q);
        t0 = now_ms();
    }
    /* flags 0: the first call flushes the commands */
    while (ID3D11DeviceContext_GetData(V.ctx, (ID3D11Asynchronous *)V.q, NULL, 0, 0) != S_OK) {
        el = now_ms() - t0;
        if (el > 25.0) { V.gpu_slow++; break; }      /* never block a stalled frame for long */
        if (!F.ok && !F.spin && (F.n & 7u) && el + 2.5 < F.mean) { Sleep(1); continue; }
        YieldProcessor();
        Sleep(0);
    }
    if (armed) WaitForSingleObject(F.ev, 0);       /* its signal, if the turn saw the end first */
    t_end = now_ms();
    el = t_end - t0;
    if (!(F.n++ & 7u)) F.mean = F.mean > 0.0 ? F.mean + (el - F.mean) / 4.0 : el;   /* every 8th: turned on */
    V.gpu_frames++;
    V.gpu_ms += el;
    if (t_old > 0.0) F.old_ms += t_end - t_old;
    if (armed && xbox_log_stats() && t_end - F.t_log >= 10000.0) {
        if (F.t_log > 0.0) gpu_fence_report("total");
        F.t_log = t_end;
    }
}

/* Next vertical blank strictly after t (ms, QPC base), from the frame
 * statistics; 0 if unknown or the refresh does not look fixed. */
static double pgrid_next_blank(IDXGISwapChain *sc, double t)
{
    DXGI_FRAME_STATISTICS fs;
    double q, n, nominal = s_hz > 0 ? 1000.0 / (double)s_hz : 0.0;
    memset(&fs, 0, sizeof fs);
    if (!s_flip || FAILED(IDXGISwapChain_GetFrameStatistics(sc, &fs)) || !fs.SyncQPCTime.QuadPart || nominal <= 0.0) return 0.0;
    q = (double)fs.SyncQPCTime.QuadPart * 1000.0 / s_qf;
    if (!V.vc0 || fs.SyncRefreshCount < V.vc0 || q < V.vq0) { V.vc0 = fs.SyncRefreshCount; V.vq0 = q; V.vper = 0.0; return 0.0; }
    if (fs.SyncRefreshCount - V.vc0 >= (UINT)(s_hz > 0 ? s_hz : 60))   /* >= 1 s of blanks */
        V.vper = (q - V.vq0) / (double)(fs.SyncRefreshCount - V.vc0);
    if (V.vper <= 0.0) return 0.0;
    if (fabs(V.vper - nominal) > 0.02 * nominal) {                 /* VRR, mode change: start again */
        V.vc0 = fs.SyncRefreshCount; V.vq0 = q; V.vper = 0.0;
        return 0.0;
    }
    n = ceil((t - q) / V.vper);
    if (q + n * V.vper <= t) n += 1.0;
    V.vbl_ok++;
    return q + n * V.vper;
}

/* The grid only where 60 fps can be even: a display at a multiple of 60 Hz
 * (120, 180, 240, 360; within 1 %, so 239 or 59.94 x 4 count). At 144 or
 * 165 Hz a 60 fps frame lasts 2.4 or 2.75 refreshes whatever is done; the
 * grid would only add delay there. Checked again when the display's
 * rate changes (window moved to another display); one log line each time. */
static int pgrid_hz_ok(long hz)
{
    static long checked = -1;
    static int ok;
    if (hz != checked) {
        long n = (hz + 30) / 60;
        checked = hz;
        ok = n >= 2 && labs(hz - 60 * n) <= (60 * n) / 100;
        fprintf(stderr, "D3D8: present grid: display %ld Hz %s\n", hz,
                ok ? "is a multiple of 60: grid on" : "is not a multiple of 60: grid off (a 60 fps frame cannot last a whole number of refreshes there)");
    }
    return ok;
}

static void pgrid_init(void)
{
    const char *e = getenv("XBOX_PRESENT_GRID"), *c;
    /* Default: mode 3 with a 3 ms minimum margin and a 2.5 ms guard, the
     * measured best (0-1.5 % of 60 fps frames off 4 refreshes at
     * 240 Hz, against 25-29 % without the grid; ~5 ms more delay). It is a
     * fix, always on: it only acts at the 60 fps limit with VSync / Adaptive
     * on a display at a multiple of 60 Hz (pgrid_hz_ok), and the game's
     * logic is untouched. XBOX_PRESENT_GRID=0 turns it off (diagnostic). */
    /* With the frame timer on an exact grid (frame_clock.c) the frames
     * arrive ~2.5 ms less scattered: the adaptive margin is let down to 1 ms
     * (it stays the p95 of GPU end - slot + 0.5). */
    if (!e || !e[0]) e = s_fc_shift ? "3:1:2.5" : "3:3:2.5";
    G.on = e[0] == '1' || e[0] == '2' || e[0] == '3' ? e[0] - '0' : 0;
    G.margin = 3.0;
    V.guard = G.on == 3 ? 2.5 : 1.0;
    if (G.on && (c = strchr(e, ':')) != NULL) {
        double m = atof(c + 1);
        if (m >= 0.0 && m <= 8.0) G.margin = m;
        if ((c = strchr(c + 1, ':')) != NULL) {
            m = atof(c + 1);
            if (m >= 0.0 && m <= 4.0) V.guard = m;
        }
    }
}


/* ── the frame timer follows the refreshes (XBOX_FCLOCK_FOLLOW=0: off, diagnostic) ──
 * At the 60 fps limit with VSync on a fixed display (60 Hz, or a multiple of 60 Hz),
 * each frame has a target (the present grid's slot + margin, the GPU usually done
 * by then; on a 60 Hz display, its plan) and is shown on a refresh after it. The
 * frame timer (frame_clock.c, where the controller is read) runs at 60.000 Hz and
 * the display a little slower (239.98 / 4 = 59.995 Hz; 59.973 Hz): the phase between
 * them goes round once a minute or so, and with it the time from target to refresh
 * sweeps the whole refresh period: 2.5 to 10.8 ms at 120 Hz, 1.5 to 18.2 ms at
 * 60 Hz. The controller is already read during that time. Here the timer's phase
 * follows the refreshes and holds that time at the room a frame needs: the
 * controller is read later, and the frame is shown on the same refresh of the same
 * cadence.
 * The room (FL.room = guard + FOLLOW_TAIL) is the time a frame must keep before its
 * refresh: the time a flip needs to be taken (frames presented 0.7 to 2.2 ms before
 * a refresh missed it 20 to 35 % of the time; the grid's guard keeps 2.5 ms), plus
 * the time that leaves a frame later than its target as many chances as the sweep
 * gives on average (the time from the timer's callback to the GPU's end has a tail
 * of about 1 ms above the grid's margin: 2.1 ms). 4.6 ms. With a refresh period no
 * longer than the room there is nothing to gain: the sweep's own time from target
 * to refresh is already about the room on average (4.58 ms at 240 Hz), and a place
 * held closer to the refresh than the room loses frames (measured at 240 Hz with
 * 3.9 ms: more frames held a refresh longer in the menus). So the timer follows
 * only a display whose refresh is longer than the room (60, 120, 180 Hz); at 240 Hz
 * and above nothing moves. On a 60 Hz display a frame that misses its refresh
 * repeats the one before: the room there (FOLLOW_ROOM60, from the plan with its own
 * lead) is a little longer.
 * When the phase comes to the end of its window the shift goes back by one
 * refresh, FOLLOW_WRAP_STEP a frame at most: one frame shown for a refresh fewer
 * once per turn of the phase, as the sweep does; at 60 Hz, one frame left out by
 * the 60 Hz display path, as it does. Never in one step: the title's callback
 * counts a tick (app+0x14, 0xAA310) and its main loop (0xAA1A0) runs the ticks
 * counted, then one render, then waits for the frame event (0xB2750). Two callbacks
 * within one turn of that loop give two ticks in one image, and that image's
 * visibility tests and the render of the tick between them never happen. The
 * loop's turn ends after the frame's Present (the title waits for the pump), which
 * the present grid may hold to its target, up to ~13 ms after the callback:
 * callbacks 12.7 ms apart still met in one turn about one wrap in four (measured).
 * A step of FOLLOW_WRAP_STEP keeps them 16.2 ms apart or more: the loop would have
 * to take 97 % of a frame, where the title misses ticks with or without the
 * change. Every tick keeps its own render. The same callbacks run in the same
 * order and each does the same; only their instants move. Only with the frames flipped on their own (media
 * statistics: an overlay plane). Anywhere else (VSync off, VRR, no statistics, a
 * composed window, not a multiple of 60 Hz), and while too many frames miss their
 * target, the shift stays where it is: any phase suits the grid, which works as it
 * did. */
#define FOLLOW_TAIL     2.1     /* ms over the guard: a late frame's share of the room (see above) */
#define FOLLOW_ROOM60   5.0     /* ms from plan to refresh on a 60 Hz display */
#define FOLLOW_HIGH     2.0     /* ms: the highest shift; 11.7 ms of a stall from the title's catch-up */
#define FOLLOW_WRAP_STEP 0.5    /* ms of a wrap per frame at most (see above) */
#define FOLLOW_GAIN     0.1
#define FOLLOW_SLEW     0.02    /* ms per frame at most: below the grid's own scatter */
#define FOLLOW_DEAD     0.1     /* ms: the target's own jitter */
#define FOLLOW_LATE     13      /* frames of 128 past their target (10 %): the target means little */
static struct {
    int tried, off, on;
    double room, room60;        /* ms a frame keeps from its target to its refresh (above 60 Hz: also the shortest
                                   refresh period followed), on a 60 Hz display */
    int said_short;             /* a refresh no longer than the room was logged */
    double shift;               /* asked of the timer (ms) */
    double move[2];             /* asked and not yet in the frames' arrivals: a shift asked during a frame is
                                   taken at the next callback, and moves the one after */
    int n, late, still;         /* this window of 128 frames, those late; the last window was over FOLLOW_LATE */
    double owe;                 /* what is left of a wrap (ms), paid FOLLOW_WRAP_STEP a frame at most */
    unsigned long long frames, wraps, stills;
} FL;

static void follow_init(void)
{
    const char *e = getenv("XBOX_FCLOCK_FOLLOW");
    FL.tried = 1;
    FL.off = !s_fc_shift || (e && e[0] == '0' && !e[1]);
    FL.room = V.guard + FOLLOW_TAIL;
    FL.room60 = FOLLOW_ROOM60;
    if (FL.off && s_fc_shift) fprintf(stderr, "D3D8: the frame timer does not follow the refreshes (diagnostic)\n");
}

/* The grid's slots move with the arrivals of the frames (pgrid_wait, s60_wait):
 * what the timer was asked two frames ago reaches this one. */
static void follow_grid(void)
{
    G.anchor += FL.move[1];
    G.last_slot += FL.move[1];
    FL.move[1] = FL.move[0];
    FL.move[0] = 0.0;
}

static void follow_shift(double s)
{
    FL.move[0] += s - FL.shift;
    FL.shift = s;
    s_fc_shift(s);
}

/* 1: this frame paid part of a wrap (nothing else moves the shift meanwhile). */
static int follow_owed(void)
{
    double d = FL.owe;
    if (d == 0.0) return 0;
    if (d > FOLLOW_WRAP_STEP) d = FOLLOW_WRAP_STEP;
    if (d < -FOLLOW_WRAP_STEP) d = -FOLLOW_WRAP_STEP;
    FL.owe -= d;
    if (fabs(FL.owe) < 1e-9) FL.owe = 0.0;
    follow_shift(FL.shift + d);
    return 1;
}

/* y: ms from this frame's target to the refresh it is planned on; per: the
 * refresh period; ahead: the time wanted (the room); late: the frame is past its
 * target. */
static void follow_refresh(double y, double per, double ahead, int late)
{
    double e, s;
    if (FL.off) return;
    if (per <= FL.room) {                       /* nothing to gain there (see above) */
        if (!FL.said_short) {
            FL.said_short = 1;
            fprintf(stderr, "D3D8: the frame timer does not follow the refreshes: a refresh of %.2f ms is not longer than "
                    "the %.1f ms a frame keeps before it\n", per, FL.room);
        }
        return;
    }
    FL.frames++;
    FL.late += late;
    if (++FL.n >= 128) {
        FL.still = FL.late > FOLLOW_LATE;
        if (FL.still) FL.stills++;
        FL.n = FL.late = 0;
    }
    if (FL.still || s_comp_mode != DXGI_FRAME_PRESENTATION_MODE_OVERLAY) return;
    if (follow_owed()) return;                  /* a wrap begun is ended first */
    if (!FL.on) {
        FL.on = 1;
        fprintf(stderr, "D3D8: the frame timer follows the refreshes (refresh %.4f ms, %.1f ms from a frame's target to its refresh)\n",
                per, ahead);
    }
    e = y - ahead;
    e -= per * floor(e / per + 0.5);            /* to the nearest refresh; > 0: the timer can fire later */
    if (fabs(e) < FOLLOW_DEAD) return;
    e *= FOLLOW_GAIN;
    if (e > FOLLOW_SLEW) e = FOLLOW_SLEW;
    if (e < -FOLLOW_SLEW) e = -FOLLOW_SLEW;
    s = FL.shift + e;
    if (s > FOLLOW_HIGH) { FL.owe -= per; FL.wraps++; }
    else if (s < FOLLOW_HIGH - per - 0.5) { FL.owe += per; FL.wraps++; }
    follow_shift(s);
}


/* ── 60 Hz displays (XBOX_S60=0: off, diagnostic) ───────────────────
 * The title's frames come at 60.000 Hz (its own timer; frame_clock.c puts it
 * on an exact grid). A "60 Hz" display refreshes a little slower: 59.973 Hz
 * here (period 16.6745 ms), 59.94 on a TV. With VSync each refresh takes one
 * frame from the DXGI queue, in order; frames came 0.008 ms per frame faster
 * than they left, the queue filled up to its limit (2 frames) in ~1 min and
 * stayed full: 28-31 ms from Present to the screen, and the presenting thread
 * blocked on the latency object until the title, held up, rendered two ticks
 * as one (measured: 2 frames waiting at every Present). Above 61 Hz the
 * grid shows one frame for a refresh fewer now and then instead; at 60 Hz
 * there is no refresh fewer: a frame has to be left out.
 * Here, as the console on an NTSC set: each frame is planned on a refresh from
 * the present grid (its steady 60 Hz slot + margin, GPU waited for) and the
 * refresh times of the frame statistics; when two frames come to plan the same
 * refresh (once every 1 / (60 - rate) s: ~37 s here, ~17 s at 59.94), the
 * second is not presented -- two ticks for one refresh -- and the queue never
 * holds more than the frame waiting for its refresh. A frame not finished in
 * time for its refresh is presented just after it (one refresh later, its
 * predecessor shown twice); the next one, planned on that refresh, is then
 * left out. The title's logic and ticks are untouched; only Presents are left
 * out. Only at the 60 fps limit, VSync (or Adaptive without tearing), on a
 * display at 59-61 Hz with a fixed refresh in the frame statistics; anywhere
 * else (VRR, no statistics: Deck / DXVK without them, a window behind) the
 * previous presentation is kept (2-frame queue), and one log line says so.
 * The frame timer's phase follows the refreshes here too (follow_refresh): frames
 * no longer sweep the refresh, and the meeting of two frames becomes the shift's
 * wrap, the timer's callback coming right after the one before.
 * A frame of the title's video player (d3d8_sync_video_frame) is
 * not one of those 60 Hz frames. The player presents once per frame of its
 * file, at 29.97 fps: every second refresh, and in a burst of 3 to 6 frames
 * when a video starts. The console shows each one from the first
 * vertical blank after its flip (PresentationInterval ONE): two refreshes a
 * frame, every frame shown. So a video frame is planned like any other and
 * shown on the first refresh it makes, and the two rules below that assume
 * one frame per refresh leave it alone: it is never left out (on an occupied
 * refresh it takes the next one), and it is never moved back to the refresh
 * right after the previous frame (that cut the previous frame to one refresh:
 * pairs of 1 and 3 refreshes instead of 2 and 2). The game's
 * frames take exactly the path above. */
#define S60_LEAD    1.5     /* ms before its refresh a Present must be made (GPU done) to be shown on it */
#define S60_HYST    1.0     /* ms: how early a frame must be for an occupied refresh to be left out (see s60_wait) */
static struct {
    int tried, off, on;
    double s_prev;          /* refresh the last presented frame is shown on (ms, QPC); 0: unknown */
    double t_log;
    const char *why;        /* last reason it stood aside */
    unsigned long long frames, drops, late, recover, held, kept;
    unsigned long long vframes, vnext;  /* video frames; of them, taken a refresh after an occupied one */
} S6;
#define S60_RING    16
static struct { UINT pc; double s; } S6R[S60_RING];     /* refresh planned for each Present (by its PresentCount) */

static void s60_note(int on, const char *why)
{
    if (on != S6.on || (!on && why != S6.why)) {
        fprintf(stderr, "D3D8: 60 Hz display: %s%s%s\n", on ? "frames planned on the refresh, one left out when two meet"
                : "previous presentation (", on ? "" : why, on ? "" : ")");
        fflush(stderr);
    }
    S6.on = on;
    S6.why = why;
    if (!on) S6.s_prev = 0.0;
}

/* 1: this frame takes the 60 Hz display path (s60_wait), from the frame's
 * arrival (t) and its sync interval. */
static int s60_check(IDXGISwapChain *sc, double t, UINT sync)
{
    long hz = s_hz, cap = s_pace_fps;
    if (!S6.tried) {
        const char *e = getenv("XBOX_S60");
        S6.tried = 1;
        S6.off = e && e[0] == '0';
        if (S6.off) fprintf(stderr, "D3D8: XBOX_S60=0: 60 Hz displays keep the 2-frame queue (diagnostic)\n");
    }
    if (S6.off || hz < 59 || hz > 61) { if (S6.on) s60_note(0, "display not at 60 Hz"); return 0; }
    if (!s_flip || !G.on || cap != 60 || sync != 1 || s_gate_live
        || (s_mode != D3D8_SYNC_VSYNC && s_mode != D3D8_SYNC_ADAPTIVE)) {
        s60_note(0, "not at the 60 fps limit with VSync");
        return 0;
    }
    if (pgrid_next_blank(sc, t) <= 0.0 || V.vper <= 0.0) {
        s60_note(0, "no fixed refresh in the frame statistics (VRR, window behind, or none)");
        return 0;
    }
    if (!S6.on) s60_note(1, NULL);
    return 1;
}

/* 60 Hz display path (see above): 1 = present now (planned refresh in
 * S6.s_prev), 0 = leave this frame out. video: a frame of the title's video
 * player, never left out. */
static int s60_wait(IDXGISwapChain *sc, double t, int video)
{
    double k, slot, margin, plan, s, per, hold;
    DXGI_FRAME_STATISTICS fs;
    int on_plan = 1;
    S6.frames++;
    if (video) S6.vframes++;
    if (!FL.tried) follow_init();
    pgrid_gpu_wait(sc);
    per = V.vper;
    /* the present grid's slot (as pgrid_wait), its margin from the GPU end */
    if (G.anchor <= 0.0 || t - G.last_slot > 4.0 * PGRID_PERIOD || t < G.last_slot) {
        G.anchor = G.last_slot = slot = t;
        G.restart++;
        FL.move[0] = FL.move[1] = 0.0;
        on_plan = 0;
    } else {
        follow_grid();
        k = floor((t - G.anchor) / PGRID_PERIOD + 0.5);
        slot = G.anchor + k * PGRID_PERIOD;
        if (slot < G.last_slot + 0.5 * PGRID_PERIOD) slot = G.last_slot + PGRID_PERIOD;
        G.anchor += 0.02 * (t - slot);
        G.last_slot = slot;
    }
    t = now_ms();
    margin = pgrid_margin3(t - slot, video);
    plan = slot + margin;
    if (plan - t > margin + 0.5 * PGRID_PERIOD) { plan = t; on_plan = 0; }  /* never planned more than ~half a frame + margin ahead */
    s = pgrid_next_blank(sc, plan + S60_LEAD);                   /* the refresh it is planned on */
    if (s <= 0.0) { S6.s_prev = 0.0; return 1; }
    if (on_plan && !video) follow_refresh(s - plan, per, FL.room60, t >= plan);
    /* The latest frame shown, by the statistics (updated at each flip): shown
     * on another refresh than planned (a refresh missed unseen) -> the frames
     * planned after it move as much, so that the next one meets them and is
     * left out (the queue back to one frame). */
    memset(&fs, 0, sizeof fs);
    if (SUCCEEDED(IDXGISwapChain_GetFrameStatistics(sc, &fs)) && fs.PresentCount && fs.SyncQPCTime.QuadPart) {
        int i = (int)(fs.PresentCount % S60_RING), j;
        if (S6R[i].pc == fs.PresentCount && S6R[i].s > 0.0) {
            double a = (double)fs.SyncQPCTime.QuadPart * 1000.0 / s_qf
                     - (double)(fs.SyncRefreshCount - fs.PresentRefreshCount) * per;
            double d = a - S6R[i].s, n = floor(d / per + 0.5);
            S6R[i].pc = 0;                                       /* checked once */
            if (n != 0.0 && fabs(d) < 8.0 * per) {
                for (j = 0; j < S60_RING; j++)
                    if (S6R[j].pc > fs.PresentCount) S6R[j].s += n * per;
                if (S6.s_prev > 0.0) S6.s_prev += n * per;
                S6.recover++;
            }
        }
    }
    /* A frame already planned on that refresh (or later): this one is left
     * out only when it is early for it by S60_HYST more than the lead; else
     * it goes on the refresh after (up to S60_HYST more delay). The plan
     * jitters by ~0.1 ms (grid anchor, margin steps) and the beat moves it
     * 0.008 ms per frame: without this margin the plan crossed the refresh
     * back and forth near the meeting point, 8-14 frames left out a minute
     * (with as many shown twice) instead of one per beat (measured).
     * After a frame left out the next one starts S60_HYST clear of it.
     * A video frame is never left out: the player shows every frame
     * of its file; on an occupied refresh it takes the next one, as on the
     * console the flip after a pending one waits for the following blank. */
    if (S6.s_prev > 0.0 && s < S6.s_prev + 0.5 * per && S6.s_prev - t < 3.0 * per) {
        if (!video && plan + S60_LEAD + S60_HYST <= S6.s_prev) { S6.drops++; return 0; }
        s = S6.s_prev + per;
        S6.kept++;
        if (video) S6.vnext++;
    }
    t = now_ms();
    /* The other side: a plan moved past the refresh after the previous one
     * (a margin step) while the frame is ready for it keeps the cadence (no
     * refresh left without a new frame). One frame a refresh is the game's
     * cadence, not a video's: a video frame planned two refreshes
     * after the previous one is on its own pace and keeps its refresh. */
    if (!video && S6.s_prev > 0.0 && s > S6.s_prev + 1.5 * per && S6.s_prev - t < 3.0 * per
        && t + S60_LEAD <= S6.s_prev + per) {
        s = S6.s_prev + per;
        S6.kept++;
    }
    if (t > s - S60_LEAD) {
        /* too late for it: the first refresh it surely makes (a refresh
         * closer than the lead may or may not catch it: just after it) */
        double b = pgrid_next_blank(sc, t);
        S6.late++;
        if (b <= 0.0) { hold = t; s = 0.0; }
        else if (b - t < S60_LEAD) { hold = b + 0.3; s = b + per; }
        else { hold = t; s = b; }
    } else {
        hold = s - per + 0.3;                                    /* never before the refresh ahead of it: shown on s, not earlier */
    }
    if (t < hold) S6.held++;
    while ((t = now_ms()) < hold) {
        if (hold - t > 1.5) Sleep((DWORD)(hold - t - 1.0));
        else { YieldProcessor(); if (hold - t > 0.3) Sleep(0); }
    }
    S6.s_prev = s;
    if (t - S6.t_log >= 10000.0) {
        if (S6.t_log > 0.0 && xbox_log_stats())
            fprintf(stderr, "D3D8: 60 Hz display: %llu frames, %llu left out (two on one refresh), %llu late (a refresh later), "
                    "%llu shown off plan by the statistics, refresh %.4f ms, margin now %.2f ms; "
                    "%llu video frames, none left out, %llu on the refresh after an occupied one\n",
                    S6.frames, S6.drops, S6.late, S6.recover, per, V.mcur, S6.vframes, S6.vnext);
        if (S6.t_log > 0.0 && xbox_log_stats())
            fprintf(stderr, "D3D8: frame timer: shift %.2f ms, %llu frames, %llu wraps, %llu windows of 128 too late\n",
                    FL.shift, FL.frames, FL.wraps, FL.stills);
        S6.t_log = t;
    }
    return 1;
}

/* Holds the calling (presenting) thread until this frame's slot + margin
 * (mode 2: GPU done first, then off the blank's guard band). video: a frame of
 * the title's video player, whose pace is not the frame timer's. */
static void pgrid_wait(IDXGISwapChain *sc, double t, int video)
{
    double k, slot, target, margin;
    long hz = s_hz, cap = s_pace_fps;
    int on_plan = 1;
    static double last_log;
    if (!G.on || cap != 60 || (s_mode != D3D8_SYNC_VSYNC && s_mode != D3D8_SYNC_ADAPTIVE)) {
        return;
    }
    if (hz <= 61) return;
    if (!pgrid_hz_ok(hz)) return;
    /* Counters every 10 s: the benches end the game with TerminateProcess (no atexit). */
    if (t - last_log >= 10000.0) {
        if (last_log > 0.0 && xbox_log_stats()) {
            fprintf(stderr, "D3D8: present grid: %llu frames held to their slot, %llu late, %llu restarts (total)\n",
                    G.held, G.late, G.restart);
            if (G.on >= 2)
                fprintf(stderr, "D3D8: present grid %d: GPU wait %.2f ms avg (%llu over 25 ms), %llu frames moved past a blank, blank known %llu, period %.4f ms, guard %.1f ms, margin now %.2f ms\n",
                        G.on, V.gpu_frames ? V.gpu_ms / (double)V.gpu_frames : 0.0, V.gpu_slow, V.guarded, V.vbl_ok, V.vper, V.guard,
                        G.on == 3 ? V.mcur : G.margin);
            fprintf(stderr, "D3D8: frame timer: shift %.2f ms, %llu frames, %llu wraps, %llu windows of 128 too late\n",
                    FL.shift, FL.frames, FL.wraps, FL.stills);
        }
        last_log = t;
    }
    if (!FL.tried) follow_init();
    if (G.on >= 2) pgrid_gpu_wait(sc);
    if (G.anchor <= 0.0 || t - G.last_slot > 4.0 * PGRID_PERIOD || t < G.last_slot) {
        /* start, load, stall: the grid starts again on this frame */
        G.anchor = G.last_slot = t;
        G.restart++;
        FL.move[0] = FL.move[1] = 0.0;
        return;
    }
    follow_grid();
    k = floor((t - G.anchor) / PGRID_PERIOD + 0.5);
    slot = G.anchor + k * PGRID_PERIOD;
    if (slot < G.last_slot + 0.5 * PGRID_PERIOD) slot = G.last_slot + PGRID_PERIOD;   /* one frame per slot */
    /* follow the game's clock: mean arrival on the slot */
    G.anchor += 0.02 * (t - slot);
    G.last_slot = slot;
    t = now_ms();                                               /* modes 2, 3: after the GPU wait */
    margin = G.on == 3 ? pgrid_margin3(t - slot, 0) : G.margin;
    target = slot + margin;
    if (t >= target) G.late++;
    else if (target - t > margin + 0.5 * PGRID_PERIOD) { target = t; on_plan = 0; }  /* never hold more than ~half a frame + margin */
    else G.held++;
    if (G.on >= 2) {
        double b = pgrid_next_blank(sc, target);
        /* before the guard: from the target to the refresh this frame will be shown on */
        if (b > 0.0 && on_plan && !video)
            follow_refresh(b - target < V.guard ? b - target + V.vper : b - target, V.vper, FL.room, t >= target);
        if (b > 0.0 && b - target < V.guard) {                  /* the flip may or may not catch this blank */
            target = b + 0.3;
            V.guarded++;
        }
    }
    while ((t = now_ms()) < target) {
        if (target - t > 1.5) Sleep((DWORD)(target - t - 1.0));
        else { YieldProcessor(); if (target - t > 0.3) Sleep(0); }
    }
}

/* Fork: how the frames reach the display (IDXGISwapChainMedia, flip model):
 * composed by the desktop (+ a refresh or so), on a hardware overlay plane
 * (an independent flip), or a mode the statistics do not specify. They
 * describe the last frame the display took, so a reading counts only when it
 * moves on: after a change (window <-> full screen, another display, the
 * display's mode: s_comp_again), only once it describes a frame presented
 * after it; and frames presented while none reached the display since the
 * last reading (the window hidden, covered, minimised) leave it unknown.
 * Read every COMP_READ_MS while known (a mode the statistics do not specify,
 * or a composition failure, included), every COMP_RETRY_MS while unknown;
 * kept for d3d8_sync_immediate. A line for the log at a change, one a second
 * at most (the changes in between counted), written whole, never flushed
 * here: the log's own thread empties it. */
#define COMP_READ_MS    100.0
#define COMP_RETRY_MS   16.0
#define COMP_LOG_MS     1000.0
#define COMP_QI_MS      2000.0
static void presentation_mode_note(IDXGISwapChain *sc, double t)
{
    static const GUID iid_media = { 0xdd95b90b, 0xf05f, 0x4f6a, { 0xbd,0x65, 0x25,0xbf,0xb2,0x64,0xbd,0x84 } };
    static const char *const k_name[] = { "composed by the desktop", "hardware overlay plane",
                                          "none specified", "composition failure" };
    static int prev = -2;
    static const char *why = "no media statistics";
    DXGI_FRAME_STATISTICS_MEDIA fs;
    UINT lpc = 0;
    double t0;
    int mode = -1;
    char line[200];
    if (!s_flip) return;
    if (InterlockedExchange(&s_comp_again, 0)) {
        InterlockedExchange(&s_comp_mode, -1);
        CM.marked = SUCCEEDED(IDXGISwapChain_GetLastPresentCount(sc, &CM.mark));
        CM.changed = t;
        prev = -1;                      /* the mode found after it is logged, the same or not */
    }
    if (t - CM.read >= (s_comp_mode >= 0 ? COMP_READ_MS : COMP_RETRY_MS)) {
        CM.read = t;
        t0 = now_ms();
        if (!CM.m && t >= CM.qi_retry && (FAILED(IDXGISwapChain_QueryInterface(sc, &iid_media, (void **)&CM.m)) || !CM.m)) {
            CM.m = NULL;                /* no media statistics here (some translation layers): not asked again at once */
            CM.qi_retry = t + COMP_QI_MS;
        }
        why = "no media statistics";
        if (CM.m) {
            memset(&fs, 0, sizeof fs);
            if (SUCCEEDED(IDXGISwapChainMedia_GetFrameStatisticsMedia(CM.m, &fs)) && fs.PresentCount
                && SUCCEEDED(IDXGISwapChain_GetLastPresentCount(sc, &lpc))) {
                if (CM.marked && fs.PresentCount <= CM.mark) why = "no frame shown since the change";
                else if (fs.PresentCount == CM.seen && lpc != CM.lpc) why = "frames presented, none shown since";
                else { mode = (int)fs.CompositionMode; CM.marked = 0; }
                CM.seen = fs.PresentCount;
                CM.lpc = lpc;
            }
        }
        CM.us += (now_ms() - t0) * 1000.0;
        CM.n++;
        InterlockedExchange(&s_comp_mode, mode);
        if (mode >= 0 && CM.changed > 0.0) {
            CM.found = t - CM.changed;
            CM.changed = 0.0;
        }
        if (mode != prev) {
            prev = mode;
            if (CM.pending) CM.skipped++;
            CM.pending = 1;
        }
    }
    if (CM.pending && t - CM.logged >= COMP_LOG_MS) {
        int n, k = s_comp_mode;
        n = snprintf(line, sizeof line, "D3D8: presentation mode: %s%s%s (media statistics read %lu times, %.1f us each",
                     k >= 0 && k <= 3 ? k_name[k] : "unknown (", k >= 0 && k <= 3 ? "" : why, k >= 0 && k <= 3 ? "" : ")",
                     CM.n, CM.n ? CM.us / CM.n : 0.0);
        if (n > 0 && n < (int)sizeof line && k >= 0 && CM.found >= 0.0)
            n += snprintf(line + n, sizeof line - n, "; %.0f ms after a change", CM.found);
        if (n > 0 && n < (int)sizeof line && CM.skipped)
            n += snprintf(line + n, sizeof line - n, "; %lu changes not written", CM.skipped);
        if (n > 0 && n < (int)sizeof line - 2) {
            line[n++] = ')'; line[n++] = '\n'; line[n] = 0;
            fputs(line, stderr);
        }
        CM.logged = t;
        CM.pending = 0;
        CM.skipped = 0;
        if (k >= 0) CM.found = -1.0;
    }
}

/* Presenting thread: the next present carries a video frame; the
 * last one was left out. */
static int s_video_next, s_left_out;

void d3d8_sync_video_frame(int on)
{
    s_video_next = on;
}

int d3d8_sync_left_out(void)
{
    return s_left_out;
}

HRESULT d3d8_sync_present(IDXGISwapChain *sc, double *dxgi_ms)
{
    static double last_arrive, arrive_avg;
    double t0, t1, t2, wait_ms = 0.0;
    UINT sync, flags = 0;
    HRESULT hr;
    int video = s_video_next;

    s_video_next = 0;
    s_left_out = 0;
    t0 = now_ms();
    /* The sync interval first (it depends on the arrival only, t0): the 60 Hz
     * display path applies to VSync'ed frames only. */
    switch (s_mode) {
    case D3D8_SYNC_OFF:
        sync = 0;
        flags = s_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        break;
    case D3D8_SYNC_VSYNC:
        sync = 1;
        break;
    case D3D8_SYNC_ADAPTIVE: {
        long hz = s_hz > 0 ? s_hz : 60, cap = s_pace_fps;
        /* Target pace: one frame per refresh, or one per frame of the frame
         * rate limit when it is below the display (a 60 fps limit on a
         * 240 Hz display shows each frame for 4 refreshes: on time). */
        double period = 1000.0 / (double)(cap > 0 && cap < hz ? cap : hz);
        /* Arrival interval of this frame, at Present, without our wait. */
        double d = last_arrive > 0 ? t0 - last_arrive : period;
        if (arrive_avg <= 0) arrive_avg = period;
        if (d >= ADAPTIVE_GAP_MS || video) {
            /* A load or a stall is not a pace: VSync for this frame, and the
             * average starts again from the target, so that the uneven
             * frames right after it (race start after the tips screen) are
             * not taken for a slow game. Nor is a video: its frames
             * come at the file's rate, each one on time; VSync, as the
             * console shows them (late against the game's 60 fps, they all
             * went out at once and could tear). */
            arrive_avg = period;
            InterlockedExchange(&s_adaptive_tears, 0);
            sync = 1;
            break;
        }
        /* Running average of the arrival interval over ~0.5 s of frames at
         * the target pace (weight period / 0.5 s per frame): a short burst
         * of uneven frames barely moves it. A late frame is shown at once
         * (tearing) only while the game stays more than 10 % slower than
         * the target pace; otherwise VSync. */
        arrive_avg += (d - arrive_avg) * (period / ADAPTIVE_AVG_MS);
        InterlockedExchange(&s_adaptive_tears, arrive_avg > ADAPTIVE_SLOW * period);
        if (arrive_avg > ADAPTIVE_SLOW * period && d > period) {
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
    last_arrive = t0;
    if (s60_check(sc, t0, sync)) {
        /* 60 Hz display: planned on its refresh first; a frame left out
         * is never presented (and takes nothing from the latency object) */
        if (!s60_wait(sc, now_ms(), video)) {
            s_left_out = 1;
            if (dxgi_ms) *dxgi_ms = 0.0;
            return S_OK;
        }
        if (s_wait && !s_gate_live && WaitForSingleObjectEx(s_wait, 100, TRUE) == WAIT_TIMEOUT) M.waits_to++;
    } else {
        if (s_wait && !s_gate_live) {
            /* A frame may enter the queue. Bounded timeout: a hidden window or a
             * display turned off must not freeze the presenting thread. */
            if (WaitForSingleObjectEx(s_wait, 100, TRUE) == WAIT_TIMEOUT) M.waits_to++;
        }
        pgrid_wait(sc, now_ms(), video);
    }
    if (s_before_present) s_before_present();   /* the reports done during the waits (O-B) */
    t1 = now_ms();
    wait_ms = t1 - t0;
    hr = IDXGISwapChain_Present(sc, sync, flags);
    sc_ForgetAll();                     /* the runtime, and any overlay hooked on Present, may change bindings there */
    if (S6.on && S6.s_prev > 0.0) {                         /* the refresh planned for this Present */
        UINT lpc = 0;
        if (SUCCEEDED(IDXGISwapChain_GetLastPresentCount(sc, &lpc)) && lpc) {
            S6R[lpc % S60_RING].pc = lpc;
            S6R[lpc % S60_RING].s = S6.s_prev;
        }
    }
    if (s_gate_live) {
        InterlockedIncrement(&s_presented);
        gate_check(sc);
    }
    t2 = now_ms();
    if (dxgi_ms) *dxgi_ms = t2 - t1;
    if (M.on) pacing_note(sc, t2, wait_ms, t2 - t1, sync, flags);
    presentation_mode_note(sc, t2);
    return hr;
}

void d3d8_sync_set_before_present(void (*fn)(void)) { s_before_present = fn; }

void d3d8_sync_release(void)
{
    if (s_gate_thread) {        /* before its handle goes */
        InterlockedExchange(&s_gate_live, 0);
        InterlockedExchange(&s_gate_stop, 1);
        WaitForSingleObject(s_gate_thread, 500);
        CloseHandle(s_gate_thread);
        s_gate_thread = NULL;
        if (s_gate_resync) fprintf(stderr, "D3D8: render gate: queue count corrected %llu times\n", s_gate_resync);
    }
    if (s_wait) { CloseHandle(s_wait); s_wait = NULL; }
    if (CM.m) { IDXGISwapChainMedia_Release(CM.m); CM.m = NULL; }
    InterlockedExchange(&s_comp_mode, -1);
    InterlockedExchange(&s_comp_again, 1);
}
