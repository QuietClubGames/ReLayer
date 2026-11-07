/*
 * Windows side of the overlay menu: turns keyboard and controller input into menu actions, implements the
 * services the menu needs (settings, remapping, restart...), and applies settings to the renderer and audio.
 */
#include "menu_host.h"
#include "../ui/ui.h"
#include "../ui/settings.h"
#include "pad.h"
#include "video.h"
#include "../core/log.h"
#include "d3d11_present.h"
#include "game.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

extern int g_hd_enabled, g_hd_scale_override, g_hd_affine, g_hd_scale_cap;
float hd_d3d_gpu_ms(void);
int hd_d3d_scale(void);
extern float g_blit_dim, g_blit_blur, g_blit_scan;
extern int g_blit_smooth;
int g_volume = 100;            /* 0-100, mute folded in */
int g_test_pulse;              /* frames left of a simulated operator test-button press */

static int g_capture = -1;
static float g_capture_t;

/* ---------------------------------------------------------------- settings */
static int g_applied_display = -1, g_applied_size = -1, g_applied_monitor = -1, g_applied_quality = -1;

static void apply(int save) {
    if (g_settings.display_mode != g_applied_display || g_settings.window_size != g_applied_size ||
        g_settings.monitor != g_applied_monitor) {
        video_apply_display(g_settings.display_mode, g_settings.window_size, g_settings.monitor);
        g_applied_display = g_settings.display_mode;
        g_applied_size = g_settings.window_size;
        g_applied_monitor = g_settings.monitor;
    }
    if (g_settings.quality != g_applied_quality) { g_hd_scale_cap = 8; g_applied_quality = g_settings.quality; }
    g_aspect_mode = g_settings.aspect;
    g_unlock_all = g_settings.unlock_all;
    g_hd_enabled = g_settings.enhanced;
    g_hd_scale_override = settings_quality_scale(g_settings.quality);
    g_hd_affine = !g_settings.perspective;
    g_blit_smooth = g_settings.filter == 1;
    g_blit_scan = g_settings.filter == 2 ? 1.0f : 0.0f;
    g_volume = g_settings.mute ? 0 : g_settings.volume;
    if (save) settings_save(&g_settings, "settings.ini");
    /* the log notes every change of the picture settings, so a report of a display problem shows what was set */
    static int logged[6] = {-1, -1, -1, -1, -1, -1};
    int now[6] = {g_settings.display_mode, g_settings.enhanced, g_settings.quality, g_settings.filter,
                  g_settings.aspect, g_settings.perspective};
    if (memcmp(now, logged, sizeof now)) {
        rt_log("settings: display %d, enhanced %d, quality %d, filter %d, shape %d, perspective %d",
               now[0], now[1], now[2], now[3], now[4], now[5]);
        memcpy(logged, now, sizeof now);
    }
}

static void host_settings_changed(void) { apply(1); }

static const char *host_controller_name(int i) { return pad_player_connected(i) ? "Xbox controller" : NULL; }
static const char *host_binding_name(int action) { return padmap_display_name(pad_map()->src[action][0]); }
static void host_start_capture(int action) { g_capture = action; g_capture_t = 0; }
static int host_capturing(void) { return g_capture; }
static void host_reset_bindings(void) {
    float dz = pad_map()->stick_deadzone;
    padmap_defaults(pad_map());
    pad_map()->stick_deadzone = dz;
    pad_save_map();
}
static int host_get_deadzone(void) { return (int)lroundf(pad_map()->stick_deadzone * 100); }
static void host_set_deadzone(int p) { pad_map()->stick_deadzone = (float)p / 100.0f; pad_save_map(); }
static void host_operator_menu(void) { ui_key(UI_CLOSE, UI_DEV_KEYBOARD); g_test_pulse = 8; }
static void host_restart(void) {
    settings_save(&g_settings, "settings.ini");
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    char *cmd = GetCommandLineA();
    if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        exit(0);
    }
    ui_toast("Couldn't restart");
}
static void host_quit(void) { exit(0); }
static int host_monitor_count(void) { return video_monitor_count(); }
static const char *host_monitor_name(int i) { return video_monitor_name(i); }
static int host_gpu_count(void) { return d3d_gpu_count(); }
static const char *host_gpu_name(int i) { return d3d_gpu_name(i); }

