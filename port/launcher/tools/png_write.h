/*
 * png_write.h -- minimal PNG and WAV writers for the launcher's host tools.
 */
#ifndef OT_PNG_WRITE_H
#define OT_PNG_WRITE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ot_image.h"

bool png_write(const char *path, const OtImage *im);
bool wav_write(const char *path, const int16_t *pcm, int frames, int channels, int rate);

#endif /* OT_PNG_WRITE_H */
