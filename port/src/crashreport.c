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
#include "settings.h"
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
static char   s_user[2][MAX_PATH]; /* user name (ANSI, UTF-8): blanked in the texts */
static int    s_user_len[2];
static wchar_t s_root[MAX_PATH];   /* the install root (launcher_install_root_w): CrashReports\ */
static ULONGLONG s_start_ms;

/* Blanked in the minidumps (scrub_dump): the user name and the player's
 * folders, each kept in UTF-16 (lower and upper case, for a simple
 * case-insensitive match of any letter, accented or not) and as 8-bit text
 * (UTF-8 and the ANSI code page, both cases). Filled by the main thread
 * only; a count is raised after its entry is complete. */
#define SCRUB_CHARS 512
#define SCRUB_WIDE  24      /* 3 forms of 6 folders, 3 folder names, the user name */
#define SCRUB_BYTES (2 * SCRUB_WIDE)
#define SCRUB_SUFFIX_MIN 4  /* "\bin": the shortest end of a folder blanked on its own */
typedef struct { WCHAR lo[SCRUB_CHARS], up[SCRUB_CHARS]; int len, dump_only; } ScrubWide;
typedef struct { char lo[SCRUB_CHARS * 3], up[SCRUB_CHARS * 3]; int len, dump_only; } ScrubBytes;
static ScrubWide  s_scrub_w[SCRUB_WIDE];
static ScrubBytes s_scrub_b[SCRUB_BYTES];
static volatile LONG s_scrub_nw, s_scrub_nb;

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
static int upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int is_path_end(char c)
{
    return c == 0 || c == '\r' || c == '\n' || c == '"' || c == '\'' || c == '\t'
        || c == '<' || c == '>' || c == '|' || c == '*' || c == '?' || c == ',' || c == ';';
}

/* Paths the player chose, whose files keep no name in the texts (anonymize):
 * the save and HD pack folders, and the disc image itself, as 8-bit text
 * (UTF-8 and ANSI). Filled by the main thread before the game starts; a
 * count is raised after its entry is complete. */
#define PRIVATE_PATHS 12
typedef struct { char s[MAX_PATH * 3]; int len, disc; } PrivatePath;
static PrivatePath s_private[PRIVATE_PATHS];
static volatile LONG s_nprivate;

static void private_path_add(const WCHAR *w, int disc)
{
    static const UINT cps[2] = { CP_UTF8, CP_ACP };
    int c, k, n;
    for (c = 0; c < 2 && s_nprivate < PRIVATE_PATHS; c++) {
        PrivatePath *e = &s_private[s_nprivate];
        n = WideCharToMultiByte(cps[c], 0, w, -1, e->s, (int)sizeof e->s, NULL, NULL) - 1;
        while (n > 0 && (e->s[n - 1] == '\\' || e->s[n - 1] == '/')) n--;
        if (n < 3) continue;
        for (k = 0; k < s_nprivate; k++)
            if (s_private[k].len == n && s_private[k].disc == disc && !memcmp(s_private[k].s, e->s, (size_t)n)) break;
        if (k < s_nprivate) continue;
        e->len = n;
        e->disc = disc;
        MemoryBarrier();
        InterlockedIncrement(&s_nprivate);
    }
}

static int path_char_eq(int a, int b)
{
    if (a == '/') a = '\\';
    if (b == '/') b = '\\';
    return lower(a) == lower(b);
}

/* A path s (n bytes, its folders dl bytes) the player named: 2 = it starts
 * with the disc image's path, 1 = its folder is a save or HD pack folder or
 * under one, 0 = neither (a file of the game: its name is kept). */
static int private_file(const char *s, size_t dl, size_t n)
{
    int count = (int)s_nprivate, k, found = 0;
    MemoryBarrier();
    for (k = 0; k < count; k++) {
        const PrivatePath *e = &s_private[k];
        size_t L = (size_t)e->len, j;
        if (L > (e->disc ? n : dl)) continue;
        for (j = 0; j < L && path_char_eq((unsigned char)s[j], (unsigned char)e->s[j]); j++) ;
        if (j < L) continue;
        if (e->disc) return 2;
        if (L == dl || s[L] == '\\' || s[L] == '/') found = 1;
    }
    return found;
}

/* The length of the save or HD pack folder that s (n bytes) starts with when
 * text, not a deeper path, follows it ("D:\Packs HD (ini) XBOX_..."), or 0:
 * the folder alone is hidden, the rest of the line kept. */
