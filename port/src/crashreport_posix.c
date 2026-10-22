/* crashreport_posix.c -- crashreport.h for hosts without the Windows crash reporter
 * (dbghelp minidumps, the report folder, the crash dialog).
 *
 * What it does instead: a fatal-signal handler that prints, to stderr and then to
 * crash.txt in the working directory, where the process died -- the faulting
 * address, the host registers, a host backtrace (with the module-relative offsets
 * addr2line/atos resolve) and the guest's own registers and last-seen labels, which
 * is what names the translated function that was running. No report folder, no
 * dialog. The MMIO trap (xboxrecomp/src/platform/mmio_trap.c) chains to this handler
 * for every fault that is not a device access. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <execinfo.h>
#include <stdarg.h>
#include "crashreport.h"
#include "recomp_types.h"
#include "version.h"
#include <sys/sysctl.h>

#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/ucontext.h>
#include <mach-o/dyld.h>
#endif

extern volatile unsigned g_last_loc;
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

static int s_report_fd = -1;
static char s_hdr[3072];      /* build, system and settings, filled at install (not in the signal handler) */
extern char **environ;

static void out(const char *fmt, ...)
{
    char b[512];
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
    (void)!write(2, b, (size_t)n);
    if (s_report_fd >= 0) (void)!write(s_report_fd, b, (size_t)n);
}

static void on_fatal(int sig, siginfo_t *si, void *ucv)
{
    static volatile sig_atomic_t in = 0;
    void *bt[48];
    int n, i;
    if (in++) { signal(sig, SIG_DFL); return; }     /* a fault inside the handler: give up */

    s_report_fd = open("crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    out("\n=== CRASH: signal %d (%s) at address %p, code %d ===\n", sig,
        sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" :
        sig == SIGFPE ? "SIGFPE" : sig == SIGABRT ? "SIGABRT" : "?", si ? si->si_addr : NULL,
        si ? si->si_code : 0);
    out("guest address of the fault (if it is a guest access): 0x%08X\n",
        (uint32_t)((uintptr_t)(si ? si->si_addr : 0) - (uintptr_t)g_xbox_mem_offset));
#if defined(__APPLE__) && defined(__aarch64__)
    if (ucv) {
        ucontext_t *uc = (ucontext_t *)ucv;
        _STRUCT_MCONTEXT64 *mc = uc->uc_mcontext;
        uintptr_t pc = (uintptr_t)mc->__ss.__pc;
        uintptr_t base = (uintptr_t)_dyld_get_image_header(0);
        out("host pc=%p lr=%p sp=%p fp=%p (image base %p, pc offset 0x%lx)\n", (void *)pc,
            (void *)mc->__ss.__lr, (void *)mc->__ss.__sp, (void *)mc->__ss.__fp, (void *)base,
            (unsigned long)(pc - base));
        for (i = 0; i < 29; i += 4)
            out(" x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx\n", i,
                (unsigned long long)mc->__ss.__x[i], i + 1, (unsigned long long)mc->__ss.__x[i + 1],
                i + 2, (unsigned long long)mc->__ss.__x[i + 2],
                i + 3 < 29 ? i + 3 : 0, (unsigned long long)(i + 3 < 29 ? mc->__ss.__x[i + 3] : 0));
        out("far=%p esr=%08x\n", (void *)mc->__es.__far, (unsigned)mc->__es.__esr);
    }
#else
    (void)ucv;
#endif
    out("%s", s_hdr);
    out("guest:  eax=%08X ecx=%08X edx=%08X ebx=%08X\n        esp=%08X esi=%08X edi=%08X\n",
        g_eax, g_ecx, g_edx, g_ebx, g_esp, g_esi, g_edi);
    out("last label stamped by any thread: loc_%08X\n", g_last_loc);
    out("last 16 indirect-call targets (oldest first):");
    for (i = 0; i < 16; i++) out(" %08X", g_icall_trace[(g_icall_trace_idx - 16 + (uint32_t)i) & 15]);
    out("\nhost backtrace:\n");
    n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n, 2);
    if (s_report_fd >= 0) backtrace_symbols_fd(bt, n, s_report_fd);
    if (s_report_fd >= 0) { close(s_report_fd); s_report_fd = -1; }
    out("=== (report also in crash.txt) ===\n");
    signal(sig, SIG_DFL);       /* return: the instruction faults again and the process ends */
    if (sig == SIGABRT) raise(SIGABRT);
}

void crashreport_install(void)
{
    static int done;
    struct sigaction sa;
    const char *e = getenv("XBOX_CRASH_REPORT");
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT }, i;
    if (done || (e && e[0] == '0')) return;
    done = 1;
    {   /* what a report needs to be read later: the build, the Mac, and the OT_/XBOX_ settings in effect */
        char os[64] = "?", model[64] = "?"; size_t l = sizeof os; int n = 0; char **ep;
        sysctlbyname("kern.osproductversion", os, &l, NULL, 0);
        l = sizeof model; sysctlbyname("hw.model", model, &l, NULL, 0);
        n += snprintf(s_hdr + n, sizeof s_hdr - (size_t)n, "build:   OpenTricky %s\nsystem:  macOS %s, %s\nsettings:", OT_VERSION, os, model);
        for (ep = environ; ep && *ep && n < (int)sizeof s_hdr - 100; ep++)
            if (!strncmp(*ep, "OT_", 3) || !strncmp(*ep, "XBOX_", 5)) n += snprintf(s_hdr + n, sizeof s_hdr - (size_t)n, " %.80s", *ep);
        snprintf(s_hdr + n, sizeof s_hdr - (size_t)n, "\n");
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < (int)(sizeof sigs / sizeof sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}

void crashreport_set_output(int player, const char *log_path, int output_redirected)
{ (void)player; (void)log_path; (void)output_redirected; }
void crashreport_game_start(void) {}
void crashreport_private_folder(const char *utf8) { (void)utf8; }
void crashreport_private_file_acp(const char *path) { (void)path; }
int crashreport_on_crash(EXCEPTION_POINTERS *ep, const char *details, wchar_t *dir_out, size_t dir_out_len)
{
    (void)ep;
    if (details) fprintf(stderr, "[crash] %s\n", details);
    if (dir_out && dir_out_len) dir_out[0] = 0;
    return 0;
}
void crashreport_fatal_overflow(EXCEPTION_POINTERS *ep) { (void)ep; }
void crashreport_crash_dialog(DWORD code, const wchar_t *dir) { (void)dir; fprintf(stderr, "[crash] exception 0x%08X\n", (unsigned)code); }
void crashreport_open_folder(const wchar_t *dir) { (void)dir; }
