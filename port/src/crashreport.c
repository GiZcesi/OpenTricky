/*
 * crashreport.c -- a report folder for crashes and freezes (crashreport.h).
 *
 * Design notes.
 *
 * - The report is written by its own thread, which exists from the start:
 *   the faulting thread only records what it saw, wakes it and waits. That
 *   keeps a stack overflow reportable (the faulting thread has almost no
 *   stack left) and is the pattern MiniDumpWriteDump asks for (dumping the
 *   calling thread's own stack is unreliable).
 * - Everything the report needs that can be gathered early (settings, XBOX_*
 *   variables, system description) is gathered at the game's start into
 *   static buffers, so the crash path allocates as little as possible: the
 *   faulting thread may hold the heap lock. The minidump itself still uses
 *   the heap; if it deadlocks, the faulting thread stops waiting after
 *   REPORT_WAIT_MS and the old one-line report and message still happen.
 * - Minidump flags: MiniDumpNormal (threads, registers, stacks, module list,
 *   exception) + MiniDumpWithThreadInfo (thread times and start addresses)
 *   + MiniDumpFilterMemory (stack contents reduced to the pointers a stack
 *   walk needs: no strings, so no paths) + MiniDumpFilterModulePaths
 *   (module names without their folders). Deliberately not: data segments
 *   (the executable's globals include the settings with their paths), code
 *   segments (the executable is the game), indirect / full memory (the
 *   emulated Xbox memory is the game's), process thread data (the PEB holds
 *   the environment and the command line). A few hundred KB.
 * - The freeze watchdog looks at what the title produces: draws translated
 *   from its push buffer, and video frames it decodes (the video player
 *   writes pixels straight into guest memory and issues no draws). If
 *   neither changes for XBOX_HANG_SECONDS, the game is frozen as far as the
 *   player can tell. The pump's timer presents are not progress: they repeat
 *   the last image and keep going in a freeze. Time is the unbiased
 *   interrupt time (it stops while the PC sleeps), a gap of more than a few
 *   seconds between two samples restarts the count (debugger, a diagnostic
 *   that suspended the threads), and nothing fires under a debugger.
 */
#include "crashreport.h"
#include "launcher.h"
#include "version.h"

#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <io.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdint.h>
#ifdef __GNUC__
#include <cpuid.h>
#endif

#ifndef SSX_BUILD_ID
#define SSX_BUILD_ID "unknown"
#endif
#define ISSUES_URL      "https://github.com/GiZcesi/OpenTricky/issues"
#define ISSUES_URL_W    L"https://github.com/GiZcesi/OpenTricky/issues"
#define REPORT_WAIT_MS  30000u      /* faulting thread waits this long at most */
#define RING_SIZE       (128u * 1024u)
#define MAX_HANG_REPORTS 3

/* Progress counters of the runtime (xboxrecomp). */
uint32_t pgraph_d3d11_draw_count(void);          /* nv2a_pgraph_d3d11.c */
unsigned d3d8_GuestFramebufferLockCount(void);   /* d3d8_device.c */
extern volatile uint32_t g_pump_beats;           /* xbox_memory_layout.c */
extern volatile unsigned g_last_loc;             /* generated code */

/* ── State ───────────────────────────────────────────────────── */

static int    s_on;                 /* XBOX_CRASH_REPORT != 0 */
static int    s_player;             /* dialogs allowed */
static HANDLE s_thread;             /* report + watchdog thread */
static DWORD  s_thread_id;
static HANDLE s_crash_event, s_done_event;
static volatile LONG s_crash_claimed;
static volatile LONG s_game_started;
static unsigned s_hang_ms = 30000;

static EXCEPTION_POINTERS *s_ep;    /* set by the faulting thread */
static DWORD  s_fault_tid;
static const char *s_details;
static int    s_written;
static volatile int s_fatal;         /* stack overflow: the report thread ends the process */
static wchar_t s_dir[MAX_PATH];     /* the last report folder */

/* Log: either a file (tail read at report time) or the ring buffer. */
static char   s_log_path[MAX_PATH];
static HANDLE s_pipe_read;
static char   s_ring[RING_SIZE];
static volatile uint64_t s_ring_total;     /* bytes ever written */
static CRITICAL_SECTION s_ring_lock;

/* Gathered at the game's start (no allocation at crash time). */
static char   s_settings[24 * 1024];
static char   s_system[2048];
static char   s_user[4][MAX_PATH]; /* user name (ANSI, UTF-8), profile folder, game folder:
                                    * blanked in the minidump; the name also in the texts */
