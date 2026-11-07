/*
 * Pullum Purna's own moves (Street Fighter EX2 Plus char 04), written against Fighting Layer's engine
 * (docs/EX2P_MOVE_CODE.md, docs/EX2P_HIT.md). She plays as a variant of Kato (src/game/roster.c), so Kato's
 * code block runs for her; here its normal-attack handler (block word 0, func_8009FEB4 in slot 1,
 * func_800AFEB4 in slot 2) is replaced for the moves she has of her own, and Kato's handles the rest.
 *
 * Her moves play her own motions, which the converter (tools/sfex/make_swap.py MOTION_MAP / HIT_MAP) puts in
 * new slots of her motion file (bank 3, 0x41 up) with hit records in her HIT file: starting a motion through
 * func_802145F0 latches its hit record, and the engine's collision pass (func_8021FBEC) does the rest.
 *
 * Normal-attack state (18), as Kato's handler and hers in EX2 Plus (80043128) run it:
 *   entry (player+5 = 0): start the motion;
 *   every frame: the swing sound at its frame; step the motion (func_80215338); in the air, move and land
 *   (state 0x0C); back to neutral when the motion ends (func_80288DB8); from the "free" frame on, state 0x11:
 *   the motion keeps playing but she can act again (EX2 Plus: 8021B1C8(p, 0x11)).
 */
#include "recomp.h"
#include "log.h"
#include "hw.h"
#include "gpu.h"
#include "gpu_hd.h"
#include "exsnd.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int roster_is_extra(uint32_t player);
const char *extra_dir(void);

void f_802145F0(void);   /* set motion (player, motion): latches its hit record */
void f_80215338(void);   /* motion step */
void f_80289104(void);   /* stop moving */
void f_80289164(void);   /* integrate velocity */
void f_8028E9F0(void);   /* landed? */
void f_80290784(void);   /* set state */
void f_802907EC(void);   /* set state, keeping the motion */
void f_80288DB8(void);   /* end of move: back to neutral */
void f_8028DB58(void);
void f_802A071C(void);   /* sound (side, id) */
void f_8028EE14(void);   /* chain (player, record): restarts the attack if the record's button came */
void f_802CE4DC(void);   /* rsin */
void f_802CE5AC(void);   /* rcos */
void f_80214568(void);   /* set motion without latching a hit record */
void f_80286CC8(void);   /* command matcher (player, state, table, &strength, id on the stack) -> buttons */
void f_8028F344(void);   /* may this move start now? (player, class, id) */
void f_80290188(void);   /* commands locked? */
void f_8028A9B4(void);   /* turn (player, angle) */
void f_8028AC4C(void);   /* turn towards the opponent (player, step) */
void f_8028B9D8(void);   /* a node's world position (player, out) */
void f_80288F90(void);   /* velocity from the facing and the speed +0x40/+0x44 */
void f_8028A638(void);   /* stop when the speed reaches 0 */
void f_8028ADFC(void);   /* spin set-up (player, saved root) */
void f_8028AEF0(void);   /* spin step (player, pivot, angular velocity, saved root): motion step, body turned */
void f_8028E178(void);   /* a super starts: 50 gauge, the super flash, its sound and announcer */
void f_8028EBFC(void);   /* an attack button in the air or neutral starts a normal (-> 1) */
void f_8021483C(void);   /* set motion with blend frames (player, motion, frames) */
void f_8028DBD0(void);
void f_802145A0(void);   /* set the thrown opponent's motion, from the thrower's file (opponent, motion) */
void f_8021F234(void);   /* an attack block of a motion's hit record (player, motion, n) -> attributes */
void f_8027DBFC(void);
void f_8028CBAC(void);   /* a hit lands on this player: damage, sound, spark */
void f_8022A34C(void);   /* screen shake (strength, frames) */
void f_8028FCC0(void);   /* is the opponent in throw range? (player, reach, ...) */
void f_8028B174(void);   /* spin end (player, saved root): position and facing from the turned body */

/* A speed curve: { until this motion frame, speed } ... ended by frame 99 (the same format in both games; Fighting
 * Layer steps one with func_8028A6A0, EX2 Plus with 80215FE0). */
typedef int16_t Curve[2];
static const Curve k_heavy_kick_curve[] = {{10, 240}, {25, 0}, {35, -240}, {99, 0}};   /* EX2 Plus 8004125C */

/* A chain record: from a motion frame, a button (with direction flags: 0x4000 = down) starts another attack. The
 * same 12 bytes in both games (Fighting Layer func_8028EE14 / func_8028F010, EX2 Plus 80219FBC); the engine reads
 * them from game memory, so they are written into her copy of the code block over Kato's own chain records
 * (block + 0x260, used only by his normals, which she no longer runs). */
typedef struct {
    int16_t frame;
    uint16_t buttons;      /* 0x0FFF buttons, 0xF000 directions */
    uint8_t posture, pad;  /* +0x1B43 after the chain: 0 standing, 1 crouching */
    uint16_t motion;       /* her slot */
    int16_t blend;
    uint8_t move, pad2;    /* the new move id */
} Chain;
#define CHAINS 0x260u
static const Chain k_chains[4] = {   /* EX2 Plus 8004126C, 80041278, 80041284, 80041290 */
    {7, 0x0800, 0, 0, 0x341, 0, 0, 0},   /* standing light punch, again */
    {7, 0x4800, 1, 0, 0x347, 0, 6, 0},   /* ... or crouching light punch */
    {7, 0x4800, 1, 0, 0x347, 0, 6, 0},   /* crouching light punch, again */
    {9, 0x4400, 1, 0, 0x34A, 2, 9, 0},   /* crouching light kick, again */
};

/* ---- her commands (block word 1) ----
 * Her EX2 Plus command tables (word 1, 80043AD8) have Fighting Layer's format: { u16 type; u16 n; u16 dir[n];
 * u16 buttons } (directions U 0x1000 F 0x2000 D 0x4000 B 0x8000, buttons LP 0x800 LK 0x400 MP 0x100 MK 0x200
 * HP 0x80 HK 0x40). Fighting Layer's matcher (func_80286CC8) reads them, and its per-command state, from game
 * memory, so both go into her data area: her copy of Kato's special-move code (block + 0x3000), which the
 * recompiled game never reads (the overlay is recognised by its header, and by its code at 0x8009FEB4,
 * 0x800A0A20, 0x800A37B0 and 0x800A4C70 only if the header changed). Kato's own command tables in her copy are
 * disabled, so his word 1, which runs after hers, only does Fighting Layer's own checks. */
#define HER_DATA 0x3000u
#define CMD_STATES (HER_DATA + 0x00u)   /* 4 bytes per command */
#define CMD_TABLES (HER_DATA + 0x40u)   /* 20 bytes per command */
#define CMD_SIZE 20u
#define CMD_CROSSED (HER_DATA + 0x100u) /* the same tables with forward and back swapped (air, crossed up) */
#define CATCH (HER_DATA + 0x260u)       /* the Meteor Combo caught him (EX2 Plus +0x16F4); +4: its object */
#define METEOR (HER_DATA + 0x268u)      /* the Meteor Combo's dance: his step, hits so far, speed */
#define THROW_ATTR (HER_DATA + 0x1D0u)  /* a throw's hit attributes, changed (20 bytes) */
#define AIR_THROW (HER_DATA + 0x240u)   /* the air throw: the opponent's spin vectors (saved root, pivot, angular
                                         * velocity, 8 apart), then his step (EX2 Plus +0x1700..0x1714, +0x06) */
#define HOPPED (HER_DATA + 0x1F0u)      /* the air hop was used in this jump (EX2 Plus +0x171C) */
#define REHIT (HER_DATA + 0x1F4u)       /* multi-hit moves: phase, timer (EX2 Plus +0x06, +0x0A) */
#define TRAIL_PRIMS (HER_DATA + 0x800u) /* her leg trails' primitives: 2 buffers x 2 legs x 7 x 52 bytes */
#define SPIN (HER_DATA + 0x200u)        /* spinning moves: saved root, pivot, angular velocity (3 x s16 each, 8
                                         * apart), then a timer: EX2 Plus player +0x16F0/+0x16F8/+0x1700, +0x08 */

typedef struct {
    uint8_t id;            /* her special (word 2) */
    uint8_t cls;           /* Fighting Layer move class for func_8028F344: 7 specials, 0xF supers */
    uint8_t air;           /* 0 on the ground, 1 in the air */
    uint8_t gauge;         /* gauge needed (one level = 50) */
    uint16_t table[9];     /* type, n, directions..., buttons */
} Command;

static const Command k_commands[] = {      /* in EX2 Plus's order of checks */
    {9, 0xF, 0, 150, {0xF, 6, 0x4000, 0x6000, 0x2000, 0x4000, 0x6000, 0x2000, 0x0980}},   /* QCF x2 + 3 punches: Meteor
                                                                                           * Combo (800410D4) */
    {7, 0xF, 0, 50, {0, 5, 0x4000, 0x6000, 0x2000, 0x4000, 0x6000, 0x0640}},   /* QCF x2+K super (800410B8) */
    {5, 0xF, 0, 50, {0, 5, 0x4000, 0xC000, 0x8000, 0x4000, 0xC000, 0x0640}},   /* QCB x2+K super (800410A8) */
    {10, 0xF, 1, 50, {0, 5, 0x4000, 0x6000, 0x2000, 0x4000, 0x6000, 0x0980}},  /* air QCF x2+P super (800410E8) */
    {0, 7, 0, 0, {0, 3, 0x2000, 0x4000, 0x6000, 0x0640}},   /* DP+K: rising kick (EX2 Plus 80041064) */
    {3, 7, 0, 0, {0, 3, 0x4000, 0xC000, 0x8000, 0x0640}},   /* QCB+K: lunge into a flying kick (80041084) */
    {4, 7, 0, 0, {8, 0, 0}},   /* punch + kick of the same strength (type 8, both games): her guard break (80041090),
                                * with Fighting Layer's rules (Kato's 0x8009E15C: no gauge cost, held to charge) */
    {1, 7, 1, 0, {0, 3, 0x4000, 0x6000, 0x2000, 0x0640}},   /* air QCF+K: spinning dive (80041070) */
    {8, 7, 0, 0, {0, 3, 0x4000, 0x6000, 0x2000, 0x0640}},   /* QCF+K: flying spin, then a dive (8004109C) */
    {6, 7, 1, 0, {0, 3, 0x4000, 0xC000, 0x8000, 0x0980}},   /* air QCB+P: a hop in the air, once a jump (800410C8) */
};
#define NCOMMANDS ((int)(sizeof k_commands / sizeof k_commands[0]))

/* Kato's command tables (block + 0x150 ... 0x198) and his command-normal list (+0x1FC), off in her copy */
static void disable_donor_commands(uint32_t block) {
    static const uint16_t tables[] = {0x150, 0x15C, 0x160, 0x178, 0x184, 0x198};
    for (int i = 0; i < 6; i++) {
        uint32_t t = block + tables[i];
        uint16_t n = MEM_R16(t + 2);
        MEM_W16(t, 0);                       /* type 0 ... */
        MEM_W16(t + 4u + 2u * n, 0);         /* ... and no buttons: it never completes */
    }
    MEM_W16(block + 0x1FC, 0xFFFF);          /* the list ends at once */
}

/* ---- her computer player ----
 * AI/P00.AI (tools/sfex/ex2p_ai.py): her EX2 Plus AI scripts in Fighting Layer's form, with Fighting Layer's
 * own computer play blended in from Kato's (runs, the roll, projectile reactions, the two extra situations). Copied into unused RAM (0x8038E000 + 0x8000 * side, between
 * her generic motions and the ghost buffers, docs/NEW_CHARACTER_SLOT.md 2.4), its pointers relocated, and her block
 * copy's header words 5-9 (the AI tables the engine reads, 802824CC) pointed at it. "EXAI", u32 offsets of words
 * 5-9, u32 relocation count, u32 relocations, data. */
#define AI_BASE(side) (0x8038E000u + 0x8000u * (uint32_t)(side))
static uint8_t *g_ai_file;
static long g_ai_size = -1;
static uint32_t ai_rd32(const uint8_t *b) { return (uint32_t)b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

static void ai_apply(uint32_t block, int side) {
    if (g_ai_size < 0) {
        g_ai_size = 0;
        char path[512];
        snprintf(path, sizeof path, "%s/AI/P00.AI", extra_dir() ? extra_dir() : ".");
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            g_ai_file = n > 28 ? malloc((size_t)n) : NULL;
            if (g_ai_file && fread(g_ai_file, 1, (size_t)n, f) == (size_t)n && !memcmp(g_ai_file, "EXAI", 4)) g_ai_size = n;
            fclose(f);
        }
        rt_log("pullum: computer player %s%s", path, g_ai_size > 0 ? "" : " not found (Kato's play)");
    }
    if (g_ai_size <= 0) return;
    uint32_t nrel = ai_rd32(g_ai_file + 24), head = 28 + 4 * nrel;
    if ((long)head > g_ai_size || g_ai_size - (long)head > 0x8000) return;
    uint32_t base = AI_BASE(side), size = (uint32_t)(g_ai_size - (long)head);
    for (uint32_t i = 0; i < size; i++) MEM_W8(base + i, g_ai_file[head + i]);
    for (uint32_t i = 0; i < nrel; i++) {
        uint32_t at = base + ai_rd32(g_ai_file + 28 + 4 * i);
        MEM_W32(at, MEM_R32(at) + base);
    }
    for (int w = 0; w < 5; w++) MEM_W32(block + 4u * (uint32_t)(5 + w), base + ai_rd32(g_ai_file + 4 + 4 * (uint32_t)w));
}

/* her copy of the code block was loaded (src/game/roster.c) */
static void views_apply(int side);
void ex_pullum_block_loaded(uint32_t block) {
    disable_donor_commands(block);
    ai_apply(block, block == 0x800AE000u ? 1 : 0);
    views_apply(block == 0x800AE000u ? 1 : 0);
    for (int i = 0; i < NCOMMANDS; i++) {
        MEM_W32(block + CMD_STATES + 4u * (uint32_t)i, 0);
        for (int j = 0; j < 9; j++) {
            uint16_t v = k_commands[i].table[j], swapped = v;
            if (j >= 2 && j < 2 + k_commands[i].table[1])   /* directions: forward 0x2000 <-> back 0x8000 */
                swapped = (uint16_t)((v & ~0xA000u) | ((v & 0x2000) << 2) | ((v & 0x8000) >> 2));
            MEM_W16(block + CMD_TABLES + CMD_SIZE * (uint32_t)i + 2u * (uint32_t)j, v);
            MEM_W16(block + CMD_CROSSED + CMD_SIZE * (uint32_t)i + 2u * (uint32_t)j, swapped);
        }
    }
    for (int i = 0; i < 4; i++) {
        uint32_t a = block + CHAINS + 12u * (uint32_t)i;
        const Chain *c = &k_chains[i];
        MEM_W16(a, (uint16_t)c->frame);
        MEM_W16(a + 2, c->buttons);
        MEM_W8(a + 4, c->posture);
        MEM_W8(a + 5, 0);
        MEM_W16(a + 6, c->motion);
        MEM_W16(a + 8, (uint16_t)c->blend);
        MEM_W8(a + 10, c->move);
        MEM_W8(a + 11, 0);
    }
}

/* EX2 Plus move id (player+0x1B47: 0 LP 1 MP 2 HP 3 LK 4 MK 5 HK, +6 crouching, +12 jumping) -> her move */
typedef struct {
    uint16_t motion;      /* Fighting Layer motion id (her slot in bank 3) */
    uint8_t sound_frame;  /* swing sound from this motion frame */
    uint8_t free_frame;   /* she can act again from this frame (0: when the motion ends) */
    const Curve *curve;   /* moves her along (or NULL) */
    uint8_t chain, nchain;  /* chain records k_chains[chain ..] */
} Move;

static const Move *her_move(int id) {
    /* EX2 Plus 80043128's tables: motions D_8004114C, {sound, -, free} D_800412D8, step functions D_800410F8.
     * Her motions are in slots 0x41 + move (tools/sfex/make_swap.py MOTION_MAP). Light attacks chain (k_chains). */
    static const Move moves[21] = {
        [0] = {0x341, 3, 14, NULL, 0, 2},   /* standing light punch: EX2 Plus motion 0x321, 18 frames, hits on 2-6 */
        [1] = {0x342, 6, 21},   /* standing medium punch: 0x300, 30 frames, two hits from 5 */
        [2] = {0x343, 7, 34},   /* standing heavy punch: 0x322, 41 frames, hits on 7-17 */
        [3] = {0x344, 5, 20},   /* standing light kick: 0x323, 26 frames, hits on 3-7 */
        [4] = {0x345, 5, 26},   /* standing medium kick: shared motion 0x202, hits on 4-10 */
        [5] = {0x346, 10, 32, k_heavy_kick_curve},   /* standing heavy kick: 0x324, hits on 9-14, steps in */
        [6] = {0x347, 4, 13, NULL, 2, 1},   /* crouching light punch: shared 0x204 */
        [7] = {0x348, 5, 16},   /* crouching medium punch: shared 0x205 */
        [8] = {0x349, 7, 0},    /* crouching heavy punch: shared 0x207 */
        [9] = {0x34A, 3, 16, NULL, 3, 1},   /* crouching light kick: 0x325 (low) */
        [10] = {0x34B, 5, 20},  /* crouching medium kick: shared 0x209 (low) */
        [11] = {0x34C, 9, 0},   /* crouching heavy kick: shared 0x20A (low, sweep) */
        [12] = {0x34D, 3, 0},   /* jumping light punch: shared 0x20B */
        [13] = {0x34E, 4, 0},   /* jumping medium punch: shared 0x20C */
        [14] = {0x34F, 4, 0},   /* jumping heavy punch: shared 0x20D */
        [15] = {0x350, 3, 0},   /* jumping light kick: 0x326 */
        [16] = {0x351, 3, 0},   /* jumping medium kick: 0x33E */
        [17] = {0x352, 6, 0},   /* jumping heavy kick: 0x327 */
        /* command normals (word 0 turns 13, 16 into these at their start; 19 has its own steps) */
        [19] = {0x363, 0, 0},   /* jumping down + medium punch: a dive, 0x32D (cmd_dive) */
        [20] = {0x364, 4, 0},   /* jumping back + medium kick: 0x33F */
    };
    return id >= 0 && id < 21 && moves[id].motion ? &moves[id] : NULL;
}

