#include "settings.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

Settings g_settings;

void settings_defaults(Settings *s) {
    memset(s, 0, sizeof *s);
    s->window_size = 1;            /* 960x720 */
    s->enhanced = 1;
    s->perspective = 1;
    s->volume = 100;
    s->unlock_all = 1;
}

int settings_quality_scale(int q) {
    static const int k[] = {0, 2, 3, 4, 6, 8};
    return q >= 0 && q < 6 ? k[q] : 0;
}

static const struct { const char *key; size_t ofs; int lo, hi; } k_fields[] = {
    {"display_mode", offsetof(Settings, display_mode), 0, 2},
    {"window_size", offsetof(Settings, window_size), 0, 4},
    {"monitor", offsetof(Settings, monitor), 0, 16},
    {"picture_shape", offsetof(Settings, aspect), 0, 2},
    {"enhanced_graphics", offsetof(Settings, enhanced), 0, 1},
    {"quality", offsetof(Settings, quality), 0, 5},
    {"perspective_correct", offsetof(Settings, perspective), 0, 1},
    {"screen_filter", offsetof(Settings, filter), 0, 2},
    {"graphics_card", offsetof(Settings, gpu), 0, 16},
    {"show_fps", offsetof(Settings, show_fps), 0, 1},
    {"volume", offsetof(Settings, volume), 0, 100},
    {"mute", offsetof(Settings, mute), 0, 1},
    {"unlock_all_characters", offsetof(Settings, unlock_all), 0, 1},
};

int settings_load(Settings *s, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        int v;
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %d", key, &v) != 2) continue;
        /* keys from older versions */
        if (!strcmp(key, "fullscreen")) { s->display_mode = v ? DISPLAY_BORDERLESS : DISPLAY_WINDOW; continue; }
        if (!strcmp(key, "render_scale")) {
            s->quality = v == 0 ? QUALITY_AUTO : v <= 2 ? QUALITY_PERFORMANCE : v == 3 ? QUALITY_BALANCED
                       : v == 4 ? QUALITY_HIGH : v <= 6 ? QUALITY_ULTRA : QUALITY_EXTREME;
            continue;
        }
        for (unsigned i = 0; i < sizeof k_fields / sizeof k_fields[0]; i++)
            if (!strcmp(key, k_fields[i].key)) {
                if (v < k_fields[i].lo) v = k_fields[i].lo;
                if (v > k_fields[i].hi) v = k_fields[i].hi;
                *(int *)((char *)s + k_fields[i].ofs) = v;
            }
    }
    fclose(f);
    return 1;
}

int settings_save(const Settings *s, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# ReLayer settings (written by the in-game menu: Tab / Esc, or Back + Start on a pad)\n");
    for (unsigned i = 0; i < sizeof k_fields / sizeof k_fields[0]; i++)
        fprintf(f, "%s = %d\n", k_fields[i].key, *(const int *)((const char *)s + k_fields[i].ofs));
    fclose(f);
    return 1;
}