static size_t private_folder_at(const char *s, size_t n)
{
    int count = (int)s_nprivate, k;
    size_t best = 0;
    MemoryBarrier();
    for (k = 0; k < count; k++) {
        const PrivatePath *e = &s_private[k];
        size_t L = (size_t)e->len, j;
        int c;
        if (e->disc || L > n || L <= best) continue;
        for (j = 0; j < L && path_char_eq((unsigned char)s[j], (unsigned char)e->s[j]); j++) ;
        if (j < L) continue;
        c = L < n ? lower((unsigned char)s[L]) : ' ';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80
            || c == '\\' || c == '/' || c == '.' || c == '_' || c == '-') continue;
        best = L;
    }
    return best;
}

/* A file's name ("SSX.iso", "music.inf (read)"), not a folder's: a dot, not
 * the first character, then 1 to 5 ASCII letters or digits (a letter at
 * least) that end the name or a word. Not: "v1.2 Saves", "Mr. Smith".
 * Returns the offset of the last such dot (the extension), 0 if none. */
static size_t file_ext(const char *s, size_t n)
{
    size_t d, k, ext = 0;
    for (d = 1; d < n; d++) {
        int letter = 0;
        if (s[d] != '.') continue;
        for (k = d + 1; k < n; k++) {
            int c = lower((unsigned char)s[k]);
            if (c >= 'a' && c <= 'z') letter = 1;
            else if (c < '0' || c > '9') break;
        }
        if (letter && k - d - 1 <= 5 && (k == n || s[k] != '_')) ext = d;
    }
    return ext;
}

/* Copy src into dst, removing what could identify the player: every absolute
 * path loses its folders and keeps its last component only when that is the
 * name of a file of the game ("D:\Games\x\SSX.iso" -> "<path>\SSX.iso",
 * "D:\Games\x" -> "<path>"); a file the player named keeps its extension
 * and what follows ("<path>\<disc image>.iso (ISO)", "<path>\<file>.dat");
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
            size_t pl = private_folder_at(src + i, end - i);
            size_t ext = file_ext(src + last + 1, end - last - 1);
            int priv = ext ? private_file(src + i, last - i, end - i) : 0;
            if (pl) {                           /* "<path> (ini) XBOX_..." */
                if (o + 16 >= cap) break;
                memcpy(dst + o, "<path>", 6);
                o += 6;
                i += pl;
                continue;
            }
            if (last > i + 2 || (unc && last > i + 1) || (end > last + 1 && (!ext || priv))) {
                static const char tag[] = "<path>\\";
                if (o + 32 >= cap) break;
                memcpy(dst + o, tag, sizeof tag - 1);
                if (ext) {                      /* "<path>\SSX.iso" */
                    o += sizeof tag - 1;
                    i = last + 1;
                    if (priv) {                 /* "<path>\<disc image>.iso" */
                        const char *name = priv == 2 ? "<disc image>" : "<file>";
                        memcpy(dst + o, name, strlen(name));
                        o += strlen(name);
                        i += ext;
                    }
                } else {                        /* a folder: "<path>" */
                    o += sizeof tag - 2;
                    i = end;
                }
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

/* The 8-bit form of a string in code page cp, both cases. When the two cases
 * do not have the same length there, the string as written, with its ASCII
 * letters in either case. Same-entry duplicates (ASCII strings are the same
 * in UTF-8 and ANSI) are dropped. */
static void scrub_add_bytes(UINT cp, const WCHAR *orig, const ScrubWide *w)
{
    ScrubBytes *b;
    int lo_n, up_n, k;
    if (s_scrub_nb >= SCRUB_BYTES) return;
    b = &s_scrub_b[s_scrub_nb];
    lo_n = WideCharToMultiByte(cp, 0, w->lo, w->len, b->lo, (int)sizeof b->lo, NULL, NULL);
    up_n = WideCharToMultiByte(cp, 0, w->up, w->len, b->up, (int)sizeof b->up, NULL, NULL);
    if (lo_n != up_n || lo_n <= 0) {
        lo_n = WideCharToMultiByte(cp, 0, orig, w->len, b->lo, (int)sizeof b->lo, NULL, NULL);
        if (lo_n <= 0) return;
        for (k = 0; k < lo_n; k++) {
            b->up[k] = (char)upper((unsigned char)b->lo[k]);
            b->lo[k] = (char)lower((unsigned char)b->lo[k]);
        }
    }
    if (lo_n < 3) return;
    for (k = 0; k < s_scrub_nb; k++)
        if (s_scrub_b[k].len == lo_n && !memcmp(s_scrub_b[k].lo, b->lo, (size_t)lo_n)
            && !memcmp(s_scrub_b[k].up, b->up, (size_t)lo_n) && s_scrub_b[k].dump_only == w->dump_only) return;
    b->len = lo_n;
    b->dump_only = w->dump_only;
    MemoryBarrier();
    InterlockedIncrement(&s_scrub_nb);
}

/* One string to blank in the minidump, n UTF-16 units (3 at least); also
 * in the texts of the report unless dump_only. */
static void scrub_add(const WCHAR *s, int n, int dump_only)
{
    ScrubWide *w;
    int k;
    if (n < 3 || n >= SCRUB_CHARS || s_scrub_nw >= SCRUB_WIDE) return;
    w = &s_scrub_w[s_scrub_nw];
    /* Simple case mapping, the same on every system (not the user's locale). */
    if (LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, s, n, w->lo, SCRUB_CHARS) != n
        || LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, s, n, w->up, SCRUB_CHARS) != n) {
        memcpy(w->lo, s, (size_t)n * sizeof(WCHAR));
        memcpy(w->up, s, (size_t)n * sizeof(WCHAR));
    }
    for (k = 0; k < s_scrub_nw; k++)
        if (s_scrub_w[k].len == n && s_scrub_w[k].dump_only == dump_only
            && !memcmp(s_scrub_w[k].lo, w->lo, (size_t)n * sizeof(WCHAR))) return;
    w->len = n;
    w->dump_only = dump_only;
    scrub_add_bytes(CP_UTF8, s, w);
    scrub_add_bytes(CP_ACP, s, w);     /* what the ...A functions return */
    MemoryBarrier();
    InterlockedIncrement(&s_scrub_nw);
}