/* step a speed curve (as func_8028A6A0, after the motion step): index at player+0x19CC, speed into +0x40, the
 * velocity along her facing (+0x52), then move */
static void speed_curve(uint32_t p, const Curve *c) {
    int i = MEM_R8(p + 0x19CC);
    if (c[i][0] < (int16_t)MEM_R16(p + 0x1CE)) MEM_W8(p + 0x19CC, (uint8_t)++i);
    int32_t speed = c[i][1], accel = (int32_t)MEM_R32(p + 0x44);
    MEM_W32(p + 0x40, (uint32_t)speed);
    R(4) = (uint32_t)(int32_t)(int16_t)MEM_R16(p + 0x52); f_802CE4DC();
    int32_t sn = (int32_t)R(2);
    MEM_W32(p + 0x28, (uint32_t)(sn * speed));
    MEM_W32(p + 0x34, (uint32_t)(sn * accel));
    R(4) = (uint32_t)(int32_t)(int16_t)MEM_R16(p + 0x52); f_802CE5AC();
    int32_t cs = (int32_t)R(2);
    MEM_W32(p + 0x30, (uint32_t)(cs * speed));
    MEM_W32(p + 0x3C, (uint32_t)(cs * accel));
    R(4) = p; f_80289164();
}

#define SWING_SOUND 917   /* Fighting Layer's swing sound (Kato's records): her normals use the common swing in EX2 Plus too */

/* ---- her voice ----
 * Her own samples (EX2 Plus SND3/P04S.PAC, P04W.PAC: PlayStation VAB banks, converted to SND/P00.SND by
 * make_swap.py) play through the port's sample player (port/core/exsnd.c), mixed into the game's sound. Ids: her
 * EX2 Plus SOUND(char, side, n) numbers 0-31, her win-pose bank 32-47, her damage cries 48-51, her KO cry 52.
 * Panned like Fighting Layer's voices (player +0x1768). */
#define V_DAMAGE 48
#define V_KO 52            /* the fourth damage cry: EX2 Plus's KO cry (tools/sfex/make_swap.py) */
#define V_WIN 32
static int g_voice = -1;

static void her_voice_side(int side, int id) {
    if (g_voice < 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/SND/P00.SND", extra_dir() ? extra_dir() : ".");
        g_voice = exsnd_load(path);
        rt_log("pullum: voice %s%s", path, g_voice ? "" : " not found");
    }
    if (!g_voice) return;
    uint32_t pl = side ? 0x8034BC7Cu : 0x8034A100u;
    int pan = ((int16_t)MEM_R16(pl + 0x1768) - 0x100) / 4;
    exsnd_play(side, id, pan);
}

static void her_voice(uint32_t p, int id) { her_voice_side(MEM_R16(p + 0x64) & 1, id); }

/* picked on the select screen (src/game/roster.c): Fighting Layer's announcer calls out the others, and has no
 * line for her, so she says one of her win-pose lines, panned to her side */
void ex_pullum_selected(int side) {
    her_voice_side(side, -1);                            /* (loads her voices; -1 plays nothing) */
    if (g_voice) exsnd_play(side, V_WIN, side ? 48 : -48);
}
static void trail_on(uint32_t p, int frames);

static void call1(void (*f)(void), uint32_t p) { R(4) = p; f(); }
static void call2(void (*f)(void), uint32_t p, uint32_t a) { R(4) = p; R(5) = a; f(); }

static void normal_attack(uint32_t p, const Move *m) {
    if (MEM_R8(p + 5) == 0) {                         /* entry */
        MEM_W8(p + 5, 1);
        MEM_W32(p + 0x19CC, 0);
        MEM_W32(p + 0x1AC8, 0xFFFFFFFFu);
        if (MEM_R8(p + 0x1B43) != 3) call1(f_80289104, p);
        call2(f_802145F0, p, m->motion);
        if (MEM_R8(p + 0x1B42) == 0x12) call2(f_8028DB58, p, 0xFFFFFFFFu);
    }
    int16_t frame = (int16_t)MEM_R16(p + 0x1CE);
    if (MEM_R8(p + 5) == 1 && frame >= m->sound_frame) {
        MEM_W8(p + 5, 2);
        call2(f_802A071C, MEM_R16(p + 0x64) & 1, SWING_SOUND);
    }
    call1(f_80215338, p);
    if (m->curve) speed_curve(p, m->curve);
    for (int i = 0; i < m->nchain; i++) {
        R(4) = p;
        R(5) = MEM_R32(p + 0x1B58) + CHAINS + 12u * (uint32_t)(m->chain + i);
        f_8028EE14();
        if (R(2)) { MEM_W8(p + 5, 1); return; }        /* chained: the next attack runs from its step 1 */
    }
    if (MEM_R8(p + 0x1B43) == 3) {                    /* in the air */
        call1(f_80289164, p);
        call1(f_8028E9F0, p);
        if (R(2)) { call2(f_80290784, p, 0x0C); return; }
    } else if (MEM_R8(p + 0x1C9) & 2) {               /* the motion ended */
        call1(f_80288DB8, p);
        return;
    }
    if (m->free_frame && (int16_t)MEM_R16(p + 0x1CE) >= m->free_frame) call2(f_802907EC, p, 0x11);
}

void o_pl00_1_8009FEB4_orig(void);
void o_pl00_2_800AFEB4_orig(void);
static void move_end(uint32_t p);
static int landed(uint32_t p);
static int air_throw(uint32_t p, uint16_t in);
static uint32_t object_slot(uint32_t p, int slot);

/* Jumping down + medium punch (EX2 Plus 800436C4): a dive; a hit bounces her back up (80043658). Landing without
 * a hit, EX2 Plus plays the dive motion on from frame 0x41: here that part is its own motion (0x362, make_swap.py
 * motion_tail). After a bounce the game's landing state lands her. */
static void land(uint32_t p, uint16_t motion);
static void cmd_dive(uint32_t p, const Move *m) {
    uint16_t contact = MEM_R16(p + 0x64) & 0x18;
    switch (MEM_R8(p + 5)) {
    case 0:
        MEM_W8(p + 5, 1);
        MEM_W8(p + 0x1B49, 0xFF);
        MEM_W16(p + 0x64, MEM_R16(p + 0x64) & 0xFFE7);
        call2(f_802145F0, p, m->motion);
        her_voice(p, 10);
        break;
    case 1:
        call1(f_8028A638, p);
        call1(f_80289164, p);
        call1(f_80215338, p);
        if (contact == 8) {                              /* it hit: bounce off */
            MEM_W8(p + 5, 2);
            her_voice(p, 10);
            MEM_W32(p + 0x40, 0x400);
            MEM_W32(p + 0x44, (uint32_t)-0x20);
            call1(f_80288F90, p);
            MEM_W32(p + 0x2C, 0x1C0000);
            MEM_W32(p + 0x38, (uint32_t)-0x28000);
            MEM_W16(p + 0x64, MEM_R16(p + 0x64) & 0xFFE7);
        } else if (landed(p)) {
            land(p, 0x362);                              /* her own landing frames */
            MEM_W8(p + 5, 3);
            return;
        }
        break;
    case 3:                                              /* landing */
        call1(f_80215338, p);
        if (MEM_R8(p + 0x1C9) & 2) move_end(p);
        break;
    default:                                             /* bouncing off */
        if ((int16_t)MEM_R16(p + 0x1CE) < 0x40) call1(f_80215338, p);
        call1(f_8028A638, p);
        call1(f_80289164, p);
        if (landed(p)) call2(f_80290784, p, 0x0C);
        break;
    }
}

/* Her air throw is picked when a jumping MP or HP starts (EX2 Plus 80043128, step 0): a direction held (not just
 * up), the opponent in the air, she 0x100 or more above the floor, and in range. */
static int air_throw(uint32_t p, uint16_t in) {
    uint32_t opp = MEM_R32(p + 0x58);
    if (!(in & 0xF000) || (in & 0xF000) == 0x1000 || MEM_R8(opp + 0x1B43) != 3) return 0;
    if ((int16_t)MEM_R16(p + 0x16) >= -0x100) return 0;
    R(4) = p; R(5) = 0x180; R(6) = 0xA0; R(7) = 0xA0; f_8028FCC0();
    if (!R(2)) return 0;
    MEM_W8(p + 0x1B47, 2);
    call2(f_80290784, p, 0x19);                          /* her throw state: word 3 runs it */
    call2(f_80290784, opp, 0x1D);                        /* his: thrown */
    return 1;
}

static void word0(void (*orig)(void)) {
    uint32_t p = R(4);
    int her = roster_is_extra(p) && MEM_R8(p + 2) == 0;
    if (her && MEM_R8(p + 5) == 0) {                     /* her command normals (EX2 Plus 80043128, step 0) */
        uint16_t in = MEM_R16(p + 0x1B2A);
        int id = MEM_R8(p + 0x1B47);
        /* (1 -> 18 with forward, her forward + medium punch: in Fighting Layer forward/back + MP/MK is the throw,
         * which the main program takes before the normal-attack state, so it cannot come up) */
        if ((id == 13 || id == 14) && air_throw(p, in)) return;
        if (id == 13 && (in & 0x4000)) id = 19;
        else if (id == 16 && (in & 0xF000) == 0x8000) id = 20;
        MEM_W8(p + 0x1B47, (uint8_t)id);
    }
    const Move *m = her ? her_move(MEM_R8(p + 0x1B47)) : NULL;
    if (!m) { orig(); return; }
    uint32_t s16 = R(16), s17 = R(17), sp = R(29), ra = R(31);   /* as a called function would keep them */
    switch (MEM_R8(p + 0x1B47)) {
    case 19: cmd_dive(p, m); break;
    default: normal_attack(p, m); break;
    }
    R(16) = s16; R(17) = s17; R(29) = sp; R(31) = ra;
}

void o_pl00_1_8009FEB4(void) { word0(o_pl00_1_8009FEB4_orig); }
void o_pl00_2_800AFEB4(void) { word0(o_pl00_2_800AFEB4_orig); }

/* ---- word 1: her commands, then Kato's word 1 for Fighting Layer's own checks (his tables are off) ---- */
void o_pl00_1_800A0A9C_orig(void);
void o_pl00_2_800B0A9C_orig(void);

static int her_commands(uint32_t p) {
    uint32_t block = MEM_R32(p + 0x1B58);
    call1(f_80290188, p);
    if (R(2)) return 0;                                /* commands locked */
    uint32_t hopped = block + HOPPED;
    if (MEM_R8(p + 0x1B43) != 3) MEM_W8(hopped, 0);      /* on the ground: the air hop is back */
    for (int i = 0; i < NCOMMANDS; i++) {
        const Command *c = &k_commands[i];
        R(29) -= 0x20;                                 /* the 5th argument goes on the stack */
        MEM_W32(R(29) + 0x10, c->id);
        R(4) = p;
        R(5) = block + CMD_STATES + 4u * (uint32_t)i;
        /* air commands when she has crossed over him in the air (side flags +0x64 bits 2 and 7 differ; EX2 Plus
         * +0x70 the same): forward and back swapped */
        uint16_t side = MEM_R16(p + 0x64);
        int crossed = c->air && ((side >> 2) & 1) != ((side >> 7) & 1);
        R(6) = block + (crossed ? CMD_CROSSED : CMD_TABLES) + CMD_SIZE * (uint32_t)i;
        R(7) = R(29) + 0x18;
        f_80286CC8();
        uint32_t buttons = R(2) & 0xFFFF, strength = MEM_R8(R(29) + 0x18);
        R(29) += 0x20;
        if (!buttons || (MEM_R8(p + 0x1B43) == 3) != c->air) continue;
        if (c->id == 1 && (int16_t)MEM_R16(p + 0x16) >= -0x7F) continue;   /* the dive: high enough (EX2 Plus too) */
        if (c->id == 6 && MEM_R8(hopped)) continue;
        if (c->id == 4 && (MEM_R16(p + 0x1766) & 8)) continue;          /* as Kato's (800A0D10) */
        if (c->id == 10 && ((int16_t)MEM_R16(p + 0x16) >= -0x3F || MEM_R8(object_slot(p, 1)))) continue;
        if (c->id == 9 && ((MEM_R16(p + 0x1B30) & 0x980) != 0x980 || MEM_R8(object_slot(p, 0)))) continue;   /* LP+MP+HP */
        if (MEM_R8(p + 0x1B45) < c->gauge) continue;
        R(4) = p; R(5) = c->cls; R(6) = c->id; f_8028F344();
        if (!R(2)) continue;
        MEM_W8(p + 0x1B47, c->id);
        call2(f_80290784, p, 0x13);                    /* state 19: word 2 runs the special */
        MEM_W32(p + 0x1A8C, buttons);
        MEM_W8(p + 0x1B48, (uint8_t)strength);
        if (c->id == 6) MEM_W8(hopped, 1);
        return 1;
    }
    return 0;
}

static void word1(void (*orig)(void)) {
    uint32_t p = R(4);
    if (!roster_is_extra(p) || MEM_R8(p + 2) != 0) { orig(); return; }
    uint32_t s16 = R(16), s17 = R(17), s18 = R(18), sp = R(29), ra = R(31);
    int matched = her_commands(p);
    R(16) = s16; R(17) = s17; R(18) = s18; R(29) = sp; R(31) = ra;
    if (matched) return;
    /* A computer player asks for a move by command id (+0x1B03, compared by the matcher instead of the joystick).
     * Kato's word 1 would start his own specials for ids hers did not take, so only the shared ones reach it: 16
     * roll, 17 run (802904F8), 18 back dash (802905A4) and 64 sidestep (80290654; no computer player asks for it) */
    uint8_t want = MEM_R8(p + 0x1B03);
    if ((want < 16 || want > 18) && want != 64) MEM_W8(p + 0x1B03, 0xFF);
    R(4) = p;
    orig();
    if (MEM_R8(p + 0x1B03) == 0xFF) MEM_W8(p + 0x1B03, want);
}
void o_pl00_1_800A0A9C(void) { word1(o_pl00_1_800A0A9C_orig); }
void o_pl00_2_800B0A9C(void) { word1(o_pl00_2_800B0A9C_orig); }

/* ---- word 2: her specials (state 19) ----
 * Each runs by steps in player+5, as in EX2 Plus. Her EX2 Plus routines' callees map to Fighting Layer's as in
 * docs/EX2P_MOVE_CODE.md; motions are her slots (tools/sfex/make_swap.py MOTION_MAP). Not done yet: her voices,
 * trails and the animation-speed factors. */
static void move_end(uint32_t p) {
    MEM_W16(p + 0x1766, MEM_R16(p + 0x1766) | 1);
    call1(f_80288DB8, p);
}

/* A special goes on to another motion that hits: its hits can land too (EX2 Plus resets the hit state on every
 * motion change; Fighting Layer only when the attack state starts or a chain fires, so it is done here). */
static void next_motion(uint32_t p, uint32_t motion) {
    MEM_W8(p + 0x1B49, 0xFF);
    call2(f_802145F0, p, motion);
}

static void step_curve(uint32_t p, const Curve *c) { call1(f_80215338, p); speed_curve(p, c); }

/* the start of every special (Kato's 800A0EC8 and hers do the same): face the opponent, hits armed, stop, the
 * root put back under the body, invulnerability, gauge (hers: +3, +6 for the rising kick; capped at 150) */
