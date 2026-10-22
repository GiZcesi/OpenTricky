/**
 * YOUR_GAME_NAME - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       YOUR_GAME_NAME
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include "apu/aci_mmio.h"
#include "kernel/xbox_diag.h"
#include "apu/apu.h"
#include <windows.h>
#include <mmsystem.h>   /* timeBeginPeriod/timeEndPeriod -- see the note in WinMain */
#include <shellapi.h>   /* CommandLineToArgvW -- without this its implicit
                         * declaration returns int and truncates the returned
                         * pointer to 32 bits, which faults on first use. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <strings.h>
#endif

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include "kernel/xbox_xdvdfs.h"
#include "launcher.h"
#include "ot_window.h"
#include "version.h"
#include "hostui.h"
#include "controls.h"
#include "crashreport.h"
/* NV2A DAC palette trap (nv2a/nv2a_mmio_hook.c). */
#include <stdbool.h>
bool nv2a_dac_trap_install(unsigned char *mem_base);
bool nv2a_dac_handle_mmio(PCONTEXT ctx, unsigned int fault_xbox_va, int is_write);
#include "pass_tags.h"
/* The load address of the executable, for turning a return address into an offset addr2line
 * can resolve: the linker-provided __ImageBase on Windows, the Mach-O header elsewhere. */
#if defined(_WIN32)
#define IMAGE_BASE_DECL extern void *__ImageBase;
#define IMAGE_BASE_PTR ((char *)&__ImageBase)
#else
#define IMAGE_BASE_DECL
#define IMAGE_BASE_PTR ((char *)GetModuleHandleA(NULL))
#endif
#include "netplay/np_cmdlog.h"
#include "netplay/np_ghost.h"
#include "netplay/np_net.h"
#include "netplay/np_racebench.h"
/* Fork: read by the generated code (fix_deadflags.py), set in main(). One bit per
 * revived branch: 0x1 sub_00019468, 0x2 sub_000BE680, 0x4 sub_001281DB. */
int g_fix_deadflags = 0x7;
/* Read by the generated code (fix_kickwait.py), set in main(). */
int g_fix_kickwait = 1;
/* Read by the generated code (fix_riderlist.py), set in main(). */
int g_fix_riderlist = 1;
/* Fork: read by the generated code (fix_speechhold.py), set in main(). */
int g_fix_speechhold = 1;
/* Fork: read by fps_cap_skip_emitters() (fix_crowdemit.py), set in main(). */
int g_fix_crowdemit = 1;
/* Fork: read by the generated code of 0x10E280 / 0x115EE0 (fix_fadeparity.py) and by
 * fps_cap_skip_fade(), set in main(). */
int g_fix_fade = 1;
/* List sizes stored by the generated code after the split (0x2FBC5). */
uint32_t g_riderlist_ai_n, g_riderlist_pl_n;
/* Called by the generated code (fix_riderlist.py) when, in sub_0002F800,
 * the rider lists disagree with the race's counts (kind 0) or a guard stops the
 * walk (kind 1 = AI list short, 2 = Player list short). One stderr line per kind
 * and per race (a new race = another race object or its tick going back), so a
 * long test run sees it in the log without a line every 6 ticks. Game thread only. */
void fork_riderlist_note(uint32_t kind, uint32_t race, uint32_t tick, uint32_t riders,
                         uint32_t ai_exp, uint32_t pl_exp, uint32_t at)
{
    static const char *const what[3] = { "lists differ from the race counts",
                                         "guard: AI list short, walk stopped",
                                         "guard: Player list short, walk stopped" };
    static uint32_t last_race, last_tick, done, hits;
    if (kind > 2) kind = 0;
    if (race != last_race || tick < last_tick) done = 0;
    last_race = race; last_tick = tick;
    hits++;
    if (done & (1u << kind)) return;
    done |= 1u << kind;
    fprintf(stderr, "[RIDERLIST] %s: ai=%u/%u players=%u/%u riders=%u at=%u tick=%u race=0x%08X (hits=%u)\n",
            what[kind], (unsigned)g_riderlist_ai_n, (unsigned)ai_exp, (unsigned)g_riderlist_pl_n,
            (unsigned)pl_exp, (unsigned)riders, (unsigned)at, (unsigned)tick, (unsigned)race, (unsigned)hits);
    fflush(stderr);
}
#include "fps_cap.h"
#include "aspect.h"
#include "drawdist.h"
#include "terrain_seams.h"
#include "ctlscheme.h"
#include "ps2legend.h"
#include "fullfade.h"
#include "pumplag.h"
#include "strmwalk.h"
#include "chantfix.h"
#include "hud_anchor.h"
#include "nv2a/nv2a_hdtex.h"
#include "ticktrace.h"

/* Host display and EEPROM hooks; see d3d8_device.c and
 * kernel_bridge.c. */
void d3d8_SetHostDisplay(unsigned render_w, unsigned render_h,
                         int widescreen, int fullscreen);
void d3d8_SetHostAspect(double aspect);
void xbox_SetVideoFlags(uint32_t eeprom_video_flags);
void d3d8_SetMsaa(int samples);
/* Split 3D / overlay -- the translator's phase callback. */
int  d3d8_post_split_wanted(void);
void d3d8_PassPhaseChanged(int old_phase, int new_phase, unsigned tag);
void pgraph_d3d11_set_pass_phase_callback(void (*fn)(int, int, unsigned));
void d3d8_SetAnisotropy(int n);
void d3d8_SetShowFps(int on);
void d3d8_RequestScreenshot(const wchar_t *path);
extern void (*g_diag_shot_hook)(const wchar_t *path);   /* xbox_diag.c `shot` */
extern int (*g_diag_press_hook)(const char *name, int ms); /* xbox_diag.c `press` */
int xinput_hle_press(const char *name, int ms);            /* xapi_input_hle.c */
extern void (*g_diag_drawlog_hook)(int frames);            /* xbox_diag.c `drawlog` */
void nv2a_drawlog_arm(int frames);                         /* nv2a_pgraph_d3d11.c */
extern void (*g_diag_skipprog_hook)(uint32_t hash);        /* xbox_diag.c `skipprog` */
void nv2a_skipprog_set(uint32_t h);                        /* nv2a_pgraph_d3d11.c */
extern void (*g_diag_ignored_hook)(void);                  /* xbox_diag.c `ignored` */
void nv2a_ignored_dump(void);                              /* nv2a_pgraph_d3d11.c */

/* Where this run's log goes (empty: no log), and whether a crash should be
 * explained in a message box -- only for the player, never in a test run,
 * where a dialog would hang the harness. */
static char s_log_path[MAX_PATH];
static BOOL s_crash_dialog = FALSE;

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ──
 * Thread-local (see xbox_memory_layout.c) -- must match exactly. Crash-handler
 * code below reads whichever thread's state it runs on, which is correct: a
 * crash is always reported from the thread that actually faulted. */

extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __thread uint32_t g_ebx, g_esi, g_edi;
extern __thread uint32_t g_seh_ebp;
void recomp_probe_init_from_env(void);
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x00154218  /* SSX Tricky XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "game\\default.xbe"
#define YOUR_GAME_DIR            "game"

/* Where the emulated Xbox hard disk lives: TDATA (per-title data) and UDATA
 * (user saves) are created underneath it. Resolved relative to the
 * executable, so it is stable no matter what directory the game is launched
 * from. Kept as its own "hdd" folder rather than dropped loose beside the
 * binary, and deliberately *not* inside the read-only asset tree (game\),
 * so save data is never mixed in with game files. */
#define YOUR_GAME_SAVE_DIR       "hdd"

/* Default disc image, looked for next to the executable when no image is
 * given on the command line. Optional: with no ISO present the game runs
 * from the extracted files in YOUR_GAME_DIR exactly as before. */
#define YOUR_GAME_ISO            "SSXTricky_USA.iso"

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/*
 * Return the first command-line argument that looks like a disc image, or
 * NULL. Lets a different game/image be pointed at without a rebuild --
 * useful now that the disc is read straight out of an ISO.
 */
#if !defined(_WIN32)
/* Command line as the C runtime gives it (set by main below). */
static int    g_argc;
static char **g_argv;

static const char *find_iso_argument(void)
{
    int i;
    /* the path right after --direct is the disc image whatever it is called (.iso, .xiso, .img ...) */
    for (i = 1; i + 1 < g_argc; i++)
        if (!strcmp(g_argv[i], "--direct") && g_argv[i + 1][0] && g_argv[i + 1][0] != '-') return g_argv[i + 1];
    for (i = 1; i < g_argc; i++) {
        size_t len = strlen(g_argv[i]);
        if (len > 4 && strcasecmp(g_argv[i] + len - 4, ".iso") == 0) return g_argv[i];
    }
    return NULL;
}

