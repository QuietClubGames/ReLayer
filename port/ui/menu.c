/* Pause menu, toasts and frame-rate counter (see ui.h). */
#include "ui.h"
#include "settings.h"
#include "game.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- palette */
#define COL_GOLD        0xE8B64CFFu
#define COL_TEXT        0xF2F3F7FFu
#define COL_TEXT_DIM    0x9AA0AEFFu
#define COL_TEXT_OFF    0x5C6170FFu
#define COL_PANEL_TOP   0x1B1D27F2u
#define COL_PANEL_BOT   0x0E0F15F4u
#define COL_LINE        0xFFFFFF16u
#define COL_RED         0xE5675FFFu

static uint32_t fade(uint32_t c, float a) {
    uint32_t al = (uint32_t)((float)(c & 0xFF) * (a < 0 ? 0 : a > 1 ? 1 : a) + 0.5f);
    return (c & 0xFFFFFF00u) | al;
}
static uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        float x = (float)((a >> s) & 0xFF), y = (float)((b >> s) & 0xFF);
        r |= (uint32_t)(x + (y - x) * t + 0.5f) << s;
    }
    return r;
}
static float ease_out(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return 1 - (1 - t) * (1 - t) * (1 - t); }
static float approach(float cur, float target, float dt, float speed) {
    float k = 1 - expf(-speed * dt);
    return cur + (target - cur) * k;
}

/* ---------------------------------------------------------------- menu model */
enum { IT_CHOICE, IT_TOGGLE, IT_SLIDER, IT_ACTION, IT_INFO, IT_BINDING };

typedef struct {
    int type;
    const char *label;
    const char *help;
    int (*get)(void);
    void (*set)(int);
    const char *const *choices;
    int nchoices;
    int lo, hi, step;            /* slider */
    void (*action)(void);
    int binding;
    int (*enabled)(void);
    const char *(*info)(void);
    int (*dyn_count)(void);          /* choices that depend on the PC (monitors, graphics cards) */
    const char *(*dyn_name)(int);
} Item;

static const UiHost *H;

/* settings accessors */
#define ACCESS(name, field) \
    static int get_##name(void) { return g_settings.field; } \
    static void set_##name(int v) { g_settings.field = v; if (H) H->settings_changed(); }
ACCESS(display_mode, display_mode)
ACCESS(window_size, window_size)
ACCESS(monitor, monitor)
ACCESS(aspect, aspect)
ACCESS(quality, quality)
ACCESS(filter, filter)
ACCESS(gpu, gpu)
ACCESS(fps, show_fps)
ACCESS(volume, volume)
ACCESS(mute, mute)
ACCESS(unlock, unlock_all)
static int get_graphics(void) { return g_settings.enhanced ? 0 : 1; }
static void set_graphics(int v) { g_settings.enhanced = v == 0; H->settings_changed(); }
static int get_textures(void) { return g_settings.perspective ? 0 : 1; }
static void set_textures(int v) { g_settings.perspective = v == 0; H->settings_changed(); }
static int enhanced_on(void) { return g_settings.enhanced; }
static int windowed(void) { return g_settings.display_mode == DISPLAY_WINDOW; }
static int get_deadzone(void) { return H->get_deadzone(); }
static void set_deadzone(int v) { H->set_deadzone(v); }

/* arcade settings live in the board's EEPROM (src/game/arcade_settings.c) */
#define ARCADE(name, which) \
    static int get_##name(void) { return arcade_setting_get(which); } \
    static void set_##name(int v) { arcade_setting_set(which, v); }
ARCADE(free_play, AS_FREE_PLAY)
ARCADE(difficulty, AS_DIFFICULTY)
ARCADE(damage, AS_DAMAGE)
ARCADE(timer, AS_TIMER)
ARCADE(fights_1p, AS_FIGHTS_1P)
ARCADE(fights_vs, AS_FIGHTS_VS)
ARCADE(change_cont, AS_CHANGE_CONTINUE)
ARCADE(change_vs, AS_CHANGE_VS)
ARCADE(attract_music, AS_ATTRACT_MUSIC)
ARCADE(event_mode, AS_EVENT_MODE)

static const char *const k_mode[] = {"Window", "Borderless full screen", "Exclusive full screen"};
static const char *const k_winsize[] = {"640\xC3\x97" "480", "960\xC3\x97" "720", "1280\xC3\x97" "960",
                                        "1600\xC3\x97" "1200", "1920\xC3\x97" "1440"};
