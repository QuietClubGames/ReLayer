/*
 * An extra fighter on the select screen without replacing anyone (docs/NEW_CHARACTER_SLOT.md, docs/SFEX2.md).
 *
 * The select screen's cursor runs over a ring of 20 spots, D_802E9C84[slot] = { s16 id, s16 x } (x = -1: off the
 * strip, like the secret characters). Spot 20's memory is an unused copy, so the extra fighter goes there: past
 * the right end of the ring. Internally the extra fighter is a *variant of a donor character* (Kato, id 00):
 * spot 20 holds the donor's id, so every table the game indexes by character id (files, model, skeleton,
 * moves, names...) works unchanged, and this file remembers which player picked spot 20. Loading for that
 * player is marked (roster_loading_extra) so the file hook can serve the extra fighter's own files, and the
 * other player can still pick the donor himself.
 *
 * On the select and VS screens the spot shows its own portrait and name through a *display id*, SHOWN = 0x0D:
 * a boss whose own spot is locked and whose name entry is blank. Spot 20 holds SHOWN, so the select screen
 * looks up record/name 0x0D; her portrait is served as SEL2/PS_0D.TIM from her folder (only while her side is
 * shown), and the name table entry 0x0D points at her name while those screens are drawn. On confirm the
 * player's id is set back to the donor; the VS screen functions see SHOWN again for the duration of their call.
 *
 * Enabled when there is a folder with the extra fighter's files (extra_dir), so a game without one is unchanged.
 */
#include "recomp.h"
#include "log.h"
#include "hw.h"
#include "gpu.h"
#include "gpu_hd.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING 0x802E9C84u
#define EXTRA_SLOT 20
#define DONOR 0x00
#define SHOWN 0x0D                 /* display id (Lance Knight: locked spot, blank name) */
#define NAMES 0x802E8DB4u          /* name string pointers by id */
#define NAME_RAM 0x802E9D74u       /* her name string: an unused copy of the ring (80 bytes) */
#define SEL_ID 0x80335434u         /* + side: the id chosen on the select screen */
#define PLAYER(side) ((side) ? 0x8034BC7Cu : 0x8034A100u)

/* The extra fighter's folder: FGT_FILE_DIR (development, e.g. ex2mod), or "extra" in the game's folder, which Setup
 * makes from the player's own Street Fighter EX2 Plus ROM (tools/install/build_from_rom.py). NULL: none. */
const char *extra_dir(void) {
    static int init;
    static const char *dir;
    if (!init) {
        init = 1;
        dir = getenv("FGT_FILE_DIR");
        if (!dir) {
            FILE *f = fopen("extra/NAME.TXT", "rb");
            if (f) { fclose(f); dir = "extra"; }
        }
    }
    return dir;
}

void f_802A9FCC_orig(void);
void f_802A9D54_orig(void);
void f_802AA24C_orig(void);
void f_802B46EC_orig(void);
void f_802B4E80_orig(void);
void f_8029C358_orig(void);

static int g_enabled = -1;
static int g_extra[2];            /* side 0/1 plays the extra fighter (picked, or an arcade opponent) */
static int g_picked[2];           /* side 0/1 picked her on the select screen */
static int g_loading_extra;
static char g_name[9] = "PULLUM";
static void plate_restore(void);