/* TRUE if `flag` (e.g. L"--direct", ASCII) is on the command line. */
static BOOL has_flag(const wchar_t *flag)
{
    int i;
    for (i = 1; i < g_argc; i++) {
        const char *a = g_argv[i];
        const wchar_t *f = flag;
        while (*a && *f && (wchar_t)(unsigned char)*a == *f) { a++; f++; }
        if (!*a && !*f) return TRUE;
    }
    return FALSE;
}
#else
static const char *find_iso_argument(void)
{
    static char arg[MAX_PATH];
    int i, n = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &n);

    if (!wargv) return NULL;
    for (i = 1; i < n; i++) {
        size_t len = wcslen(wargv[i]);
        if (len > 4 && _wcsicmp(wargv[i] + len - 4, L".iso") == 0) {
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, arg, sizeof(arg), NULL, NULL);
            LocalFree(wargv);
            return arg;
        }
    }
    LocalFree(wargv);
    return NULL;
}

/* TRUE if `flag` (e.g. L"--direct") is on the command line. */
static BOOL has_flag(const wchar_t *flag)
{
    int i, n = 0;
    BOOL found = FALSE;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wargv) return FALSE;
    for (i = 1; i < n && !found; i++)
        if (_wcsicmp(wargv[i], flag) == 0) found = TRUE;
    LocalFree(wargv);
    return found;
}
#endif /* _WIN32 */

/*
 * No good disc image in settings.ini (a first start by double-click, a disc
 * image moved): start the launcher, OpenTricky.exe in the install root (this
 * executable's folder, or the parent of bin\), where the player chooses one;
 * without it, say what is missing. Returns the game's exit code.
 *
 * Started by the launcher's PLAY, the launcher is still there, waiting for
 * the game: it shows itself on OT_EXIT_DISC_REFUSED, and no second one is
 * started. The one started here gets --no-skip, so "Skip launcher" cannot
 * send it straight back to a game that refuses the disc image.
 */
#if defined(_WIN32)
static int open_launcher_instead(const char *why)
{
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 16];
    if (getenv("OPENTRICKY_LAUNCHER")) {
        printf("No good disc image (%s): back to the launcher\n", why);
        return OT_EXIT_DISC_REFUSED;
    }
    if (launcher_install_root_w(exe, MAX_PATH) && wcslen(exe) + 16 < MAX_PATH) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        wcscat(exe, L"\\OpenTricky.exe");
        _snwprintf(cmd, MAX_PATH + 16, L"\"%ls\" --no-skip", exe);
        cmd[MAX_PATH + 15] = 0;
        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES &&
            CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            printf("No good disc image (%s): OpenTricky.exe started\n", why);
            return 0;
        }
    }
    {
        char msg[768];
        snprintf(msg, sizeof msg, "%s\n\nChoose your SSX Tricky (USA) Xbox disc image in OpenTricky.exe, "
                 "the game's launcher.", why);
        MessageBoxA(NULL, msg, "SSX Tricky", MB_ICONWARNING);
    }
    return 0;
}
#else
static int open_launcher_instead(const char *why)
{
    fprintf(stderr, "No good disc image (%s).\nChoose your SSX Tricky (USA) Xbox disc image in settings.ini "
                    "(DiscImage=), or run with --direct <image.iso>.\n", why);
    return 0;
}
#endif /* _WIN32 */

/*
 * TRUE when standard output goes somewhere other than a person: a pipe, a
 * file or NUL. Every test harness launches the game that way, and none of
 * them can click through a launcher. A double-clicked GUI executable has no
 * standard output at all, and a real console answers GetConsoleMode, so both
 * of those still use settings.ini.
 */
#if defined(_WIN32)
static BOOL output_is_redirected(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    if (GetConsoleMode(h, &mode)) return FALSE;
    return GetFileType(h) != FILE_TYPE_UNKNOWN;
}
#else
static BOOL output_is_redirected(void) { return !isatty(1); }
#endif /* _WIN32 */

/*
 * Read a whole file out of the mounted disc image into a freshly allocated
 * buffer, matching load_xbe's contract.
 */
static BOOL load_xbe_from_iso(const char *rel_path, void **out_data, size_t *out_size)
{
    uint32_t sector = 0, size = 0, got;
    uint8_t  attrs = 0;
    void    *data;

    if (!xdvdfs_find(rel_path, &sector, &size, &attrs) ||
        (attrs & XDVDFS_ATTR_DIRECTORY) || size == 0)
        return FALSE;

    data = malloc(size);
    if (!data) return FALSE;

    got = xdvdfs_read(sector, size, 0, data, size);
    if (got != size) { free(data); return FALSE; }

    *out_data = data;
    *out_size = size;
    return TRUE;
}

/*
 * Resolve a path relative to the executable's own directory, normalised to
 * an absolute path (so an embedded "..\" is collapsed rather than passed
 * through to every later file operation). Falls back to the input string
 * unchanged if the module path can't be determined.
 */
/* Like resolve_from_exe, but leaves an already-absolute path (C:\..., or a
 * UNC share) untouched -- used for a path the user typed. */
static void resolve_from_exe(const char *rel, char *out, size_t out_sz);

static void resolve_from_exe_or_absolute(const char *p, char *out, size_t out_sz)
{
#if defined(_WIN32)
    BOOL absolute = (p[0] && p[1] == ':') || (p[0] == '\\' && p[1] == '\\');
#else
    BOOL absolute = p[0] == '/';
#endif
    if (absolute) {
        strncpy(out, p, out_sz - 1);
        out[out_sz - 1] = '\0';
    } else {
        resolve_from_exe(p, out, out_sz);
    }
}