static void special_entry(uint32_t p, int invulnerable, int gauge) {
    call2(f_8028DB58, p, 0xFFFFFFFFu);
    if (MEM_R8(p + 0x1B43) != 3) {
        call2(f_8028A9B4, p, 0x800);
        uint16_t f = MEM_R16(p + 0x64);
        MEM_W16(p + 0x64, (uint16_t)((f & 0x80) ? f | 4 : f & ~4u));
    }
    MEM_W8(p + 0x1B49, 0xFF);
    MEM_W16(p + 0x64, MEM_R16(p + 0x64) & 0xFFE7);
    call1(f_80289104, p);
    MEM_W8(p + 0x1B43, 0);
    call2(f_8028B9D8, p, 0x1F800000u);
    MEM_W16(p + 0x12, MEM_R16(0x1F800000u));
    MEM_W16(p + 0x1A, MEM_R16(0x1F800008u));
    MEM_W32(p + 0x14, 0);
    MEM_W32(p + 0x19CC, 0);
    MEM_W32(p + 0x1A90, 0);
    MEM_W32(p + 0x1AC8, 0xFFFFFFFFu);
    MEM_W8(p + 0x1B4A, (uint8_t)invulnerable);       /* frames she cannot be hit */
    MEM_W8(p + 0x1B4C, (uint8_t)invulnerable);
    uint32_t opp = MEM_R32(p + 0x58);
    if (MEM_R8(opp + 0x1B44) != 0xFF && MEM_R16(0x80335430u)) {
        int g = MEM_R8(p + 0x1B45) + gauge;
        MEM_W8(p + 0x1B45, (uint8_t)(g > 150 ? 150 : g));
    }
}

static void take_off(uint32_t p, int32_t speed, int32_t accel, int32_t vy, int32_t ay) {
    MEM_W8(p + 0x1B43, 3);
    MEM_W32(p + 0x40, (uint32_t)speed);
    MEM_W32(p + 0x44, (uint32_t)accel);
    call1(f_80288F90, p);
    MEM_W32(p + 0x2C, (uint32_t)vy);
    MEM_W32(p + 0x38, (uint32_t)ay);
}

static int landed(uint32_t p) { call1(f_8028E9F0, p); return R(2) != 0; }

static void land(uint32_t p, uint16_t motion) {
    MEM_W8(p + 0x1B43, 0);
    MEM_W8(p + 5, (uint8_t)(MEM_R8(p + 5) + 1));
    call1(f_80289104, p);
    call2(f_80214568, p, motion);
    MEM_W32(p + 0x19CC, 0);
}

/* DP+K: a rising kick (EX2 Plus 80044284). 8 frames invulnerable; three hits on the way up. */
static void sp_rising_kick(uint32_t p) {
    static const Curve rise[] = {{1, 696}, {3, 72}, {99, 0}};               /* 80041178 */
    static const Curve slide[] = {{5, -280}, {8, -416}, {18, 0}, {99, 0}};  /* 80041184 */
    static const int32_t speed[3] = {0x50, 0x68, 0x88}, vy[3] = {0x180000, 0x1C0000, 0x200000};
    int s = MEM_R8(p + 0x1B48) % 3;
    switch (MEM_R8(p + 5)) {
    case 0:
        MEM_W8(p + 5, 1);
        special_entry(p, 8, 6);
        her_voice(p, 0);
        trail_on(p, 40);
        call2(f_802145F0, p, 0x353u + (uint32_t)s);   /* EX2 Plus 0x302-0x304 */
        MEM_W32(p + 0x19CC, 0);
        break;
    case 1:
        call2(f_8028AC4C, p, 0x40);
        step_curve(p, rise);
        if ((int16_t)MEM_R16(p + 0x1CE) >= 3) {
            MEM_W8(p + 5, 2);
            take_off(p, speed[s], -1, vy[s], -0x18000);
        }
        break;
    case 2:
        call1(f_80289164, p);
        call1(f_80215338, p);
        if (landed(p)) land(p, 0x356);                 /* 0x305 */
        break;
    default:
        step_curve(p, slide);
        if (MEM_R8(p + 0x1C9) & 2) move_end(p);
        break;
    }
}

/* QCB+K: a lunge, then a flying kick (EX2 Plus 80044E74). Invulnerable 10 frames (light) or 4. */
static void sp_flying_kick(uint32_t p) {
    static const Curve lunge[] = {{6, 0}, {7, 1056}, {8, 536}, {99, 0}};     /* 800411E8 */
    static const int32_t fly[3][4] = {                                      /* 8004139C: speed, accel, vy, ay */
        {0x100, -2, 0x180000, -0x28000}, {0x200, -2, 0x1A0000, -0x28000}, {0x140, -2, 0x200000, -0x22000}};
    int s = MEM_R8(p + 0x1B48) % 3;
    switch (MEM_R8(p + 5)) {
    case 0:
        MEM_W8(p + 5, 1);
        special_entry(p, s == 0 ? 10 : 4, 3);
        her_voice(p, 1);
        call2(f_802145F0, p, 0x357);                   /* 0x306 */
        MEM_W32(p + 0x19CC, 0);
        break;
    case 1:
        call2(f_8028AC4C, p, 0x40);
        step_curve(p, lunge);
        if (MEM_R8(p + 0x1C9) & 2) {
            MEM_W16(p + 0x16, (uint16_t)(MEM_R16(p + 0x16) - 0x1C));
            MEM_W8(p + 5, 2);
            next_motion(p, 0x359u + (uint32_t)s);          /* 0x308-0x30A */
            trail_on(p, 24);
            take_off(p, fly[s][0], fly[s][1], fly[s][2], fly[s][3]);
        }
        break;
    case 2:
        call2(f_8028AC4C, p, 0x40);
        call1(f_80289164, p);
        call1(f_8028A638, p);
        call1(f_80215338, p);
        if (landed(p)) land(p, 0x358);                 /* 0x307 */
        break;
    default:
        call1(f_80215338, p);
        if (MEM_R8(p + 0x1C9) & 2) move_end(p);
        break;
    }
}

/* Punch + kick of the same strength: her guard break. EX2 Plus (8004525C) spends 50 gauge on a crouch, a hop
 * forward and a falling kick that breaks the guard. Fighting Layer's guard break is every character's held P+K
 * (Kato's 800A12E4): no gauge cost (+1), a charge of up to 40 frames with the charge glow (an object running
 * func_802575AC that follows the move id at +0x19D4), the charge stored at +0x195C for the hit (func_8028CD2C adds
 * damage for it), and only a full charge breaks the guard. So hers keeps Fighting Layer's rules and her motion
 * (0x30B, slot 0x69): she holds the crouch (frame 10) while charging, then hops at frame 14 (her speed 0x100, rise
 * 0xC8000, falling from frame 19 at -0x20000) and the kick lands on frames 30-34. Its hits take Kato's guard-break
 * attributes (tools/sfex/make_swap.py HIT_MAP 0x69). */
void f_80221F08(void);   /* a free effect object */
#define GB_HOLD 10        /* the crouch she holds while charging */
#define GB_FULL 40        /* Kato's charge motion is 40 frames: the most a charge can be */
static void sp_guard_break(uint32_t p) {
    int16_t f = (int16_t)MEM_R16(p + 0x1CE);
    switch (MEM_R8(p + 5)) {
    case 0: {
        MEM_W8(p + 5, 1);
        special_entry(p, 0, 1);
        MEM_W32(p + 0x1A8C, 0);
        her_voice(p, 10);
        trail_on(p, 56);
        call2(f_802145F0, p, 0x369);                   /* 0x30B */
        f_80221F08();                                  /* the charge glow, as Kato's (800A1428) */
        uint32_t ob = R(2);
        if (ob) {
            MEM_W32(ob, 0x802575ACu);
            MEM_W32(ob + 0xBC, MEM_R32(p + 0x1B58) + 0x5DCu);   /* his glow record (node 15), in her block copy */
            MEM_W32(ob + 0xC0, p + 0x19D4);
            MEM_W8(ob + 6, MEM_R16(p + 0x64) & 1);
            MEM_W32(p + 0x19D4, MEM_R8(p + 0x1B47));
        }
    }   /* fall through: the first charge frame */
    case 1: {
        if (f < GB_HOLD) call1(f_80215338, p);
        uint32_t n = MEM_R32(p + 0x1A8C) + 1;
        MEM_W32(p + 0x1A8C, n);
        uint16_t held = MEM_R16(p + 0x1B2A);
        if (n >= GB_FULL || !(held & 0x980) || !(held & 0x640)) {
            MEM_W16(p + 0x195C, (uint16_t)n);           /* the charge, read by the hit */
            MEM_W32(p + 0x19D4, 0xFFFFFFFFu);
            MEM_W8(p + 5, 2);
        }
        break;
    }
    case 2:
        call1(f_80215338, p);
        if (f >= 14) {
            MEM_W8(p + 5, 3);
            call2(f_802A071C, MEM_R16(p + 0x64) & 1, SWING_SOUND);
            take_off(p, 0x100, 0, 0xC8000, 0);
        }
        break;
    case 3:
        call1(f_80289164, p);
        call1(f_80215338, p);
        if (f >= 19 && !MEM_R32(p + 0x38)) MEM_W32(p + 0x38, (uint32_t)-0x20000);
        if (f == 30) { R(4) = 10; R(5) = 12; f_8022A34C(); }   /* Kato's shake when his blow lands */
        if (f > 19 && landed(p)) {
            MEM_W8(p + 0x1B43, 0);
            call1(f_80289104, p);
            MEM_W32(p + 0x19CC, 0);
            MEM_W8(p + 5, 4);
        }
        break;
    default:
        call1(f_80215338, p);
        if (MEM_R8(p + 0x1C9) & 2) move_end(p);
        break;
    }
}

/* QCF+K: a run-up, a spinning flight, then a dive and a slide (EX2 Plus 80046640). The body turns about a pivot
 * (Fighting Layer's spin functions take the same arguments as EX2 Plus's 8021663C / 80216730 / 80216860). */
static void sp_flying_spin(uint32_t p) {
    static const int16_t fly_speed[3] = {256, 480, 704};                    /* 800413EC */
    uint32_t spin = MEM_R32(p + 0x1B58) + SPIN, saved = spin, pivot = spin + 8, vel = spin + 16, timer = spin + 24;
    int s = MEM_R8(p + 0x1B48) % 3;
    int step = MEM_R8(p + 5);
    switch (step) {
    case 0:
        MEM_W8(p + 5, 1);
        special_entry(p, 0, 3);
        call2(f_802145F0, p, 0x35C);                   /* run-up 0x335 */
        break;
    case 1: {
        int16_t f = (int16_t)MEM_R16(p + 0x1CE);
        if (f == 8 || f == 11) { MEM_W32(p + 0x40, f == 8 ? 300 : 396); call1(f_80288F90, p); }
        call2(f_8028AC4C, p, 0x80);
        call1(f_80289164, p);
        call1(f_80215338, p);
        if (MEM_R8(p + 0x1C9) & 2) {
            MEM_W16(p + 0x16, 0x50);
            MEM_W8(p + 5, 2);
            her_voice(p, 2);
            trail_on(p, 30);
            next_motion(p, 0x35Du + (uint32_t)s);          /* flight 0x336-0x338 */
        }
        break;
    }
    case 2:
        MEM_W16(timer, 12);
        MEM_W8(p + 5, 3);
        for (int i = 0; i < 3; i++) { MEM_W16(pivot + 2u * (uint32_t)i, 0); MEM_W16(vel + 2u * (uint32_t)i, 0); }
        call2(f_8028ADFC, p, saved);
        MEM_W16(vel + 4, (uint16_t)-0x200);
        MEM_W16(pivot + 2, (uint16_t)-0x146);
        MEM_W32(p + 0x40, (uint32_t)(int32_t)fly_speed[s]);
        call1(f_80288F90, p);
        MEM_W32(p + 0x2C, 0xFFFF0000u);
        break;
    case 3: {
        uint16_t contact = MEM_R16(p + 0x64) & 0x18;
        uint16_t t = contact ? 0 : (uint16_t)(MEM_R16(timer) - 1);
        MEM_W16(timer, t);
        if (contact || !t) {                                 /* it hit, was guarded, or ran out: dive */
            MEM_W8(p + 5, contact == 8 ? 0x12 : 0x10);
            MEM_W32(p + 0x2C, 0xFFC00000u);
            next_motion(p, 0x360);                           /* dive 0x33B */
            MEM_W16(p + 0x1B4, 0x400);
            MEM_W16(vel + 4, 0);
        } else {
            call2(f_8028AC4C, p, 0x80);
            call1(f_80289164, p);
        }
        R(4) = p; R(5) = pivot; R(6) = vel; R(7) = saved; f_8028AEF0();
        break;
    }
    case 0x10: case 0x12:                                    /* diving */
        call1(f_8028A638, p);
        call1(f_80289164, p);
        R(4) = p; R(5) = pivot; R(6) = vel; R(7) = saved; f_8028AEF0();
        if (landed(p)) {
            MEM_W32(p + 0x2C, 0);
            MEM_W32(p + 0x38, 0);
            MEM_W32(p + 0x44, (uint32_t)-0x40);
            MEM_W8(p + 5, (uint8_t)(step + 1));
            call2(f_8028B174, p, saved);                     /* the spin ends where the body is */
            MEM_W16(p + 0x12, MEM_R16(p + 0x1F4 + 0x5C));    /* root node's world x, z */
            MEM_W16(p + 0x1A, MEM_R16(p + 0x1F4 + 0x64));
            MEM_W16(p + 0x16, 0x140);
            call2(f_80214568, p, 0x361);                     /* sliding landing 0x33C */
            MEM_W16(p + 0x175E, s ? 100 : 95);
        }
        break;
    default:                                                 /* sliding */
        call1(f_8028A638, p);
        call1(f_80289164, p);
        if (!MEM_R32(p + 0x40)) call1(f_80215338, p);
        if (MEM_R8(p + 0x1C9) & 2) { MEM_W16(p + 0x175E, 100); move_end(p); }
        break;
    }
}

/* air QCF+K: she stops in the air, spins up, then dives spinning (EX2 Plus 80044644). Not done yet: her trail. (Her contact branch, step 3, is never reached in EX2 Plus either.) */
static void sp_spinning_dive(uint32_t p) {
    static const int16_t spin_x[3] = {320, 512, 768}, vy[3] = {-448, -576, -704}, speed[3] = {576, 448, 320};
    uint32_t spin = MEM_R32(p + 0x1B58) + SPIN, saved = spin, pivot = spin + 8, vel = spin + 16;
    int s = MEM_R8(p + 0x1B48) % 3;
    switch (MEM_R8(p + 5)) {
    case 0:
        MEM_W8(p + 5, 2);
        MEM_W8(p + 0x1B43, 3);
        call1(f_80289104, p);
        MEM_W8(p + 0x1B49, 0xFF);
        MEM_W16(p + 0x64, MEM_R16(p + 0x64) & 0xFFE7);
        call2(f_8028B9D8, p, 0x1F800000u);
        MEM_W16(p + 0x12, MEM_R16(0x1F800000u));
        MEM_W16(p + 0x1A, MEM_R16(0x1F800008u));
        for (int i = 0; i < 3; i++) { MEM_W16(pivot + 2u * (uint32_t)i, 0); MEM_W16(vel + 2u * (uint32_t)i, 0); }
        call2(f_8028ADFC, p, saved);
        MEM_W16(vel, (uint16_t)spin_x[s]);
        MEM_W16(vel + 4, (uint16_t)-0x200);
        MEM_W16(pivot + 2, (uint16_t)-0x146);
        MEM_W32(p + 0x2C, (uint32_t)((int32_t)vy[s] << 12));
        MEM_W32(p + 0x40, (uint32_t)(int32_t)speed[s]);
        call1(f_80288F90, p);
        call2(f_802145F0, p, 0x365u + (uint32_t)s);     /* 0x30C / 0x332 / 0x333 */
        R(4) = p; R(5) = pivot; R(6) = vel; R(7) = saved; f_8028AEF0();
        MEM_W16(vel, 0);
        MEM_W16(vel + 4, 0);
        break;
    case 2: {                                            /* hanging in the air, spinning up */
        call2(f_8028AC4C, p, 0x40);
        R(4) = p; R(5) = pivot; R(6) = vel; R(7) = saved; f_8028AEF0();
        int16_t vz = (int16_t)MEM_R16(vel + 4);
        if (vz < 0x300) MEM_W16(vel + 4, (uint16_t)(vz + 0x20));
        if (MEM_R8(p + 0x1C9) & 2) {
            MEM_W8(p + 5, 4);
            her_voice(p, 2);
            trail_on(p, 30);
            MEM_W16(vel + 4, 0x300);
        }
        break;
    }
    case 4:                                              /* the dive */
        call1(f_80289164, p);
        R(4) = p; R(5) = pivot; R(6) = vel; R(7) = saved; f_8028AEF0();
        if (landed(p)) {
            MEM_W8(p + 5, 5);
            call2(f_8028B174, p, saved);
            MEM_W16(p + 0x12, MEM_R16(p + 0x1F4 + 0x5C));
            MEM_W16(p + 0x1A, MEM_R16(p + 0x1F4 + 0x64));
            call1(f_80289104, p);
            MEM_W8(p + 0x1B43, 0);
            call2(f_80214568, p, 0x368);                /* landing: shared 0x10C */
            MEM_W16(p + 0x16, 0x140);
        }
        break;
    default:                                             /* landing; free from frame 3 */
        call1(f_80215338, p);
        if ((int16_t)MEM_R16(p + 0x1CE) >= 3) call2(f_802907EC, p, 0x11);
        break;
    }
}

/* air QCB+P (EX2 Plus 80045B60): a hop in the air, back / up / forward for L / M / H, with her jump constants
 * (block word 12: the same layout in both games); an air normal can follow. Not done yet: a buffered air QCF+K
 * or QCF x2+P fired from it. */
