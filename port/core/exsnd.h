/* A small sample player mixed into the game's sound output, for a converted character's own voice (her samples
 * come from her folder, made by tools/sfex/make_swap.py; docs/SFEX2.md). Portable C99. */
#ifndef EXSND_H
#define EXSND_H
#include <stdint.h>

int  exsnd_load(const char *path);                /* SND file ("EXSN"); 1 if loaded */
void exsnd_play(int side, int id, int pan);       /* pan -63 (left) .. 63 (right) */
void exsnd_mix(int16_t *stereo, int frames);      /* add into 88200 Hz stereo frames (port/core/s12sound.c) */
void exsnd_stop_all(void);

#endif