static void resolve_from_exe(const char *rel, char *out, size_t out_sz)
{
    char exe[MAX_PATH];
    char joined[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    char *slash;

    if (n == 0 || n >= sizeof(exe)) {
        strncpy(out, rel, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }

#if defined(_WIN32)
    slash = strrchr(exe, '\\');
#else
    slash = strrchr(exe, '/');
#endif
    if (slash) *slash = '\0'; else exe[0] = '\0';

#if defined(_WIN32)
    snprintf(joined, sizeof(joined), "%s\\%s", exe, rel);
#else
    snprintf(joined, sizeof(joined), "%s/%s", exe, rel);
#endif

    if (GetFullPathNameA(joined, (DWORD)out_sz, out, NULL) == 0) {
        strncpy(out, joined, out_sz - 1);
        out[out_sz - 1] = '\0';
    }
}

/* resolve_from_exe for xbox_path_init, which takes UTF-8 and opens files
 * through the wide calls: built wide, so a game folder with accents (or any
 * name outside the code page) reaches the runtime intact. */
#if defined(_WIN32)
static void resolve_from_exe_u8(const char *rel, char *out, size_t out_sz)
{
    WCHAR exe[MAX_PATH], wrel[MAX_PATH], joined[MAX_PATH * 2], full[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR *slash;

    if (!MultiByteToWideChar(CP_UTF8, 0, rel, -1, wrel, MAX_PATH)) wrel[0] = L'\0';
    if (n == 0 || n >= MAX_PATH) {
        snprintf(out, out_sz, "%s", rel);
        return;
    }
    slash = wcsrchr(exe, L'\\');
    if (slash) *slash = L'\0'; else exe[0] = L'\0';
    _snwprintf(joined, MAX_PATH * 2, L"%ls\\%ls", exe, wrel);
    joined[MAX_PATH * 2 - 1] = 0;
    if (!GetFullPathNameW(joined, MAX_PATH, full, NULL)) {
        wcsncpy(full, joined, MAX_PATH - 1);
        full[MAX_PATH - 1] = 0;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, full, -1, out, (int)out_sz, NULL, NULL))
        snprintf(out, out_sz, "%s", rel);
}
#else
static void resolve_from_exe_u8(const char *rel, char *out, size_t out_sz)
{   /* paths are UTF-8 throughout */
    resolve_from_exe(rel, out, out_sz);
}
#endif /* _WIN32 */

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);
/* Defined in the generated dispatch table; sorts it so
 * recomp_lookup's binary search is valid. */
extern size_t recomp_dispatch_init(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
#if defined(_WIN32)
/* End of the stack scan below: 8 KB above the faulting rsp, but never past
 * the top of the thread's stack. A fault near the top of a shallow stack (a
 * thread that just started) otherwise faulted again inside this handler,
 * and that second fault is the one that got reported. */
static uintptr_t veh_stack_end(PEXCEPTION_POINTERS ep)
{
    uintptr_t end = (uintptr_t)ep->ContextRecord->Rsp + 0x2000;
    uintptr_t top = (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
    return (top && top < end) ? top : end;
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    /* A stack overflow leaves too little stack for the lines below and for
     * the unhandled-exception filter: the process used to die of a second
     * fault with nothing reported. Hand it to the crash report first (it ends
     * the process; it returns only when reports are off). */
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
        crashreport_fatal_overflow(ep);

    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        /*
         * Added to diagnose the PsTerminateSystemThread fall-through crash:
         * the bridge stub returns instead of terminating the thread (see
         * kernel_bridge.c's bridge_PsTerminateSystemThread comment), so
         * execution falls into whatever x86 bytes follow the call in the
         * original binary -- bytes never meant to be reached as code. RIP
         * itself IS the faulting Xbox address here (no ExceptionInformation
         * fault-address the way access violations have).
         */
        uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
        uintptr_t mod_base = (uintptr_t)GetModuleHandle(NULL);
        fprintf(stderr, "[CRASH] Illegal instruction at RIP=0x%llX\n", (unsigned long long)rip);
        fprintf(stderr, "  Module base: 0x%llX, link addr (for nm lookup): 0x%llX\n",
            (unsigned long long)mod_base,
            (unsigned long long)(0x140000000ULL + (rip - mod_base)));
        fprintf(stderr, "  Xbox VA of fault (only meaningful if RIP fell into raw Xbox memory): 0x%08X\n",
            (uint32_t)(rip - (uintptr_t)g_xbox_mem_offset));
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            uintptr_t mod_lo = mod_base;
            uintptr_t mod_hi = mod_base + 0x08000000ULL; /* generous module-size bound */
            fprintf(stderr, "  Native stack (first 256 slots, filtered to code range, shown as link addr for nm):\n");
            for (int i = 0; i < 256 && (uintptr_t)(sp + i) < veh_stack_end(ep); i++) {
                if (sp[i] >= mod_lo && sp[i] < mod_hi) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i,
                        (unsigned long long)(0x140000000ULL + (sp[i] - mod_base)));
                }
            }
        }
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* Live write-watches (xbox_diag) get first refusal on faults: a guarded
     * page raises an access violation that is ours, not the title's, and the
     * matching single-step is how the guard is re-armed. */
    if (xbox_diag_handle_fault(ep))
        return EXCEPTION_CONTINUE_EXECUTION;

    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        {
            uint32_t fault_va = (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);
            if (fault_va >= XBOX_ACI_MMIO_BASE &&
                fault_va <  XBOX_ACI_MMIO_BASE + XBOX_ACI_MMIO_SIZE) {
                int is_write = (int)ep->ExceptionRecord->ExceptionInformation[0];
                if (aci_hook_handle_mmio(ep->ContextRecord, fault_va, is_write))
                    return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (fault_va >= 0xFE800000u && fault_va < 0xFE880000u) {
                int is_write = (int)ep->ExceptionRecord->ExceptionInformation[0];
                if (apu_hook_handle_mmio(ep->ContextRecord, fault_addr, fault_va, is_write))
                    return EXCEPTION_CONTINUE_EXECUTION;
            }
            /* The NV2A DAC palette page (trapped unless
             * XBOX_FIX_GAMMA=0, or with XBOX_DAC_LOG=1). */
            if (fault_va >= 0xFD681000u && fault_va < 0xFD682000u) {
                int is_write = (int)ep->ExceptionRecord->ExceptionInformation[0];
                if (nv2a_dac_handle_mmio(ep->ContextRecord, fault_va, is_write))
                    return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        {
            uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
            uintptr_t mod_base = (uintptr_t)GetModuleHandle(NULL);
            fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
                (unsigned long long)rip,
                (unsigned long long)fault_addr,
                ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
            fprintf(stderr, "  Module base: 0x%llX, link addr (for nm lookup): 0x%llX\n",
                (unsigned long long)mod_base,
                (unsigned long long)(0x140000000ULL + (rip - mod_base)));
        }
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            uintptr_t mod_base = (uintptr_t)GetModuleHandle(NULL);
            uintptr_t mod_lo = mod_base;
            uintptr_t mod_hi = mod_base + 0x08000000ULL;
            fprintf(stderr, "  Native stack (first 256 slots, filtered to code range, shown as link addr for nm):\n");
            for (int i = 0; i < 256 && (uintptr_t)(sp + i) < veh_stack_end(ep); i++) {
                if (sp[i] >= mod_lo && sp[i] < mod_hi) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i,
                        (unsigned long long)(0x140000000ULL + (sp[i] - mod_base)));
                }
            }
        }
        fflush(stderr);
    }

    /*
     * Every other fatal exception.
     *
     * This handler previously reported only illegal instructions and access
     * violations. Any other fatal exception unwound in complete silence and
     * the process died with no diagnostic, which made a whole class of crash
     * look like a clean run: bash's `timeout` renders an integer divide by
     * zero as exit 127, which is easy to mistake for an ordinary early exit.
     * Five consecutive "clean" 45-second runs turned out to be dying at about
     * one second on STATUS_INTEGER_DIVIDE_BY_ZERO (0xC0000094), with the true
     * code only visible through the Win32 process exit code.
     *
     * Only genuinely fatal hardware exceptions are listed. VEH sees
     * first-chance exceptions, so breakpoints and the C++ EH exception code
     * (0xE06D7363) are deliberately excluded -- those are normal traffic and
     * reporting them would bury the real faults.
     */
    {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        const char *what = NULL;

        switch (code) {
        case EXCEPTION_INT_DIVIDE_BY_ZERO:  what = "integer divide by zero";     break;
        case EXCEPTION_INT_OVERFLOW:        what = "integer overflow";           break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:  what = "float divide by zero";       break;
        case EXCEPTION_FLT_INVALID_OPERATION: what = "float invalid operation";  break;
        case EXCEPTION_FLT_STACK_CHECK:     what = "float stack check";          break;
        case EXCEPTION_FLT_OVERFLOW:        what = "float overflow";             break;
        case EXCEPTION_STACK_OVERFLOW:      what = "stack overflow";             break;
        case EXCEPTION_PRIV_INSTRUCTION:    what = "privileged instruction";     break;
        case EXCEPTION_DATATYPE_MISALIGNMENT: what = "datatype misalignment";    break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: what = "array bounds exceeded";    break;
        case EXCEPTION_IN_PAGE_ERROR:       what = "in-page error";              break;
        default: break;
        }

        if (what) {
            uintptr_t rip      = (uintptr_t)ep->ContextRecord->Rip;
            uintptr_t mod_base = (uintptr_t)GetModuleHandle(NULL);
            uintptr_t mod_hi   = mod_base + 0x08000000ULL;

            /*
             * Dump the indirect-call ring buffer.
             *
             * For a stack overflow the native backtrace is useless -- the stack
             * is already blown, so only a frame or two survive in module range.
             * The last 16 ICALL targets are globals, cost nothing to print, and
             * show the repeating cycle directly, which is the one thing needed
             * to identify runaway recursion.
             */
            {
                extern volatile uint32_t g_icall_trace[16];
                extern volatile uint32_t g_icall_trace_idx;
                int k;
                fprintf(stderr, "  Last 16 indirect-call targets (oldest first):\n    ");
                for (k = 0; k < 16; k++) {
                    int idx = (int)((g_icall_trace_idx - 16 + k) & 15);
                    fprintf(stderr, "0x%08X ", g_icall_trace[idx]);
                }
                fprintf(stderr, "\n");
            }

            fprintf(stderr, "[CRASH] %s (0x%08lX) at RIP=0x%llX\n",
                what, (unsigned long)code, (unsigned long long)rip);
            fprintf(stderr, "  Module base: 0x%llX, link addr (for nm/addr2line): 0x%llX\n",
                (unsigned long long)mod_base,
                (unsigned long long)(0x140000000ULL + (rip - mod_base)));
            fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
                g_eax, g_ecx, g_edx, g_esp);
            fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
                g_ebx, g_esi, g_edi);
            {
                uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
                fprintf(stderr, "  Native stack (first 256 slots, filtered to code range, shown as link addr):\n");
                for (int i = 0; i < 256 && (uintptr_t)(sp + i) < veh_stack_end(ep); i++) {
                    if (sp[i] >= mod_base && sp[i] < mod_hi) {
                        fprintf(stderr, "    [%d] 0x%llX\n", i,
                            (unsigned long long)(0x140000000ULL + (sp[i] - mod_base)));
                    }
                }
            }
            fflush(stderr);
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}
#endif /* _WIN32: the vectored exception handler; macOS traps device pages in platform/mmio_trap.c */

/*
 * Who reads guest VA 8 / 0xC?
 *
 * The low-address TIB redirect cannot be narrowed to exclude those two offsets
 * without collapsing the title, even now that every real fs: access is lifted
 * to the FS* macros. So some reader gets there by a route that is not an fs:
 * prefix -- most likely a legitimately-NULL pointer plus a small field offset.
 * This records the distinct return addresses so they can be resolved with
 * addr2line, instead of guessing. Gated on XBOX_LOWACC_LOG.
 */
int g_lowacc_enabled = 0;
#define LOWACC_MAX 64
static void  *g_lowacc[LOWACC_MAX];
static unsigned g_lowacc_va[LOWACC_MAX];
static unsigned g_lowacc_hits[LOWACC_MAX];
static int      g_lowacc_n = 0;

