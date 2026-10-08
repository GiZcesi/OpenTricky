/**
 * kernel_bridge.c - Bridge between translated game code and kernel functions
 *
 * Problem:
 *   Translated game code calls kernel functions via indirect calls through
 *   the kernel thunk table at VA 0x0036B7C0. In the XBE file, these entries
 *   contain unresolved ordinals (0x80000000 | ordinal). On real Xbox hardware,
 *   the kernel loader replaces these with actual function pointers before the
 *   game runs.
 *
 * Solution:
 *   1. After xbox_MemoryLayoutInit copies .rdata, call xbox_kernel_bridge_init()
 *   2. Replace each ordinal entry in Xbox memory with a synthetic VA
 *   3. When RECOMP_ICALL encounters a synthetic VA, route it to a per-ordinal
 *      bridge function that reads args from the simulated Xbox stack, translates
 *      pointer arguments from Xbox VA→native, and calls the kernel function.
 *
 * Synthetic VA scheme:
 *   Each thunk slot i gets VA 0xFE000000 + i*4
 *   The lookup function checks this range and dispatches appropriately.
 *
 * Why per-ordinal bridges instead of a generic trampoline:
 *   Kernel functions receive Xbox pointers (32-bit VAs) that must be translated
 *   to native pointers by adding g_xbox_mem_offset. Different functions have
 *   different parameter layouts (pointer vs value), so each needs its own bridge.
 */

#include "xbox_perf.h"
#include <xmmintrin.h>
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <setjmp.h>
static void trap_nan_check_path(const char *path);   /* XBOX_TRAP_NAN_FILE, below */

/* Implemented in nv2a/nv2a_live_pb.c; declared here the same way
 * xbox_memory_layout.c declares nv2a_live_pb_tick. */
extern void nv2a_live_pb_note_ring(uint32_t base, uint32_t size);
extern void nv2a_live_pb_capture(uint8_t *mem_base);

/* Access to recompiled code globals.
 * Thread-local (must match the real definitions in xbox_memory_layout.c
 * exactly, __thread included -- each genuine Xbox worker thread
 * gets its own register state; g_xbox_mem_offset is NOT thread-local, all
 * threads legitimately share the same Xbox address space mapping). */
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __thread uint32_t g_ebx, g_esi, g_edi;
extern __thread uint32_t g_seh_ebp;
extern ptrdiff_t g_xbox_mem_offset;

/* Dispatch table lookup (for function pointer args) */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/* Memory access - same as recomp_types.h MEM32 but without the #define guard */
#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)xbox_fold_ram_alias((uint32_t)(addr)) + g_xbox_mem_offset))

/* Translate Xbox VA to native pointer (NULL-safe: 0 → NULL) */
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)xbox_fold_ram_alias((uint32_t)(va)) + g_xbox_mem_offset) : NULL)

/* ── Dispatcher-object VA -> native HANDLE resolution ──────
 * Real Xbox kernel dispatcher objects (KEVENT, KSEMAPHORE, ...) are structs
 * games embed directly in their own memory and pass by address -- there is
 * no NtCreateEvent-style call to intercept for them (KeInitializeEvent isn't
 * bridged; the object's "identity" is just its Xbox VA). Casting that VA
 * straight to a Win32 HANDLE (as if it were a real handle value) is invalid
 * and makes WaitForSingleObject/WaitForMultipleObjects fail immediately.
 * Instead, lazily synthesize one real Win32 auto-reset event per distinct
 * Xbox VA the first time any wait/set-event bridge touches it, and reuse
 * that same native HANDLE for every later touch of the same VA. */
#define XBOX_DISPATCHER_HANDLE_MAP_SIZE 256
static uint32_t g_dispatcher_handle_keys[XBOX_DISPATCHER_HANDLE_MAP_SIZE];
static HANDLE   g_dispatcher_handle_values[XBOX_DISPATCHER_HANDLE_MAP_SIZE];
static int      g_dispatcher_handle_count = 0;

/* XBOX_FIX_DISPATCHMAP (default 1; =0 restores the unlocked map).
 * The map used to be read and extended with no lock. Two threads touching
 * the same VA for the first time at once (the title's timer thread entering
 * its first wait on its control event while the main thread signals that
 * event to register the frame timer) each created their own Win32 event and
 * appended it: the waiter and the signaller then used different events, the
 * wake-up was lost, and the boot froze before the EA logo with the frame
 * timer never armed. With the fix, lookup and insertion happen under one
 * lock, so a VA always maps to exactly one event. The same lock covers the
 * handle-token table below, which had the same check-then-insert race. */
static SRWLOCK g_dispatch_lock = SRWLOCK_INIT;
static volatile LONG g_dispatch_fix = -1;

static int dispatch_fix_on(void)
{
    if (g_dispatch_fix < 0) {
        const char *e = getenv("XBOX_FIX_DISPATCHMAP");
        LONG v = (e && e[0] == '0') ? 0 : 1;
        if (InterlockedCompareExchange(&g_dispatch_fix, v, -1) == -1)
            fprintf(stderr, "[DISPATCHMAP] XBOX_FIX_DISPATCHMAP=%ld\n", (long)v);
    }
    return (int)g_dispatch_fix;
}

static HANDLE resolve_dispatcher_unlocked(uint32_t obj_va)
{
    int i;
    for (i = 0; i < g_dispatcher_handle_count; i++) {
        if (g_dispatcher_handle_keys[i] == obj_va) return g_dispatcher_handle_values[i];
    }
    {
        HANDLE h = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (g_dispatcher_handle_count < XBOX_DISPATCHER_HANDLE_MAP_SIZE) {
            g_dispatcher_handle_keys[g_dispatcher_handle_count] = obj_va;
            g_dispatcher_handle_values[g_dispatcher_handle_count] = h;
            g_dispatcher_handle_count++;
        } else {
            /* Past the map size every call makes a new unregistered event,
             * so a wait and a signal on such a VA never meet. */
            static LONG warned = 0;
            if (InterlockedExchange(&warned, 1) == 0) {
                fprintf(stderr, "  [DISPATCHMAP] more than %d dispatcher objects: VA 0x%08X "
                        "and later ones get an unshared event (waits on them can hang)\n",
                        XBOX_DISPATCHER_HANDLE_MAP_SIZE, obj_va);
                fflush(stderr);
            }
        }
        return h;
    }
}

static HANDLE xbox_resolve_dispatcher_handle(uint32_t obj_va)
{
    HANDLE h;
    if (!obj_va) return NULL;

    /* A real Win32 HANDLE (e.g. from a prior NtCreateEvent, read back and
     * passed here by value) is a small kernel-object-table value, never a
     * plausible Xbox VA -- use it directly, exactly as before, to avoid
     * regressing the already-working NtCreateEvent-backed call sites. Only
     * values that actually look like Xbox memory addresses go through the
     * lazy-synthesis path below. */
    if (obj_va < XBOX_BASE_ADDRESS) return (HANDLE)(uintptr_t)obj_va;
    /* One object, one key: an object in contiguous memory can be named by
     * its 0x80000000 alias or its plain address. */
    obj_va = xbox_fold_ram_alias(obj_va);

    if (!dispatch_fix_on()) return resolve_dispatcher_unlocked(obj_va);
    AcquireSRWLockExclusive(&g_dispatch_lock);
    h = resolve_dispatcher_unlocked(obj_va);
    ReleaseSRWLockExclusive(&g_dispatch_lock);
    return h;
}

/*
 * xbox_signal_dispatcher_event - public wrapper so other runtime subsystems
 * (e.g. the PFIFO/GPU pump thread in xbox_memory_layout.c, which has no real
 * vertical-blank interrupt to fire a VBlank event on its own) can signal an
 * Xbox-VA-identified dispatcher object through the same lazily-synthesized
 * handle the KeSetEvent/KeWaitForSingleObject/KeWaitForMultipleObjects
 * bridges above use, so a wait on a given VA and a periodic signal on that
 * same VA always resolve to the same underlying Win32 event.
 */
void xbox_signal_dispatcher_event(uint32_t obj_va)
{
    HANDLE h = xbox_resolve_dispatcher_handle(obj_va);
    if (h) SetEvent(h);
}

/* ── Synthetic VA range (for function exports) ─────────── */

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)

/* ── x86 port I/O (in/out) ─────────────────────────────────
 *
 * Backs the XBOX_IO_READ.../XBOX_IO_WRITE... macros in recomp_types.h. Unlike
 * MEM8/MEM32 (real, mapped memory), each port is a distinct piece of real
 * hardware behavior with no generic emulation possible -- has to be modeled
 * per-port as they're found. Found needed while tracing SSX Tricky's D3D
 * device-init chain into real hardware bring-up code --
 *
 * Port 0x80C0 is confirmed (cross-referenced against xemu's
 * hw/xbox/acpi_xbox.c, which emulates this faithfully) to be the first
 * byte of the Xbox ACPI unit's GPIO block (PM I/O base 0x8000 +
 * XBOX_PM_GPIO_BASE 0xC0): bit 5 is the TV encoder's "field pin", a signal
 * that alternates every read to indicate which interlaced video field is
 * current. Software polls it to detect field transitions. We don't have a
 * real TV encoder driving this, but the alternation itself is exactly the
 * observable behavior real hardware provides -- so toggling it here on
 * every read, independent of any actual video timing, satisfies any driver
 * code that's just waiting for *a* transition rather than a specific one.
 */
#define XBOX_IO_PORT_ACPI_GPIO_FIELD_PIN  0x80C0u
#define XBOX_IO_FIELD_PIN_BIT             0x20u

uint32_t xbox_io_port_read(uint16_t port, int width)
{
    (void)width;
    switch (port) {
    case XBOX_IO_PORT_ACPI_GPIO_FIELD_PIN: {
        static uint8_t field_pin = 0;
        field_pin ^= 1;
        return (uint32_t)field_pin << 5;
    }
    default:
        /* Unimplemented port: same "nothing here" behavior real
         * unpopulated I/O space has. */
        return 0;
    }
}

void xbox_io_port_write(uint16_t port, int width, uint32_t value)
{
    (void)port; (void)width; (void)value;
    /* No known write-sensitive port hit yet; absorb silently. */
}

/* ── Kernel data exports ──────────────────────────────────
 *
 * Some kernel ordinals are DATA exports (structs/variables), not functions.
 * The game reads their thunk entries and dereferences the result to access
 * the data. These cannot use synthetic VAs — they must point to real,
 * dereferenceable addresses in the Xbox VA space.
 *
 * We allocate a "kernel data area" at XBOX_KERNEL_DATA_BASE and populate
 * it with the expected structures.
 */

#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)xbox_fold_ram_alias((uint32_t)(addr)) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)xbox_fold_ram_alias((uint32_t)(addr)) + g_xbox_mem_offset))

/**
 * Get the Xbox VA of data for a kernel DATA export ordinal.
 * Returns 0 if the ordinal is not a data export (i.e., it's a function).
 */
static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    case  16: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  64: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  70: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 353: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 354: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    default:  return 0;  /* Not a data export */
    }
}

/*
 * KeTickCount updater.
 *
 * KeTickCount is a kernel DATA export: real hardware advances it continuously
 * (once per clock tick) and titles read it directly as their millisecond clock,
 * never through a function call, so nothing else can lazily refresh it on
 * access. It was previously written exactly once, at kernel_data_init below,
 * with a comment asserting "a background thread in main.c updates this every
 * ~1ms" -- but no such thread was ever written (confirmed: main.c contains no
 * reference to it at all), so the value stayed frozen at its boot value for the
 * entire run.
 *
 * That froze the game's own clock. Confirmed live: SSX Tricky's frame pacing
 * (Application_FrameTimerCallback) computes each frame's elapsed time as
 * `KeTickCount - last`, which was therefore always 0, so its time accumulator
 * grew by the full 16.667 ms frame target every single frame and never drained
 * -- the next-frame delay it handed the timer climbed without bound
 * (observed: 33, 49, 66, 83 ... ms), making the game run progressively slower
 * and never settle at its intended 60 Hz.
 *
 * A dedicated thread is the right shape here precisely because the export is
 * data, not a call: this mirrors what the hardware clock genuinely does. The
 * 1 ms cadence matches KeTickCount's real millisecond resolution.
 */
static volatile LONG g_tick_thread_running = 0;
static HANDLE g_tick_thread = NULL;

static DWORD WINAPI xbox_tick_count_thread(LPVOID param)
{
    /* From the performance counter, not GetTickCount: GetTickCount
     * advances in 15.6 ms steps however high the timer resolution is set, so
     * the title's frame limiter measured each frame as 0, 15 or 31 ms. */
    LARGE_INTEGER f, q0, q;
    DWORD base = GetTickCount();
    (void)param;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    while (g_tick_thread_running) {
        QueryPerformanceCounter(&q);
        BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) =
            base + (DWORD)((q.QuadPart - q0.QuadPart) * 1000 / f.QuadPart);
        Sleep(1);
    }
    return 0;
}

/**
 * Initialize kernel data export values at the kernel data area.
 * Called during bridge init, after Xbox memory is mapped.
 */
static void kernel_data_init(void)
{
    /* XboxHardwareInfo (ordinal 322) - XBOX_HARDWARE_INFO
     *   +0: ULONG Flags (0 = retail, 0x20 = devkit)
     *   +4: UCHAR GpuRevision
     *   +5: UCHAR McpRevision
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;   /* Retail */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1; /* NV2A A1 */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1; /* MCPX B1 */

    /* XboxKrnlVersion (ordinal 324) - XBOX_KRNL_VERSION
     *   +0: USHORT Major (1)
     *   +2: USHORT Minor (0)
     *   +4: USHORT Build (5849 = XDK version)
     *   +6: USHORT Qfe (0)
     */
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    /* KeTickCount (ordinal 156) - seeded here, then advanced every ~1 ms by
     * xbox_tick_count_thread (started just below). See that thread's comment
     * for why a thread is required and what froze without it. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = GetTickCount();
    if (!g_tick_thread) {
        g_tick_thread_running = 1;
        g_tick_thread = CreateThread(NULL, 0, xbox_tick_count_thread, NULL, 0, NULL);
        if (!g_tick_thread) {
            g_tick_thread_running = 0;
            fprintf(stderr, "  [KERNEL] WARNING: KeTickCount updater thread failed to start "
                            "(error %lu) -- the game's millisecond clock will not advance\n",
                    GetLastError());
            fflush(stderr);
        }
    }

    /* LaunchDataPage (ordinal 164) - NULL (no launch data) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;

    /* PsThreadObjectType (ordinal 259) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE) = 0;

    /* ExEventObjectType (ordinal 17) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE) = 0;

    /* IoCompletionObjectType (ordinal 65) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;

    /* IoDeviceObjectType (ordinal 71) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;

    /* XboxHDKey (ordinal 323) - 16 bytes of zeros (no key) */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxSignatureKey (ordinal 325) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxLANKey (ordinals 326, 355) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxAlternateSignatureKeys (ordinals 327, 356) - 256 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    /* XePublicKeyData (ordinal 357) - 284 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

/* ── Per-slot ordinal and bridge function ────────────────── */

/* Ordinal for each slot (read from Xbox memory during init) */
static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* How many kernel calls to trace before going quiet. The default keeps
 * boot logs readable, but a hang usually happens well past call 200 --
 * set XBOX_KCALL_LOG to a larger number (or 0 for unlimited) to see what
 * the title is actually doing when it goes quiet. */
static long g_kernel_call_log_limit = 200;
static int  g_kernel_call_log_init = 0;

static long kernel_call_log_limit(void)
{
    if (!g_kernel_call_log_init) {
        const char *e = getenv("XBOX_KCALL_LOG");
        if (e) {
            long v = atol(e);
            g_kernel_call_log_limit = (v <= 0) ? 0x7FFFFFFFL : v;
        }
        g_kernel_call_log_init = 1;
    }
    return g_kernel_call_log_limit;
}

/* Current dispatching slot -- thread-local: recomp_lookup_kernel sets this
 * and returns kernel_thunk_dispatch as the function to call; with real
 * concurrent Xbox threads (see bridge_PsCreateSystemThreadEx) a shared global
 * here would race if two threads resolve a kernel call at the same time,
 * each potentially dispatching through the other's slot. Declared here
 * (rather than next to kernel_thunk_dispatch below) so ordinal-dependent
 * bridges earlier in the file, like bridge_KeSetTimer, can read it too. */
static __thread int g_kernel_dispatch_slot = -1;

/* Log counter - limit output to avoid flooding */
static int g_kernel_call_count = 0;

/* Read Xbox stack arg as uint32_t.
 * After kernel_thunk_dispatch pops the dummy return address (g_esp += 4),
 * arg0 is at g_esp+0, arg1 at g_esp+4, etc. */
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

/* ── Per-ordinal bridge functions ─────────────────────────
 *
 * Each bridge reads args from the Xbox stack, translates pointer
 * args from Xbox VA→native, calls the kernel function, and stores
 * the result in g_eax.
 *
 * Xbox cdecl: args pushed right-to-left, caller cleans stack.
 * Xbox stdcall: args pushed right-to-left, callee cleans stack.
 * In our case the caller (translated code) does "PUSH32" for each arg
 * before calling, and the kernel function's ret-N is handled by the
 * translated code's own stack adjustment.
 */

/* ── PsCreateSystemThreadEx (ordinal 255) ────────────────
 * NTSTATUS PsCreateSystemThreadEx(
 *   PHANDLE ThreadHandle,      // arg0: Xbox VA → pointer
 *   ULONG ThreadExtraSize,     // arg1: value
 *   ULONG KernelStackSize,     // arg2: value
 *   ULONG TlsDataSize,         // arg3: value
 *   PULONG ThreadId,           // arg4: Xbox VA → pointer (can be NULL)
 *   PVOID StartContext1,       // arg5: Xbox VA → opaque
 *   PVOID StartContext2,       // arg6: Xbox VA → opaque
 *   BOOLEAN CreateSuspended,   // arg7: value
 *   BOOLEAN DebugStack,        // arg8: value
 *   PXBOX_SYSTEM_ROUTINE StartRoutine  // arg9: Xbox function pointer
 * )
 *
 * For static recompilation, we don't create a real thread.
 * Instead we call the StartRoutine synchronously via RECOMP_ICALL.
 * This is correct because on Xbox, the entry point creates a system
 * thread and returns, and the thread runs the actual game.
 *
 * Update: worker (non-first) calls now spawn a genuine CreateThread instead
 * of running as a nested synchronous call -- see the full explanation right
 * before the worker branch in bridge_PsCreateSystemThreadEx below. The first
 * call still runs synchronously, inheriting the caller's own OS thread,
 * exactly as before.
 */
static volatile LONG g_thread_call_count = 0;

/*
 * PsTerminateSystemThread never returns on real hardware -- it ends the
 * calling thread outright. Since threads run synchronously here as nested
 * C calls (see the comment above), "terminate" has to unwind the C call
 * stack all the way back to the fn() call site below, not just return one
 * level and fall through into whatever code happens to follow (which was
 * never meant to be reached and is not valid to execute -- this is exactly
 * what caused a SIGILL from a wild jump into unmapped memory, tracing
 * PsTerminateSystemThread's Xbox behavior for SSX Tricky specifically). A small stack of jmp_bufs supports nested
 * thread-start calls (a worker thread spawned while another thread's call
 * chain is still active each gets its own termination point).
 *
 * Thread-local: a jmp_buf captures stack/CPU context that's only valid within
 * the thread that called setjmp -- now that worker threads run as genuine
 * concurrent OS threads (see bridge_PsCreateSystemThreadEx), each real thread
 * needs its own independent nesting stack, not one shared (and racing) across
 * all of them.
 */
#define MAX_NESTED_THREAD_CALLS 16
static __thread jmp_buf g_thread_terminate_jmp[MAX_NESTED_THREAD_CALLS];
static __thread int g_thread_terminate_depth = 0;

/*
 * Call a thread start routine with termination support: returns 1 if the
 * routine called PsTerminateSystemThread (via longjmp), 0 if it returned
 * normally. Falls back to a plain call with no termination support if the
 * nesting stack is exhausted (should not happen in practice at depth 16).
 */
static int run_thread_start_routine(recomp_func_t fn)
{
    int terminated;
    if (g_thread_terminate_depth >= MAX_NESTED_THREAD_CALLS) {
        fn();
        return 0;
    }
    g_thread_terminate_depth++;
    if (setjmp(g_thread_terminate_jmp[g_thread_terminate_depth - 1]) == 0) {
        fn();
        terminated = 0;
    } else {
        terminated = 1;
    }
    g_thread_terminate_depth--;
    return terminated;
}

/*
 * Worker-thread trampoline: runs on a genuine new OS thread. Each real
 * thread has its own independent copy of every g_eax/g_esp/etc. register
 * global (they're __thread -- see xbox_memory_layout.c), so unlike
 * the old synchronous model, there is nothing to save/restore here: this
 * thread simply never touches any other thread's copies.
 */
typedef struct {
    recomp_func_t fn;
    uint32_t context1;
    uint32_t context2;
    uint32_t stack_top;
    uint32_t stack_size;   /* so the worker can describe its own stack in its TIB */
} xbox_worker_thread_params_t;

static DWORD WINAPI xbox_worker_thread_trampoline(LPVOID param)
{
    xbox_worker_thread_params_t p = *(xbox_worker_thread_params_t *)param;
    int terminated;
    free(param);

    /*
     * Claim this thread's own TIB before running any guest code.
     *
     * `fs:` is per-thread on hardware; the lifter drops the prefix, so without
     * this every thread would share one block at Xbox VA 0 -- and the title
     * genuinely uses both the SEH chain head at +0x00 and the TLS pointer at
     * +0x28 per thread. Sharing them scrambled the exception chain across
     * stacks and, once CRT thread-init ran, sent _threadstartex into a ~4 GB
     * memcpy off a torn TLS pointer.
     *
     * The stack bounds are this worker's own: stack_top is the high address
     * handed out by xbox_HeapAlloc, and the allocation is p.stack_size below it.
     */
    xbox_tib_alloc_for_thread(p.stack_top, p.stack_top - p.stack_size);

    g_esp = p.stack_top;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = p.context2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = p.context1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    g_seh_ebp = g_esp;

    terminated = run_thread_start_routine(p.fn);
    fprintf(stderr, "  [KERNEL] worker thread 0x%p %s (g_eax=0x%08X)\n",
            (void *)p.fn, terminated ? "terminated via PsTerminateSystemThread" : "returned", g_eax);
    fflush(stderr);
    return 0;
}

static void bridge_PsCreateSystemThreadEx(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t stack_size      = STACK_ARG(2);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    uint32_t start_routine   = STACK_ARG(9);
    LONG call_index = InterlockedIncrement(&g_thread_call_count);
    int is_first_call = (call_index == 1);

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx #%ld: routine=0x%08X ctx1=0x%08X ctx2=0x%08X stack=0x%X\n",
            call_index, start_routine, start_context1, start_context2, stack_size);
    fflush(stderr);

    /* Xbox thread start routines receive two parameters:
     *   void ThreadRoutine(PVOID StartContext1, PVOID StartContext2)
     * pushed onto the simulated stack right-to-left, matching real stdcall.
     *
     * First call: the game's main thread entry point. Runs synchronously,
     * inheriting the calling OS thread and its current register state --
     * this IS the game starting, on our process's own primary thread.
     *
     * Subsequent calls: worker threads. Spawn a genuine concurrent OS thread
     * with its own freshly allocated Xbox-memory stack (sized from the
     * game's own requested KernelStackSize, arg 2, floored at a sane
     * minimum). This matches real Xbox hardware, where PsCreateSystemThreadEx
     * genuinely creates a new hardware thread rather than running inline --
     * a worker that itself needs to wait on another concurrently-running
     * worker (impossible under the old nested-synchronous-call model, and
     * the root cause of a real deadlock this was written to fix) now works
     * exactly like it does on real hardware. Any caller that needs a given
     * worker's result before continuing is expected to synchronize on it
     * itself (event, critical section, or waiting on the returned thread
     * handle below) -- exactly as it must on real hardware, since these
     * really are independent threads there too. */
    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                /* Main game thread: run directly, inheriting register state */
                int terminated;
                if (xbox_handle_ptr) {
                    BRIDGE_MEM32(xbox_handle_ptr) = (uint32_t)(uintptr_t)GetCurrentThread();
                }
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                terminated = run_thread_start_routine(fn);
                g_esp += 12;
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: main thread %s (g_eax=0x%08X)\n",
                        terminated ? "terminated via PsTerminateSystemThread" : "returned", g_eax);
                fflush(stderr);
            } else {
                uint32_t stack_top;
                xbox_worker_thread_params_t *params;
                HANDLE hThread;

                if (stack_size < 0x4000) stack_size = 0x10000; /* sane floor if the game passed 0 */
                /* Kernel stacks never need the GPU-visible region. */
                stack_top = xbox_HeapAllocVirtual(stack_size, 16);
                if (!stack_top) {
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: worker stack alloc failed\n");
                    fflush(stderr);
                    if (xbox_handle_ptr) BRIDGE_MEM32(xbox_handle_ptr) = 0;
                    g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
                    return;
                }
                stack_top += stack_size - 16;

                params = (xbox_worker_thread_params_t *)malloc(sizeof(*params));
                params->fn        = fn;
                params->context1  = start_context1;
                params->context2  = start_context2;
                params->stack_top = stack_top;
                params->stack_size = stack_size;

                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawning worker 0x%08X (ctx=0x%08X, stack=0x%08X)\n",
                        start_routine, start_context1, stack_top);
                fflush(stderr);

                hThread = CreateThread(NULL, 0, xbox_worker_thread_trampoline, params, 0, NULL);
                if (hThread) {
                    if (xbox_handle_ptr) BRIDGE_MEM32(xbox_handle_ptr) = (uint32_t)(uintptr_t)hThread;
                    fprintf(stderr, "  [KERNEL]   worker thread handle = 0x%08X written to 0x%08X\n",
                            (uint32_t)(uintptr_t)hThread, xbox_handle_ptr);
                    fflush(stderr);
                    /* Intentionally not closed here -- the game may later wait on
                     * this handle (KeWaitForSingleObject/KeWaitForMultipleObjects,
                     * which pass small real HANDLE values straight through --
                     * see xbox_resolve_dispatcher_handle). Real Xbox thread
                     * handles are similarly caller-owned until explicitly closed. */
                } else {
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: CreateThread failed (error %lu)\n",
                            GetLastError());
                    fflush(stderr);
                    free(params);
                    if (xbox_handle_ptr) BRIDGE_MEM32(xbox_handle_ptr) = 0;
                    g_eax = 0xC0000001u; /* STATUS_UNSUCCESSFUL */
                    return;
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtClose (ordinal 187) ───────────────────────────────
 * NTSTATUS NtClose(HANDLE Handle)
 * Handle is a value (not a pointer), so safe for generic call.
 */
