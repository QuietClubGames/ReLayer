/*
 * Uncensored: the game as it was released in Japan, made from the World ROM.
 *
 * The two dumped program ROMs are the same code. They differ only in a region word at 0x1FC201FE, which
 * func_8022306C turns into a region byte at 0x803689B8 at power-on: 0 for the Japanese revision ("FTL0",
 * word 0xFFFF), 3 for World ("FTL3", word 0xFC02), and 1, 2, 4 for others. Censorship comes from two places:
 *
 * - The region. With it set to World, Sessyu's sword and Janis's knives are drawn as sticks; Lan's, Clemence's
 *   and Blair's chest physics are skipped (their move code tests "region == 3"); the VS screen names Tetsuo by
 *   his surname, Kato. Japan also shows the Namco logo and its own title picture.
 * - Operator options in the game-options word at 0x80368CB4 (EEPROM 0x1C-0x1F, see arcade_settings.c).
 *   Bit 23 is REGULATION (green sparks, purple fire and light effects, no "SUPER FINISH!" banner, and per
 *   tcrf.net some moves switched off), bit 21 is HIT COLOR (1 = red). The Japanese test menu shows both; the
 *   World one hides them and its defaults keep Regulation on. Bits 19-22 are further region-specific options
 *   whose defaults differ by region; bits 19-23 together are copied from the defaults table at 0x802EA040:
 *   Japan 0x0162925A, World 0x018A925A.
 *
 * Both are applied once a frame, so they hold from power-on (func_8022306C runs during boot and is
 * overwritten a frame later, before the logos) and after the operator menu saves its options. The EEPROM
 * copy is left alone: switching this off gives the World behaviour back.
 */
#include "recomp.h"
#include "log.h"
#include "game.h"

int g_uncensored = 1;

#define RAM_REGION        0x803689B8u
#define RAM_GAME_OPTIONS  0x80368CB4u
#define REGION_JAPAN      0
#define REGION_WORLD      3
#define REGION_BITS       0x00F80000u   /* option bits 19-23 */
#define JAPAN_OPTIONS     0x0162925Au   /* the region defaults, from 0x802EA040 */
#define WORLD_OPTIONS     0x018A925Au

void uncensored_frame(void) {
    static int logged = -1;
    uint8_t region = g_uncensored ? REGION_JAPAN : REGION_WORLD;
    if (MEM_R8(RAM_REGION) != region) MEM_W8(RAM_REGION, region);
    uint32_t opts = MEM_R32(RAM_GAME_OPTIONS);
    if (opts) {   /* 0 until the game has loaded its options from the EEPROM */
        uint32_t want = (opts & ~REGION_BITS) | ((g_uncensored ? JAPAN_OPTIONS : WORLD_OPTIONS) & REGION_BITS);
        if (want != opts) MEM_W32(RAM_GAME_OPTIONS, want);
    }
    if (logged != g_uncensored) {
        rt_log("uncensored: %s", g_uncensored ? "on (Japanese version, Regulation off, red sparks)" : "off (World version)");
        logged = g_uncensored;
    }
}