int roster_extra_enabled(void) {
    if (g_enabled < 0) {
        g_enabled = extra_dir() != NULL;
        if (g_enabled) {
            char name[32] = "PULLUM", spaced[18];
            const char *dir = extra_dir();
            char path[512];
            snprintf(path, sizeof path, "%s/NAME.TXT", dir);
            FILE *f = fopen(path, "rb");
            if (f) { size_t n = fread(name, 1, 8, f); name[n] = 0; fclose(f); }
            for (char *c = name; *c; c++) if (*c < ' ') *c = 0; else *c = (char)toupper((unsigned char)*c);
            snprintf(g_name, sizeof g_name, "%s", name);
            /* the game's names are 17 characters, letters spaced out and centred: "     K A T O     " */
            int len = (int)strlen(name), w = 2 * len - 1, pad = (17 - w) / 2;
            memset(spaced, ' ', 17);
            spaced[17] = 0;
            for (int i = 0; i < len && pad + 2 * i < 17; i++) spaced[pad + 2 * i] = name[i];
            for (int i = 0; i < 18; i++) MEM_W8(NAME_RAM + (uint32_t)i, (uint8_t)spaced[i]);
            MEM_W16(RING + 4u * EXTRA_SLOT, SHOWN);
            MEM_W16(RING + 4u * EXTRA_SLOT + 2, 0xFFFF);
            rt_log("roster: extra fighter on select spot %d (variant of character %02X)", EXTRA_SLOT, DONOR);
        }
    }
    return g_enabled;
}

int roster_is_extra(uint32_t player) {
    if (player == PLAYER(0)) return g_extra[0];
    if (player == PLAYER(1)) return g_extra[1];
    return 0;
}

static int g_loading_side = -1;
int roster_loading_extra(void) { return g_loading_extra; }
int roster_loading_side(void) { return g_loading_extra ? g_loading_side : -1; }
void file_skeleton_begin(int side);
void file_skeleton_end(int side);
void ex_pullum_block_loaded(uint32_t block);
void ex_pullum_selected(int side);

static uint32_t cursor(uint32_t work, uint32_t side) { return work + 8u + 2u * side; }

/* The select screen's state machine (func_802A794C) runs every frame while it is up; when it starts again
 * after a gap, it is a new selection: forget the previous picks (a computer opponent never confirms). */
void f_802A794C_orig(void);
void f_802A794C(void) {
    static uint64_t last = (uint64_t)-10;
    if (g_frame - last > 4) { g_extra[0] = g_extra[1] = g_picked[0] = g_picked[1] = 0; plate_restore(); }
    last = g_frame;
    f_802A794C_orig();
}

/* cursor right: ... -> last open spot -> extra spot -> wrap to the start */
void f_802A9FCC(void) {
    uint32_t work = R(4), side = R(5) & 1;
    if (!roster_extra_enabled()) { f_802A9FCC_orig(); return; }
    int16_t old = (int16_t)MEM_R16(cursor(work, side));
    if (old == EXTRA_SLOT) {
        MEM_W16(cursor(work, side), EXTRA_SLOT - 1);   /* move on from the ring's last spot (wraps to 0) */
        f_802A9FCC_orig();
        return;
    }
    f_802A9FCC_orig();
    if ((int16_t)MEM_R16(cursor(work, side)) < old) MEM_W16(cursor(work, side), EXTRA_SLOT);   /* it wrapped */
}

/* cursor left: start -> extra spot -> last open spot */
void f_802A9D54(void) {
    uint32_t work = R(4), side = R(5) & 1;
    if (!roster_extra_enabled()) { f_802A9D54_orig(); return; }
    int16_t old = (int16_t)MEM_R16(cursor(work, side));
    if (old == EXTRA_SLOT) {
        MEM_W16(cursor(work, side), 0);              /* move on from spot 0 (wraps to the last open spot) */
        f_802A9D54_orig();
        return;
    }
    f_802A9D54_orig();
    if ((int16_t)MEM_R16(cursor(work, side)) > old) MEM_W16(cursor(work, side), EXTRA_SLOT);
}

/* ---- the arcade ladder: her as a computer opponent ----
 * func_802ABB84(side) builds a player's arcade ladder (D_8034A040 + 16 * side, 9 fights) at select confirm and
 * after an ending: fights 0, 1, 3 and 4 are regular fighters drawn from ids 00-09 (not the player's own), the
 * rest are fixed (Lance Knight twice, the sub-boss pairs, the boss: docs/NEW_CHARACTER_SLOT.md). She joins the
 * regular draw as an eleventh name: with the chance a regular fighter has (4 of 10) one of those four fights is
 * hers, written as the donor's id and remembered here; the arcade screens before that fight (func_80202180)
 * mark the computer's side as her, so she is loaded and named as when picked. Never when the player is her. */