/* The part of a path that every form of it contains: after the drive letter
 * (D:\x, \\?\D:\x, \??\D:\x and \Device\HarddiskVolume3\x all hold \x), or
 * from the server name of a share (\\server\share, \\?\UNC\server\share,
 * \Device\Mup\server\share). */
static const WCHAR *path_tail(const WCHAR *p)
{
    if ((p[0] == L'\\' && (p[1] == L'\\' || p[1] == L'?') && p[2] == L'?' && p[3] == L'\\')) {
        p += 4;
        if (!_wcsnicmp(p, L"UNC\\", 4)) return p + 3;
    } else if (p[0] == L'\\' && p[1] == L'\\') {
        return p + 1;
    }
    if (p[0] && p[1] == L':') return p + 2;
    return p;
}

/* A folder of the player: its tail (above), as given, in its 8.3 form
 * ("PROGRA~1", on volumes that keep short names) and as Windows resolves it
 * (8.3 names expanded, subst drives and links followed). */
static void scrub_add_folder(const WCHAR *path)
{
    WCHAR fin[SCRUB_CHARS];
    const WCHAR *t;
    HANDLE h;
    int n;
    if (!path || !path[0]) return;
    t = path_tail(path);
    n = (int)wcslen(t);
    while (n > 0 && t[n - 1] == L'\\') n--;
    scrub_add(t, n, 0);
    n = (int)GetShortPathNameW(path, fin, SCRUB_CHARS);
    if (n > 0 && n < SCRUB_CHARS) {
        t = path_tail(fin);
        n = (int)wcslen(t);
        while (n > 0 && t[n - 1] == L'\\') n--;
        scrub_add(t, n, 0);
    }
    h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    n = (int)GetFinalPathNameByHandleW(h, fin, SCRUB_CHARS, FILE_NAME_NORMALIZED | VOLUME_NAME_NONE);
    CloseHandle(h);
    if (n > 0 && n < SCRUB_CHARS) {
        while (n > 0 && fin[n - 1] == L'\\') n--;
        scrub_add(fin, n, 0);
    }
}

/* The last name of a folder the player chose, alone (5 characters or more):
 * a thread's stack can hold it without its path (seen: the HD pack folder's
 * name in UTF-16, from a folder listing). In the minidumps only: in the
 * texts, a word of the game could match it. */
