/*
 * Arcade (operator) settings. The test menu stores them as bit fields in EEPROM bytes 0x18-0x1F (found by
 * changing one setting at a time in the operator
 * menu and comparing the EEPROM). There is no checksum. The running game keeps two RAM copies, loaded at boot:
 *   0x803552B0 = EEPROM 0x18-0x1B (coin options; free play is bit 18 of the word, read by func_801F1474)
 *   0x80368CB4 = EEPROM 0x1C-0x1F (game options: difficulty, damage, timer, fight counts...)
 */
#include "recomp.h"
#include "hw.h"
#include "game.h"
#include <string.h>

#define EEPROM_SETTINGS 0x18
#define RAM_COIN        0x803552B0u   /* copy of EEPROM 0x18-0x1B */
#define RAM_GAME        0x80368CB4u   /* copy of EEPROM 0x1C-0x1F */

static const struct { int byte, shift, bits, max; } k_fields[AS_COUNT] = {
    [AS_FREE_PLAY]       = {0x1A, 2, 1, 1},
    [AS_DIFFICULTY]      = {0x1C, 0, 3, 7},
    [AS_DAMAGE]          = {0x1C, 3, 3, 7},
    [AS_TIMER]           = {0x1C, 6, 2, 3},
    [AS_FIGHTS_1P]       = {0x1D, 1, 3, 4},
    [AS_FIGHTS_VS]       = {0x1D, 4, 3, 4},
    [AS_CHANGE_CONTINUE] = {0x1D, 7, 1, 1},
    [AS_CHANGE_VS]       = {0x1E, 0, 1, 1},
    [AS_ATTRACT_MUSIC]   = {0x1E, 1, 1, 1},
    [AS_EVENT_MODE]      = {0x1E, 2, 1, 1},
};

static const char *const k_names[AS_COUNT] = {
    "free_play", "difficulty", "damage", "timer", "fights_1p", "fights_vs",
    "change_continue", "change_vs", "attract_music", "event_mode",
};

int arcade_setting_find(const char *name) {
    for (int i = 0; i < AS_COUNT; i++)
        if (!strcmp(name, k_names[i])) return i;
    return -1;
}

int arcade_setting_max(int which) { return which >= 0 && which < AS_COUNT ? k_fields[which].max : 0; }

int arcade_setting_get(int which) {
    if (which < 0 || which >= AS_COUNT) return 0;
    uint8_t b = eeprom_get(k_fields[which].byte);
    return (b >> k_fields[which].shift) & ((1 << k_fields[which].bits) - 1);
}

void arcade_setting_set(int which, int value) {
    if (which < 0 || which >= AS_COUNT) return;
    if (value < 0) value = 0;
    if (value > k_fields[which].max) value = k_fields[which].max;
    int idx = k_fields[which].byte;
    uint8_t mask = (uint8_t)(((1 << k_fields[which].bits) - 1) << k_fields[which].shift);
    uint8_t b = (uint8_t)((eeprom_get(idx) & ~mask) | ((value << k_fields[which].shift) & mask));
    eeprom_set(idx, b);
    /* the running game reads its RAM copy, so update that too (only once the game has loaded it) */
    if (MEM_R32(RAM_GAME) == 0) return;   /* not loaded yet: the game will read the EEPROM at boot */
    uint32_t ram = idx < 0x1C ? RAM_COIN + (uint32_t)(idx - EEPROM_SETTINGS) : RAM_GAME + (uint32_t)(idx - 0x1C);
    MEM_W8(ram, b);
}