static void sp_air_hop(uint32_t p) {
    static const uint8_t landing[3] = {0xE, 0xC, 0xD};   /* the jump landings for back, up, forward */
    int s = MEM_R8(p + 0x1B48) % 3;
    if (MEM_R8(p + 5) == 0) {
        static const uint8_t word[3][3] = {{10, 8, 9}, {4, 2, 3}, {7, 5, 6}};   /* speed, vy, ay words */
        uint32_t table = MEM_R32(MEM_R32(p + 0x1B58) + 0x30);
        MEM_W8(p + 5, 1);
        her_voice(p, 3);
        MEM_W8(p + 0x1B43, 3);
        MEM_W16(p + 0x16, (uint16_t)(MEM_R16(p + 0x16) - 0x80));
        call1(f_80289104, p);
        MEM_W8(p + 0x1B49, 0xFF);
        MEM_W16(p + 0x64, MEM_R16(p + 0x64) & 0xFFE7);
        MEM_W32(p + 0x40, (uint32_t)((int32_t)MEM_R32(table + 4u * word[s][0]) * 3 / 4));
        MEM_W32(p + 0x2C, (uint32_t)((int32_t)MEM_R32(table + 4u * word[s][1]) >> 1));
        MEM_W32(p + 0x38, (uint32_t)((int32_t)MEM_R32(table + 4u * word[s][2]) >> 1));
        call2(f_8028A9B4, p, 0x800);
        uint16_t f = MEM_R16(p + 0x64);
        MEM_W16(p + 0x64, (uint16_t)((f & 0x80) ? f | 4 : f & ~4u));
        call2(f_8028B9D8, p, 0x1F800000u);
        MEM_W16(p + 0x12, MEM_R16(0x1F800000u));
        MEM_W16(p + 0x1A, MEM_R16(0x1F800008u));
        call1(f_80288F90, p);
        call2(f_80214568, p, 0x370);                     /* 0x32E */
        return;
    }
    call1(f_80289164, p);
    if (landed(p)) { MEM_W8(p + 0x1B43, 0); call2(f_80290784, p, landing[s]); return; }
    call1(f_8028EBFC, p);                                /* an air normal may follow */
    if (!R(2)) call1(f_80215338, p);
}

/* QCF x2+K super (EX2 Plus 80045F48): four hopping kicks, then a rising one, and the rising kick's landing. 18
 * frames invulnerable. */
static void sp_super_kicks(uint32_t p) {
    static const Curve hop[] = {{1, 696}, {4, 0}, {24, 92}, {29, 0}, {40, -4}, {99, 0}};   /* 800411F8 */
    static const Curve rise[] = {{1, 696}, {3, 72}, {99, 0}};                               /* 80041178 */
    static const Curve slide[] = {{5, -280}, {8, -416}, {18, 0}, {99, 0}};                  /* 80041184 */
    static const uint16_t next[4] = {0x372, 0x373, 0x374, 0x375};   /* 0x30F, 0x32F, 0x330, then 0x310 */
    int step = MEM_R8(p + 5);
    if (step == 0) {
        MEM_W8(p + 5, 1);
        special_entry(p, 18, 0);
        call1(f_8028E178, p);                            /* the super: gauge, flash, announcer */
        her_voice(p, 4);
        trail_on(p, 200);
        call2(f_802145F0, p, 0x371);                     /* 0x30E */
        return;
    }
    call2(f_8028AC4C, p, 0x80);
    if (step <= 8 && (step & 1)) {                       /* a kick, on the ground */
        step_curve(p, hop);
        if ((int16_t)MEM_R16(p + 0x1CE) >= 4) {
            MEM_W8(p + 0x1B43, 3);
            MEM_W32(p + 0x2C, 0x1B8000);
            MEM_W32(p + 0x38, (uint32_t)-0x37000);
            MEM_W8(p + 5, (uint8_t)(step + 1));
        }
    } else if (step <= 8) {                              /* a kick, in the air */
        step_curve(p, hop);
        int16_t f = (int16_t)MEM_R16(p + 0x1CE);
        MEM_W16(p + 0x175E, f >= 21 && f <= 24 ? 60 : 100);
        if (landed(p)) { MEM_W8(p + 0x1B43, 0); MEM_W32(p + 0x38, 0); MEM_W32(p + 0x2C, 0); }
        if (f >= 27) {
            MEM_W32(p + 0x19CC, 0);
            if (step == 4) MEM_W16(p + 0x64, MEM_R16(p + 0x64) & 0xFFE7);
            MEM_W8(p + 5, (uint8_t)(step + 1));
            MEM_W8(p + 0x1B49, 0xFF);
            R(4) = p; R(5) = next[step / 2 - 1]; R(6) = 2; f_8021483C();
            her_voice(p, step == 8 ? 7 : 9);
        }
    } else if (step == 9) {                              /* the rising kick */
        MEM_W16(p + 0x175E, 100);
        step_curve(p, rise);
        if ((int16_t)MEM_R16(p + 0x1CE) == 3) {
            MEM_W8(p + 5, 10);
            take_off(p, 0x80, -4, 0x200000, -0x1A000);
        }
    } else if (step == 10) {
        call1(f_80289164, p);
        call1(f_80215338, p);
        if (landed(p)) land(p, 0x356);                   /* the rising kick's landing, 0x305 */
    } else {
        step_curve(p, slide);
        if (MEM_R8(p + 0x1C9) & 2) move_end(p);
    }
}

/* QCB x2+K super (EX2 Plus 8004555C): she rises spinning, hovers, comes down; hits again every 8 frames while
 * it connects (frames 19-50). 16 frames invulnerable. */
static void sp_super_spin(uint32_t p) {
    static const Curve drift[] = {{19, 163}, {74, 28}, {84, -60}, {92, -68}, {98, -44}, {99, 0}};   /* 80041244 */
    uint32_t rehit = MEM_R32(p + 0x1B58) + REHIT;
    int step = MEM_R8(p + 5);
    if (step == 0) {
        MEM_W8(p + 5, 1);
        special_entry(p, 16, 0);
        call1(f_8028E178, p);
        her_voice(p, 5);
        trail_on(p, 100);
        MEM_W8(rehit, 0);
        MEM_W8(rehit + 1, 0);
        MEM_W16(rehit + 2, 0);
        call2(f_802145F0, p, 0x36E);                     /* 0x30D */
        step = 1;
    }
    if (step == 1) call2(f_8028AC4C, p, 0x40);
    step_curve(p, drift);
    int16_t f = (int16_t)MEM_R16(p + 0x1CE);
    if (step == 1 && f >= 19) { MEM_W8(p + 0x1B43, 3); MEM_W32(p + 0x2C, 0xB8000); MEM_W8(p + 5, 2); }
    else if (step == 2 && (int16_t)MEM_R16(p + 0x16) < -0x95) {
        MEM_W32(p + 0x38, 0); MEM_W32(p + 0x2C, 0); MEM_W8(p + 5, 3);
    } else if (step == 3 && f >= 0x3E) { MEM_W32(p + 0x2C, (uint32_t)-0xB8000); MEM_W8(p + 5, 4); }
    else if (step == 4 && landed(p)) { MEM_W8(p + 0x1B43, 0); MEM_W32(p + 0x38, 0); MEM_W32(p + 0x2C, 0); }
    /* hitting again: from frame 19 to 50, the hit is armed again 8 motion frames after each contact (counted in
     * her motion's frames, not game frames: a hit's hit-stop holds her motion, and against Lance Knight it is long
     * enough that a game-frame timer re-armed the hit before the motion moved on, so the super never ended).
     * rehit +1: hits so far (at most 6), +2: the motion frame to re-arm at (0: waiting for a contact). */
    int phase = MEM_R8(rehit);
    if (phase == 0 && f >= 19) MEM_W8(rehit, 1);
    else if (phase == 1) {
        int16_t at = (int16_t)MEM_R16(rehit + 2);
        if (f >= 50 || MEM_R8(rehit + 1) >= 6) MEM_W8(rehit, 2);
        else if (!at) {
            if ((MEM_R16(p + 0x64) & 0x18) == 8) {
                MEM_W16(rehit + 2, (uint16_t)(f + 8));
                MEM_W8(rehit + 1, (uint8_t)(MEM_R8(rehit + 1) + 1));
            }
        } else if (f >= at) {
            MEM_W16(rehit + 2, 0);
            MEM_W8(p + 0x1B49, 0xFF);
            MEM_W16(p + 0x64, MEM_R16(p + 0x64) & 0xFFE7);   /* armed again, its last contact forgotten */
        }
    }
    if (MEM_R8(p + 0x1C9) & 2) move_end(p);
}

/* ---- her big objects (Fighting Layer's object slots: 4 x 0x20FC at 0x80355548, side * 2 + slot) ----
 * An object is spawned with func_8027CD98(object, owner, type, param); the engine runs a callback for it every
 * frame (func_8022A540, by type, from the owner's block words 14-23; hers are run from the wrapper below), and its
 * collision pass hits the opponent with the object's armed spheres (func_80220664): +0x20FB flags (1 draw, 2 hit
 * armed, 4 proximity guard), +0x20B4 attributes, +0x20B8 spheres {x, y, z, radius, on}. On a hit or a guard the
 * engine sets +4 = 2, on a clash +4 = 3. EX2 Plus's objects are the same system (80207964, docs/SFEX2.md). */
#define OBJECTS 0x80355548u
#define OBJ_SIZE 0x20FCu
void f_8027CD98(void);   /* spawn (object, owner, type, param) */
void f_802D0C9C(void);   /* ratan2 (y, x) */
void f_80207E5C(void);   /* add a primitive to an ordering-table entry (entry, primitive) */

static uint32_t object_slot(uint32_t p, int slot) {
    return OBJECTS + OBJ_SIZE * (2u * (MEM_R16(p + 0x64) & 1) + (uint32_t)slot);
}

/* her work area in an object (the engine leaves +0xA8 .. +0x20A7 to the callback) */
#define OB_HITS 0xA8u     /* u8 hits left */
#define OB_WAIT 0xAAu     /* u16 frames before it is armed again */
#define OB_LIFE 0xACu     /* u16 frames left */
#define OB_SPIN 0xAEu     /* u16 spin angle (drawing) */

static int objects_paused(void) { return MEM_R8(0x8033031Au) != 0; }   /* as Fighting Layer's own callbacks */

static void object_sphere(uint32_t ob, int i, int radius, int on) {
    uint32_t sp = ob + 0x20B8u + 8u * (uint32_t)i;
    MEM_W16(sp, MEM_R16(ob + 0x12));
    MEM_W16(sp + 2, MEM_R16(ob + 0x16));
    MEM_W16(sp + 4, MEM_R16(ob + 0x1A));
    MEM_W8(sp + 6, (uint8_t)radius);
    if (on >= 0) MEM_W8(sp + 7, (uint8_t)on);
}

/* her attack attributes for objects: the super kicks' (small knock-back) and the last kick's (knock-down) */
static uint32_t her_attr(uint32_t owner, uint32_t motion) {
    R(4) = owner; R(5) = motion; R(6) = 0; f_8021F234();
    return R(2);
}

/* ---- drawing her objects ----
 * Fighting Layer's engine never draws objects; their callbacks build GPU primitives (two copies, by the draw
 * buffer D_8034D808) and link them into the frame's ordering table (D_8034D800 + 0x80 + 4 * depth, func_80207E5C),
 * projecting with the camera matrix D_803303DC, as pl07/pl09 do. Her sprites use her effect texture (EX2 Plus
 * PAC part 8, FX/P00.TIM from her folder), put where EX2 Plus puts it and Fighting Layer leaves VRAM unused:
 * picture at (512 + 128 * side, 256), its 16 palettes at (576 + 128 * side, 256) (EX2 Plus has them at
 * (384 + 32 * side, 480), which Fighting Layer's own palette rows use). */
static uint16_t g_fx_img[64 * 128], g_fx_clut[16 * 16];
static int g_fx = -1;

static int fx_load(void) {
    if (g_fx >= 0) return g_fx;
    g_fx = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/FX/P00.TIM", extra_dir() ? extra_dir() : ".");
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t head[2], blk[3];
    int ok = fread(head, 4, 2, f) == 2 && head[1] == 8 &&
             fread(blk, 4, 3, f) == 3 && fread(g_fx_clut, 2, 256, f) == 256 &&
             fread(blk, 4, 3, f) == 3 && fread(g_fx_img, 2, 64 * 128, f) == 64 * 128;
    fclose(f);
    g_fx = ok;
    rt_log("pullum: effect texture %s%s", path, ok ? "" : " not valid");
    return g_fx;
}

static void vram_put_rect(int x0, int y0, int w, int h, const uint16_t *src) {
    for (int y = 0; y < h; y++)
        if (memcmp(&g_vram[((y0 + y) << 10) | x0], src + y * w, (size_t)w * 2)) {
            hd_vram_written(x0, y0, w, h);
            for (int yy = 0; yy < h; yy++) memcpy(&g_vram[((y0 + yy) << 10) | x0], src + yy * w, (size_t)w * 2);
            return;
        }
}

/* her texture in VRAM for this side (put back if anything overwrote it) */
static int fx_upload(int side) {
    if (!fx_load()) return 0;
    vram_put_rect(512 + 128 * side, 256, 64, 128, g_fx_img);
    vram_put_rect(576 + 128 * side, 256, 16, 16, g_fx_clut);
    return 1;
}

/* project a world point with the game camera: screen x, y, depth (0 if behind) */
static int project(int32_t x, int32_t y, int32_t z, int *sx, int *sy) {
    const uint32_t cam = 0x803303DCu;
    for (int i = 0; i < 5; i++) gte_ctc2(i, MEM_R32(cam + 4u * (uint32_t)i));
    for (int i = 0; i < 3; i++) gte_ctc2(5 + i, MEM_R32(cam + 0x14u + 4u * (uint32_t)i));
    gte_mtc2(0, ((uint32_t)x & 0xFFFF) | ((uint32_t)y << 16));
    gte_mtc2(1, (uint32_t)z & 0xFFFF);
    gte_op(0x0180001);                                   /* RTPS */
    uint32_t sxy = gte_mfc2(14);
    *sx = (int16_t)(sxy & 0xFFFF);
    *sy = (int16_t)(sxy >> 16);
    return (int)(gte_mfc2(19) & 0xFFFF);
}

static int isin(int a) { R(4) = (uint32_t)(a & 0xFFF); f_802CE4DC(); return (int32_t)R(2); }
static int icos(int a) { R(4) = (uint32_t)(a & 0xFFF); f_802CE5AC(); return (int32_t)R(2); }

/* n additive textured sprites around a world point: each centred ring_r from it at angle spin + k * 4096 / n,
 * size half (world units), picture (u, v, 64 x 64), palette row; prims in the object at +0x400 */
static void fx_ring(uint32_t ob, int side, int32_t x, int32_t y, int32_t z, int n, int ring_r, int half,
                    int spin, int u, int v, int pal) {
    int cx, cy, sz = project(x, y, z, &cx, &cy);
    if (sz < 64) return;
    int otz = (sz >> 3) - 8 - 6;                         /* a little in front of what is at its depth */
    if (otz < 1) otz = 1;
    if (otz > 0x3FE) return;
    int H = (int)(gte_cfc2(26) & 0xFFFF);
    int rr = ring_r * H / sz, hs = half * H / sz;
    uint32_t prims = ob + 0x400u + (MEM_R32(0x8034D808u) & 1) * 0x400u, ot = MEM_R32(0x8034D800u) + 0x80u + 4u * (uint32_t)otz;
    uint16_t clut = (uint16_t)(((256 + pal) << 6) | ((576 + 128 * side) >> 4));
    uint16_t tpage = (uint16_t)(((512 + 128 * side) >> 6) | 0x10 | (1 << 5));   /* 4-bit, additive */
    static const int cdx[4] = {-1, 1, -1, 1}, cdy[4] = {-1, -1, 1, 1};
    for (int k = 0; k < n && k < 16; k++) {
        int a = spin + k * 4096 / n, cs = icos(a), sn = isin(a);
        int px = cx + rr * cs / 4096, py = cy + rr * sn / 4096;
        int rc = icos(a + 1024), rs = isin(a + 1024);
        uint32_t pr = prims + 40u * (uint32_t)k;
        MEM_W8(pr + 3, 9);
        MEM_W32(pr + 4, 0x2E808080u);                    /* textured, semi-transparent */
        for (int c = 0; c < 4; c++) {
            int qx = px + (cdx[c] * hs * rc - cdy[c] * hs * rs) / 4096;
            int qy = py + (cdx[c] * hs * rs + cdy[c] * hs * rc) / 4096;
            MEM_W32(pr + 8 + 8u * (uint32_t)c, ((uint32_t)(uint16_t)qx) | ((uint32_t)(uint16_t)qy << 16));
            uint32_t uv = (uint32_t)((u + (cdx[c] > 0 ? 63 : 0)) | ((v + (cdy[c] > 0 ? 63 : 0)) << 8));
            if (c == 0) uv |= (uint32_t)clut << 16;
            if (c == 1) uv |= (uint32_t)tpage << 16;
            MEM_W32(pr + 12 + 8u * (uint32_t)c, uv);
        }
        R(4) = ot; R(5) = pr; f_80207E5C();
    }
}

