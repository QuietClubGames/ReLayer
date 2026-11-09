#pragma once
/* Hang watchdog (watchdog.c): logs where the game is stuck if no frame is drawn for 3 seconds. */
void watchdog_start(void);              /* call from the main thread */
void watchdog_beat(void);               /* a frame reached the screen */
void watchdog_note(const char *state);  /* what the game is doing, for the report */