#define LADDER(side) (0x8034A040u + 16u * (uint32_t)(side))
static int g_ladder_her[2] = {-1, -1};   /* the fight of side's ladder that is hers, or -1 */
static uint8_t g_ladder_was[2];          /* the id it had */
static uint32_t g_ladder_seed;

static void ladder_forget(int side) {
    if (g_ladder_her[side] < 0) return;
    if (MEM_R8(LADDER(side) + (uint32_t)g_ladder_her[side]) == DONOR)
        MEM_W8(LADDER(side) + (uint32_t)g_ladder_her[side], g_ladder_was[side]);
    g_ladder_her[side] = -1;
}

void f_802ABB84_orig(void);
void f_802ABB84(void) {
    int side = (int)(R(4) & 1);
    f_802ABB84_orig();
    g_ladder_her[side] = -1;
    if (!roster_extra_enabled() || g_picked[side]) return;
    uint32_t r = g_ladder_seed += (uint32_t)g_frame * 0x9E3779B9u + 0x7F4A7C15u;   /* frame-seeded, mixed */
    r ^= r >> 16; r *= 0x85EBCA6Bu; r ^= r >> 13; r *= 0xC2B2AE35u; r ^= r >> 16;
    if (r % 10 >= 4) return;
    static const int fights[4] = {0, 1, 3, 4};
    int fight = fights[(r / 10) % 4];
    g_ladder_was[side] = MEM_R8(LADDER(side) + (uint32_t)fight);
    MEM_W8(LADDER(side) + (uint32_t)fight, DONOR);
    g_ladder_her[side] = fight;
    rt_log("roster: side %d's arcade fight %d is the extra fighter", side, fight + 1);
}

/* confirm: remember whether this side picked the extra spot */
void f_802AA24C(void) {
    uint32_t work = R(4), side = R(5) & 1;
    int extra = roster_extra_enabled() && (int16_t)MEM_R16(cursor(work, side)) == EXTRA_SLOT;
    f_802AA24C_orig();
    if (extra) {                                  /* she is loaded and played as a variant of the donor */
        MEM_W8(PLAYER(side) + 2, DONOR);
        MEM_W8(SEL_ID + side, DONOR);
        if (!g_extra[side]) ex_pullum_selected((int)side);   /* her line, once per pick */
    }
    if (extra != g_extra[side]) rt_log("roster: side %u %s the extra fighter", side, extra ? "picked" : "left");
    g_extra[side] = g_picked[side] = extra;
    if (extra) ladder_forget(side);               /* she does not fight herself */
}

/* Loading a fight (func_802B4D68) loads each fighter with func_802B46EC(side) (files), func_802B4E80(side, dest)
 * (code block) and func_8029C358(player) (model and skeleton setup); mark the extra fighter's loads. */
static void load_as(int side, void (*orig)(void)) {
    g_loading_extra = side >= 0 && g_extra[side] && MEM_R8(PLAYER(side) + 2) == DONOR;
    g_loading_side = side;
    orig();
    g_loading_extra = 0;
    g_loading_side = -1;
}
void f_802B46EC(void) { load_as((int)(R(4) & 1), f_802B46EC_orig); }
/* the code block (func_802B4E80(side, dest)): her movement constants (walk speeds, jump velocities: block word
 * 12, 11 words, the same layout in both games) from <her folder>/MOVE/P00.MOV, into her copy of the block */