/* The Meteor Combo's grab is a swirl of music notes (her texture's three note frames, u 0 / 64 / 128, palette 0:
 * white notes): 8 small notes circling and turning, each a note and a tint of its own (pink, yellow, cyan, white). */
static void fx_notes(uint32_t ob, int side, int32_t x, int32_t y, int32_t z, int half, int t) {
    static const uint32_t tint[4] = {0xC080F0, 0x60E0F0, 0xF0E070, 0xF0F0F0};   /* BGR */
    int cx, cy, sz = project(x, y, z, &cx, &cy);
    if (sz < 64) return;
    int otz = (sz >> 3) - 8 - 6;
    if (otz < 1) otz = 1;
    if (otz > 0x3FE) return;
    int H = (int)(gte_cfc2(26) & 0xFFFF), hs = half * H / sz;
    uint32_t prims = ob + 0x400u + (MEM_R32(0x8034D808u) & 1) * 0x400u, ot = MEM_R32(0x8034D800u) + 0x80u + 4u * (uint32_t)otz;
    uint16_t clut = (uint16_t)(((256 + 0) << 6) | ((576 + 128 * side) >> 4));
    uint16_t tpage = (uint16_t)(((512 + 128 * side) >> 6) | 0x10 | (1 << 5));
    static const int cdx[4] = {-1, 1, -1, 1}, cdy[4] = {-1, -1, 1, 1};
    for (int k = 0; k < 8; k++) {
        int a = t * 0x60 + k * 512, r = (28 + 10 * ((k * 3 + t / 4) % 3)) * H / sz;   /* a loose, breathing ring */
        int px = cx + r * icos(a) / 4096, py = cy + r * isin(a) / 4096 * 3 / 4;
        int tilt = (t * 40 + k * 300) & 0xFFF, rc = icos(tilt / 8 - 256), rs = isin(tilt / 8 - 256);
        uint32_t pr = prims + 40u * (uint32_t)k;
        int u = 64 * (k % 3);
        MEM_W8(pr + 3, 9);
        MEM_W32(pr + 4, 0x2E000000u | tint[k & 3]);
        for (int c = 0; c < 4; c++) {
            int qx = px + (cdx[c] * hs * rc - cdy[c] * hs * rs) / 4096;
            int qy = py + (cdx[c] * hs * rs + cdy[c] * hs * rc) / 4096;
            MEM_W32(pr + 8 + 8u * (uint32_t)c, ((uint32_t)(uint16_t)qx) | ((uint32_t)(uint16_t)qy << 16));
            uint32_t uv = (uint32_t)((u + (cdx[c] > 0 ? 63 : 0)) | ((cdy[c] > 0 ? 63 : 0) << 8));
            if (c == 0) uv |= (uint32_t)clut << 16;
            if (c == 1) uv |= (uint32_t)tpage << 16;
            MEM_W32(pr + 12 + 8u * (uint32_t)c, uv);
        }
        R(4) = ot; R(5) = pr; f_80207E5C();
    }
}

/* air QCF x2+P's projectile, type 0x29 (EX2 Plus 8004BCC0): a spinning field in front of her that stays where it
 * was made, up to 5 hits 5 frames apart (the last one knocks down), 48 frames. */
static void ob_spin_field(uint32_t ob) {
    uint32_t owner = MEM_R32(ob + 0x20A8);
    int state = MEM_R8(ob + 4);
    if (state == 0) {
        MEM_W8(ob + 4, 1);
        for (uint32_t i = 0xA8; i < 0xC0; i++) MEM_W8(ob + i, 0);
        MEM_W8(ob + OB_HITS, 5);
        MEM_W16(ob + OB_LIFE, 48);
        MEM_W16(ob + 0x64, MEM_R16(ob + 0x64) & 0xFFE7);
        MEM_W32(ob + 0x20B4, her_attr(owner, 0x371));
        MEM_W8(ob + 0x20FA, 160);
        MEM_W8(ob + 0x20FB, 7);
        /* in front of her: her chest node's world position, 0x180 ahead (EX2 Plus: (0, 0, -0x180) turned by her
         * facing; ahead is minus sin/cos of the facing here), pitched down 0x100 */
        int32_t x = (int32_t)MEM_R32(owner + 0x420), y = (int32_t)MEM_R32(owner + 0x424), z = (int32_t)MEM_R32(owner + 0x428);
        R(4) = MEM_R16(owner + 0x52); f_802CE4DC(); int32_t sn = (int32_t)R(2);
        R(4) = MEM_R16(owner + 0x52); f_802CE5AC(); int32_t cs = (int32_t)R(2);
        R(4) = 0x100; f_802CE4DC(); int32_t sp = (int32_t)R(2);
        R(4) = 0x100; f_802CE5AC(); int32_t cp = (int32_t)R(2);
        int32_t ahead = 0x180 * cp / 4096, down = 0x180 * sp / 4096;
        MEM_W32(ob + 0x10, (uint32_t)((x - ahead * sn / 4096) << 16));
        MEM_W32(ob + 0x14, (uint32_t)((y + down) << 16));
        MEM_W32(ob + 0x18, (uint32_t)((z - ahead * cs / 4096) << 16));
        object_sphere(ob, 0, 160, 1);
        return;
    }
    MEM_W16(ob + OB_SPIN, (uint16_t)(MEM_R16(ob + OB_SPIN) + 0x100));
    if (!objects_paused()) {
        switch (state) {
        case 1:
            if (MEM_R16(ob + OB_LIFE)) MEM_W16(ob + OB_LIFE, (uint16_t)(MEM_R16(ob + OB_LIFE) - 1));
            else MEM_W8(ob + 4, 5);
            break;
        case 2: case 3: {                                /* it hit (or was guarded, or clashed) */
            int hits = MEM_R8(ob + OB_HITS) - 1;
            MEM_W8(ob + OB_HITS, (uint8_t)(hits < 0 ? 0 : hits));
            if (hits <= 0) { MEM_W8(ob + 0x20FB, 0); MEM_W8(ob + 4, 6); }
            else { MEM_W16(ob + OB_WAIT, 5); MEM_W8(ob + 4, 4); }
            break;
        }
        case 4:
            if (MEM_R16(ob + OB_WAIT) > 1) { MEM_W16(ob + OB_WAIT, (uint16_t)(MEM_R16(ob + OB_WAIT) - 1)); break; }
            if (MEM_R8(ob + OB_HITS) == 1) MEM_W32(ob + 0x20B4, her_attr(owner, 0x375));   /* the last one */
            MEM_W8(ob + 0x20FB, (uint8_t)(MEM_R8(ob + 0x20FB) | 2));
            object_sphere(ob, 0, 160, 1);
            MEM_W8(ob + 4, 1);
            break;
        case 5:
            MEM_W8(ob + 0x20FB, 0);
            MEM_W8(ob + 4, 6);
            break;
        default:
            MEM_W8(ob, 0);                               /* free */
            return;
        }
    }
    object_sphere(ob, 0, 160, -1);
    if (MEM_R8(ob + 0x20FB) & 1 || MEM_R8(ob + 4) == 4) {   /* drawn: a spinning ring of her field sprites */
        uint32_t owner2 = MEM_R32(ob + 0x20A8);
        int life = MEM_R16(ob + OB_LIFE), grow = life < 10 ? (10 - life) * 12 : 0;
        int side = MEM_R16(owner2 + 0x64) & 1;
        if (fx_upload(side))
            fx_ring(ob, side, (int16_t)MEM_R16(ob + 0x12), (int16_t)MEM_R16(ob + 0x16), (int16_t)MEM_R16(ob + 0x1A),
                    8, 70 + grow / 2, 90 + grow, MEM_R16(ob + OB_SPIN), 0, 64, 1);
    }
}

/* Sparkles around an object (EX2 Plus 8004B71C, the Meteor Combo's grab): 16 particles in the object's work area
 * (+0x1200, 16 bytes each: s16 x, y, z, s8 vx, vy, vz, u8 r, g, b, life), each born at the object with a random
 * offset and bright colour, drifting and fading over 14 frames; drawn as small additive sprites (prims at +0x1400,
 * two buffers). */
#define PARTICLES 16
#define PART_STATE 0x1200u
#define PART_PRIMS 0x1400u
static uint32_t g_part_seed = 12345;
static int part_rand(int n) { g_part_seed = g_part_seed * 1103515245u + 12345u; return (int)((g_part_seed >> 16) % (uint32_t)n); }

static void particles(uint32_t ob, int side, int on) {
    uint32_t ps = ob + PART_STATE;
    uint32_t prims = ob + PART_PRIMS + (MEM_R32(0x8034D808u) & 1) * 0x300u;
    uint32_t ot0 = MEM_R32(0x8034D800u) + 0x80u;
    uint16_t clut = (uint16_t)(((256 + 1) << 6) | ((576 + 128 * side) >> 4));   /* the soft ring, palette 1 */
    uint16_t tpage = (uint16_t)(((512 + 128 * side) >> 6) | 0x10 | (1 << 5));
    int k = 0;
    for (int i = 0; i < PARTICLES; i++) {
        uint32_t q = ps + 16u * (uint32_t)i;
        int life = MEM_R8(q + 15);
        if (!objects_paused()) {
            if (life == 0 && on) {                       /* born at the object */
                MEM_W16(q, (uint16_t)((int16_t)MEM_R16(ob + 0x12) + part_rand(128) - 64));
                MEM_W16(q + 2, (uint16_t)((int16_t)MEM_R16(ob + 0x16) + part_rand(128) - 64));
                MEM_W16(q + 4, (uint16_t)((int16_t)MEM_R16(ob + 0x1A) + part_rand(128) - 64));
                for (int c = 0; c < 3; c++) MEM_W8(q + 6 + (uint32_t)c, (uint8_t)(int8_t)(part_rand(9) - 4));
                for (int c = 0; c < 3; c++) MEM_W8(q + 12 + (uint32_t)c, (uint8_t)(0x30 + part_rand(192)));
                life = 8 + part_rand(7);
            } else if (life) {
                for (int c = 0; c < 3; c++)
                    MEM_W16(q + 2u * (uint32_t)c, (uint16_t)((int16_t)MEM_R16(q + 2u * (uint32_t)c) + (int8_t)MEM_R8(q + 6 + (uint32_t)c)));
                life--;
            }
            MEM_W8(q + 15, (uint8_t)life);
        }
        if (!life) continue;
        int sx, sy, sz = project((int16_t)MEM_R16(q), (int16_t)MEM_R16(q + 2), (int16_t)MEM_R16(q + 4), &sx, &sy);
        int otz = (sz >> 3) - 8 - 6;
        if (sz < 64 || otz < 1 || otz > 0x3FE) continue;
        int H = (int)(gte_cfc2(26) & 0xFFFF), hs = 16 * H / sz;
        if (hs < 1) hs = 1;
        uint32_t rgb = 0;
        for (int c = 0; c < 3; c++) rgb |= (uint32_t)(MEM_R8(q + 12 + (uint32_t)c) * life / 14) << (8 * c);
        uint32_t pr = prims + 40u * (uint32_t)k++;
        MEM_W8(pr + 3, 9);
        MEM_W32(pr + 4, 0x2E000000u | rgb);
        static const int cdx[4] = {-1, 1, -1, 1}, cdy[4] = {-1, -1, 1, 1};
        for (int c = 0; c < 4; c++) {
            MEM_W32(pr + 8 + 8u * (uint32_t)c, ((uint32_t)(uint16_t)(sx + cdx[c] * hs)) | ((uint32_t)(uint16_t)(sy + cdy[c] * hs) << 16));
            uint32_t uv = (uint32_t)((cdx[c] > 0 ? 63 : 0) | ((64 + (cdy[c] > 0 ? 63 : 0)) << 8));
            if (c == 0) uv |= (uint32_t)clut << 16;
            if (c == 1) uv |= (uint32_t)tpage << 16;
            MEM_W32(pr + 12 + 8u * (uint32_t)c, uv);
        }
        R(4) = ot0 + 4u * (uint32_t)otz; R(5) = pr; f_80207E5C();
    }
}

/* the Meteor Combo's grab, type 0x19 (EX2 Plus 8004A500): thrown from her hand, it flies on (slowing), turning
 * a little towards him, for 24 frames; a hit that is not guarded, while she is still throwing it, is a catch. Not
 * done yet: its glow particles, the burst when guarded. */
#define OB_SPEED 0xB0u    /* s32 its speed */
static void ob_grab(uint32_t ob) {
    uint32_t owner = MEM_R32(ob + 0x20A8), opp = MEM_R32(owner + 0x58), data = MEM_R32(owner + 0x1B58);
    int state = MEM_R8(ob + 4);
    if (state == 0) {
        MEM_W8(ob + 4, 1);
        for (uint32_t i = 0xA8; i < 0xC0; i++) MEM_W8(ob + i, 0);
        MEM_W16(ob + OB_LIFE, 24);
        MEM_W32(ob + OB_SPEED, 0x200);
        MEM_W16(ob + 0x64, MEM_R16(ob + 0x64) & 0xFFE7);
        MEM_W32(ob + 0x20B4, her_attr(owner, 0x371));
        MEM_W8(ob + 0x20FA, 48);
        MEM_W8(ob + 0x20FB, 7);
        /* from her hand (node 11), a little above her root's height */
        MEM_W32(ob + 0x10, MEM_R32(owner + 0x250 + 0xE8 * 11) << 16);
        MEM_W32(ob + 0x14, (uint32_t)((int32_t)(int16_t)(MEM_R16(owner + 0x16) - 0x140) << 16));
        MEM_W32(ob + 0x18, MEM_R32(owner + 0x258 + 0xE8 * 11) << 16);
        object_sphere(ob, 0, 48, 1);
        object_sphere(ob, 1, 48, 1);
        for (uint32_t i = 0; i < 16u * PARTICLES; i++) MEM_W8(ob + PART_STATE + i, 0);
        return;
    }
    MEM_W16(ob + OB_SPIN, (uint16_t)(MEM_R16(ob + OB_SPIN) + 1));
    if (!objects_paused()) {
        switch (state) {
        case 1: {
            uint8_t ost = MEM_R8(owner + 0x1B42);
            if (ost == 0x1C || ost == 0x1D || !MEM_R16(ob + OB_LIFE)) {   /* she was hit, or it is spent */
                MEM_W8(ob + 0x20FB, 1); MEM_W8(ob + 4, 5); MEM_W16(ob + OB_WAIT, 8);
                break;
            }
            MEM_W16(ob + OB_LIFE, (uint16_t)(MEM_R16(ob + OB_LIFE) - 1));
            /* turn towards him, 4 a frame, if he is within 0x200 of its heading */
            int dx = (int16_t)MEM_R16(opp + 0x12) - (int16_t)MEM_R16(ob + 0x12);
            int dz = (int16_t)MEM_R16(opp + 0x1A) - (int16_t)MEM_R16(ob + 0x1A);
            R(4) = (uint32_t)dx; R(5) = (uint32_t)dz; f_802D0C9C();
            int want = (int)R(2) & 0xFFF, facing = MEM_R16(ob + 0x52) & 0xFFF;
            int diff = ((want - facing + 0x800) & 0xFFF) - 0x800;
            if (diff > -0x200 && diff < 0x200) {
                facing = (facing + (diff > 4 ? 4 : diff < -4 ? -4 : diff)) & 0xFFF;
                MEM_W16(ob + 0x52, (uint16_t)facing);
                MEM_W16(ob + 0x4A, (uint16_t)facing);
            }
            /* on along its heading: speed 0x200 = 32 a frame, slowing by 1 (EX2 Plus: velocity from its facing,
             * acceleration -1, integrate); moved here, as Fighting Layer's integrate scales an object's step down */
            int32_t speed = (int32_t)MEM_R32(ob + OB_SPEED) - 1;
            if (speed < 0) speed = 0;
            MEM_W32(ob + OB_SPEED, (uint32_t)speed);
            MEM_W32(ob + 0x10, MEM_R32(ob + 0x10) - (uint32_t)(isin(facing) * speed));   /* ahead: minus sin/cos */
            MEM_W32(ob + 0x18, MEM_R32(ob + 0x18) - (uint32_t)(icos(facing) * speed));
            break;
        }
        case 2: case 3:                                  /* it hit, was guarded, or clashed */
            MEM_W8(ob + 0x20FB, 1);
            if (state == 2 && MEM_R8(opp + 0x1B42) != 0x16 && !(MEM_R16(ob + 0x64) & 0x10) &&
                MEM_R16(owner + 0x1CA) == MEM_R16(ob + 0x20AC)) {
                MEM_W8(data + CATCH, 1);                 /* caught: her Meteor Combo goes on to its dance */
                MEM_W8(ob + 4, 7);
            } else {
                MEM_W8(ob + 4, 5);
                MEM_W16(ob + OB_WAIT, 8);
            }
            break;
        case 7:                                          /* held while she has him */
            if (MEM_R8(owner + 0x1B42) != 0x19 && !MEM_R8(data + CATCH)) { MEM_W8(ob + 4, 5); MEM_W16(ob + OB_WAIT, 8); }
            break;
        case 5:                                          /* fading out */
            if (MEM_R16(ob + OB_WAIT)) { MEM_W16(ob + OB_WAIT, (uint16_t)(MEM_R16(ob + OB_WAIT) - 1)); break; }
            MEM_W8(ob + 0x20FB, 0);
            MEM_W8(ob + 4, 6);
            break;
        default:
            MEM_W8(ob, 0);
            return;
        }
    }
    if (MEM_R8(ob + 4) == 1) {                           /* its front sphere: radius ahead along its heading */
        object_sphere(ob, 0, 48, -1);
        uint32_t sp1 = ob + 0x20C0u;
        int f = MEM_R16(ob + 0x52);
        MEM_W16(sp1, (uint16_t)((int16_t)MEM_R16(ob + 0x12) - 48 * isin(f) / 4096));
        MEM_W16(sp1 + 2, MEM_R16(ob + 0x16));
        MEM_W16(sp1 + 4, (uint16_t)((int16_t)MEM_R16(ob + 0x1A) - 48 * icos(f) / 4096));
        MEM_W8(sp1 + 6, 48);
    }
    if (fx_upload(MEM_R16(owner + 0x64) & 1))           /* its sparkles, while it flies or holds him */
        particles(ob, MEM_R16(owner + 0x64) & 1, MEM_R8(ob + 4) == 1 || MEM_R8(ob + 4) == 7);
    if (MEM_R8(ob + 0x20FB) & 1) {                       /* drawn: a swirl of her music notes */
        int side = MEM_R16(owner + 0x64) & 1;
        int half = MEM_R8(ob + 4) == 5 ? 2 * MEM_R16(ob + OB_WAIT) + 4 : 20;
        if (fx_upload(side))
            fx_notes(ob, side, (int16_t)MEM_R16(ob + 0x12), (int16_t)MEM_R16(ob + 0x16), (int16_t)MEM_R16(ob + 0x1A),
                     half, MEM_R16(ob + OB_SPIN));
    }
}

