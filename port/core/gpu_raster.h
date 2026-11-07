#pragma once
#include <stdint.h>

typedef struct {
    int clip_x1, clip_y1, clip_x2, clip_y2;   /* drawing area (E3/E4), inclusive */
    int off_x, off_y;                         /* drawing offset (E5) */
    uint32_t texpage;                         /* E1 draw mode */
    int flip_x, flip_y;
    int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    int mask_set, mask_check;
} RasterState;

RasterState *raster_state(void);
void raster_poly(const uint32_t *words);
void raster_rect(const uint32_t *words);
void raster_line(const uint32_t *words, int nwords);
void raster_env(uint32_t word);
