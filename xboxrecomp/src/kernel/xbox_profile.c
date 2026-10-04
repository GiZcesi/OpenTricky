/*
 * xbox_profile.c -- a sampling profiler for the whole process.
 *
 *   XBOX_PROFILE=START,SECONDS
 *
 * From START seconds after launch, for SECONDS seconds, every thread is
 * briefly suspended about a thousand times a second and its instruction
 * pointer recorded. At the end, each thread's CPU use over the window (from
 * GetThreadTimes) and its hottest addresses are written to stderr:
 *
 *   [PROFILE] thread 1234  cpu 97.3%  samples 9981
 *   [PROFILE]   31.2%  exe+0x0012A4F0
 *   [PROFILE]   12.0%  nvwgf2umx.dll
 *
 * Addresses inside the executable are module offsets; resolve them with
 *   addr2line -f -e "SSX Tricky.exe" 0x14012A4F0   (image base 0x140000000)
 * Addresses in other modules are reported by module name only.
 *
 * Sampling another thread with SuspendThread/GetThreadContext is the only
 * profiler this needs: the executable is Release with -g but has no PDB, so
 * the Windows tools cannot name its functions, and the guest code is one flat
 * sea of generated functions with no frame pointers to walk.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_THREADS 64
#define MAX_SITES   8192

typedef struct {
    DWORD    tid;
    HANDLE   h;
    ULONGLONG cpu0, cpu1;          /* 100 ns units, user + kernel */
    unsigned samples;
    unsigned nsites;
    struct { DWORD64 key; unsigned n; } site[MAX_SITES];
} ProfThread;

static ProfThread *s_threads;
static int s_nthreads;

static ULONGLONG thread_cpu(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) +
           ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

