/* Tests for the controller -> cabinet mapping (port/pc/pad_map.c). Run: ctest --test-dir build/pc */
#include "../port/pc/pad.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static PadRaw pad(uint16_t buttons, int lx, int ly, int lt, int rt) {
    PadRaw p = {1, buttons, (uint8_t)lt, (uint8_t)rt, (int16_t)lx, (int16_t)ly};
    return p;
}

int main(void) {
    PadMap m;
    padmap_defaults(&m);
    int coin;

    /* default face buttons: X Y RB = 1 2 3, A B RT = 4 5 6 */
    CHECK(padmap_player_bits(&m, &(PadRaw){1, 0x4000}, NULL) == 0x02, "X -> b1");
    CHECK(padmap_player_bits(&m, &(PadRaw){1, 0x8000}, NULL) == 0x01, "Y -> b2");
    CHECK(padmap_player_bits(&m, &(PadRaw){1, 0x0200}, NULL) == 0x8000, "RB -> b3");
    CHECK(padmap_player_bits(&m, &(PadRaw){1, 0x1000}, NULL) == 0x4000, "A -> b4");
    CHECK(padmap_player_bits(&m, &(PadRaw){1, 0x2000}, NULL) == 0x2000, "B -> b5");
    PadRaw rt = pad(0, 0, 0, 0, 255);
    CHECK(padmap_player_bits(&m, &rt, NULL) == 0x1000, "RT -> b6");
    PadRaw rt_light = pad(0, 0, 0, 0, 20);
    CHECK(padmap_player_bits(&m, &rt_light, NULL) == 0, "RT barely touched does nothing");
    CHECK(padmap_player_bits(&m, &(PadRaw){1, 0x0010}, NULL) == 0x80, "START -> start");
    CHECK(padmap_player_bits(&m, &(PadRaw){1, 0x0020}, &coin) == 0 && coin == 1, "BACK -> coin");

    /* D-pad */
    CHECK(padmap_player_bits(&m, &(PadRaw){1, 0x0001}, NULL) == 0x20, "dpad up");
    CHECK(padmap_player_bits(&m, &(PadRaw){1, 0x0009}, NULL) == 0x24, "dpad up-right");
    CHECK(padmap_player_bits(&m, &(PadRaw){1, 0x000C}, NULL) == 0, "dpad left+right cancel");

    /* left stick, 8 sectors */
    struct { int x, y; uint16_t want; const char *name; } st[] = {
        {30000, 0, 0x04, "right"}, {21000, 21000, 0x24, "up-right"}, {0, 30000, 0x20, "up"},
        {-21000, 21000, 0x28, "up-left"}, {-30000, 0, 0x08, "left"}, {-21000, -21000, 0x18, "down-left"},
        {0, -30000, 0x10, "down"}, {21000, -21000, 0x14, "down-right"},
        {30000, 9000, 0x04, "mostly right stays right"}, {5000, 5000, 0, "inside deadzone"},
    };
    for (unsigned i = 0; i < sizeof st / sizeof st[0]; i++) {
        PadRaw p = pad(0, st[i].x, st[i].y, 0, 0);
        uint16_t got = padmap_player_bits(&m, &p, NULL);
        CHECK(got == st[i].want, "stick %s: got %04X want %04X", st[i].name, got, st[i].want);
    }

    /* disconnected pad gives nothing */
    PadRaw off = {0, 0xFFFF, 255, 255, 30000, 30000};
    CHECK(padmap_player_bits(&m, &off, &coin) == 0 && coin == 0, "disconnected pad is ignored");

    /* config file remapping */
    FILE *f = fopen("pad_map_test.ini", "w");
    fprintf(f, "# test\nb1 = A\nb4 = x\ncoin = BACK LB\nb6 = none\nstick_deadzone = 0.8\nbogus = A\n");
    fclose(f);
    PadMap c;
    padmap_defaults(&c);
    CHECK(padmap_load(&c, "pad_map_test.ini") == 1, "config loads");
    CHECK(padmap_player_bits(&c, &(PadRaw){1, 0x1000}, NULL) == 0x02, "remapped A -> b1");
    CHECK(padmap_player_bits(&c, &(PadRaw){1, 0x4000}, NULL) == 0x4000, "remapped X -> b4 (lower case name)");
    CHECK(padmap_player_bits(&c, &(PadRaw){1, 0x0100}, &coin) == 0 && coin == 1, "second coin button LB");
    CHECK(padmap_player_bits(&c, &rt, NULL) == 0, "b6 unmapped");
    PadRaw half = pad(0, 20000, 0, 0, 0);
    CHECK(padmap_player_bits(&c, &half, NULL) == 0, "larger deadzone from config");
    remove("pad_map_test.ini");
    CHECK(padmap_load(&c, "does_not_exist.ini") == 0, "missing file reported");

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("all controller mapping checks passed\n");
    return 0;
}