void f_802B4E80(void) {
    int side = (int)(R(4) & 1);
    uint32_t block = R(5);
    load_as(side, f_802B4E80_orig);
    if (!roster_extra_enabled() || !g_extra[side] || MEM_R8(PLAYER(side) + 2) != DONOR) return;
    ex_pullum_block_loaded(block);                 /* her move data in her copy (src/game/ex_pullum.c) */
    char path[512];
    snprintf(path, sizeof path, "%s/MOVE/P%02X.MOV", extra_dir(), DONOR);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    int32_t v[11];
    size_t n = fread(v, 4, 11, f);
    fclose(f);
    uint32_t table = MEM_R32(block + 0x30);
    if (n != 11 || (table >> 24) != 0x80) return;
    for (int i = 0; i < 11; i++) MEM_W32(table + 4u * (uint32_t)i, (uint32_t)v[i]);
    rt_log("roster: her movement constants in the block at %08X", block);
}
void f_8029C358(void) {
    uint32_t p = R(4);
    int side = p == PLAYER(0) ? 0 : p == PLAYER(1) ? 1 : -1;
    int extra = side >= 0 && g_extra[side] && MEM_R8(p + 2) == DONOR;
    if (extra) file_skeleton_begin(side);      /* her bone lengths for this set-up only */
    load_as(side, f_8029C358_orig);
    if (extra) file_skeleton_end(side);
}

/* ---- select and VS screens: her portrait and name ---- */
void f_802AA6F4_orig(void);
void f_802A8208_orig(void);
void f_802AAF0C_orig(void);
void f_80203254_orig(void);
void f_80203E58_orig(void);
void f_80203FE0_orig(void);

/* run a screen function with name entry SHOWN pointing at her name and the extra players shown as SHOWN */
static void as_shown(void (*orig)(void), int load_side) {
    if (!roster_extra_enabled()) { orig(); return; }
    uint32_t saved_name = MEM_R32(NAMES + 4u * SHOWN);
    int ids[2] = {-1, -1};
    MEM_W32(NAMES + 4u * SHOWN, NAME_RAM);
    for (int side = 0; side < 2; side++)
        if (g_extra[side] && MEM_R8(PLAYER(side) + 2) == DONOR) { ids[side] = DONOR; MEM_W8(PLAYER(side) + 2, SHOWN); }
    g_loading_extra = load_side >= 0;
    g_loading_side = load_side;
    orig();
    g_loading_extra = 0;
    g_loading_side = -1;
    for (int side = 0; side < 2; side++) if (ids[side] >= 0) MEM_W8(PLAYER(side) + 2, (uint8_t)ids[side]);
    MEM_W32(NAMES + 4u * SHOWN, saved_name);
}

/* select screen: the big portrait for a side (work, side) */
void f_802AA6F4(void) {
    uint32_t work = R(4), side = R(5) & 1;
    int extra = roster_extra_enabled() && (int16_t)MEM_R16(cursor(work, side)) == EXTRA_SLOT;
    as_shown(f_802AA6F4_orig, extra ? (int)side : -1);
}
void f_802A8208(void) { as_shown(f_802A8208_orig, -1); }      /* select screen names */
void f_802AAF0C(void) { as_shown(f_802AAF0C_orig, -1); }
void f_80203254(void) { as_shown(f_80203254_orig, -1); }      /* VS screen names */
void f_80203E58(void) { uint32_t side = R(5) & 1; as_shown(f_80203E58_orig, g_extra[side] ? (int)side : -1); }
void f_80203FE0(void) { uint32_t side = R(5) & 1; as_shown(f_80203FE0_orig, g_extra[side] ? (int)side : -1); }

/* the arcade screens before a fight (func_80202180: the stage map and the arcade VS screen, every frame) look up
 * the fighters' names by id, and hers is the donor's: the donor's name entry points at hers meanwhile. If the
 * other fighter is the donor himself, both are shown through SHOWN instead, as on the other screens. */