void xbox_lowaccess_log(unsigned va, void *ra)
{
    int i;
    for (i = 0; i < g_lowacc_n; i++)
        if (g_lowacc[i] == ra && g_lowacc_va[i] == va) { g_lowacc_hits[i]++; return; }
    if (g_lowacc_n < LOWACC_MAX) {
        IMAGE_BASE_DECL
        g_lowacc[g_lowacc_n] = ra;
        g_lowacc_va[g_lowacc_n] = va;
        g_lowacc_hits[g_lowacc_n] = 1;
        g_lowacc_n++;
        /* Printed on first sight: the process is normally killed by a timeout,
         * so an atexit report never runs. */
        fprintf(stderr, "[LOWACC] new reader #%d va=0x%02X rva=0x%08llX\n",
                g_lowacc_n, va,
                (unsigned long long)((char *)ra - IMAGE_BASE_PTR));
        fflush(stderr);
    }
}

static void xbox_lowaccess_report(void)
{
    int i;
    IMAGE_BASE_DECL
    if (!g_lowacc_n) { fprintf(stderr, "[LOWACC] no reads of VA 8/0xC\n"); return; }
    fprintf(stderr, "[LOWACC] %d distinct reader(s) of VA 8/0xC:\n", g_lowacc_n);
    for (i = 0; i < g_lowacc_n; i++)
        fprintf(stderr, "[LOWACC]   va=0x%02X rva=0x%08llX  x%u\n", g_lowacc_va[i],
                (unsigned long long)((char *)g_lowacc[i] - IMAGE_BASE_PTR),
                g_lowacc_hits[i]);
    fflush(stderr);
}

/* Paired with the timeBeginPeriod(1) in WinMain; see the note there. */
static void recomp_release_timer_period(void)
{
    timeEndPeriod(1);
}

/* XBOX_FIX_BOOTHANG (default 1): the ~1-in-15 boot that freezes
 * on the EA logo. The title's DirectSound asks the APU front end to trap on
 * SE2FE_IDLE_VOICE (FETFORCE1) and relies on its interrupt routine (bus IRQ 5,
 * 0x17A651 -> 0x17A608 -> 0x17A5C1) to handle the trapped method and set the
 * front end free-running again. This runtime has no hardware interrupts: when
 * a voice went idle inside that window the front end stayed TRAPPED for good,
 * the APU ran one frame and no sound ever started, and the logo video waits
 * for its sound. Here the interrupt is delivered the way a CPU takes one --
 * between two instructions of whatever guest thread runs -- at the end of the
 * next kernel call of any Xbox thread, through the routine the title
 * registered. One thread at a time; the routine's own kernel calls do not
 * re-enter (xbox_kernel_call_isr). */
extern volatile long g_apu_fe_trap_pending;
uint32_t mcpx_apu_irq_raise(void);
uint32_t mcpx_apu_reg(uint32_t addr);
int xbox_kernel_call_isr(int level);
extern void (*xbox_kernel_post_call_hook)(void);

static void boothang_hook(void)
{
    static volatile LONG busy;
    if (!g_apu_fe_trap_pending || InterlockedExchange(&busy, 1)) return;
    if (InterlockedExchange(&g_apu_fe_trap_pending, 0)) {
        uint32_t ists = mcpx_apu_irq_raise();
        int ran = xbox_kernel_call_isr(5);
        static int shown;
        if (shown++ < 20)
            fprintf(stderr, "[BOOTHANG] APU front-end trap -> interrupt routine %s "
                    "(ISTS %08X, IEN %08X); FECTL now %08X\n", ran ? "run" : "NOT run",
                    ists, mcpx_apu_reg(0x1004), mcpx_apu_reg(0x1100));
        if (!ran) g_apu_fe_trap_pending = 1;   /* routine not registered yet: retry */
    }
    busy = 0;
}

/* ── WinMain ───────────────────────────────────────────────── */

