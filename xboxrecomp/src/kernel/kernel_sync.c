/*
 * kernel_sync.c - Xbox Synchronization Primitives
 *
 * Implements events, semaphores, wait functions, kernel timers, and DPCs
 * using Win32 synchronization objects.
 *
 * Xbox synchronization model:
 *   - Events: notification (manual-reset) or synchronization (auto-reset)
 *   - Semaphores: standard counting semaphores
 *   - Wait functions: single and multiple, with optional timeout
 *   - Timers: kernel timers with optional DPC callback on expiry
 *   - DPCs: deferred procedure calls, executed via thread pool on Windows
 *
 * Time values use NT 100-nanosecond units (negative = relative).
 */

#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>

/* ============================================================================
 * Helper: Convert NT 100ns interval to Win32 milliseconds
 *
 * NT time intervals:
 *   - Negative = relative (most common), in 100ns units
 *   - Positive = absolute FILETIME
 *   - NULL = infinite wait
 * ============================================================================ */

static DWORD xbox_nt_timeout_to_ms(PLARGE_INTEGER Timeout)
{
    if (!Timeout)
        return INFINITE;

    if (Timeout->QuadPart == 0)
        return 0;

    if (Timeout->QuadPart < 0) {
        /* Relative: negative 100ns units */
        LONGLONG relative_100ns = -Timeout->QuadPart;
        DWORD ms = (DWORD)(relative_100ns / 10000);
        if (ms == 0 && relative_100ns > 0)
            ms = 1;
        return ms;
    }

    /* Absolute: compute delta from now */
    LARGE_INTEGER now;
    GetSystemTimeAsFileTime((LPFILETIME)&now);
    LONGLONG diff = Timeout->QuadPart - now.QuadPart;
    if (diff <= 0)
        return 0;
    return (DWORD)(diff / 10000);
}

/* ============================================================================
 * Events
 *
 * Xbox event types:
 *   XboxNotificationEvent (0) = manual-reset (Win32: TRUE)
 *   XboxSynchronizationEvent (1) = auto-reset (Win32: FALSE)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateEvent(
    PHANDLE EventHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    ULONG EventType,
    BOOLEAN InitialState)
{
    HANDLE hEvent;
    BOOL bManualReset;

    (void)ObjectAttributes;

    if (!EventHandle)
        return STATUS_INVALID_PARAMETER;

    /* XboxNotificationEvent = manual-reset, XboxSynchronizationEvent = auto-reset */
    bManualReset = (EventType == XboxNotificationEvent) ? TRUE : FALSE;

    hEvent = CreateEventW(NULL, bManualReset, InitialState, NULL);
    if (!hEvent) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateEvent: CreateEventW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *EventHandle = hEvent;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateEvent: handle=%p, type=%s, initial=%d",
        hEvent, bManualReset ? "notification" : "synchronization", InitialState);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtSetEvent(HANDLE EventHandle, PLONG PreviousState)
{
    if (PreviousState)
        *PreviousState = 0; /* We don't track previous state */

    if (!SetEvent(EventHandle)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtSetEvent: SetEvent failed for handle %p (error %u)",
            EventHandle, GetLastError());
        return STATUS_INVALID_HANDLE;
    }

    return STATUS_SUCCESS;
}

/*
 * KeSetEvent - kernel-mode event signal.
 * On Xbox, this is the kernel-mode equivalent of NtSetEvent.
 * The Object parameter is treated as a Win32 event HANDLE.
 * Returns the previous signal state.
 */
LONG __stdcall xbox_KeSetEvent(PVOID Event, LONG Increment, BOOLEAN Wait)
{
    HANDLE hEvent = (HANDLE)Event;

    (void)Increment;
    (void)Wait;

    /* We can't easily query previous state, so just set and return 0 */
    SetEvent(hEvent);
    return 0;
}

