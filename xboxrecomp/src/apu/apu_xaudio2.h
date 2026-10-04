/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* Initialize XAudio2. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);

/* Buffers queued on the device and not yet played. */
int xa2_queued(void);
/* 8 x 256 samples = ~43 ms of queued audio (was 16 = ~85 ms; 0 underruns
 * were measured in race at 8, and the console has no such
 * queue, so less is closer to it; XBOX_AUDIO_QUEUE=16 restores the old
 * value). The buffers used to be 1024
 * samples, and the frame thread renders one buffer in a single burst before
 * waiting on the device: a title that refills its streaming ring ~1000
 * samples ahead of the play cursor (SSX Tricky's EA mixer) had the last 32
 * samples of every burst read last lap's data -- an exact copy of the sound
 * 50 ms earlier, a click every 21 ms. Small buffers keep each
 * burst well inside the title's lead. */
#define XA2_TARGET_QUEUED 8

/* XBOX_AUDIO_QUEUE=N (fork): buffers the frame thread keeps queued
 * instead of XA2_TARGET_QUEUED, clamped to XA2_QUEUE_MIN..XA2_QUEUE_MAX
 * (each buffer is 256 samples = 5.33 ms at 48 kHz, so 8 = ~43 ms of sound
 * between the title mixing it and the device playing it). Fewer buffers
 * bring the sound closer to the picture but leave less room for the frame
 * thread to be late: an underrun is a gap, heard as a click. Read once, at
 * the first call; the effective value and the measured queue latency go to
 * the periodic [XA2] line. */
#define XA2_QUEUE_MIN 2
#define XA2_QUEUE_MAX 20
int xa2_target_queued(void);

#endif /* APU_XAUDIO2_H */
