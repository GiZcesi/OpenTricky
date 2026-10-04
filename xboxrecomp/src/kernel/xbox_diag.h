/*
 * xbox_diag -- in-process live diagnostics for the recompiled title.
 *
 * WHY THIS EXISTS
 * ---------------
 * Diagnosing this port has meant, over and over: edit a generated .c file to
 * insert an fprintf, rebuild (minutes), run, read, then edit the probe back
 * out. That loop is slow, and it is not safe -- probe removal has twice
 * damaged real code (a stripped closing brace in recomp_0003.c/0007.c, and a
 * stray `}` in xbox_HeapAlloc that made a whole run batch report "clean"
 * against a stale binary).
 *
 * Nearly every one of those probes was asking a question this server can
 * answer live, with no rebuild: what does that guest address hold, is the CRT
 * pool's free list still consistent, which indirect-call targets are missing,
 * is this VA dispatched.
 *
 * It runs inside the process on purpose. An external inspector would have to
 * marshal symbols, defeat ASLR and re-derive the guest layout through
 * ReadProcessMemory; in here, g_xbox_mem_offset, the pool descriptors, the
 * dispatch table and the miss list are just variables.
 *
 * PROTOCOL
 * --------
 * Line-based text over TCP on 127.0.0.1, so it drives from bash, python or
 * netcat with no client library. Send one command per line; the reply is zero
 * or more result lines followed by a single line "OK" or "ERR <reason>".
 *
 * Off unless XBOX_DIAG_PORT is set, and it costs nothing when off.
 *
 * Reads are deliberately unsynchronised -- the game keeps running while you
 * look, exactly like a debugger. Treat a single sample as a sample.
 */
#ifndef XBOX_DIAG_H
#define XBOX_DIAG_H

/*
 * Start the listener if XBOX_DIAG_PORT names a usable port. Safe to call when
 * the variable is unset (does nothing) and safe to call twice.
 */
void xbox_diag_start(void);

/* Stop serving and close the listener. */
void xbox_diag_stop(void);

/*
 * Write-watch support, driven by the `watch` command.
 *
 * xbox_diag_handle_fault() must be called FIRST from the process VEH, for both
 * EXCEPTION_ACCESS_VIOLATION and EXCEPTION_SINGLE_STEP. It returns non-zero
 * when it has handled the exception, in which case the VEH must return
 * EXCEPTION_CONTINUE_EXECUTION.
 */
int  xbox_diag_watch_add(unsigned int xbox_va);
/* As above, but reports only faults touching [xbox_va, xbox_va+len).
 * Protection is page-granular; without a range every write to the
 * other 4KB of the page is reported too, which buries the one that
 * matters. xbox_diag_watch_add() is this with len = 4. */
int  xbox_diag_watch_add_ex(unsigned int xbox_va, unsigned int len);
void xbox_diag_watch_clear(void);
int  xbox_diag_handle_fault(void *exception_pointers);

#endif /* XBOX_DIAG_H */