static const char *const k_aspect[] = {"Arcade 4:3", "Stretch to fill", "4:3, whole-number scale"};
static const char *const k_graphics[] = {"Enhanced", "Original arcade"};
static const char *const k_quality[] = {"Auto (keeps 60 fps)", "Performance  2\xC3\x97", "Balanced  3\xC3\x97",
                                        "High  4\xC3\x97", "Ultra  6\xC3\x97", "Extreme  8\xC3\x97"};
static const char *const k_tex[] = {"Perspective-correct", "Original (PS1 warping)"};
static const char *const k_filter[] = {"Sharp", "Smooth", "Scanlines"};
static const char *const k_levels8[] = {"1  Lowest", "2", "3", "4", "5", "6", "7", "8  Highest"};
static const char *const k_timer[] = {"Slowest", "Normal", "Fast", "Fastest"};
static const char *const k_fights[] = {"1", "2", "3", "4", "5"};

static void act_resume(void);
static void act_operator(void) { H->operator_menu(); }
static void act_restart(void) { H->restart(); }
static int g_quit_armed;
static void act_quit(void) {
    if (g_quit_armed) H->quit();
    else g_quit_armed = 1;
}
static void act_reset_binds(void) { H->reset_bindings(); ui_toast("Buttons restored to the defaults"); }
static const char *info_controllers(void) { return ""; }    /* drawn as two status badges */

/* lists that depend on the PC (monitors, graphics cards) */
static int count_monitors(void) { return 1 + H->monitor_count(); }
static const char *name_monitor(int i) { return i == 0 ? "Automatic" : H->monitor_name(i - 1); }
static int count_gpus(void) { return 1 + H->gpu_count(); }
static const char *name_gpu(int i) { return i == 0 ? "Automatic (fastest)" : H->gpu_name(i - 1); }

#define CHOICE(lbl, hlp, g, s, arr, en) {IT_CHOICE, lbl, hlp, g, s, arr, (int)(sizeof arr / sizeof arr[0]), 0, 0, 0, NULL, 0, en, NULL, NULL, NULL}
#define DCHOICE(lbl, hlp, g, s, cnt, nm, en) {IT_CHOICE, lbl, hlp, g, s, NULL, 0, 0, 0, 0, NULL, 0, en, NULL, cnt, nm}
#define TOGGLE(lbl, hlp, g, s) {IT_TOGGLE, lbl, hlp, g, s, NULL, 0, 0, 1, 1, NULL, 0, NULL, NULL, NULL, NULL}
#define SLIDER(lbl, hlp, g, s, lo, hi, st) {IT_SLIDER, lbl, hlp, g, s, NULL, 0, lo, hi, st, NULL, 0, NULL, NULL, NULL, NULL}
#define ACTION(lbl, hlp, fn) {IT_ACTION, lbl, hlp, NULL, NULL, NULL, 0, 0, 0, 0, fn, 0, NULL, NULL, NULL, NULL}
#define BIND(lbl, hlp, n) {IT_BINDING, lbl, hlp, NULL, NULL, NULL, 0, 0, 0, 0, NULL, n, NULL, NULL, NULL, NULL}
#define INFO(lbl, hlp, fn) {IT_INFO, lbl, hlp, NULL, NULL, NULL, 0, 0, 0, 0, NULL, 0, NULL, fn, NULL, NULL}

