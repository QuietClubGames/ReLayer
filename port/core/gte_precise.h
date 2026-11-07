#pragma once
#include <stdint.h>

/* Look up the unrounded screen position (and depth) of a vertex the GTE projected in the last couple of
 * frames, by the packed 16:16 screen coordinate the game wrote into its drawing packet. 0 if unknown. */
int gte_precise_lookup(uint32_t packed_xy, float *x, float *y, float *z);