static void scrub_add_leaf(const WCHAR *path)
{
    const WCHAR *leaf;
    int n;
    if (!path || !path[0]) return;
    n = (int)wcslen(path);
    while (n > 0 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) n--;
    for (leaf = path + n; leaf > path && leaf[-1] != L'\\' && leaf[-1] != L'/' && leaf[-1] != L':'; leaf--) ;
    if (path + n - leaf >= 5) scrub_add(leaf, (int)(path + n - leaf), 1);
}

void crashreport_private_folder(const char *utf8)
{
    WCHAR w[SCRUB_CHARS];
    if (!s_on || !utf8 || !utf8[0]) return;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, SCRUB_CHARS) > 0) {
        scrub_add_folder(w);
        scrub_add_leaf(w);
        private_path_add(w, 0);     /* its files' names: hidden in the texts */
    }
}

void crashreport_private_file_acp(const char *path)
{
    WCHAR w[SCRUB_CHARS], full[SCRUB_CHARS], *slash;
    DWORD n;
    if (!s_on || !path || !path[0]) return;
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, w, SCRUB_CHARS) <= 0) return;
    private_path_add(w, 1);         /* the disc image's name: hidden in the texts */
    n = GetFullPathNameW(w, SCRUB_CHARS, full, NULL);           /* a relative name too */
    if (n == 0 || n >= SCRUB_CHARS) return;
    if ((slash = wcsrchr(full, L'\\')) == NULL) return;
    *slash = 0;
    scrub_add_folder(full);
    scrub_add_leaf(full);
}

static void gather_user(void)
{
    WCHAR wn[128];
    DWORD n = 128;
    int wl = 0;
    char an[128];
    DWORD na = sizeof an;
    if (GetUserNameA(an, &na) && na > 1) {
        memcpy(s_user[0], an, na - 1);
        s_user_len[0] = (int)(na - 1);
    }
    if (GetUserNameW(wn, &n) && n > 1) {
        int k = WideCharToMultiByte(CP_UTF8, 0, wn, (int)(n - 1), s_user[1], sizeof s_user[1] - 1, NULL, NULL);
        if (k > 0) s_user_len[1] = k;
        wl = (int)(n - 1);
    }
    /* For the minidump: the game's folder, the install root (the game's
     * folder or the parent of its bin\) and the profile folder (the dump
     * can hold them, seen: the game's folder as \Device\HarddiskVolumeN\...
     * on a thread's stack), then the user name. */
    {
        WCHAR p[SCRUB_CHARS], *slash;
        DWORD k = GetModuleFileNameW(NULL, p, SCRUB_CHARS);
        if (k > 0 && k < SCRUB_CHARS && (slash = wcsrchr(p, L'\\')) != NULL) {
            *slash = 0;
            scrub_add_folder(p);
        }
        scrub_add_folder(s_root);
        k = GetEnvironmentVariableW(L"USERPROFILE", p, SCRUB_CHARS);
        if (k > 0 && k < SCRUB_CHARS) scrub_add_folder(p);
        scrub_add(wn, wl, 0);
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

static int is_absolute_path(const char *v, size_t n)
{
    return (n >= 3 && (v[0] | 32) >= 'a' && (v[0] | 32) <= 'z' && v[1] == ':' && (v[2] == '\\' || v[2] == '/'))
        || (n >= 2 && (v[0] == '\\' || v[0] == '/') && v[1] == v[0]);
}

/* What a path value is, without any of its names, for settings.txt:
 * "<absolute path: folder found>", "<relative path: not found>"... A relative
 * one is looked up from base (the install root, as the launcher does). A
 * network path is not looked up (a share that does not answer would hold
 * the game's start). */
static size_t describe_path(const char *v, size_t n, UINT cp, const WCHAR *base, char *dst, size_t cap)
{
    WCHAR w[MAX_PATH], full[2 * MAX_PATH + 2];
    const char *what = "not checked";
    int net = n >= 2 && (v[0] == '\\' || v[0] == '/') && v[1] == v[0];
    int abs = net || (n >= 2 && v[1] == ':');      /* as launcher_resolve_path reads it */
    int len, k;
    if (!net) {
        what = "not found";
        k = MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, v, (int)n, w, MAX_PATH - 1);
        if (k <= 0 && cp == CP_UTF8) k = MultiByteToWideChar(CP_ACP, 0, v, (int)n, w, MAX_PATH - 1);
        if (k > 0) {
            const WCHAR *look = w;
            DWORD attr;
            DWORD old_mode = 0;
            w[k] = 0;
            if (!abs && base && base[0]) {
                _snwprintf(full, sizeof full / sizeof full[0], L"%ls\\%ls", base, w);
                full[sizeof full / sizeof full[0] - 1] = 0;
                look = full;
            }
            SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);  /* no "insert a disk" box */
            attr = GetFileAttributesW(look);
            SetThreadErrorMode(old_mode, NULL);
            if (attr != INVALID_FILE_ATTRIBUTES)
                what = (attr & FILE_ATTRIBUTE_DIRECTORY) ? "folder found" : "file found";
        }
    }
    len = snprintf(dst, cap, "<%s path: %s>", net ? "network" : abs ? "absolute" : "relative", what);
    if (len < 0) return 0;
    return (size_t)len < cap ? (size_t)len : cap - 1;
}