/* ============================================================================
 * Semaphores
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateSemaphore(
    PHANDLE SemaphoreHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    LONG InitialCount,
    LONG MaximumCount)
{
    HANDLE hSemaphore;

    (void)ObjectAttributes;

    if (!SemaphoreHandle)
        return STATUS_INVALID_PARAMETER;

    hSemaphore = CreateSemaphoreW(NULL, InitialCount, MaximumCount, NULL);
    if (!hSemaphore) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateSemaphore: CreateSemaphoreW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *SemaphoreHandle = hSemaphore;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateSemaphore: handle=%p, initial=%d, max=%d",
        hSemaphore, InitialCount, MaximumCount);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReleaseSemaphore(
    HANDLE SemaphoreHandle,
    LONG ReleaseCount,
    PLONG PreviousCount)
{
    if (!ReleaseSemaphore(SemaphoreHandle, ReleaseCount, PreviousCount)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtReleaseSemaphore: failed for handle %p (error %u)",
            SemaphoreHandle, GetLastError());
        return STATUS_INVALID_HANDLE;
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Wait Functions
 * ============================================================================ */

/*
 * Map Win32 WaitFor* return codes to NTSTATUS.
 */
static NTSTATUS xbox_wait_result_to_ntstatus(DWORD result, ULONG count)
{
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
        return (NTSTATUS)(STATUS_SUCCESS + (result - WAIT_OBJECT_0));

    switch (result) {
        case WAIT_TIMEOUT:          return STATUS_TIMEOUT;
        case WAIT_IO_COMPLETION:    return STATUS_ALERTED;
        case WAIT_ABANDONED_0:      return STATUS_ABANDONED;
        case WAIT_FAILED:           return STATUS_UNSUCCESSFUL;
        default:                    return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS __stdcall xbox_NtWaitForSingleObject(
    HANDLE Handle,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result = WaitForSingleObjectEx(Handle, ms, Alertable);
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_NtWaitForSingleObjectEx(
    HANDLE Handle,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result;

    /*
     * Same as NtWaitForSingleObject but with an explicit wait mode. We run
     * everything in one host process with no kernel/user split, so WaitMode
     * has nothing to select and is ignored.
     */
    (void)WaitMode;

    result = WaitForSingleObjectEx(Handle, ms, Alertable);
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_NtWaitForMultipleObjectsEx(
    ULONG Count,
    HANDLE Handles[],
    ULONG WaitType,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    BOOL bWaitAll;
    DWORD result;

    /* WaitType: 0 = WaitAll, 1 = WaitAny (matches NT definitions) */
    bWaitAll = (WaitType == 0) ? TRUE : FALSE;

    result = WaitForMultipleObjectsEx(Count, Handles, bWaitAll, ms, Alertable);
    return xbox_wait_result_to_ntstatus(result, Count);
}

/*
 * KeWaitForSingleObject - kernel-mode wait on a dispatcher object.
 * On Xbox, Objects can be events, timers, threads, etc.
 * We treat the object pointer as a Win32 HANDLE.
 */
NTSTATUS __stdcall xbox_KeWaitForSingleObject(
    PVOID Object,
    ULONG WaitReason,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    (void)WaitReason;
    (void)WaitMode;

    HANDLE hObject = (HANDLE)Object;
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result = WaitForSingleObjectEx(hObject, ms, Alertable);
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_KeWaitForMultipleObjects(
    ULONG Count,
    PVOID Objects[],
    ULONG WaitType,
    ULONG WaitReason,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout,
    PVOID WaitBlockArray)
{
    (void)WaitReason;
    (void)WaitMode;
    (void)WaitBlockArray;

    BOOL bWaitAll = (WaitType == 0) ? TRUE : FALSE;
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);

    /* Objects[] is an array of PVOID which we treat as HANDLE[] */
    DWORD result = WaitForMultipleObjectsEx(Count, (HANDLE*)Objects,
                                            bWaitAll, ms, Alertable);
    return xbox_wait_result_to_ntstatus(result, Count);
}

/* ============================================================================
 * Kernel Timers
 *
 * Xbox kernel timers are dispatcher objects that can be waited on and
 * optionally queue a DPC when they expire.
 *
 * Implementation: Each XBOX_KTIMER contains a Win32 event (for waitable
 * behavior); the timer thread below fires it at the due time and runs the
 * DPC, if any.
 * ============================================================================ */

/*
 * The timer thread.
 *
 * Timers used to be Windows timer-queue timers. Those fire on the system
 * clock's 15.6 ms grid whatever the timer resolution: asked for 9.4 ms, the
 * title's frame timer fired after 16.8 ms on average and up to 16 ms late.
 * The title's frame limiter (Application_FrameTimerCallback) re-arms a
 * one-shot timer every frame, so its frames landed on that grid -- 15.6 or
 * 31 ms apart -- and a race ran at 45-52 frames a second.
 *
 * Now one high-priority thread keeps the armed timers with exact due times
 * on the performance counter, and sleeps on a high-resolution waitable timer
 * until the earliest. Firing (event, DPC, periodic re-arm), arming and
 * cancelling all happen under one lock, so a timer is never freed while it
 * fires.
 */
#define KT_MAX 256
static PXBOX_KTIMER     g_kt[KT_MAX];         /* armed timers */
static LONGLONG         g_kt_due[KT_MAX];     /* QPC */
static LONGLONG         g_kt_period[KT_MAX];  /* QPC, 0 = one-shot */
static int              g_kt_n;
static CRITICAL_SECTION g_kt_lock;
static HANDLE           g_kt_wake, g_kt_wait;
static LONGLONG         g_kt_qpf;
static volatile LONG    g_kt_state;           /* 0 none, 1 starting, 2 ready */

static void kt_log_fire(PXBOX_KTIMER timer, LONGLONG now)
{
    /* XBOX_TIMER_LOG=1 (diagnostic): how late one-shot timers fire against
     * the delay asked for, summarised every 3 s. */
    static int on = -1;
    static LONG n = 0;
    static double sum_req = 0, sum_act = 0, max_late = 0;
    static LONGLONG last_print = 0;
    double act;
    if (on < 0) { const char *e = getenv("XBOX_TIMER_LOG"); on = e && (e[0] == '1' || e[0] == '2') ? e[0] - '0' : 0; }
    if (!on || !timer->armed_qpc || timer->Period) return;
    act = (double)(now - timer->armed_qpc) * 1000.0 / (double)g_kt_qpf;
    if (on == 2)    /* XBOX_TIMER_LOG=2: every one-shot fire, timestamped */
        fprintf(stderr, "[TFIRE] t=%.1f timer %p asked %lu after %.2f\n",
                (double)now * 1000.0 / (double)g_kt_qpf, (void *)timer, (unsigned long)timer->due_ms, act);
    n++; sum_req += timer->due_ms; sum_act += act;
    if (act - timer->due_ms > max_late) max_late = act - timer->due_ms;
    if (!last_print) last_print = now;
    if (now - last_print > 3 * g_kt_qpf) {
        fprintf(stderr, "[TIMER] %ld one-shot fires: asked %.2f ms, fired after %.2f ms on average (max late %.2f ms)\n",
                n, sum_req / n, sum_act / n, max_late);
        n = 0; sum_req = sum_act = max_late = 0;
        last_print = now;
    }
}

static int kt_find(PXBOX_KTIMER t)
{
    int i;
    for (i = 0; i < g_kt_n; i++)
        if (g_kt[i] == t) return i;
    return -1;
}

static void kt_remove_at(int i)
{
    g_kt_n--;
    g_kt[i] = g_kt[g_kt_n];
    g_kt_due[i] = g_kt_due[g_kt_n];
    g_kt_period[i] = g_kt_period[g_kt_n];
}

static DWORD WINAPI kt_thread(LPVOID unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        LARGE_INTEGER q;
        LONGLONG next = 0x7FFFFFFFFFFFFFFFll;
        int i;

        EnterCriticalSection(&g_kt_lock);
        QueryPerformanceCounter(&q);
        for (i = 0; i < g_kt_n; ) {
            PXBOX_KTIMER t = g_kt[i];
            if (g_kt_due[i] > q.QuadPart) {
                if (g_kt_due[i] < next) next = g_kt_due[i];
                i++;
                continue;
            }
            kt_log_fire(t, q.QuadPart);
            if (t->win32_event)
                SetEvent(t->win32_event);
            if (t->Dpc && t->Dpc->DeferredRoutine)
                t->Dpc->DeferredRoutine(t->Dpc, t->Dpc->DeferredContext,
                                        t->Dpc->SystemArgument1, t->Dpc->SystemArgument2);
            /* The DPC may have re-armed or cancelled this timer; only touch
             * the slot if it still holds the same timer. */
            if (i < g_kt_n && g_kt[i] == t && g_kt_due[i] <= q.QuadPart) {
                if (g_kt_period[i]) {
                    g_kt_due[i] += g_kt_period[i];
                    if (g_kt_due[i] <= q.QuadPart) g_kt_due[i] = q.QuadPart + g_kt_period[i];
                    if (g_kt_due[i] < next) next = g_kt_due[i];
                    i++;
                } else {
                    t->Inserted = FALSE;
                    kt_remove_at(i);
                }
            } else {
                i = 0;                       /* the list changed: rescan */
                next = 0x7FFFFFFFFFFFFFFFll;
            }
        }
        LeaveCriticalSection(&g_kt_lock);

        if (next == 0x7FFFFFFFFFFFFFFFll) {
            WaitForSingleObject(g_kt_wake, INFINITE);
        } else {
            LARGE_INTEGER due;
            HANDLE h[2];
            QueryPerformanceCounter(&q);
            if (next <= q.QuadPart) continue;
            due.QuadPart = -(LONGLONG)((double)(next - q.QuadPart) * 1e7 / (double)g_kt_qpf);
            if (due.QuadPart == 0) due.QuadPart = -1;
            h[0] = g_kt_wait;
            h[1] = g_kt_wake;
            if (g_kt_wait && SetWaitableTimer(g_kt_wait, &due, 0, NULL, NULL, FALSE))
                WaitForMultipleObjects(2, h, FALSE, INFINITE);
            else
                WaitForSingleObject(g_kt_wake, (DWORD)((next - q.QuadPart) * 1000 / g_kt_qpf) + 1);
        }
    }
    return 0;
}

static void xbox_ensure_timer_queue(void)
{
    if (InterlockedCompareExchange(&g_kt_state, 1, 0) == 0) {
        LARGE_INTEGER f;
        HANDLE th;
        QueryPerformanceFrequency(&f);
        g_kt_qpf = f.QuadPart;
        InitializeCriticalSection(&g_kt_lock);
        g_kt_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (0x2), where available. */
        g_kt_wait = CreateWaitableTimerExW(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);
        if (!g_kt_wait) g_kt_wait = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        th = CreateThread(NULL, 0, kt_thread, NULL, 0, NULL);
        if (th) CloseHandle(th);
        InterlockedExchange(&g_kt_state, 2);
        return;
    }
    while (g_kt_state != 2) Sleep(0);
}

VOID __stdcall xbox_KeInitializeTimerEx(PXBOX_KTIMER Timer, XBOX_TIMER_TYPE Type)
{
    (void)Type;

    if (!Timer)
        return;

    memset(Timer, 0, sizeof(XBOX_KTIMER));

    /* Create a manual-reset event for notification timers,
     * auto-reset for synchronization timers */
    BOOL manual_reset = (Type == XboxNotificationTimer) ? TRUE : FALSE;
    Timer->win32_event = CreateEventW(NULL, manual_reset, FALSE, NULL);
    Timer->Inserted = FALSE;
    Timer->Period = 0;
    Timer->Dpc = NULL;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeInitializeTimerEx: timer=%p, type=%d, event=%p",
        Timer, Type, Timer->win32_event);
}

BOOLEAN __stdcall xbox_KeSetTimer(PXBOX_KTIMER Timer, LARGE_INTEGER DueTime, PXBOX_KDPC Dpc)
{
    return xbox_KeSetTimerEx(Timer, DueTime, 0, Dpc);
}

BOOLEAN __stdcall xbox_KeSetTimerEx(
    PXBOX_KTIMER Timer,
    LARGE_INTEGER DueTime,
    LONG Period,
    PXBOX_KDPC Dpc)
{
    BOOLEAN was_inserted;
    LONGLONG rel_100ns;
    LARGE_INTEGER now;
    int i;

    if (!Timer)
        return FALSE;

    xbox_ensure_timer_queue();

    /* DueTime is 100 ns units: negative = relative, positive = absolute
     * system time, 0 = now. Kept exact, not rounded to milliseconds. */
    if (DueTime.QuadPart < 0) {
        rel_100ns = -DueTime.QuadPart;
    } else if (DueTime.QuadPart == 0) {
        rel_100ns = 0;
    } else {
        LARGE_INTEGER sys;
        GetSystemTimeAsFileTime((LPFILETIME)&sys);
        rel_100ns = DueTime.QuadPart - sys.QuadPart;
        if (rel_100ns < 0) rel_100ns = 0;
    }

    EnterCriticalSection(&g_kt_lock);
    was_inserted = Timer->Inserted;
    if (Timer->win32_event)
        ResetEvent(Timer->win32_event);
    Timer->Dpc = Dpc;
    Timer->Period = Period;
    QueryPerformanceCounter(&now);
    i = kt_find(Timer);
    if (i < 0 && g_kt_n < KT_MAX)
        i = g_kt_n++;
    if (i >= 0) {
        g_kt[i] = Timer;
        g_kt_due[i] = now.QuadPart + (LONGLONG)((double)rel_100ns * (double)g_kt_qpf / 1e7);
        g_kt_period[i] = Period > 0 ? (LONGLONG)Period * g_kt_qpf / 1000 : 0;
        Timer->Inserted = TRUE;
    } else {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC, "KeSetTimerEx: more than %d timers armed", KT_MAX);
    }
    Timer->armed_qpc = now.QuadPart;
    Timer->due_ms = (DWORD)(rel_100ns / 10000);
    LeaveCriticalSection(&g_kt_lock);
    SetEvent(g_kt_wake);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeSetTimerEx: timer=%p, due=%lld00ns, period=%ldms, dpc=%p",
        Timer, (long long)rel_100ns, (long)Period, Dpc);

    return was_inserted;
}