#if defined(_WIN32)
#define OT_WINMAIN WinMain
#else
#define OT_WINMAIN ot_winmain
#endif
int WINAPI OT_WINMAIN(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                      LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== YOUR_GAME_NAME - Static Recompilation ===\n");
    printf("OpenTricky %s\n", OT_VERSION);
    printf("Loading XBE...\n");

    /* Windows in real pixels: a 1280x960 setting is a 1280x960 window,
     * not one the desktop scales up and blurs. */
    launcher_enable_dpi_awareness();

    /* Install VEH handler (first handler in chain) */
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, veh_handler);
#endif

    /* Rider command log (XBOX_NETLOG=1). Off by default, and then
     * recomp_lookup_manual() hands out none of its hooks. */
    np_cmdlog_init();

    /* Local ghost (XBOX_GHOST=<file.npcl>). Off by default. */
    np_ghost_init();

    /* Two instances over UDP (XBOX_NET=host | join:IP:port). Off
     * by default; exclusive with the ghost (which is switched off). */
    np_net_init();
    if (g_np_net_on && g_np_ghost_on) {
        fprintf(stderr, "[NET] XBOX_GHOST ignored: exclusive with XBOX_NET\n");
        g_np_ghost_on = 0;
    }

    /* Replayed race for benchmarks (XBOX_RACEBENCH=<file.npcl>). Off by
     * default; exclusive with the ghost and the network (both switched off). */
    np_rb_init();
    np_stuck_init();
    if (g_np_rb_on && (g_np_ghost_on || g_np_net_on)) {
        fprintf(stderr, "[RACEBENCH] XBOX_GHOST / XBOX_NET ignored: exclusive with XBOX_RACEBENCH\n");
        g_np_ghost_on = 0;
        g_np_net_on = 0;
    }

    /* Ask for a 1 ms scheduler tick.
     *
     * The Xbox kernel ran timers and thread delays at millisecond
     * granularity, and the bridge is written for that: KeDelayExecutionThread
     * clamps sub-millisecond intervals up to Sleep(1), and the KeTickCount
     * thread refreshes on a "1 ms cadence".  On Windows, though, the default
     * scheduler tick is ~15.6 ms, so every one of those waits really slept
     * fifteen times as long as intended.
     *
     * The cost was not a dropped frame here and there -- it was throughput.
     * The intro's twelve asset items are each gated on async reads that the
     * loader polls through timer waits, so the whole boot sequence ran at
     * roughly a fifteenth speed and looked, on a short sample, like a hang.
     *
     * timeBeginPeriod is process-wide and must be released; the matching
     * timeEndPeriod is registered with atexit so it also runs on the abnormal
     * exits this title still takes. */
    if (timeBeginPeriod(1) == TIMERR_NOERROR)
        atexit(recomp_release_timer_period);
    else
        fprintf(stderr, "warning: could not raise the timer resolution to 1 ms; "
                        "timer-driven waits will run at the ~15.6 ms default\n");
    if (getenv("XBOX_LOWACC_LOG")) { g_lowacc_enabled = 1; atexit(xbox_lowaccess_report); }

    /* Step 0: choose the disc and the display.
     *
     * A normal start (a double-click, Steam) plays straight away with the
     * settings of settings.ini (settings.h: beside the executables, or in
     * Documents\My Games\SSX Tricky), the file the separate launcher
     * (OpenTricky.exe) writes; --play (the launcher's PLAY) does the same.
     * The launcher window that was built into this executable is
     * gone: without a good disc image in settings.ini,
     * OpenTricky.exe beside the game is started to choose one, or a message
     * says what is missing. --direct, a disc image named on
     * the command line, or redirected output (every automated run) keep the
     * pre-launcher behaviour exactly: the ISO or extracted files beside the
     * executable, the title's own 640x480 at 4:3, and "hdd" beside the
     * executable -- so test runs and their captures never depend on what the
     * player configured. In that mode XBOX_RENDER=WxH, XBOX_WIDESCREEN=1 and
     * XBOX_FULLSCREEN=1 exercise the display options. */
    static LauncherConfig launch_cfg;   /* the menu keeps using it */
    BOOL use_config;

    launcher_init(YOUR_GAME_ENTRY_POINT);
    launcher_config_load(&launch_cfg);
    {
        const char *cli = find_iso_argument();
        BOOL direct = has_flag(L"--direct") || cli != NULL ||
                      (output_is_redirected() && !has_flag(L"--play"));
        use_config = !direct;

        if (use_config) {
            /* A relative DiscImage is relative to the install root, as in the
             * launcher: not to the working folder (bin\ on a double-click). */
            char why[512], iso[MAX_PATH];
            launcher_resolve_path(launch_cfg.iso, iso, sizeof iso);
            /* Before the check: its folder is blanked in crash dumps, its
             * name hidden in the texts (the check's message too). */
            crashreport_private_file_acp(iso);
            if (!launcher_check_iso(iso, why, sizeof why))
                return open_launcher_instead(why);
            if (!xdvdfs_mount(iso))
                return open_launcher_instead("The disc image could not be opened.");
            printf("Game disc: %s (ISO)\n", iso);
        } else {
            char iso_path[MAX_PATH];
            BOOL mounted = FALSE;

            if (cli) {
                resolve_from_exe_or_absolute(cli, iso_path, sizeof(iso_path));
                mounted = xdvdfs_mount(iso_path);
            } else {
                /* Look next to the executable first, then one directory up --
                 * the build output lives in a subdirectory, so keeping the
                 * image beside the project rather than inside build\ is both
                 * tidier and survives a clean rebuild. */
                static const char *const candidates[] = {
                    YOUR_GAME_ISO, "..\\" YOUR_GAME_ISO
                };
                size_t i;
                for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
                    resolve_from_exe(candidates[i], iso_path, sizeof(iso_path));
                    if (xdvdfs_mount(iso_path)) { mounted = TRUE; break; }
                }
            }

            if (mounted) {
                printf("Game disc: %s (ISO)\n", iso_path);
                crashreport_private_file_acp(iso_path);     /* its folder: blanked in crash dumps */
            } else if (cli) {
                /* An explicitly requested image that will not mount is an error
                 * worth reporting rather than silently ignoring. */
                char msg[MAX_PATH + 128];
                snprintf(msg, sizeof(msg),
                         "Could not mount disc image:\n%s\n\n"
                         "It is missing, unreadable, or not an Xbox (XDVDFS) image.",
                         iso_path);
                MessageBoxA(NULL, msg, "Recomp", MB_ICONERROR);
                return 1;
            }
        }
    }

    /* The log file, if the player asked for one: everything the game reports
     * goes to "<exe name>.log" beside settings.ini, replacing the previous run's. The
     * executable has no console, so without this a crash leaves nothing. */
    if (use_config && launch_cfg.log_file) {
        FILE *t;
        launcher_log_path(s_log_path, sizeof s_log_path);
        t = fopen(s_log_path, "w");
        if (t) fclose(t);
        if (freopen(s_log_path, "a", stdout) && freopen(s_log_path, "a", stderr)) {
            SYSTEMTIME st;
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
            GetLocalTime(&st);
            printf("SSX Tricky log, %04u-%02u-%02u %02u:%02u:%02u, OpenTricky %s\n",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, OT_VERSION);
            launcher_config_log();      /* the settings lines printed before the log opened, again */
        } else {
            s_log_path[0] = '\0';
        }
    }
    /* Started by OpenTricky.exe's PLAY: the launcher shows the crash screen
     * when the game ends (it finds the report folder), so no message here. */
    s_crash_dialog = use_config && !getenv("OPENTRICKY_LAUNCHER");
    /* Crash / freeze reports (crashreport.h): with no log file, a player's
     * output is kept in memory for the report instead of going nowhere. */
    crashreport_set_output(use_config, s_log_path, output_is_redirected());

    /* The display the launcher chose (or the test overrides). 16:9 also tells
     * the title its display is widescreen -- the dashboard setting on a real
     * console -- so it renders its own wider, anamorphic view; the host then
     * shows the frame at 16:9. Textures, anti-aliasing, the frame rate and
     * the bindings are set in the launcher's Settings.
     * 21:9, 32:9 or the monitor's own shape use the same mode with a wider
     * view (aspect.h); 4:3 and 16:9 go exactly as before. */
    if (use_config) {
        double shape = launcher_display_aspect(&launch_cfg);
        int wide;
        aspect_set_fov(launch_cfg.wide_fov);    /* XBOX_WIDE_FOV wins */
        aspect_set_menus(launch_cfg.menus);     /* XBOX_WIDE_MENUS wins */
        wide = aspect_init(shape);
        d3d8_SetHostDisplay((unsigned)launch_cfg.width, (unsigned)launch_cfg.height,
                            wide, launch_cfg.fullscreen);
        if (g_aspect_hook_on) d3d8_SetHostAspect(shape);
        hud_anchor_set(shape, launch_cfg.hud_shape, launch_cfg.hud_size);  /* XBOX_HUD_* win */
        xbox_SetVideoFlags(wide ? 0x00010000u : 0u);
        d3d8_SetMsaa(launch_cfg.msaa);
        d3d8_SetAnisotropy(launch_cfg.aniso);
        d3d8_SetShowFps(launch_cfg.show_fps);
        controls_keep_essential(&launch_cfg.controls);   /* after the log opened: its line is kept */
        controls_set_current(&launch_cfg.controls);
        hostui_install(&launch_cfg, TRUE);
        printf("Display:    %dx%d, %s%s, textures %s, %dx AA\n",
               launch_cfg.width, launch_cfg.height,
               launcher_aspect_name(launch_cfg.aspect),
               launch_cfg.fullscreen ? ", fullscreen" : "",
               launch_cfg.aniso ? "anisotropic" : "as on Xbox", launch_cfg.msaa);
    } else {
        const char *r  = getenv("XBOX_RENDER");
        const char *ws = getenv("XBOX_WIDESCREEN");
        const char *fs = getenv("XBOX_FULLSCREEN");
        const char *aa = getenv("XBOX_MSAA");
        const char *an = getenv("XBOX_ANISO");
        unsigned rw = 0, rh = 0;
        int wide = ws && ws[0] == '1';
        int full = fs && fs[0] == '1';
        double shape;
        if (r && sscanf(r, "%ux%u", &rw, &rh) != 2) rw = rh = 0;
        shape = aspect_from_env(rw, rh);        /* XBOX_ASPECT */
        aspect_set_fov(ASPECT_FOV_NOSTRETCH);   /* XBOX_WIDE_FOV */
        aspect_set_menus(ASPECT_MENUS_DEFAULT); /* XBOX_WIDE_MENUS */
        if (shape > 0.0) wide = aspect_init(shape);
        else if (wide && g_aspect_menus43)      /* XBOX_WIDESCREEN=1 alone, menus at 4:3 */
            aspect_init(shape = 16.0 / 9.0);    /* need the x-scale hook, as with XBOX_ASPECT=16:9 */
        if (rw || wide || full)
            d3d8_SetHostDisplay(rw, rh, wide, full);
        if (g_aspect_hook_on) d3d8_SetHostAspect(shape);
        hud_anchor_set(shape > 0.0 ? shape : wide ? 16.0 / 9.0 : 4.0 / 3.0,
                       HUD_SHAPE_PROPORTIONAL, HUD_SIZE_DEFAULT);   /* XBOX_HUD_* */
        if (wide)
            xbox_SetVideoFlags(0x00010000u);
        if (aa) d3d8_SetMsaa(atoi(aa));
        if (an) d3d8_SetAnisotropy(atoi(an));
        if (getenv("OT_HOME_DIR")) {   /* the macOS app starts the game with --direct but keeps the player's key and button bindings (settings.ini) */
            controls_keep_essential(&launch_cfg.controls);
            controls_set_current(&launch_cfg.controls);
        }
        /* The menu is there in every mode; only launcher starts save to the
         * .ini, so a test run never rewrites the player's settings. */
        hostui_install(NULL, FALSE);
    }

    /* The fork options of the launcher ([Fork] in the .ini) become
     * the XBOX_* variables the code reads; one already set in the environment
     * keeps priority. Test runs (--direct, redirected output) ignore the
     * player's .ini as above, unless XBOX_FORK_INI names an .ini to take the
     * [Fork] section from ("1" = the one beside the executable) -- that is how
     * the bench checks the launcher's settings against the variables. */
    if (use_config) {
        launcher_fork_apply(&launch_cfg, "ini");
    } else {
        const char *fi = getenv("XBOX_FORK_INI");
        if (fi && fi[0] && strcmp(fi, "0")) {
            LauncherConfig fc;
            char ini[MAX_PATH];
            if (!strcmp(fi, "1")) launcher_config_path(ini, sizeof ini);
            else resolve_from_exe_or_absolute(fi, ini, sizeof ini);
            if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES)
                printf("XBOX_FORK_INI: %s not found, fork defaults\n", ini);
            launcher_fork_load(&fc, ini);
            launcher_fork_apply(&fc, "XBOX_FORK_INI");
        } else {
            launcher_fork_apply(&launch_cfg, NULL);     /* report only */
        }
    }

    /* Pass tags (XBOX_PASS_TAGS). Off by default, and then
     * recomp_lookup_manual() hands out none of its hooks. */
    pass_tags_init();
    /* Host frame cap (XBOX_FPS_CAP, or FrameRateCap in the .ini
     * through the launcher just above). 60 by default, and then
     * recomp_lookup_manual() hands out none of its hooks. */
    fps_cap_init();
    /* Draw distance (XBOX_DRAW_DISTANCE, or DrawDistance in the
     * .ini through the launcher above). Original by default: no hook. */
    drawdist_init();
    /* Texture seams between terrain patches (terrain_seams.h): PC options,
     * off by default, and then nothing is hooked. */
    terrain_seams_init();
    /* Control scheme: the button maps are served in the PS2 layout, lessons
     * included (ctlscheme.h); XBOX_CONTROL_SCHEME=xbox (debug only, never set
     * by the launcher) keeps the original files, nothing hooked. */
    ctlscheme_init();
    /* and the loading screens' pad legend in the same layout (ps2legend.h);
     * the Basic Controls splash is always skipped. */
    ps2legend_init();
    /* Full-screen fades (the respawn flash) reach the screen edges instead of
     * the TV safe area (XBOX_FULLSCREEN_FADES, on by default; see fullfade.h). */
    fullfade_init();
    /* Mesh parts drawn after their constant block was rewritten
     * (XBOX_PUMPLAG, diagnostic, off by default; see pumplag.h). */
    pumplag_init();
    /* EA stream reader: block search bounded to the ring (XBOX_FIX_STRMWALK,
     * on by default; see strmwalk.h). */
    strmwalk_init();
    /* Crowd chant names: no pick past the end of a list (XBOX_FIX_CHANT,
     * on by default; see chantfix.h). */
    chantfix_init();
    /* HD textures (XBOX_HD_TEXTURES, or HdTextures + HdTexturesPath
     * in the .ini through the launcher above). Off by default: the translator
     * never looks at the index. The index the game carries is a resource. */
    {
#if defined(_WIN32)
        HRSRC r = FindResourceA(NULL, "HDTEX_INDEX", MAKEINTRESOURCEA(10) /* RT_RCDATA */);
        HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
        const char *text = g ? (const char *)LockResource(g) : NULL;
        if (text) hdtex_set_builtin_index(text, SizeofResource(NULL, r));
#else
        {   /* not Windows: the index is a file of the assets folder (platform/ot_assets.c) */
            extern const void *ot_asset(const char *name, size_t *size);
            size_t n = 0; const char *text = (const char *)ot_asset("HDTEX_INDEX", &n);
            if (text) hdtex_set_builtin_index(text, n);
        }
#endif
    }
    /* Fork: three branches the lifter cut from their flags, never taken until the
     * generated code got the condition back (fork pass fix_deadflags.py).
     * XBOX_FIX_DEADFLAGS = bit mask (default 7; 0 = all three never taken, as
     * before); XBOX_FIX_DEADFLAGS_A1 / _A2 / _A3 = 0 or 1 set one bit:
     *   A1 0x1  sub_00019468: a negative length no longer runs `rep movsd` on ~2^32
     *   A2 0x2  sub_000BE680: the respawn point is checked against every track zone,
     *           not only the first
     *   A3 0x4  sub_001281DB: the collision-cache RLE decoder stops at the end mark
     *           instead of writing one byte past the block */
    {
        static const char *const names[3] = { "XBOX_FIX_DEADFLAGS_A1",
                                              "XBOX_FIX_DEADFLAGS_A2",
                                              "XBOX_FIX_DEADFLAGS_A3" };
        const char *e = getenv("XBOX_FIX_DEADFLAGS");
        if (e && e[0]) g_fix_deadflags = (int)(strtol(e, NULL, 0) & 0x7);
        for (int k = 0; k < 3; k++) {
            e = getenv(names[k]);
            if (e && e[0] == '0') g_fix_deadflags &= ~(1 << k);
            else if (e && e[0] == '1') g_fix_deadflags |= 1 << k;
        }
        if (g_fix_deadflags != 0x7)
            fprintf(stderr, "[DEADFLAGS] XBOX_FIX_DEADFLAGS=0x%X (A1 %d, A2 %d, A3 %d)\n",
                    g_fix_deadflags, g_fix_deadflags & 1, (g_fix_deadflags >> 1) & 1,
                    (g_fix_deadflags >> 2) & 1);
    }
    /* No busy wait on the NV2A write-combine flush (XBOX_FIX_KICKWAIT,
     * default 1). The guard is in the generated code (fork pass fix_kickwait.py). */
    {
        const char *e = getenv("XBOX_FIX_KICKWAIT");
        g_fix_kickwait = !(e && e[0] == '0');
    }
    /* Bounded walk of the AI and Player lists in sub_0002F800
     * (XBOX_FIX_RIDERLIST, default 1). The guards are in the generated code (fork pass
     * fix_riderlist.py); when one acts, fork_riderlist_note writes a [RIDERLIST] line. */
    {
        const char *e = getenv("XBOX_FIX_RIDERLIST");
        g_fix_riderlist = !(e && e[0] == '0');
        if (!g_fix_riderlist) fprintf(stderr, "[RIDERLIST] XBOX_FIX_RIDERLIST=0 (original unchecked walk)\n");
    }
    /* Fork: the jge of sub_0015A79C that the lifter cut off from its test
     * (XBOX_FIX_SPEECHHOLD, default 1). With it, a cutscene line is held for its
     * animation cue (the rider's close-up) instead of playing at once. The condition
     * is in the generated code (fork pass fix_speechhold.py); 0 = the branch is never
     * taken, as before. */
    {
        const char *e = getenv("XBOX_FIX_SPEECHHOLD");
        g_fix_speechhold = !(e && e[0] == '0');
        if (!g_fix_speechhold) fprintf(stderr, "[SPEECHHOLD] XBOX_FIX_SPEECHHOLD=0 (dead jge kept)\n");
    }
    /* Fork: the ambient sound emitters (crowd, cheers, track ambience) are kept alive
     * by the 60 Hz tick and dropped by their update (0x120CF0) in the race render;
     * above 60 fps every extra render dropped them all (XBOX_FIX_CROWDEMIT, default 1:
     * the extra renders skip that update; 0 = as before). See fps_cap.c. */
    {
        const char *e = getenv("XBOX_FIX_CROWDEMIT");
        g_fix_crowdemit = !(e && e[0] == '0');
        if (!g_fix_crowdemit) fprintf(stderr, "[CROWDEMIT] XBOX_FIX_CROWDEMIT=0 (emitter update in every render)\n");
    }
    {
        const char *e = getenv("XBOX_FIX_FADE");
        g_fix_fade = !(e && e[0] == '0');
        if (!g_fix_fade) fprintf(stderr, "[FADE] XBOX_FIX_FADE=0 (1 s volume fades done in one frame)\n");
    }
    /* Deliver the APU's front-end trap interrupt (XBOX_FIX_BOOTHANG,
     * default 1). See boothang_hook above. */
    {
        const char *e = getenv("XBOX_FIX_BOOTHANG");
        int on = !(e && e[0] == '0');
        if (on) xbox_kernel_post_call_hook = boothang_hook;
        fprintf(stderr, "[BOOTHANG] XBOX_FIX_BOOTHANG=%d\n", on);
    }
    /* Frame profile by zones (XBOX_PERF), off by default. */
    {
        extern void perf_init(void);
        perf_init();
    }
    /* GPU time per pass (XBOX_GPUPROF), off by default. */
    {
        extern void gpuprof_init(void);
        gpuprof_init();
    }
    /* Per-tick trace (XBOX_TICKTRACE), off by default. */
    ticktrace_init();
    /* Post on the 3D only (XBOX_POST_SPLIT / XBOX_POST_CYCLE):
     * the translator calls back at each pass phase change, FRAME_END runs
     * the post (see d3d8_post.h). Not registered otherwise. */
    if (d3d8_post_split_wanted())
        pgraph_d3d11_set_pass_phase_callback(d3d8_PassPhaseChanged);

    /* Step 1: Load the XBE -- from the mounted disc if there is one,
     * otherwise from the extracted game directory. Paths are resolved from
     * the executable, not the working directory, so launching from
     * elsewhere still finds the game files. */
    char xbe_path[MAX_PATH];
    BOOL xbe_ok;

    if (xdvdfs_is_mounted()) {
        xbe_ok = load_xbe_from_iso("default.xbe", &xbe_data, &xbe_size);
        strncpy(xbe_path, "D:\\default.xbe", sizeof(xbe_path) - 1);
        xbe_path[sizeof(xbe_path) - 1] = '\0';
        if (xbe_ok)
            printf("XBE loaded from disc image (%zu bytes)\n", xbe_size);
    } else {
        resolve_from_exe(YOUR_GAME_XBE_PATH, xbe_path, sizeof(xbe_path));
        xbe_ok = load_xbe(xbe_path, &xbe_data, &xbe_size);
    }

    if (!xbe_ok) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /*
     * Read the title's own name and ID out of the XBE certificate.
     *
     * The certificate pointer is at header+0x118 as a *virtual* address, so it
     * is rebased through the image base at header+0x104. Inside it: the title
     * ID at +0x08 and the name at +0x0C as 40 UTF-16 characters.
     *
     * The ID matters beyond cosmetics -- it names the per-title save
     * directories (\TDATA\<id>, \UDATA\<id>), which the title reaches both
     * through its Partition1 paths and through T:/U:. Taking it from the
     * certificate keeps every route pointing at the same place for whatever
     * game this build is pointed at.
     */
    if (xbe_size > 0x180) {
        const unsigned char *hdr = (const unsigned char *)xbe_data;
        unsigned int image_base, cert_va, cert_off;
        memcpy(&image_base, hdr + 0x104, 4);
        memcpy(&cert_va,    hdr + 0x118, 4);
        cert_off = cert_va - image_base;
        if (cert_off + 0x5C <= xbe_size) {
            unsigned int title_id;
            char name[41];
            int i;
            memcpy(&title_id, hdr + cert_off + 0x08, 4);
            for (i = 0; i < 40; i++) {
                unsigned short wc;
                memcpy(&wc, hdr + cert_off + 0x0C + i * 2, 2);
                name[i] = wc ? (char)(wc & 0x7F) : '\0';
                if (!wc) break;
            }
            name[i < 40 ? i : 40] = '\0';
            printf("Title:      %s (ID %08X)\n", name, title_id);
            xbox_path_set_title_id(title_id);
#if defined(_WIN32)
            SetConsoleTitleA(name[0] ? name : "Xbox recompilation");
#endif
        }
    }

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.\n\n"
                    "On Wine/Proton, try setting the environment variable "
                    "XBOX_TEST_BASE=200000000 and send the log if it still fails.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    /* Trap the AC'97 aperture so its registers get device semantics rather
     * than behaving as plain RAM -- see src/apu/aci_mmio.c for why DSOUND
     * deadlocks without this. */
    aci_mmio_install((void *)(uintptr_t)g_xbox_mem_offset);
    /* Same for the MCPX APU aperture: DSOUND programs the voice processor and
     * then waits on an APU status counter, which never advances while the
     * registers are plain RAM. */
    apu_mmio_install((uint8_t *)(uintptr_t)g_xbox_mem_offset);
    /* The DAC palette (the title's gamma ramp) -- trapped only
     * when the gamma fix is on (the default) or logged; XBOX_FIX_GAMMA=0
     * leaves the page plain RAM as before. */
    {
        const char *g = getenv("XBOX_FIX_GAMMA"), *l = getenv("XBOX_DAC_LOG");
        if (!(g && g[0] == '0') || (l && l[0] == '1'))
            nv2a_dac_trap_install((uint8_t *)(uintptr_t)g_xbox_mem_offset);
    }

    /* Live diagnostics, if XBOX_DIAG_PORT is set. Started here so the guest
     * address space and the pool table are already resolvable. */
    /*
     * Zero the guest's low page.
     *
     * Before per-thread TIBs existed, every thread shared one block at Xbox
     * VA 0, and its leftovers are still there: VA 0 = 0xDEADBEEF, VA 4 = a
     * stack pointer, VA 8 = a TIB address. Real fs: accesses now go through the
     * FS8/FS16/FS32 macros (the lifter emits them from Capstone's mem.segment),
     * so nothing reads those leftovers on purpose any more -- but a genuine
     * NULL dereference does, and on hardware it would read zeros.
     *
     * That matters for the circular-list terminator sub_000A3890, which tests
     * MEM32(ecx + 8) == ecx with ecx == 0 and needs both sides to be zero to
     * report end-of-list.
     */
    if (g_xbox_mem_offset) {
        memset((void *)(uintptr_t)g_xbox_mem_offset, 0, 0x100);
        printf("Low page (VA 0x0-0xFF) zeroed for NULL-dereference semantics\n");
        fflush(stdout);
    }

    /*
     * Process the XBE's TLS directory.
     *
     * The header's TlsAddress points at a six-dword directory whose third field,
     * AddressOfIndex, names a guest variable the loader must fill in with the
     * allocated TLS slot index -- exactly as Windows does for PE TLS. Nothing in
     * the title ever writes it: the displacement appears six times in the whole
     * image and every one is a read.
     *
     * Left unwritten it keeps the image's compiled-in 0xFFFFFFFB ("no index"),
     * and the CRT's TLS accessors then index the array with -5:
     *
     *     ecx = fs:[4]                 ; TLS array
     *     eax = ds:[AddressOfIndex]    ; -5
     *     eax = [ecx + eax*4]          ; reads before the array -> 0
     *     [eax + 4] = value            ; writes to Xbox VA 4
     *
     * That write lands on the TIB via the low-address redirect and smears the
     * KPCR, which is what corrupted fs:[4]/fs:[8] and, downstream, made the
     * circular-list terminator sub_000A3890 never report end-of-list.
     *
     * This title has a single TLS directory, so slot 0 is the right index.
     */
    {
        /* Read the header from guest memory -- it is mapped at the XBE base,
         * and the host-side copy may already have been released by now. */
        uint32_t tls_dir = 0;
        if (g_xbox_mem_offset)
            tls_dir = *(uint32_t *)(uintptr_t)(0x00010000u + 0x12C + g_xbox_mem_offset);
        if (tls_dir && g_xbox_mem_offset) {
            uint32_t *dir = (uint32_t *)(uintptr_t)(tls_dir + g_xbox_mem_offset);
            uint32_t idx_va = dir[2];   /* AddressOfIndex */
            if (idx_va) {
                *(uint32_t *)(uintptr_t)(idx_va + g_xbox_mem_offset) = 0;
                fprintf(stderr, "  [XBE] TLS index 0 written to VA 0x%08X (zero-fill %u bytes)\n",
                       idx_va, dir[4]);
                fflush(stderr);
            }
        }
    }

    g_diag_shot_hook = d3d8_RequestScreenshot;
    g_diag_press_hook = xinput_hle_press;
    g_diag_drawlog_hook = nv2a_drawlog_arm;
    g_diag_skipprog_hook = nv2a_skipprog_set;
    g_diag_ignored_hook = nv2a_ignored_dump;
    xbox_diag_start();
    {   /* XBOX_PROFILE=START,SECONDS: see xboxrecomp/src/kernel/xbox_profile.c */
        extern void xbox_profile_start_from_env(void);
        xbox_profile_start_from_env();
    }
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game and save directories for file I/O path translation.
     *
     * Both are resolved against the executable's own directory rather than
     * the process working directory. Passing the bare relative strings meant
     * every path depended on where the game happened to be launched from --
     * which is how an empty save folder ended up created next to the build
     * output instead of in the intended place. Both are UTF-8, as
     * xbox_path_init reads them (an ANSI path with an accent became a
     * folder that does not exist, and no save could be written). */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        char game_dir[MAX_PATH * 3], save_dir[MAX_PATH * 3];

        resolve_from_exe_u8(YOUR_GAME_DIR,   game_dir, sizeof(game_dir));
        if (use_config) {                      /* the launcher's save folder (automatic if unset) */
            launcher_hdd_path(&launch_cfg, save_dir, sizeof(save_dir));
            launcher_migrate_saves(save_dir);  /* saves an earlier version put in a misnamed folder */
        } else
            resolve_from_exe_u8(YOUR_GAME_SAVE_DIR, save_dir, sizeof(save_dir));
        /* Save profile Unlock all: a save of its own, below the save folder,
         * whenever the cheat is on (settings.ini or XBOX_UNLOCK_ALL) -- the
         * player's own save is never opened with everything unlocked. */
        if (launcher_unlock_all() && !launcher_unlock_all_folder(save_dir, sizeof(save_dir))) {
            /* no room for it: never the player's folder, the default one's */
            resolve_from_exe_u8(YOUR_GAME_SAVE_DIR, save_dir, sizeof(save_dir));
            if (!launcher_unlock_all_folder(save_dir, sizeof(save_dir))) {
                _putenv_s("XBOX_UNLOCK_ALL", "0");      /* no folder of its own: no cheat */
                printf("Unlock all: off (no room for its save folder)\n");
            }
        }

        printf("Game data:  %s\n", game_dir);
        printf("Save data:  %s\n", save_dir);
        crashreport_private_folder(save_dir);   /* blanked in crash dumps */

        xbox_path_init(game_dir, save_dir);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * Step 6b: run the title's own CRT lock-table initialiser.
     *
     * On a real MSVC CRT, mainCRTStartup calls _mtinit -> _mtinitlocks before
     * anything else, and that pre-creates every lock whose table entry is
     * flagged for static allocation. This port jumps straight to the XBE entry
     * point, so that never happened and the table at 0x001C58D0 stayed all
     * zeros.
     *
     * That is not a cosmetic gap. _lock(n) (sub_00160EBA) creates a missing
     * lock by calling _mtinitlocknum (sub_00160E3E), whose very first act is
     * _lock(10) to guard the table -- and lock 10 was itself missing, so the
     * two called each other until the stack died. It cost this project the
     * eleven C++ static-object registrations in 0x001652E0-0x00165FA0, which
     * had to be left out of the dispatch table to keep the title booting (see
     * the note above g_recomp_table).
     *
     * Calling the game's own _mtinitlocks here fixes it at the source: entry 10
     * is flagged 1 in the XBE's static data, so it gets a real critical section
     * before any CRT code can ask for one, and the recursion becomes
     * impossible. Must run after xbox_kernel_bridge_init(), because
     * _mtinitlocks reaches InitializeCriticalSectionAndSpinCount through the
     * kernel import thunks.
     */
    {
        extern void sub_00160DEE(void);   /* _mtinitlocks */
        /* On by default: per-thread TIBs removed the
         * crash that enabling this used to expose. XBOX_CRT_MTINIT=0 skips it. */
        const char *e = getenv("XBOX_CRT_MTINIT");
        if (!e || e[0] != '0') {
            printf("Initializing CRT lock table...\n");
            fflush(stdout);
            sub_00160DEE();
        }
    }

    /*
     * Optional: write-protect the low page (Xbox VA 0x0-0xFFF).
     *
     * That page exists only to back the fake TIB the recompiler needs because
     * it drops `fs:` segment prefixes (see xbox_MemoryLayoutInit). A side
     * effect is that **every null-pointer write in the title silently
     * succeeds** instead of faulting, and one of them corrupts the TIB: the
     * TLS pointer at 0x28 was observed changing from 0x00760000 to 0x0000003E,
     * after which the CRT's _threadstartex computed a destination of Xbox VA 4
     * and a length of 1,073,328,495 dwords and memcpy'd roughly 4 GB across
     * the whole address space -- which is what zeroes the CRT heap descriptor
     * at 0x001C4D94 and kills the title with a divide by zero.
     *
     * The TIB is written once during init, before this point, and nothing
     * should legitimately write low memory afterwards -- so with the page
     * read-only the first access violation *is* the offending instruction,
     * reported by the VEH handler above with an exact RIP and stack.
     *
     * XBOX_PROTECT_LOWPAGE=1.
     */
    {
        const char *lp = getenv("XBOX_PROTECT_LOWPAGE");
        if (lp && lp[0] == '1') {
            DWORD old_prot = 0;
            if (VirtualProtect((void *)g_xbox_mem_offset, 0x1000,
                               PAGE_READONLY, &old_prot)) {
                printf("Low page (VA 0x0-0xFFF) write-protected\n");
            } else {
                printf("Low page protect FAILED: %lu\n", GetLastError());
            }
            fflush(stdout);
        }
    }

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* The dispatch table's binary search requires ascending order, and the
     * generated table has repeatedly not been in it -- entries appended by
     * the recovery tooling land at the end. Establish the invariant here
     * rather than trusting it, and say so when it was not already true. */
    {
        size_t unordered = recomp_dispatch_init();
    recomp_probe_init_from_env();
        if (unordered)
            fprintf(stderr, "  [DISPATCH] sorted the table: %u entr%s were "
                    "unreachable by the binary search until now\n",
                    (unsigned)unordered, unordered == 1 ? "y" : "ies");
    }

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    {   /* Fork: indirect-call target cache, on from here -- every hook flag is
         * set by now (recomp_manual.c, XBOX_FIX_ICALL_CACHE=0 keeps it off). */
        extern void recomp_icall_cache_start(void);
        recomp_icall_cache_start();
    }
    /* Save profile Unlock all (XBOX_UNLOCK_ALL=1): the game's own "unlock
     * everything" cheat, as if its code had been entered once -- the flag its
     * cheat handler (0x82AE0) toggles and its unlock queries read (0x68C00,
     * 0x68C30, 0x6A290, 0x6A2F0). It is outside the saved profile, so no save
     * keeps it; the saves go to a folder of their own (Step 4). */
    if (launcher_unlock_all()) {
        *(volatile uint8_t *)(uintptr_t)(0x001E13A8u + g_xbox_mem_offset) = 1;
        printf("Unlock all: on (the game's unlock cheat, separate save folder)\n");
    }
    crashreport_game_start();   /* settings snapshot, freeze watchdog */
    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