void f_80202180_orig(void);
void f_80202180(void) {
    if (roster_extra_enabled()) {                  /* is this fight of the ladder hers? */
        int human = (MEM_R32(0x80335454u) & 4) ? 1 : 0, cpu = human ^ 1;
        int fight = MEM_R8(0x80335438u + (uint32_t)human);
        int hers = g_ladder_her[human] >= 0 && fight == g_ladder_her[human] &&
                   MEM_R8(LADDER(human) + (uint32_t)fight) == DONOR;
        g_extra[cpu] = g_picked[cpu] || hers;
    }
    int her = -1, donor_too = 0;
    for (int side = 0; side < 2; side++)
        if (MEM_R8(PLAYER(side) + 2) == DONOR) { if (g_extra[side]) her = side; else donor_too = 1; }
    if (!roster_extra_enabled() || her < 0) { f_80202180_orig(); return; }
    if (donor_too) { as_shown(f_80202180_orig, -1); return; }
    uint32_t saved = MEM_R32(NAMES + 4u * DONOR);
    MEM_W32(NAMES + 4u * DONOR, NAME_RAM);
    f_80202180_orig();
    MEM_W32(NAMES + 4u * DONOR, saved);
}

/* ---- fight HUD: her name plate ----
 * func_8020FF04 sets up the HUD's name plates: 80x16 sprites (records at 0x803680B0 + 0x32C/0x340 for player 1,
 * + 0x5E8/0x5FC for player 2: { tag, rgb+code, x y, u v clut, w h }) with u,v from D_802DC8E0[id], in a 4-bit
 * texture page at VRAM (768,256) (CLUT (832,495)). The page has no free 80x16 spot, so her plate is written over
 * a plate nobody in this fight uses (Lance Knight's, or the donor's), put together from the letters of the other
 * names; the plate is restored at the next HUD setup or selection. */
#define PLATES 0x802DC8E0u
#define HUD 0x803680B0u
#define PAGE_X 768
#define PAGE_Y 256

/* letters in the page: plate u, v and the glyph's columns [x0, x1) (KATO, GEORGE, HONG, LAN, SHANG, JIGJID,
 * SESSYU, CAPRICCIO, AJAX, CLEMENCE, SHARK, FALCON, VOLD, BLAIR); no Q, W or Z */
static const struct { char c; uint8_t u, v, x0, x1; } k_letters[] = {
    {'A',   0,  32, 23, 32}, {'B', 160, 128,  0,  9}, {'C',   0,  80,  0,  9}, {'D',  80,  64, 70, 79},
    {'E',  80,  32, 16, 24}, {'F',  80, 112,  0,  8}, {'G',  80,  32,  2, 11}, {'H', 160,  32,  0,  9},
    {'I',  80,  64, 17, 22}, {'J',  80,  64,  1, 10}, {'K',   0,  32,  0,  9}, {'L',   0,  48,  0,  8},
    {'M', 160,  80, 30, 40}, {'N', 160,  32, 47, 56}, {'O',   0,  32, 71, 80}, {'P',   0,  80, 20, 29},
    {'R',  80,  32, 43, 52}, {'S',   0,  64,  0,  9}, {'T',   0,  32, 47, 56}, {'U', 160,  64, 70, 79},
    {'V', 160, 112,  0, 10}, {'X',  80,  80, 70, 79}, {'Y', 160,  64, 56, 65},
};

static struct { int active, u, v; uint16_t saved[16][20]; } g_plate;

static int texel(int u, int v) {
    uint16_t w = g_vram[((PAGE_Y + v) << 10) | (PAGE_X + u / 4)];
    return (w >> (4 * (u & 3))) & 15;
}

static void plate_restore(void) {
    if (!g_plate.active) return;
    hd_vram_written(PAGE_X + g_plate.u / 4, PAGE_Y + g_plate.v, 20, 16);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 20; x++) g_vram[((PAGE_Y + g_plate.v + y) << 10) | (PAGE_X + g_plate.u / 4 + x)] = g_plate.saved[y][x];
    g_plate.active = 0;
}

