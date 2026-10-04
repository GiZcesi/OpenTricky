/**
 * Xbox Memory Layout Compatibility
 *
 * The Xbox has 64MB of unified memory shared between CPU and GPU.
 * Memory is identity-mapped (physical == virtual for most of it).
 * Game code and data are linked to specific address ranges which vary
 * per game. Section addresses are parsed dynamically from the XBE header
 * at runtime, so this module works with ANY Xbox game.
 *
 * On Windows, we:
 * 1. Create a 64MB file mapping (CreateFileMapping)
 * 2. Map the base view + 28 mirror views at 64MB intervals
 * 3. Parse the XBE section table and copy sections to their Xbox VAs
 * 4. Set up simulated stack, heap, TIB, and kernel data area
 *
 * The mirror views ensure Xbox RAM wrapping works correctly: the Xbox
 * memory controller uses a 26-bit address bus, so ALL addresses wrap
 * modulo 64MB. File mapping views backed by the same section give us
 * true aliases where writes at one address are visible at all mirrors.
 */

#ifndef XBOX_MEMORY_LAYOUT_H
#define XBOX_MEMORY_LAYOUT_H

#include "platform/xbox_winnt.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox memory map constants
 * ================================================================ */

/* Base address of all XBE files in Xbox memory */
#define XBOX_BASE_ADDRESS       0x00010000

/* Start of mapped region - includes low memory (KPCR at 0x0) because
 * game code reads from addresses like 0x20 and 0x28 (Xbox kernel structures). */
#define XBOX_MAP_START          0x00000000

/* Xbox physical memory */
/* The console has 64 MB, and this port must model that exactly -- not because
 * of the amount, but because of what the title does with the *addresses*.
 * SSX Tricky hands the GPU `va & 0x03FFFFFF`, a 64 MB mask, and it is allowed
 * to do that because on real hardware no address can exceed 64 MB. The moment
 * this constant is larger, any allocation placed above the line becomes
 * invisible to the GPU: the mask folds it onto a different, unrelated page.
 *
 * This was raised to 128 MB and then 140 MB to satisfy the title's hardcoded
 * 55679384-byte (0x3519998) arena request, which failed to fit. That made the
 * allocation succeed and the rendering wrong -- the arena straddled the line,
 * and every texture the title placed in its upper half (the 512x512 splash
 * among them) was fetched by the GPU from somewhere else. Raising the ceiling
 * treated the symptom; the arena did not fit because this port was spending
 * 32.5 MB before the arena was even reached:
 *
 *     XBE image      1.74 MB actually mapped, but the stack was pinned at
 *                    0x00780000, reserving 7.5 MB for it
 *     stack          8 MB (raised from 1 MB to absorb an ICALL arg leak)
 *     contig pool    16 MB reserved up front, of which ~7 MB is ever used
 *
 * With those three corrected the title's own demand fits in 64 MB the way it
 * does on the console: ~3.1 MB of image and stack, the 53.1 MB arena, and
 * ~7 MB of contiguous surfaces allocated downward from the top. */
#define XBOX_TOTAL_RAM          (140 * 1024 * 1024)
#define XBOX_GPU_RESERVED       (4 * 1024 * 1024)   /* ~4 MB for GPU */

/** The address ceiling the GPU can reach. The title masks every address
 *  it gives the GPU with 0x03FFFFFF, so anything above this is invisible
 *  to it no matter how much RAM this port maps. */
#define XBOX_GPU_VISIBLE_END    (64u * 1024u * 1024u)

/* The 64 MB of RAM is also visible uncached at 0x80000000 and write-combined
 * at 0xF0000000. Guest code folds these in XBOX_PTR (recomp_types.h); host
 * code that takes a guest address -- the kernel bridges -- folds with this.
 * Contiguous memory is handed out in the uncached form, as the real kernel
 * does (the title keys "already relocated" off bit 31 of heap
 * pointers, and frees D3D buffers as `physical | 0x80000000`). */