/*
 * Tear down whatever host timer-queue timer a KTIMER holds, armed or not, and
 * wait for a callback already running on it to finish.
 *
 * xbox_KeCancelTimer only deletes the queue timer while Inserted is set, which
 * is right for KeCancelTimer's return value but leaves two things behind: a
 * one-shot timer that has already fired still owns its queue-timer handle, and
 * a timer re-initialised while armed loses its handle to the memset and keeps
 * firing against the struct forever. Anything that is about to free or reuse a
 * XBOX_KTIMER calls this first.
 */
VOID xbox_KeReleaseTimerResources(PXBOX_KTIMER Timer)
{
    int i;
    if (!Timer)
        return;
    xbox_ensure_timer_queue();
    EnterCriticalSection(&g_kt_lock);       /* also waits out a firing */
    i = kt_find(Timer);
    if (i >= 0) kt_remove_at(i);
    Timer->Inserted = FALSE;
    LeaveCriticalSection(&g_kt_lock);
}

BOOLEAN __stdcall xbox_KeCancelTimer(PXBOX_KTIMER Timer)
{
    BOOLEAN was_inserted;
    int i;

    if (!Timer)
        return FALSE;

    xbox_ensure_timer_queue();
    EnterCriticalSection(&g_kt_lock);
    was_inserted = Timer->Inserted;
    i = kt_find(Timer);
    if (i >= 0) kt_remove_at(i);
    Timer->Inserted = FALSE;
    LeaveCriticalSection(&g_kt_lock);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeCancelTimer: timer=%p, was_inserted=%d", Timer, was_inserted);

    return was_inserted;
}