/* settings.ini text (or one XBOX_* variable), line by line: the value of a
 * path setting (settings.h SETTING_PATH) or any value that is an absolute
 * path becomes what it is (describe_path), never its folders' or file's
 * names; every other line is anonymized. Strings in code page cp. */
static size_t settings_lines(const char *src, size_t n, UINT cp, char *dst, size_t cap)
{
    char section[64] = "", key[64];
    size_t i = 0, o = 0;
    if (n >= 3 && !memcmp(src, "\xEF\xBB\xBF", 3)) i = 3;     /* UTF-8 byte order mark */
    while (i < n && o + 80 < cap) {
        size_t le = i, next, a = i, ve;
        const char *eq;
        while (le < n && src[le] != '\n') le++;
        next = le < n ? le + 1 : le;
        while (a < le && (src[a] == ' ' || src[a] == '\t')) a++;
        if (a < le && src[a] == '[') {
            size_t b = a + 1, kl = 0;
            while (b < le && src[b] != ']' && kl + 1 < sizeof section) section[kl++] = src[b++];
            section[kl] = 0;
        } else if (a < le && src[a] != ';' && src[a] != '#'
                   && (eq = memchr(src + a, '=', le - a)) != NULL) {
            size_t vs = (size_t)(eq - src) + 1, kl = (size_t)(eq - src) - a;
            int id;
            while (kl && (src[a + kl - 1] == ' ' || src[a + kl - 1] == '\t')) kl--;
            if (kl >= sizeof key) kl = sizeof key - 1;
            memcpy(key, src + a, kl);
            key[kl] = 0;
            while (vs < le && (src[vs] == ' ' || src[vs] == '\t')) vs++;
            ve = le;
            while (ve > vs && (src[ve - 1] == ' ' || src[ve - 1] == '\t' || src[ve - 1] == '\r')) ve--;
            id = settings_find(section, key);
            if (ve > vs && ((id >= 0 && settings_def(id)->type == SETTING_PATH) || is_absolute_path(src + vs, ve - vs))
                && o + (vs - i) + 64 + (next - ve) < cap) {
                memcpy(dst + o, src + i, vs - i);                  /* "Key=" */
                o += vs - i;
                o += describe_path(src + vs, ve - vs, cp, s_root, dst + o, cap - o);
                memcpy(dst + o, src + ve, next - ve);              /* the line's end */
                o += next - ve;
                dst[o] = 0;
                i = next;
                continue;
            }
        }
        o += anonymize(src + i, next - i, dst + o, cap - o);
        i = next;
    }
    return o;
}

/* settings.ini (launcher.h) and the XBOX_* variables, without the player's
 * paths (settings_lines). */
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
            len += settings_lines(s_raw, got, CP_UTF8, s_settings + len, sizeof s_settings - len - 1024);
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
            len += settings_lines(e, el, CP_ACP, s_settings + len, sizeof s_settings - len - 2);
            s_settings[len++] = '\n';
            s_settings[len] = 0;
        }
        FreeEnvironmentStringsA(env);
    }
}