static int    s_user_len[4];
static ULONGLONG s_start_ms;

/* Scratch for the report (only one report is written at a time). */
static char   s_text[24 * 1024];
static char   s_raw[RING_SIZE + 1];
static char   s_clean[RING_SIZE + RING_SIZE / 2];

/* ── Small helpers ───────────────────────────────────────────── */

static ULONGLONG now_ms(void)
{
    ULONGLONG t100 = 0;
    /* Unbiased: does not advance while the PC sleeps. */
    if (QueryUnbiasedInterruptTime(&t100)) return t100 / 10000u;
    return GetTickCount64();
}

static int env_int(const char *name, int dflt)
{
    const char *e = getenv(name);
    return (e && *e) ? atoi(e) : dflt;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int is_path_end(char c)
{
    return c == 0 || c == '\r' || c == '\n' || c == '"' || c == '\'' || c == '\t'
        || c == '<' || c == '>' || c == '|' || c == '*' || c == '?' || c == ',' || c == ';';
}

/* Copy src into dst, removing what could identify the player: every absolute
 * path keeps only its last component ("D:\Games\x\SSX.iso" -> "<path>\SSX.iso"),
 * then every occurrence of the user name is blanked with '*'. Returns the
 * length written. */
static size_t anonymize(const char *src, size_t n, char *dst, size_t cap)
{
    size_t i = 0, o = 0, k, u;
    char *tmp = dst;
    if (cap == 0) return 0;
    while (i < n && o + 8 < cap) {
        int drive = i + 2 < n && ((src[i] | 32) >= 'a' && (src[i] | 32) <= 'z')
                 && src[i + 1] == ':' && (src[i + 2] == '\\' || src[i + 2] == '/')
                 && (i == 0 || !((src[i - 1] | 32) >= 'a' && (src[i - 1] | 32) <= 'z'));
        int unc = i + 1 < n && src[i] == '\\' && src[i + 1] == '\\'
                 && (i == 0 || src[i - 1] == ' ' || src[i - 1] == '"' || src[i - 1] == '=');
        if (drive || unc) {
            size_t end = i, last = 0;
            while (end < n && !is_path_end(src[end])) {
                if (src[end] == '\\' || src[end] == '/') last = end;
                end++;
            }
            if (last > i + 2 || (unc && last > i + 1)) {
                static const char tag[] = "<path>\\";
                memcpy(dst + o, tag, sizeof tag - 1);
                o += sizeof tag - 1;
                i = last + 1;
                continue;
            }
        }
        dst[o++] = src[i++];
    }
    /* User name, case-insensitive, in place. */
    for (u = 0; u < 2; u++) {
        size_t L = (size_t)s_user_len[u];
        if (L < 3) continue;
        for (k = 0; k + L <= o; k++) {
            size_t j;
            for (j = 0; j < L && lower((unsigned char)tmp[k + j]) == lower((unsigned char)s_user[u][j]); j++) ;
            if (j == L) {
                for (j = 0; j < L; j++) tmp[k + j] = '*';
                k += L - 1;
            }
        }
    }
    dst[o] = 0;
    return o;
}

static BOOL write_file(const wchar_t *dir, const wchar_t *name, const char *data, size_t n)
{
    wchar_t path[MAX_PATH];
    HANDLE h;
    DWORD w = 0;
    BOOL ok;
    _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, name);
    path[MAX_PATH - 1] = 0;
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(h, data, (DWORD)n, &w, NULL) && w == (DWORD)n;
    CloseHandle(h);
    return ok;
}

/* Append to s_text. */
static void tadd(const char *fmt, ...)
{
    size_t len = strlen(s_text);
    va_list ap;
    if (len + 1 >= sizeof s_text) return;
    va_start(ap, fmt);
    vsnprintf(s_text + len, sizeof s_text - len, fmt, ap);
    va_end(ap);
}

/* ── Log capture ─────────────────────────────────────────────── */

static void ring_put(const char *p, size_t n)
{
    uint64_t t;
    size_t at;
    if (n > RING_SIZE) { p += n - RING_SIZE; n = RING_SIZE; }
    EnterCriticalSection(&s_ring_lock);
    t = s_ring_total;
    at = (size_t)(t % RING_SIZE);
    if (at + n <= RING_SIZE) memcpy(s_ring + at, p, n);
    else {
        memcpy(s_ring + at, p, RING_SIZE - at);
        memcpy(s_ring, p + (RING_SIZE - at), n - (RING_SIZE - at));
    }
    s_ring_total = t + n;
    LeaveCriticalSection(&s_ring_lock);
}