/* Handle-table helpers; defined further below. Xbox memory slots are 32-bit
 * but native HANDLEs are 64-bit pointers, so handles are kept in a table and
 * referenced by tagged 32-bit tokens. */
static void   bridge_write_handle(uint32_t handle_va, HANDLE h);
static HANDLE bridge_take_handle(uint32_t token);

static void hpath_drop(uint32_t tok);   /* handle -> path map, below */

static void bridge_NtClose(void)
{
    uint32_t raw_handle = STACK_ARG(0);

    if ((long)g_kernel_call_count <= kernel_call_log_limit()) {
        fprintf(stderr, "  [KERNEL] NtClose: handle=0x%08X\n", raw_handle);
        fflush(stderr);
    }
    hpath_drop(raw_handle);

    /* Close real handles but skip fake/synthetic ones */
    if (raw_handle && raw_handle != 0xDEAD0001u && raw_handle != 0xBEEF0010u) {
        HANDLE h = bridge_take_handle(raw_handle);
        if (h && h != INVALID_HANDLE_VALUE) {
            /* Drop the directory scan bound to this handle before Windows
             * hands the value out again. xbox_NtClose does this, but the
             * translated code closes through here, so a scan the title stopped
             * early (XFindFirstSaveGame + XFindClose on a match) stayed bound
             * to the value. The next "U:\" open that got the same value then
             * continued that old scan past the save folder, the save was not
             * found, its folder-name buffer was never filled (0xDEADC0DE fill),
             * and an overwrite -- which deletes the old files first -- failed
             * with "Save Failed." and lost the save. XBOX_FIX_DIRCTX=0 restores
             * the old behaviour. */
            static int fix_dirctx = -1;
            if (fix_dirctx < 0) {
                const char *e = getenv("XBOX_FIX_DIRCTX");
                fix_dirctx = !(e && e[0] == '0');
            }
            if (fix_dirctx) xbox_dir_context_release(h);
            CloseHandle(h);
        }
    }
    g_eax = 0; /* STATUS_SUCCESS */
}

/* Contiguous memory is returned as its uncached alias, 0x80000000 + physical,
 * as the Xbox kernel returns it. The title depends on that: it
 * treats a negative value in a mesh's triangle table as "already relocated"
 * (sub_00103900), so with plain addresses every outfit change relocated the
 * board-shadow table again until its pointers wrapped onto a rider's mesh
 * parts; and it frees D3D buffers as `physical | 0x80000000`. The heap it
 * builds on the 54 MB block therefore has to live at the alias too.
 *
 * Not yet for blocks the caller confines below 64 MB (high < 0xFFFFFFFF):
 * those are D3D's push buffer and surfaces, and D3D's flow control compares
 * `GET | 0x80000000` against the ring's bounds. With an aliased ring that
 * test finally succeeds, D3D waits for GET to advance -- and the NV2A
 * emulation never writes GET back, so boot stalls in sub_0016B870. Until it
 * does, those stay plain (the mixed forms D3D has always run with here).
 * XBOX_CONTIG_ALIAS=0 restores plain addresses everywhere, =2 aliases all. */
static uint32_t xbox_contig_alias_ex(uint32_t va, uint32_t high)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_CONTIG_ALIAS"); on = e ? atoi(e) : 1; }
    if (!on || !va || va >= XBOX_GPU_VISIBLE_END) return va;
    if (on == 1 && high < 0xFFFFFFFFu) return va;
    return va | 0x80000000u;
}
static uint32_t xbox_contig_alias(uint32_t va) { return xbox_contig_alias_ex(va, 0xFFFFFFFFu); }

/* ── MmAllocateContiguousMemory (ordinal 165) ─────────────
 * PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)
 */
static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    /* Allocate from Xbox heap so MEM32(result) works correctly */
    uint32_t xbox_va = xbox_contig_alias(xbox_HeapAlloc(size, 4096));

    if (g_kernel_call_count <= 100) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmAllocateContiguousMemoryEx (ordinal 166) ───────────
 * PVOID MmAllocateContiguousMemoryEx(SIZE_T size, ULONG_PTR low, ULONG_PTR high,
 *                                     ULONG alignment, ULONG protect)
 */
static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);

    /* Allocate inside the physical range the caller asked for -- that range is
     * how the title guarantees the GPU can address the memory. */
    if (align < 4096) align = 4096;
    uint32_t xbox_va = xbox_HeapAllocRange(size, align, low, high);

    /*
     * Always report these, not just during the first hundred kernel calls.
     * This is where the title takes its large memory regions -- the pools it
     * sub-allocates everything else from -- so a failure here is invisible in
     * the ordinal histogram but fatal several layers up.
     */
    {
        /* Failures are always reported: the 16-line cap used to run out before
         * DirectSound's allocations, and its one NULL -- which comes back to
         * the title as E_OUTOFMEMORY from DirectSoundCreate -- was invisible. */
        static int shown = 0;
        if (shown < 16 || !xbox_va) {
            shown++;
            extern volatile unsigned g_last_loc;
            fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u (%u KB) align=%u"
                            " low=0x%08X high=0x%08X -> Xbox VA 0x%08X%s (after loc_%08X)\n",
                    size, size / 1024u, align, low, high, xbox_va,
                    xbox_va ? "" : "   FAILED", (unsigned)g_last_loc);
            fflush(stderr);
        }
    }

    /* Publish the push-buffer allocation to the live consumer.
     *
     * The title programs its one-time texture, format and surface state into
     * this ring during D3D init, then recycles it -- all before the consumer
     * can attach, because attaching needs a D3D11 device that cannot exist
     * until that same init finishes. The consumer therefore has to start
     * capturing before it can parse, and it cannot learn the ring's extent
     * from the channel context: during init the title publishes neither the
     * write pointer nor the base/limit fields. The allocation is the one
     * place the extent is known that early. */
    if (xbox_va && size >= 0x100000u && size <= 0x400000u)
        nv2a_live_pb_note_ring(xbox_va, size);

    g_eax = xbox_contig_alias_ex(xbox_va, high);
}

/* Mirrors XBOX_FAKE_KERNEL_HEADER_VA for recomp_types.h's
 * xbox_resolve_uncached_alias, which cannot include the layout header.
 *
 * It lives here rather than in xbox_memory_layout.c on purpose: that file is
 * swapped wholesale by tools/audit/layout_snap.py, so a definition there is
 * reverted every time a layout snapshot is restored.
 *
 * A variable, not a literal: it *was* a literal 0x00741000 inside
 * recomp_types.h, and when the kernel data area moved for the 64 MB layout
 * work the literal stayed behind and pointed into the title's arena. Every
 * RenderWare cache-line read got garbage, and three different memory layouts
 * "rendered nothing" for what looked like a memory-map reason. */
uint32_t g_xbox_fake_hdr_va = XBOX_FAKE_KERNEL_HEADER_VA;

/* Operands of a comparison that has to cross a function split -- see the
 * SPLIT_* macros in recomp_types.h for why the flags themselves cannot. */
__thread uint32_t g_cmp_a = 0;
__thread uint32_t g_cmp_b = 0;
__thread int      g_cmp_test = 0;


/* ── MmFreeContiguousMemory (ordinal 171) ─────────────────
 * VOID MmFreeContiguousMemory(PVOID BaseAddress)
 */
static void bridge_MmFreeContiguousMemory(void)
{
    uint32_t addr = STACK_ARG(0);
    xbox_HeapFree(addr);
    g_eax = 0;
}

/* ── NtAllocateVirtualMemory (ordinal 184) ────────────────
 * NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *     PULONG AllocationSize, ULONG AllocationType, ULONG Protect)
 */
static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);  /* PVOID* in Xbox VA */
    uint32_t zero_bits = STACK_ARG(1);
    uint32_t size_ptr = STACK_ARG(2);  /* PULONG in Xbox VA */
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    /* Read the requested size from Xbox memory */
    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    /* Read the base address hint (0 = let kernel choose) */
    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if ((long)g_kernel_call_count <= kernel_call_log_limit()) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    if (size == 0) {
        g_eax = 0xC0000045u; /* STATUS_INVALID_PAGE_PROTECTION */
        return;
    }

    /*
     * Xbox NtAllocateVirtualMemory supports two modes:
     * - MEM_RESERVE (0x2000): Reserve virtual address space
     * - MEM_COMMIT  (0x1000): Commit pages within a reserved region
     * - MEM_RESERVE|MEM_COMMIT (0x3000): Both in one call
     *
     * Our Xbox heap (bump allocator) always commits memory immediately,
     * so MEM_COMMIT on an already-reserved region is a no-op.
     * Only allocate new memory when MEM_RESERVE is requested.
     */
    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {
        /* MEM_COMMIT only, on an already-reserved region.
         * The memory is already committed by our bump allocator.
         * Don't change the base address - just return success. */
        if ((long)g_kernel_call_count <= kernel_call_log_limit()) {
            fprintf(stderr, "  [KERNEL] → MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* Allocate from Xbox heap (MEM_RESERVE or MEM_RESERVE|MEM_COMMIT).
     * Virtual memory: above the GPU line (xbox_HeapAllocVirtual). */
    uint32_t xbox_va = xbox_HeapAllocVirtual(size, 4096);
    if (!xbox_va) {
        g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
        return;
    }

    /* Write back the allocated address and actual size */
    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtFreeVirtualMemory (ordinal 199) ────────────────────
 * NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize,
 *     ULONG FreeType)
 */
static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t free_type = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtFreeVirtualMemory(
        XBOX_TO_NATIVE(base_ptr), XBOX_TO_NATIVE(size_ptr), free_type);
}

/* ── ExAllocatePool / ExAllocatePoolWithTag (ordinals 15, 16) ─
 * Must allocate from Xbox heap so the returned pointer is an Xbox VA
 * that can be accessed via MEM32(). Native HeapAlloc returns 64-bit
 * pointers that get truncated and produce garbage Xbox VAs.
 */
static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if ((long)g_kernel_call_count <= kernel_call_log_limit()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if ((long)g_kernel_call_count <= kernel_call_log_limit()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' → Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── KfRaiseIrql / KfLowerIrql (ordinals 160, 161) ──────
 *
 * These are __fastcall: the new IRQL arrives in **ecx**, and nothing is
 * pushed. The arg-size table below has always said so (0 stack bytes), but
 * these two bridges read STACK_ARG(0) anyway, so they took whatever the guest
 * happened to have on its stack. That produced 9,484 warnings a run of the
 * form "KfLowerIrql: attempt to raise IRQL from 2 to 48" -- 48, 224, 176, 128,
 * 80 being stack debris, not IRQLs -- and left the tracked level stuck.
 */
static void bridge_KfRaiseIrql(void)
{
    g_eax = (uint32_t)xbox_KfRaiseIrql((UCHAR)(g_ecx & 0xFF));
}

static void bridge_KfLowerIrql(void)
{
    xbox_KfLowerIrql((UCHAR)(g_ecx & 0xFF));
    g_eax = 0;
}

/* ── KeRaiseIrqlToDpcLevel (ordinal 129) ─────────────────── */
static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
}

/* ── RtlInitializeCriticalSection / Enter / Leave (ordinals 291, 277, 294) ─ */
static void bridge_RtlInitializeCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* ── KeQueryInterruptTime (ordinal 125) ──────────────────────
 * Never bridged -- kernel_thunk_dispatch's "no bridge" fallback returned 0
 * unconditionally. First reached inside the game's own software timer queue
 * (sub_00152230, called from Application_ArmFrameTimer et al.) which uses
 * this to compute a newly-armed timer's expiration baseline; with it always
 * reading 0, every timer's expiration math is computed against a bogus
 * "current time", so the pump thread's periodic check never correctly
 * recognizes the frame timer as due. */
static void bridge_KeQueryInterruptTime(void)
{
    ULONGLONG t = xbox_KeQueryInterruptTime();
    g_eax = (uint32_t)(t & 0xFFFFFFFFu);
    g_edx = (uint32_t)(t >> 32);
}

/* ── KeQueryPerformanceCounter / Frequency (ordinals 126, 127) ─ */
static void bridge_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceFrequency();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

/* ── KeQuerySystemTime (ordinal 128) ─────────────────────── */
static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}

/* ── MmQueryStatistics (ordinal 181) ─────────────────────── */
static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

/* ── NtCreateEvent (ordinal 189) ─────────────────────────── */
static void bridge_NtCreateEvent(void)
{
    uint32_t handle_ptr = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    uint32_t event_type = STACK_ARG(2);
    uint32_t initial_state = STACK_ARG(3);

    /* Use local HANDLE to avoid 8-byte write to 4-byte Xbox memory slot.
     * On x64, HANDLE is 8 bytes but Xbox expects 4-byte handles. */
    HANDLE local_handle = NULL;
    NTSTATUS status = xbox_NtCreateEvent(
        &local_handle,
        XBOX_TO_NATIVE(obj_attr_ptr),
        event_type, initial_state);

    if (handle_ptr) {
        bridge_write_handle(handle_ptr, local_handle);
    }

    fprintf(stderr, "  [BRIDGE] NtCreateEvent: handle_ptr=0x%08X type=%u init=%u → status=0x%08X handle=0x%08X\n",
            handle_ptr, event_type, initial_state, (uint32_t)status,
            (uint32_t)(uintptr_t)local_handle);

    g_eax = (uint32_t)status;
}

/* ── NtSetEvent (ordinal 225) / NtClearEvent (ordinal 186) ──
 * The xbox_* implementations already existed in kernel_sync.c and were
 * registered in the kernel_thunks table, but no bridge wrapper existed, so
 * every call fell through kernel_thunk_dispatch's "no bridge" path and
 * silently returned 0 without touching the event. That mattered: the game's
 * frame pacing reconciles its timer callback with the main loop through a
 * Win32 event (XBoxExecutionMan_SignalFrameEvent / WaitForFrameEvent). NtCreateEvent was bridged, so the event
 * object existed, but nothing could ever signal it -- so the main loop's
 * per-frame poll never observed a ready frame and spun forever without
 * advancing the top-level state machine. */
/* Handle-table tagging. Defined here rather than beside the table itself
 * further down because callers above need the tag to tell a bridge token
 * from a raw Xbox VA. */
#define BRIDGE_HANDLE_TAG  0x48000000u
#define BRIDGE_HANDLE_MASK 0x00FFFFFFu
#define BRIDGE_HANDLE_MAX  16384

static HANDLE bridge_read_handle(uint32_t va); /* defined below */

static void bridge_NtSetEvent(void)
{
    HANDLE   handle   = bridge_read_handle(STACK_ARG(0));
    uint32_t prev_va  = STACK_ARG(1);
    LONG     previous = 0;

    g_eax = (uint32_t)xbox_NtSetEvent(handle, &previous);
    if (prev_va) BRIDGE_MEM32(prev_va) = (uint32_t)previous;
}

static void bridge_NtClearEvent(void)
{
    HANDLE handle = bridge_read_handle(STACK_ARG(0));
    g_eax = (uint32_t)xbox_NtClearEvent(handle);
}

/* ── NtWaitForSingleObjectEx (ordinal 234) ─────────────────
 * First reached inside Application_RunMainLoop's real body -- a per-frame wait, not the one-time startup gate that made
 * NtWaitForMultipleObjectsEx (235, below) unsafe to bridge. Implemented the
 * same way as that one; watch for a hang on first use since it's still
 * possible this waits on an event this layer never signals. */
/* XBOX_WAIT_LOG=1 (diagnostic): report any single-object wait
 * that blocks longer than 30 ms -- which object, from where, for how long.
 * XBOX_WAIT_LOG=N (N > 1) sets the threshold to N ms. Each line carries the
 * performance-counter time in ms, to line up with [FPS] stall lines. */
static double s_wait_log_ms = 30.0;
static int wait_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XBOX_WAIT_LOG");
        on = e && e[0] >= '1' && e[0] <= '9';
        if (on && atoi(e) > 1) s_wait_log_ms = atoi(e);
    }
    return on;
}
static double wait_log_now(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER q;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart * 1000.0 / (double)f.QuadPart;
}
static uint32_t bridge_guess_caller(void);
static void wait_log(const char *what, uint32_t obj, double t0, uint32_t caller)
{
    double ms = wait_log_now() - t0;
    if (ms > s_wait_log_ms)
        fprintf(stderr, "[WAIT] t=%.1f %s obj=0x%08X %.1f ms caller=0x%08X thread %lu\n",
                t0, what, obj, ms, caller, GetCurrentThreadId());
}

static void bridge_NtWaitForSingleObjectEx(void)
{
    HANDLE   handle     = bridge_read_handle(STACK_ARG(0));
    uint32_t wait_mode  = STACK_ARG(1);
    uint32_t alertable  = STACK_ARG(2);
    uint32_t timeout_va = STACK_ARG(3);
    LARGE_INTEGER  to;
    PLARGE_INTEGER pto = NULL;

    if (timeout_va) {
        to.LowPart  = BRIDGE_MEM32(timeout_va);
        to.HighPart = (LONG)BRIDGE_MEM32(timeout_va + 4);
        pto = &to;
    }

    {
        int log = wait_log_on();
        double t0 = log ? wait_log_now() : 0;
        uint32_t caller = log ? bridge_guess_caller() : 0;
        g_eax = (uint32_t)xbox_NtWaitForSingleObjectEx(
            handle, (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable, pto);
        if (log) wait_log("NtWaitForSingleObjectEx", STACK_ARG(0), t0, caller);
    }
}

/* ── NtWaitForMultipleObjectsEx (ordinal 235) ───────────────
 * Was deliberately left unregistered -- see the (now-updated) note at its
 * dispatch case: the precondition that made it unsafe (the FILESYS queue
 * being driven synchronously, with nothing to ever signal completion
 * events) no longer holds once sub_0014D850 (the real async-I/O
 * worker-thread completion loop) is correctly translated. Re-enabled to
 * test; revert (add __attribute__((unused)) back and remove the case 235
 * line below) if this deadlocks startup. */
/* Scan the guest stack for the first value that looks like a code address in
 * .text. kernel_thunk_dispatch has already popped its own dummy frame, so the
 * caller's return address is not at a fixed offset -- but it is the only
 * .text-looking dword near the top of the stack. */
static uint32_t bridge_guess_caller(void)
{
    int k;
    for (k = -2; k < 24; k++) {
        uint32_t v = (uint32_t)BRIDGE_MEM32(g_esp + k * 4);
        if (v >= 0x00011000u && v < 0x001A0000u) return v;
    }
    return 0;
}

static void bridge_NtWaitForMultipleObjectsEx(void)
{
    uint32_t count       = STACK_ARG(0);
    uint32_t handles_va  = STACK_ARG(1);
    uint32_t wait_type   = STACK_ARG(2);
    /* STACK_ARG(3) = WaitMode (KPROCESSOR_MODE) -- unused here, matches
     * KeWaitForSingleObject/KeWaitForMultipleObjects which also ignore it. */
    uint32_t alertable   = STACK_ARG(4);
    uint32_t timeout_va  = STACK_ARG(5);
    HANDLE   handles[MAXIMUM_WAIT_OBJECTS];
    LARGE_INTEGER  to;
    PLARGE_INTEGER pto = NULL;
    uint32_t i;

    if (count > MAXIMUM_WAIT_OBJECTS) count = MAXIMUM_WAIT_OBJECTS;
    for (i = 0; i < count; i++) {
        /* Read the entry *out of* the array. This previously passed the
         * array element's address to bridge_read_handle, which takes a
         * handle value, not an address -- so the array was never read and
         * the array's own Xbox VA was handed to WaitForMultipleObjects as
         * if it were a handle. That failed instantly with
         * STATUS_UNSUCCESSFUL (0xC0000001) every time, and the title's
         * file-I/O thread spun retrying it forever: ~8,300 waits and the
         * matching RtlNtStatusToDosError conversions in a 25-second run.
         * Compare bridge_NtWaitForSingleObjectEx, which correctly passes
         * STACK_ARG(0) (a value) rather than an address.
         *
         * A tagged token maps through the handle table; anything else is an
         * Xbox-VA dispatcher object, resolved the same lazily-synthesised
         * way bridge_KeWaitForMultipleObjects resolves its objects, so a
         * wait and a later KeSetEvent on that VA share one Win32 event. */
        uint32_t v = BRIDGE_MEM32(handles_va + i * 4);
        handles[i] = ((v & 0xFF000000u) == BRIDGE_HANDLE_TAG)
                   ? bridge_read_handle(v)
                   : xbox_resolve_dispatcher_handle(v);
    }

    if (timeout_va) {
        to.LowPart  = BRIDGE_MEM32(timeout_va);
        to.HighPart = (LONG)BRIDGE_MEM32(timeout_va + 4);
        pto = &to;
    }

    g_eax = (uint32_t)xbox_NtWaitForMultipleObjectsEx(
        count, handles, wait_type, (BOOLEAN)alertable, pto);
    {
        /* 319,000 of these in a 12-second run, each followed by an
         * RtlNtStatusToDosError -- the title is spinning on a wait that never
         * gives it what it wants. Report what is being waited on and what comes
         * back, rather than guessing which. */
        static unsigned n = 0;
        static int want = -1;
        if (want < 0) { const char *e = getenv("XBOX_WAIT_TRACE");
                        want = (e && e[0] == '1') ? 1 : 0; }
        if (want && (n < 6 || (n % 100000) == 0)) {
            fprintf(stderr, "  [KERNEL] NtWaitForMultipleObjectsEx #%u: count=%u "
                            "type=%u alertable=%u timeout=%s(%d ms) -> 0x%08X  [0]=raw 0x%08X handle %p array@0x%08X\n",
                    n, count, wait_type, alertable,
                    pto ? "yes" : "INFINITE",
                    pto ? (int)(-(to.QuadPart) / 10000) : -1, g_eax,
                    count ? BRIDGE_MEM32(handles_va) : 0,
                    count ? (void *)handles[0] : NULL,
                    handles_va);
            if (g_eax != 0) {
                /* CaptureStackBackTrace is safe where __builtin_return_address(2)
                 * faulted. Each guest function is a C function, so the host chain
                 * mirrors the guest one and sym.py maps it back. */
                void *bt[24];
                USHORT nf = CaptureStackBackTrace(0, 24, bt, NULL);
                USHORT bi;
                uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
                fprintf(stderr, "  [KERNEL]   backtrace (link addrs):");
                for (bi = 0; bi < nf; bi++)
                    fprintf(stderr, " 0x%llX",
                            (unsigned long long)((uintptr_t)bt[bi] - base + 0x140000000ull));
                fprintf(stderr, "\n");
                fflush(stderr);
            }
            fflush(stderr);
        }
        n++;
    }
}

/* ── KeSetEvent (ordinal 145) ────────────────────────────── */
static void bridge_KeSetEvent(void)
{
    uint32_t event_ptr = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);

    g_eax = (uint32_t)xbox_KeSetEvent(xbox_resolve_dispatcher_handle(event_ptr), increment, (BOOLEAN)wait);
}

