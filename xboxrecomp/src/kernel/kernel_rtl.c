/*
 * kernel_rtl.c - Xbox Runtime Library Functions
 *
 * Implements Rtl* functions: critical sections, string init/conversion,
 * NTSTATUS→Win32 error mapping, time conversion, sprintf variants.
 *
 * Most of these map 1:1 to Win32 CRT functions.
 */

#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

/* ============================================================================
 * String Initialization
 * ============================================================================ */

VOID __stdcall xbox_RtlInitAnsiString(PXBOX_ANSI_STRING DestinationString, const char* SourceString)
{
    if (SourceString) {
        USHORT len = (USHORT)strlen(SourceString);
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + 1;
        DestinationString->Buffer = (PCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

VOID __stdcall xbox_RtlInitUnicodeString(PXBOX_UNICODE_STRING DestinationString, const WCHAR* SourceString)
{
    if (SourceString) {
        USHORT len = (USHORT)(wcslen(SourceString) * sizeof(WCHAR));
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + sizeof(WCHAR);
        DestinationString->Buffer = (PWCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

/* ============================================================================
 * String Conversion (ANSI ↔ Unicode)
 * ============================================================================ */

NTSTATUS __stdcall xbox_RtlAnsiStringToUnicodeString(
    PXBOX_UNICODE_STRING DestinationString,
    PXBOX_ANSI_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG unicode_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    unicode_len = (SourceString->Length + 1) * sizeof(WCHAR);

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PWCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, unicode_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)unicode_len;
    } else if (DestinationString->MaximumLength < unicode_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = MultiByteToWideChar(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length,
        DestinationString->Buffer, DestinationString->MaximumLength / sizeof(WCHAR));

    if (result > 0) {
        DestinationString->Length = (USHORT)(result * sizeof(WCHAR));
        DestinationString->Buffer[result] = L'\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_RtlUnicodeStringToAnsiString(
    PXBOX_ANSI_STRING DestinationString,
    PXBOX_UNICODE_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG ansi_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    ansi_len = SourceString->Length / sizeof(WCHAR) + 1;

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ansi_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)ansi_len;
    } else if (DestinationString->MaximumLength < ansi_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = WideCharToMultiByte(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length / sizeof(WCHAR),
        DestinationString->Buffer, DestinationString->MaximumLength,
        NULL, NULL);

    if (result >= 0) {
        DestinationString->Length = (USHORT)result;
        if ((USHORT)result < DestinationString->MaximumLength)
            DestinationString->Buffer[result] = '\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

/* ============================================================================
 * String Comparison
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlEqualString(
    PXBOX_ANSI_STRING String1,
    PXBOX_ANSI_STRING String2,
    BOOLEAN CaseInSensitive)
{
    if (String1->Length != String2->Length)
        return FALSE;

    if (CaseInSensitive)
        return _strnicmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
    else
        return strncmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
}

/*
 * RtlCompareMemoryUlong - Scans memory for a ULONG pattern.
 * Returns the number of bytes that matched.
 */
ULONG __stdcall xbox_RtlCompareMemoryUlong(PVOID Source, ULONG Length, ULONG Pattern)
{
    PULONG src = (PULONG)Source;
    ULONG count = Length / sizeof(ULONG);

    for (ULONG i = 0; i < count; i++) {
        if (src[i] != Pattern)
            return i * sizeof(ULONG);
    }
    return count * sizeof(ULONG);
}

/* ============================================================================
 * Critical Sections (shadow-mapped onto real native locks)
 * ============================================================================
 *
 * These used to be no-ops. The justification in the old comment was that
 * "the recompiled game runs single-threaded (all Xbox threads are called
 * synchronously), so there is no contention" -- that stopped being true:
 *
 *   - PsCreateSystemThreadEx creates a *real* OS thread. SSX Tricky spawns its
 *     file-I/O worker this way, and that worker walks the same file and handle
 *     lists the main thread walks.
 *   - The KeTickCount updater (kernel_bridge.c) is a second real thread.
 *
 * So the title was running three threads with every one of its own mutual
 * exclusion primitives disabled. That produces exactly the failure observed:
 * an intermittent (roughly 1 run in 3), timing-dependent crash inside a list
 * search (sub_00164600) reached through the file system -- a list read on one
 * thread while another mutates it.
 *
 * The fix is the shadow mapping the old TODO called for. The guest's own
 * 28-byte Xbox RTL_CRITICAL_SECTION cannot be handed to the native API (a
 * Win32 CRITICAL_SECTION is 40 bytes here and must live outside guest memory),
 * so each guest CS address is mapped to a native CRITICAL_SECTION held in a
 * fixed table. Win32 critical sections are recursive and owner-checked, which
 * matches Xbox RtlEnterCriticalSection semantics exactly.
 *
 * Entries are never recycled, so a shadow pointer stays valid for the life of
 * the process; that removes any use-after-free window if the guest frees a
 * structure containing a CS. The table is keyed by guest address, so a CS that
 * gets memcpy'd elsewhere correctly becomes a different lock -- the same thing
 * real hardware does with the embedded KEVENT.
 */

#define CS_SHADOW_CAP 4096u   /* power of two; SSX uses far fewer */

typedef struct {
    void            *key;   /* native pointer to the guest CS; NULL = free */
    CRITICAL_SECTION lock;
} cs_shadow;

static cs_shadow        g_cs_shadow[CS_SHADOW_CAP];
static CRITICAL_SECTION g_cs_table_lock;
static LONG             g_cs_table_ready = 0;
static LONG             g_cs_count       = 0;

static void cs_table_init_once(void)
{
    /* Init-once as an interlocked state machine: 0 = untouched,
     * 1 = initialising, 2 = ready. Cheap, and correct if two guest threads
     * race on first use. */
    if (InterlockedCompareExchange(&g_cs_table_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_cs_table_lock);
        InterlockedExchange(&g_cs_table_ready, 2);
        return;
    }
    while (InterlockedCompareExchange(&g_cs_table_ready, 2, 2) != 2) {
        Sleep(0);
    }
}

static size_t cs_hash(const void *key)
{
    uint64_t h = (uint64_t)(uintptr_t)key;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 29;
    return (size_t)(h & (CS_SHADOW_CAP - 1));
}

/*
 * Find (or create) the native lock shadowing this guest critical section.
 * Returns NULL only if the table is exhausted, in which case the caller
 * degrades to the old no-op behaviour rather than deadlocking.
 */
static CRITICAL_SECTION *cs_shadow_for(void *guest_cs)
{
    size_t i, slot;
    CRITICAL_SECTION *result = NULL;

    if (!guest_cs) {
        return NULL;
    }
    cs_table_init_once();
    slot = cs_hash(guest_cs);

    /* Lookup without the table lock. Every Enter and Leave came
     * through here, and taking g_cs_table_lock each time made it the most
     * contended lock in the process -- a quarter of the game thread's time in
     * a race went to waiting for it. Entries are never removed and a key is
     * published only after its lock is initialised, so a reader that finds
     * its key finds a ready lock; one that reaches an empty slot falls
     * through to the locked path below, which rechecks before inserting. */
    for (i = 0; i < CS_SHADOW_CAP; i++) {
        cs_shadow *e = &g_cs_shadow[(slot + i) & (CS_SHADOW_CAP - 1)];
        void *k = InterlockedCompareExchangePointer(&e->key, NULL, NULL);
        if (k == guest_cs) return &e->lock;
        if (k == NULL) break;
    }

    EnterCriticalSection(&g_cs_table_lock);
    for (i = 0; i < CS_SHADOW_CAP; i++) {
        cs_shadow *e = &g_cs_shadow[(slot + i) & (CS_SHADOW_CAP - 1)];
        if (e->key == guest_cs) {
            result = &e->lock;
            break;
        }
        if (e->key == NULL) {
            /* First use of this guest CS. Titles do not always call
             * RtlInitializeCriticalSection -- a zero-filled structure is a
             * valid starting state for some CRT paths -- so create the shadow
             * lazily on Enter as well as on Initialize. A short spin before
             * blocking suits the brief holds the title makes. */
            InitializeCriticalSectionAndSpinCount(&e->lock, 1000);
            InterlockedExchangePointer(&e->key, guest_cs);   /* publish last */
            g_cs_count++;
            result = &e->lock;
            break;
        }
    }
    LeaveCriticalSection(&g_cs_table_lock);

    if (!result) {
        static LONG warned = 0;
        if (InterlockedExchange(&warned, 1) == 0) {
            fprintf(stderr,
                    "  [RTL] critical-section shadow table full (%u entries); "
                    "further sections are unsynchronised\n",
                    (unsigned)CS_SHADOW_CAP);
        }
    }
    return result;
}

/*
 * Mirror the lock state back into the guest's own structure.
 *
 * The Xbox RTL_CRITICAL_SECTION is 0x1C bytes:
 *     0x00  KEVENT Event (0x10 bytes)
 *     0x10  LONG   LockCount        (-1 when free, per the Rtl convention)
 *     0x14  LONG   RecursionCount
 *     0x18  HANDLE OwningThread
 * Nothing in this implementation reads those fields, but guest code is free
 * to, and leaving them frozen at whatever the guest last wrote would be a lie
 * about state we now genuinely track.
 */
static void cs_mirror_guest(void *guest_cs, LONG lock_count, LONG recursion,
                            ULONG owner)
{
    volatile LONG *p;
    if (!guest_cs) {
        return;
    }
    p = (volatile LONG *)((uint8_t *)guest_cs + 0x10);
    p[0] = lock_count;
    p[1] = recursion;
    p[2] = (LONG)owner;
}

VOID __stdcall xbox_RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    CRITICAL_SECTION *lock = cs_shadow_for(CriticalSection);
    if (!lock) {
        return;
    }

    /*
     * Try briefly, then block. The old loop spun on TryEnter with Sleep(0)
     * for as long as the lock was held, so a contended lock cost a whole core
     * and delayed its owner; it existed to print a warning after 5 s, which
     * XBOX_CS_WATCH=1 still does.
     */
    if (!TryEnterCriticalSection(lock)) {
        static int watch = -1;
        if (watch < 0) { const char *e = getenv("XBOX_CS_WATCH"); watch = e && e[0] == '1'; }
        if (watch) {
            DWORD start = GetTickCount();
            BOOL moaned = FALSE;
            while (!TryEnterCriticalSection(lock)) {
                if (!moaned && GetTickCount() - start > 5000) {
                    fprintf(stderr,
                            "  [RTL] thread %lu has waited >5s for critical section "
                            "%p (possible unbalanced Enter/Leave)\n",
                            GetCurrentThreadId(), (void *)CriticalSection);
                    fflush(stderr);
                    moaned = TRUE;
                }
                Sleep(0);
            }
        } else {
            EnterCriticalSection(lock);
        }
    }

    cs_mirror_guest(CriticalSection, 0, 1, GetCurrentThreadId());
}

VOID __stdcall xbox_RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    CRITICAL_SECTION *lock = cs_shadow_for(CriticalSection);
    if (!lock) {
        return;
    }
    cs_mirror_guest(CriticalSection, -1, 0, 0);
    LeaveCriticalSection(lock);
}

VOID __stdcall xbox_RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    (void)cs_shadow_for(CriticalSection);   /* creates the shadow lock */
    cs_mirror_guest(CriticalSection, -1, 0, 0);
}

/* ============================================================================
 * NTSTATUS → Win32 Error Code Mapping
 * ============================================================================ */

ULONG __stdcall xbox_RtlNtStatusToDosError(NTSTATUS Status)
{
    switch (Status) {
        case STATUS_SUCCESS:                    return ERROR_SUCCESS;
        case STATUS_INVALID_PARAMETER:          return ERROR_INVALID_PARAMETER;
        case STATUS_NO_MEMORY:                  return ERROR_NOT_ENOUGH_MEMORY;
        case STATUS_INSUFFICIENT_RESOURCES:     return ERROR_NO_SYSTEM_RESOURCES;
        case STATUS_ACCESS_DENIED:              return ERROR_ACCESS_DENIED;
        case STATUS_OBJECT_NAME_NOT_FOUND:      return ERROR_FILE_NOT_FOUND;
        case STATUS_OBJECT_PATH_NOT_FOUND:      return ERROR_PATH_NOT_FOUND;
        case STATUS_OBJECT_NAME_COLLISION:      return ERROR_ALREADY_EXISTS;
        case STATUS_NO_SUCH_FILE:               return ERROR_FILE_NOT_FOUND;
        case STATUS_END_OF_FILE:                return ERROR_HANDLE_EOF;
        case STATUS_INVALID_HANDLE:             return ERROR_INVALID_HANDLE;
        case STATUS_NOT_IMPLEMENTED:            return ERROR_CALL_NOT_IMPLEMENTED;
        case STATUS_UNSUCCESSFUL:               return ERROR_GEN_FAILURE;
        case STATUS_PENDING:                    return ERROR_IO_PENDING;
        case STATUS_BUFFER_OVERFLOW:            return ERROR_MORE_DATA;
        case STATUS_NO_MORE_FILES:              return ERROR_NO_MORE_FILES;
        case STATUS_NOT_SUPPORTED:              return ERROR_NOT_SUPPORTED;
        case STATUS_CANCELLED:                  return ERROR_CANCELLED;
        case STATUS_ALREADY_COMMITTED:          return ERROR_COMMITMENT_LIMIT;
        default:
            /* Fall back to RtlNtStatusToDosError from ntdll if available */
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_RTL,
                "RtlNtStatusToDosError: unmapped status 0x%08X", Status);
            return ERROR_MR_MID_NOT_FOUND;
    }
}

/* ============================================================================
 * Time Conversion
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlTimeFieldsToTime(PXBOX_TIME_FIELDS TimeFields, PLARGE_INTEGER Time)
{
    SYSTEMTIME st;
    FILETIME ft;

    st.wYear         = (WORD)TimeFields->Year;
    st.wMonth        = (WORD)TimeFields->Month;
    st.wDayOfWeek    = (WORD)TimeFields->Weekday;
    st.wDay          = (WORD)TimeFields->Day;
    st.wHour         = (WORD)TimeFields->Hour;
    st.wMinute       = (WORD)TimeFields->Minute;
    st.wSecond       = (WORD)TimeFields->Second;
    st.wMilliseconds = (WORD)TimeFields->Milliseconds;

    if (!SystemTimeToFileTime(&st, &ft))
        return FALSE;

    Time->LowPart  = ft.dwLowDateTime;
    Time->HighPart = ft.dwHighDateTime;
    return TRUE;
}

VOID __stdcall xbox_RtlTimeToTimeFields(PLARGE_INTEGER Time, PXBOX_TIME_FIELDS TimeFields)
{
    FILETIME ft;
    SYSTEMTIME st;

    ft.dwLowDateTime  = Time->LowPart;
    ft.dwHighDateTime = Time->HighPart;

    if (FileTimeToSystemTime(&ft, &st)) {
        TimeFields->Year         = (SHORT)st.wYear;
        TimeFields->Month        = (SHORT)st.wMonth;
        TimeFields->Day          = (SHORT)st.wDay;
        TimeFields->Hour         = (SHORT)st.wHour;
        TimeFields->Minute       = (SHORT)st.wMinute;
        TimeFields->Second       = (SHORT)st.wSecond;
        TimeFields->Milliseconds = (SHORT)st.wMilliseconds;
        TimeFields->Weekday      = (SHORT)st.wDayOfWeek;
    } else {
        memset(TimeFields, 0, sizeof(XBOX_TIME_FIELDS));
    }
}

/* ============================================================================
 * Exception Handling
 * ============================================================================ */

VOID __stdcall xbox_RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PVOID ExceptionRecord, PVOID ReturnValue)
{
    /* Delegate to Win32 RtlUnwind */
    RtlUnwind(TargetFrame, TargetIp, (PEXCEPTION_RECORD)ExceptionRecord, ReturnValue);
}

VOID __stdcall xbox_RtlRaiseException(PVOID ExceptionRecord)
{
    RaiseException(
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionCode,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionFlags,
        ((PEXCEPTION_RECORD)ExceptionRecord)->NumberParameters,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionInformation);
}

VOID __stdcall xbox_RtlRip(PCHAR ApiName, PCHAR Expression, PCHAR Message)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL, "RtlRip: %s - %s: %s",
        ApiName ? ApiName : "?",
        Expression ? Expression : "?",
        Message ? Message : "?");

#ifdef _DEBUG
    DebugBreak();
#endif
}

/* ============================================================================
 * String Formatting (Rtl sprintf variants → CRT)
 * ============================================================================ */

int __cdecl xbox_RtlSnprintf(char* buffer, size_t count, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, count, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlSprintf(char* buffer, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsprintf(buffer, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlVsnprintf(char* buffer, size_t count, const char* format, va_list argptr)
{
    return vsnprintf(buffer, count, format, argptr);
}

int __cdecl xbox_RtlVsprintf(char* buffer, const char* format, va_list argptr)
{
    return vsprintf(buffer, format, argptr);
}