static DWORD WINAPI pipe_reader(LPVOID arg)
{
    static char buf[4096];
    DWORD n;
    (void)arg;
    while (ReadFile(s_pipe_read, buf, sizeof buf, &n, NULL) && n)
        ring_put(buf, n);
    return 0;
}

/* The ring, oldest first, into s_raw. Lets the reader drain the pipe first. */
static size_t ring_snapshot(void)
{
    size_t n = 0, at;
    uint64_t t;
    int i;
    for (i = 0; i < 20; i++) {          /* up to 200 ms for the reader */
        DWORD avail = 0;
        if (!PeekNamedPipe(s_pipe_read, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
        Sleep(10);
    }
    for (i = 0; i < 50 && !TryEnterCriticalSection(&s_ring_lock); i++) Sleep(10);
    t = s_ring_total;
    if (t <= RING_SIZE) { n = (size_t)t; memcpy(s_raw, s_ring, n); }
    else {
        at = (size_t)(t % RING_SIZE);
        memcpy(s_raw, s_ring + at, RING_SIZE - at);
        memcpy(s_raw + (RING_SIZE - at), s_ring, at);
        n = RING_SIZE;
    }
    if (i < 50) LeaveCriticalSection(&s_ring_lock);
    return n;
}

/* The last RING_SIZE bytes of the log file, into s_raw. */
static size_t file_tail(void)
{
    HANDLE h = CreateFileA(s_log_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz, off;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return 0; }
    off.QuadPart = sz.QuadPart > (LONGLONG)RING_SIZE ? sz.QuadPart - RING_SIZE : 0;
    SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    ReadFile(h, s_raw, RING_SIZE, &got, NULL);
    CloseHandle(h);
    return got;
}

/* ── Gathered at the start ───────────────────────────────────── */

static void gather_user(void)
{
    WCHAR wn[128];
    DWORD n = 128;
    char an[128];
    DWORD na = sizeof an;
    if (GetUserNameA(an, &na) && na > 1) {
        memcpy(s_user[0], an, na - 1);
        s_user_len[0] = (int)(na - 1);
    }
    if (GetUserNameW(wn, &n) && n > 1) {
        int k = WideCharToMultiByte(CP_UTF8, 0, wn, (int)(n - 1), s_user[1], sizeof s_user[1] - 1, NULL, NULL);
        if (k > 0) s_user_len[1] = k;
    }
    /* The profile folder and the game's folder, as whole strings: the dump
     * can hold them (seen: the save and game folders), so they go first. */
    {
        DWORD k = GetEnvironmentVariableA("USERPROFILE", s_user[2], MAX_PATH);
        char *slash;
        if (k > 0 && k < MAX_PATH) s_user_len[2] = (int)k;
        k = GetModuleFileNameA(NULL, s_user[3], MAX_PATH);
        if (k > 0 && k < MAX_PATH && (slash = strrchr(s_user[3], '\\')) != NULL) {
            *slash = 0;
            s_user_len[3] = (int)strlen(s_user[3]);
        }
    }
}

static void gather_system(void)
{
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW v;
    MEMORYSTATUSEX m;
    SYSTEM_INFO si;
    char cpu[64] = "?";
    size_t len = 0;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn rgv = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;

    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (rgv) rgv(&v);
#ifdef __GNUC__
    {
        unsigned r[12], i;
        if (__get_cpuid_max(0x80000000u, NULL) >= 0x80000004u) {
            for (i = 0; i < 3; i++)
                __get_cpuid(0x80000002u + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
            memcpy(cpu, r, 48);
            cpu[48] = 0;
        }
    }
#endif
    m.dwLength = sizeof m;
    GlobalMemoryStatusEx(&m);
    GetNativeSystemInfo(&si);
    len += (size_t)snprintf(s_system + len, sizeof s_system - len,
        "Windows:   %lu.%lu build %lu\n"
        "CPU:       %s (%lu logical processors)\n"
        "RAM:       %llu MB\n",
        (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion,
        (unsigned long)v.dwBuildNumber, cpu, (unsigned long)si.dwNumberOfProcessors,
        (unsigned long long)(m.ullTotalPhys / (1024 * 1024)));
    /* GPU: the display devices Windows lists (no DXGI needed here). */
    {
        DISPLAY_DEVICEA dd;
        DWORD i;
        for (i = 0; i < 8 && len + 128 < sizeof s_system; i++) {
            memset(&dd, 0, sizeof dd);
            dd.cb = sizeof dd;
            if (!EnumDisplayDevicesA(NULL, i, &dd, 0)) break;
            if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
            len += (size_t)snprintf(s_system + len, sizeof s_system - len,
                                    "GPU:       %s%s\n", dd.DeviceString,
                                    (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "");
        }
    }
}

/* settings.ini (launcher.h) and the XBOX_* variables, anonymized. */
static void gather_settings(void)
{
    char ini[MAX_PATH];
    size_t len = 0;
    LPCH env;

    len += (size_t)snprintf(s_settings + len, sizeof s_settings - len, "== %s settings (settings.ini) ==\n",
                            "SSX Tricky");
    launcher_config_path(ini, sizeof ini);
    {
        HANDLE h;
        h = CreateFileA(ini, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            ReadFile(h, s_raw, 12 * 1024, &got, NULL);
            CloseHandle(h);
            len += anonymize(s_raw, got, s_settings + len, sizeof s_settings - len - 1024);
        } else {
            len += (size_t)snprintf(s_settings + len, sizeof s_settings - len, "(no settings.ini)\n");
        }
    }
    len += (size_t)snprintf(s_settings + len, sizeof s_settings - len, "\n== XBOX_* variables in effect ==\n");
    env = GetEnvironmentStringsA();
    if (env) {
        const char *e;
        for (e = env; *e; e += strlen(e) + 1) {
            size_t el = strlen(e);
            if (_strnicmp(e, "XBOX_", 5) || len + el + 16 >= sizeof s_settings) continue;
            len += anonymize(e, el, s_settings + len, sizeof s_settings - len - 2);
            s_settings[len++] = '\n';
            s_settings[len] = 0;
        }
        FreeEnvironmentStringsA(env);
    }
}

/* ── Report folder ───────────────────────────────────────────── */

static BOOL make_dir_chain(wchar_t *path)
{
    wchar_t *p;
    for (p = path + 3; *p; p++) {
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(path, NULL);
            *p = L'\\';
        }
    }
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* <exe dir>\CrashReports\<date>-<kind>, else under %LOCALAPPDATA%. */
static BOOL make_report_dir(const wchar_t *kind)
{
    wchar_t base[MAX_PATH], *slash;
    SYSTEMTIME t;
    DWORD k = GetModuleFileNameW(NULL, base, MAX_PATH);
    int attempt;
    GetLocalTime(&t);
    for (attempt = 0; attempt < 2; attempt++) {
        if (attempt == 0) {
            if (k == 0 || k >= MAX_PATH || !(slash = wcsrchr(base, L'\\'))) continue;
            *slash = 0;
            _snwprintf(s_dir, MAX_PATH, L"%ls\\CrashReports\\%04u-%02u-%02u_%02u-%02u-%02u_%ls",
                       base, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, kind);
        } else {
            wchar_t la[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) return FALSE;
            _snwprintf(s_dir, MAX_PATH, L"%ls\\SSX Tricky\\CrashReports\\%04u-%02u-%02u_%02u-%02u-%02u_%ls",
                       la, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, kind);
        }
        s_dir[MAX_PATH - 1] = 0;
        if (make_dir_chain(s_dir)) {
            /* Probe that it is writable (Program Files: the folder may exist
             * through virtualization or not be writable at all). */
            if (write_file(s_dir, L"report.txt", "", 0)) return TRUE;
        }
    }
    return FALSE;
}

/* Blank the game folder, the profile folder and the user name in the written
 * minidump (ANSI / UTF-8 and UTF-16 for ASCII strings). The flags above keep
 * module names without folders, but a dump of this process was seen holding
 * the game's and the save folder's paths in memory it captures. */
static void scrub_dump(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE map;
    unsigned char *p;
    LARGE_INTEGER sz;
    size_t n, i, j;
    if (f == INVALID_HANDLE_VALUE) return;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0 || sz.QuadPart > (256LL << 20)) { CloseHandle(f); return; }
    map = CreateFileMappingW(f, NULL, PAGE_READWRITE, 0, 0, NULL);
    p = map ? (unsigned char *)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    n = (size_t)sz.QuadPart;
    if (p) {
        static const int order[4] = { 3, 2, 0, 1 };   /* longest strings first */
        size_t o;
        for (o = 0; o < 4; o++) {
            size_t L = (size_t)s_user_len[order[o]];
            const char *s_pat = s_user[order[o]];
            if (L < 3) continue;
            for (i = 0; i + 2 * L <= n; i++) {
                /* UTF-16LE (ASCII names) */
                for (j = 0; j < L && p[i + 2 * j + 1] == 0
                     && lower(p[i + 2 * j]) == lower((unsigned char)s_pat[j]); j++) ;
                if (j == L) { for (j = 0; j < L; j++) p[i + 2 * j] = '*'; continue; }
                /* 8-bit */
                for (j = 0; j < L && lower(p[i + j]) == lower((unsigned char)s_pat[j]); j++) ;
                if (j == L) for (j = 0; j < L; j++) p[i + j] = '*';
            }
        }
        UnmapViewOfFile(p);
    }
    if (map) CloseHandle(map);
    CloseHandle(f);
}

typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);
static MiniDumpWriteDumpFn s_mdwd;

static BOOL write_dump(const wchar_t *name, EXCEPTION_POINTERS *ep, DWORD tid)
{
    wchar_t path[MAX_PATH];
    MINIDUMP_EXCEPTION_INFORMATION mei;
    HANDLE h;
    BOOL ok;
    if (!s_mdwd) return FALSE;
    _snwprintf(path, MAX_PATH, L"%ls\\%ls", s_dir, name);
    path[MAX_PATH - 1] = 0;
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    mei.ThreadId = tid;
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    ok = s_mdwd(GetCurrentProcess(), GetCurrentProcessId(), h,
                (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo |
                                MiniDumpFilterMemory | MiniDumpFilterModulePaths),
                ep ? &mei : NULL, NULL, NULL);
    CloseHandle(h);
    if (ok) scrub_dump(path);
    return ok;
}

static void write_common_files(void)
{
    size_t n;
    if (s_log_path[0]) n = file_tail();
    else if (s_pipe_read) n = ring_snapshot();
    else n = 0;
    if (n) {
        /* Start at a line boundary when the tail was cut. */
        size_t start = 0, m;
        if (n >= RING_SIZE) {
            while (start < n && s_raw[start] != '\n') start++;
            if (start < n) start++;
        }
        m = anonymize(s_raw + start, n - start, s_clean, sizeof s_clean);
        write_file(s_dir, L"log.txt", s_clean, m);
    } else {
        static const char none[] =
            "No log was captured: the output went to a redirected stream (test run)\n"
            "or the capture is off.\n";
        write_file(s_dir, L"log.txt", none, sizeof none - 1);
    }
    write_file(s_dir, L"settings.txt", s_settings, strlen(s_settings));
}

static void report_header(const char *kind)
{
    HMODULE self = GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)self;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)self + dos->e_lfanew);
    SYSTEMTIME t;
    ULONGLONG up = now_ms() - s_start_ms;
    GetLocalTime(&t);
    s_text[0] = 0;
    tadd("SSX Tricky -- %s report\n", kind);
    tadd("Attach this whole folder to a bug report: %s\n\n", ISSUES_URL);
    tadd("Date:      %04u-%02u-%02u %02u:%02u:%02u\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    tadd("Build:     OpenTricky " OT_VERSION ", %s (exe timestamp 0x%08lX)\n", SSX_BUILD_ID, (unsigned long)nt->FileHeader.TimeDateStamp);
    if (s_game_started) tadd("Game time: %llu s since the game started\n", up / 1000u);
    else                tadd("Game time: not started (launcher or start-up)\n");
    tadd("%s", s_system);
    tadd("Log:       %s\n\n", s_log_path[0] ? "log file (tail in log.txt)"
                              : s_pipe_read ? "in-memory ring buffer (log.txt)" : "not captured");
}

/* ── Crash ───────────────────────────────────────────────────── */

static const char *exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
    case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR:         return "in-page error (disc or file read)";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "float divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "float invalid operation";
    case EXCEPTION_INT_OVERFLOW:          return "integer overflow";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    case 0xC0000409u:                     return "stack buffer overrun / fail fast";
    case 0xE06D7363u:                     return "C++ exception";
    default:                              return "exception";
    }
}