/* Add threads that appeared since the last scan. */
static void scan_threads(DWORD self)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            int i, known = 0;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            for (i = 0; i < s_nthreads; i++) if (s_threads[i].tid == te.th32ThreadID) known = 1;
            if (!known && s_nthreads < MAX_THREADS) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                      THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (h) {
                    ProfThread *t = &s_threads[s_nthreads++];
                    memset(t, 0, sizeof *t);
                    t->tid = te.th32ThreadID;
                    t->h = h;
                    t->cpu0 = thread_cpu(h);
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

/* Modules, listed once before sampling starts: looking a module up while
 * another thread is suspended could deadlock on the loader lock it holds. */
#define MOD_TAG (1ull << 63)
#define VIA_TAG (1ull << 62)   /* with MOD_TAG: system code reached from exe+low bits */
static int s_via = 0;
static int s_all = 0;     /* XBOX_PROFILE=...,all: idle threads too */
static struct { DWORD64 lo, hi; } s_mods[512];
static int s_nmods;

static void list_modules(void)
{
    HMODULE mods[512];
    DWORD need = 0, i;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    for (i = 0; i < need / sizeof(HMODULE) && s_nmods < 512; i++) {
        MODULEINFO mi;
        if (GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) {
            s_mods[s_nmods].lo = (DWORD64)(uintptr_t)mi.lpBaseOfDll;
            s_mods[s_nmods].hi = s_mods[s_nmods].lo + mi.SizeOfImage;
            s_nmods++;
        }
    }
}

/* Inside the executable: the exact address. Elsewhere: the module, so a
 * driver or the kernel shows as one line. */
static DWORD64 site_key(DWORD64 rip, DWORD64 exe_lo, DWORD64 exe_hi)
{
    int i;
    if (rip >= exe_lo && rip < exe_hi) return rip;
    for (i = 0; i < s_nmods; i++)
        if (rip >= s_mods[i].lo && rip < s_mods[i].hi) return MOD_TAG | s_mods[i].lo;
    return MOD_TAG;                                  /* unknown */
}

/* Stall mode (XBOX_PROFILE=...,stall): every sample is also kept with its
 * time, and the report counts only samples inside the windows the renderer
 * marks as stalls (a present interval over 40 ms, see d3d8_device.c). */
#define TRACE_MAX (1u << 21)
typedef struct { LONGLONG t; DWORD64 key; unsigned short th; } TraceSample;
static TraceSample *s_trace;
static unsigned s_ntrace;
static int s_stall = 0;
static LONGLONG s_now;
#define STALL_MAX 4096
static LONGLONG s_stall_win[STALL_MAX][2];
static volatile LONG s_nstall;

void xbox_profile_note_stall(LONGLONG qpc_start, LONGLONG qpc_end)
{
    LONG i = InterlockedIncrement(&s_nstall) - 1;
    if (i < STALL_MAX) { s_stall_win[i][0] = qpc_start; s_stall_win[i][1] = qpc_end; }
}

static void record(ProfThread *t, DWORD64 key)
{
    if (s_stall && s_trace && s_ntrace < TRACE_MAX) {
        s_trace[s_ntrace].t = s_now;
        s_trace[s_ntrace].key = key;
        s_trace[s_ntrace].th = (unsigned short)(t - s_threads);
        s_ntrace++;
    }
    unsigned i;
    t->samples++;
    for (i = 0; i < t->nsites; i++)
        if (t->site[i].key == key) { t->site[i].n++; return; }
    if (t->nsites < MAX_SITES) {
        t->site[t->nsites].key = key;
        t->site[t->nsites].n = 1;
        t->nsites++;
    }
}

static int by_count(const void *a, const void *b)
{
    unsigned x = ((const unsigned *)a)[2], y = ((const unsigned *)b)[2];
    return x < y ? 1 : x > y ? -1 : 0;
}

static DWORD WINAPI prof_main(LPVOID arg)
{
    const char *e = (const char *)arg;
    double start = 20.0, secs = 10.0;
    DWORD self = GetCurrentThreadId(), t_end, last_scan = 0;
    HMODULE exe = GetModuleHandleW(NULL);
    MODULEINFO mi;
    DWORD64 exe_lo = 0, exe_hi = 0;
    LARGE_INTEGER f, w0, w1;
    int i;

    sscanf(e, "%lf,%lf", &start, &secs);
    {   /* XBOX_PROFILE=START,SECONDS,via[N]: attribute system time to the
         * Nth frame inside the executable (default 1, the direct caller). */
        const char *v = strstr(e, ",via");
        s_via = v ? (v[4] >= '1' && v[4] <= '9' ? v[4] - '0' : 1) : 0;
        s_all = strstr(e, ",all") != NULL;
        s_stall = strstr(e, ",stall") != NULL;
        if (s_stall) s_trace = (TraceSample *)malloc(TRACE_MAX * sizeof *s_trace);
    }
    if (GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof mi)) {
        exe_lo = (DWORD64)(uintptr_t)mi.lpBaseOfDll;
        exe_hi = exe_lo + mi.SizeOfImage;
    }
    s_threads = (ProfThread *)calloc(MAX_THREADS, sizeof *s_threads);
    if (!s_threads) return 1;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    Sleep((DWORD)(start * 1000.0));

    list_modules();
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&w0);
    t_end = GetTickCount() + (DWORD)(secs * 1000.0);
    scan_threads(self);
    while ((LONG)(GetTickCount() - t_end) < 0) {
        if (GetTickCount() - last_scan > 500) { scan_threads(self); last_scan = GetTickCount(); }
        {
            LARGE_INTEGER qn;
            QueryPerformanceCounter(&qn);
            s_now = qn.QuadPart;
        }
        for (i = 0; i < s_nthreads; i++) {
            CONTEXT c;
            ProfThread *t = &s_threads[i];
            if (SuspendThread(t->h) == (DWORD)-1) continue;
            memset(&c, 0, sizeof c);
            c.ContextFlags = s_via ? CONTEXT_FULL : CONTEXT_CONTROL;
            if (GetThreadContext(t->h, &c)) {
                DWORD64 key = site_key(c.Rip, exe_lo, exe_hi);
                /* Inside Windows or a driver: also name the executable's code
                 * that led there, by unwinding the suspended thread's stack
                 * with the x64 unwind tables (both the system DLLs and this
                 * MinGW executable carry them) to the first frame inside the
                 * executable. Diagnostic only: unwinding a suspended thread
                 * can in principle meet a lock that thread holds. */
                if ((key & MOD_TAG) && s_via) {
                    CONTEXT u = c;
                    int depth, hits = 0;
                    for (depth = 0; depth < 40; depth++) {
                        DWORD64 image = 0, est = 0;
                        PVOID hd = NULL;
                        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(u.Rip, &image, NULL);
                        if (f) {
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, u.Rip, f, &u, &hd, &est, NULL);
                        } else {
                            DWORD64 ret = 0;
                            SIZE_T got = 0;
                            if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)u.Rsp,
                                                   &ret, 8, &got) || got != 8) break;
                            u.Rip = ret;
                            u.Rsp += 8;
                        }
                        if (!u.Rip) break;
                        if (u.Rip >= exe_lo && u.Rip < exe_hi) {
                            key = MOD_TAG | VIA_TAG | (u.Rip - exe_lo);
                            if (++hits >= s_via) break;   /* the Nth frame in the executable */
                        }
                    }
                }
                record(t, key);
            }
            ResumeThread(t->h);
        }
        Sleep(1);
    }
    QueryPerformanceCounter(&w1);

    if (s_stall && s_trace) {
        /* Rebuild every thread's histogram from the samples inside stalls. */
        unsigned k;
        LONG nw = s_nstall < STALL_MAX ? s_nstall : STALL_MAX;
        for (i = 0; i < s_nthreads; i++) { s_threads[i].samples = 0; s_threads[i].nsites = 0; }
        s_stall = 0;
        for (k = 0; k < s_ntrace; k++) {
            LONG w;
            for (w = 0; w < nw; w++)
                if (s_trace[k].t >= s_stall_win[w][0] && s_trace[k].t <= s_stall_win[w][1]) break;
            if (w < nw) record(&s_threads[s_trace[k].th], s_trace[k].key);
        }
        fprintf(stderr, "[PROFILE] stall mode: %ld stalls marked, samples inside them only\n", (long)nw);
    }

    {
        double wall = (double)(w1.QuadPart - w0.QuadPart) / (double)f.QuadPart;
        fprintf(stderr, "[PROFILE] %.1f s window, %d threads, exe base 0x%llX\n",
                wall, s_nthreads, (unsigned long long)exe_lo);
        for (i = 0; i < s_nthreads; i++) {
            ProfThread *t = &s_threads[i];
            unsigned (*rows)[3];
            unsigned k, shown = 0;
            double cpu;
            t->cpu1 = thread_cpu(t->h);
            cpu = (double)(t->cpu1 - t->cpu0) / 1e7 / wall * 100.0;
            if (!t->samples) continue;
            if (cpu < 2.0 && !s_all) continue;             /* idle threads, unless ,all */
            fprintf(stderr, "[PROFILE] thread %lu  cpu %.1f%%  samples %u\n",
                    (unsigned long)t->tid, cpu, t->samples);
            rows = (unsigned (*)[3])malloc(t->nsites * sizeof *rows);
            if (!rows) continue;
            for (k = 0; k < t->nsites; k++) {
                rows[k][0] = (unsigned)(t->site[k].key & 0xFFFFFFFFu);
                rows[k][1] = (unsigned)(t->site[k].key >> 32);
                rows[k][2] = t->site[k].n;
            }
            qsort(rows, t->nsites, sizeof *rows, by_count);
            for (k = 0; k < t->nsites && shown < (cpu < 2.0 ? 8u : 60u); k++, shown++) {
                DWORD64 key = ((DWORD64)rows[k][1] << 32) | rows[k][0];
                double pct = 100.0 * rows[k][2] / t->samples;
                if ((key & MOD_TAG) && (key & VIA_TAG)) {
                    fprintf(stderr, "[PROFILE]   %5.1f%%  system via exe+0x%08llX\n", pct,
                            (unsigned long long)(key & 0xFFFFFFFFull));
                } else if (key & MOD_TAG) {
                    char name[MAX_PATH] = "?";
                    if (key != MOD_TAG) GetModuleBaseNameA(GetCurrentProcess(),
                                                           (HMODULE)(uintptr_t)(key & ~MOD_TAG), name, sizeof name);
                    fprintf(stderr, "[PROFILE]   %5.1f%%  %s\n", pct, name);
                } else {
                    fprintf(stderr, "[PROFILE]   %5.1f%%  exe+0x%08llX\n", pct,
                            (unsigned long long)(key - exe_lo));
                }
            }
            free(rows);
        }
        fflush(stderr);
    }
    return 0;
}

void xbox_profile_start_from_env(void)
{
    static char spec[64];
    const char *e = getenv("XBOX_PROFILE");
    HANDLE h;
    if (!e || !e[0]) return;
    snprintf(spec, sizeof spec, "%s", e);
    h = CreateThread(NULL, 0, prof_main, spec, 0, NULL);
    if (h) CloseHandle(h);
}
