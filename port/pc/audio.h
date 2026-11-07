#pragma once
#include <stdint.h>

int  audio_init(int rate);
void audio_push(const int16_t *stereo, int frames);
int  audio_queued(void);    /* buffers waiting to play, -1 if audio is off */

int  wav_open(const char *path, int rate);
void wav_write(const int16_t *stereo, int frames);
void wav_close(void);