static void write_crash_report(void)
{
    const EXCEPTION_RECORD *r = s_ep->ExceptionRecord;
    HMODULE self = GetModuleHandleW(NULL);
    s_written = 0;
    if (!make_report_dir(L"crash")) return;
    report_header("crash");
    tadd("What:      %s (0x%08lX)\n", exception_name(r->ExceptionCode), (unsigned long)r->ExceptionCode);
    tadd("Where:     rva 0x%08llX in the executable (addr2line -e \"SSX Tricky.exe\" 0x%llX)\n",
         (unsigned long long)((const char *)r->ExceptionAddress - (const char *)self),
         0x140000000ull + (unsigned long long)((const char *)r->ExceptionAddress - (const char *)self));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        tadd("Access:    %s 0x%llX\n", r->ExceptionInformation[0] == 8 ? "execute"
                                      : r->ExceptionInformation[0] ? "write to" : "read from",
             (unsigned long long)r->ExceptionInformation[1]);
    tadd("Thread:    %lu\n", (unsigned long)s_fault_tid);
    if (s_details && s_details[0]) tadd("%s", s_details);
    write_common_files();
    s_written = write_dump(L"crash.dmp", s_ep, s_fault_tid);
    if (!s_written) tadd("\n(minidump not written: error %lu)\n", (unsigned long)GetLastError());
    write_file(s_dir, L"report.txt", s_text, strlen(s_text));
    s_written = 1;
}

