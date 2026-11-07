#pragma once
#include <stdint.h>
#include "../core/s12sound.h"

/* Raw game-controller state (XInput layout: buttons bitfield, triggers 0-255, sticks -32768..32767). */
typedef struct {
    int connected;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly;
} PadRaw;

/* Physical controller inputs that can be assigned to a cabinet input. */
enum {
    SRC_NONE = 0,
    SRC_DUP, SRC_DDOWN, SRC_DLEFT, SRC_DRIGHT, SRC_START, SRC_BACK, SRC_LS, SRC_RS,
    SRC_LB, SRC_RB, SRC_A, SRC_B, SRC_X, SRC_Y, SRC_LT, SRC_RT,
    SRC_COUNT
};

/* Cabinet inputs that can be mapped (per player). */
enum { MAP_B1, MAP_B2, MAP_B3, MAP_B4, MAP_B5, MAP_B6, MAP_START, MAP_COIN, MAP_COUNT };

typedef struct {
    uint8_t src[MAP_COUNT][2];    /* up to two physical inputs per cabinet input */
    float stick_deadzone;         /* 0..1 of full deflection */
} PadMap;

/* ---- pure mapping logic (pad_map.c), testable without hardware ---- */
void        padmap_defaults(PadMap *m);
int         padmap_load(PadMap *m, const char *path);   /* returns 0 if the file is missing */
int         padmap_source_from_name(const char *name);
const char *padmap_source_name(int src);
/* Cabinet player bits (S12Inputs layout) for one pad; *coin set if the coin input is held. */
uint16_t    padmap_player_bits(const PadMap *m, const PadRaw *p, int *coin);

const char *padmap_display_name(int src);
uint32_t    padmap_held_sources(const PadRaw *p);
int         padmap_save(const PadMap *m, const char *path);

/* ---- platform part (pad_xinput.c) ---- */
void pad_init(const char *config_path);
void pad_poll(void);                       /* once per frame */
void pad_apply(S12Inputs *in);             /* OR controller 1 -> player 1, controller 2 -> player 2 */
PadMap  *pad_map(void);                    /* the live mapping (edit, then pad_save_map) */
void     pad_save_map(void);
int      pad_player_connected(int player); /* 0/1 */
uint32_t pad_held_all(void);               /* SRC_* bits held on any connected pad */
int      pad_stick_dir(void);              /* left stick on any pad: bit0 up, 1 down, 2 left, 3 right */
