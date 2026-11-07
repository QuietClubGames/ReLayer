#pragma once

/* Player settings, saved to settings.ini. Platform independent. */
enum { DISPLAY_WINDOW, DISPLAY_BORDERLESS, DISPLAY_EXCLUSIVE };
enum { ASPECT_ARCADE, ASPECT_STRETCH, ASPECT_INTEGER };
enum { QUALITY_AUTO, QUALITY_PERFORMANCE, QUALITY_BALANCED, QUALITY_HIGH, QUALITY_ULTRA, QUALITY_EXTREME };

typedef struct {
    int display_mode;     /* DISPLAY_* */
    int window_size;      /* index into the window sizes (640x480 steps) */
    int monitor;          /* 0 = the monitor the window is on / primary, else 1-based display number */
    int aspect;           /* ASPECT_* */
    int enhanced;         /* 1 = high-resolution renderer, 0 = original picture */
    int quality;          /* QUALITY_* (render scale; Auto adapts to keep 60 fps) */
    int perspective;      /* 1 = perspective-correct textures, 0 = PS1 warping */
    int filter;           /* 0 sharp, 1 smooth, 2 scanlines */
    int gpu;              /* 0 = automatic (fastest), else 1-based adapter number; applies after restart */
    int show_fps;
    int volume;           /* 0-100 */
    int mute;
    int unlock_all;       /* secret characters available from the start */
} Settings;

extern Settings g_settings;

void settings_defaults(Settings *s);
int  settings_load(Settings *s, const char *path);     /* 0 if the file doesn't exist */
int  settings_save(const Settings *s, const char *path);
int  settings_quality_scale(int quality);              /* render scale for a preset, 0 = auto */
