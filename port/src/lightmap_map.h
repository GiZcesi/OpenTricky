/*
 * lightmap_map -- the terrain's light map gathered for an effect that this
 * source tree does not hold: the game still calls these, and each one
 * (extras_off.c) does nothing; g_lmap_armed stays 0.
 */
#ifndef LIGHTMAP_MAP_H
#define LIGHTMAP_MAP_H

#include <stdint.h>

extern int g_lmap_armed;

void lightmap_map_init(void);
void lightmap_map_note(uint32_t obj, uint32_t vb, uint32_t pidx);
void lightmap_map_renderer(uint32_t r);

#endif
