/*
 * crashreport.h -- a report folder for crashes and freezes.
 *
 * When the game crashes (an unhandled exception) or freezes (nothing new
 * reaches the screen for XBOX_HANG_SECONDS), a folder is written under
 * "CrashReports\" beside the executable (or %LOCALAPPDATA%\SSX Tricky\
 * CrashReports\ when that is not writable) with:
 *
 *   report.txt    what happened, where (module-relative address, guest
 *                 registers), build, Windows version, CPU, GPU, RAM
 *   crash.dmp /   a small minidump: thread list, registers and stack
 *   hang.dmp      pointers only (MiniDumpFilterMemory), module names
 *                 without their folders (MiniDumpFilterModulePaths);
 *                 no game memory, no code, no data segments
 *   log.txt       the last lines the game printed, even with LogFile=0
 *                 (kept in a ring buffer in memory)
 *   settings.txt  the .ini and the XBOX_* variables in effect
 *
 * Folders and the user name are removed from every text file (a path keeps
 * only its file name), and the user name is blanked in the minidump. Nothing
 * is sent anywhere.
 *
 * XBOX_CRASH_REPORT=0 turns all of it off (the previous behaviour exactly).
 * XBOX_HANG_SECONDS=N sets the freeze threshold (default 30, 0 = no watchdog).
 * XBOX_CRASH_TEST=av|div0|stack|hang provokes a crash, or a simulated freeze,
 * XBOX_CRASH_TEST_DELAY seconds (default 20) after the game starts (tests).
 *
 * Cost while playing: none per frame. One thread wakes once a second to read
 * three counters; the ring buffer is static and filled by a thread that only
 * wakes when a 4 KB output buffer is written (a few times a second in a race).
 */
#ifndef CRASHREPORT_H
#define CRASHREPORT_H

#include <stddef.h>
#include <windows.h>

/* main(), first thing: reads XBOX_CRASH_REPORT, starts the report thread. */
void crashreport_install(void);

/* After the log decision: player = a launcher start (dialogs allowed);
 * log_path = the log file in use, or NULL / "" when there is none -- then,
 * unless the output is redirected (test runs), the game's output goes to the
 * in-memory ring buffer instead of nowhere. */
void crashreport_set_output(int player, const char *log_path, int output_redirected);

/* Right before the entry point: snapshots the settings and arms the freeze
 * watchdog (and the XBOX_CRASH_TEST thread, when set). */
void crashreport_game_start(void);

/* From the unhandled exception filter, on the faulting thread. details is a
 * short text block (guest registers...) added to report.txt. Returns 1 and
 * the folder (for the message) when a report was written. */
int crashreport_on_crash(EXCEPTION_POINTERS *ep, const char *details,
                         wchar_t *dir_out, size_t dir_out_len);

/* From the vectored handler, first thing, on a stack overflow: the faulting
 * thread has no stack left for the filter or a message box, so the report
 * thread writes the report, shows the message and ends the process. Returns
 * only when reports are off (or after REPORT_WAIT_MS if the report hangs). */
void crashreport_fatal_overflow(EXCEPTION_POINTERS *ep);

/* The crash message (folder, issues page); Yes opens the folder. */
void crashreport_crash_dialog(DWORD code, const wchar_t *dir);

/* Opens a report folder in Explorer. */
void crashreport_open_folder(const wchar_t *dir);

#endif