/* ---- her generic motions: views of the shared motion banks ----
 * Fighting Layer plays shared (COMMON) motions, banks 0-2, for what every character does (hit reactions, guard,
 * crouching, jumps, landing, lying, getting up ...). A side's motion banks are the table D_802DCAA4 (player 1) /
 * D_802DCAB8 (player 2): banks 0-4, motion = bank + index[n]; poses are bank + the key's offset. For her side,
 * banks 0 and 1 point at views built here: her EX2 Plus versions of the motions in MOTDAT/VIEW.DAT (make_swap.py
 * VIEW_MAP), and for every other id a copy of Fighting Layer's header and keys with the pose offsets pointing back
 * at the shared file. The motion numbers stay Fighting Layer's, so the code that checks them, the hit records and
 * the chains all work as before. The views live in RAM the game leaves unused (0x8036D4B0-0x803EDE60, docs/
 * NEW_CHARACTER_SLOT.md 2.4). */
#define BANKS(side) ((side) ? 0x802DCAB8u : 0x802DCAA4u)
#define VIEW_BASE(side) (0x8036E000u + 0x10000u * (uint32_t)(side))
static uint8_t *g_view_file;
static long g_view_size = -1;
static uint32_t g_common[2], g_view_built[2], g_view_at[2][2];   /* the shared banks; what each view was built on */

static int view_load(void) {
    if (g_view_size >= 0) return g_view_size > 0;
    g_view_size = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/MOTDAT/VIEW.DAT", extra_dir() ? extra_dir() : ".");
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_view_file = n > 8 ? malloc((size_t)n) : NULL;
    if (g_view_file && fread(g_view_file, 1, (size_t)n, f) == (size_t)n && !memcmp(g_view_file, "EXVW", 4)) g_view_size = n;
    fclose(f);
    rt_log("pullum: generic motions %s%s", path, g_view_size > 0 ? "" : " not valid");
    return g_view_size > 0;
}

static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

/* keys of a motion (with a chain's id entry), as make_swap.py counts them */
static int motion_keys(uint32_t m, int *last) {
    *last = MEM_R16(m);
    int n = *last + 1;
    while (n <= *last + 3) {
        uint32_t po = MEM_R32(m + 8 + 8u * (uint32_t)n + 4);
        if (po >> 24 != 8) break;
        n++;
        if (po == 0x08000002u) { n++; break; }
    }
    return n;
}

/* rebase a motion's pose offsets at m (copied) by delta, skipping control entries and a chain's id */
static void motion_rebase(uint32_t m, int nkeys, int last, uint32_t delta) {
    for (int k = 0; k < nkeys; k++) {
        uint32_t a = m + 8 + 8u * (uint32_t)k + 4, po = MEM_R32(a);
        if (po >> 24 == 8) { if (po == 0x08000002u) break; continue; }
        if (k <= last) MEM_W32(a, po + delta);
    }
}

static void view_build(int side) {
    uint32_t cur = VIEW_BASE(side), end = cur + 0x10000u;
    const uint8_t *f = g_view_file + 4;
    for (int b = 0; b < 2; b++) {
        int count = f[0] | f[1] << 8;
        const uint8_t *offs = f + 4;
        f += 4 + 4 * count;
        uint32_t view = cur, common = g_common[b];
        cur += 4u * (uint32_t)count;
        for (int n = 0; n < count && cur < end - 0x400; n++) {
            uint32_t o = rd32(offs + 4 * n);
            if (o && o < (uint32_t)g_view_size) {         /* hers: copied up to the next blob */
                uint32_t next = (uint32_t)g_view_size;
                for (int q = 0; q < count; q++) { uint32_t x = rd32(offs + 4 * q); if (x > o && x < next) next = x; }
                for (int q = 0; b == 0 && q < 64; q++) {  /* blobs of bank 1 follow bank 0's */
                    const uint8_t *t2 = g_view_file + 4 + 4 + 4 * count;
                    int c2 = t2[0] | t2[1] << 8;
                    if (q >= c2) break;
                    uint32_t x = rd32(t2 + 4 + 4 * q);
                    if (x > o && x < next) next = x;
                }
                if (next - o > end - cur) break;
                for (uint32_t i = 0; i < next - o; i++) MEM_W8(cur + i, g_view_file[o + i]);
                int last, nk = motion_keys(cur, &last);
                motion_rebase(cur, nk, last, cur - view);
                MEM_W32(view + 4u * (uint32_t)n, cur - view);
                cur += (next - o + 3) & ~3u;
                continue;
            }
            uint32_t fo = MEM_R32(common + 4u * (uint32_t)n);
            if (fo >> 24 == 8) { MEM_W32(view + 4u * (uint32_t)n, fo); continue; }   /* a marker */
            uint32_t m = common + fo;
            int last, nk = motion_keys(m, &last);
            uint32_t len = 8 + 8u * (uint32_t)nk;
            for (uint32_t i = 0; i < len; i += 4) MEM_W32(cur + i, MEM_R32(m + i));
            motion_rebase(cur, nk, last, common - view);  /* back at the shared file's poses */
            MEM_W32(view + 4u * (uint32_t)n, cur - view);
            cur += len;
        }
        g_view_at[side][b] = view;
    }
    g_view_built[side] = g_common[0];
    rt_log("pullum: generic motions for side %d, %u bytes", side, cur - VIEW_BASE(side));
}

/* her side uses her views, any other side the shared banks */
static void views_apply(int side) {
    uint32_t p = side ? 0x8034BC7Cu : 0x8034A100u, t = BANKS(side);
    if (!g_common[0]) return;
    int her = roster_is_extra(p) && MEM_R8(p + 2) == 0 && view_load();
    if (her && g_view_built[side] != g_common[0]) view_build(side);
    for (int b = 0; b < 2; b++) {
        uint32_t want = her ? g_view_at[side][b] : g_common[b];
        if (MEM_R32(t + 4u * (uint32_t)b) != want) MEM_W32(t + 4u * (uint32_t)b, want);
    }
}

/* the shared motions were loaded (func_802144B0 sets banks 0-2 of both sides) */
void f_802144B0_orig(void);
void f_802144B0(void) {
    f_802144B0_orig();
    g_common[0] = MEM_R32(0x802DCAA4u);
    g_common[1] = MEM_R32(0x802DCAA4u + 4);
    views_apply(0);
    views_apply(1);
}

/* "Play it from the other player's file": +0x1C9 & 0x88 makes the motion functions take the opponent's motion
 * banks (D_802DCACC[his side]) instead of the player's own, for a move that makes its victim play the attacker's
 * motions. For banks 0-1 that changes nothing for Fighting Layer's characters, who all share them; but her side's
 * banks 0-1 are her views, whose pose offsets are relative to her view, so through his banks every pose was read
 * from the wrong place. Joe's super (half-circle forward + punch) hits its victim with bank-0 hit reactions played
 * this way: her root ran off through the floor and her mesh came apart. So for her the flag is left out while the
 * motion is in bank 0 or 1 (setters: the new motion's bank; per-frame steps: the current one, +0x1CB). */
static int her_views_on(uint32_t p) {
    int side = MEM_R16(p + 0x64) & 1;
    return g_common[0] && MEM_R32(BANKS(side)) != g_common[0] && roster_is_extra(p) && MEM_R8(p + 2) == 0;
}
#define OWN_BANKS_WRAPPER(addr, bank_expr, keep) \
    void f_##addr##_orig(void); \
    void f_##addr(void) { \
        uint32_t p = R(4); \
        uint8_t flags = MEM_R8(p + 0x1C9); \
        if (!(flags & 0x88) || !her_views_on(p) || (bank_expr) >= 2) { f_##addr##_orig(); return; } \
        MEM_W8(p + 0x1C9, flags & ~0x88u); \
        f_##addr##_orig(); \
        MEM_W8(p + 0x1C9, (uint8_t)(MEM_R8(p + 0x1C9) | (flags & (keep)))); \
    }
/* the setters keep the flag (Fighting Layer's setter keeps it too); the steps put back only bit 7 (they clear
 * bit 3 themselves once used) */
OWN_BANKS_WRAPPER(802145F0, (R(5) >> 8) & 0xFF, 0x88)
OWN_BANKS_WRAPPER(8021483C, (R(5) >> 8) & 0xFF, 0x88)
OWN_BANKS_WRAPPER(80216234, MEM_R8(p + 0x1CB), 0x80)
OWN_BANKS_WRAPPER(80216A70, MEM_R8(p + 0x1CB), 0x80)
OWN_BANKS_WRAPPER(80215E98, MEM_R8(p + 0x1CB), 0x80)
OWN_BANKS_WRAPPER(80215284, MEM_R8(p + 0x1CB), 0x80)

/* ---- her leg trails ----
 * EX2 Plus draws a streak behind her during her rising kick, flying kick and spins (its second effect pool, which
 * Fighting Layer does not have). Here: while a trail is on, each frame keeps both shins (knee and ankle: nodes
 * 14-15 and 17-18, world positions) for 8 frames, and the segments a leg moved fast along are drawn as additive
 * quads (POLY_GT4 with a glow of her effect texture), fading with age. */
#define TRAIL_N 8
typedef struct { int16_t p[TRAIL_N][2][2][3]; int count; int until; } Trail;   /* [age][leg][knee, ankle][xyz] */
static Trail g_trail[2];

static void trail_on(uint32_t p, int frames) {
    Trail *t = &g_trail[MEM_R16(p + 0x64) & 1];
    if (t->until < (int)g_frame + frames) t->until = (int)g_frame + frames;
    if (t->count == 0 || t->until - frames > (int)g_frame + 1) t->count = 0;
}

static void node_pos(uint32_t p, int n, int16_t out[3]) {
    for (int i = 0; i < 3; i++) out[i] = (int16_t)(int32_t)MEM_R32(p + 0x250u + 0xE8u * (uint32_t)n + 4u * (uint32_t)i);
}

static void trail_frame(int side) {
    Trail *t = &g_trail[side];
    uint32_t p = side ? 0x8034BC7Cu : 0x8034A100u;
    if (!roster_is_extra(p) || MEM_R8(p + 2) != 0 || (int)g_frame > t->until) { t->count = 0; return; }
    if (!objects_paused()) {
        memmove(t->p[1], t->p[0], sizeof t->p[0] * (TRAIL_N - 1));
        static const int nodes[2][2] = {{14, 15}, {17, 18}};
        for (int leg = 0; leg < 2; leg++)
            for (int j = 0; j < 2; j++) node_pos(p, nodes[leg][j], t->p[0][leg][j]);
        if (t->count < TRAIL_N) t->count++;
    }
    if (t->count < 2 || !fx_upload(side)) return;
    uint32_t prims = MEM_R32(p + 0x1B58) + TRAIL_PRIMS + (MEM_R32(0x8034D808u) & 1) * 0x400u;
    uint16_t clut = (uint16_t)(((256 + 0) << 6) | ((576 + 128 * side) >> 4));   /* palette 0: an even bright glow */
    uint16_t tpage = (uint16_t)(((512 + 128 * side) >> 6) | 0x10 | (1 << 5));
    int k = 0;
    for (int leg = 0; leg < 2; leg++)
        for (int a = 0; a + 1 < t->count; a++) {
            const int16_t (*q0)[3] = t->p[a][leg], (*q1)[3] = t->p[a + 1][leg];
            int dx = q0[1][0] - q1[1][0], dy = q0[1][1] - q1[1][1], dz = q0[1][2] - q1[1][2];
            if (dx * dx + dy * dy + dz * dz < 24 * 24) continue;   /* the leg was not swinging here */
            int sx[4], sy[4], sz = 0, ok = 1;
            const int16_t *pts[4] = {q0[0], q0[1], q1[0], q1[1]};
            for (int c = 0; c < 4; c++) {
                int z = project(pts[c][0], pts[c][1], pts[c][2], &sx[c], &sy[c]);
                if (z < 64) ok = 0;
                sz += z;
            }
            int otz = (sz / 4 >> 3) - 8 - 2;
            if (!ok || otz < 1 || otz > 0x3FE) continue;
            int glow0 = 0x70 * (TRAIL_N - a) / TRAIL_N, glow1 = 0x70 * (TRAIL_N - a - 1) / TRAIL_N;
            uint32_t pr = prims + 52u * (uint32_t)k++;
            MEM_W8(pr + 3, 12);
            for (int c = 0; c < 4; c++) {
                int g = c < 2 ? glow0 : glow1;
                uint32_t rgb = (uint32_t)(g | g << 8 | g << 16);
                MEM_W32(pr + 4 + 12u * (uint32_t)c, rgb | (c == 0 ? 0x3E000000u : 0));   /* gouraud, textured, semi */
                MEM_W32(pr + 8 + 12u * (uint32_t)c, ((uint32_t)(uint16_t)sx[c]) | ((uint32_t)(uint16_t)sy[c] << 16));
                uint32_t uv = (uint32_t)((24 + (c & 1) * 16) | ((88 + (c >> 1) * 16) << 8));   /* a field glow */
                if (c == 0) uv |= (uint32_t)clut << 16;
                if (c == 1) uv |= (uint32_t)tpage << 16;
                MEM_W32(pr + 12 + 12u * (uint32_t)c, uv);
            }
            R(4) = MEM_R32(0x8034D800u) + 0x80u + 4u * (uint32_t)otz; R(5) = pr; f_80207E5C();
            if (k >= 2 * (TRAIL_N - 1)) return;
        }
}

/* ---- her ghost images ----
 * During her supers EX2 Plus draws afterimages: copies of her from a few frames back, see-through and tinted. Here:
 * each frame her player record (pose, node matrices, position) is kept for GHOST_HIST frames. While a ghost is on,
 * before she is drawn (func_802069CC, src/game/ex_mirror.c), she is drawn three more times from older records.
 * Each of those draws links her packets into the ordering table; they are found by comparing the table before and
 * after (the renderers add packets at the head of a depth's list), copied to RAM the game leaves unused, made
 * additive and tinted (fading with age), and the table is put back with the copies in front. Then her real draw
 * reuses her packets as usual. Packets that were already see-through (her shadow) are left out. */
#define PLAYER_SIZE 0x1B7Cu
#define GHOST_HIST 12
#define GHOST_PRIMS 0x803A0000u                         /* two 0x18000 buffers (docs/NEW_CHARACTER_SLOT.md 2.4) */
#define GHOST_BUF 0x18000u
#define OT_FIRST 0x70u                                  /* the frame's ordering table: 0x818 entries at +0x70 */
#define OT_COUNT 0x818u
typedef struct { uint8_t pl[GHOST_HIST][PLAYER_SIZE]; int head, count, until; uint32_t last_ot; } Ghost;
static Ghost g_ghost[2];
int g_ghost_drawing;                                    /* read by the draw wrapper and the ponytail */
static uint32_t g_gbuf_ot, g_gbuf_off;
static uint32_t g_ot_before[OT_COUNT];

/* keep the ghosts on for a few more frames (called every frame of a move that has them) */
static void ghost_on(uint32_t p) {
    Ghost *g = &g_ghost[MEM_R16(p + 0x64) & 1];
    if (g->until < (int)g_frame) g->count = 0;          /* a fresh set: only frames from now on */
    g->until = (int)g_frame + 2;
}

/* A see-through textured polygon blends only the texels whose palette entry has bit 15 set, and her palettes have
 * none. So the ghosts use a copy of her palette row (480 / 482, sixteen 16-colour palettes) with the bit set on
 * every colour but the clear one, at (576 + 128 * side, 300), four rows of 64. */