static const Item k_display[] = {
    CHOICE("Display mode", "Borderless keeps alt-tab instant. Exclusive takes over the monitor. Alt+Enter / F11 also switch.", get_display_mode, set_display_mode, k_mode, NULL),
    CHOICE("Window size", "Size of the window when not in full screen.", get_window_size, set_window_size, k_winsize, windowed),
    DCHOICE("Monitor", "Which display to use for full screen (and to place the window on).", get_monitor, set_monitor, count_monitors, name_monitor, NULL),
    CHOICE("Picture shape", "The arcade monitor was 4:3. Whole-number scaling keeps every scanline the same height.", get_aspect, set_aspect, k_aspect, NULL),
    CHOICE("Graphics", "Enhanced redraws the 3D on your graphics card at high resolution. F9 switches while playing.", get_graphics, set_graphics, k_graphics, NULL),
    CHOICE("Quality", "Auto picks the sharpest setting your PC can hold at 60 fps and steps down if it ever can't.", get_quality, set_quality, k_quality, enhanced_on),
    CHOICE("3D textures", "Perspective-correct removes the warping textures had on the original hardware.", get_textures, set_textures, k_tex, enhanced_on),
    CHOICE("Screen filter", "Sharp pixels, smooth scaling, or arcade-monitor scanlines.", get_filter, set_filter, k_filter, NULL),
    DCHOICE("Graphics card", "For PCs with more than one. Takes effect after Restart game (System).", get_gpu, set_gpu, count_gpus, name_gpu, NULL),
    TOGGLE("Frame rate counter", "Shows frames per second in the corner of the screen.", get_fps, set_fps),
};
static const Item k_audio[] = {
    SLIDER("Volume", "Music, voices and effects (the arcade board mixes them together).", get_volume, set_volume, 0, 100, 5),
    TOGGLE("Mute", "Silence all sound.", get_mute, set_mute),
};
static const Item k_controls[] = {
    INFO("Controllers", "Xbox controllers: the first one connected is player 1, the second is player 2.", info_controllers),
    BIND("Button 1  \xC2\xB7  top left", "Select, then press the controller button you want for this.", 0),
    BIND("Button 2  \xC2\xB7  top middle", "Select, then press the controller button you want for this.", 1),
    BIND("Button 3  \xC2\xB7  top right", "Select, then press the controller button you want for this.", 2),
    BIND("Button 4  \xC2\xB7  bottom left", "Select, then press the controller button you want for this.", 3),
    BIND("Button 5  \xC2\xB7  bottom middle", "Select, then press the controller button you want for this.", 4),
    BIND("Button 6  \xC2\xB7  bottom right", "Select, then press the controller button you want for this.", 5),
    BIND("Start", "Select, then press the controller button you want for this.", 6),
    BIND("Insert coin", "Select, then press the controller button you want for this.", 7),
    SLIDER("Stick dead zone", "How far the left stick must move before it counts.", get_deadzone, set_deadzone, 10, 90, 5),
    ACTION("Restore default buttons", "X Y RB on top, A B RT on the bottom, Start, and Back for coins.", act_reset_binds),
};
static const Item k_game[] = {
    TOGGLE("Unlock all characters", "Preston Ajax, Clemence Keliber and Joe Fendi, which the arcade released after 15 to 43 days.", get_unlock, set_unlock),
    TOGGLE("Free play", "No coins needed: press Start to play.", get_free_play, set_free_play),
    CHOICE("Difficulty", "How strong the computer opponents are. The arcade default is 3.", get_difficulty, set_difficulty, k_levels8, NULL),
    CHOICE("Damage", "How much health each hit takes. The arcade default is 4.", get_damage, set_damage, k_levels8, NULL),
    CHOICE("Round timer", "How fast the round clock counts down. The arcade default is Normal.", get_timer, set_timer, k_timer, NULL),
    CHOICE("Fight count  \xC2\xB7  1 player", "The operator menu's FIGHT COUNT for games against the computer (default 2).", get_fights_1p, set_fights_1p, k_fights, NULL),
    CHOICE("Fight count  \xC2\xB7  versus", "The operator menu's FIGHT COUNT for two-player games (default 2).", get_fights_vs, set_fights_vs, k_fights, NULL),
    TOGGLE("Change character on continue", "Lets you pick a different fighter after continuing.", get_change_cont, set_change_cont),
    TOGGLE("Change character in versus", "Lets both players pick again between versus matches.", get_change_vs, set_change_vs),
    TOGGLE("Music in attract mode", "Plays music while the demo runs between games.", get_attract_music, set_attract_music),
    TOGGLE("Event mode", "The arcade's tournament setting from the operator menu.", get_event_mode, set_event_mode),
};
static const Item k_system[] = {
    ACTION("Resume", "Back to the game.", act_resume),
    ACTION("Arcade operator menu", "The cabinet's own test menu: display, switch and sound tests, statistics.", act_operator),
    ACTION("Restart game", "Starts the arcade board again from power on.", act_restart),
    ACTION("Quit to desktop", "Settings are saved automatically.", act_quit),
};

typedef struct { const char *name; const Item *items; int n; } Section;
static const Section k_sections[] = {
    {"DISPLAY", k_display, (int)(sizeof k_display / sizeof k_display[0])},
    {"AUDIO", k_audio, (int)(sizeof k_audio / sizeof k_audio[0])},
    {"CONTROLS", k_controls, (int)(sizeof k_controls / sizeof k_controls[0])},
    {"GAME", k_game, (int)(sizeof k_game / sizeof k_game[0])},
    {"SYSTEM", k_system, (int)(sizeof k_system / sizeof k_system[0])},
};
#define NSECT 5
#define MAXROWS 16