static inline uint32_t xbox_fold_ram_alias(uint32_t va)
{
    if (va >= 0x80000000u && va < 0x84000000u) return va & 0x7FFFFFFFu;
    if (va >= 0xF0000000u && va < 0xF4000000u) return va & 0x03FFFFFFu;
    return va;
}

/* NOTE: Section addresses (.text, .rdata, .data, etc.) are NOT hardcoded.
 * They are parsed from the XBE header at runtime in xbox_MemoryLayoutInit().
 * This allows the toolkit to work with ANY Xbox game without modification. */

/* ================================================================
 * Memory initialization
 * ================================================================ */

/**
 * Initialize the Xbox memory layout.
 *
 * Reserves the virtual address range 0x00010000 through 0x0076F000
 * and maps the XBE sections to their expected addresses:
 * - .rdata: copied from XBE, read-only
 * - .data: initialized portion copied from XBE, BSS zeroed
 *
 * Note: .text is NOT mapped here - the recompiled code is native
 * Windows code and doesn't need to be at the original address.
 * The data sections DO need to be at their original addresses
 * because the recompiled code references globals by absolute address.
 *
 * @param xbe_data  Pointer to the loaded XBE file contents.
 * @param xbe_size  Size of the XBE file.
 * @return TRUE on success, FALSE on failure.
 */
BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size);

/**
 * Release the reserved Xbox memory layout.
 */
void xbox_MemoryLayoutShutdown(void);

/**
 * Check if an address falls within the Xbox memory map.
 */
BOOL xbox_IsXboxAddress(uintptr_t address);

/**
 * Get the base pointer for direct memory access.
 * Returns NULL if memory layout is not initialized.
 */
void *xbox_GetMemoryBase(void);

/**
 * Get the offset from Xbox VA to actual mapped address.
 * actual_address = xbox_va + offset
 * Returns 0 if memory is mapped at original Xbox addresses (ideal case).
 */
ptrdiff_t xbox_GetMemoryOffset(void);

/* ================================================================
 * Xbox stack for recompiled code
 * ================================================================ */

/* ================================================================
 * Kernel data export area
 * ================================================================ */

/** Base VA for kernel data exports (XboxHardwareInfo, XboxKrnlVersion, etc.)
 *  These are kernel exports that are DATA, not functions. The game reads
 *  their thunk entries and dereferences them to access the data. */
#define XBOX_KERNEL_DATA_BASE   0x04100000
#define XBOX_KERNEL_DATA_SIZE   4096   /* 4 KB - plenty for all data exports */

/** Adjacent 4 KB page holding the synthetic Xbox-kernel PE header that
 *  RenderWare's xbcache.c reads (MEM32(0x8001003C) et al.) to detect CPU
 *  cache-line info -- see xbox_MemoryLayoutInit for what's written here.
 *  Lives in ordinary Xbox VA space, backed by the same base RAM mapping
 *  (no separate native allocation), unlike the old approach of a fixed
 *  native VirtualAlloc at base+0x80010000: that collided with the RAM-wrap
 *  mirror view covering the same relative offset once XBOX_TOTAL_RAM grew
 *  large enough to reach it (confirmed live). recomp_types.h's
 *  xbox_resolve_uncached_alias redirects Xbox VA 0x80010000-0x80010FFF
 *  here before applying its normal top-bit uncached-alias mask. */
#define XBOX_FAKE_KERNEL_HEADER_VA   0x04101000
#define XBOX_FAKE_KERNEL_HEADER_SIZE 4096

