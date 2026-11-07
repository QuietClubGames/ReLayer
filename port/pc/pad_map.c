/*
 * Controller -> cabinet mapping. Platform independent: the Dreamcast port can reuse it with its own
 * controller reader.
 *
 * Default layout (the usual six-button fighting game layout on an Xbox pad):
 *   cabinet top row    1 2 3  ->  X  Y  RB
 *   cabinet bottom row 4 5 6  ->  A  B  RT
 *   joystick -> D-pad and left stick (8-way), start -> Start, coin -> Back/View
 */
#include "pad.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const struct { const char *name; int src; } k_names[] = {
    {"none", SRC_NONE}, {"dup", SRC_DUP}, {"ddown", SRC_DDOWN}, {"dleft", SRC_DLEFT}, {"dright", SRC_DRIGHT},
    {"start", SRC_START}, {"back", SRC_BACK}, {"view", SRC_BACK}, {"menu", SRC_START},
    {"ls", SRC_LS}, {"rs", SRC_RS}, {"lb", SRC_LB}, {"rb", SRC_RB},
    {"a", SRC_A}, {"b", SRC_B}, {"x", SRC_X}, {"y", SRC_Y}, {"lt", SRC_LT}, {"rt", SRC_RT},
};

static const char *k_targets[MAP_COUNT] = {"b1", "b2", "b3", "b4", "b5", "b6", "start", "coin"};

/* XInput button bits for each digital source */
static const uint16_t k_xbits[SRC_COUNT] = {
    [SRC_DUP] = 0x0001, [SRC_DDOWN] = 0x0002, [SRC_DLEFT] = 0x0004, [SRC_DRIGHT] = 0x0008,
    [SRC_START] = 0x0010, [SRC_BACK] = 0x0020, [SRC_LS] = 0x0040, [SRC_RS] = 0x0080,
    [SRC_LB] = 0x0100, [SRC_RB] = 0x0200, [SRC_A] = 0x1000, [SRC_B] = 0x2000, [SRC_X] = 0x4000, [SRC_Y] = 0x8000,
};

/* cabinet bits for MAP_* targets */
static const uint16_t k_cab[MAP_COUNT] = {0x02, 0x01, 0x8000, 0x4000, 0x2000, 0x1000, 0x80, 0};

void padmap_defaults(PadMap *m) {
    memset(m, 0, sizeof *m);
    m->src[MAP_B1][0] = SRC_X;
    m->src[MAP_B2][0] = SRC_Y;
    m->src[MAP_B3][0] = SRC_RB;
    m->src[MAP_B4][0] = SRC_A;
    m->src[MAP_B5][0] = SRC_B;
    m->src[MAP_B6][0] = SRC_RT;
    m->src[MAP_START][0] = SRC_START;
    m->src[MAP_COIN][0] = SRC_BACK;
    m->stick_deadzone = 0.35f;
}

