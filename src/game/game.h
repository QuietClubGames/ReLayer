#pragma once
/*
 * Hand-written Fighting Layer code and knowledge of the game's data (the start of the decompilation).
 * Platform independent: used by the PC build now and the Dreamcast build later.
 */

/* ---- character release (release.c) ---- */
/* 1 = the secret characters (Preston Ajax, Clemence Keliber, Joe Fendi) and everything else the arcade
 * releases over time are available from the start. 0 = the original time release, counted from the date
 * the board was first switched on (needs the real-time clock). */
extern int g_unlock_all;

/* ---- arcade (operator) settings stored in the EEPROM (arcade_settings.c) ---- */
enum {
    AS_FREE_PLAY,        /* 0/1 */
    AS_DIFFICULTY,       /* 0-7  (shown 1-8, default 3) */
    AS_DAMAGE,           /* 0-7  (shown 1-8, default 4) */
    AS_TIMER,            /* 0-3  (shown 1-4 slow..fast, default 2) */
    AS_FIGHTS_1P,        /* 0-4  (shown 1-5, default 2) */
    AS_FIGHTS_VS,        /* 0-4  (shown 1-5, default 2) */
    AS_CHANGE_CONTINUE,  /* character change at continue, 0/1 */
    AS_CHANGE_VS,        /* character change between VS games, 0/1 */
    AS_ATTRACT_MUSIC,    /* music in attract mode, 0/1 */
    AS_EVENT_MODE,       /* 0/1 */
    AS_COUNT
};
int  arcade_setting_get(int which);
void arcade_setting_set(int which, int value);   /* updates the EEPROM and the running game */
int  arcade_setting_max(int which);
int  arcade_setting_find(const char *name);       /* "free_play", "difficulty"... -> AS_*, or -1 */