/* write her name into the plate at (u, v), letters spread over the 80 pixels like the game's own plates */
static void plate_build(int u, int v) {
    uint8_t pix[16][80];
    int idx[8], n = 0, width = 0;
    memset(pix, 0, sizeof pix);
    for (const char *c = g_name; *c && n < 8; c++)
        for (int i = 0; i < (int)(sizeof k_letters / sizeof k_letters[0]); i++)
            if (k_letters[i].c == *c) { idx[n++] = i; width += k_letters[i].x1 - k_letters[i].x0; break; }
    int x = n == 1 ? (80 - width) / 2 : 0, gaps = n > 1 ? 80 - width : 0;
    for (int k = 0; k < n; k++) {
        int i = idx[k], w = k_letters[i].x1 - k_letters[i].x0;
        for (int yy = 0; yy < 16; yy++)
            for (int xx = 0; xx < w; xx++)
                if (x + xx >= 0 && x + xx < 80) pix[yy][x + xx] = (uint8_t)texel(k_letters[i].u + k_letters[i].x0 + xx, k_letters[i].v + yy);
        x += w;
        if (k < n - 1) x += gaps * (k + 1) / (n - 1) - gaps * k / (n - 1);
    }
    g_plate.u = u;
    g_plate.v = v;
    for (int y = 0; y < 16; y++)
        for (int w = 0; w < 20; w++) g_plate.saved[y][w] = g_vram[((PAGE_Y + v + y) << 10) | (PAGE_X + u / 4 + w)];
    hd_vram_written(PAGE_X + u / 4, PAGE_Y + v, 20, 16);
    for (int y = 0; y < 16; y++)
        for (int w = 0; w < 20; w++)
            g_vram[((PAGE_Y + v + y) << 10) | (PAGE_X + u / 4 + w)] =
                (uint16_t)(pix[y][4 * w] | pix[y][4 * w + 1] << 4 | pix[y][4 * w + 2] << 8 | pix[y][4 * w + 3] << 12);
    g_plate.active = 1;
}

void f_8020FF04_orig(void);
void f_8020FF04(void) {
    f_8020FF04_orig();
    if (!roster_extra_enabled()) return;
    plate_restore();
    int extra[2], any = 0;
    for (int side = 0; side < 2; side++) any |= extra[side] = g_extra[side] && MEM_R8(PLAYER(side) + 2) == DONOR;
    /* the page this was worked out from: Kato's plate at (0,32), the HUD's plate CLUT */
    if (!any || MEM_R8(PLATES) != 0 || MEM_R8(PLATES + 2) != 32 || MEM_R16(HUD + 0x32C + 0xE) != 0x7BF4) return;
    /* a plate nobody else in this fight shows */
    static const int candidates[] = {SHOWN, DONOR};
    int u = -1, v = -1;
    for (int c = 0; c < 2 && u < 0; c++) {
        uint32_t e = PLATES + 4u * (uint32_t)candidates[c];
        int cu = MEM_R8(e), cv = MEM_R8(e + 2), used = 0;
        for (int side = 0; side < 2; side++) {
            if (extra[side]) continue;
            uint32_t o = PLATES + 4u * MEM_R8(PLAYER(side) + 2);
            if (MEM_R8(o) == cu && MEM_R8(o + 2) == cv) used = 1;
        }
        if (!used) { u = cu; v = cv; }
    }
    if (u < 0) return;
    plate_build(u, v);
    static const uint32_t recs[2][2] = {{0x32C, 0x340}, {0x5E8, 0x5FC}};
    for (int side = 0; side < 2; side++)
        if (extra[side])
            for (int r = 0; r < 2; r++) { MEM_W8(HUD + recs[side][r] + 0xC, (uint8_t)u); MEM_W8(HUD + recs[side][r] + 0xD, (uint8_t)v); }
    rt_log("roster: her HUD name plate at (%d,%d)", u, v);
}

/* An attract-mode demo fight is being set up (func_802A3C48 writes the demo pairing's ids to player+2): the
 * players' picks are over, or a demo Kato on that side would be loaded as the extra fighter. */
void f_802A3C48_orig(void);
void f_802A3C48(void) {
    if (g_extra[0] || g_extra[1]) rt_log("roster: attract demo, picks forgotten");
    g_extra[0] = g_extra[1] = 0;
    plate_restore();
    f_802A3C48_orig();
}
