/*
 * MCPX APU Audio Emulation - Public API
 *
 * Extracted from xemu (LGPL v2+), adapted for standalone use.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MCPXAPUState MCPXAPUState;

/* Initialize the APU emulation.
 * ram_ptr: pointer to the base of Xbox physical RAM (64MB).
 * Returns the APU state, or NULL on failure. */
MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);

/* Shut down and free the APU state. */
void mcpx_apu_shutdown(MCPXAPUState *d);

/* Bring the APU up and trap its MMIO aperture (0xFE800000, 512 KB) so its
 * registers get device behaviour instead of reading back as the plain RAM the
 * aperture is otherwise mapped as. DSOUND writes the voice-processor setup and
 * then waits on a status counter; backed by RAM that counter never advances.
 * Returns false if the aperture could not be guarded. */
bool apu_mmio_install(uint8_t *mem_base);

#ifdef _WIN32
#include <windows.h>
/* Service a faulting APU access; advances Rip on success. */
bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write);
#endif

/* MMIO read from APU register space (addr is offset from 0xFE800000). */
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);

/* MMIO write to APU register space (addr is offset from 0xFE800000). */
void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);

/* Play a 440Hz test tone through the APU pipeline to verify audio output.
 * Directly programs a voice without going through DirectSound. */
void mcpx_apu_play_test_tone(MCPXAPUState *d);

/* ============================================================
 * Software mixer - DirectSound buffer bridge
 *
 * DirectSound buffers register here. The APU frame thread
 * mixes active buffers directly into the waveOut output,
 * bypassing the VP hardware voice pipeline.
 * ============================================================ */

#define APU_MIXER_MAX_VOICES 64

typedef struct APUMixerVoice {
    volatile int     active;       /* 1 = playing, 0 = stopped */
    volatile int     looping;      /* 1 = loop, 0 = one-shot */
    const int16_t   *pcm_data;     /* Pointer to 16-bit PCM samples */
    uint32_t         pcm_bytes;    /* Total buffer size in bytes */
    uint32_t         num_channels;  /* 1=mono, 2=stereo */
    uint32_t         sample_rate;   /* e.g. 22050, 44100, 48000 */
    volatile uint32_t play_offset; /* Current byte offset into buffer */
    float            volume;       /* 0.0 to 1.0 */
} APUMixerVoice;

/* Allocate a mixer voice slot. Returns slot index or -1 if full. */
int apu_mixer_alloc_voice(void);

/* Free a mixer voice slot. */
void apu_mixer_free_voice(int slot);

/* Get pointer to a mixer voice for configuration. */
APUMixerVoice *apu_mixer_get_voice(int slot);

/* Start/stop playback on a voice. */
void apu_mixer_play(int slot, int looping);
void apu_mixer_stop(int slot);

#ifdef __cplusplus
}
#endif
