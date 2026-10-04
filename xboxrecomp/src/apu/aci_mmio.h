/*
 * MCPX ACI (AC'97 Codec Interface) MMIO emulation.
 *
 * The ACI register file lives at Xbox VA 0xFEC00000. Backing it with plain RAM
 * is not enough: several AC'97 registers have side effects that the hardware
 * applies as part of the write, and titles busy-wait on the result. See
 * aci_mmio.c for the specific case that made this necessary.
 */
#ifndef XBOX_ACI_MMIO_H
#define XBOX_ACI_MMIO_H

#include <stdint.h>
#include <stdbool.h>

#define XBOX_ACI_MMIO_BASE  0xFEC00000u
#define XBOX_ACI_MMIO_SIZE  0x00001000u

#ifdef _WIN32
#include <windows.h>
/* Service a faulting ACI access. Returns true if the instruction was decoded
 * and emulated (and Rip advanced), false to let the fault propagate. */
bool aci_hook_handle_mmio(PCONTEXT ctx, uint32_t fault_xbox_va, int is_write);
#endif

/* Install/remove the no-access guard over the ACI aperture. Returns false if
 * the aperture could not be protected, in which case the hook never runs. */
bool aci_mmio_install(void *mem_base);

#endif /* XBOX_ACI_MMIO_H */
