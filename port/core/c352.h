#pragma once
#include <stdint.h>

/* Namco C352 PCM chip. Registers are 16-bit; `off` is the word offset (byte address / 2). */
void     c352_init(const uint8_t *wave_rom, uint32_t size);
void     c352_reset(void);
uint16_t c352_read(int off);
void     c352_write(int off, uint16_t data, uint16_t mask);
/* Render n stereo frames (front left/right) at the chip rate: 25.4016 MHz / 288 = 88200 Hz. */
void     c352_render(int16_t *stereo_out, int n);
#define  C352_RATE 88200
