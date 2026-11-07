/*
 * Character release ("time release").
 *
 * The arcade releases its secret characters gradually: func_802B3F98 returns the hours elapsed between the
 * release start date (saved at first power-on, RAM 0x80318230 / EEPROM 0x2A0) and the board's real-time
 * clock, and the callers compare it with:
 *     360 h (15 days)  first secret character      696 h (29 days)  second
 *    1032 h (43 days)  third                        1200 h, 1489 h   later stages
 * It returns 0 when either date is invalid (e.g. a board without a working clock), which is why nothing
 * ever unlocks without a clock.
 *
 * Replacement: with g_unlock_all set, report a board that has been running long enough for everything;
 * otherwise run the original calculation.
 */
#include "recomp.h"
#include "game.h"

int g_unlock_all = 1;

#define RELEASE_ALL_HOURS 2000u      /* ~83 days: past every threshold the game checks */

void f_802B3F98_orig(void);

void f_802B3F98(void) {
    if (g_unlock_all) {
        R(2) = RELEASE_ALL_HOURS;
        return;
    }
    f_802B3F98_orig();
}