void crashreport_crash_dialog(DWORD code, const wchar_t *dir)
{
    static wchar_t msg[2 * MAX_PATH + 512];
    _snwprintf(msg, sizeof msg / sizeof msg[0],
               L"SSX Tricky stopped because of an error (0x%08lX).\n\n"
               L"A crash report was saved in:\n%ls\n\n"
               L"Please attach the files in this folder to a bug report (" ISSUES_URL_W L").\n\n"
               L"Open the folder now?", (unsigned long)code, dir);
    msg[sizeof msg / sizeof msg[0] - 1] = 0;
    if (MessageBoxW(NULL, msg, L"SSX Tricky",
                    MB_YESNO | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND) == IDYES)
        crashreport_open_folder(dir);
}

void crashreport_fatal_overflow(EXCEPTION_POINTERS *ep)
{
    if (!s_on || !s_thread || GetCurrentThreadId() == s_thread_id) return;
    if (InterlockedCompareExchange(&s_crash_claimed, 1, 0) != 0) {
        WaitForSingleObject(s_done_event, REPORT_WAIT_MS);
        return;
    }
    s_ep = ep;
    s_fault_tid = GetCurrentThreadId();
    s_details = "Stack overflow: reported before any other handler ran (no stack left);\n"
                "guest registers not read.\n";
    s_fatal = 1;
    SetEvent(s_crash_event);
    /* The report thread ends the process; if it cannot, give up. */
    WaitForSingleObject(s_done_event, REPORT_WAIT_MS);
}

