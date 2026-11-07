#pragma once
#include <stdint.h>

/*
 * System 12 sound / I-O sub-board: H8/3002 running the original sound program (ftl1vera.11s),
 * C352 PCM chip, RAM shared with the main CPU, and the cabinet inputs the H8 reads.
 */
#define S12SND_H8_HZ     16934400   /* H8/3002 clock */
#define S12SND_OUT_RATE  44100      /* rate handed to the platform (C352 runs at 88200) */

void s12sound_init(const uint8_t *h8_rom_le, uint32_t rom_size, const uint8_t *wave, uint32_t wave_size);
void s12sound_run(int h8_states);           /* advance the H8 and the C352 together */
void s12sound_vblank(int state);            /* vblank drives H8 IRQ1 and port B bit 7 */

/* main-CPU side of the shared RAM (0x1F080000-0x1F083FFF), little-endian view */
uint32_t s12sound_shared_r(uint32_t ofs, int size);
void     s12sound_shared_w(uint32_t ofs, uint32_t v, int size);

/* pull up to `frames` stereo frames at S12SND_OUT_RATE; returns frames written (rest is silence) */
int  s12sound_audio_pull(int16_t *out, int frames);
/* take everything produced so far at the chip rate (88200 Hz stereo); returns frames copied */
int  s12sound_audio_take_raw(int16_t *out, int max_frames);
#define S12SND_CHIP_RATE 88200

/* Cabinet inputs (active high), JVS-style layout as in MAME's namcos12 driver:
 * player: 0x80 start, 0x40 service, 0x20 up, 0x10 down, 0x08 left, 0x04 right,
 *         0x02 button1, 0x01 button2, 0x8000 button3, 0x4000 button4, 0x2000 button5, 0x1000 button6 */
typedef struct { uint16_t player[2]; uint8_t test, coin[2], service_dip; } S12Inputs;
void plat_read_inputs(S12Inputs *in);        /* implemented by the platform layer */