size_t crashreport_private_text(const char *src, size_t n, int ansi, char *dst, size_t cap)
{
    static volatile LONG names_once;
    if (!dst || cap < 96) return 0;
    dst[0] = 0;
    /* Reports off (XBOX_CRASH_REPORT=0): the install root and the user name
     * were not gathered at start; once, here. */
    if (!s_on && InterlockedCompareExchange(&names_once, 1, 0) == 0) {
        launcher_install_root_w(s_root, MAX_PATH);
        gather_user();
    }
    return settings_lines(src, n, ansi ? CP_ACP : CP_UTF8, dst, cap);
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

/* <install root>\CrashReports\<date>-<kind> (where the launcher looks), else
 * under %LOCALAPPDATA%. */
static BOOL make_report_dir(const wchar_t *kind)
{
    SYSTEMTIME t;
    int attempt;
    GetLocalTime(&t);
    for (attempt = 0; attempt < 2; attempt++) {
        if (attempt == 0) {
            if (!s_root[0]) continue;
            _snwprintf(s_dir, MAX_PATH, L"%ls\\CrashReports\\%04u-%02u-%02u_%02u-%02u-%02u_%ls",
                       s_root, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, kind);
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

/* The 8-bit strings of s_scrub_b (UTF-8 and ANSI) blanked with '*' in
 * p[0, n), longest first, each also from any of its separators to its end
 * (see scrub_dump). For the minidump and, with text set, the texts of the
 * report, where a match must also end a name ("\bin" leaves "\bink.dll"). */
static void scrub_bytes(unsigned char *p, size_t n, int text)
{
    int nb = (int)s_scrub_nb, done[SCRUB_BYTES] = { 0 };
    int pass, o;
    size_t i, j, k;
    MemoryBarrier();
    for (pass = 0; pass < nb; pass++) {
        const ScrubBytes *b = NULL;
        size_t L;
        for (o = 0; o < nb; o++)
            if (!done[o] && (!b || s_scrub_b[o].len > b->len)) b = &s_scrub_b[o];
        done[b - s_scrub_b] = 1;
        if (text && b->dump_only) continue;
        for (k = 0; k < (size_t)b->len; k++) {
            const unsigned char *lo = (const unsigned char *)b->lo + k, *up = (const unsigned char *)b->up + k;
            if (k && ((size_t)b->len - k < SCRUB_SUFFIX_MIN || lo[0] != '\\')) continue;
            L = (size_t)b->len - k;
            for (i = 0; i + L <= n; i++) {
                for (j = 0; j < L && (p[i + j] == lo[j] || p[i + j] == up[j]); j++) ;
                if (j == L && text && i + L < n) {
                    int c = lower(p[i + L]);
                    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') j = 0;
                }
                if (j == L) {
                    memset(p + i, '*', L);
                    i += L - 1;
                }
            }
        }
    }
}

/* Blank the player's folders and user name (s_scrub_w / s_scrub_b) in the
 * written minidump, at any byte offset. The flags above keep module names
 * without folders, but a dump of this process was seen holding the game's
 * and the save folder's paths in memory it captures (the game's folder as
 * \Device\HarddiskVolumeN\..., on a thread's stack). Longest strings first, so
 * that a folder holding the user name is blanked whole. A path can also stay
 * as a fragment, its start overwritten by a later, shorter string (seen:
 * "...ojects\OpenTricky\...\bin" on a stack): each folder is also blanked
 * from any of its separators to its end ("\x\y\z", "\y\z", "\z"). */
static void scrub_dump(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE map;
    unsigned char *p;
    LARGE_INTEGER sz;
    size_t n, i, j, k;
    if (f == INVALID_HANDLE_VALUE) return;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0 || sz.QuadPart > (256LL << 20)) { CloseHandle(f); return; }
    map = CreateFileMappingW(f, NULL, PAGE_READWRITE, 0, 0, NULL);
    p = map ? (unsigned char *)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    n = (size_t)sz.QuadPart;
    if (p) {
        int nw = (int)s_scrub_nw, done[SCRUB_WIDE] = { 0 };
        int pass, o;
        MemoryBarrier();
        /* UTF-16LE, then 8-bit: each time the longest string left. */
        for (pass = 0; pass < nw; pass++) {
            const ScrubWide *w = NULL;
            size_t L;
            for (o = 0; o < nw; o++)
                if (!done[o] && (!w || s_scrub_w[o].len > w->len)) w = &s_scrub_w[o];
            done[w - s_scrub_w] = 1;
            for (k = 0; k < (size_t)w->len; k++) {
                const WCHAR *lo = w->lo + k, *up = w->up + k;
                if (k && ((size_t)w->len - k < SCRUB_SUFFIX_MIN || lo[0] != L'\\')) continue;       /* the whole string, then each "\..." end */
                L = (size_t)w->len - k;
                for (i = 0; i + 2 * L <= n; i++) {
                    for (j = 0; j < L; j++) {
                        WCHAR c = (WCHAR)(p[i + 2 * j] | (p[i + 2 * j + 1] << 8));
                        if (c != lo[j] && c != up[j]) break;
                    }
                    if (j == L) {
                        for (j = 0; j < L; j++) { p[i + 2 * j] = '*'; p[i + 2 * j + 1] = 0; }
                        i += 2 * L - 1;
                    }
                }
            }
        }
        scrub_bytes(p, n, 0);
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

static DWORD WINAPI flush_output_thread(LPVOID arg)
{
    (void)arg;
    fflush(stdout);
    fflush(stderr);
    return 0;
}

/* The buffered output (log file, or the pipe of the in-memory log) out of
 * the runtime's buffers before the log is read or the process ends. On a
 * thread of its own, waited for at most 500 ms: the thread that crashed or
 * froze may hold a stream's lock, and this one must go on with the report.
 * FALSE when it did not end in time (or could not start): the log then
 * lacks what the buffers held. */
static BOOL flush_output(void)
{
    HANDLE t = CreateThread(NULL, 64 * 1024, flush_output_thread, NULL, 0, NULL);
    BOOL done = FALSE;
    if (t) {
        done = WaitForSingleObject(t, 500) == WAIT_OBJECT_0;
        CloseHandle(t);
    }
    return done;
}

static void write_common_files(void)
{
    size_t n;
    if (!flush_output() && (s_log_path[0] || s_pipe_read))
        tadd("Log:       the last %s of output may be missing (an output stream stayed locked "
             "or could not be flushed)\n", s_log_path[0] ? "second" : "lines");
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
        scrub_bytes((unsigned char *)s_clean, m, 1);   /* the player's folders in any other form */
        write_file(s_dir, L"log.txt", s_clean, m);
    } else {
        static const char none[] =
            "No log was captured: the output went to a redirected stream (test run)\n"
            "or the capture is off.\n";
        write_file(s_dir, L"log.txt", none, sizeof none - 1);
    }
    scrub_bytes((unsigned char *)s_settings, strlen(s_settings), 1);
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

static volatile LONGLONG s_hold_until;     /* now_ms() up to which a stall is a wait (crashreport_hold) */

void crashreport_hold(unsigned ms)
{
    InterlockedExchange64(&s_hold_until, ms ? (LONGLONG)(now_ms() + ms) : 0);
}

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
            flush_output();
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

    {   /* a wait on purpose (crashreport_hold): counted as progress, without the "then progress again" line */
        LONGLONG h = InterlockedCompareExchange64(&s_hold_until, 0, 0);
        if (h && (LONGLONG)t < h) { stall_from = t; reported = 0; return; }
    }
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
                flush_output();
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
    launcher_install_root_w(s_root, MAX_PATH);    /* found once here: no file call at crash time */
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
    /* Buffered (4 KB): unbuffered, every print was a pipe write on the
     * thread printing, the pump's included. The reports empty the buffers
     * before they read the log (flush_output). */
    if (freopen("NUL", "w", stdout)) { _dup2(fd, _fileno(stdout)); setvbuf(stdout, NULL, _IOFBF, 4096); }
    if (freopen("NUL", "w", stderr)) { _dup2(fd, _fileno(stderr)); setvbuf(stderr, NULL, _IOFBF, 4096); }
    _close(fd);                              /* the two duplicates stay open */
}

/* The HD pack folder, as the launcher hands it to the game (XBOX_HD_TEXTURES
 * = its absolute path, or "0"): blanked in the minidumps, its files' names
 * hidden in the texts. */
static void gather_hd_pack(void)
{
    const char *hd = getenv("XBOX_HD_TEXTURES");
    WCHAR w[SCRUB_CHARS];
    if (hd && is_absolute_path(hd, strlen(hd)) && MultiByteToWideChar(CP_ACP, 0, hd, -1, w, SCRUB_CHARS) > 0) {
        scrub_add_folder(w);
        scrub_add_leaf(w);
        private_path_add(w, 0);
    }
}

void crashreport_game_start(void)
{
    const char *test;
    if (!s_on) return;
    gather_system();
    gather_hd_pack();
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