/* ── KeWaitForSingleObject (ordinal 159) ─────────────────── */
static void bridge_KeWaitForSingleObject(void)
{
    uint32_t object = STACK_ARG(0);
    uint32_t wait_reason = STACK_ARG(1);
    uint32_t wait_mode = STACK_ARG(2);
    uint32_t alertable = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);

    {
        int log = wait_log_on();
        double t0 = log ? wait_log_now() : 0;
        uint32_t caller = log ? bridge_guess_caller() : 0;
        g_eax = (uint32_t)xbox_KeWaitForSingleObject(
            xbox_resolve_dispatcher_handle(object), wait_reason, wait_mode,
            (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
        if (log) wait_log("KeWaitForSingleObject", object, t0, caller);
    }
}

/* ── KeWaitForMultipleObjects (ordinal 158) ──────────────────
 * NTSTATUS KeWaitForMultipleObjects(ULONG Count, PVOID Object[], ULONG WaitType,
 *     ULONG WaitReason, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
 *     PLARGE_INTEGER Timeout, PKWAIT_BLOCK WaitBlockArray)
 *
 * Object[] is an array of Xbox-VA dispatcher-object pointers living in Xbox
 * memory; xbox_KeWaitForMultipleObjects (kernel_sync.c) wants an array of real
 * native HANDLEs, so translate each entry through XBOX_TO_NATIVE before calling.
 */
static void bridge_KeWaitForMultipleObjects(void)
{
    /* A thread that never comes back from here is waiting on an object nothing
     * signals. Naming the objects and the timeout turns "blocked" into
     * something actionable. XBOX_WAIT_TRACE=1. */
    {
        static int enabled = -1;
        static unsigned long n = 0;
        if (enabled < 0) {
            const char *e = getenv("XBOX_WAIT_TRACE");
            enabled = (e && e[0] == '1') ? 1 : 0;
        }
        if (enabled && ((++n & 0x3F) == 1)) {
            uint32_t cnt = STACK_ARG(0), objs = STACK_ARG(1);
            uint32_t tmo = STACK_ARG(6);
            fprintf(stderr, "  [WAIT] #%lu count=%u objs=0x%08X first=0x%08X "
                    "timeout=%s\n", n, cnt, objs,
                    objs ? (unsigned)BRIDGE_MEM32(objs) : 0u,
                    tmo ? "finite" : "INFINITE");
            fflush(stderr);
        }
    }
    uint32_t count = STACK_ARG(0);
    uint32_t objects_ptr = STACK_ARG(1);
    uint32_t wait_type = STACK_ARG(2);
    uint32_t wait_reason = STACK_ARG(3);
    uint32_t wait_mode = STACK_ARG(4);
    uint32_t alertable = STACK_ARG(5);
    uint32_t timeout_ptr = STACK_ARG(6);
    uint32_t wait_block_ptr = STACK_ARG(7);
    PVOID native_objects[MAXIMUM_WAIT_OBJECTS];
    uint32_t i;

    if (count > MAXIMUM_WAIT_OBJECTS) count = MAXIMUM_WAIT_OBJECTS;

    for (i = 0; i < count; i++) {
        uint32_t obj_va = BRIDGE_MEM32(objects_ptr + i * 4);
        native_objects[i] = xbox_resolve_dispatcher_handle(obj_va);
    }

    g_eax = (uint32_t)xbox_KeWaitForMultipleObjects(
        count, native_objects, wait_type, wait_reason, wait_mode,
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr), XBOX_TO_NATIVE(wait_block_ptr));

}

/* ── NtYieldExecution (ordinal 238) ──────────────────────── */
static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

/* ── MmGetPhysicalAddress (ordinal 173) ──────────────────── */
static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Xbox uses identity mapping (physical == virtual) for the lower 64MB,
     * and contiguous memory is handed out as its 0x80000000 alias, so fold
     * the alias. Don't call xbox_MmGetPhysicalAddress, which would return a
     * native pointer. */
    g_eax = xbox_fold_ram_alias(addr);
}

/* ── MmSetAddressProtect (ordinal 182) ───────────────────── */
static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

/* ── AvSetDisplayMode (ordinal 3) ────────────────────────── */
static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    g_eax = 0;
}

/* ── PsTerminateSystemThread (ordinal 258) ───────────────
 * VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
 *
 * On real Xbox, this terminates the calling thread (never returns).
 * Originally this bridge just returned, on the assumption the caller would
 * "handle it gracefully" (verified only for Burnout 3's specific call
 * site). For SSX Tricky, the real caller does NOT tolerate this call
 * returning -- code continues past a call that was never expected to come
 * back, walks off the end of valid control flow, and the CPU ends up
 * executing garbage (observed as a wild jump to an address entirely
 * outside the mapped Xbox image, not even a simple fall-through into the
 * next function). Since threads already run as nested synchronous C calls
 * here (see bridge_PsCreateSystemThreadEx / run_thread_start_routine),
 * terminating a thread should unwind the C call stack back to that
 * synchronous call site -- exactly what longjmp does.
 */
static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);

    fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X\n", exit_status);
    fflush(stderr);

    g_eax = exit_status;

    if (g_thread_terminate_depth > 0) {
        longjmp(g_thread_terminate_jmp[g_thread_terminate_depth - 1], 1);
        /* unreachable */
    }
    /* No active thread-start call frame to unwind to (e.g. called outside
     * a PsCreateSystemThreadEx-spawned routine) -- nothing safe to do but
     * return, same as the original behavior. */
}

/* ── HalRequestSoftwareInterrupt (ordinal 48) ────────────
 * VOID HalRequestSoftwareInterrupt(KIRQL Irql)
 *
 * On real Xbox, queues a software interrupt (APC/DPC-style deferred
 * callback) at the given IRQL for the hardware interrupt controller to
 * fire later. There's no real interrupt controller here -- everything
 * runs synchronously on one native thread with no preemption -- so
 * there's nothing to actually queue; a no-op is the correct emulation
 * of "the interrupt was requested" in a model with no asynchronous
 * execution to defer it to. Found missing (silent zero-return via the
 * generic unbridged-ordinal fallback) while tracing SSX Tricky's real
 * boot sequence --
 */
static void bridge_HalRequestSoftwareInterrupt(void)
{
    /* arg0: Irql - ignored, nothing to defer to in this synchronous model */
    g_eax = 0;
}

/* ── HalReadSMCTrayState (ordinal 9) ───────────────────────
 * VOID HalReadSMCTrayState(PDWORD TrayState, PDWORD TrayStateChangeCount)
 *
 * Returns DVD tray state. 0x10 = no disc, 0x14 = tray closed with disc.
 */
static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x10;  /* No disc */
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

/* ── HalRegisterShutdownNotification (ordinal 47) ─────────
 * VOID HalRegisterShutdownNotification(PVOID ShutdownRegistration, BOOLEAN Register)
 *
 * Registers/unregisters a callback the HAL invokes on shutdown/reset.
 * We never perform a real Xbox-style shutdown, so there's nothing to
 * ever fire the callback -- a no-op correctly emulates "registered,
 * will never be needed." Found via the same unbridged-ordinal audit as
 * bridge_ExQueryNonVolatileSetting: VOID with no output params, so unlike that one
 * this was already harmless as a bare stub -- only the missing 8-byte
 * stdcall cleanup (fixed alongside this) was an actual bug.
 */
static void bridge_HalRegisterShutdownNotification(void)
{
    g_eax = 0;
}

/* ── HalReturnToFirmware (ordinal 49) ─────────────────────
 * VOID HalReturnToFirmware(ULONG Routine)
 *
 * Reboots to a firmware routine (reset, shutdown, quick-reboot, etc).
 * A no-op keeps the process running instead of trying to emulate an
 * actual Xbox reboot; only the missing 4-byte stdcall cleanup was a
 * real bug (same audit as above).
 *
 * The host may still act on it: g_hal_return_hook (set by the host, NULL by
 * default) sees each call's Routine first. The title also gets here on its
 * own (XAPI's start-up checks launch the dashboard and expect never to come
 * back), so the hook must tell the player's request from those.
 */
void (*g_hal_return_hook)(uint32_t routine) = NULL;

static void bridge_HalReturnToFirmware(void)
{
    if (g_hal_return_hook) g_hal_return_hook(STACK_ARG(0));
    g_eax = 0;
}

/* ── ExFreePool (ordinal 17) ───────────────────────────────
 * VOID ExFreePool(PVOID P)
 *
 * xbox_HeapAlloc (xbox_memory_layout.c) is a bump allocator with no free
 * support by design, so there's nothing for this to actually do; a
 * no-op is consistent with the rest of this runtime's memory model.
 * Only the missing 4-byte stdcall cleanup was a real bug (same audit).
 */
static void bridge_ExFreePool(void)
{
    g_eax = 0;
}

/* ── KeInitializeDpc (ordinal 107) ────────────────────────
 * VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
 *                       PVOID DeferredContext)
 *
 * Initializes a DPC object. The Xbox KDPC structure is 32 bytes.
 * We zero it and set the routine and context pointers.
 */
static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    /* Zero the structure (32 bytes) */
    memset(XBOX_TO_NATIVE(dpc_va), 0, 32);

    /* Set Type (0x13 = DpcObject) and fields */
    BRIDGE_MEM16(dpc_va + 0) = 0x13;   /* Type */
    BRIDGE_MEM32(dpc_va + 12) = routine; /* DeferredRoutine */
    BRIDGE_MEM32(dpc_va + 16) = context; /* DeferredContext */
    g_eax = 0;
}

/* ── KeInsertQueueDpc (ordinal 119) ───────────────────────
 * BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1,
 *                           PVOID SystemArgument2)
 *
 * Real Xbox queues the DPC to run at DISPATCH_LEVEL, asynchronously
 * w.r.t. whatever raised it. There's no real IRQL/async queue here, and
 * critically the DeferredRoutine is itself recompiled register-based code
 * that shares g_eax/g_ebx/g_esi/g_edi/g_esp with every other Xbox
 * "thread" -- running it from a real second OS thread would race on all
 * of those. Instead, run it synchronously, right here, on the calling
 * thread (same pattern as bridge_PsCreateSystemThreadEx's worker-thread
 * path: save every global register, push the 4 DPC args, call through
 * recomp_lookup, restore every register). Same thread, so no race; the
 * only difference from real hardware is timing (immediate vs deferred),
 * which doesn't matter for a DPC whose job is just to get done.
 */
static void bridge_KeInsertQueueDpc(void)
{
    uint32_t dpc_va    = STACK_ARG(0);
    uint32_t sysarg1   = STACK_ARG(1);
    uint32_t sysarg2   = STACK_ARG(2);
    uint32_t routine_va = dpc_va ? BRIDGE_MEM32(dpc_va + 12) : 0;
    uint32_t context_va = dpc_va ? BRIDGE_MEM32(dpc_va + 16) : 0;
    recomp_func_t fn;

    if (!routine_va) {
        g_eax = 0;  /* FALSE: nothing to queue */
        return;
    }

    BRIDGE_MEM32(dpc_va + 20) = sysarg1;  /* SystemArgument1 */
    BRIDGE_MEM32(dpc_va + 24) = sysarg2;  /* SystemArgument2 */

    fn = recomp_lookup(routine_va);
    if (!fn) fn = recomp_lookup_manual(routine_va);
    if (fn) {
        uint32_t save_eax = g_eax, save_ecx = g_ecx, save_edx = g_edx;
        uint32_t save_ebx = g_ebx, save_esi = g_esi, save_edi = g_edi;
        uint32_t save_esp = g_esp, save_ebp = g_seh_ebp;

        g_esp -= 4; BRIDGE_MEM32(g_esp) = sysarg2;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = sysarg1;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = context_va;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = dpc_va;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;  /* dummy return address */
        g_seh_ebp = g_esp;
        run_thread_start_routine(fn);

        g_eax = save_eax; g_ecx = save_ecx; g_edx = save_edx;
        g_ebx = save_ebx; g_esi = save_esi; g_edi = save_edi;
        g_esp = save_esp; g_seh_ebp = save_ebp;
    } else {
        fprintf(stderr, "  [KERNEL] KeInsertQueueDpc: routine 0x%08X not found in dispatch!\n",
                routine_va);
    }

    g_eax = 1;  /* TRUE: was queued (and has now already run) */
}

/* ── HalGetInterruptVector (ordinal 44) ───────────────────
 * ULONG HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql)
 *
 * Maps a bus interrupt level to a vector + IRQL. We don't dispatch by
 * vector number in this model (KeConnectInterrupt below keys off the
 * KINTERRUPT object directly), so any nonzero vector works; a zero
 * vector is what was silently returned before this was bridged, and
 * some driver init paths check it against zero as a "did HAL accept my
 * interrupt line" test.
 */
/* The bus level asked for last on this thread; the KeInitializeInterrupt
 * that follows (same driver init, same thread) is recorded under it. */
static __thread int t_hal_bus_level = -1;
static struct { uint32_t kint, routine, context; } g_isr_by_level[32];

static void bridge_HalGetInterruptVector(void)
{
    uint32_t irql_va = STACK_ARG(1);
    t_hal_bus_level = (int)STACK_ARG(0);
    if (irql_va >= XBOX_BASE_ADDRESS) BRIDGE_MEM32(irql_va) = 27; /* a plausible device IRQL */
    g_eax = 0x1E; /* arbitrary nonzero vector */
}

/* ── KeInitializeInterrupt / KeConnectInterrupt (ordinals 109, 98) ──
 * VOID KeInitializeInterrupt(PKINTERRUPT Interrupt, PVOID ServiceRoutine,
 *     PVOID ServiceContext, ULONG Vector, KIRQL Irql, ULONG InterruptMode,
 *     BOOLEAN ShareVector)
 * BOOLEAN KeConnectInterrupt(PKINTERRUPT Interrupt)
 *
 * The real Xbox KINTERRUPT layout isn't load-bearing here (game code only
 * ever passes this struct opaquely between these two calls, never reads
 * its fields itself), so this bridge owns its own layout:
 *   +0  ServiceRoutine (Xbox VA)
 *   +4  ServiceContext (Xbox VA)
 *   +8  BusInterruptLevel/Vector
 *   +12 Connected (0/1)
 * KeConnectInterrupt returning FALSE (the old unbridged-ordinal default,
 * since there was no bridge at all) is what left driver init spinning/
 * retrying forever waiting for a connect that could never succeed --
 * found by tracing the hang. There's no real interrupt controller to actually deliver a
 * GPU interrupt from here (see bridge_HalRequestSoftwareInterrupt), so
 * this only fixes "the driver believes its interrupt is live"; it does
 * not yet simulate periodic VBLANK-style delivery to the stored
 * ServiceRoutine.
 */
static void bridge_KeInitializeInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t service_routine = STACK_ARG(1);
    uint32_t service_context = STACK_ARG(2);
    uint32_t vector = STACK_ARG(3);

    if (interrupt_va) {
        BRIDGE_MEM32(interrupt_va + 0)  = service_routine;
        BRIDGE_MEM32(interrupt_va + 4)  = service_context;
        BRIDGE_MEM32(interrupt_va + 8)  = vector;
        BRIDGE_MEM32(interrupt_va + 12) = 0;  /* not connected yet */
    }
    /* Name the title's interrupt service routines -- none is called
     * by any hardware here, which is what the EA-logo boot hang comes down
     * to -- and keep them by bus level for xbox_kernel_call_isr(). */
    fprintf(stderr, "  [KERNEL] KeInitializeInterrupt: kinterrupt 0x%08X routine 0x%08X "
            "context 0x%08X vector %u bus level %d\n", interrupt_va, service_routine,
            service_context, vector, t_hal_bus_level);
    if (t_hal_bus_level >= 0 && t_hal_bus_level < 32) {
        g_isr_by_level[t_hal_bus_level].kint = interrupt_va;
        g_isr_by_level[t_hal_bus_level].routine = service_routine;
        g_isr_by_level[t_hal_bus_level].context = service_context;
    }
    t_hal_bus_level = -1;
}

/* Run the title's interrupt service routine for a bus level, now, on
 * the calling Xbox thread -- the way bridge_KeInsertQueueDpc runs a DPC:
 * every guest register saved, BOOLEAN ISR(PKINTERRUPT, PVOID context)
 * stdcall-called through the dispatch, registers restored. There are no
 * hardware interrupts in this runtime; a caller that knows a device has one
 * pending (XBOX_FIX_BOOTHANG, port/src) calls this between two kernel calls
 * of a guest thread, never from a host thread and never re-entrantly.
 * Returns 1 when the routine ran, 0 when none is known for that level. */
static __thread int t_in_isr;
int xbox_kernel_call_isr(int level)
{
    recomp_func_t fn;
    uint32_t va;
    if (level < 0 || level >= 32 || t_in_isr) return 0;
    va = g_isr_by_level[level].routine;
    if (!va || !BRIDGE_MEM32(g_isr_by_level[level].kint + 12)) return 0;   /* not connected */
    fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_manual(va);
    if (!fn) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "  [KERNEL] interrupt routine 0x%08X (bus level %d) not in dispatch\n",
                    va, level);
        return 0;
    }
    {
        uint32_t save_eax = g_eax, save_ecx = g_ecx, save_edx = g_edx;
        uint32_t save_ebx = g_ebx, save_esi = g_esi, save_edi = g_edi;
        uint32_t save_esp = g_esp, save_ebp = g_seh_ebp;

        g_esp -= 4; BRIDGE_MEM32(g_esp) = g_isr_by_level[level].context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = g_isr_by_level[level].kint;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;  /* dummy return address */
        g_seh_ebp = g_esp;
        t_in_isr = 1;
        run_thread_start_routine(fn);
        t_in_isr = 0;

        g_eax = save_eax; g_ecx = save_ecx; g_edx = save_edx;
        g_ebx = save_ebx; g_esi = save_esi; g_edi = save_edi;
        g_esp = save_esp; g_seh_ebp = save_ebp;
    }
    return 1;
}

/* Called at the end of every kernel call, on the calling Xbox thread,
 * when set (port/src sets it under XBOX_FIX_BOOTHANG). NULL = nothing runs. */
void (*xbox_kernel_post_call_hook)(void) = NULL;

static void bridge_KeConnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    if (interrupt_va) {
        BRIDGE_MEM32(interrupt_va + 12) = 1;  /* Connected */
    }
    g_eax = 1;  /* TRUE */
}

/* ── Xbox-VA -> native XBOX_KTIMER map (ordinals 113/149/150) ──
 * The Xbox-side "PKTIMER Timer" argument is just an opaque VA the game
 * embeds a 40-byte struct at -- it is never large enough to hold a real
 * host-native XBOX_KTIMER (whose HANDLE/pointer fields are 8 bytes each on
 * x64), so a real timer needs a separately-allocated native struct, looked
 * up by that VA, exactly like the file-handle token table above.
 *
 * The one thing that must stay reconciled: xbox_KeInitializeTimerEx (real
 * implementation, kernel_sync.c) creates its own Win32 event for
 * Timer->win32_event, but anything that later *waits* on this same VA
 * (KeWaitForSingleObject/KeWaitForMultipleObjects, both below) resolves it
 * through the completely separate xbox_resolve_dispatcher_handle() VA cache.
 * Without reconciling the two, the timer would signal one native event
 * while every waiter blocks on a different one, forever. Fixed by replacing
 * the event xbox_KeInitializeTimerEx creates with the canonical one
 * xbox_resolve_dispatcher_handle() returns for the same VA, immediately
 * after initializing. */
/*
 * Capacity, and what may be recycled.
 *
 * This was 64 slots recycled round-robin with no regard for whether the
 * victim was armed. XAPI's multimedia-timer thread initialises 63 KTIMERs at
 * startup, so the map was full almost immediately, and every later
 * KeInitializeTimerEx -- DirectSound initialises one at a fresh heap address
 * (its object + 0x618) each time it is created -- evicted a live XAPI timer,
 * cancelling it. One of those is the periodic timer behind timeSetEvent that
 * drives Application_FrameTimerCallback. With DirectSound enabled the
 * evictions reached it after a few dozen frames: the frame callback stopped,
 * the main thread waited forever on the frame event, and drawing went to zero.
 * The default build only survived because it evicted fewer slots and missed
 * that one.
 *
 * Now: 1024 slots, and recycling picks an UNARMED timer. An armed timer is
 * only ever evicted if every slot is armed, which is reported, because it
 * silently stops something that is still counting.
 *
 * The map is shared by every guest thread and had no lock; it now has one,
 * held for the whole of each bridge operation so a lookup cannot hand back a
 * struct that a concurrent eviction is freeing.
 */
#define XBOX_KTIMER_MAP_SIZE 1024
static uint32_t     g_ktimer_keys[XBOX_KTIMER_MAP_SIZE];
static PXBOX_KTIMER  g_ktimer_values[XBOX_KTIMER_MAP_SIZE];
static int           g_ktimer_count = 0;
static unsigned      g_ktimer_evict = 0;  /* recycling cursor once the map is full */
static SRWLOCK       g_ktimer_lock = SRWLOCK_INIT;

/* Caller holds g_ktimer_lock. */
static PXBOX_KTIMER bridge_ktimer_for_va(uint32_t timer_va)
{
    int i;
    for (i = 0; i < g_ktimer_count; i++)
        if (g_ktimer_keys[i] == timer_va) return g_ktimer_values[i];
    return NULL;
}

/* Caller holds g_ktimer_lock and the map is full. Prefer a slot whose timer is
 * not armed; fall back to the cursor only if every one is. */