static int choice_count(const Item *it) { return it->dyn_count ? it->dyn_count() : it->nchoices; }
static const char *choice_name(const Item *it, int v) {
    int n = choice_count(it);
    if (v < 0 || v >= n) v = 0;
    return it->dyn_name ? it->dyn_name(v) : it->choices[v];
}


/* ---------------------------------------------------------------- state */
static int g_open;              /* target state */
static float g_t;               /* 0 closed .. 1 open (animated) */
static int g_sect, g_row[NSECT];
static float g_sel_y, g_tab_x, g_tab_w;   /* animated highlight positions */
static float g_knob[NSECT][MAXROWS];      /* toggle knob / slider animation */
static int g_device = UI_DEV_KEYBOARD;
static float g_flash;                     /* value-change pulse */
static float g_time;                      /* seconds, for gentle pulsing */
static float g_panel_h;                   /* animated panel height */

typedef struct { char text[96]; float age; } Toast;
static Toast g_toasts[4];

void ui_init(const UiHost *host) { H = host; }

static int item_enabled(const Item *it) {
    return it->type != IT_INFO && (!it->enabled || it->enabled());
}

static void select_valid(int dir) {
    const Section *s = &k_sections[g_sect];
    int r = g_row[g_sect];
    for (int k = 0; k < s->n; k++) {
        if (r < 0) r = s->n - 1;
        if (r >= s->n) r = 0;
        if (item_enabled(&s->items[r])) break;
        r += dir ? dir : 1;
    }
    g_row[g_sect] = r < 0 ? 0 : r >= s->n ? s->n - 1 : r;
}

void ui_menu_toggle(int device) {
    g_device = device;
    g_open = !g_open;
    g_quit_armed = 0;
    if (g_open) select_valid(1);
}
static void act_resume(void) { g_open = 0; }
int ui_menu_visible(void) { return g_open || g_t > 0.001f; }
int ui_menu_wants_pause(void) { return g_open || g_t > 0.001f; }
float ui_backdrop(void) { return ease_out(g_t); }

void ui_toast(const char *msg) {
    for (int i = 3; i > 0; i--) g_toasts[i] = g_toasts[i - 1];
    snprintf(g_toasts[0].text, sizeof g_toasts[0].text, "%s", msg);
    g_toasts[0].age = 0;
}

static void change(const Item *it, int dir) {
    if (!it->get || !it->set) return;
    int v = it->get();
    if (it->type == IT_CHOICE) { int n = choice_count(it); v = (v + dir + n) % n; }
    else if (it->type == IT_TOGGLE) v = !v;
    else if (it->type == IT_SLIDER) {
        v += dir * it->step;
        if (v < it->lo) v = it->lo;
        if (v > it->hi) v = it->hi;
        if (v == it->get()) return;
    }
    it->set(v);
    g_flash = 1;
}

void ui_key(int key, int device) {
    g_device = device;
    if (!g_open) return;
    if (H->capturing() >= 0) return;               /* waiting for a button press to remap */
    const Section *s = &k_sections[g_sect];
    const Item *it = &s->items[g_row[g_sect]];
    if (key != UI_OK) g_quit_armed = 0;
    switch (key) {
    case UI_UP: g_row[g_sect]--; select_valid(-1); break;
    case UI_DOWN: g_row[g_sect]++; select_valid(1); break;
    case UI_LEFT: if (it->type == IT_CHOICE || it->type == IT_SLIDER) change(it, -1); break;
    case UI_RIGHT: if (it->type != IT_ACTION && it->type != IT_BINDING) change(it, 1); break;
    case UI_PREV_TAB: g_sect = (g_sect + NSECT - 1) % NSECT; select_valid(1); break;
    case UI_NEXT_TAB: g_sect = (g_sect + 1) % NSECT; select_valid(1); break;
    case UI_OK:
        if (it->type == IT_ACTION) it->action();
        else if (it->type == IT_BINDING) H->start_capture(it->binding);
        else if (it->type == IT_TOGGLE || it->type == IT_CHOICE) change(it, 1);
        break;
    case UI_BACK:
    case UI_CLOSE: g_open = 0; break;
    }
}