int padmap_source_from_name(const char *name) {
    char low[16];
    size_t i = 0;
    for (; name[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;
    for (unsigned k = 0; k < sizeof k_names / sizeof k_names[0]; k++)
        if (!strcmp(low, k_names[k].name)) return k_names[k].src;
    return -1;
}

const char *padmap_source_name(int src) {
    for (unsigned k = 0; k < sizeof k_names / sizeof k_names[0]; k++)
        if (k_names[k].src == src) return k_names[k].name;
    return "?";
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* File format:  target = SOURCE [SOURCE]     e.g.  "b1 = X",  "coin = BACK LB",  "stick_deadzone = 0.35" */
int padmap_load(PadMap *m, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line), *val = trim(eq + 1);
        if (!strcmp(key, "stick_deadzone")) {
            float dz = 0;
            if (sscanf(val, "%f", &dz) == 1 && dz > 0 && dz < 1) m->stick_deadzone = dz;
            continue;
        }
        int t = -1;
        for (int k = 0; k < MAP_COUNT; k++) if (!strcmp(key, k_targets[k])) t = k;
        if (t < 0) { fprintf(stderr, "%s:%d: unknown input '%s'\n", path, lineno, key); continue; }
        m->src[t][0] = m->src[t][1] = SRC_NONE;
        int n = 0;
        for (char *tok = strtok(val, " \t,"); tok && n < 2; tok = strtok(NULL, " \t,")) {
            int s = padmap_source_from_name(tok);
            if (s < 0) { fprintf(stderr, "%s:%d: unknown controller button '%s'\n", path, lineno, tok); continue; }
            m->src[t][n++] = (uint8_t)s;
        }
    }
    fclose(f);
    return 1;
}

static int src_held(int src, const PadRaw *p) {
    if (src == SRC_NONE) return 0;
    if (src == SRC_LT) return p->lt > 30;
    if (src == SRC_RT) return p->rt > 30;
    return (p->buttons & k_xbits[src]) != 0;
}

uint16_t padmap_player_bits(const PadMap *m, const PadRaw *p, int *coin) {
    uint16_t bits = 0;
    if (coin) *coin = 0;
    if (!p->connected) return 0;
    /* digital pad */
    if (p->buttons & 0x0001) bits |= 0x20;
    if (p->buttons & 0x0002) bits |= 0x10;
    if (p->buttons & 0x0004) bits |= 0x08;
    if (p->buttons & 0x0008) bits |= 0x04;
    /* left stick: 8 sectors of 45 degrees, so diagonals are as easy as straight directions */
    float x = p->lx / 32767.0f, y = p->ly / 32767.0f;
    if (sqrtf(x * x + y * y) > m->stick_deadzone) {
        float a = atan2f(y, x) * (180.0f / 3.14159265f);   /* 0 = right, 90 = up */
        int sector = (int)floorf((a + 22.5f) / 45.0f);
        sector = ((sector % 8) + 8) % 8;
        static const uint16_t dir[8] = {0x04, 0x24, 0x20, 0x28, 0x08, 0x18, 0x10, 0x14};
        bits |= dir[sector];
    }
    for (int t = 0; t < MAP_COUNT; t++) {
        int held = src_held(m->src[t][0], p) || src_held(m->src[t][1], p);
        if (!held) continue;
        if (t == MAP_COIN) { if (coin) *coin = 1; }
        else bits |= k_cab[t];
    }
    /* opposite directions cancel (a worn stick or D-pad + stick fighting each other) */
    if ((bits & 0x30) == 0x30) bits &= (uint16_t)~0x30;
    if ((bits & 0x0C) == 0x0C) bits &= (uint16_t)~0x0C;
    return bits;
}

/* Friendly names for on-screen display */
const char *padmap_display_name(int src) {
    static const char *n[SRC_COUNT] = {
        [SRC_NONE] = "—", [SRC_DUP] = "D-UP", [SRC_DDOWN] = "D-DOWN", [SRC_DLEFT] = "D-LEFT",
        [SRC_DRIGHT] = "D-RIGHT", [SRC_START] = "MENU", [SRC_BACK] = "VIEW", [SRC_LS] = "LS", [SRC_RS] = "RS",
        [SRC_LB] = "LB", [SRC_RB] = "RB", [SRC_A] = "A", [SRC_B] = "B", [SRC_X] = "X", [SRC_Y] = "Y",
        [SRC_LT] = "LT", [SRC_RT] = "RT",
    };
    return (src >= 0 && src < SRC_COUNT && n[src]) ? n[src] : "?";
}

/* Bitmask of physical inputs held on one pad (bit = SRC_*), for menus and remapping */
uint32_t padmap_held_sources(const PadRaw *p) {
    uint32_t m = 0;
    if (!p->connected) return 0;
    for (int s = 1; s < SRC_COUNT; s++) if (src_held(s, p)) m |= 1u << s;
    return m;
}

int padmap_save(const PadMap *m, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# Xbox controller mapping for Fighting Layer (also editable in the in-game menu: Controls).\n"
               "# Cabinet buttons: top row 1 2 3, bottom row 4 5 6.\n"
               "# Names: A B X Y LB RB LT RT START BACK LS RS DUP DDOWN DLEFT DRIGHT NONE\n\n");
    for (int t = 0; t < MAP_COUNT; t++) {
        fprintf(f, "%s = %s", k_targets[t], padmap_source_name(m->src[t][0]));
        if (m->src[t][1]) fprintf(f, " %s", padmap_source_name(m->src[t][1]));
        fprintf(f, "\n");
    }
    fprintf(f, "\nstick_deadzone = %.2f\n", m->stick_deadzone);
    fclose(f);
    return 1;
}