/* ------------------------------------------------------------------ *
 * Crash reporter.
 *
 * The guest is ~10,500 translated functions with no host stack frames to
 * unwind, so a segfault otherwise reports nothing at all.  g_last_loc is
 * the watchdog global that every generated label stamps with its guest VA
 * (a single store, measured free), which turns a bare fault into the guest
 * address that was executing when it happened.
 * ------------------------------------------------------------------ */
extern volatile unsigned g_last_loc;

#if defined(_WIN32)
static LONG WINAPI recomp_crash_filter(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    /* Report the module-relative address, which addr2line resolves exactly.
     *
     * g_last_loc is printed only as a hint, and it is deliberately *not*
     * called "the guest location": it is a single global that every guest
     * thread stamps, so on a multi-threaded fault it names whichever thread
     * stored last, which need not be the one that faulted. It is also only a
     * lower bound, since not every generated file carries the stamps: a crash
     * this line attributed to sub_00179411 was placed by addr2line in
     * sub_00178DA8, on another thread. Trust the RVA. */
    HMODULE self = GetModuleHandle(NULL);
    fprintf(stderr, "CRASH: code=0x%08lX at host %p (module %p, rva 0x%08llX)"
                    " [tid %lu; last label stamped by any thread: loc_%08X]\n",
            (unsigned long)r->ExceptionCode, r->ExceptionAddress, (void *)self,
            (unsigned long long)((char *)r->ExceptionAddress - (char *)self),
            (unsigned long)GetCurrentThreadId(), g_last_loc);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        fprintf(stderr, "CRASH: %s address 0x%p\n",
                r->ExceptionInformation[0] ? "write to" : "read from",
                (void *)r->ExceptionInformation[1]);
    }
    fflush(stderr);
    {
        /* The report folder (crashreport.h). What only this thread can see
         * goes in: its guest registers (thread-local) and the last indirect
         * calls. Static buffers: after a stack overflow there is little stack. */
        static char details[1024];
        static WCHAR dir[MAX_PATH];
        extern volatile uint32_t g_icall_trace[16];
        extern volatile uint32_t g_icall_trace_idx;
        int len, k;
        len = snprintf(details, sizeof details,
                       "Guest:     eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
                       "           esp=%08X esi=%08X edi=%08X\n"
                       "Last label stamped by any thread: loc_%08X\n"
                       "Last 16 indirect-call targets (oldest first):\n ",
                       g_eax, g_ecx, g_edx, g_ebx, g_esp, g_esi, g_edi, g_last_loc);
        for (k = 0; k < 16 && len > 0 && len < (int)sizeof details - 12; k++)
            len += snprintf(details + len, sizeof details - len, " %08X",
                            g_icall_trace[(g_icall_trace_idx - 16 + k) & 15]);
        if (len > 0 && len < (int)sizeof details - 2) { details[len++] = '\n'; details[len] = 0; }
        if (crashreport_on_crash(ep, details, dir, MAX_PATH)) {
            if (s_crash_dialog)
                crashreport_crash_dialog(r->ExceptionCode, dir);
            return EXCEPTION_EXECUTE_HANDLER;
        }
    }
    if (s_crash_dialog) {
        WCHAR msg[MAX_PATH + 256], path[MAX_PATH];
        if (s_log_path[0] && MultiByteToWideChar(CP_ACP, 0, s_log_path, -1, path, MAX_PATH))
            swprintf(msg, MAX_PATH + 256,
                     L"SSX Tricky stopped because of an error (0x%08lX).\n\n"
                     L"The details are in the log file:\n%ls",
                     (unsigned long)r->ExceptionCode, path);
        else
            swprintf(msg, MAX_PATH + 256,
                     L"SSX Tricky stopped because of an error (0x%08lX).\n\n"
                     L"To record the details next time, turn on \"Write a log file\" in Settings.",
                     (unsigned long)r->ExceptionCode);
        MessageBoxW(NULL, msg, L"SSX Tricky", MB_OK | MB_ICONERROR | MB_TOPMOST);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif /* _WIN32 */

#if !defined(_WIN32)
static int ot_guest_main(void *unused)
{
    (void)unused;
    return ot_winmain(NULL, NULL, NULL, 1);
}
#endif

int main(int argc, char **argv)
{
    crashreport_install();      /* XBOX_CRASH_REPORT=0: nothing */
#if defined(_WIN32)
    SetUnhandledExceptionFilter(recomp_crash_filter);

    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
#else
    g_argc = argc;
    g_argv = argv;
    if (ot_window_wanted()) {
        /* The window and its event loop own the main thread; the game runs on a second one. */
        extern void d3d8_SetFrameSink(void (*fn)(const uint8_t *rgba, unsigned w, unsigned h));
        d3d8_SetFrameSink(ot_window_post_frame);
        return ot_window_run(ot_guest_main, NULL);
    }
    return ot_winmain(NULL, NULL, NULL, 1);
#endif
}
