#pragma once
#include <stdint.h>

void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...);
void rt_log_close(void);   /* stop logging and close the file (before a restart) */
/* platform: tell the player about a fatal error (a message box on PC; stderr is enough elsewhere) */
void plat_fatal_message(const char *msg);
/* Log a message only the first time `key` is seen in `channel` (for noisy hardware accesses). */
int  rt_once(int channel, uint32_t key);

enum { ONCE_IO_R, ONCE_IO_W, ONCE_BIOS, ONCE_GTE, ONCE_GPU, ONCE_MISC, ONCE_CHANNELS };