static int bridge_ktimer_pick_victim(void)
{
    int n;
    for (n = 0; n < XBOX_KTIMER_MAP_SIZE; n++) {
        int slot = (int)((g_ktimer_evict + (unsigned)n) % XBOX_KTIMER_MAP_SIZE);
        if (!g_ktimer_values[slot] || !g_ktimer_values[slot]->Inserted) {
            g_ktimer_evict = (unsigned)slot + 1u;
            return slot;
        }
    }
    {
        static LONG warned = 0;
        if (InterlockedExchange(&warned, 1) == 0) {
            fprintf(stderr, "  [KTIMER] all %d timer slots are armed -- evicting a "
                    "live timer; something that is still counting will stop\n",
                    XBOX_KTIMER_MAP_SIZE);
            fflush(stderr);
        }
    }
    return (int)(g_ktimer_evict++ % XBOX_KTIMER_MAP_SIZE);
}

/* ── KeInitializeTimerEx (ordinal 113) ────────────────────
 * VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
 */
static void bridge_KeInitializeTimerEx(void)
{
    uint32_t timer_va = STACK_ARG(0);
    uint32_t type = STACK_ARG(1);
    PXBOX_KTIMER t;

    AcquireSRWLockExclusive(&g_ktimer_lock);
    t = bridge_ktimer_for_va(timer_va);

    if (t) {
        /* Re-initialising a known VA: drop any host timer it still owns, or
         * the memset in xbox_KeInitializeTimerEx loses the handle and the old
         * queue timer keeps firing against this struct forever. */
        xbox_KeReleaseTimerResources(t);
    } else {
        int slot;

        /* The title initialises KTIMERs inside stack frames (confirmed live:
         * VAs 0x04B3C6E4, 0x04B3C728, ... all sit in the guest stack), so a
         * map that only ever grows saturates at XBOX_KTIMER_MAP_SIZE within
         * one boot. Past that the old code still calloc'd an object but never
         * registered it, so it leaked AND every later lookup returned NULL,
         * silently dropping the timer. Recycle round-robin instead, tearing
         * the previous host timer down first -- a live timer-queue timer left
         * behind keeps firing xbox_timer_callback against an object nobody
         * owns any more.
         *
         * win32_event is deliberately NOT closed here: by this point it is
         * the canonical dispatcher handle for that VA (see below), shared
         * with KeWaitFor* callers, so closing it would break them. */
        if (g_ktimer_count < XBOX_KTIMER_MAP_SIZE) {
            slot = g_ktimer_count++;
        } else {
            slot = bridge_ktimer_pick_victim();
            if (g_ktimer_values[slot]) {
                /* Waits out a callback still running on it before the free. */
                xbox_KeReleaseTimerResources(g_ktimer_values[slot]);
                free(g_ktimer_values[slot]);
                g_ktimer_values[slot] = NULL;
            }
        }

        t = (PXBOX_KTIMER)calloc(1, sizeof(XBOX_KTIMER));
        g_ktimer_keys[slot] = timer_va;
        g_ktimer_values[slot] = t;
    }

    xbox_KeInitializeTimerEx(t, (XBOX_TIMER_TYPE)type);

    /* Adopt the canonical dispatcher-handle event for this VA instead of the
     * one xbox_KeInitializeTimerEx just created, so a later KeWaitFor* on
     * this same VA sees the exact handle this timer will signal. */
    if (t->win32_event) CloseHandle(t->win32_event);
    t->win32_event = xbox_resolve_dispatcher_handle(timer_va);

    ReleaseSRWLockExclusive(&g_ktimer_lock);
    g_eax = 0;
}

/* ── KeSetTimer / KeSetTimerEx (ordinal 149/150) ──────────
 * BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
 * BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc)
 *
 * Was a complete no-op stub ("timer functionality is not needed for basic
 * execution") -- confirmed live to be the actual
 * reason the game's own software timer queue (sub_00152230 et al.) never
 * fires: it calls this to arm each timer's real due-time/period, and with
 * it doing nothing, the timer's Win32 event this is supposed to schedule
 * is never signaled, so anything waiting on it blocks forever.
 *
 * The two ordinals share this handler and differ only in argument shape
 * (KeSetTimerEx inserts a Period dword before Dpc); g_slot_ordinals[] is
 * checked to tell them apart since STACK_ARG alone can't.
 *
 * Dpc is deliberately never forwarded to the real xbox_KeSetTimerEx: it is
 * a raw Xbox VA, not a native pointer, and xbox_timer_callback would call
 * Dpc->DeferredRoutine directly as a host function pointer on expiry --
 * fine when Dpc is NULL (confirmed the actual case for this session's
 * target caller), but unsafe in general without translating the DPC
 * through RECOMP_ICALL. Passing NULL always still lets the timer signal
 * its event correctly; it just means a title relying on the DPC path
 * specifically (rather than waiting on the timer object) wouldn't see its
 * DPC fire. No such caller has been found yet.
 */
static void bridge_KeSetTimer(void)
{
    uint32_t timer_va = STACK_ARG(0);
    uint32_t due_lo    = STACK_ARG(1);
    uint32_t due_hi    = STACK_ARG(2);
    ULONG ordinal = (g_kernel_dispatch_slot >= 0) ? g_slot_ordinals[g_kernel_dispatch_slot] : 149;
    LONG period = 0;
    PXBOX_KTIMER t;
    LARGE_INTEGER due;

    if (ordinal == 150) period = (LONG)STACK_ARG(3);

    AcquireSRWLockExclusive(&g_ktimer_lock);
    t = bridge_ktimer_for_va(timer_va);
    if (!t) {
        /* Not initialized via KeInitializeTimerEx first -- shouldn't happen
         * for a faithful translation, but don't crash on it. */
        ReleaseSRWLockExclusive(&g_ktimer_lock);
        g_eax = 0;
        return;
    }

    due.LowPart = due_lo;
    due.HighPart = (LONG)due_hi;

    g_eax = (uint32_t)xbox_KeSetTimerEx(t, due, period, NULL);
    ReleaseSRWLockExclusive(&g_ktimer_lock);
}

/* ── ExQueryPoolBlockSize (ordinal 24) ────────────────────
 * ULONG ExQueryPoolBlockSize(PVOID PoolBlock)
 *
 * Returns the size of a pool memory block.
 * Since we use HeapAlloc, we can query the Windows heap.
 */
static void bridge_ExQueryPoolBlockSize(void)
{
    /* The allocation ledger knows every block xbox_HeapAlloc handed out, so
     * answer from it. This used to return 0 unconditionally, on the theory
     * that callers only use it for statistics; DirectSound calls it during
     * device creation. */
    uint32_t base = 0, size = 0;
    uint32_t block = STACK_ARG(0);
    g_eax = (block && xbox_heap_owner_of(block, &base, &size, NULL, NULL, 0, NULL))
          ? size : 0u;
}

/* ── RtlNtStatusToDosError (ordinal 301) ─────────────────
 * ULONG RtlNtStatusToDosError(NTSTATUS Status)
 *
 * Converts an NTSTATUS to a Win32 error code.
 */
static void bridge_RtlNtStatusToDosError(void)
{
    uint32_t status = STACK_ARG(0);

    /* Simple mapping of common status codes */
    switch (status) {
    case 0x00000000: g_eax = 0; break;          /* STATUS_SUCCESS → ERROR_SUCCESS */
    case 0xC0000034: g_eax = 2; break;          /* STATUS_OBJECT_NAME_NOT_FOUND → ERROR_FILE_NOT_FOUND */
    case 0xC000003A: g_eax = 3; break;          /* STATUS_OBJECT_PATH_NOT_FOUND → ERROR_PATH_NOT_FOUND */
    case 0xC0000022: g_eax = 5; break;          /* STATUS_ACCESS_DENIED → ERROR_ACCESS_DENIED */
    case 0xC0000008: g_eax = 6; break;          /* STATUS_INVALID_HANDLE → ERROR_INVALID_HANDLE */
    case 0xC0000017: g_eax = 8; break;          /* STATUS_NO_MEMORY → ERROR_NOT_ENOUGH_MEMORY */
    case 0xC000000D: g_eax = 87; break;         /* STATUS_INVALID_PARAMETER → ERROR_INVALID_PARAMETER */
    default:         g_eax = 317; break;         /* ERROR_MR_MID_NOT_FOUND (generic) */
    }
}

/* ── File I/O bridge helpers ─────────────────────────────── */

/*
 * Xbox structures use 32-bit pointers. On Win64, the C structs
 * (XBOX_OBJECT_ATTRIBUTES, etc.) have 64-bit pointers, so we can't
 * cast Xbox memory to them directly. Instead, parse the 32-bit
 * Xbox layout manually:
 *
 * XBOX_OBJECT_ATTRIBUTES (12 bytes):
 *   offset 0: RootDirectory  (uint32_t)
 *   offset 4: ObjectName     (uint32_t, Xbox VA to ANSI_STRING)
 *   offset 8: Attributes     (uint32_t)
 *
 * XBOX_ANSI_STRING (8 bytes):
 *   offset 0: Length          (uint16_t)
 *   offset 2: MaximumLength   (uint16_t)
 *   offset 4: Buffer          (uint32_t, Xbox VA to char[])
 *
 * XBOX_IO_STATUS_BLOCK (8 bytes):
 *   offset 0: Status          (uint32_t)
 *   offset 4: Information     (uint32_t)
 */

/* The Xbox path each open handle was opened with, so an OBJECT_ATTRIBUTES
 * whose RootDirectory is a handle can be resolved: XAPI opens a save folder
 * and then its files by bare name relative to it (XDeleteSaveGame deletes
 * "Data.ssx" that way), and those relative opens all failed with
 * STATUS_OBJECT_NAME_NOT_FOUND. Keyed by the guest's handle value. */
#define XBOX_HANDLE_PATHS 256
static struct { uint32_t tok; char path[260]; } s_hpath[XBOX_HANDLE_PATHS];
static CRITICAL_SECTION s_hpath_cs;
static volatile LONG s_hpath_init = 0;

static void hpath_lock(void)
{
    if (InterlockedCompareExchange(&s_hpath_init, 1, 0) == 0) {
        InitializeCriticalSection(&s_hpath_cs);
        s_hpath_init = 2;
    }
    while (s_hpath_init != 2) Sleep(0);
    EnterCriticalSection(&s_hpath_cs);
}

static void hpath_set(uint32_t tok, const char *path)
{
    int i, slot = -1;
    if (!tok || !path) return;
    hpath_lock();
    for (i = 0; i < XBOX_HANDLE_PATHS; i++) {
        if (s_hpath[i].tok == tok) { slot = i; break; }
        if (!s_hpath[i].tok && slot < 0) slot = i;
    }
    if (slot >= 0) {
        s_hpath[slot].tok = tok;
        strncpy(s_hpath[slot].path, path, sizeof s_hpath[slot].path - 1);
        s_hpath[slot].path[sizeof s_hpath[slot].path - 1] = 0;
    }
    LeaveCriticalSection(&s_hpath_cs);
}

static int hpath_get(uint32_t tok, char *out, size_t cap)
{
    int i, ok = 0;
    if (!tok) return 0;
    hpath_lock();
    for (i = 0; i < XBOX_HANDLE_PATHS; i++)
        if (s_hpath[i].tok == tok) {
            strncpy(out, s_hpath[i].path, cap - 1);
            out[cap - 1] = 0;
            ok = 1;
            break;
        }
    LeaveCriticalSection(&s_hpath_cs);
    return ok;
}

static void hpath_drop(uint32_t tok)
{
    int i;
    if (!tok) return;
    hpath_lock();
    for (i = 0; i < XBOX_HANDLE_PATHS; i++)
        if (s_hpath[i].tok == tok) s_hpath[i].tok = 0;
    LeaveCriticalSection(&s_hpath_cs);
}

/* Extract the ANSI path string from an Xbox OBJECT_ATTRIBUTES */
static const char* bridge_get_xbox_path(uint32_t obj_attrs_va)
{
    /* ANSI_STRING is a COUNTED string: Buffer carries exactly Length bytes and
     * is NOT required to be NUL-terminated. This used to hand the raw guest
     * pointer to strlen(), which ran off the end of the name into whatever
     * followed it -- SSX's 7-character "D:\data" arrived as
     * "D:\data @a\x01" and the open failed with STATUS_OBJECT_NAME_NOT_FOUND.
     *
     * Honour Length, and only scan for a terminator when a caller leaves it
     * zero but supplies a bounded buffer (RtlInitAnsiString fills Length in,
     * but a title is free to build the struct by hand). */
    static __thread char path_buf[520];
    uint32_t ansi_str_va, buf_va;
    unsigned len, cap, i;

    if (!obj_attrs_va) return NULL;
    ansi_str_va = BRIDGE_MEM32(obj_attrs_va + 4);
    if (!ansi_str_va) return NULL;
    buf_va = BRIDGE_MEM32(ansi_str_va + 4);
    if (!buf_va) return NULL;

    len = BRIDGE_MEM16(ansi_str_va + 0);
    if (len == 0) {
        cap = BRIDGE_MEM16(ansi_str_va + 2);
        if (cap == 0 || cap > sizeof(path_buf) - 1) cap = sizeof(path_buf) - 1;
        while (len < cap && BRIDGE_MEM8(buf_va + len) != 0) len++;
    }
    if (len > sizeof(path_buf) - 1) len = sizeof(path_buf) - 1;

    for (i = 0; i < len; i++) path_buf[i] = (char)BRIDGE_MEM8(buf_va + i);
    /* Trailing NULs inside Length are common when a name is built in a fixed
     * field; drop them so the path compares equal to the on-disc name. */
    while (len > 0 && path_buf[len - 1] == '\0') len--;
    path_buf[len] = '\0';
    {   /* Relative to RootDirectory: prefix the directory's own path. */
        uint32_t root = BRIDGE_MEM32(obj_attrs_va + 0);
        char base[260];
        if (root && path_buf[0] != '\\' && !strchr(path_buf, ':') &&
            hpath_get(root, base, sizeof base)) {
            size_t bl = strlen(base);
            char joined[520];
            snprintf(joined, sizeof joined, "%s%s%s", base,
                     (bl && base[bl - 1] == '\\') ? "" : "\\", path_buf);
            strncpy(path_buf, joined, sizeof(path_buf) - 1);
            path_buf[sizeof(path_buf) - 1] = '\0';
        }
    }
    return path_buf;
}

/* Write NTSTATUS + Information into Xbox IO_STATUS_BLOCK */
static void bridge_write_iostatus(uint32_t ios_va, NTSTATUS status, uint32_t info)
{
    if (ios_va) {
        BRIDGE_MEM32(ios_va + 0) = (uint32_t)status;
        BRIDGE_MEM32(ios_va + 4) = info;
    }
}

/*
 * Handle table.
 *
 * Xbox memory only has 32-bit handle slots, but native HANDLEs are 64-bit
 * pointers (win32_compat objects, or real Win32 handles on Windows). Map
 * 32-bit tokens <-> native HANDLEs so a handle survives a round-trip through
 * Xbox memory. Tokens carry a tag in the high byte so they never collide
 * with the synthetic handles (0xDEAD0001 / 0xBEEF0010) used elsewhere.
 */
static HANDLE s_handle_table[BRIDGE_HANDLE_MAX];

static uint32_t bridge_handle_token_unlocked(HANDLE h)
{
    int i;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == h) return BRIDGE_HANDLE_TAG | (uint32_t)i;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == NULL) {
            s_handle_table[i] = h;
            return BRIDGE_HANDLE_TAG | (uint32_t)i;
        }
    fprintf(stderr, "  [BRIDGE] handle table full\n");
    return 0;
}

/* Two threads storing a handle at once could both pick the same free slot
 * (one token then names the other thread's handle): under
 * XBOX_FIX_DISPATCHMAP the search and the store are done under the lock. */
static uint32_t bridge_handle_token(HANDLE h)
{
    uint32_t tok;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    if (!dispatch_fix_on()) return bridge_handle_token_unlocked(h);
    AcquireSRWLockExclusive(&g_dispatch_lock);
    tok = bridge_handle_token_unlocked(h);
    ReleaseSRWLockExclusive(&g_dispatch_lock);
    return tok;
}

/* Store a native HANDLE into a 32-bit Xbox memory slot (as a token). */
static void bridge_write_handle(uint32_t handle_va, HANDLE h)
{
    if (handle_va)
        BRIDGE_MEM32(handle_va) = bridge_handle_token(h);
}

/*
 * Resolve a by-value Xbox HANDLE argument (e.g. NtReadFile's FileHandle,
 * NtSetEvent's EventHandle) to a native HANDLE.
 *
 * FIXED, This used to
 * do `BRIDGE_MEM32(va)` before the tag check, treating an already-resolved
 * token as if it were an address to fetch a token *from* -- every by-value
 * handle call site actually passes the token itself via STACK_ARG(n), which
 * already reads the stack slot's value, so that was a second, spurious
 * dereference. Confirmed live: for a real token like 0x48000001, the old
 * code read unrelated/garbage Xbox memory (BRIDGE_HANDLE_TAG's 0x48000000
 * range is deliberately outside real Xbox memory) that essentially never
 * carried the 0x48 tag, so it always fell through to "pass through
 * unchanged" and resolved to native NULL -- every by-value handle bridge in
 * this file had silently been operating on a NULL/garbage HANDLE for this
 * whole project's history, unnoticed because nothing had gotten far enough
 * to actually need one of these calls to succeed.
 *
 * Fixing just this line previously turned "spins without crashing" into a
 * genuine hang: `PsCreateSystemThreadEx` stores *raw, untagged* native
 * thread HANDLEs for a separate resolution path
 * (`xbox_resolve_dispatcher_handle`), so once by-value resolution stopped
 * silently failing to NULL, `NtWaitForSingleObjectEx` correctly blocked on
 * the game's real frame-sync event -- and nothing was signaling it, because
 * `bridge_KeSetTimer` (right above) was itself a no-op stub the game's own
 * software timer queue depends on to actually arm that signal. With that
 * fixed too, this dereference fix is safe to keep.
 */
static HANDLE bridge_read_handle(uint32_t va)
{
    uint32_t token = va;
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged value: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

/* Resolve a token to a HANDLE and release its table slot (for NtClose). */
static HANDLE bridge_take_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX) {
            HANDLE h;
            int fix = dispatch_fix_on();
            if (fix) AcquireSRWLockExclusive(&g_dispatch_lock);
            h = s_handle_table[i];
            s_handle_table[i] = NULL;
            if (fix) ReleaseSRWLockExclusive(&g_dispatch_lock);
            return h;
        }
    }
    return NULL;   /* untagged -> not a table handle, do not close */
}

/* Build a native OBJECT_ATTRIBUTES wrapping the translated Xbox path. */
static void bridge_build_oa(uint32_t obj_attrs_va,
                            XBOX_OBJECT_ATTRIBUTES* oa, XBOX_ANSI_STRING* name)
{
    const char* path = bridge_get_xbox_path(obj_attrs_va);
    name->Buffer        = (PCHAR)path;
    name->Length        = path ? (USHORT)strlen(path) : 0;
    name->MaximumLength = (USHORT)(name->Length + 1);
    oa->RootDirectory = NULL;
    oa->ObjectName    = name;
    oa->Attributes    = 0;
}

/* Per-file I/O ledger, defined further down next to the read bridge. */
static void file_ledger_open(HANDLE h, const char *path);
static void file_ledger_read(HANDLE h, uint32_t n);

