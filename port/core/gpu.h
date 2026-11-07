#pragma once
#include <stdint.h>

/* PS1 GPU command interface. gpu.c decodes the command stream; drawing goes to VRAM. */
void     gpu_gp0(uint32_t v);
void     gpu_gp1(uint32_t v);
uint32_t gpu_read(void);
uint32_t gpu_stat(void);
void     gpu_vblank(void);

extern uint16_t g_vram[1024 * 1024];   /* 1024 x 1024 (System 12 has 2 MB VRAM) */
/* display area set by the game (GP1 0x05/0x06/0x07/0x08) */
typedef struct { int x, y, w, h; int enabled; int depth24; } GpuDisplay;
GpuDisplay gpu_display(void);