#define GHOST_CLUT_Y 300
static void ghost_palettes(int side) {
    uint16_t row[256];
    for (int i = 0; i < 256; i++) {
        uint16_t c = g_vram[((480 + 2 * side) << 10) | i];
        row[i] = c ? (uint16_t)(c | 0x8000) : 0;
    }
    for (int r = 0; r < 4; r++) vram_put_rect(576 + 128 * side, GHOST_CLUT_Y + r, 64, 1, row + 64 * r);
}

/* a copied packet: additive, its colours scaled by the tint, on the ghost palettes. 0: leave it out (a layout or a
 * palette the ghost cannot use) */
static int ghost_tint(uint32_t c, int len, int side, const int tint[3]) {
    uint8_t code = MEM_R8(c + 7);
    if ((code & 0xE0) != 0x20) return 1;
    int quad = code & 8, tex = code & 4, gour = code & 0x10, nv = quad ? 4 : 3;
    if (len != nv * (1 + (tex ? 1 : 0)) + (gour ? nv : 1)) return 0;
    uint32_t w = c + 4;
    for (int v = 0; v < nv; v++) {
        if (v == 0 || gour) {
            uint32_t rgb = MEM_R32(w), out = 0;
            for (int k = 0; k < 3; k++) {
                int x = (int)((rgb >> (8 * k)) & 0xFF) * tint[k] / 128;
                out |= (uint32_t)(x > 255 ? 255 : x) << (8 * k);
            }
            if (v == 0) out |= (uint32_t)((code | 2) & ~1u) << 24;   /* see-through, colour-modulated */
            MEM_W32(w, out);
            w += 4;
        }
        w += 4;                                         /* xy */
        if (tex) {
            uint32_t uv = MEM_R32(w), hi = uv >> 16;
            if (v == 0) {                               /* its palette: the ghost copy */
                int px = (int)(hi & 0x3F) * 16, py = (int)(hi >> 6);
                if (py != 480 + 2 * side || px >= 256) return 0;
                int i = px / 16, gx = 576 + 128 * side + 16 * (i % 4), gy = GHOST_CLUT_Y + i / 4;
                hi = (uint32_t)((gy << 6) | (gx >> 4));
            }
            if (v == 1) hi = (hi & ~0x60u) | 0x20u;      /* its texture page: additive */
            MEM_W32(w, (uv & 0xFFFFu) | (hi << 16));
            w += 4;
        }
    }
    return 1;
}

/* move the packets the last draw linked in into the ghost buffer, tinted, and put the table back */
static void ghost_collect(uint32_t ot, uint32_t buf, int side, const int tint[3]) {
    for (uint32_t i = 0; i < OT_COUNT; i++) {
        uint32_t a = ot + OT_FIRST + 4u * i, now = MEM_R32(a), was = g_ot_before[i];
        if (now == was) continue;
        uint32_t stop = was & 0xFFFFFFu, q = now & 0xFFFFFFu, first = 0, prev = 0;
        int n = 0;
        while (q != stop && n++ < 4096) {
            uint32_t tag = MEM_R32(0x80000000u | q), len = tag >> 24;
            uint8_t code = MEM_R8(0x80000000u | (q + 7));
            int prim = (code & 0xE0) >= 0x20 && (code & 0xE0) <= 0x60;
            if (len && !(prim && (code & 2)) && g_gbuf_off + 4u * (len + 1) <= GHOST_BUF) {
                uint32_t c = buf + g_gbuf_off;
                for (uint32_t k = 0; k <= len; k++) MEM_W32(c + 4 * k, MEM_R32(0x80000000u | (q + 4 * k)));
                if (!ghost_tint(c, (int)len, side, tint)) { q = tag & 0xFFFFFFu; continue; }
                g_gbuf_off += 4u * (len + 1);
                if (prev) MEM_W32(prev, (MEM_R32(prev) & 0xFF000000u) | (c & 0xFFFFFFu));
                else first = c;
                prev = c;
            }
            q = tag & 0xFFFFFFu;
        }
        if (q != stop) prev = 0;                        /* not a plain head insertion: no ghost here */
        if (prev) {
            MEM_W32(prev, (MEM_R32(prev) & 0xFF000000u) | stop);
            MEM_W32(a, (was & 0xFF000000u) | (first & 0xFFFFFFu));
        } else MEM_W32(a, was);
    }
}

/* called by the draw wrapper before a fighter is drawn: keep her record, and draw her ghosts if they are on */
void f_802069CC(void);
void ex_ghosts_draw(uint32_t player, uint32_t a1) {
    int side = MEM_R16(player + 0x64) & 1;
    if (player != (side ? 0x8034BC7Cu : 0x8034A100u)) return;
    Ghost *g = &g_ghost[side];
    if (!roster_is_extra(player) || MEM_R8(player + 2) != 0) { g->count = 0; return; }
    uint32_t ot = MEM_R32(0x8034D800u);
    if (ot == g->last_ot) return;                       /* once a frame */
    g->last_ot = ot;
    if (!objects_paused()) {
        g->head = (g->head + 1) % GHOST_HIST;
        memcpy(g->pl[g->head], ram_ptr(player), PLAYER_SIZE);
        if (g->count < GHOST_HIST) g->count++;
    }
    if ((int)g_frame > g->until) return;
    static const int age[3] = {9, 6, 3};
    static const int tint[3][3] = {{0x28, 0x18, 0x38}, {0x40, 0x24, 0x58}, {0x60, 0x30, 0x78}};   /* violet, r g b */
    if (g_gbuf_ot != ot) { g_gbuf_ot = ot; g_gbuf_off = 0; }
    uint32_t buf = GHOST_PRIMS + (MEM_R32(0x8034D808u) & 1) * GHOST_BUF;
    static uint8_t live[PLAYER_SIZE];
    memcpy(live, ram_ptr(player), PLAYER_SIZE);
    ghost_palettes(side);
    uint32_t s16 = R(16), s17 = R(17), s18 = R(18), sp = R(29), ra = R(31);
    for (int k = 0; k < 3; k++) {
        if (age[k] >= g->count) continue;
        memcpy(ram_ptr(player), g->pl[(g->head - age[k] + GHOST_HIST) % GHOST_HIST], PLAYER_SIZE);
        for (uint32_t i = 0; i < OT_COUNT; i++) g_ot_before[i] = MEM_R32(ot + OT_FIRST + 4u * i);
        g_ghost_drawing = 1;
        R(4) = player; R(5) = a1; f_802069CC();
        g_ghost_drawing = 0;
        ghost_collect(ot, buf, side, tint[k]);
    }
    memcpy(ram_ptr(player), live, PLAYER_SIZE);
    R(16) = s16; R(17) = s17; R(18) = s18; R(29) = sp; R(31) = ra;
}

/* the engine's object update (func_8022A540): hers first, then the engine's own loop for the others (Kato's block
 * has no callbacks for her types, so it leaves hers alone) */
void f_8022A540_orig(void);
void f_8022A540(void) {
    uint32_t s16 = R(16), s17 = R(17), s18 = R(18), sp = R(29), ra = R(31);
    for (int i = 0; i < 4; i++) {
        uint32_t ob = OBJECTS + OBJ_SIZE * (uint32_t)i, owner = MEM_R32(ob + 0x20A8);
        if (!MEM_R8(ob) || !roster_is_extra(owner) || MEM_R8(owner + 2) != 0) continue;
        if (MEM_R8(ob + 2) == 0x29) ob_spin_field(ob);
        else if (MEM_R8(ob + 2) == 0x19) ob_grab(ob);
    }
    trail_frame(0);
    trail_frame(1);
    views_apply(0);
    views_apply(1);
    R(16) = s16; R(17) = s17; R(18) = s18; R(29) = sp; R(31) = ra;
    f_8022A540_orig();
}

/* air QCF x2+P super (EX2 Plus 80047040): a hop back, and at frame 10 the spinning field (type 0x29, slot 1).
 * 12 frames invulnerable; ghost images while it runs. */
static void sp_air_super(uint32_t p) {
    int step = MEM_R8(p + 5);
    if (step == 0) {
        MEM_W8(p + 5, 1);
        MEM_W8(p + 0x1B43, 3);
        call1(f_80289104, p);
        MEM_W8(p + 0x1B49, 0xFF);
        MEM_W8(p + 0x1B4A, 12);
        MEM_W8(p + 0x1B4C, 12);
        MEM_W16(p + 0x64, MEM_R16(p + 0x64) & 0xFFE7);
        call2(f_8028A9B4, p, 0x800);
        uint16_t f = MEM_R16(p + 0x64);
        MEM_W16(p + 0x64, (uint16_t)((f & 0x80) ? f | 4 : f & ~4u));
        call2(f_8028B9D8, p, 0x1F800000u);
        MEM_W16(p + 0x12, MEM_R16(0x1F800000u));
        MEM_W16(p + 0x1A, MEM_R16(0x1F800008u));
        call1(f_8028E178, p);                            /* the super: gauge, flash, announcer */
        call2(f_802145F0, p, 0x37C);                     /* 0x34D */
        return;
    }
    if (step <= 2) {
        if (step == 1 && (int16_t)MEM_R16(p + 0x1CE) >= 4) {
            MEM_W8(p + 5, 2);
            MEM_W32(p + 0x40, (uint32_t)-0x100);
            MEM_W32(p + 0x44, 2);
            MEM_W32(p + 0x2C, 0x200000);
            MEM_W32(p + 0x38, (uint32_t)-0x1C000);
            call1(f_80288F90, p);
        }
        if ((int16_t)MEM_R16(p + 0x1CE) == 10) {
            uint32_t ob = object_slot(p, 1);
            if (!MEM_R8(ob)) {
                R(4) = ob; R(5) = p; R(6) = 0x29; R(7) = 0; f_8027CD98();
                her_voice(p, 10);
            }
        }
        call1(f_80289164, p);
        call1(f_80215338, p);
        if (landed(p)) {
            MEM_W8(p + 5, 3);
            MEM_W8(p + 0x1B43, 0);
            call1(f_80289104, p);
            call2(f_802145F0, p, 0x368);                 /* landing: shared 0x10C */
        }
        return;
    }
    call1(f_80215338, p);
    if ((int16_t)MEM_R16(p + 0x1CE) >= 9) call2(f_802907EC, p, 0x11);
}

/* Meteor Combo, QCF x2 + LP+MP+HP (EX2 Plus 80046CC4): needs a full gauge (150) and spends all of it; she throws her
 * grab (type 0x19, slot 0); if it catches him she takes him into her dance (throw 3, th_meteor), otherwise she
 * recovers. 10 frames invulnerable. */
static void sp_meteor(uint32_t p) {
    uint32_t data = MEM_R32(p + 0x1B58), opp = MEM_R32(p + 0x58);
    switch (MEM_R8(p + 5)) {
    case 0:
        MEM_W8(p + 5, 1);
        special_entry(p, 10, 0);
        MEM_W8(data + CATCH, 0);
        call2(f_802145F0, p, 0x37D);                     /* 0x340 */
        break;
    case 1: case 2:
        if (MEM_R8(p + 5) == 1 && (int16_t)MEM_R16(p + 0x1CE) >= 9) {
            MEM_W8(p + 5, 2);
            call1(f_8028E178, p);                        /* the super: 50, the flash, the announcer ... */
            /* ... and 100 more: EX2 Plus spends the whole gauge, 100 here (80046E34) and 50 in its super start
             * (801EA79C), as every super does; with only 50 more she kept a level */
            int g = MEM_R8(p + 0x1B45) - 100;
            MEM_W8(p + 0x1B45, (uint8_t)(g < 0 ? 0 : g));
        }
        MEM_W16(p + 0x175E, MEM_R8(p + 5) == 2 ? 200 : 100);
        call1(f_80215338, p);
        if (MEM_R8(p + 5) == 2 && (MEM_R8(p + 0x1C9) & 1)) {
            MEM_W8(p + 5, 0xF0);
            MEM_W16(p + 0x175E, 100);
            call2(f_802145F0, p, 0x37E);                 /* 0x341: throwing it */
            uint32_t ob = object_slot(p, 0);
            if (!MEM_R8(ob)) {
                R(4) = ob; R(5) = p; R(6) = 0x19; R(7) = 0; f_8027CD98();
                her_voice(p, 19);
                MEM_W32(data + CATCH + 4, ob);
            }
        }
        break;
    case 0xF0: {
        call1(f_80215338, p);
        uint32_t ob = MEM_R32(data + CATCH + 4);
        if (MEM_R8(data + CATCH) && MEM_R8(opp + 0x1B44) != 0xFF) {
            MEM_W8(p + 0x1B47, 3);                       /* caught: her throw 3, the dance */
            her_voice(p, 6);
            MEM_W8(p + 5, 0);
            call2(f_80290784, p, 0x19);
            call2(f_80290784, opp, 0x1D);
        } else if (!ob || MEM_R8(ob + 4) >= 5 || !MEM_R8(ob) || MEM_R8(opp + 0x1B44) == 0xFF) {
            MEM_W8(p + 5, 0x0B);
            MEM_W8(data + CATCH, 0);
            R(4) = p; R(5) = 0x37F; R(6) = 0; f_8021483C();   /* 0x348: the recovery */
        }
        break;
    }
    default:
        call1(f_80215338, p);
        if (MEM_R8(p + 0x1C9) & 2) move_end(p);
        break;
    }
}

void o_pl00_1_800A0E8C_orig(void);
void o_pl00_2_800B0E8C_orig(void);
static void word2(void (*orig)(void)) {
    uint32_t p = R(4);
    if (!roster_is_extra(p) || MEM_R8(p + 2) != 0) { orig(); return; }
    /* Fighting Layer's shared moves, which every character's table sends to the main program: 16 (80295E6C, an
     * invulnerable roll), 17 the run (double-tap forward, 80296B7C) and 18 the back dash (8029708C). Kato's
     * dispatcher runs them; her table copy still has them (block + 0x1AC + 4 * id) */
    int id = MEM_R8(p + 0x1B47);
    if (id >= 16 && id <= 18) { orig(); return; }
    uint32_t s16 = R(16), s17 = R(17), s18 = R(18), sp = R(29), ra = R(31);
    /* a safety net: none of her specials lasts 10 seconds, so one that does is stuck: end it */
    static int running[2];
    int side = MEM_R16(p + 0x64) & 1;
    running[side] = MEM_R8(p + 5) == 0 ? 0 : running[side] + 1;
    if (running[side] > 600) {
        rt_log("pullum: special %d stuck at step %d, ended", MEM_R8(p + 0x1B47), MEM_R8(p + 5));
        running[side] = 0;
        move_end(p);
        R(16) = s16; R(17) = s17; R(18) = s18; R(29) = sp; R(31) = ra;
        return;
    }
    switch (MEM_R8(p + 0x1B47)) {
    case 0: sp_rising_kick(p); break;
    case 3: sp_flying_kick(p); break;
    case 4: sp_guard_break(p); break;
    case 1: sp_spinning_dive(p); break;
    case 5: sp_super_spin(p); break;
    case 6: sp_air_hop(p); break;
    case 7: sp_super_kicks(p); break;
    case 8: sp_flying_spin(p); break;
    case 9: sp_meteor(p); break;
    case 10: sp_air_super(p); break;
    default: move_end(p); break;               /* not one of hers (yet) */
    }
    /* he is knocked out (health 0xFF): her move's later hits must not land, or each one puts him back into a
     * standing hit reaction and he never collapses (the super kicks' first kick often knocks out). The hit test
     * skips a defender whose +0x1B4A (invulnerable frames) is set */
    uint32_t opp = MEM_R32(p + 0x58);
    if (MEM_R8(opp + 0x1B44) == 0xFF && MEM_R8(opp + 0x1B4A) < 2) MEM_W8(opp + 0x1B4A, 2);
    int sup = MEM_R8(p + 0x1B47);
    if (sup == 5 || sup == 7 || sup == 9 || sup == 10) ghost_on(p);   /* her supers have ghosts */
    R(16) = s16; R(17) = s17; R(18) = s18; R(29) = sp; R(31) = ra;
}
void o_pl00_1_800A0E8C(void) { word2(o_pl00_1_800A0E8C_orig); }
void o_pl00_2_800B0E8C(void) { word2(o_pl00_2_800B0E8C_orig); }

/* ---- word 3: her throws ----
 * Fighting Layer's main program picks a throw (forward/back + MP: 0, + MK: 1) and enters state 25; the thrower's
 * word 3 then runs both fighters: the opponent plays the thrower's "thrown" motion (func_802145A0), the thrower
 * steps both motions, deals the damage and ends the throw (Kato's 800A3450). Throw damage comes from the shared
 * record of motion 0 in her HIT file (4 attack blocks), as Kato's does; EX2 Plus has none for her throws. */