int crashreport_on_crash(EXCEPTION_POINTERS *ep, const char *details,
                         wchar_t *dir_out, size_t dir_out_len)
{
    if (!s_on || !s_thread) return 0;
    if (GetCurrentThreadId() == s_thread_id) return 0;     /* the reporter itself */
    if (InterlockedCompareExchange(&s_crash_claimed, 1, 0) != 0) {
        /* Another thread is already reporting: wait for it, then die. */
        WaitForSingleObject(s_done_event, REPORT_WAIT_MS);
        return 0;
    }
    s_ep = ep;
    s_fault_tid = GetCurrentThreadId();
    s_details = details;
    SetEvent(s_crash_event);
    if (WaitForSingleObject(s_done_event, REPORT_WAIT_MS) != WAIT_OBJECT_0 || !s_written)
        return 0;
    if (dir_out && dir_out_len) {
        wcsncpy(dir_out, s_dir, dir_out_len - 1);
        dir_out[dir_out_len - 1] = 0;
    }
    fprintf(stderr, "[CRASHREPORT] report written\n");
    return 1;
}

/* ── Freeze watchdog ─────────────────────────────────────────── */

static void write_hang_report(unsigned stalled_s, uint32_t draws, unsigned video)
{
    uint32_t loc[16], pump0, pump1;
    int i;
    if (!make_report_dir(L"freeze")) return;
    /* Where the guest is spinning (or sleeping): a few samples of the last
     * label any guest thread stamped, and whether the pump still turns. */
    pump0 = g_pump_beats;
    for (i = 0; i < 16; i++) { loc[i] = g_last_loc; Sleep(100); }
    pump1 = g_pump_beats;
    report_header("freeze");
    tadd("What:      nothing new on screen for %u s (no draw from the game, no video frame)\n", stalled_s);
    tadd("Counters:  draws %u, video frames %u, pump %s (%u turns in 1.6 s)\n",
         (unsigned)draws, video, pump1 != pump0 ? "running" : "stopped", (unsigned)(pump1 - pump0));
    {
        uint32_t any = 0;
        for (i = 0; i < 16; i++) any |= loc[i];
        if (any) {
            tadd("Last guest labels (any thread, 100 ms apart):\n ");
            for (i = 0; i < 16; i++) tadd(" %08X", (unsigned)loc[i]);
            tadd("\n");
        } else {
            tadd("Last guest labels: none (no label stamps in this build)\n");
        }
    }
    tadd("The minidump has every thread's host stack: the game thread's tells where it is stuck.\n");
    write_common_files();
    if (!write_dump(L"hang.dmp", NULL, 0))
        tadd("\n(minidump not written: error %lu)\n", (unsigned long)GetLastError());
    write_file(s_dir, L"report.txt", s_text, strlen(s_text));
    fprintf(stderr, "[CRASHREPORT] freeze report written (%u s without progress)\n", stalled_s);

    if (s_player) {
        static wchar_t msg[MAX_PATH + 512];
        int r;
        _snwprintf(msg, MAX_PATH + 512,
            L"SSX Tricky has not shown anything new for %u seconds and seems to be frozen.\n\n"
            L"A report was saved in:\n%ls\n\n"
            L"Please attach the files in this folder to a bug report (" ISSUES_URL_W L").\n\n"
            L"Close the game and open the folder?\n"
            L"(No: keep waiting.)", stalled_s, s_dir);
        msg[MAX_PATH + 511] = 0;
        r = MessageBoxW(NULL, msg, L"SSX Tricky", MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        if (r == IDYES) {
            crashreport_open_folder(s_dir);
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
}

static void watchdog_tick(void)
{
    static ULONGLONG last_sample, stall_from;
    static uint32_t last_draws;
    static unsigned last_video;
    static int reported, reports, simulate;
    static ULONGLONG sim_at;
    ULONGLONG t = now_ms();
    uint32_t d = pgraph_d3d11_draw_count();
    unsigned v = d3d8_GuestFramebufferLockCount();
    int progress;

    if (!sim_at) {
        const char *e = getenv("XBOX_CRASH_TEST");
        simulate = e && !_stricmp(e, "hang");
        sim_at = s_start_ms + (ULONGLONG)env_int("XBOX_CRASH_TEST_DELAY", 20) * 1000u;
    }
    progress = d != last_draws || v != last_video;
    if (simulate && t >= sim_at) progress = 0;     /* XBOX_CRASH_TEST=hang */
    last_draws = d;
    last_video = v;

    /* Not scheduled for a while (threads suspended by a diagnostic, a
     * debugger, a stalled machine): that time is not the game's. */
    if (last_sample && t - last_sample > 5000) stall_from = t;
    last_sample = t;
    if (!stall_from) stall_from = t;

    if (progress || IsDebuggerPresent()) {
        if (t - stall_from >= 5000)
            fprintf(stderr, "[WATCHDOG] nothing new on screen for %.1f s, then progress again%s\n",
                    (double)(t - stall_from) / 1000.0, reported ? " (after a freeze report)" : "");
        stall_from = t;
        reported = 0;
        return;
    }
    if (!reported && reports < MAX_HANG_REPORTS && s_hang_ms && t - stall_from >= s_hang_ms) {
        reported = 1;
        reports++;
        write_hang_report((unsigned)((t - stall_from) / 1000u), d, v);
        last_sample = now_ms();      /* the dialog's time is not a scheduling gap */
    }
}

/* ── Report thread ───────────────────────────────────────────── */

static DWORD WINAPI report_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        DWORD r = WaitForSingleObject(s_crash_event, 1000);
        if (r == WAIT_OBJECT_0) {
            DWORD code = s_ep->ExceptionRecord->ExceptionCode;
            write_crash_report();
            if (s_fatal) {
                /* Stack overflow: the faulting thread cannot run the filter
                 * or a message box, so this thread ends the process. */
                if (s_player && s_written) crashreport_crash_dialog(code, s_dir);
                TerminateProcess(GetCurrentProcess(), code);
            }
            SetEvent(s_done_event);
            return 0;
        }
        if (s_game_started) watchdog_tick();
    }
}

/* ── Test triggers (XBOX_CRASH_TEST) ─────────────────────────── */

static volatile int s_zero;

#ifdef __GNUC__
__attribute__((noinline))
#endif
static int recurse(volatile char *prev)
{
    volatile char pad[4096];
    pad[0] = prev ? prev[0] : 1;
    return recurse(pad) + pad[1];
}

static DWORD WINAPI crash_test_thread(LPVOID arg)
{
    const char *what = (const char *)arg;
    Sleep((DWORD)env_int("XBOX_CRASH_TEST_DELAY", 20) * 1000u);
    fprintf(stderr, "[CRASHREPORT] XBOX_CRASH_TEST=%s: crashing now\n", what);
    if (!_stricmp(what, "div0")) return (DWORD)(100 / s_zero);
    if (!_stricmp(what, "stack")) return (DWORD)recurse(NULL);
    *(volatile int *)(uintptr_t)s_zero = 1;         /* "av" */
    return 0;
}

/* ── Public ──────────────────────────────────────────────────── */

void crashreport_install(void)
{
    HMODULE dbg;
    s_on = env_int("XBOX_CRASH_REPORT", 1) != 0;
    if (!s_on) return;
    s_hang_ms = (unsigned)env_int("XBOX_HANG_SECONDS", 30) * 1000u;
    InitializeCriticalSection(&s_ring_lock);
    /* From System32 only (no DLL planted beside the game). */
    dbg = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dbg) s_mdwd = (MiniDumpWriteDumpFn)(void *)GetProcAddress(dbg, "MiniDumpWriteDump");
    gather_user();
    s_crash_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_done_event  = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_start_ms = now_ms();
    if (s_crash_event && s_done_event)
        s_thread = CreateThread(NULL, 256 * 1024, report_thread, NULL, 0, &s_thread_id);
    if (!s_thread) s_on = 0;
}