/* ================================================================
 * Null-pointer absorption page
 * ================================================================
 *
 * xbox_resolve_uncached_alias redirects every guest access below 0x100
 * somewhere real, so a null-pointer dereference reads or writes instead of
 * faulting. That redirect used to point at the calling thread's own KPCR,
 * which put the landing zone for every stray null write directly on top of
 * live per-thread state -- and it landed: a `mov [ecx+0x24], al` with a null
 * `ecx` wrote 0xBC over **KPCR+0x24, the IRQL byte**, after which the CRT's
 * _getptd (sub_001633C9) read an IRQL of 188, decided it had been called at
 * raised IRQL, and called KeBugCheck(0x0A). That is what ended every run at
 * exit code 10 while the video was playing.
 *
 * The safety net is still wanted -- removing the redirect outright makes
 * sub_00172202 fault immediately -- so it now points here instead: a
 * dedicated page that nothing else reads. Null writes are absorbed, null
 * reads return whatever a previous null write left (zero until one happens),
 * and no thread's KPCR, SEH chain or TLS pointer is in the blast radius.
 *
 * Set XBOX_PROTECT_LOWPAGE=1 to fault on access instead, when the goal is to
 * find the writer rather than survive it.
 */
#define XBOX_NULL_PAGE_VA    0x04102000
#define XBOX_NULL_PAGE_SIZE  4096

/* ================================================================
 * Per-thread Thread Information Blocks
 * ================================================================
 *
 * The recompiler drops `fs:` segment prefixes, so `mov eax, fs:0x28`
 * becomes a plain absolute `MEM32(0x28)`. That used to be backed by a
 * single fake TIB at Xbox VA 0, shared by every guest thread -- but on
 * real hardware `fs:` is per-thread, and the title relies on that:
 *
 *   fs:[0x00]  SEH exception-chain head. Every _SEH_prolog pushes a frame
 *              pointing into its own stack and stores it here; with one
 *              shared slot, threads splice each other's frames into one
 *              chain that then walks across foreign stacks.
 *   fs:[0x28]  Per-thread TLS block. The CRT stores a heap pointer here
 *              per thread; sharing it meant one thread read another's
 *              value, and a torn read sent _threadstartex into a ~4 GB
 *              memcpy from Xbox VA 4.
 *
 * Each guest thread now gets its own 0x100-byte TIB out of this pool, and
 * xbox_resolve_uncached_alias (recomp_types.h) redirects any access below
 * 0x100 to the calling thread's own block. One predictable compare on the
 * memory-access path buys genuine per-thread semantics.
 */
/*
 * Layout, per thread. Field offsets follow the real Xbox KPCR/KTHREAD, cross
 * checked against Cxbx-Reloaded's src/core/kernel/common/types.h:
 *
 *   KPCR      +0x00 NT_TIB (ExceptionList, StackBase, StackLimit, ... Self)
 *             +0x1C SelfPcr
 *             +0x20 Prcb          -> points at PrcbData, i.e. KPCR+0x28
 *             +0x24 Irql
 *             +0x28 PrcbData      -- and PrcbData's first field is CurrentThread,
 *                                    so `mov eax, fs:0x28` is KeGetCurrentThread()
 *   KTHREAD   +0x1C StackBase  +0x20 StackLimit  +0x28 TlsData
 *
 * That last chain is what the CRT's _threadstartex (sub_001543DE) walks:
 *     eax = fs:[0x28]        ; current KTHREAD
 *     edx = [eax + 0x28]     ; its TlsData
 *     edx += 4; [edx-4] = edx ; publish the TLS array
 * so every thread needs its own KTHREAD *and* its own TlsData block, not just
 * its own KPCR. Sharing either one is what sent _threadstartex into a ~4 GB
 * memcpy in part forty-three.
 */
#define XBOX_TIB_POOL_VA    0x04110000
#define XBOX_TIB_SIZE       0x800     /* KPCR + KTHREAD + TlsData per thread */
#define XBOX_TIB_MAX        64        /* 0x4110000-0x4130000, above the GPU line */
#define XBOX_TIB_POOL_SIZE  (XBOX_TIB_SIZE * XBOX_TIB_MAX)

/* Offsets within one pool slot. */
#define XBOX_TIB_KPCR_OFF     0x000
#define XBOX_TIB_KTHREAD_OFF  0x300
#define XBOX_TIB_TLSDATA_OFF  0x500

