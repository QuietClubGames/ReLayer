/*
 * Xbox controllers (360 / One / Series, wired or wireless) through XInput.
 * The XInput DLL is loaded at run time, so the game still starts on systems without it.
 * Controller 1 -> player 1, controller 2 -> player 2. Plugging in or out works while running.
 */
#include "pad.h"
#include "../core/log.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XPad;
typedef struct { DWORD dwPacketNumber; XPad Gamepad; } XState;
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XState *);

static XInputGetStateFn g_get;
static PadMap g_map;
static PadRaw g_slot[4];        /* XInput has four slots */
static int g_retry[4];          /* frames until we look at an empty slot again (polling those is slow) */
static uint32_t g_last_sig[4];  /* last logged button state per slot */
static char g_map_path[260] = "controls.ini";

void pad_init(const char *config_path) {
    static const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (unsigned i = 0; i < sizeof dlls / sizeof dlls[0] && !g_get; i++) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) {
            g_get = (XInputGetStateFn)(void *)GetProcAddress(h, "XInputGetState");
            if (g_get) rt_log("pad: using %s", dlls[i]);
        }
    }
    if (!g_get) rt_log("pad: XInput not available, controllers disabled");
    padmap_defaults(&g_map);
    if (config_path) snprintf(g_map_path, sizeof g_map_path, "%s", config_path);
    if (config_path && padmap_load(&g_map, config_path)) rt_log("pad: mapping loaded from %s", config_path);
    char buf[256];
    int o = 0;
    static const char *t[MAP_COUNT] = {"b1", "b2", "b3", "b4", "b5", "b6", "start", "coin"};
    for (int k = 0; k < MAP_COUNT; k++)
        o += snprintf(buf + o, sizeof buf - (size_t)o, " %s=%s", t[k], padmap_source_name(g_map.src[k][0]));
    rt_log("pad: mapping%s", buf);
}

/* Test hook: FGT_FAKE_PAD="frame:buttons:hold,..." replaces slot 0 with scripted XInput button states
 * (hex XInput bits, e.g. 0020 = Back, 0010 = Start). Exercises the same code path as a real pad. */
#include "../core/hw.h"
#include <stdlib.h>
static int fake_pad(void) {
    static int checked, on;
    static char spec[512];
    if (!checked) {
        checked = 1;
        const char *e = getenv("FGT_FAKE_PAD");
        if (e) { on = 1; snprintf(spec, sizeof spec, "%s", e); rt_log("pad: fake controller in slot 0: %s", spec); }
    }
    if (!on) return 0;
    memset(&g_slot[0], 0, sizeof g_slot[0]);
    g_slot[0].connected = 1;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", spec);
    for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
        long f = 0, hold = 1; unsigned b = 0;
        if (sscanf(tok, "%ld:%x:%ld", &f, &b, &hold) >= 2 && (long)g_frame >= f && (long)g_frame < f + hold)
            g_slot[0].buttons |= (uint16_t)b;
    }
    return 1;
}

void pad_poll(void) {
    if (fake_pad()) return;
    if (!g_get) return;
    for (int i = 0; i < 4; i++) {
        if (!g_slot[i].connected && g_retry[i] > 0) { g_retry[i]--; continue; }
        XState st;
        memset(&st, 0, sizeof st);
        int was = g_slot[i].connected;
        if (g_get((DWORD)i, &st) == ERROR_SUCCESS) {
            g_slot[i].connected = 1;
            g_slot[i].buttons = st.Gamepad.wButtons;
            g_slot[i].lt = st.Gamepad.bLeftTrigger;
            g_slot[i].rt = st.Gamepad.bRightTrigger;
            g_slot[i].lx = st.Gamepad.sThumbLX;
            g_slot[i].ly = st.Gamepad.sThumbLY;
        } else {
            memset(&g_slot[i], 0, sizeof g_slot[i]);
            g_retry[i] = 120;     /* look again in about two seconds */
        }
        if (was != g_slot[i].connected)
            rt_log("pad: XInput slot %d %s", i, g_slot[i].connected ? "connected" : "disconnected");
        /* diagnostics: log button / trigger / stick-direction changes */
        if (g_slot[i].connected) {
            int dummy;
            uint16_t cab = padmap_player_bits(&g_map, &g_slot[i], &dummy);
            uint32_t sig = (uint32_t)g_slot[i].buttons | ((uint32_t)(g_slot[i].lt > 30) << 16) |
                           ((uint32_t)(g_slot[i].rt > 30) << 17) | ((uint32_t)dummy << 18) | ((uint32_t)cab << 19);
            if (sig != g_last_sig[i]) {
                rt_log("pad: slot %d buttons=%04X lt=%u rt=%u stick=(%d,%d) -> cabinet bits %04X%s",
                       i, g_slot[i].buttons, g_slot[i].lt, g_slot[i].rt, g_slot[i].lx, g_slot[i].ly, cab,
                       dummy ? " +COIN" : "");
                g_last_sig[i] = sig;
            }
        }
    }
}

/* The first connected controller is player 1 and the second is player 2, whatever XInput slots they use
 * (a virtual pad from Steam Input or similar can sit in slot 0). */
void pad_apply(S12Inputs *in) {
    int player = 0;
    for (int i = 0; i < 4 && player < 2; i++) {
        if (!g_slot[i].connected) continue;
        int coin = 0;
        in->player[player] |= padmap_player_bits(&g_map, &g_slot[i], &coin);
        if (coin) in->coin[player] = 1;
        player++;
    }
}

PadMap *pad_map(void) { return &g_map; }
void pad_save_map(void) { padmap_save(&g_map, g_map_path); }

int pad_player_connected(int player) {
    int n = 0;
    for (int i = 0; i < 4; i++) if (g_slot[i].connected) { if (n == player) return 1; n++; }
    return 0;
}

uint32_t pad_held_all(void) {
    uint32_t m = 0;
    for (int i = 0; i < 4; i++) m |= padmap_held_sources(&g_slot[i]);
    return m;
}

int pad_stick_dir(void) {
    int d = 0;
    for (int i = 0; i < 4; i++) {
        if (!g_slot[i].connected) continue;
        if (g_slot[i].ly > 20000) d |= 1;
        if (g_slot[i].ly < -20000) d |= 2;
        if (g_slot[i].lx < -20000) d |= 4;
        if (g_slot[i].lx > 20000) d |= 8;
    }
    return d;
}
