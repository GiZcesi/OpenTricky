/*
 * xbox_logstats.h -- XBOX_LOG_STATS=1: the periodic statistics in the log.
 *
 * Counters printed every few seconds or every few thousand draws while the
 * game runs ([VSH], the [LIVE-PB] method histogram, the [KERNEL] call
 * summary, [PRESENT], audio and cache counters...) are development tools.
 * Most are printed on the pump thread, and a player's log file grew by about
 * 20 KB/s with them. They are off unless this variable is 1; one-off lines
 * (start-up, errors, the first occurrence of a counter) are printed either way.
 * The test tools that read those lines set the variable.
 *
 * xbox_log_flush(), in place of fflush for a line printed during play: the
 * log file's buffers are emptied every second by a thread of their own
 * (the port's main.c sets g_xbox_log_flusher when it starts it), and while
 * it runs this does nothing, rather than write the whole buffer (every
 * thread's lines) on the game's, the pump's or the audio thread. Without
 * that thread (in-memory log, unbuffered output) it flushes, as before.
 * A plain fflush writes on the calling thread; it stays on the start-up,
 * crash and exit paths, in diagnostics behind their own variables and in
 * the lines not moved to this.
 */
#ifndef XBOX_LOGSTATS_H
#define XBOX_LOGSTATS_H

#include <stdio.h>
#include <stdlib.h>

/* Read once per file; a race on the first calls only reads it again. */
static inline int xbox_log_stats(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XBOX_LOG_STATS");
        on = e && e[0] == '1';
    }
    return on;
}

extern volatile long g_xbox_log_flusher;   /* kernel_bridge.c; 1 once the flush thread runs */

static inline void xbox_log_flush(void)
{
    if (!g_xbox_log_flusher) fflush(stderr);
}

#endif