void crashreport_set_output(int player, const char *log_path, int output_redirected)
{
    HANDLE wr = NULL;
    int fd;
    if (!s_on) return;
    s_player = player;
    if (log_path && log_path[0]) {
        strncpy(s_log_path, log_path, sizeof s_log_path - 1);
        return;
    }
    if (output_redirected) return;          /* the harness keeps its output */
    /* No log file and nowhere to print: keep the output in memory. */
    if (!CreatePipe(&s_pipe_read, &wr, NULL, 64 * 1024)) { s_pipe_read = NULL; return; }
    /* The reader first: without it a full pipe would block every print. */
    {
        HANDLE t = CreateThread(NULL, 64 * 1024, pipe_reader, NULL, 0, NULL);
        if (!t) { CloseHandle(wr); CloseHandle(s_pipe_read); s_pipe_read = NULL; return; }
        CloseHandle(t);
    }
    fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_TEXT);
    if (fd < 0) { CloseHandle(wr); return; }  /* reader ends (broken pipe) */
    /* Buffered (4 KB): the runtime prints ~30 KB/s while racing (the [VSH]
     * statistics of the pump), and unbuffered that was a pipe write per line
     * on the pump thread. The crash paths flush stderr before the report
     * (VEH, filter); a freeze report can miss the last < 4 KB. */
    if (freopen("NUL", "w", stdout)) { _dup2(fd, _fileno(stdout)); setvbuf(stdout, NULL, _IOFBF, 4096); }
    if (freopen("NUL", "w", stderr)) { _dup2(fd, _fileno(stderr)); setvbuf(stderr, NULL, _IOFBF, 4096); }
    _close(fd);                              /* the two duplicates stay open */
}

void crashreport_game_start(void)
{
    const char *test;
    if (!s_on) return;
    gather_system();
    gather_settings();
    s_start_ms = now_ms();
    InterlockedExchange(&s_game_started, 1);
    test = getenv("XBOX_CRASH_TEST");
    if (test && *test && _stricmp(test, "hang")) {
        static char what[16];
        HANDLE t;
        strncpy(what, test, sizeof what - 1);
        t = CreateThread(NULL, 1024 * 1024, crash_test_thread, what, 0, NULL);
        if (t) CloseHandle(t);
    }
    fprintf(stderr, "[CRASHREPORT] on (freeze watchdog %u s%s), build %s\n", s_hang_ms / 1000u,
            s_hang_ms ? "" : ": off", SSX_BUILD_ID);
}

void crashreport_open_folder(const wchar_t *dir)
{
    if (dir && dir[0])
        ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}
