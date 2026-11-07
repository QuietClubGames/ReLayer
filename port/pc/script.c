/*
 * Scripted inputs and screenshots, for testing without a person at the keyboard.
 *
 * Script file, one command per line ('#' starts a comment):
 *   <frame> <hold-frames> <input>     press an input, e.g.  "600 4 coin1", "700 4 p1.start", "900 30 p1.right"
 *   <frame> shot <name>               save the screen as <name>.bmp
 *   <frame> set <setting>=<value>     change an arcade setting, e.g. "900 set free_play=1" (see game.h)
 *   <frame> poke <hex address>=<value> write a byte of game RAM (testing), e.g. "1990 poke 80335435=17"
 *   <frame> vram <name>               save VRAM (1024x1024 16-bit) and main RAM, raw, as <name>.vram / .ram
 * Inputs: coin1 coin2 test service, p1./p2. + up down left right b1..b6 start
 */
#include "script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { long frame, hold; int kind; int player; uint16_t bit; char name[64]; } Cmd;
enum { K_PLAYER, K_COIN1, K_COIN2, K_TEST, K_SERVICE, K_SHOT, K_SET };

static Cmd *cmds;
static int ncmds;

static int parse_input(const char *s, Cmd *c) {
    static const struct { const char *n; uint16_t bit; } pb[] = {
        {"up", 0x20}, {"down", 0x10}, {"left", 0x08}, {"right", 0x04}, {"b1", 0x02}, {"b2", 0x01},
        {"b3", 0x8000}, {"b4", 0x4000}, {"b5", 0x2000}, {"b6", 0x1000}, {"start", 0x80},
    };
    if (!strcmp(s, "coin1")) { c->kind = K_COIN1; return 1; }
    if (!strcmp(s, "coin2")) { c->kind = K_COIN2; return 1; }
    if (!strcmp(s, "test")) { c->kind = K_TEST; return 1; }
    if (!strcmp(s, "service")) { c->kind = K_SERVICE; return 1; }
    if ((s[0] == 'p' || s[0] == 'P') && (s[1] == '1' || s[1] == '2') && s[2] == '.') {
        for (unsigned i = 0; i < sizeof pb / sizeof pb[0]; i++)
            if (!strcmp(s + 3, pb[i].n)) { c->kind = K_PLAYER; c->player = s[1] - '1'; c->bit = pb[i].bit; return 1; }
    }
    return 0;
}

int script_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open script %s\n", path); return 0; }
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char a[64], b[64], c[64];
        int n = sscanf(line, "%63s %63s %63s", a, b, c);
        if (n <= 0) continue;
        if (n < 3) { fprintf(stderr, "%s:%d: expected 3 fields\n", path, lineno); continue; }
        Cmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.frame = atol(a);
        if (!strcmp(b, "shot")) { cmd.kind = K_SHOT; snprintf(cmd.name, sizeof cmd.name, "%s", c); }
        else if (!strcmp(b, "set")) { cmd.kind = K_SET; snprintf(cmd.name, sizeof cmd.name, "%s", c); }
        else if (!strcmp(b, "poke")) { cmd.kind = K_SET; snprintf(cmd.name, sizeof cmd.name, "@%s", c); }
        else if (!strcmp(b, "vram")) { cmd.kind = K_SET; snprintf(cmd.name, sizeof cmd.name, "!%s", c); }
        else {
            cmd.hold = atol(b);
            if (!parse_input(c, &cmd)) { fprintf(stderr, "%s:%d: unknown input '%s'\n", path, lineno, c); continue; }
        }
        cmds = realloc(cmds, sizeof(Cmd) * (size_t)(ncmds + 1));
        cmds[ncmds++] = cmd;
    }
    fclose(f);
    return 1;
}

void script_inputs(long frame, S12Inputs *in) {
    for (int i = 0; i < ncmds; i++) {
        Cmd *c = &cmds[i];
        if (c->kind == K_SHOT || c->kind == K_SET || frame < c->frame || frame >= c->frame + c->hold) continue;
        switch (c->kind) {
        case K_PLAYER: in->player[c->player] |= c->bit; break;
        case K_COIN1: in->coin[0] = 1; break;
        case K_COIN2: in->coin[1] = 1; break;
        case K_TEST: in->test = 1; break;
        case K_SERVICE: in->service_dip = 1; break;
        }
    }
}

const char *script_set(long frame, int index) {
    for (int i = 0; i < ncmds; i++)
        if (cmds[i].kind == K_SET && cmds[i].frame == frame && index-- == 0) return cmds[i].name;
    return NULL;
}

const char *script_shot(long frame) {
    for (int i = 0; i < ncmds; i++)
        if (cmds[i].kind == K_SHOT && cmds[i].frame == frame) return cmds[i].name;
    return NULL;
}
