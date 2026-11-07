#pragma once
#include <stdint.h>

/*
 * Overlay UI: the pause menu, toast notifications and the frame-rate counter.
 * Platform independent: it lays everything out in a virtual space 1080 units tall (width follows the window)
 * and draws through the small uip_* interface, implemented per platform (pc/ui_d3d11.c).
 * Colours are 0xRRGGBBAA.
 */

enum { UI_FONT_BODY, UI_FONT_VALUE, UI_FONT_SMALL, UI_FONT_HEAD, UI_FONT_TITLE, UI_FONT_COUNT };

/* ---- drawing, provided by the platform ---- */
/* rounded rectangle with a vertical gradient, optional border; feather > 0 makes a soft shadow */
void  uip_rect(float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom,
               float border, uint32_t border_col, float feather);
void  uip_text(float x, float y, int font, uint32_t col, const char *utf8);   /* y = top of the line */
float uip_text_w(int font, const char *utf8);
float uip_line_h(int font);
/* horizontal gradient rectangle (left -> right) */
void  uip_rect_hgrad(float x, float y, float w, float h, float radius, uint32_t left, uint32_t right);
/* optional menu art: drawn to cover the rectangle, clipped to its rounded corners */
int   uip_art_available(void);
void  uip_art(float x, float y, float w, float h, float radius, float alpha);

/* ---- input ---- */
enum { UI_UP, UI_DOWN, UI_LEFT, UI_RIGHT, UI_OK, UI_BACK, UI_PREV_TAB, UI_NEXT_TAB, UI_CLOSE };
enum { UI_DEV_KEYBOARD, UI_DEV_PAD };

/* ---- services the menu needs from the platform ---- */
typedef struct {
    void        (*settings_changed)(void);          /* apply g_settings and save them */
    const char *(*controller_name)(int index);      /* NULL when not connected */
    const char *(*binding_name)(int action);        /* "X", "RB", "VIEW"... for buttons 1-6, start, coin */
    void        (*start_capture)(int action);       /* remap: wait for the next button press */
    int         (*capturing)(void);                 /* index being captured, or -1 */
    void        (*reset_bindings)(void);
    int         (*get_deadzone)(void);              /* percent */
    void        (*set_deadzone)(int percent);
    void        (*operator_menu)(void);
    void        (*restart)(void);
    void        (*quit)(void);
    int         (*monitor_count)(void);
    const char *(*monitor_name)(int index);
    int         (*gpu_count)(void);
    const char *(*gpu_name)(int index);
} UiHost;

void ui_init(const UiHost *host);
void ui_menu_toggle(int device);
int  ui_menu_visible(void);      /* drawn (including the closing animation) */
int  ui_menu_wants_pause(void);  /* the game should stay paused */
void ui_key(int key, int device);
void ui_toast(const char *msg);
void ui_update(float dt);
void ui_draw(float vw, float vh, float fps);
float ui_backdrop(void);         /* 0..1: how much to darken and blur the game behind the menu */
