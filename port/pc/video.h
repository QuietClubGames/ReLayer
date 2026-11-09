#pragma once
#include <stdint.h>

int video_init(const char *title, int w, int h);
#include "../core/gpu_hd.h"
/* Show a frame: the recorded drawing (hd, redrawn at high resolution when possible) or the RGB32 software
 * picture. Returns 0 when the user closed the window. */
int video_present(const uint32_t *rgb, int w, int h, const HdFrame *hd);
int video_hd_used(void);       /* 1 if the last frame came from the high-resolution renderer */
extern int g_hd_enabled;
int video_key(int vk);
void video_pace(void);       /* wait for the next tick of a fixed 60 Hz schedule */
void video_wait_frame(void); /* wait for the next frame: display vsync at ~60 Hz, else video_pace() */
int  video_take_repeats(void); /* frames shown twice since last call (Direct3D only, else -1) */
void video_set_fullscreen(int on);  /* borderless full screen; Alt+Enter / F11 toggle it */
int  video_pop_key(void);      /* next queued key press (virtual-key code), 0 if none */
void video_inject_key(int vk);
int  video_menu_drawable(void);  /* 0: no Direct3D, the menu can't be shown */
void video_test_minimize(int on);
void video_test_defocus(int away);  /* minimise / restore the window (tests) */ /* queue a key press as if typed (tests) */
void video_apply_display(int mode, int window_size, int monitor);   /* 0 window, 1 borderless, 2 exclusive */
int  video_monitor_count(void);
const char *video_monitor_name(int index);
extern int g_gpu_choice, g_aspect_mode;