/* ============================================================================
 * Deferred Procedure Calls (DPCs)
 *
 * Xbox DPCs are typically queued from ISRs or timer callbacks to run at
 * DISPATCH_LEVEL. On Windows, we execute them immediately or via thread pool
 * since we don't have real IRQL levels.
 * ============================================================================ */

VOID __stdcall xbox_KeInitializeDpc(
    PXBOX_KDPC Dpc,
    PKDEFERRED_ROUTINE DeferredRoutine,
    PVOID DeferredContext)
{
    if (!Dpc)
        return;

    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
    Dpc->SystemArgument1 = NULL;
    Dpc->SystemArgument2 = NULL;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeInitializeDpc: dpc=%p, routine=%p", Dpc, DeferredRoutine);
}

/*
 * Thread pool callback for executing DPCs.
 */
static VOID CALLBACK xbox_dpc_work_callback(PTP_CALLBACK_INSTANCE Instance,
                                            PVOID Context)
{
    PXBOX_KDPC dpc = (PXBOX_KDPC)Context;

    (void)Instance;

    if (dpc && dpc->DeferredRoutine) {
        dpc->DeferredRoutine(dpc, dpc->DeferredContext,
                            dpc->SystemArgument1, dpc->SystemArgument2);
    }
}

BOOLEAN __stdcall xbox_KeInsertQueueDpc(
    PXBOX_KDPC Dpc,
    PVOID SystemArgument1,
    PVOID SystemArgument2)
{
    if (!Dpc || !Dpc->DeferredRoutine)
        return FALSE;

    Dpc->SystemArgument1 = SystemArgument1;
    Dpc->SystemArgument2 = SystemArgument2;

    /* Submit to the Windows thread pool for async execution */
    if (!TrySubmitThreadpoolCallback(xbox_dpc_work_callback, Dpc, NULL)) {
        /* Fallback: execute synchronously */
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_SYNC,
            "KeInsertQueueDpc: threadpool submit failed, executing synchronously");
        Dpc->DeferredRoutine(Dpc, Dpc->DeferredContext,
                            SystemArgument1, SystemArgument2);
    }

    return TRUE;
}