void ui_update(float dt) {
    if (dt > 0.1f) dt = 0.1f;
    g_t += (g_open ? 1 : -1) * dt / (g_open ? 0.22f : 0.16f);
    if (g_t < 0) g_t = 0;
    if (g_t > 1) g_t = 1;
    g_flash = approach(g_flash, 0, dt, 6);
    g_time += dt;
    for (int i = 0; i < 4; i++) g_toasts[i].age += dt;
    for (int s = 0; s < NSECT; s++)
        for (int r = 0; r < k_sections[s].n; r++) {
            const Item *it = &k_sections[s].items[r];
            float target = 0;
            if (it->type == IT_TOGGLE) target = (float)it->get();
            else if (it->type == IT_SLIDER) target = (float)(it->get() - it->lo) / (float)(it->hi - it->lo);
            g_knob[s][r] = approach(g_knob[s][r], target, dt, 18);
        }
}

/* ---------------------------------------------------------------- drawing helpers */
static void text_c(float cx, float y, int font, uint32_t col, const char *s) { uip_text(cx - uip_text_w(font, s) / 2, y, font, col, s); }
static void text_r(float rx, float y, int font, uint32_t col, const char *s) { uip_text(rx - uip_text_w(font, s), y, font, col, s); }
static float vcenter(float y, float h, int font) { return y + (h - uip_line_h(font)) / 2; }

/* a controller button drawn like the real thing: coloured face buttons, dark shoulder buttons */
static float button_glyph(float x, float cy, const char *name, float a, int right_align) {
    uint32_t face = 0;
    if (!strcmp(name, "A")) face = 0x5DBB63FFu;
    else if (!strcmp(name, "B")) face = 0xE05C5CFFu;
    else if (!strcmp(name, "X")) face = 0x4A90E2FFu;
    else if (!strcmp(name, "Y")) face = 0xE8C447FFu;
    float h = 32, w;
    if (face) {
        w = h;
        if (right_align) x -= w;
        uip_rect(x, cy - h / 2, w, h, h / 2, fade(face, a), fade(mix(face, 0x000000FFu, 0.25f), a), 0, 0, 0);
        text_c(x + w / 2, vcenter(cy - h / 2, h, UI_FONT_SMALL), UI_FONT_SMALL, fade(0x101114FFu, a), name);
    } else {
        w = uip_text_w(UI_FONT_SMALL, name) + 22;
        if (right_align) x -= w;
        uip_rect(x, cy - h / 2, w, h, 8, fade(0x3A3E4CFFu, a), fade(0x2A2D38FFu, a), 1, fade(0xFFFFFF22u, a), 0);
        text_c(x + w / 2, vcenter(cy - h / 2, h, UI_FONT_SMALL), UI_FONT_SMALL, fade(COL_TEXT, a), name);
    }
    return w;
}

static float key_glyph(float x, float cy, const char *name, float a) {
    float h = 30, w = uip_text_w(UI_FONT_SMALL, name) + 20;
    if (w < h) w = h;
    uip_rect(x, cy - h / 2, w, h, 6, fade(0xE9EAEEFFu, a), fade(0xC9CBD3FFu, a), 0, 0, 0);
    text_c(x + w / 2, vcenter(cy - h / 2, h, UI_FONT_SMALL), UI_FONT_SMALL, fade(0x15161BFFu, a), name);
    return w;
}

static float hint(float x, float cy, const char *glyph_pad, const char *glyph_key, const char *label, float a) {
    float w = g_device == UI_DEV_PAD ? button_glyph(x, cy, glyph_pad, a, 0) : key_glyph(x, cy, glyph_key, a);
    uip_text(x + w + 10, vcenter(cy - 16, 32, UI_FONT_SMALL), UI_FONT_SMALL, fade(COL_TEXT_DIM, a), label);
    return w + 10 + uip_text_w(UI_FONT_SMALL, label) + 34;
}

