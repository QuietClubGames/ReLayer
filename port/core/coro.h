#pragma once
#include <stddef.h>

/* Minimal coroutine interface used to run the game's kernel threads (OpenTh/ChangeTh).
 * Implemented per platform: port/pc/coro_win32.c (Windows fibers); a KallistiOS version later. */
typedef struct Coro Coro;
Coro *coro_init_main(void);
Coro *coro_create(void (*fn)(void *), void *arg, size_t stack_size);
void  coro_switch(Coro *to);
