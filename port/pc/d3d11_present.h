#pragma once
#include <windows.h>
#include <stdint.h>

int d3d_init(HWND hwnd, int refresh_hz, int gpu);   /* gpu: 0 automatic, else 1-based adapter. 0 = failed (use GDI) */
int d3d_set_exclusive(int on, HMONITOR monitor);
void d3d_shutdown(void);                            /* stop using Direct3D (before falling back to GDI) */    /* exclusive full screen on that monitor; 0 if refused */
int d3d_gpu_count(void);
const char *d3d_gpu_name(int index);
int d3d_present(const uint32_t *rgb, int w, int h); /* 0 on failure (caller falls back to GDI) */
#include "../core/gpu_hd.h"
int d3d_present_hd(const HdFrame *f);               /* 1 ok, -1 not possible (use d3d_present), 0 device failure */
int d3d_vsync(void);                                /* 1 if d3d_present() already waited for the display */
int d3d_take_repeats(void);                         /* refreshes that repeated a frame since the last call (-1: unknown) */
int d3d_repeats_total(void);                        /* running count of missed refreshes (-1: can't tell) */

/* Where the picture goes inside a cw x ch client area: the arcade monitor is 4:3, so keep that shape and
 * leave black bars on the sides (or top/bottom) instead of stretching. */
extern int g_aspect_mode;
static inline void picture_rect(int cw, int ch, int *x, int *y, int *w, int *h) {
    if (g_aspect_mode == 1) { *x = 0; *y = 0; *w = cw; *h = ch; return; }      /* stretch to fill */
    if (cw * 3 >= ch * 4) { *h = ch; *w = ch * 4 / 3; }
    else { *w = cw; *h = cw * 3 / 4; }
    if (g_aspect_mode == 2 && *h >= 240) {           /* 4:3 at a whole-number multiple of 240 lines */
        *h = (*h / 240) * 240;
        *w = *h * 4 / 3;
    }
    *x = (cw - *w) / 2;
    *y = (ch - *h) / 2;
}