/* KPCR fields (the subset the title touches). */
#define XBOX_KPCR_SEH_LIST    0x00   /* exception chain head, -1 = end */
/* KPCR+0x04 is the **TLS array pointer**, not StackBase.
 * Every one of the six `fs:[4]` reads in the image uses it the same way:
 *     mov eax, ds:0x2016d8      ; TLS index
 *     mov ecx, fs:0x4           ; TLS array base
 *     mov eax, [ecx + eax*4]    ; tls_array[index]
 * and the CRT's thread bootstrap writes TlsData[0] = TlsData + 4 to match.
 * Writing stack_base here made tls_array[index] read zero, so callers
 * computed `0 + 8` and dereferenced address 8 -- which the low-address TIB
 * redirect then served from the KPCR, masking it. */
#define XBOX_KPCR_TLS_ARRAY   0x04
#define XBOX_KPCR_STACK_LIMIT 0x08   /* low address */
#define XBOX_KPCR_SELF        0x18
#define XBOX_KPCR_SELF_PCR    0x1C
#define XBOX_KPCR_PRCB        0x20
#define XBOX_KPCR_IRQL        0x24   /* UCHAR; the CRT bugchecks if it reads >= 2 */
#define XBOX_KPCR_PRCB_DATA   0x28   /* PrcbData.CurrentThread lives here */

/* KTHREAD fields. */
#define XBOX_KTHREAD_STACK_BASE  0x1C
#define XBOX_KTHREAD_STACK_LIMIT 0x20
#define XBOX_KTHREAD_TLS_DATA    0x28

/*
 * The Prcb's +0x250 field: SSX Tricky's entry-point logic reads
 * [fs:[0x20]+0x250] and takes a near-empty branch when it is zero (see the
 * FAKE_PRCB note in xbox_MemoryLayoutInit). Now that Prcb correctly points at
 * PrcbData inside this slot, the field lives here.
 */
#define XBOX_PRCB_D3D_CACHE_OFF  0x250

/*
 * Claim a slot for the calling thread and point the redirect at it.
 * `stack_base` is the high address, `stack_limit` the low one. Returns the
 * KPCR's Xbox VA, or 0 if the pool is exhausted (the thread then falls back to
 * the shared block at VA 0, which is the pre-part-forty-four behaviour).
 */
uint32_t xbox_tib_alloc_for_thread(uint32_t stack_base, uint32_t stack_limit);

/* Offsets within the kernel data area */
#define KDATA_HARDWARE_INFO     0x000  /* XBOX_HARDWARE_INFO (8 bytes) */
#define KDATA_KRNL_VERSION      0x010  /* XBOX_KRNL_VERSION (8 bytes) */
#define KDATA_TICK_COUNT        0x020  /* KeTickCount (4 bytes) */
#define KDATA_LAUNCH_DATA_PAGE  0x030  /* LaunchDataPage (4 bytes, pointer) */
#define KDATA_THREAD_OBJ_TYPE   0x040  /* PsThreadObjectType (4 bytes) */
#define KDATA_EVENT_OBJ_TYPE    0x050  /* ExEventObjectType (4 bytes) */
#define KDATA_XE_IMAGE_FILENAME 0x060  /* XeImageFileName (ANSI_STRING) */
#define KDATA_IO_COMPLETION_TYPE 0x070 /* IoCompletionObjectType (4 bytes) */
#define KDATA_IO_DEVICE_TYPE    0x080  /* IoDeviceObjectType (4 bytes) */
#define KDATA_HD_KEY            0x100  /* XboxHDKey (16 bytes) */
#define KDATA_SIGNATURE_KEY     0x110  /* XboxSignatureKey (16 bytes) */
#define KDATA_LAN_KEY           0x120  /* XboxLANKey (16 bytes) */
#define KDATA_ALT_SIGNATURE_KEYS 0x130 /* XboxAlternateSignatureKeys (256 bytes) */
#define KDATA_XE_PUBLIC_KEY     0x300  /* XePublicKeyData (284 bytes) */