/* ---------------------------------------------------------------- the menu */
static void draw_menu(float vw, float vh) {
    float e = ease_out(g_t), a = e;
    const Section *s = &k_sections[g_sect];
    const float W = 1140, HMAX = 940, row_h = 48, gap = 4;
    float target_h = 210 + 11.0f * (row_h + gap) + 150;      /* one height for every page */
    if (target_h > HMAX) target_h = HMAX;
    if (g_panel_h == 0) g_panel_h = target_h;
    g_panel_h = approach(g_panel_h, target_h, 1.0f / 60, 16);
    const float Hh = g_panel_h;
    float px = (vw - W) / 2, py = (vh - Hh) / 2 + (1 - e) * 28;      /* centred; glides as the height changes */

    /* soft shadow, then the glass panel */
    uip_rect(px - 10, py + 6, W + 20, Hh + 20, 30, fade(0x00000000u | 0x90, a), fade(0x00000000u | 0x90, a), 0, 0, 34);
    uip_rect(px, py, W, Hh, 20, fade(COL_PANEL_TOP, a), fade(COL_PANEL_BOT, a), 1.5f, fade(0xFFFFFF1Cu, a), 0);
    if (uip_art_available()) {
        /* menu art (menu_art.png / .jpg next to the game): under a dark gradient so the text stays readable */
        uip_art(px, py, W, Hh, 20, a);
        uip_rect_hgrad(px, py, W, Hh, 20, fade(0x0E0F15F4u, a), fade(0x0E0F1550u, a));
        uip_rect(px, py, W, Hh, 20, fade(0x0E0F1500u, a), fade(0x0E0F15C0u, a), 0, 0, 0);
        uip_rect(px, py, W, Hh, 20, 0, 0, 1.5f, fade(0xFFFFFF1Cu, a), 0);
    }
    /* a faint gold glow along the top edge */
    uip_rect(px + 40, py, W - 80, 2, 1, fade(0xE8B64C00u | 0x70, a), fade(0xE8B64C00u | 0x70, a), 0, 0, 0);

    /* header */
    uip_text(px + 58, py + 38, UI_FONT_TITLE, fade(COL_TEXT, a), "RELAYER");
    uip_rect(px + 60, py + 100, 70, 4, 2, fade(COL_GOLD, a), fade(COL_GOLD, a), 0, 0, 0);
    {
        const char *p = "PAUSED";
        float tw = uip_text_w(UI_FONT_SMALL, p) + 28;
        uip_rect(px + W - 58 - tw, py + 48, tw, 34, 17, fade(0xFFFFFF14u, a), fade(0xFFFFFF0Cu, a), 1, fade(0xFFFFFF18u, a), 0);
        text_c(px + W - 58 - tw / 2, vcenter(py + 48, 34, UI_FONT_SMALL), UI_FONT_SMALL, fade(COL_TEXT_DIM, a), p);
    }

    /* section tabs */
    float ty = py + 132, tx = px + 58;
    float tab_x[NSECT], tab_w[NSECT];
    float lead = g_device == UI_DEV_PAD ? button_glyph(tx, ty + 18, "LB", a, 0) : key_glyph(tx, ty + 18, "Q", a);
    tx += lead + 26;
    for (int i = 0; i < NSECT; i++) {
        tab_w[i] = uip_text_w(UI_FONT_HEAD, k_sections[i].name);
        tab_x[i] = tx;
        uint32_t c = i == g_sect ? COL_TEXT : COL_TEXT_DIM;
        uip_text(tx, vcenter(ty, 36, UI_FONT_HEAD), UI_FONT_HEAD, fade(c, a), k_sections[i].name);
        tx += tab_w[i] + 44;
    }
    if (g_device == UI_DEV_PAD) button_glyph(tx - 18, ty + 18, "RB", a, 0);
    else key_glyph(tx - 18, ty + 18, "E", a);
    if (g_tab_w == 0) { g_tab_x = tab_x[g_sect]; g_tab_w = tab_w[g_sect]; }
    uip_rect(g_tab_x, ty + 42, g_tab_w, 3, 1.5f, fade(COL_GOLD, a), fade(COL_GOLD, a), 0, 0, 0);
    uip_rect(px + 40, py + 190, W - 80, 1, 0, fade(COL_LINE, a), fade(COL_LINE, a), 0, 0, 0);

    /* rows */
    const float y0 = py + 210, rows_bottom = py + Hh - 124;
    float target_y = y0 + (float)g_row[g_sect] * (row_h + gap);
    if (g_sel_y == 0) g_sel_y = target_y;
    int capturing = H->capturing();
    {
        float hy = g_sel_y;
        uip_rect(px + 34, hy, W - 68, row_h, 12, fade(0xE8B64C24u, a), fade(0xE8B64C14u, a), 1, fade(0xE8B64C30u, a), 0);
        uip_rect(px + 34, hy + 10, 4, row_h - 20, 2, fade(COL_GOLD, a), fade(COL_GOLD, a), 0, 0, 0);
    }
    for (int r = 0; r < s->n; r++) {
        const Item *it = &s->items[r];
        float y = y0 + (float)r * (row_h + gap);
        /* while the panel is still growing, rows below its edge fade in */
        float ra = (rows_bottom - y) / row_h;
        if (ra <= 0) continue;
        float a_save = a;
        a = a_save * (ra > 1 ? 1 : ra);
        int sel = r == g_row[g_sect], en = it->type == IT_INFO || !it->enabled || it->enabled();
        uint32_t lc = !en ? COL_TEXT_OFF : sel ? COL_TEXT : 0xC9CDD6FFu;
        const char *label = it->label;
        if (it->action == act_quit && g_quit_armed) label = "Press again to quit";
        uip_text(px + 62, vcenter(y, row_h, UI_FONT_BODY), UI_FONT_BODY, fade(it->action == act_quit && g_quit_armed ? COL_RED : lc, a), label);
        float rx = px + W - 66, cy = y + row_h / 2;
        switch (it->type) {
        case IT_CHOICE: {
            const char *v = choice_name(it, it->get());
            uint32_t vc = !en ? COL_TEXT_OFF : sel ? mix(COL_TEXT, COL_GOLD, g_flash * 0.8f) : COL_TEXT_DIM;
            uint32_t ac = !en ? COL_TEXT_OFF : sel ? COL_GOLD : 0x6A7080FFu;
            float vw2 = uip_text_w(UI_FONT_VALUE, v);
            if (sel && en) {           /* arrows only on the focused row */
                text_r(rx, vcenter(y, row_h, UI_FONT_HEAD) - 2, UI_FONT_HEAD, fade(ac, a), "\xE2\x80\xBA");
                uip_text(rx - 30 - vw2, vcenter(y, row_h, UI_FONT_VALUE), UI_FONT_VALUE, fade(vc, a), v);
                text_r(rx - 48 - vw2, vcenter(y, row_h, UI_FONT_HEAD) - 2, UI_FONT_HEAD, fade(ac, a), "\xE2\x80\xB9");
            } else {
                text_r(rx, vcenter(y, row_h, UI_FONT_VALUE), UI_FONT_VALUE, fade(vc, a), v);
            }
            break;
        }
        case IT_TOGGLE: {
            float k = g_knob[g_sect][r], tw = 60, th = 32;
            uint32_t on = COL_GOLD, off = 0x3B3F4DFFu;
            uip_rect(rx - tw, cy - th / 2, tw, th, th / 2, fade(mix(off, on, k), a), fade(mix(off, mix(on, 0x000000FFu, 0.15f), k), a), 0, 0, 0);
            float kx = rx - tw + 4 + k * (tw - th);
            uip_rect(kx, cy - th / 2 + 4, th - 8, th - 8, (th - 8) / 2, fade(0xFFFFFFFFu, a), fade(0xE4E5EAFFu, a), 0, 0, 0);
            break;
        }
        case IT_SLIDER: {
            float k = g_knob[g_sect][r], tw = 250;
            char buf[16];
            snprintf(buf, sizeof buf, "%d%%", it->get());
            text_r(rx - tw - 22, vcenter(y, row_h, UI_FONT_VALUE), UI_FONT_VALUE, fade(sel ? COL_TEXT : COL_TEXT_DIM, a), buf);
            uip_rect(rx - tw, cy - 3, tw, 6, 3, fade(0xFFFFFF1Eu, a), fade(0xFFFFFF1Eu, a), 0, 0, 0);
            uip_rect(rx - tw, cy - 3, tw * k, 6, 3, fade(COL_GOLD, a), fade(COL_GOLD, a), 0, 0, 0);
            float kx = rx - tw + tw * k;
            uip_rect(kx - 11, cy - 11, 22, 22, 11, fade(0xFFFFFFFFu, a), fade(0xE4E5EAFFu, a), 0, 0, 0);
            break;
        }
        case IT_BINDING:
            if (capturing == it->binding) {
                float pulse = 0.5f + 0.5f * sinf(g_time * 5.0f);
                const char *msg = "Press a button\xE2\x80\xA6";
                float w2 = uip_text_w(UI_FONT_SMALL, msg) + 30;
                uip_rect(rx - w2, cy - 17, w2, 34, 17, fade(0xE8B64C30u + (uint32_t)(pulse * 0x28), a),
                         fade(0xE8B64C20u, a), 1, fade(COL_GOLD, a * (0.6f + 0.4f * pulse)), 0);
                text_c(rx - w2 / 2, vcenter(cy - 17, 34, UI_FONT_SMALL), UI_FONT_SMALL, fade(COL_TEXT, a), msg);
            } else {
                button_glyph(rx, cy, H->binding_name(it->binding), a, 1);
            }
            break;
        case IT_ACTION:
            text_r(rx, vcenter(y, row_h, UI_FONT_VALUE), UI_FONT_VALUE, fade(sel ? COL_GOLD : 0x6A7080FFu, a), "\xE2\x80\xBA");
            break;
        case IT_INFO:
            if (it->info == info_controllers) {      /* a badge per player with a status light */
                float bx = rx;
                for (int p = 1; p >= 0; p--) {
                    int on = H->controller_name(p) != NULL;
                    char lbl[32];
                    snprintf(lbl, sizeof lbl, "P%d  %s", p + 1, on ? "Connected" : "Not connected");
                    float bw = uip_text_w(UI_FONT_SMALL, lbl) + 46, bh = 32;
                    bx -= bw;
                    uip_rect(bx, cy - bh / 2, bw, bh, bh / 2, fade(on ? 0x5DBB6324u : 0xFFFFFF0Cu, a),
                             fade(on ? 0x5DBB6318u : 0xFFFFFF08u, a), 1, fade(on ? 0x5DBB6366u : 0xFFFFFF18u, a), 0);
                    uint32_t dot = on ? 0x6FD675FFu : 0x6A7080FFu;
                    uip_rect(bx + 16, cy - 4, 8, 8, 4, fade(dot, a), fade(dot, a), 0, 0, 0);
                    uip_text(bx + 32, vcenter(cy - bh / 2, bh, UI_FONT_SMALL), UI_FONT_SMALL, fade(on ? COL_TEXT : COL_TEXT_DIM, a), lbl);
                    bx -= 10;
                }
            } else {
                text_r(rx, vcenter(y, row_h, UI_FONT_VALUE), UI_FONT_VALUE, fade(COL_TEXT_DIM, a), it->info());
            }
            break;
        }
        a = a_save;
    }

    /* help line for the selected row */
    float fy = py + Hh - 118;
    uip_rect(px + 40, fy, W - 80, 1, 0, fade(COL_LINE, a), fade(COL_LINE, a), 0, 0, 0);
    uip_text(px + 62, fy + 18, UI_FONT_SMALL, fade(COL_TEXT_DIM, a), s->items[g_row[g_sect]].help);

    /* button hints */
    float hx = px + 62, hy = py + Hh - 42;
    hx += hint(hx, hy, "A", "Enter", "Select", a);
    hx += hint(hx, hy, "B", "Esc", "Back", a);
    if (g_device == UI_DEV_PAD) hint(hx, hy, "MENU", "Tab", "Resume", a);
    else hint(hx, hy, "VIEW", "\xE2\x86\x90\xE2\x86\x92", "Change", a);

    g_sel_y = approach(g_sel_y, target_y, 1.0f / 60, 22);
    g_tab_x = approach(g_tab_x, tab_x[g_sect], 1.0f / 60, 20);
    g_tab_w = approach(g_tab_w, tab_w[g_sect], 1.0f / 60, 20);
}