static void throw_damage(uint32_t p, int block, int knock_down) {
    uint32_t opp = MEM_R32(p + 0x58);
    R(4) = p; R(5) = 0; R(6) = (uint32_t)block; f_8021F234();
    uint32_t attr = R(2);
    if (knock_down && attr) {
        /* thrown off (EX2 Plus sends him flying): the throw's attributes with the knock-down reaction, launch and
         * knock-back of Kato's special (0x0E, 0xA3, 0x01), copied to her data area so the record stays as it is */
        uint32_t copy = MEM_R32(p + 0x1B58) + THROW_ATTR;
        for (uint32_t i = 0; i < 20; i++) MEM_W8(copy + i, MEM_R8(attr + i));
        MEM_W8(copy + 4, 0x0E);
        MEM_W8(copy + 8, 0xA3);
        MEM_W8(copy + 10, 0x01);
        attr = copy;
    }
    MEM_W32(opp + 0x1950, attr);
    MEM_W32(opp + 0x5C, p);
    call1(f_8027DBFC, p);
    MEM_W16(opp + 0x19C0, MEM_R16(p + 0x250));           /* where it lands: her root node */
    MEM_W16(opp + 0x19C2, MEM_R16(p + 0x254));
    MEM_W16(opp + 0x19C4, MEM_R16(p + 0x258));
    call1(f_8028CBAC, opp);
}

/* After throw damage: if it knocked him out (health 0xFF), his KO reaction, as a normal hit's (func_8027EB10 ->
 * func_8027F3EC(defender, attributes); throw damage leaves 0, a hit 0xFF); the caller must then leave his state alone, or he never collapses and the
 * round does not end. 1: knocked out. */
void f_8027F3EC(void);
static int throw_ko(uint32_t p) {
    uint32_t opp = MEM_R32(p + 0x58);
    uint8_t hp = MEM_R8(opp + 0x1B44);
    if (hp != 0 && hp != 0xFF) return 0;
    MEM_W8(opp + 0x1B44, 0xFF);                          /* throw damage stops at 0: knocked out all the same */
    R(4) = opp; R(5) = MEM_R32(opp + 0x1950); f_8027F3EC();
    return 1;
}

static void step_both(uint32_t p, uint32_t opp) {
    call1(f_80215338, p);
    MEM_W8(opp + 0x1C9, (uint8_t)(MEM_R8(opp + 0x1C9) | 8));   /* he plays from her motion file */
    call1(f_80215338, opp);
}

/* where the body is now: the root node's world x, z */
static void to_body(uint32_t q) {
    MEM_W16(q + 0x12, MEM_R16(q + 0x250));
    MEM_W16(q + 0x1A, MEM_R16(q + 0x258));
}

static void throw_start(uint32_t p, uint16_t her, uint16_t his) {
    MEM_W8(p + 5, 1);
    call2(f_8028DBD0, p, 0xFFFFFFFFu);
    MEM_W32(p + 0x1AC8, 0xFFFFFFFFu);
    call2(f_80214568, p, her);
    call2(f_802145A0, MEM_R32(p + 0x58), his);
}

/* forward/back + MP (EX2 Plus 800472B8): at frame 55 she throws him off */
static void th_punch(uint32_t p) {
    uint32_t opp = MEM_R32(p + 0x58);
    switch (MEM_R8(p + 5)) {
    case 0: throw_start(p, 0x376, 0x377); break;         /* 0x311 / 0x312 */
    case 1:
        step_both(p, opp);
        if ((int16_t)MEM_R16(p + 0x1CE) >= 55) {
            MEM_W8(p + 5, 2);
            her_voice(p, 10);
            throw_damage(p, 0, 1);
            if (throw_ko(p)) break;
            call2(f_80290784, opp, 0x1C);              /* the damage state throws him off ... */
            /* ... over her: EX2 Plus launches him up and forwards, past her, to land on her other side
             * (800473F8: up 0x1C0000, gravity 0x24000, about 25 frames in the air). Fighting Layer's knock-down
             * sends him away from her, low: his speeds are set instead. +0x2C up (16.16, y -= it), +0x28 / +0x30
             * along x / z (x -= it), set once and slowed down on the ground after: towards her, to land about
             * 150 past her */
            {
                int dx = (int16_t)MEM_R16(p + 0x250) - (int16_t)MEM_R16(opp + 0x250);
                int dz = (int16_t)MEM_R16(p + 0x258) - (int16_t)MEM_R16(opp + 0x258);
                double len = sqrt((double)dx * dx + (double)dz * dz);
                if (len < 1) len = 1;
                double speed = (len + 150) / 25 * 65536;  /* units a frame, 16.16 */
                MEM_W32(opp + 0x2C, 0x1C0000);
                MEM_W32(opp + 0x38, (uint32_t)-0x24000);
                MEM_W32(opp + 0x28, (uint32_t)(int32_t)(-speed * dx / len));
                MEM_W32(opp + 0x30, (uint32_t)(int32_t)(-speed * dz / len));
            }
        }
        break;
    default:
        call1(f_80215338, p);
        if (MEM_R8(p + 0x1C9) & 2) { to_body(p); move_end(p); }
        break;
    }
}

/* forward/back + MK (EX2 Plus 800474FC): damage on his frame 76, then he lies where he landed */
static void th_kick(uint32_t p) {
    uint32_t opp = MEM_R32(p + 0x58);
    switch (MEM_R8(p + 5)) {
    case 0: throw_start(p, 0x378, 0x379); break;         /* 0x313 / 0x314 */
    case 1:
        step_both(p, opp);
        if ((int16_t)MEM_R16(p + 0x1CE) == 20) her_voice(p, 7);
        if ((int16_t)MEM_R16(p + 0x1CE) == 54) her_voice(p, 8);
        if ((int16_t)MEM_R16(opp + 0x1CE) == 76) {
            throw_damage(p, 1, 0);
            R(4) = 8; R(5) = 10; f_8022A34C();
        }
        if (MEM_R8(opp + 0x1C9) & 1) {
            MEM_W8(p + 5, 2);
            uint16_t facing = (uint16_t)((MEM_R16(p + 0x52) + 0xD00) & 0xFFF);
            MEM_W16(opp + 0x4A, facing);
            MEM_W16(opp + 0x52, facing);
            to_body(opp);
            MEM_W8(opp + 0x1B47, 0);
            MEM_W8(opp + 0x1B43, 2);
            MEM_W8(opp + 0x1B55, 0);
            MEM_W8(opp + 0x1B48, 0x15);
            call2(f_80290784, opp, 0x1A);
        }
        break;
    case 2:
        call1(f_80215338, p);
        if (MEM_R8(p + 0x1C9) & 1) MEM_W8(p + 5, 3);
        break;
    default: {
        to_body(p);
        uint16_t facing = (uint16_t)((MEM_R16(p + 0x4A) - 0x140) & 0xFFF);
        MEM_W16(p + 0x4A, facing);
        MEM_W16(p + 0x52, facing);
        move_end(p);
        break;
    }
    }
}

/* the air throw (EX2 Plus 8004772C): she holds him, he turns over her (the spin functions, on him) playing her
 * 0x32C, and is thrown down when it ends; she falls from frame 29 */
static void th_air(uint32_t p) {
    uint32_t opp = MEM_R32(p + 0x58), t = MEM_R32(p + 0x1B58) + AIR_THROW;
    uint32_t saved = t, pivot = t + 8, vel = t + 16, his = t + 24;
    if (MEM_R8(p + 5) == 0) MEM_W8(his, 0);
    switch (MEM_R8(his)) {
    case 0: {
        MEM_W8(his, 1);
        her_voice(p, 11);
        call1(f_80289104, opp);
        uint16_t facing = MEM_R16(p + 0x4A);
        MEM_W16(opp + 0x4A, facing);
        MEM_W16(opp + 0x52, facing);
        for (int i = 0; i < 3; i++) { MEM_W16(pivot + 2u * (uint32_t)i, 0); MEM_W16(vel + 2u * (uint32_t)i, 0); }
        call2(f_8028ADFC, opp, saved);
        call2(f_802145A0, opp, 0x37B);                   /* 0x32C */
        for (uint32_t i = 0; i < 12; i += 4) MEM_W32(opp + 0x10 + i, MEM_R32(p + 0x10 + i));   /* in her hands */
    }   /* fall through */
    case 1:
        MEM_W8(opp + 0x1C9, (uint8_t)(MEM_R8(opp + 0x1C9) | 8));
        R(4) = opp; R(5) = pivot; R(6) = vel; R(7) = saved; f_8028AEF0();
        MEM_W16(vel + 2, (uint16_t)-0x92);
        if (MEM_R8(opp + 0x1C9) & 1) {
            MEM_W8(his, 2);
            call2(f_8028B174, opp, saved);
            to_body(opp);
            uint16_t facing = (uint16_t)((MEM_R16(p + 0x4A) + 0x800) & 0xFFF);
            MEM_W16(opp + 0x4A, facing);
            MEM_W16(opp + 0x52, facing);
            throw_damage(p, 2, 0);
            if (!throw_ko(p)) call2(f_80290784, opp, 0x1C);
        }
        break;
    default:
        break;
    }
    switch (MEM_R8(p + 5)) {
    case 0:
        MEM_W8(p + 5, 1);
        call1(f_80289104, p);
        call2(f_802145F0, p, 0x37A);                     /* 0x320 */
        /* fall through */
    case 1:
        call1(f_80215338, p);
        if ((int16_t)MEM_R16(p + 0x1CE) >= 29) {
            MEM_W32(p + 0x2C, 0);
            MEM_W32(p + 0x38, (uint32_t)-0x18000);
            MEM_W8(p + 5, 2);
        }
        break;
    default:
        call1(f_80289164, p);
        call1(f_80215338, p);
        if (landed(p)) call2(f_80290784, p, 0x0C);
        break;
    }
}

/* the Meteor Combo's dance (throw 3, EX2 Plus 800479A0): he faces her and both play her three dance motions,
 * each ending in a hit, faster each time (to 400%); then her finisher knocks him down. Not done yet: his fall when
 * caught in the air, his stagger before the dance, the music change and random cues. */
static void th_meteor(uint32_t p) {
    static const uint16_t dance[3] = {0x36A, 0x36B, 0x36C};   /* 0x346, 0x344, 0x345 */
    static const int16_t frames[3] = {50, 10, 50};       /* their lengths (two loop, one holds: go on by frame) */
    uint32_t opp = MEM_R32(p + 0x58), m = MEM_R32(p + 0x1B58) + METEOR, data = MEM_R32(p + 0x1B58);
    int step = MEM_R8(p + 5);
    if (step == 0) {
        MEM_W8(p + 5, 1);
        MEM_W8(m, 0);                                    /* dance moves so far */
        MEM_W16(m + 2, 100);                             /* speed */
        uint16_t facing = (uint16_t)((MEM_R16(p + 0x4A) + 0x800) & 0xFFF);
        MEM_W16(opp + 0x4A, facing);
        MEM_W16(opp + 0x52, facing);
        call1(f_80289104, opp);
        MEM_W32(opp + 0x14, 0);
        MEM_W8(opp + 0x1B43, 0);
        call2(f_80214568, p, dance[0]);
        call2(f_802145A0, opp, dance[0]);
        her_voice(p, 20);
        return;
    }
    if (step == 1) {                                     /* dancing */
        uint16_t speed = MEM_R16(m + 2);
        MEM_W16(p + 0x1D2, speed);                       /* animation speed (EX2 Plus +0x1DE), both */
        MEM_W16(opp + 0x1D2, speed);
        step_both(p, opp);
        if ((MEM_R8(p + 0x1C9) & 1) || (int16_t)MEM_R16(p + 0x1CE) >= frames[MEM_R8(m) % 3] - 1) {
            int n = MEM_R8(m) + 1;
            MEM_W8(m, (uint8_t)n);
            throw_damage(p, 0, 0);                       /* the light throw block: six of these, then the finisher */
            int ko = throw_ko(p);                        /* knocked out in the dance: he collapses, she finishes */
            MEM_W16(m + 2, (uint16_t)(speed + 60 > 400 ? 400 : speed + 60));
            if (n >= 6 || ko) {                          /* the finisher */
                MEM_W8(p + 5, 2);
                MEM_W16(p + 0x1D2, 100);
                MEM_W16(opp + 0x1D2, 100);
                call2(f_80214568, p, 0x36D);             /* 0x34C */
                if (!ko) {
                    throw_damage(p, 3, 1);
                    if (!throw_ko(p)) call2(f_80290784, opp, 0x1C);
                }
                MEM_W8(data + CATCH, 0);
            } else {
                call2(f_80214568, p, dance[n % 3]);
                call2(f_802145A0, opp, dance[n % 3]);
            }
        }
        return;
    }
    call1(f_80215338, p);
    if (MEM_R8(p + 0x1C9) & 2) { to_body(p); move_end(p); }
}

void o_pl00_1_800A3414_orig(void);
void o_pl00_2_800B3414_orig(void);
static void word3(void (*orig)(void)) {
    uint32_t p = R(4);
    int id = MEM_R8(p + 0x1B47);
    if (!roster_is_extra(p) || MEM_R8(p + 2) != 0 || id > 3) { orig(); return; }
    uint32_t s16 = R(16), s17 = R(17), s18 = R(18), sp = R(29), ra = R(31);
    if (id == 0) th_punch(p); else if (id == 1) th_kick(p); else if (id == 2) th_air(p); else { th_meteor(p); ghost_on(p); }
    R(16) = s16; R(17) = s17; R(18) = s18; R(29) = sp; R(31) = ra;
}
void o_pl00_1_800A3414(void) { word3(o_pl00_1_800A3414_orig); }
void o_pl00_2_800B3414(void) { word3(o_pl00_2_800B3414_orig); }

/* Fighting Layer plays a character's voice with func_802A0CB8(side, id) / func_802A0D8C(side, id, level): on her
 * side the engine still asks for Kato's (0x336-0x343: damage 0x343, KO 0x336, his win-pose lines 0x338 / 0x33A
 * from his word 4): those become hers, the rest of his are dropped. */
void f_802A0CB8_orig(void);
void f_802A0D8C_orig(void);
static int her_voice_for(int side, int id) {
    static uint32_t seed = 1;
    uint32_t pl = side ? 0x8034BC7Cu : 0x8034A100u;
    if (!roster_is_extra(pl) || MEM_R8(pl + 2) != 0 || id < 0x336 || id > 0x343) return 0;
    seed = seed * 1103515245u + 12345u;                  /* not the game's random numbers: they stay as they were */
    int v = id == 0x343 ? V_DAMAGE + (int)((seed >> 16) % 3) : id == 0x336 ? V_KO
          : id == 0x338 ? V_WIN + 3 : id == 0x33A ? V_WIN : -1;
    if (v >= 0) her_voice_side(side, v);
    return 1;
}
void f_802A0CB8(void) { if (!her_voice_for((int16_t)R(4) & 1, (int16_t)R(5))) f_802A0CB8_orig(); }
void f_802A0D8C(void) { if (!her_voice_for((int16_t)R(4) & 1, (int16_t)R(5))) f_802A0D8C_orig(); }

/* ---- word 4: her win poses ----
 * Block word 4 runs in the neutral state while the game is in its round-won flow (main 80280358): the win pose.
 * Kato's (800A3E4C) picks one of two at random; hers (EX2 Plus 80048618) one of four, each a motion that chains
 * into a held or looping one, with a line from her win-pose voice bank. (EX2 Plus has a fifth against one
 * opponent, char 0x17, which Fighting Layer does not have.) */
void f_802C52AC(void);   /* rand */
void f_8022A400(void);
void o_pl00_1_800A3E4C_orig(void);
void o_pl00_2_800B3E4C_orig(void);
static void her_win(uint32_t p) {
    static const uint16_t pose[4] = {0x301, 0x303, 0x305, 0x307};   /* 0x319, 0x317, 0x31B, 0x34F */
    static const uint8_t line[4][8] = {{0, 1, 2, 3, 4, 3, 1, 8}, {3, 3, 3, 3, 3, 3, 3, 3},
                                       {3, 3, 3, 3, 3, 3, 3, 3}, {6, 6, 6, 6, 6, 6, 6, 6}};
    if (MEM_R8(p + 5) == 0) {
        MEM_W8(p + 5, 1);
        MEM_W8(p + 6, 0);
        MEM_W8(p + 0x1B43, 0);
        call1(f_80289104, p);
        MEM_W32(p + 0x19CC, 0);
        MEM_W32(p + 0x1AC8, 0xFFFFFFFFu);
        f_802C52AC();
        uint32_t v = R(2) & 3;
        MEM_W32(p + 0x1A8C, v);
        call2(f_8022A400, p, 0x34);
        MEM_W8(p + 0x1C8, MEM_R8(p + 0x1C8) & 0xFE);
        call2(f_80214568, p, pose[v]);
        return;
    }
    call1(f_80215338, p);
    if (MEM_R8(p + 6) == 0 && (int16_t)MEM_R16(p + 0x1CE) >= 20) {   /* her line */
        MEM_W8(p + 6, 1);
        f_802C52AC();
        her_voice(p, V_WIN + line[MEM_R32(p + 0x1A8C) & 3][R(2) & 7]);
    }
}
static void word4(void (*orig)(void)) {
    uint32_t p = R(4);
    if (!roster_is_extra(p) || MEM_R8(p + 2) != 0) { orig(); return; }
    uint32_t s16 = R(16), s17 = R(17), sp = R(29), ra = R(31);
    her_win(p);
    R(16) = s16; R(17) = s17; R(29) = sp; R(31) = ra;
}
void o_pl00_1_800A3E4C(void) { word4(o_pl00_1_800A3E4C_orig); }
void o_pl00_2_800B3E4C(void) { word4(o_pl00_2_800B3E4C_orig); }