/** Size of the simulated Xbox stack (512 KB).
 *  This was 8 MB to absorb stdcall args leaked onto the stack by failed
 *  RECOMP_ICALL indirect calls. That leak is now down to 3 dropped calls a
 *  run, and the space it cost is needed below the 64 MB line for the title's
 *  arena -- see XBOX_TOTAL_RAM. 512 KB is not a guess: the guest's ESP stays
 *  within 0x20 bytes of the top through boot, and the two spawned threads get
 *  their own 64 KB stacks, so this is ~100x observed peak use. If
 *  indirect-call misses ever climb again, fix the misses; do not buy headroom
 *  here, there is none to spend. */
#define XBOX_STACK_SIZE     (1 * 1024 * 1024)

/** Base VA of the stack area, now ABOVE the 64 MB line.
 *
 *  None of the stack, the kernel data area or the TIB pool is ever read by the
 *  GPU, so none of them needs to survive the title's `& 0x03FFFFFF`. Keeping
 *  them below 64 MB cost the low region 8.5 MB that the title's 53 MB arena
 *  needs: the arena has to start below ~11.4 MB for the splash texture, which
 *  sits 51.6 MB into it, to be GPU-reachable.
 *
 *  Moving this block *down* against the image was tried twice and renders
 *  nothing at all while allocating cleanly. Moving it
 *  up leaves 0x00220000..0x04000000 as one unbroken GPU-visible region.
 *
 *  Old placement, for reference: kernel data 0x00740000, TIB pool 0x00750000,
 *  stack 0x00780000.
 *  The 11 XBE sections end at 0x00213480 (ABORTFONT, 1.74 MB mapped in
 *  total). This whole block -- kernel data exports, the synthetic kernel PE
 *  header, the TIB pool, then the stack -- used to sit at 0x00740000 with the
 *  stack at 0x00780000, leaving 5.4 MB above the image mapped by nothing.
 *  That gap was NOT free to reuse: moving only the stack into it left
 *  XBOX_HEAP_BASE below the kernel block, and the title's 53 MB arena then
 *  grew straight through the TIB pool, destroying per-thread SEH and TLS
 *  state -- the title stopped booting after 28 allocations, reading its own
 *  0xDEADC0DE poison. The block moves as a unit, relative spacing intact. */
#define XBOX_STACK_BASE     0x04140000

/** Initial ESP value (top of stack, 16-byte aligned). */
#define XBOX_STACK_TOP      (XBOX_STACK_BASE + XBOX_STACK_SIZE - 16)

/* ================================================================
 * Xbox dynamic heap (for MmAllocateContiguousMemory, etc.)
 * ================================================================ */

/** Base VA of the dynamic heap area (above stack). */
#define XBOX_HEAP_BASE      0x00214000

/** Where non-GPU allocations continue once the low region is full. Must clear
 *  the kernel data area, TIB pool and stack, which now live above the line. */
#define XBOX_HEAP_SPILL_BASE 0x04240000

/** Smallest unconstrained allocation still served from the GPU-visible low
 *  region.
 *
 *  Only two kinds of allocation need to survive the title's `& 0x03FFFFFF`:
 *  the ones it asks for by range (MmAllocateContiguousMemoryEx -- surfaces),
 *  and the bulk pools it sub-allocates GPU resources out of. This port cannot
 *  see which allocation is which, but it can see how big they are: SSX
 *  Tricky's arena is a single 53 MB block, while everything else it allocates
 *  unconstrained is 64 KB or less.
 *
 *  Without this split, ~700 KB of small allocations landed above the arena and
 *  squeezed the low region until contiguous requests started returning 0. The
 *  title stored one of those nulls as an .xbd mesh table and faulted reading
 *  Xbox VA 0 in Mesh_RegisterVertexBuffers, ~2 frames after the splash
 *  appeared. */
#define XBOX_LOW_REGION_MIN_ALLOC  (1u * 1024u * 1024u)

/** Size of the dynamic heap.
 *  The total mapped region (data + stack + heap) equals 64 MB so the
 *  RenderWare engine's memory probing stops at the correct boundary. On a
 *  real Xbox, probing past 64 MB causes a page fault that the engine catches
 *  via SEH to determine available memory -- and so that every address the
 *  title can produce survives the `& 0x03FFFFFF` it applies before handing
 *  one to the GPU. */