static void draw_toasts(float vw, float vh) {
    (void)vw;
    float y = vh - 92;
    for (int i = 0; i < 4; i++) {
        Toast *t = &g_toasts[i];
        if (!t->text[0] || t->age > 3.0f) continue;
        float in = ease_out(t->age / 0.25f), out = t->age > 2.5f ? 1 - (t->age - 2.5f) / 0.5f : 1;
        float a = in * out;
        float w = uip_text_w(UI_FONT_SMALL, t->text) + 64, h = 46, x = 40 - (1 - in) * 30;
        uip_rect(x, y, w, h, 23, fade(0x1B1D27EEu, a), fade(0x12131AEEu, a), 1, fade(0xFFFFFF1Cu, a), 0);
        uip_rect(x + 20, y + h / 2 - 5, 10, 10, 5, fade(COL_GOLD, a), fade(COL_GOLD, a), 0, 0, 0);
        uip_text(x + 42, vcenter(y, h, UI_FONT_SMALL), UI_FONT_SMALL, fade(COL_TEXT, a), t->text);
        y -= h + 10;
    }
}

void ui_draw(float vw, float vh, float fps) {
    if (g_settings.show_fps) {
        char b[32];
        snprintf(b, sizeof b, "%.0f FPS", fps);
        float w = uip_text_w(UI_FONT_SMALL, b) + 28;
        uip_rect(vw - 30 - w, 26, w, 36, 18, 0x101118B0u, 0x101118B0u, 1, 0xFFFFFF18u, 0);
        text_c(vw - 30 - w / 2, vcenter(26, 36, UI_FONT_SMALL), UI_FONT_SMALL, fps >= 59 ? 0x9BE39FFFu : COL_GOLD, b);
    }
    if (ui_menu_visible()) draw_menu(vw, vh);
    draw_toasts(vw, vh);
}