/* Open a file by delegating to the ported xbox_NtCreateFile kernel HLE. */
static NTSTATUS bridge_create_file_impl(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;
    XBOX_IO_STATUS_BLOCK   ios;
    HANDLE   h  = NULL;
    NTSTATUS st;

    bridge_build_oa(obj_attrs_va, &oa, &name);
    if (!name.Buffer) {
        fprintf(stderr, "  [FILE] create/open: path extraction FAILED (obj_attrs_va=0x%08X)\n", obj_attrs_va);
        fflush(stderr);
        bridge_write_iostatus(iostatus_va, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    fprintf(stderr, "  [FILE] create/open: path=\"%s\" disposition=0x%X share=0x%X\n",
            name.Buffer, disposition, share);
    {   /* A create (not an open) of a name ending in '\' is a file name that
         * came out empty (the save image copy). Report who built
         * it -- host frames resolve with addr2line, image base 0x140000000. */
        size_t nl = strlen(name.Buffer);
        if (nl && name.Buffer[nl - 1] == '\\' && disposition != 1 && !(options & 0x1)) {
            void *fr[16];
            unsigned short c = (unsigned short)CaptureStackBackTrace(0, 16, fr, NULL), i;
            uintptr_t mb = (uintptr_t)GetModuleHandleW(NULL);
            fprintf(stderr, "  [FILE] empty file name (options 0x%X); host frames:", (unsigned)options);
            for (i = 0; i < c; i++)
                fprintf(stderr, " 0x%llX", (unsigned long long)(0x140000000ULL + ((uintptr_t)fr[i] - mb)));
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
    trap_nan_check_path(name.Buffer);
    memset(&ios, 0, sizeof(ios));

    st = xbox_NtCreateFile(&h, access, &oa, &ios, NULL,
                           file_attrs, share, disposition, options);

    fprintf(stderr, "  [FILE] create/open: path=\"%s\" -> status=0x%08X\n", name.Buffer, (uint32_t)st);
    fflush(stderr);

    if (NT_SUCCESS(st)) {
        if (xbox_dir_context_bound(h)) {
            /* Should never happen now that NtClose drops the scan; reported
             * because it silently breaks directory listings (save lookup). */
            fprintf(stderr, "  [DIRCTX] %s: handle %p inherits a live directory scan\n",
                    name.Buffer, h);
            fflush(stderr);
        }
        file_ledger_open(h, name.Buffer);
        bridge_write_handle(handle_va, h);
        if (handle_va) hpath_set(BRIDGE_MEM32(handle_va), name.Buffer);
        bridge_write_iostatus(iostatus_va, ios.Status, (uint32_t)ios.Information);
    } else {
        bridge_write_iostatus(iostatus_va, st, 0);
    }
    return st;
}

/* ── NtCreateFile (ordinal 190, 9 args = 36 bytes) ─────── */
static void bridge_NtCreateFile(void)
{
    uint32_t handle_va   = STACK_ARG(0);  /* PHANDLE */
    uint32_t access      = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs   = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus    = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    /* arg4: AllocationSize - ignored */
    uint32_t file_attrs  = STACK_ARG(5);  /* FileAttributes */
    uint32_t share       = STACK_ARG(6);  /* ShareAccess */
    uint32_t disposition = STACK_ARG(7);  /* CreateDisposition */
    uint32_t options     = STACK_ARG(8);  /* CreateOptions */

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* ── NtOpenFile (ordinal 202, 6 args = 24 bytes) ──────── */
static void bridge_NtOpenFile(void)
{
    uint32_t handle_va = STACK_ARG(0);  /* PHANDLE */
    uint32_t access    = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus  = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    uint32_t share     = STACK_ARG(4);  /* ShareAccess */
    uint32_t options   = STACK_ARG(5);  /* OpenOptions */

    /* NtOpenFile = NtCreateFile with FILE_OPEN disposition */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        0, share, 1 /* FILE_OPEN */, options);
}

/*
 * Per-file I/O ledger. "Was this asset actually read, or only opened?" comes up
 * constantly -- a title can open a file, fail to size it, and read nothing,
 * which looks identical to success in the open log. Tracked here and exposed
 * through the diagnostics server's `files` command.
 */
#define XBOX_FILE_LEDGER 256
typedef struct {
    char     path[128];
    uint64_t bytes_read;
    uint32_t reads;
    HANDLE   h;
} xbox_file_rec;
static xbox_file_rec g_files[XBOX_FILE_LEDGER];
static int g_files_n = 0;

static void file_ledger_open(HANDLE h, const char *path)
{
    int i;
    if (!h || !path) return;
    for (i = 0; i < g_files_n; i++)
        if (g_files[i].h == h) { g_files[i].h = NULL; }   /* handle reused */
    /* Reopening a path reuses its row: the memory-unit scan opens "U:\\" dozens
     * of times, which filled the 64-row ledger before the first menu and hid
     * every later read. */
    for (i = 0; i < g_files_n; i++)
        if (!strcmp(g_files[i].path, path)) { g_files[i].h = h; return; }
    if (g_files_n >= XBOX_FILE_LEDGER) return;
    {
        xbox_file_rec *r = &g_files[g_files_n++];
        strncpy(r->path, path, sizeof(r->path) - 1);
        r->path[sizeof(r->path) - 1] = 0;
        r->bytes_read = 0; r->reads = 0; r->h = h;
    }
}

static void file_ledger_read(HANDLE h, uint32_t n)
{
    int i;
    for (i = 0; i < g_files_n; i++)
        if (g_files[i].h == h) { g_files[i].bytes_read += n; g_files[i].reads++; return; }
}

/* Snapshot for the diagnostics server. */
int xbox_file_ledger_get(int idx, const char **path, unsigned long long *bytes,
                         unsigned int *reads)
{
    if (idx < 0 || idx >= g_files_n) return 0;
    if (path)  *path  = g_files[idx].path;
    if (bytes) *bytes = g_files[idx].bytes_read;
    if (reads) *reads = g_files[idx].reads;
    return 1;
}

/* ── NtReadFile (ordinal 219, 8 args = 32 bytes) ──────── */
static void bridge_NtReadFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    PLARGE_INTEGER poff = NULL;

    if (!buffer_va && length) {
        /* A read into guest address 0 means the caller's buffer allocation
         * failed and it carried on (the fourth outfit in Customize
         * Outfit froze the frontend this way). Always reported, with the
         * host frames (addr2line, image base 0x140000000) and the guest code
         * addresses on the stack. */
        static int shown = 0;
        if (shown++ < 8) {
            extern volatile unsigned g_last_loc;
            void *fr[16];
            unsigned short c = (unsigned short)CaptureStackBackTrace(0, 16, fr, NULL), i;
            uintptr_t mb = (uintptr_t)GetModuleHandleW(NULL);
            fprintf(stderr, "[READ] NULL buffer: h=%p len=%u after loc_%08X; host frames:",
                    handle, length, (unsigned)g_last_loc);
            for (i = 0; i < c; i++)
                fprintf(stderr, " 0x%llX",
                        (unsigned long long)(0x140000000ULL + ((uintptr_t)fr[i] - mb)));
            fprintf(stderr, "\n         guest stack code refs:");
            for (i = 8; i < 72; i++) {
                uint32_t v = BRIDGE_MEM32(g_esp + i * 4);
                if (v >= 0x00011000u && v < 0x00190000u)
                    fprintf(stderr, " [esp+0x%X]=0x%08X", i * 4, v);
            }
            fprintf(stderr, "\n");
            fflush(stderr);
        }
    }
    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    {
        /* XBOX_READ_STATS=1 (diagnostic): reads, bytes and the time
         * the calling thread spent blocked in them, every 2 s. */
        static int stats = -1;
        static LARGE_INTEGER f, last;
        static long n;
        static double bytes, ms, worst;
        LARGE_INTEGER a, b;
        if (stats < 0) { const char *e = getenv("XBOX_READ_STATS"); stats = e && e[0] == '1'; }
        if (stats) { if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a); }
        {
            /* A diag write-watch on the destination would make ReadFile fail
             * with ERROR_NOACCESS; lift it for the read (xbox_diag.c). */
            extern int xbox_diag_watch_host_write(uint32_t va, uint32_t len, int begin);
            int wl = xbox_diag_watch_host_write(buffer_va, length, 1);
            g_eax = (uint32_t)xbox_NtReadFile(handle, NULL, NULL, NULL, &ios,
                        XBOX_TO_NATIVE(buffer_va), length, poff);
            if (wl) xbox_diag_watch_host_write(buffer_va, length, 0);
        }
        if (stats) {
            double t;
            QueryPerformanceCounter(&b);
            t = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
            n++; bytes += (double)ios.Information; ms += t;
            if (t > worst) worst = t;
            if (!last.QuadPart) last = b;
            if (b.QuadPart - last.QuadPart > 2 * f.QuadPart) {
                fprintf(stderr, "[READS] %ld reads, %.0f KB, %.1f ms blocked (worst %.1f ms) on thread %lu\n",
                        n, bytes / 1024.0, ms, worst, GetCurrentThreadId());
                n = 0; bytes = ms = worst = 0;
                last = b;
            }
        }
    }
    /* XBOX_READ_LOG=1 traces where each read actually lands. A file whose
     * ledger says it was read in full can still leave its buffer empty if the
     * destination never advances, or if the title asked for a completion
     * event this bridge drops (args 1-3 are passed as NULL below). */
    if (getenv("XBOX_READ_LOG")) {
        fprintf(stderr, "[READ] h=%p buf=0x%08X len=%u off=0x%llX -> %u"
                " status=0x%08X evt=0x%08X apc=0x%08X\n",
                handle, buffer_va, length,
                poff ? (unsigned long long)off.QuadPart : 0ULL,
                (unsigned)ios.Information, (unsigned)ios.Status,
                STACK_ARG(1), STACK_ARG(2));
        /* The first bytes decide how the title interprets the block -- a
         * Gimex stream is recognised by byte[1] == 0xFB and a format byte,
         * and an unrecognised one makes FILE_LoadPackedGimexAsset skip the
         * decode and hand back the raw buffer. Print them. */
        if (ios.Information) {
            const unsigned char *b = (const unsigned char *)XBOX_TO_NATIVE(buffer_va);
            unsigned k, n = (unsigned)ios.Information; if (n > 16) n = 16;
            fprintf(stderr, "         data:");
            for (k = 0; k < n; k++) fprintf(stderr, " %02X", b[k]);
            fprintf(stderr, "  |");
            for (k = 0; k < n; k++)
                fprintf(stderr, "%c", (b[k] >= 32 && b[k] < 127) ? b[k] : '.');
            fprintf(stderr, "|\n");
            /* Name the caller of the small header reads. The pack layer reads
             * a 16-byte header and then the directory sequentially, and the
             * directory in the file starts at 12 -- so which function chose 16
             * is the whole question. */
            if (length <= 32 && getenv("XBOX_READ_TRACE")) {
                void *fr[12];
                unsigned short c = (unsigned short)CaptureStackBackTrace(1, 12, fr, NULL), i;
                uintptr_t mb = (uintptr_t)GetModuleHandleW(NULL);
                fprintf(stderr, "         callers:");
                for (i = 0; i < c; i++)
                    fprintf(stderr, " 0x%llX",
                            (unsigned long long)(0x140000000ULL + ((uintptr_t)fr[i] - mb)));
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }
    file_ledger_read(handle, (uint32_t)ios.Information);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
}

/* ── NtWriteFile (ordinal 236, 8 args = 32 bytes) ─────── */
static void bridge_NtWriteFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryInformationFile (ordinal 211, 5 args = 20 bytes) */
static void bridge_NtQueryInformationFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    /* Same switch as XBOX_READ_LOG: a whole-file loader sizes its buffer from
     * this call, so a wrong answer here shows up much later as a short read
     * and a structure parsed out of the wrong bytes. */
    if (getenv("XBOX_READ_LOG")) {
        const uint32_t *w = (const uint32_t *)XBOX_TO_NATIVE(info_va);
        fprintf(stderr, "[QINFO] h=%p class=%u len=%u -> status=0x%08X info=%u"
                        " alloc=%u eof=%u attr=0x%08X\n",
                handle, infoclass, length, (unsigned)ios.Status,
                (unsigned)ios.Information,
                length >= 40 ? w[8]  : 0u,   /* AllocationSize.LowPart @ +32 */
                length >= 48 ? w[10] : 0u,   /* EndOfFile.LowPart      @ +40 */
                length >= 52 ? w[12] : 0u);  /* FileAttributes         @ +48 */
        fflush(stderr);
    }
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtSetInformationFile (ordinal 226, 5 args = 20 bytes) ─ */
static void bridge_NtSetInformationFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtSetInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    /* Seeks matter as much as reads here: a pack directory read that starts
     * four bytes late looks identical in the read log whether the title asked
     * for the wrong offset or asked correctly and the seek was dropped. */
    if (getenv("XBOX_READ_LOG")) {
        const uint32_t *w = (const uint32_t *)XBOX_TO_NATIVE(info_va);
        fprintf(stderr, "[SEEK] h=%p class=%u len=%u -> status=0x%08X pos=%u\n",
                handle, infoclass, length, (unsigned)g_eax,
                length >= 4 ? w[0] : 0u);
        fflush(stderr);
    }
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryVolumeInformationFile (ordinal 218, 5 args = 20 bytes) */
static void bridge_NtQueryVolumeInformationFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryVolumeInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FS_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryFullAttributesFile (ordinal 210, 2 args = 8 bytes) */
static void bridge_NtQueryFullAttributesFile(void)
{
    uint32_t obj_attrs = STACK_ARG(0);
    uint32_t info_va   = STACK_ARG(1);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(obj_attrs, &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtQueryFullAttributesFile(&oa,
                (PXBOX_FILE_NETWORK_OPEN_INFORMATION)XBOX_TO_NATIVE(info_va));
}

/* ── NtFlushBuffersFile (ordinal 198, 2 args = 8 bytes) ─── */
static void bridge_NtFlushBuffersFile(void)
{
    HANDLE   handle = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(1);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFlushBuffersFile(handle, &ios);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtDeleteFile (ordinal 195, 1 arg = 4 bytes) ─────── */
static void bridge_NtDeleteFile(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(STACK_ARG(0), &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtDeleteFile(&oa);
}

/* ── NtQueryDirectoryFile (ordinal 207, 9 args = 36 bytes) ─ */
static void bridge_NtQueryDirectoryFile(void)
{
    HANDLE   handle      = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va      = STACK_ARG(4);
    uint32_t info_va     = STACK_ARG(5);
    uint32_t length      = STACK_ARG(6);
    /* Xbox's NtQueryDirectoryFile takes TEN arguments, not nine: there is a
     * FILE_INFORMATION_CLASS at index 7, between Length and FileName. The
     * generated call site pushes exactly that -- ..., 0x148, 1, &FileName, 0 --
     * and reading FileName/RestartScan one slot early handed the ANSI_STRING
     * a literal 1 as its pointer. The query then ran with a garbage pattern
     * and returned STATUS_NO_MORE_FILES every time, so the save-slot
     * enumerator found nothing and the title never saw the save on the HDD. */
    uint32_t infoclass   = STACK_ARG(7);  /* FILE_INFORMATION_CLASS */
    uint32_t filename_va = STACK_ARG(8);  /* PXBOX_ANSI_STRING */
    uint32_t restart     = STACK_ARG(9);  /* BOOLEAN */
    (void)infoclass;
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING     fn;
    PXBOX_ANSI_STRING    pfn = NULL;

    memset(&ios, 0, sizeof(ios));
    if (filename_va) {
        /* Xbox ANSI_STRING: 0=Length(u16), 2=MaximumLength(u16), 4=Buffer(u32) */
        uint32_t fn_buf  = BRIDGE_MEM32(filename_va + 4);
        fn.Length        = BRIDGE_MEM16(filename_va);
        fn.MaximumLength = BRIDGE_MEM16(filename_va + 2);
        fn.Buffer        = fn_buf ? (PCHAR)XBOX_TO_NATIVE(fn_buf) : NULL;
        if (fn.Buffer) pfn = &fn;
    }
    g_eax = (uint32_t)xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(info_va), length, pfn, (BOOLEAN)restart);
    /* The save-slot enumerator lives or dies on this call, and the ordinary
     * kernel call log is capped at 200 entries -- which is why it looked like
     * this was never invoked at all. */
    if (getenv("XBOX_SAVE_LOG")) {
        fprintf(stderr, "  [QDIR] h=%p pattern=\"%.*s\" restart=%u len=%u"
                        " -> status=0x%08X info=%u\n",
                handle, pfn ? (int)pfn->Length : 0,
                pfn && pfn->Buffer ? pfn->Buffer : "",
                (unsigned)restart, length,
                (unsigned)g_eax, (unsigned)ios.Information);
        fflush(stderr);
    }
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtOpenSymbolicLinkObject (ordinal 203, 2 args = 8 bytes) */
static void bridge_NtOpenSymbolicLinkObject(void)
{
    uint32_t handle_va = STACK_ARG(0);
    /* arg1: POBJECT_ATTRIBUTES - ignored, we return a synthetic handle.
     * Written raw (untagged) so NtClose recognises it and skips it. */
    if (handle_va) BRIDGE_MEM32(handle_va) = 0xDEAD0001u;
    g_eax = STATUS_SUCCESS;
}

/* ── NtQuerySymbolicLinkObject (ordinal 215, 3 args = 12 bytes) */
static void bridge_NtQuerySymbolicLinkObject(void)
{
    /* uint32_t handle = STACK_ARG(0); */
    uint32_t target_va = STACK_ARG(1);
    uint32_t retlen_va = STACK_ARG(2);
    const char* target = "\\Device\\CdRom0";
    USHORT len = (USHORT)strlen(target);

    if (target_va) {
        uint16_t max_len = BRIDGE_MEM16(target_va + 2);
        uint32_t buf_va  = BRIDGE_MEM32(target_va + 4);
        if (buf_va && len < max_len) {
            memcpy(XBOX_TO_NATIVE(buf_va), target, len + 1);
            BRIDGE_MEM16(target_va) = len;
        }
    }
    if (retlen_va) BRIDGE_MEM32(retlen_va) = (uint32_t)len;
    g_eax = STATUS_SUCCESS;
}

/* ── Rtl/Ke/Mm/Ob/Av bridges that were declared but never wired up ──────
 *
 * Each of these already had a working HLE implementation (kernel_rtl.c,
 * kernel_thread.c, kernel_hal.c, kernel_memory.c, kernel_ob.c) *and* an
 * entry in the stdcall arg-size table below, but no case in
 * bridge_for_ordinal -- so every call fell through kernel_thunk_dispatch's
 * "no bridge, returning 0" path. Found by auditing the "no bridge for
 * ordinal N" warnings the dispatcher already prints, against the ordinals
 * SSX Tricky actually calls.
 *
 * The two string ones were doing real damage rather than merely being
 * absent:
 *   - RtlInitAnsiString is what fills in a counted ANSI_STRING's Length /
 *     MaximumLength / Buffer. With it a no-op, the descriptor the game then
 *     handed to NtCreateFile kept whatever stack garbage was already there
 *     (observed live: Length=29345 with MaximumLength=24 -- impossible, and
 *     Length > MaximumLength -- alongside a perfectly valid Buffer pointing
 *     at "00000000", an Xbox title-ID save folder name).
 *   - RtlEqualString returning 0 means "not equal" for *every* comparison,
 *     so no string lookup in the title could ever match. It was called
 *     ~200 times during boot alone.
 */

/* ── RtlInitAnsiString (ordinal 289, 2 args = 8 bytes) ──
 * VOID RtlInitAnsiString(PANSI_STRING DestinationString, PCSZ SourceString)
 * Writes the counted-string fields straight into Xbox memory: the game reads
 * them back from there, so they must be stored as Xbox VAs, not native ones. */
static void bridge_RtlInitAnsiString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) { g_eax = 0; return; }

    if (!src_va) {
        BRIDGE_MEM16(dest_va + 0) = 0;   /* Length        */
        BRIDGE_MEM16(dest_va + 2) = 0;   /* MaximumLength */
        BRIDGE_MEM32(dest_va + 4) = 0;   /* Buffer        */
    } else {
        const char *s = (const char *)XBOX_TO_NATIVE(src_va);
        size_t len = strlen(s);
        if (len > 0xFFFE) len = 0xFFFE;  /* USHORT fields */
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)len;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(len + 1); /* includes the NUL */
        BRIDGE_MEM32(dest_va + 4) = src_va;
    }
    g_eax = 0;
}

/* ── RtlEqualString (ordinal 279, 3 args = 12 bytes) ────
 * BOOLEAN RtlEqualString(PANSI_STRING S1, PANSI_STRING S2, BOOLEAN CaseInSensitive)
 * Both descriptors live in Xbox memory and are counted, not NUL-terminated,
 * so compare exactly Length bytes rather than using strcmp. */
static void bridge_RtlEqualString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t ci    = STACK_ARG(2);
    uint16_t l1, l2;
    uint32_t b1, b2;

    if (!s1_va || !s2_va) { g_eax = 0; return; }

    l1 = BRIDGE_MEM16(s1_va + 0); b1 = BRIDGE_MEM32(s1_va + 4);
    l2 = BRIDGE_MEM16(s2_va + 0); b2 = BRIDGE_MEM32(s2_va + 4);

    if (l1 != l2)      { g_eax = 0; return; }
    if (l1 == 0)       { g_eax = 1; return; }
    if (!b1 || !b2)    { g_eax = 0; return; }

    {
        const unsigned char *p1 = (const unsigned char *)XBOX_TO_NATIVE(b1);
        const unsigned char *p2 = (const unsigned char *)XBOX_TO_NATIVE(b2);
        uint16_t i;
        for (i = 0; i < l1; i++) {
            unsigned char c1 = p1[i], c2 = p2[i];
            if (ci) {
                if (c1 >= 'a' && c1 <= 'z') c1 = (unsigned char)(c1 - 32);
                if (c2 >= 'a' && c2 <= 'z') c2 = (unsigned char)(c2 - 32);
            }
            if (c1 != c2) { g_eax = 0; return; }
        }
    }
    g_eax = 1;
}

/* ── KeDelayExecutionThread (ordinal 99, 3 args = 12 bytes) ──
 * NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE, BOOLEAN Alertable,
 *                                 PLARGE_INTEGER Interval)
 * This is the title's Sleep(). Unbridged it returned immediately, turning
 * every intended sleep into a busy spin that starves the other threads. */
static void bridge_KeDelayExecutionThread(void)
{
    uint32_t wait_mode    = STACK_ARG(0);
    uint32_t alertable    = STACK_ARG(1);
    uint32_t interval_va  = STACK_ARG(2);

    g_eax = (uint32_t)xbox_KeDelayExecutionThread(
        (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        interval_va ? (PLARGE_INTEGER)XBOX_TO_NATIVE(interval_va) : NULL);
}

/* ── KeStallExecutionProcessor (ordinal 151, 1 arg = 4 bytes) ── */
static void bridge_KeStallExecutionProcessor(void)
{
    /* This is a busy-wait, so a caller that spins here is spinning on a
     * hardware condition that is never going to change under HLE. Naming the
     * guest label that keeps calling it costs one counter and turns "12% of
     * kernel calls are stalls" into an address. XBOX_STALL_TRACE=1. */
    {
        static int enabled = -1;
        static unsigned long n = 0;
        if (enabled < 0) {
            const char *e = getenv("XBOX_STALL_TRACE");
            enabled = (e && e[0] == '1') ? 1 : 0;
        }
        if (enabled && ((++n & 0xFF) == 1)) {
            extern volatile unsigned g_main_loc, g_last_loc;
            /* The guest return address sits at [esp] on entry to the bridge.
             * That names the caller outright; g_last_loc only reports the last
             * label anything stamped, which for a leaf spin is the wrong
             * function entirely. */
            fprintf(stderr, "  [STALL] #%lu us=%u ret=0x%08X main=loc_%08X last=loc_%08X\n",
                    n, (unsigned)STACK_ARG(0),
                    (unsigned)BRIDGE_MEM32(g_esp - 4),
                    (unsigned)g_main_loc, (unsigned)g_last_loc);
            fflush(stderr);
        }
    }
    xbox_KeStallExecutionProcessor((ULONG)STACK_ARG(0));
    g_eax = 0;
}

/* ── KeSetBasePriorityThread (ordinal 143, 2 args = 8 bytes) ──
 * Thread is an Xbox VA dispatcher object, not a native pointer; the HLE
 * only uses it as an opaque key, so pass it straight through. */
static void bridge_KeSetBasePriorityThread(void)
{
    uint32_t thread_va = STACK_ARG(0);
    int32_t  increment = (int32_t)STACK_ARG(1);
    g_eax = (uint32_t)xbox_KeSetBasePriorityThread((PVOID)(uintptr_t)thread_va,
                                                   (LONG)increment);
}

/* ── MmLockUnlockBufferPages (ordinal 175, 3 args = 12 bytes) ──
 * No real paging here, so this is advisory; forward it anyway so the HLE
 * keeps whatever bookkeeping it does and the call stops warning. */
static void bridge_MmLockUnlockBufferPages(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t bytes   = STACK_ARG(1);
    uint32_t unlock  = STACK_ARG(2);
    xbox_MmLockUnlockBufferPages(base_va ? XBOX_TO_NATIVE(base_va) : NULL,
                                 (ULONG)bytes, (BOOLEAN)unlock);
    g_eax = 0;
}

/* ── RtlTimeToTimeFields (ordinal 305, 2 args = 8 bytes) ── */
static void bridge_RtlTimeToTimeFields(void)
{
    uint32_t time_va   = STACK_ARG(0);
    uint32_t fields_va = STACK_ARG(1);
    if (time_va && fields_va) {
        xbox_RtlTimeToTimeFields((PLARGE_INTEGER)XBOX_TO_NATIVE(time_va),
                                 (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(fields_va));
    }
    g_eax = 0;
}

/* ── ObfDereferenceObject (ordinal 250, __fastcall: object in ecx) ──
 * Declared with 0 stdcall arg bytes because the argument arrives in ecx,
 * not on the stack -- read g_ecx directly. */
static void bridge_ObfDereferenceObject(void)
{
    uint32_t obj_va = g_ecx;
    if (obj_va) xbox_ObfDereferenceObject((PVOID)(uintptr_t)obj_va);
    g_eax = 0;
}

/* ── NtResumeThread (ordinal 224, 2 args = 8 bytes) ──
 * NTSTATUS NtResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)
 * The handle arrives as a 32-bit Xbox-memory token, so resolve it through the
 * bridge handle table rather than casting it straight to a native HANDLE. */
static void bridge_NtResumeThread(void)
{
    uint32_t thread_tok  = STACK_ARG(0);
    uint32_t prev_cnt_va = STACK_ARG(1);
    HANDLE h = bridge_read_handle(thread_tok);
    ULONG prev = 0;

    g_eax = (uint32_t)xbox_NtResumeThread(h, &prev);
    if (prev_cnt_va) BRIDGE_MEM32(prev_cnt_va) = (uint32_t)prev;
}

/* ── AvGetSavedDataAddress (ordinal 1, 0 args) ── */
static void bridge_AvGetSavedDataAddress(void)
{
    g_eax = (uint32_t)xbox_AvGetSavedDataAddress();
}

/* ── AvSendTVEncoderOption (ordinal 2, 4 args = 16 bytes) ── */
static void bridge_AvSendTVEncoderOption(void)
{
    uint32_t reg_base = STACK_ARG(0);
    uint32_t option   = STACK_ARG(1);
    uint32_t param    = STACK_ARG(2);
    uint32_t result   = STACK_ARG(3);
    xbox_AvSendTVEncoderOption(reg_base ? XBOX_TO_NATIVE(reg_base) : NULL,
                               (ULONG)option, (ULONG)param,
                               result ? (PULONG)XBOX_TO_NATIVE(result) : NULL);
    g_eax = 0;
}

/* ── IoCreateFile (ordinal 67, 10 args = 40 bytes) ────── */
static void bridge_IoCreateFile(void)
{
    /* Same as NtCreateFile with an extra Options arg at the end */
    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);
    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* ── NtDeviceIoControlFile (ordinal 196, 10 args = 40 bytes) */
static void bridge_NtDeviceIoControlFile(void)
{
    uint32_t ioctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);
    fprintf(stderr, "  [FILE] NtDeviceIoControlFile(0x%X) - stub\n", ioctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu; /* STATUS_NOT_IMPLEMENTED */
}

/* ── NtFsControlFile (ordinal 200, 10 args = 40 bytes) ──── */
static void bridge_NtFsControlFile(void)
{
    uint32_t fsctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);
    fprintf(stderr, "  [FILE] NtFsControlFile(0x%X) - stub\n", fsctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu;
}

/* ── NtCreateDirectoryObject (ordinal 188) ──────────────── */
static void bridge_NtCreateDirectoryObject(void)
{
    /* Return STATUS_SUCCESS with a fake handle */
    uint32_t handle_ptr = STACK_ARG(0);
    if (handle_ptr) BRIDGE_MEM32(handle_ptr) = 0xBEEF0010;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── IoCreateSymbolicLink (ordinal 67) ───────────────────── */
static void bridge_IoCreateSymbolicLink(void)
{
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── IoCreateDevice (ordinal 65) ──────────────────────────
 * NTSTATUS IoCreateDevice(PVOID DriverObject, ULONG DeviceExtensionSize,
 *     PXBOX_ANSI_STRING DeviceName, ULONG DeviceType, BOOLEAN Exclusive,
 *     PVOID* DeviceObject)
 *
 * Unlike xbox_IoCreateDevice() in kernel_io.c (which native-HeapAllocs and
 * hands back a real host pointer -- fine for the legacy thunk-table path,
 * where callers receive native pointers directly, but wrong here: this
 * bridge path only ever hands Xbox VAs to game code, which stores the
 * result and later dereferences it through MEM32/XBOX_PTR as if it were
 * one). Allocate the fake device from Xbox VA space instead, and put
 * DeviceExtension at +0x18 -- the real Xbox DEVICE_OBJECT's offset for it,
 * and the field driver code actually dereferences after creation.
 */
static void bridge_IoCreateDevice(void)
{
    uint32_t driver_obj  = STACK_ARG(0);
    uint32_t ext_size    = STACK_ARG(1);
    uint32_t device_type = STACK_ARG(3);
    uint32_t out_va      = STACK_ARG(5);
    uint32_t header_size = 0x20;
    uint32_t total_size  = header_size + ext_size;
    uint32_t dev_va      = xbox_HeapAlloc(total_size, 16);

    if (!dev_va) {
        if (out_va) BRIDGE_MEM32(out_va) = 0;
        g_eax = 0xC000009Au;  /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }

    /* xbox_HeapAlloc zero-fills, so untouched fields (Flags, Characteristics,
     * etc.) default to 0 -- safe for the |=/&= bit-twiddling driver init
     * code typically does on them. */
    BRIDGE_MEM16(dev_va + 0x00) = (uint16_t)device_type; /* Type */
    BRIDGE_MEM32(dev_va + 0x08) = driver_obj;             /* DriverObject */
    BRIDGE_MEM32(dev_va + 0x18) = ext_size ? dev_va + header_size : 0; /* DeviceExtension */

    if (out_va) BRIDGE_MEM32(out_va) = dev_va;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── ExQueryNonVolatileSetting (ordinal 24) ───────────────
 * NTSTATUS ExQueryNonVolatileSetting(ULONG ValueIndex, PULONG Type,
 *     PVOID Value, ULONG ValueLength, PULONG ResultLength)
 *
 * Reads a setting out of Xbox EEPROM/NVRAM (video mode, language, etc.)
 * into a caller-supplied buffer. Found via the unbridged-ordinal fallback
 * silently returning 0 (STATUS_SUCCESS) without ever touching that buffer
 * -- the exact "reports success over an unpopulated output" shape that
 * turned out to be the real bug behind bridge_IoCreateDevice, while
 * tracing a ~2.13GB bogus D3D allocation request back through a garbage
 * format-table index.
 * We don't know this title's exact EEPROM layout, so rather than guess
 * per-ValueIndex defaults (risking a *different*, equally wrong value),
 * zero-fill whatever buffer the caller gave us -- deterministic, so any
 * code path that carelessly reads it anyway gets 0, not stack garbage --
 * and report the setting as not found, so any code path that (as is
 * normal/expected on a first boot with no saved config) checks the
 * status falls back to its own real default instead.
 */
/*
 * The one setting the host does answer: XC_VIDEO_FLAGS (index 8), when the
 * launcher has set it. SSX Tricky reads it through its linked XAPI
 * XGetVideoFlags (0x15299F, which returns (flags >> 16) & 0x5F), and
 * Video_GetScreenModeFromFlags (0xFE580) turns bit 0 (AV_FLAGS_WIDESCREEN,
 * 0x00010000 in the stored value) into screen mode 2 -- anamorphic 16:9,
 * the dashboard's "Widescreen" setting on a real console -- and bit 4
 * (letterbox, 0x00100000) into mode 1. Unset, the query keeps failing as
 * before and the title picks its own default, 4:3.
 */
static int      s_video_flags_set = 0;
static uint32_t s_video_flags     = 0;

void xbox_SetVideoFlags(uint32_t eeprom_video_flags)
{
    s_video_flags = eeprom_video_flags;
    s_video_flags_set = 1;
}

static void bridge_ExQueryNonVolatileSetting(void)
{
    uint32_t index        = STACK_ARG(0);
    uint32_t type_va      = STACK_ARG(1);
    uint32_t value_va     = STACK_ARG(2);
    uint32_t value_len    = STACK_ARG(3);
    uint32_t result_len_va = STACK_ARG(4);

    if (index == 8 && s_video_flags_set && value_va && value_len >= 4) {
        if (type_va) BRIDGE_MEM32(type_va) = 4;           /* REG_DWORD */
        BRIDGE_MEM32(value_va) = s_video_flags;
        if (result_len_va) BRIDGE_MEM32(result_len_va) = 4;
        g_eax = 0;                                        /* STATUS_SUCCESS */
        return;
    }

    if (type_va) BRIDGE_MEM32(type_va) = 0;
    if (value_va && value_len) {
        memset(XBOX_TO_NATIVE(value_va), 0, value_len);
    }
    if (result_len_va) BRIDGE_MEM32(result_len_va) = 0;

    g_eax = 0xC0000034u;  /* STATUS_OBJECT_NAME_NOT_FOUND */
}

/* ── ObReferenceObjectByHandle (ordinal 246) ─────────────── */
static void bridge_ObReferenceObjectByHandle(void)
{
    /* Xbox: NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object)
     * 3 args (not 6 like Windows NT) */
    uint32_t handle = STACK_ARG(0);
    uint32_t obj_type = STACK_ARG(1);
    uint32_t object_ptr = STACK_ARG(2);
    if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── RtlRaiseException (ordinal 302) ─────────────────────
 * VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
 *
 * Called by CRT / SEH code to raise structured exceptions.
 * On Xbox this triggers the kernel exception dispatcher.
 * For recompilation, we log and continue (no real SEH dispatch yet).
 */
static void bridge_RtlRaiseException(void)
{
    uint32_t record_ptr = STACK_ARG(0);
    uint32_t code = record_ptr ? BRIDGE_MEM32(record_ptr) : 0;

    static int raise_count = 0;
    raise_count++;
    if (raise_count <= 10) {
        /* The guest call chain, approximately: stack words that point into
         * .text, nearest first. Enough to name which CRT math routine raised
         * and from where (invalid-operation raises seeded NaN into
         * the race camera). */
        char chain[256];
        int n = 0, k;
        chain[0] = 0;
        for (k = 0; k < 96 && n < 8; k++) {
            uint32_t w = BRIDGE_MEM32(g_esp + 4u * (uint32_t)k);
            if (w >= 0x00011000u && w < 0x00187000u) {
                int len = (int)strlen(chain);
                snprintf(chain + len, sizeof chain - (size_t)len, " %08X", w);
                n++;
            }
        }
        fprintf(stderr, "  [KERNEL] RtlRaiseException: record=0x%08X code=0x%08X (#%d) stack:%s\n",
                record_ptr, code, raise_count, chain);
        {
            /* Translated calls push a dummy return address, so the guest
             * stack cannot be walked; the native one can (addr2line). */
            void *fr[16];
            USHORT nf = CaptureStackBackTrace(0, 16, fr, NULL), f;
            uintptr_t base = (uintptr_t)GetModuleHandle(NULL);
            fprintf(stderr, "  [KERNEL]   native frames:");
            for (f = 0; f < nf; f++)
                fprintf(stderr, " 0x%llX",
                        (unsigned long long)(0x140000000ULL + ((uintptr_t)fr[f] - base)));
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }

    /* Handle float exceptions by clearing the FPU status.
     *
     * On the real Xbox, RtlRaiseException dispatches through the SEH chain.
     * For float exceptions (0xC0000090-0xC0000096), the CRT exception handler
     * clears the x87/SSE status word and continues execution. Without clearing,
     * the caller re-checks the FPU status, sees the exception still pending,
     * and re-raises in an infinite loop.
     *
     * _clearfp() clears both x87 and SSE exception flags on Windows x64.
     */
    if (code >= 0xC0000090u && code <= 0xC0000096u) {
        _clearfp();
    }

    g_eax = 0;
}

/* ── MmMapIoSpace (ordinal 177) ──────────────────────────
 * PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
 *
 * Maps physical I/O memory (GPU registers, etc.) into virtual address space.
 * Allocate from Xbox heap so the returned pointer is a valid Xbox VA.
 */
static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t xbox_va = xbox_HeapAlloc(num_bytes, 4096);

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u → Xbox VA 0x%08X\n",
            phys_addr, num_bytes, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

/* ── MmPersistContiguousMemory (ordinal 178) ─────────────
 * VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
 *
 * Marks contiguous memory as persistent across reboots (for save data).
 * No-op for recompilation.
 */
static void bridge_MmPersistContiguousMemory(void)
{
    /* No-op stub */
    g_eax = 0;
}

/* ── Generic fallback for simple value-only functions ────── */
static void bridge_generic_stub(void)
{
    /* Success-returning stub for functions whose callers only check for 0.
     * Deliberately silent: the caller (kernel_thunk_dispatch) warns for
     * ordinals with no bridge at all, which is the case worth hearing about. */
    g_eax = 0;
}

/* ── Dispatch table: ordinal → bridge function + stack arg bytes ── */

typedef void (*bridge_func_t)(void);

/**
 * stdcall arg byte count for each kernel ordinal.
 * On x86 stdcall, the callee cleans (ret N). Our bridges must do the same
 * via g_esp += N after execution so the simulated stack stays balanced.
 *
 * Special cases:
 *   - KfRaiseIrql/KfLowerIrql: fastcall (arg in ecx), 0 stack bytes
 *   - KeSetTimer: DueTime is LARGE_INTEGER (8 bytes on stack) + Timer + Dpc
 */
static int stdcall_args_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* ── Display / AV ── */
    case   1: return  0;  /* AvGetSavedDataAddress(void) */
    case   2: return 16;  /* AvSendTVEncoderOption(4) */
    case   3: return 24;  /* AvSetDisplayMode(6) */
    case   4: return  4;  /* AvSetSavedDataAddress(1) */

    /* ── Unknown stubs ── */
    case   8: return  0;  /* DbgPrint(varargs) -- __cdecl, caller cleans */
    case  42: return  0;  /* Unknown_42(void) */

    /* ── Pool Allocator ── */
    case  14: return  4;  /* ExAllocatePool(1) */
    case  15: return  8;  /* ExAllocatePoolWithTag(2) */
    /* case  16: DATA export - ExEventObjectType */
    case  17: return  4;  /* ExFreePool(1) */
    case  23: return  4;  /* ExQueryPoolBlockSize(1) */
    case  24: return 20;  /* ExQueryNonVolatileSetting(5) */

    /* ── HAL ── */
    case  38: return  4;  /* HalClearSoftwareInterrupt(1) */
    case  39: return  8;  /* HalDisableSystemInterrupt(2) */
    case  44: return  8;  /* HalGetInterruptVector(BusInterruptLevel, PKIRQL) -- 2 args.
                           *
                           * This said 12 for a long time, on the reading that the call site
                           * pushes three dwords:
                           *
                           *   0x00170471  push edi                <- NOT an argument
                           *   0x00170472  lea  eax, [ebp - 0xc]
                           *   0x00170475  push eax                <- &Irql
                           *   0x00170476  push 3                  <- BusInterruptLevel
                           *   0x00170478  call [0x187444]
                           *
                           * That `push edi` is the enclosing function's callee-saved
                           * register save -- it is popped at 0x00170523 -- not a third
                           * argument. Counting it made the bridge pop 4 bytes too many, so
                           * every call left esp 4 high, and the epilogue then read `esi`
                           * out of the slot where `ebx` had been saved. In this title that
                           * handed D3D device init a null NV2A context object
                           * (device + 0x2308 became 0), which is why `sub_00172202` wrote
                           * its 64-byte block through a null pointer and
                           * UI_BuildButtonGroup's list walk then span on the value that
                           * left at Xbox VA 0.
                           *
                           * Changing it to 8 was tried once before and reverted: it fixed
                           * the measured +4 at that epilogue but the run did not improve,
                           * because the IRQL and null-page defects were still corrupting
                           * everything downstream. Both are fixed now. */
    case   9: return  8;  /* HalReadSMCTrayState(2) */
    case  46: return 24;  /* HalReadWritePCISpace(6) */
    case  47: return  8;  /* HalRegisterShutdownNotification(2) */
    case  48: return  4;  /* HalRequestSoftwareInterrupt(1) */
    case  49: return  4;  /* HalReturnToFirmware(1) */
    case 358: return  0;  /* HalIsResetOrShutdownPending(void) */

    /* ── I/O Manager ── */
    case  61: return 36;  /* IoBuildDeviceIoControlRequest(9) */
    /* case  64: DATA export - IoCompletionObjectType */
    case  65: return 24;  /* IoCreateDevice(6) */
    case  66: return 40;  /* IoCreateFile(10) */
    case  67: return  8;  /* IoCreateSymbolicLink(2) */
    case  68: return  4;  /* IoDeleteDevice(1) */
    /* case  70: DATA export - IoDeviceObjectType */
    case  73: return 12;  /* IoInitializeIrp(3) */
    case  79: return 20;  /* IoSetIoCompletion(5) */
    case  81: return  8;  /* IoStartNextPacket(2) */
    case  82: return 12;  /* IoStartNextPacketByKey(3) */
    case  83: return 16;  /* IoStartPacket(4) */
    case  84: return 32;  /* IoSynchronousDeviceIoControlRequest(8) */
    case  85: return 20;  /* IoSynchronousFsdRequest(5) */
    case 359: return  4;  /* IoMarkIrpMustComplete(1) */

    /* ── Kernel Synchronization ── */
    case  93: return  8;  /* KeAlertThread(2) */
    case  95: return  4;  /* KeBugCheck(1) */
    case  96: return 20;  /* KeBugCheckEx(5) */
    case  97: return  4;  /* KeCancelTimer(1) */
    case  98: return  4;  /* KeConnectInterrupt(1) */
    case 107: return 12;  /* KeInitializeDpc(3) */
    case 109: return 28;  /* KeInitializeInterrupt(7) */
    case 113: return  8;  /* KeInitializeTimerEx(2) */
    case 119: return 12;  /* KeInsertQueueDpc(3) */
    case 124: return  4;  /* KeQueryBasePriorityThread(1) */
    case 125: return  0;  /* KeQueryInterruptTime(void) */
    case 126: return  0;  /* KeQueryPerformanceCounter(void) */
    case 127: return  0;  /* KeQueryPerformanceFrequency(void) */
    case 128: return  4;  /* KeQuerySystemTime(1) */
    case 129: return  0;  /* KeRaiseIrqlToDpcLevel(void) */
    case 137: return  4;  /* KeRemoveQueueDpc(1) */
    case 139: return  4;  /* KeRestoreFloatingPointState(1) */
    case 142: return  4;  /* KeSaveFloatingPointState(1) */
    case 143: return  8;  /* KeSetBasePriorityThread(2) */
    case 145: return 12;  /* KeSetEvent(3) */
    case 149: return 16;  /* KeSetTimer(Timer+DueTime[8]+Dpc) */
    case 150: return 20;  /* KeSetTimerEx(Timer+DueTime[8]+Period+Dpc) */
    case 151: return  4;  /* KeStallExecutionProcessor(1) */
    case 153: return 12;  /* KeSynchronizeExecution(3) */
    /* case 156: DATA export - KeTickCount */
    case 158: return 32;  /* KeWaitForMultipleObjects(8) */
    case 159: return 20;  /* KeWaitForSingleObject(5) */
    case 160: return  0;  /* KfRaiseIrql (fastcall: arg in ecx) */
    case 161: return  0;  /* KfLowerIrql (fastcall: arg in ecx) */

    /* ── Launch Data ── */
    /* case 164: DATA export - LaunchDataPage */

    /* ── Memory Management ── */
    case 165: return  4;  /* MmAllocateContiguousMemory(1) */
    case 166: return 20;  /* MmAllocateContiguousMemoryEx(5) */
    case 168: return  8;  /* MmClaimGpuInstanceMemory(2) */
    case 169: return  8;  /* MmCreateKernelStack(2) */
    case 170: return  8;  /* MmDeleteKernelStack(2) */
    case 171: return  4;  /* MmFreeContiguousMemory(1) */
    case 173: return  4;  /* MmGetPhysicalAddress(1) */
    case 175: return 12;  /* MmLockUnlockBufferPages(3) */
    case 176: return  8;  /* MmLockUnlockPhysicalPage(2) */
    case 177: return 12;  /* MmMapIoSpace(3) */
    case 178: return 12;  /* MmPersistContiguousMemory(3) */
    case 179: return  4;  /* MmQueryAddressProtect(1) */
    case 180: return  4;  /* MmQueryAllocationSize(1) */
    case 181: return  4;  /* MmQueryStatistics(1) */
    case 182: return 12;  /* MmSetAddressProtect(3) */

    /* ── NT Virtual Memory ── */
    case 184: return 20;  /* NtAllocateVirtualMemory(5) */

    /* ── NT File I/O & Handle ── */
    case 187: return  4;  /* NtClose(1) */
    case 188: return 12;  /* NtCreateDirectoryObject(3) */
    case 189: return 16;  /* NtCreateEvent(4) */
    case 190: return 36;  /* NtCreateFile(9) */
    case 193: return 16;  /* NtCreateSemaphore(4) */
    case 195: return  4;  /* NtDeleteFile(1) */
    case 196: return 40;  /* NtDeviceIoControlFile(10) */
    case 197: return 12;  /* NtDuplicateObject(3) */
    case 198: return  8;  /* NtFlushBuffersFile(2) */
    case 199: return 12;  /* NtFreeVirtualMemory(3) */
    case 200: return 40;  /* NtFsControlFile(10) */
    case 202: return 24;  /* NtOpenFile(6) */
    case 203: return  8;  /* NtOpenSymbolicLinkObject(2) */
    case 207: return 40;  /* NtQueryDirectoryFile(10) -- incl. FileInformationClass */
    case 210: return  8;  /* NtQueryFullAttributesFile(2) */
    case 211: return 20;  /* NtQueryInformationFile(5) */
    case 215: return 12;  /* NtQuerySymbolicLinkObject(3) */
    case 217: return 16;  /* NtQueryVirtualMemory(4) */
    case 218: return 20;  /* NtQueryVolumeInformationFile(5) */
    case 219: return 32;  /* NtReadFile(8) */
    case 222: return 12;  /* NtReleaseSemaphore(3) */
    case 186: return  4;  /* NtClearEvent(1) */
    case 234: return 16;  /* NtWaitForSingleObjectEx(4) */
    case 224: return  8;  /* NtResumeThread(2) -- was missing entirely, so the
                           * default 0 left both args on the simulated stack on
                           * every call: a silent 8-byte esp leak per thread
                           * resume, on the thread-creation path. Verified
                           * against xbox_NtResumeThread(HANDLE, PULONG). */
    case 225: return  8;  /* NtSetEvent(2) */
    case 226: return 20;  /* NtSetInformationFile(5) */
    case 228: return  8;  /* NtSetSystemTime(2) */
    case 235: return 24;  /* NtWaitForMultipleObjectsEx(6) -- Count, Handles,
                            * WaitType, WaitMode, Alertable, Timeout. Was
                            * declared as 5 args/20 bytes, missing WaitMode;
                            * left a permanent 4-byte stack leak on every
                            * call, which corrupted the caller's ebx/esi/edi
                            * (thread-local register globals restored via
                            * stack pops) for the rest of that thread's life.
                            * Ground-truthed against real x86 bytes at
                            * 0x00151C7D (6 pushes, no caller-side esp
                            * cleanup after the call -- callee must clean all
                            * 24 bytes itself). */
    case 233: return 12;  /* NtWaitForSingleObject(3) */
    case 236: return 32;  /* NtWriteFile(8) */
    case 238: return  0;  /* NtYieldExecution(void) */

    /* ── Object Manager ── */
    case 246: return 12;  /* ObReferenceObjectByHandle(3) - Xbox: Handle,Type,Object* */
    case 247: return 20;  /* ObReferenceObjectByName(5) */
    case 250: return  0;  /* ObfDereferenceObject (fastcall: arg in ecx) */

    /* ── Network / PHY ── */
    case 252: return  4;  /* PhyGetLinkState(1) */
    case 253: return  8;  /* PhyInitialize(2) */

    /* ── Threading ── */
    case 255: return 40;  /* PsCreateSystemThreadEx(10) */
    case  99: return 12;  /* KeDelayExecutionThread(3) */
    case 258: return  4;  /* PsTerminateSystemThread(1) */
    /* case 259: DATA export - PsThreadObjectType */

    /* ── Runtime Library ── */
    case 260: return 12;  /* RtlAnsiStringToUnicodeString(3) */
    case 269: return 12;  /* RtlCompareMemoryUlong(3) */
    case 277: return  4;  /* RtlEnterCriticalSection(1) */
    case 279: return 12;  /* RtlEqualString(3) */
    case 289: return  8;  /* RtlInitAnsiString(2) */
    case 291: return  4;  /* RtlInitializeCriticalSection(1) */
    case 294: return  4;  /* RtlLeaveCriticalSection(1) */
    case 301: return  4;  /* RtlNtStatusToDosError(1) */
    case 302: return  4;  /* RtlRaiseException(1) */
    case 304: return  8;  /* RtlTimeFieldsToTime(2) */
    case 305: return  8;  /* RtlTimeToTimeFields(2) */
    case 308: return 12;  /* RtlUnicodeStringToAnsiString(3) */
    case 312: return 16;  /* RtlUnwind(4) */
    case 352: return 12;  /* RtlRip(3) */

    /* ── Xbox Identity (data exports) ── */
    /* cases 321-326, 353-356: DATA exports */

    /* ── Port I/O ── */
    case 333: return 12;  /* WRITE_PORT_BUFFER_USHORT(3) */
    case 334: return 12;  /* WRITE_PORT_BUFFER_ULONG(3) */

    /* ── Crypto ── */
    case 335: return  4;  /* XcSHAInit(1) */
    case 336: return 12;  /* XcSHAUpdate(3) */
    case 337: return  8;  /* XcSHAFinal(2) */
    case 338: return 12;  /* XcRC4Key(3) */
    case 342: return 12;  /* XcPKDecPrivate(3) */
    case 343: return  4;  /* XcPKGetKeyLen(1) */
    case 344: return 12;  /* XcVerifyPKCS1Signature(3) */
    case 345: return 20;  /* XcModExp(5) */
    case 347: return 12;  /* XcKeyTable(3) */
    case 351: return  8;  /* XcUpdateCrypto(2) */


    /* Imported by the title but previously absent from this table, so they
     * fell through to `default: return 0` and leaked their arguments on the
     * simulated stack every call. See the block above bridge_for_ordinal. */
    case  69: return  4;  /* IoDeleteSymbolicLink(1) */
    case  74: return  8;  /* IoInvalidDeviceRequest(2) */
    case  87: return  0;  /* IofCompleteRequest(2) -- __fastcall, both args in ecx/edx */
    case  91: return  4;  /* IoDismountVolumeByName(1) */
    case 100: return  4;  /* KeDisconnectInterrupt(1) */
    case 205: return  8;  /* NtPulseEvent(2) */
    case 327: return  4;  /* XeLoadSection(1) */
    case 328: return  4;  /* XeUnloadSection(1) */
    case 360: return  0;  /* HalInitiateShutdown(void) */
    default:  return  0;  /* DATA exports or truly unknown */
    }
}

/* ============================================================================
 * Ordinals imported by the title but never wired into bridge_for_ordinal
 * ============================================================================
 *
 * Found by diffing the XBE's own kernel thunk table (114 imported ordinals,
 * decoded from the header's KernelImageThunkAddress) against the cases present
 * in bridge_for_ordinal. 43 imported ordinals had no bridge; 7 of those are
 * data exports handled by kernel_data_va_for_ordinal, leaving 36 functions
 * that silently did nothing and returned 0.
 *
 * 33 of the 36 already had a complete xbox_* implementation sitting in this
 * tree -- they had simply never been connected to the dispatch table. The
 * legacy kernel_thunks.c switch does map most of them to native function
 * pointers, which is presumably why they looked done, but that path is dead
 * code for recompiled titles: kernel_thunk_dispatch resolves calls solely
 * through bridge_for_ordinal.
 *
 * Two of these were doing real damage:
 *
 *   233 NtWaitForSingleObject -- a *blocking wait* that returned
 *       STATUS_SUCCESS immediately without waiting for anything. Any code
 *       that waited on an event before touching shared state proceeded
 *       straight through into data that was not ready yet.
 *   205 NtPulseEvent -- the matching signal side, equally absent.
 *
 * Together with the no-op critical sections fixed in kernel_rtl.c, the title
 * had no working synchronisation of any kind while running three real OS
 * threads.
 *
 * A third defect rode along: stdcall_args_for_ordinal ends in
 * `default: return 0`, so 15 of these ordinals also failed to clean their
 * arguments off the simulated stack -- a progressive esp leak on every call.
 * Entries for all of them are added alongside the bridges below.
 */

/* ── AvSetSavedDataAddress (ordinal 4, 1 arg) ─────────────── */
static void bridge_AvSetSavedDataAddress(void)
{
    xbox_AvSetSavedDataAddress(STACK_ARG(0));
    g_eax = 0;
}

/* ── DbgPrint (ordinal 8, __cdecl varargs) ─────────────────
 * Caller-cleaned, so 0 stdcall arg bytes is correct here -- not the accident
 * the missing table entry made it. The title's own debug output is worth
 * having, so the format string is walked and the variadic arguments are read
 * straight off the simulated stack rather than the string being dumped raw.
 * Only the conversions titles actually use are handled; anything else is
 * emitted verbatim so nothing is silently swallowed. */
static void bridge_DbgPrint(void)
{
    uint32_t fmt_va = STACK_ARG(0);
    const char *fmt;
    char out[1024];
    size_t o = 0;
    int argi = 1;              /* next variadic slot, in dwords */

    if (!fmt_va) {
        g_eax = 0;
        return;
    }
    fmt = (const char *)XBOX_TO_NATIVE(fmt_va);

    while (*fmt && o < sizeof(out) - 64) {
        if (*fmt != '%') {
            out[o++] = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') { out[o++] = '%'; fmt++; continue; }

        /* Skip flags, width and precision -- they change presentation only,
         * never how many stack slots the conversion consumes. */
        while (*fmt && strchr("-+ #0123456789.*lh", *fmt)) fmt++;

        switch (*fmt) {
        case 'd': case 'i':
            o += (size_t)snprintf(out + o, sizeof(out) - o, "%d",
                                  (int)STACK_ARG(argi++));
            break;
        case 'u':
            o += (size_t)snprintf(out + o, sizeof(out) - o, "%u",
                                  (unsigned)STACK_ARG(argi++));
            break;
        case 'x':
            o += (size_t)snprintf(out + o, sizeof(out) - o, "%x",
                                  (unsigned)STACK_ARG(argi++));
            break;
        case 'X':
            o += (size_t)snprintf(out + o, sizeof(out) - o, "%X",
                                  (unsigned)STACK_ARG(argi++));
            break;
        case 'p':
            o += (size_t)snprintf(out + o, sizeof(out) - o, "0x%08X",
                                  (unsigned)STACK_ARG(argi++));
            break;
        case 'c':
            out[o++] = (char)STACK_ARG(argi++);
            break;
        case 's': {
            uint32_t s_va = STACK_ARG(argi++);
            const char *s = s_va ? (const char *)XBOX_TO_NATIVE(s_va) : "(null)";
            o += (size_t)snprintf(out + o, sizeof(out) - o, "%.400s", s);
            break;
        }
        case 'f': case 'g': case 'e': {
            /* A double occupies two stack slots. */
            uint32_t lo = STACK_ARG(argi++);
            uint32_t hi = STACK_ARG(argi++);
            uint64_t bits = ((uint64_t)hi << 32) | lo;
            double   d;
            memcpy(&d, &bits, sizeof(d));
            o += (size_t)snprintf(out + o, sizeof(out) - o, "%g", d);
            break;
        }
        default:
            out[o++] = '%';
            if (*fmt) out[o++] = *fmt;
            break;
        }
        if (*fmt) fmt++;
    }
    out[o < sizeof(out) ? o : sizeof(out) - 1] = '\0';

    fprintf(stderr, "  [DbgPrint] %s", out);
    if (o == 0 || out[o - 1] != '\n') fprintf(stderr, "\n");
    fflush(stderr);
    g_eax = 0;
}

/* ── Io* device/IRP plumbing ──────────────────────────────── */
static void bridge_IoDeleteSymbolicLink(void)   /* 69, 1 arg */
{
    uint32_t name_va = STACK_ARG(0);
    g_eax = (uint32_t)xbox_IoDeleteSymbolicLink(
        (PXBOX_ANSI_STRING)XBOX_TO_NATIVE(name_va));
}

static void bridge_IoInvalidDeviceRequest(void) /* 74, 2 args */
{
    g_eax = (uint32_t)xbox_IoInvalidDeviceRequest(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)));
}

static void bridge_IoStartNextPacket(void)      /* 81, 2 args */
{
    xbox_IoStartNextPacket(XBOX_TO_NATIVE(STACK_ARG(0)),
                           (BOOLEAN)STACK_ARG(1));
    g_eax = 0;
}

static void bridge_IoStartPacket(void)          /* 83, 4 args */
{
    xbox_IoStartPacket(XBOX_TO_NATIVE(STACK_ARG(0)),
                       XBOX_TO_NATIVE(STACK_ARG(1)),
                       (PULONG)XBOX_TO_NATIVE(STACK_ARG(2)),
                       XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

/* ── IofCompleteRequest (ordinal 87, __fastcall: Irp in ecx, boost in edx) ──
 * 0 stdcall arg bytes is correct: both arguments arrive in registers. */
static void bridge_IofCompleteRequest(void)
{
    xbox_IofCompleteRequest(XBOX_TO_NATIVE(g_ecx), (CCHAR)g_edx);
    g_eax = 0;
}

/* ── IoDismountVolumeByName (ordinal 91, 1 arg) ────────────
 * No xbox_* implementation exists because there is nothing to dismount: every
 * Xbox volume this layer exposes is either a host directory or the mounted
 * XDVDFS image, neither of which has a dismount concept. Reporting success is
 * the honest answer to "make sure this volume is no longer mounted". */
static void bridge_IoDismountVolumeByName(void)
{
    g_eax = (uint32_t)STATUS_SUCCESS;
}

/* ── Ke* ───────────────────────────────────────────────────── */
static void bridge_KeBugCheck(void)             /* 95, 1 arg */
{
    xbox_KeBugCheck(STACK_ARG(0));
    g_eax = 0;
}

static void bridge_KeCancelTimer(void)          /* 97, 1 arg */
{
    /* The XBOX_KTIMER we manage is a HOST object (native HANDLEs, host
     * pointer widths) held in the VA->object side map -- it is not the
     * guest's KTIMER layout. Casting the guest VA straight to PXBOX_KTIMER,
     * as this used to do, made xbox_KeCancelTimer store `win32_timer = NULL`
     * (an 8-byte host pointer) and `Inserted = FALSE` directly into guest
     * RAM, smashing whatever the title kept there -- confirmed live to
     * corrupt CRT pool block headers. Resolve through the same side map
     * every other timer bridge uses, and treat an unknown VA as "was not
     * inserted" rather than inventing an object for it. */
    PXBOX_KTIMER t;
    AcquireSRWLockExclusive(&g_ktimer_lock);
    t = bridge_ktimer_for_va(STACK_ARG(0));
    g_eax = (t && xbox_KeCancelTimer(t)) ? 1u : 0u;
    ReleaseSRWLockExclusive(&g_ktimer_lock);
}

static void bridge_KeDisconnectInterrupt(void)  /* 100, 1 arg */
{
    g_eax = xbox_KeDisconnectInterrupt(
        (PXBOX_KINTERRUPT)XBOX_TO_NATIVE(STACK_ARG(0))) ? 1u : 0u;
}

static void bridge_KeRemoveQueueDpc(void)       /* 137, 1 arg */
{
    g_eax = xbox_KeRemoveQueueDpc((PXBOX_KDPC)XBOX_TO_NATIVE(STACK_ARG(0)))
          ? 1u : 0u;
}

static void bridge_KeRestoreFloatingPointState(void)  /* 139, 1 arg */
{
    g_eax = (uint32_t)xbox_KeRestoreFloatingPointState(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_KeSaveFloatingPointState(void)     /* 142, 1 arg */
{
    g_eax = (uint32_t)xbox_KeSaveFloatingPointState(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_KeSynchronizeExecution(void)       /* 153, 3 args */
{
    g_eax = xbox_KeSynchronizeExecution(
        (PXBOX_KINTERRUPT)XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2))) ? 1u : 0u;
}

/* ── Mm* ───────────────────────────────────────────────────── */
static void bridge_MmClaimGpuInstanceMemory(void)     /* 168, 2 args */
{
    PVOID p = xbox_MmClaimGpuInstanceMemory(
        STACK_ARG(0), (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = p ? (uint32_t)((uintptr_t)p - g_xbox_mem_offset) : 0u;
}

static void bridge_MmLockUnlockPhysicalPage(void)     /* 176, 2 args */
{
    xbox_MmLockUnlockPhysicalPage((ULONG_PTR)STACK_ARG(0),
                                  (BOOLEAN)STACK_ARG(1));
    g_eax = 0;
}

static void bridge_MmQueryAddressProtect(void)        /* 179, 1 arg */
{
    g_eax = xbox_MmQueryAddressProtect(XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_MmQueryAllocationSize(void)        /* 180, 1 arg */
{
    /* The size of the allocation, page-rounded as the Xbox kernel reports it.
     *
     * This used to ask VirtualQuery on the host pointer, whose RegionSize is
     * everything from that address to the end of the host mapping -- 87 MB for
     * a 4 KB buffer. DirectSound sizes its APU scatter-gather mapping from this
     * value during DirectSoundCreate, so an 87 MB "buffer" came back as
     * E_OUTOFMEMORY three times at boot, and its freed-bytes counter went
     * negative. The heap ledger has the real extent. */
    uint32_t va = xbox_fold_ram_alias(STACK_ARG(0)), base = 0, size = 0;
    if (va && xbox_heap_owner_of(va, &base, &size, NULL, NULL, 0, NULL))
        g_eax = (size + 0xFFFu) & ~0xFFFu;
    else
        g_eax = 0;
}

/* ── NtPulseEvent (ordinal 205, 2 args) ────────────────────
 * NTSTATUS NtPulseEvent(HANDLE EventHandle, PLONG PreviousState)
 * Releases every thread currently waiting and leaves the event unsignalled.
 * No xbox_* wrapper existed; the Win32 primitive has exactly these semantics.
 * Handles are resolved the same dual way bridge_NtWaitForMultipleObjectsEx
 * resolves them: a tagged token through the handle table, anything else as an
 * Xbox-VA dispatcher object, so a pulse and a wait on the same object meet on
 * one native event. */
static void bridge_NtPulseEvent(void)
{
    uint32_t raw      = STACK_ARG(0);
    uint32_t prev_va  = STACK_ARG(1);
    HANDLE   h        = ((raw & 0xFF000000u) == BRIDGE_HANDLE_TAG)
                      ? bridge_read_handle(raw)
                      : xbox_resolve_dispatcher_handle(raw);

    if (!h) {
        g_eax = (uint32_t)STATUS_INVALID_HANDLE;
        return;
    }
    if (prev_va) {
        BRIDGE_MEM32(prev_va) = 0;   /* pulse always leaves it non-signalled */
    }
    g_eax = PulseEvent(h) ? (uint32_t)STATUS_SUCCESS
                          : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* ── Nt* queries ───────────────────────────────────────────── */
static void bridge_NtQueryVirtualMemory(void)         /* 217, 4 args */
{
    g_eax = (uint32_t)xbox_NtQueryVirtualMemory(
        XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)),
        STACK_ARG(2),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)));
}

/* ── NtWaitForSingleObject (ordinal 233, 3 args) ───────────
 * NTSTATUS NtWaitForSingleObject(HANDLE, BOOLEAN Alertable, PLARGE_INTEGER)
 *
 * The single most damaging gap in this list: with no bridge, every call
 * returned STATUS_SUCCESS *without waiting*, so any guest code that gated on
 * an event before reading shared state ran straight on into data that was not
 * ready. Uses the dual handle resolution (tagged token, else Xbox-VA
 * dispatcher object) so a wait here and a KeSetEvent elsewhere share one
 * native event -- the same correction already applied to
 * bridge_NtWaitForMultipleObjectsEx. */
static void bridge_NtWaitForSingleObject(void)
{
    uint32_t raw        = STACK_ARG(0);
    uint32_t alertable  = STACK_ARG(1);
    uint32_t timeout_va = STACK_ARG(2);
    HANDLE   handle     = ((raw & 0xFF000000u) == BRIDGE_HANDLE_TAG)
                        ? bridge_read_handle(raw)
                        : xbox_resolve_dispatcher_handle(raw);
    LARGE_INTEGER  to;
    PLARGE_INTEGER pto = NULL;

    if (timeout_va) {
        to.LowPart  = BRIDGE_MEM32(timeout_va);
        to.HighPart = (LONG)BRIDGE_MEM32(timeout_va + 4);
        pto = &to;
    }

    g_eax = (uint32_t)xbox_NtWaitForSingleObject(handle, (BOOLEAN)alertable, pto);
}

/* ── Rtl* ──────────────────────────────────────────────────── */
static void bridge_RtlCompareMemoryUlong(void)        /* 269, 3 args */
{
    g_eax = xbox_RtlCompareMemoryUlong(XBOX_TO_NATIVE(STACK_ARG(0)),
                                       STACK_ARG(1), STACK_ARG(2));
}

static void bridge_RtlTimeFieldsToTime(void)          /* 304, 2 args */
{
    g_eax = xbox_RtlTimeFieldsToTime(
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(1))) ? 1u : 0u;
}

static void bridge_RtlUnwind(void)                    /* 312, 4 args */
{
    xbox_RtlUnwind(XBOX_TO_NATIVE(STACK_ARG(0)),
                   XBOX_TO_NATIVE(STACK_ARG(1)),
                   XBOX_TO_NATIVE(STACK_ARG(2)),
                   XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

/* ── Xe* section loading (327/328, 1 arg each) ────────────── */
static void bridge_XeLoadSection(void)
{
    g_eax = (uint32_t)xbox_XeLoadSection(
        (PXBE_SECTION_HEADER)XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_XeUnloadSection(void)
{
    g_eax = (uint32_t)xbox_XeUnloadSection(
        (PXBE_SECTION_HEADER)XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* ── Xc* SHA-1 (335/336/337) ──────────────────────────────── */
static void bridge_XcSHAInit(void)
{
    xbox_XcSHAInit((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_XcSHAUpdate(void)
{
    xbox_XcSHAUpdate((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                     (const UCHAR *)XBOX_TO_NATIVE(STACK_ARG(1)),
                     STACK_ARG(2));
    g_eax = 0;
}

static void bridge_XcSHAFinal(void)
{
    xbox_XcSHAFinal((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    (UCHAR *)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* Hal and Io shutdown plumbing (358/359/360) */
static void bridge_HalIsResetOrShutdownPending(void)  /* 358, 0 args */
{
    g_eax = xbox_HalIsResetOrShutdownPending() ? 1u : 0u;
}

static void bridge_IoMarkIrpMustComplete(void)        /* 359, 1 arg */
{
    xbox_IoMarkIrpMustComplete(XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_HalInitiateShutdown(void)          /* 360, 0 args */
{
    xbox_HalInitiateShutdown();
    g_eax = 0;
}

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* Threading */
    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;

    /* File/Handle */
    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 195: return bridge_NtDeleteFile;
    case 196: return bridge_NtDeviceIoControlFile;
    case 198: return bridge_NtFlushBuffersFile;
    case 200: return bridge_NtFsControlFile;
    case 202: return bridge_NtOpenFile;
    case 203: return bridge_NtOpenSymbolicLinkObject;
    case 207: return bridge_NtQueryDirectoryFile;
    case 210: return bridge_NtQueryFullAttributesFile;
    case 211: return bridge_NtQueryInformationFile;
    case 218: return bridge_NtQueryVolumeInformationFile;
    case 219: return bridge_NtReadFile;
    case 226: return bridge_NtSetInformationFile;
    case 236: return bridge_NtWriteFile;

    /* Memory - contiguous */
    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 171: return bridge_MmFreeContiguousMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;

    /* Memory - virtual */
    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;

    /* Pool */
    case  14: return bridge_ExAllocatePool;
    case  15: return bridge_ExAllocatePoolWithTag;
    case  17: return bridge_ExFreePool;
    case  23: return bridge_ExQueryPoolBlockSize;
    case  24: return bridge_ExQueryNonVolatileSetting;

    /* IRQL */
    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    /* Critical sections */
    case 291: return bridge_RtlInitializeCriticalSection;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    /* Previously declared-but-unwired (see the block of bridges above) --
     * every one of these already had a working HLE implementation and an
     * arg-size entry, but no case here, so it silently returned 0. */
    case 289: return bridge_RtlInitAnsiString;
    case 279: return bridge_RtlEqualString;
    case 305: return bridge_RtlTimeToTimeFields;
    case  99: return bridge_KeDelayExecutionThread;
    case 151: return bridge_KeStallExecutionProcessor;
    case 143: return bridge_KeSetBasePriorityThread;
    case 175: return bridge_MmLockUnlockBufferPages;
    case 250: return bridge_ObfDereferenceObject;
    case   1: return bridge_AvGetSavedDataAddress;
    case   2: return bridge_AvSendTVEncoderOption;
    case 224: return bridge_NtResumeThread;

    /* Timing */
    case 125: return bridge_KeQueryInterruptTime;
    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimer;  /* KeSetTimerEx */

    /* DPC / Timer init */
    case 107: return bridge_KeInitializeDpc;
    case 113: return bridge_KeInitializeTimerEx;
    case 119: return bridge_KeInsertQueueDpc;

    /* Synchronization */
    case 186: return bridge_NtClearEvent;
    case 189: return bridge_NtCreateEvent;
    case 225: return bridge_NtSetEvent;
    case 234: return bridge_NtWaitForSingleObjectEx;
    /* ordinal 235 (NtWaitForMultipleObjectsEx): re-enabled -- see the comment
     * on bridge_NtWaitForMultipleObjectsEx above. Previously left unbridged
     * because the FILESYS queue was driven synchronously with nothing to
     * ever signal completion events; now that sub_0014D850 (the real async
     * I/O worker-thread completion loop) is
     * correctly translated, this precondition may no longer hold. */
    case 235: return bridge_NtWaitForMultipleObjectsEx;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case 158: return bridge_KeWaitForMultipleObjects;
    case 238: return bridge_NtYieldExecution;

    /* Hardware */
    case   9: return bridge_HalReadSMCTrayState;
    case  47: return bridge_HalRegisterShutdownNotification;
    case  48: return bridge_HalRequestSoftwareInterrupt;
    case  49: return bridge_HalReturnToFirmware;
    case  44: return bridge_HalGetInterruptVector;

    /* Interrupts */
    case 109: return bridge_KeInitializeInterrupt;
    case  98: return bridge_KeConnectInterrupt;

    /* Display */
    case   3: return bridge_AvSetDisplayMode;

    /* I/O */
    case  65: return bridge_IoCreateDevice;
    case  67: return bridge_IoCreateSymbolicLink;
    case  66: return bridge_IoCreateFile;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    /* Memory - I/O mapping */
    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    /* RTL */
    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;


    /* Ordinals the XBE imports that had no bridge at first.
     * 233 (NtWaitForSingleObject) and 205 (NtPulseEvent) are the two that
     * were actively breaking synchronisation; the rest returned 0. */
    case   4: return bridge_AvSetSavedDataAddress;
    case   8: return bridge_DbgPrint;
    case  69: return bridge_IoDeleteSymbolicLink;
    case  74: return bridge_IoInvalidDeviceRequest;
    case  81: return bridge_IoStartNextPacket;
    case  83: return bridge_IoStartPacket;
    case  87: return bridge_IofCompleteRequest;
    case  91: return bridge_IoDismountVolumeByName;
    case  95: return bridge_KeBugCheck;
    case  97: return bridge_KeCancelTimer;
    case 100: return bridge_KeDisconnectInterrupt;
    case 137: return bridge_KeRemoveQueueDpc;
    case 139: return bridge_KeRestoreFloatingPointState;
    case 142: return bridge_KeSaveFloatingPointState;
    case 153: return bridge_KeSynchronizeExecution;
    case 168: return bridge_MmClaimGpuInstanceMemory;
    case 176: return bridge_MmLockUnlockPhysicalPage;
    case 179: return bridge_MmQueryAddressProtect;
    case 180: return bridge_MmQueryAllocationSize;
    case 205: return bridge_NtPulseEvent;
    case 215: return bridge_NtQuerySymbolicLinkObject;
    case 217: return bridge_NtQueryVirtualMemory;
    case 233: return bridge_NtWaitForSingleObject;
    case 269: return bridge_RtlCompareMemoryUlong;
    case 304: return bridge_RtlTimeFieldsToTime;
    case 312: return bridge_RtlUnwind;
    case 327: return bridge_XeLoadSection;
    case 328: return bridge_XeUnloadSection;
    case 335: return bridge_XcSHAInit;
    case 336: return bridge_XcSHAUpdate;
    case 337: return bridge_XcSHAFinal;
    case 358: return bridge_HalIsResetOrShutdownPending;
    case 359: return bridge_IoMarkIrpMustComplete;
    case 360: return bridge_HalInitiateShutdown;

    default:  return NULL;
    }
}

/* ── Per-slot bridge functions (resolved at init) ────────── */

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];


/* ══════════════════════════════════════════════════════════════════════
 * Kernel call histogram
 *
 * Generated from the arg-size table in this same file, so a bridge that
 * knows an ordinal's signature also knows its name.
 * ══════════════════════════════════════════════════════════════════════ */
#define KERNEL_ORD_SLOTS 512
static unsigned int g_ord_hist[KERNEL_ORD_SLOTS];
static unsigned int g_ord_prev[KERNEL_ORD_SLOTS];

static const char *bridge_ordinal_name(unsigned int ord)
{
    switch (ord) {
    case   1: return "AvGetSavedDataAddress";
    case   2: return "AvSendTVEncoderOption";
    case   3: return "AvSetDisplayMode";
    case   4: return "AvSetSavedDataAddress";
    case   8: return "DbgPrint";
    case   9: return "HalReadSMCTrayState";
    case  14: return "ExAllocatePool";
    case  15: return "ExAllocatePoolWithTag";
    case  17: return "ExFreePool";
    case  23: return "ExQueryPoolBlockSize";
    case  24: return "ExQueryNonVolatileSetting";
    case  38: return "HalClearSoftwareInterrupt";
    case  39: return "HalDisableSystemInterrupt";
    case  46: return "HalReadWritePCISpace";
    case  47: return "HalRegisterShutdownNotification";
    case  48: return "HalRequestSoftwareInterrupt";
    case  49: return "HalReturnToFirmware";
    case  61: return "IoBuildDeviceIoControlRequest";
    case  65: return "IoCreateDevice";
    case  66: return "IoCreateFile";
    case  67: return "IoCreateSymbolicLink";
    case  68: return "IoDeleteDevice";
    case  69: return "IoDeleteSymbolicLink";
    case  73: return "IoInitializeIrp";
    case  74: return "IoInvalidDeviceRequest";
    case  79: return "IoSetIoCompletion";
    case  81: return "IoStartNextPacket";
    case  82: return "IoStartNextPacketByKey";
    case  83: return "IoStartPacket";
    case  84: return "IoSynchronousDeviceIoControlRequest";
    case  85: return "IoSynchronousFsdRequest";
    case  87: return "IofCompleteRequest";
    case  91: return "IoDismountVolumeByName";
    case  93: return "KeAlertThread";
    case  95: return "KeBugCheck";
    case  96: return "KeBugCheckEx";
    case  97: return "KeCancelTimer";
    case  98: return "KeConnectInterrupt";
    case  99: return "KeDelayExecutionThread";
    case 100: return "KeDisconnectInterrupt";
    case 107: return "KeInitializeDpc";
    case 109: return "KeInitializeInterrupt";
    case 113: return "KeInitializeTimerEx";
    case 119: return "KeInsertQueueDpc";
    case 124: return "KeQueryBasePriorityThread";
    case 125: return "KeQueryInterruptTime";
    case 126: return "KeQueryPerformanceCounter";
    case 127: return "KeQueryPerformanceFrequency";
    case 128: return "KeQuerySystemTime";
    case 129: return "KeRaiseIrqlToDpcLevel";
    case 137: return "KeRemoveQueueDpc";
    case 139: return "KeRestoreFloatingPointState";
    case 142: return "KeSaveFloatingPointState";
    case 143: return "KeSetBasePriorityThread";
    case 145: return "KeSetEvent";
    case 149: return "KeSetTimer";
    case 150: return "KeSetTimerEx";
    case 151: return "KeStallExecutionProcessor";
    case 153: return "KeSynchronizeExecution";
    case 158: return "KeWaitForMultipleObjects";
    case 159: return "KeWaitForSingleObject";
    case 160: return "KfRaiseIrql";
    case 161: return "KfLowerIrql";
    case 165: return "MmAllocateContiguousMemory";
    case 166: return "MmAllocateContiguousMemoryEx";
    case 168: return "MmClaimGpuInstanceMemory";
    case 169: return "MmCreateKernelStack";
    case 170: return "MmDeleteKernelStack";
    case 171: return "MmFreeContiguousMemory";
    case 173: return "MmGetPhysicalAddress";
    case 175: return "MmLockUnlockBufferPages";
    case 176: return "MmLockUnlockPhysicalPage";
    case 177: return "MmMapIoSpace";
    case 178: return "MmPersistContiguousMemory";
    case 179: return "MmQueryAddressProtect";
    case 180: return "MmQueryAllocationSize";
    case 181: return "MmQueryStatistics";
    case 182: return "MmSetAddressProtect";
    case 184: return "NtAllocateVirtualMemory";
    case 186: return "NtClearEvent";
    case 187: return "NtClose";
    case 188: return "NtCreateDirectoryObject";
    case 189: return "NtCreateEvent";
    case 190: return "NtCreateFile";
    case 193: return "NtCreateSemaphore";
    case 195: return "NtDeleteFile";
    case 196: return "NtDeviceIoControlFile";
    case 197: return "NtDuplicateObject";
    case 198: return "NtFlushBuffersFile";
    case 199: return "NtFreeVirtualMemory";
    case 200: return "NtFsControlFile";
    case 202: return "NtOpenFile";
    case 203: return "NtOpenSymbolicLinkObject";
    case 205: return "NtPulseEvent";
    case 207: return "NtQueryDirectoryFile";
    case 210: return "NtQueryFullAttributesFile";
    case 211: return "NtQueryInformationFile";
    case 215: return "NtQuerySymbolicLinkObject";
    case 217: return "NtQueryVirtualMemory";
    case 218: return "NtQueryVolumeInformationFile";
    case 219: return "NtReadFile";
    case 222: return "NtReleaseSemaphore";
    case 224: return "NtResumeThread";
    case 225: return "NtSetEvent";
    case 226: return "NtSetInformationFile";
    case 228: return "NtSetSystemTime";
    case 233: return "NtWaitForSingleObject";
    case 234: return "NtWaitForSingleObjectEx";
    case 235: return "NtWaitForMultipleObjectsEx";
    case 236: return "NtWriteFile";
    case 238: return "NtYieldExecution";
    case 246: return "ObReferenceObjectByHandle";
    case 247: return "ObReferenceObjectByName";
    case 250: return "ObfDereferenceObject";
    case 252: return "PhyGetLinkState";
    case 253: return "PhyInitialize";
    case 255: return "PsCreateSystemThreadEx";
    case 258: return "PsTerminateSystemThread";
    case 260: return "RtlAnsiStringToUnicodeString";
    case 269: return "RtlCompareMemoryUlong";
    case 277: return "RtlEnterCriticalSection";
    case 279: return "RtlEqualString";
    case 289: return "RtlInitAnsiString";
    case 291: return "RtlInitializeCriticalSection";
    case 294: return "RtlLeaveCriticalSection";
    case 301: return "RtlNtStatusToDosError";
    case 302: return "RtlRaiseException";
    case 304: return "RtlTimeFieldsToTime";
    case 305: return "RtlTimeToTimeFields";
    case 308: return "RtlUnicodeStringToAnsiString";
    case 312: return "RtlUnwind";
    case 327: return "XeLoadSection";
    case 328: return "XeUnloadSection";
    case 333: return "WRITE_PORT_BUFFER_USHORT";
    case 334: return "WRITE_PORT_BUFFER_ULONG";
    case 335: return "XcSHAInit";
    case 336: return "XcSHAUpdate";
    case 337: return "XcSHAFinal";
    case 338: return "XcRC4Key";
    case 342: return "XcPKDecPrivate";
    case 343: return "XcPKGetKeyLen";
    case 344: return "XcVerifyPKCS1Signature";
    case 345: return "XcModExp";
    case 347: return "XcKeyTable";
    case 351: return "XcUpdateCrypto";
    case 352: return "RtlRip";
    case 358: return "HalIsResetOrShutdownPending";
    case 359: return "IoMarkIrpMustComplete";
    case 360: return "HalInitiateShutdown";
    default: return "?";
    }
}

/*
 * Print the busiest ordinals since the previous report. The delta is the
 * useful column: a steady main loop spreads its calls over many ordinals,
 * whereas a title stuck in a poll spends nearly all of them on one.
 */
static void bridge_report_ordinals(void)
{
    unsigned int i, top[10], shown = 0;
    unsigned int delta[KERNEL_ORD_SLOTS];
    unsigned long long total_delta = 0;

    for (i = 0; i < 10; i++) top[i] = 0;
    for (i = 0; i < KERNEL_ORD_SLOTS; i++) {
        delta[i] = g_ord_hist[i] - g_ord_prev[i];
        g_ord_prev[i] = g_ord_hist[i];
        total_delta += delta[i];
    }
    if (total_delta == 0) {
        fprintf(stderr, "  [KERNEL] no kernel calls since the last report\n");
        fflush(stderr);
        return;
    }
    for (i = 0; i < KERNEL_ORD_SLOTS; i++) {
        unsigned int j, k;
        if (!delta[i]) continue;
        for (j = 0; j < 10; j++) {
            if (delta[i] > delta[top[j]]) {
                for (k = 9; k > j; k--) top[k] = top[k - 1];
                top[j] = i;
                break;
            }
        }
    }
    fprintf(stderr, "  [KERNEL] %llu calls since last report; busiest:\n", total_delta);
    for (i = 0; i < 10 && delta[top[i]]; i++) {
        fprintf(stderr, "  [KERNEL]   %5u x  ordinal %3u  %s  (%.0f%%)\n",
                delta[top[i]], top[i], bridge_ordinal_name(top[i]),
                100.0 * (double)delta[top[i]] / (double)total_delta);
        shown++;
    }
    (void)shown;
    fflush(stderr);
}

/*
 * Apply the XBE TLS index once, on the first bridged kernel call.
 *
 * The header's TLS directory names a guest variable (AddressOfIndex) that the
 * loader must fill with the allocated TLS slot. Doing it at load time does not
 * stick: the guest's own .data initialisation runs afterwards inside
 * xbe_entry_point and copies the image's compiled-in 0xFFFFFFFB back over it
 * (confirmed with a write-watch -- exactly one writer, from xbe_entry_point).
 *
 * Left at -5, every CRT TLS accessor indexes the array negatively:
 *     ecx = fs:[4]; eax = ds:[index]; eax = [ecx + eax*4]; [eax + 4] = value
 * which reads before the array, gets 0, and writes to Xbox VA 4 -- landing on
 * the TIB through the low-address redirect and smearing the KPCR. That is what
 * corrupted fs:[4]/fs:[8] and, downstream, stopped the circular-list terminator
 * sub_000A3890 ever reporting end-of-list.
 *
 * The first bridged call is the earliest point that is reliably after the
 * guest's data init, so the index is applied here. This title has one TLS
 * directory, so slot 0 is correct.
 */
static void xbox_apply_tls_index_once(void)
{
    static int done = 0;
    uint32_t tls_dir, idx_va;
    /* Latch only once the value is actually corrected: the first bridged
     * call can precede the guest's .data init, in which case the index still
     * holds the loader's value and there is nothing to fix yet. */
    if (done) return;
    tls_dir = BRIDGE_MEM32(0x00010000u + 0x12C);
    if (!tls_dir) return;
    idx_va = BRIDGE_MEM32(tls_dir + 8);          /* AddressOfIndex */
    if (!idx_va) return;
    if (BRIDGE_MEM32(idx_va) == 0xFFFFFFFBu || (int32_t)BRIDGE_MEM32(idx_va) < 0) {
        BRIDGE_MEM32(idx_va) = 0;
        done = 1;
        fprintf(stderr, "  [XBE] TLS index applied: VA 0x%08X = 0\n", idx_va);
        fflush(stderr);
    }
}

/* XBOX_TRAP_NAN_FILE=<text> (diagnostic): once the title opens a
 * file whose path contains <text>, every guest thread unmasks the SSE
 * invalid-operation exception at its next kernel call. The translated code
 * does its float math in SSE, so the first operation that manufactures a NaN
 * (0/0, inf-inf, sqrt of a negative) faults on the spot and the crash reporter
 * names the generated line -- for the race riders' NaN state, which watches on
 * shifting heap addresses could not pin down. */
static volatile LONG g_trap_nan_armed = 0;
static __thread int  t_trap_nan_on = 0;

/* Report an invalid-operation trap with a native backtrace, then mask the
 * exception again in the faulting context and retry: the operation produces
 * its NaN as it would have, and the run continues to the next report. */
static LONG CALLBACK trap_nan_veh(EXCEPTION_POINTERS *ep)
{
    static volatile LONG reports = 0;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != 0xC0000090u && code != 0xC00002B5u)   /* FLOAT_INVALID / MULTIPLE_TRAPS */
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&reports) <= 8) {
        void *fr[20];
        USHORT n = CaptureStackBackTrace(0, 20, fr, NULL), k;
        uintptr_t base = (uintptr_t)GetModuleHandle(NULL);
        char buf[512];
        int p = snprintf(buf, sizeof buf, "  [TRAPNAN] invalid op #%ld at 0x%llX, frames:",
                         (long)reports, (unsigned long long)(0x140000000ULL +
                         ((uintptr_t)ep->ExceptionRecord->ExceptionAddress - base)));
        for (k = 0; k < n && p < (int)sizeof buf - 24; k++)
            p += snprintf(buf + p, sizeof buf - (size_t)p, " 0x%llX",
                          (unsigned long long)(0x140000000ULL + ((uintptr_t)fr[k] - base)));
        fprintf(stderr, "%s\n", buf);
        fflush(stderr);
    }
    ep->ContextRecord->MxCsr |= 0x0080u;          /* mask IM, retry */
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void trap_nan_check_path(const char *path)
{
    static const char *want = (const char *)-1;
    if (want == (const char *)-1) want = getenv("XBOX_TRAP_NAN_FILE");
    if (want && *want && path && strstr(path, want) && !g_trap_nan_armed) {
        AddVectoredExceptionHandler(1, trap_nan_veh);
        InterlockedExchange(&g_trap_nan_armed, 1);
        fprintf(stderr, "  [TRAPNAN] armed on %s\n", path);
        fflush(stderr);
    }
}

static void kernel_thunk_dispatch(void)
{
    if (g_trap_nan_armed && !t_trap_nan_on) {
        t_trap_nan_on = 1;
        _mm_setcsr(_mm_getcsr() & ~0x0080u);   /* unmask invalid (IM) */
    }
    xbox_apply_tls_index_once();
    /* Sample the push buffer on the guest's own thread while the translator is
     * still coming up. The 1 ms pump cannot see D3D init -- the title emits its
     * one-time texture and vertex-format state and recycles the ring inside a
     * single tick -- and the translator cannot attach that early regardless,
     * since it needs a D3D11 device that init has not finished creating. Here we
     * are interleaved with that init at kernel-call granularity, so nothing is
     * missed. The call returns immediately once the translator is live. */
    nv2a_live_pb_capture((uint8_t *)g_xbox_mem_offset);

    int slot = g_kernel_dispatch_slot;
    bridge_func_t bridge;
    ULONG ordinal;

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;  /* pop dummy return address */
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];

    /*
     * Runaway-guest-stack detector.
     *
     * A native stack overflow leaves no usable backtrace -- by the time the
     * VEH handler runs the stack is gone and only a frame or two survive in
     * module range. Catching the runaway *early* is what identified the CRT
     * lock recursion: every kernel call passes through here,
     * so watching the guest esp costs one compare, fires long before the guard
     * page, and still has a complete native stack to capture.
     *
     * The threshold is 128 KB below XBOX_STACK_BASE's top. Measured across
     * three clean 45-second runs: never reached, so it is a genuine runaway
     * signal rather than a depth warning. Prints once per process. Derived
     * from the layout macros, not written out: as literals (0x00F60000 and
     * 0x00F80000) these silently described the wrong stack the moment the
     * stack moved.
     */
    {
        static LONG shouted = 0;
        if (g_esp && g_esp < XBOX_STACK_BASE + 128u * 1024u &&
            InterlockedExchange(&shouted, 1) == 0) {
            void *fr[40];
            USHORT k = CaptureStackBackTrace(0, 40, fr, NULL);
            uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
            USHORT i;
            fprintf(stderr, "[DEEPSTACK] guest esp=0x%08X (%u bytes used) at ordinal %u; frames:\n",
                    g_esp, (unsigned)(XBOX_STACK_TOP - g_esp), ordinal);
            for (i = 0; i < k; i++) {
                uintptr_t f = (uintptr_t)fr[i];
                if (f >= base && f < base + 0x08000000ULL)
                    fprintf(stderr, "    0x%llX\n",
                            (unsigned long long)(0x140000000ULL + (f - base)));
            }
            fflush(stderr);
        }
    }

    /* A guest esp outside RAM on ANY thread. The check above only covers the
     * main thread's stack; a thread was once found running with esp at the
     * top of the APU aperture (0xFE87FFFC), so every push became an APU
     * register write and the emulator was fed stack data as FE methods.
     * Every thread passes through here often (the timer thread waits every
     * frame), so this names the path within a frame of the damage. */
    {
        static LONG reported = 0;
        if ((g_esp >= 0x08000000u || g_esp < 0x00010000u) &&
            InterlockedExchange(&reported, 1) == 0) {
            extern volatile unsigned g_last_loc;
            void *fr[40];
            USHORT k = CaptureStackBackTrace(0, 40, fr, NULL);
            uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
            USHORT i;
            fprintf(stderr, "[BADSTACK] guest esp=0x%08X on host tid %lu at ordinal %u, "
                    "last label loc_%08X; frames:\n", g_esp, GetCurrentThreadId(),
                    ordinal, g_last_loc);
            for (i = 0; i < k; i++) {
                uintptr_t f = (uintptr_t)fr[i];
                if (f >= base && f < base + 0x08000000ULL)
                    fprintf(stderr, "    0x%llX\n",
                            (unsigned long long)(0x140000000ULL + (f - base)));
            }
            fflush(stderr);
        }
    }

    g_kernel_call_count++;
    if ((unsigned)ordinal < KERNEL_ORD_SLOTS) g_ord_hist[ordinal]++;

    if ((long)g_kernel_call_count <= kernel_call_log_limit()) {
        fprintf(stderr, "  [KERNEL] #%d: ordinal %u (slot %d) esp=0x%08X\n",
                g_kernel_call_count, ordinal, slot, g_esp);
        fflush(stderr);
    }

    {
        static DWORD last_summary_tick = 0;
        DWORD now = GetTickCount();
        if (last_summary_tick == 0) last_summary_tick = now;
        if (now - last_summary_tick >= 3000 && g_kernel_call_count > 200) {
            fprintf(stderr, "  [KERNEL] summary: %d total calls, guest esp=0x%08X\n",
                    g_kernel_call_count, g_esp);
            bridge_report_ordinals();
            {   /* The simulated x87 top is now shared between lifted fragments.
                 * Real code keeps it inside an eight-register window; unbounded
                 * drift means some translation pushes without popping, which the
                 * old per-function stack used to hide by resetting each call. */
                extern __thread int g_fp_top;
                static int first = 1, base_top = 0;
                if (first) { first = 0; base_top = g_fp_top; }
                fprintf(stderr, "  [KERNEL] x87 top = %d (drift %+d since first report)\n",
                        g_fp_top, g_fp_top - base_top);
            }
            last_summary_tick = now;
        }
    }

    /* Pop the dummy return address that PUSH32(esp, 0) pushed before RECOMP_ICALL.
     * On real x86, "call [thunk]" pushes a real return address and "ret" pops it.
     * In our model, the bridge is called directly (not via the simulated stack),
     * so we must manually consume the dummy return address. */
    g_esp += 4;

    if (bridge) {
        if (g_perf_on) {                /* kernel time of the game thread */
            double t0 = perf_now();
            bridge();
            perf_kernel(ordinal, perf_now() - t0);
        } else
            bridge();
    } else {
        /* No specific bridge - return 0. Warn once per ordinal rather than
         * gating on g_kernel_call_count: a missing bridge is rare and is
         * usually the reason a game misbehaves, so it must not be swallowed
         * by the general call-trace throttle. Bounded to one line per slot. */
        static uint8_t warned[XBOX_KERNEL_THUNK_TABLE_SIZE];
        if (!warned[slot]) {
            warned[slot] = 1;
            fprintf(stderr, "  [KERNEL] WARNING: no bridge for ordinal %u (slot %d), returning 0\n",
                    ordinal, slot);
            fflush(stderr);
        }
        g_eax = 0;
    }

    /* Clean stdcall args from the simulated stack.
     * On real x86, stdcall callee does "ret N" to pop the return address
     * and N bytes of arguments. We already popped the dummy return address
     * above; now pop the args. */
    g_esp += g_slot_arg_bytes[slot];

    if ((long)g_kernel_call_count <= kernel_call_log_limit()) {
        fprintf(stderr, "  [KERNEL] → returned 0x%08X\n", g_eax);
        fflush(stderr);
    }

    /* A point between two kernel calls of this guest thread, where an
     * interrupt pending on a device can be delivered (see xbox_kernel_call_isr). */
    if (xbox_kernel_post_call_hook)
        xbox_kernel_post_call_hook();
}

/* ── Dispatch lookup ────────────────────────────────────── */

/**
 * Look up a kernel thunk by synthetic VA.
 * Called as a fallback when recomp_lookup() returns NULL.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            return kernel_thunk_dispatch;
        }
    }
    return NULL;
}

/* ── Initialization ─────────────────────────────────────── */

/*
 * Where this title's kernel thunk table lives. Defaults to the compile-time
 * constant, but every XBE puts it somewhere different (it comes from the
 * header's KernelImageThunkAddress), so xbox_MemoryLayoutInit() parses the
 * real address out of the binary and overrides it here.
 *
 * Halo build 2276 puts it at 0x00253090 against the default's 0x0036B7C0 --
 * without the override the bridge patches ordinals into whatever happens to
 * live at the wrong address and every kernel call goes somewhere arbitrary.
 */
static uint32_t g_thunk_table_base  = XBOX_KERNEL_THUNK_TABLE_BASE;
static uint32_t g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count)
{
    if (!xbox_va) {
        return;
    }

    g_thunk_table_base = xbox_va;

    /* count indexes g_slot_* arrays, which are sized by the macro. A title
     * importing more slots than the real kernel exports would run off them. */
    if (count && count <= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        g_thunk_table_count = count;
    } else if (count > XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr,
                "  Kernel thunk bridge: XBE declares %u thunk slots, clamping to %d\n",
                count, XBOX_KERNEL_THUNK_TABLE_SIZE);
        g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;
    }
}

/**
 * Resolve the kernel thunk table in Xbox memory.
 *
 * Must be called AFTER xbox_MemoryLayoutInit() so Xbox memory is mapped.
 *
 * Reads the actual ordinals from the XBE memory thunk table (0x80000000|ordinal),
 * resolves each to a per-ordinal bridge function, and replaces the entry
 * with a synthetic VA for dispatch.
 */
void xbox_kernel_bridge_init(void)
{
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            g_thunk_table_count, g_thunk_table_base);

    /* The thunk table lives in .rdata which is marked PAGE_READONLY.
     * Temporarily make it writable so we can patch the ordinals. */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        PAGE_READWRITE,
        &old_protect
    );

    /* Initialize kernel data export values first */
    kernel_data_init();

    for (i = 0; i < g_thunk_table_count; i++) {
        uint32_t va = g_thunk_table_base + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {
            /* Read the actual ordinal from Xbox memory */
            ULONG ordinal = current & 0x7FFFFFFF;
            g_slot_ordinals[i] = ordinal;

            /* Check if this is a data export */
            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                /* DATA export: point thunk to actual data in mapped memory.
                 * This allows the game to dereference the thunk entry. */
                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            /* FUNCTION export: use synthetic VA for dispatch */
            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            g_slot_arg_bytes[i] = stdcall_args_for_ordinal(ordinal);
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
            }

            /* Replace Xbox memory entry with synthetic VA */
            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    /* Restore original protection */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, g_thunk_table_count, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);

}