#define XBOX_HEAP_SIZE      (XBOX_TOTAL_RAM - XBOX_HEAP_BASE)  /* ~55.5 MB */

/** No static mirror/guard region. RAM mirror is handled via file mapping
 *  views that alias the same physical pages as the base 64 MB region. */
#define XBOX_MIRROR_SIZE    0
#define XBOX_GUARD_SIZE     0

/** Number of 64 MB mirror views to pre-map (covers 1.75 GB of address space). */
#define XBOX_NUM_MIRRORS    28

/**
 * Allocate from the Xbox heap. Returns an Xbox VA, or 0 on failure.
 * Alignment must be a power of 2 (minimum 4).
 * Thread-safe: no (single-threaded recompiled code).
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/**
 * Allocate memory the title reaches only by virtual address (virtual-memory
 * reservations, thread stacks) from above the 64 MB GPU line, leaving the
 * GPU-visible region to what needs it. See xbox_memory_layout.c.
 */
uint32_t xbox_HeapAllocVirtual(uint32_t size, uint32_t alignment);
uint32_t xbox_HeapAllocRange(uint32_t size, uint32_t alignment,
                             uint32_t low, uint32_t high);

/**
 * Free a block from the Xbox heap. Currently a no-op (bump allocator).
 */
void xbox_HeapFree(uint32_t xbox_va);

/* Which ledgered allocation contains `va`? Returns 1 and fills the out
 * parameters (any of which may be NULL) if one does, 0 otherwise. `frames`
 * receives up to `max_frames` host return addresses from the allocation's
 * call site; pass NULL/0 when only the extent is wanted.
 *
 * This prototype must stay visible to every caller. xbox_HeapFree used to call
 * it from above its definition with no declaration in scope, so C accepted an
 * implicit four-argument call to a seven-argument function. On Win64 the three
 * missing arguments were read from whatever sat in the caller's stack slots,
 * and the backtrace copy then wrote through that garbage `frames` pointer --
 * which is what zeroed the guest esp inside MmFreeContiguousMemory and hung
 * DirectSound teardown. */
int xbox_heap_owner_of(uint32_t va, uint32_t *out_base, uint32_t *out_size,
                       uint32_t *out_index, void **frames, int max_frames,
                       int *out_nframes);

/**
 * Report this heap's real, live consumption -- both in bytes, matching
 * the same accounting xbox_HeapAlloc's own "out of memory" check uses.
 * Use this (not host-PC memory status) for anything reporting available
 * memory back to the game, e.g. MmQueryStatistics.
 */
void xbox_HeapGetStats(uint32_t* out_used, uint32_t* out_total);

/**
 * Get the file mapping handle for the Xbox memory region.
 * Used by the VEH handler to map additional mirror views on demand.
 * Returns NULL if file mapping is not available.
 */
HANDLE xbox_GetMappingHandle(void);
size_t xbox_GetMemorySize(void);
int xbox_GetMirrorCount(void);

/**
 * Verify that the whole guest view is still exclusively ours.
 *
 * The base view is placed by MapViewOfFileEx, and when none of the fixed
 * candidates validate we fall back to letting the OS pick the address. A
 * view is supposed to be an exclusive reservation, so nothing else in the
 * process should ever be able to hand out memory inside it -- but a wrong
 * base can still leave us aliasing something (confirmed live: ntdll's
 * RtlAllocateHeap was seen writing inside the view, smashing CRT pool block
 * headers in guest RAM). This walks the view with VirtualQuery and reports
 * every region that is not a MEM_MAPPED region whose AllocationBase is our
 * own base.
 *
 * `tag` is printed with any complaint so callers can say when they checked.
 * Returns the number of foreign regions found (0 == the view is intact).
 */
int xbox_VerifyViewIntegrity(const char *tag);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_MEMORY_LAYOUT_H */