static const UiHost k_host = {
    host_settings_changed, host_controller_name, host_binding_name, host_start_capture, host_capturing,
    host_reset_bindings, host_get_deadzone, host_set_deadzone, host_operator_menu, host_restart, host_quit,
    host_monitor_count, host_monitor_name, host_gpu_count, host_gpu_name,
};

void menu_host_init(void) {
    ui_init(&k_host);
    apply(0);
}

/* ---------------------------------------------------------------- input */
static uint32_t g_prev_held;
static int g_prev_dir;
static float g_dir_timer;
static int g_combo_latched;
static int g_prev_connected[2];

/* Alt+Enter / F11: window <-> the full-screen mode last used (borderless unless exclusive was chosen) */
static int g_last_full = DISPLAY_BORDERLESS;
static void toggle_fullscreen(void) {
    if (g_settings.display_mode == DISPLAY_WINDOW) g_settings.display_mode = g_last_full;
    else { g_last_full = g_settings.display_mode; g_settings.display_mode = DISPLAY_WINDOW; }
    apply(1);
    ui_toast(g_settings.display_mode == DISPLAY_WINDOW ? "Window" :
             g_settings.display_mode == DISPLAY_EXCLUSIVE ? "Exclusive full screen" : "Borderless full screen");
}

/* Auto quality: step the resolution down (never up) only when frames are really being missed.
 * The GPU timer alone is not enough: a fast card clocks itself down for a game this light, so its
 * per-frame time can look high (8-16 ms on an RTX 5070 Ti) while every frame still makes the refresh.
 * So: missed refreshes (from DXGI frame statistics) in two 3-second windows in a row, with the high-res
 * pass taking a real share of the frame. */
static void auto_quality(void) {
    static int frames, last_total = -1, bad_windows;
    if (++frames < 180) return;
    frames = 0;
    int total = d3d_repeats_total();
    int missed = (total >= 0 && last_total >= 0) ? total - last_total : 0;
    last_total = total;
    if (!g_settings.enhanced || g_settings.quality != QUALITY_AUTO || total < 0) { bad_windows = 0; return; }
    int sc = hd_d3d_scale();
    bad_windows = (missed >= 6 && hd_d3d_gpu_ms() > 6.0f) ? bad_windows + 1 : 0;
    if (sc > 2 && bad_windows >= 2) {
        bad_windows = 0;
        g_hd_scale_cap = sc - 1;
        char b[96];
        snprintf(b, sizeof b, "Quality lowered to %d\xC3\x97 to keep 60 fps", sc - 1);
        ui_toast(b);
        rt_log("auto quality: %d missed refreshes, GPU %.1f ms at %dx, capping at %dx", missed, hd_d3d_gpu_ms(), sc, sc - 1);
    }
}

static void keyboard(void) {
    int vk;
    while ((vk = video_pop_key()) != 0) {
        if (vk == VK_F11) { toggle_fullscreen(); continue; }
        if (vk == VK_F9) {
            g_settings.enhanced = !g_settings.enhanced;
            apply(1);
            ui_toast(g_settings.enhanced ? "Enhanced graphics" : "Original arcade graphics");
            continue;
        }
        if (g_capture >= 0) {                         /* remapping: Esc cancels, other keys are ignored */
            if (vk == VK_ESCAPE) { g_capture = -1; ui_toast("Remapping cancelled"); }
            continue;
        }
        if (!ui_menu_visible() || !ui_menu_wants_pause()) {
            if (vk == VK_ESCAPE || vk == VK_TAB) ui_menu_toggle(UI_DEV_KEYBOARD);
            continue;
        }
        switch (vk) {
        case VK_UP: ui_key(UI_UP, UI_DEV_KEYBOARD); break;
        case VK_DOWN: ui_key(UI_DOWN, UI_DEV_KEYBOARD); break;
        case VK_LEFT: ui_key(UI_LEFT, UI_DEV_KEYBOARD); break;
        case VK_RIGHT: ui_key(UI_RIGHT, UI_DEV_KEYBOARD); break;
        case VK_RETURN: case VK_SPACE: ui_key(UI_OK, UI_DEV_KEYBOARD); break;
        case VK_ESCAPE: case VK_BACK: ui_key(UI_BACK, UI_DEV_KEYBOARD); break;
        case VK_TAB: ui_key(UI_CLOSE, UI_DEV_KEYBOARD); break;
        case 'Q': case VK_PRIOR: ui_key(UI_PREV_TAB, UI_DEV_KEYBOARD); break;
        case 'E': case VK_NEXT: ui_key(UI_NEXT_TAB, UI_DEV_KEYBOARD); break;
        }
    }
}