BOOLEAN __stdcall xbox_KeRemoveQueueDpc(PXBOX_KDPC Dpc)
{
    /*
     * On Windows, once submitted to the thread pool we can't easily cancel.
     * DPCs are typically very short-lived, so this is rarely called.
     * Return FALSE to indicate the DPC was not in the queue (may have already run).
     */
    (void)Dpc;
    return FALSE;
}

/* ============================================================================
 * KeSynchronizeExecution
 *
 * On Xbox, this raises IRQL to the interrupt's level and executes a routine.
 * Since we don't have real IRQLs, we just call the routine directly.
 * ============================================================================ */

BOOLEAN __stdcall xbox_KeSynchronizeExecution(
    PXBOX_KINTERRUPT Interrupt,
    PVOID SynchronizeRoutine,
    PVOID SynchronizeContext)
{
    typedef BOOLEAN (__stdcall *PKSYNCHRONIZE_ROUTINE)(PVOID);
    PKSYNCHRONIZE_ROUTINE routine = (PKSYNCHRONIZE_ROUTINE)SynchronizeRoutine;

    (void)Interrupt;

    if (!routine)
        return FALSE;

    return routine(SynchronizeContext);
}

/* ============================================================================
 * Events (continued)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtClearEvent(HANDLE EventHandle)
{
    if (!ResetEvent(EventHandle)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtClearEvent: ResetEvent failed (error %u)", GetLastError());
        return STATUS_INVALID_HANDLE;
    }
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Mutants (mutexes)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateMutant(
    PHANDLE MutantHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    BOOLEAN InitialOwner)
{
    HANDLE hMutex;

    (void)ObjectAttributes;

    if (!MutantHandle)
        return STATUS_INVALID_PARAMETER;

    hMutex = CreateMutexW(NULL, InitialOwner, NULL);
    if (!hMutex) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateMutant: CreateMutexW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *MutantHandle = hMutex;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateMutant: handle=%p initial_owner=%d", hMutex, InitialOwner);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount)
{
    /*
     * ReleaseMutex reports the previous count only through its own bookkeeping,
     * which Win32 does not expose. Callers overwhelmingly use PreviousCount as
     * an ignore-me out-param; report 0 (the count before this release took it
     * to unheld) rather than leaving the caller's storage uninitialised.
     */
    if (!ReleaseMutex(MutantHandle)) {
        DWORD err = GetLastError();
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtReleaseMutant: ReleaseMutex failed (error %u)", err);
        /* Releasing a mutex this thread does not own is the common failure. */
        return (err == ERROR_NOT_OWNER) ? STATUS_MUTANT_NOT_OWNED
                                        : STATUS_INVALID_HANDLE;
    }

    if (PreviousCount)
        *PreviousCount = 0;

    return STATUS_SUCCESS;
}