static void controller(float dt) {
    uint32_t held = pad_held_all();
    uint32_t pressed = held & ~g_prev_held;
    const PadMap *m = pad_map();
    uint32_t coin_bit = 1u << m->src[MAP_COIN][0], start_bit = 1u << m->src[MAP_START][0];

    /* hot-plug notices */
    for (int i = 0; i < 2; i++) {
        int c = pad_player_connected(i);
        if (c != g_prev_connected[i]) {
            char b[64];
            snprintf(b, sizeof b, "Controller %d %s", i + 1, c ? "connected" : "disconnected");
            ui_toast(b);
            g_prev_connected[i] = c;
        }
    }

    /* remapping: the next newly pressed button (not the one that started it) */
    if (g_capture >= 0) {
        g_capture_t += dt;
        uint32_t p = pressed & ~((1u << SRC_DUP) | (1u << SRC_DDOWN) | (1u << SRC_DLEFT) | (1u << SRC_DRIGHT));
        if (p) {
            int src = 0;
            while (!(p & (1u << src))) src++;
            PadMap *mm = pad_map();
            mm->src[g_capture][0] = (uint8_t)src;
            mm->src[g_capture][1] = 0;
            pad_save_map();
            char b[64];
            snprintf(b, sizeof b, "Assigned to %s", padmap_display_name(src));
            ui_toast(b);
            g_capture = -1;
            g_combo_latched = 1;                  /* don't treat this press as anything else */
        } else if (g_capture_t > 6.0f) {
            g_capture = -1;
            ui_toast("Remapping cancelled");
        }
        g_prev_held = held;
        return;
    }

    /* coin + start together opens or closes the menu */
    if ((held & coin_bit) && (held & start_bit) && (pressed & (coin_bit | start_bit)) && !g_combo_latched) {
        ui_menu_toggle(UI_DEV_PAD);
        g_combo_latched = 1;
    }
    if (!(held & (coin_bit | start_bit))) g_combo_latched = 0;

    if (ui_menu_visible() && ui_menu_wants_pause() && !g_combo_latched) {
        if (pressed & (1u << SRC_A)) ui_key(UI_OK, UI_DEV_PAD);
        if (pressed & (1u << SRC_B)) ui_key(UI_BACK, UI_DEV_PAD);
        if (pressed & (1u << SRC_LB)) ui_key(UI_PREV_TAB, UI_DEV_PAD);
        if (pressed & (1u << SRC_RB)) ui_key(UI_NEXT_TAB, UI_DEV_PAD);
        if ((pressed & start_bit) && !(held & coin_bit)) ui_key(UI_CLOSE, UI_DEV_PAD);
        /* directions from the D-pad or the stick, with key repeat */
        int dir = pad_stick_dir();
        if (held & (1u << SRC_DUP)) dir |= 1;
        if (held & (1u << SRC_DDOWN)) dir |= 2;
        if (held & (1u << SRC_DLEFT)) dir |= 4;
        if (held & (1u << SRC_DRIGHT)) dir |= 8;
        int fire = 0;
        if (dir != g_prev_dir) { fire = dir != 0; g_dir_timer = 0.38f; }
        else if (dir) { g_dir_timer -= dt; if (g_dir_timer <= 0) { fire = 1; g_dir_timer = 0.085f; } }
        if (fire) {
            if (dir & 1) ui_key(UI_UP, UI_DEV_PAD);
            else if (dir & 2) ui_key(UI_DOWN, UI_DEV_PAD);
            else if (dir & 4) ui_key(UI_LEFT, UI_DEV_PAD);
            else if (dir & 8) ui_key(UI_RIGHT, UI_DEV_PAD);
        }
        g_prev_dir = dir;
    } else {
        g_prev_dir = 0;
    }
    g_prev_held = held;
}

void menu_host_frame(float dt) {
    auto_quality();
    keyboard();
    controller(dt);
    ui_update(dt);
    float b = ui_backdrop();
    g_blit_dim = b;
    g_blit_blur = b;
}

/* While the menu has just closed, keep the button that closed it away from the game. */
int menu_host_input_blocked(void) {
    static int blocked;
    if (ui_menu_wants_pause()) { blocked = 1; return 1; }
    if (blocked && pad_held_all() == 0) blocked = 0;
    return blocked;
}

void menu_host_cancel_capture(void) { g_capture = -1; }
