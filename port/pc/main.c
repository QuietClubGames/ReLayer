/*
 * Windows entry point (headless for now): load the ROM images, place the game executable in RAM the way
 * the System 12 loader does, and run the recompiled code.
 *
 *   fgtlayer.exe [--frames N] [--rom DIR] [--headless] [--fullscreen] [--wav out.wav] [--mute] [--script file]
 *                [--native] [--render-scale N] [--affine]
 *
 * Writes fgtlayer.log (everything the runtime notices), vram.bmp (GPU memory at exit) and
 * screen.bmp (the last displayed frame).
 */
#include "../core/recomp.h"
#include "../core/hw.h"
#include "../core/gpu.h"
#include "../core/log.h"
#include "video.h"
#include "audio.h"
#include "script.h"
#include "pad.h"
#include "menu_host.h"
#include "watchdog.h"
#include "../ui/ui.h"
#include "../ui/settings.h"
#include "game.h"
#include "../core/gpu_hd.h"
#include "../core/s12sound.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void f_801F0000(void);

static const char *g_rom_dir = "baserom";
static long g_max_frames = 0;
static int g_headless;
static uint32_t g_screen[640 * 512];
static int g_screen_w = 320, g_screen_h = 240;

/* Convert the GPU display area to RGB32 */
static void grab_display(void) {
    GpuDisplay d = gpu_display();
    int w = d.w > 640 ? 640 : d.w, h = d.h > 512 ? 512 : d.h;
    g_screen_w = w; g_screen_h = h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t rgb = 0;
            if (d.enabled) {
                uint16_t p = g_vram[((d.y + y) & 1023) * 1024 + ((d.x + x) & 1023)];
                rgb = ((uint32_t)((p & 31) << 3) << 16) | ((uint32_t)(((p >> 5) & 31) << 3) << 8) | (uint32_t)(((p >> 10) & 31) << 3);
            }
            g_screen[y * w + x] = rgb;
        }
}

static uint8_t *load_file(const char *name, uint32_t *size_out) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_rom_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("Cannot open %s. The game's files are made from your fgtlayer.zip by Setup "
                     "(or by tools/extract_rom.py when building it yourself).", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "cannot read %s\n", path); exit(1); }
    fclose(f);
    if (size_out) *size_out = (uint32_t)n;
    return buf;
}

static void write_rgb_bmp(const char *path, const uint32_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t data = (uint32_t)(w * h * 3), file = 54 + data;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &file, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &data, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--)
        for (int x = 0; x < w; x++) {
            uint32_t p = rgb[y * w + x];
            uint8_t bgr[3] = {(uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16)};
            fwrite(bgr, 1, 3, f);
        }
    fclose(f);
}

static void write_vram_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int w = 1024, h = 1024;
    uint32_t data = (uint32_t)(w * h * 3), file = 54 + data;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &file, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &data, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--)
        for (int x = 0; x < w; x++) {
            uint16_t p = g_vram[y * 1024 + x];
            uint8_t bgr[3] = {(uint8_t)(((p >> 10) & 31) << 3), (uint8_t)(((p >> 5) & 31) << 3), (uint8_t)((p & 31) << 3)};
            fwrite(bgr, 1, 3, f);
        }
    fclose(f);
}

float hd_d3d_gpu_ms(void);
int hd_d3d_scale(void);

#ifdef FGT_MEMWATCH
/* FGT_MEMWATCH=<hex lo>-<hex hi> (physical RAM addresses): log each distinct caller (ra) that reads there */
uint32_t g_memwatch_lo = 0xFFFFFFFFu, g_memwatch_hi = 0;
void memwatch_hit(uint32_t addr) {
    static uint32_t seen[512];
    static int n;
    uint32_t ra = R(31);
    for (int i = 0; i < n; i++) if (seen[i] == ra) return;
    if (n < 512) seen[n++] = ra;
    rt_log("memwatch: read %08X, ra %08X, frame %llu", addr, ra, (unsigned long long)g_frame);
}
static void memwatch_init(void) {
    const char *e = getenv("FGT_MEMWATCH");
    unsigned lo, hi;
    if (e && sscanf(e, "%x-%x", &lo, &hi) == 2) { g_memwatch_lo = lo & 0x1FFFFFFFu; g_memwatch_hi = hi & 0x1FFFFFFFu; }
}
#endif

/* Ask laptop drivers for the dedicated (NVIDIA / AMD) GPU instead of the integrated one. */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

void plat_fatal_message(const char *msg) {
    /* started from Explorer or a shortcut there is no console to print to, so show it */
    if (!g_headless && !GetConsoleWindow())
        MessageBoxA(NULL, msg, "ReLayer", MB_OK | MB_ICONERROR);
}

/* Installed copies are started from a shortcut or Explorer: find the game's files next to the exe when the
 * current folder doesn't have them, and accept either baserom/ (developer layout) or rom/ (installed). */
static void find_game_files(int rom_given) {
    if (rom_given) return;
    if (GetFileAttributesA("baserom/main.bin") == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesA("rom/main.bin") == INVALID_FILE_ATTRIBUTES) {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        while (n > 0 && exe[n - 1] != '\\' && exe[n - 1] != '/') n--;
        if (n > 0) { exe[n] = 0; SetCurrentDirectoryA(exe); }
    }
    if (GetFileAttributesA("baserom/main.bin") == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesA("rom/main.bin") != INVALID_FILE_ATTRIBUTES)
        g_rom_dir = "rom";
}

void plat_local_time(int *y, int *mo, int *d, int *wd, int *h, int *mi, int *s) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    *y = t.wYear; *mo = t.wMonth; *d = t.wDay; *wd = t.wDayOfWeek; *h = t.wHour; *mi = t.wMinute; *s = t.wSecond;
}

static int g_mute;
static int g_start_fullscreen;
static int g_keep_eeprom;
static int g_cli_scale;          /* --render-scale N (testing) */
static const char *g_wav_path;

/* Keyboard -> cabinet (Xbox controllers are added by pad_apply). P1: arrows, Z X C, A S D, 1 = start, 5 = coin.
 * P2: I J K L, Q W E, 2 = start, 6 = coin. F2 = test button, F1 held = service DIP. */
/* Coins: a real coin closes the switch for a few hundredths of a second, and the game treats a switch held
 * much longer as a jammed coin and ignores it. So a press of the coin key / Back button becomes one short
 * pulse, however long it is held (like MAME's PORT_IMPULSE). Updated once per frame in coin_update(). */
#define COIN_PULSE_FRAMES 4
static int g_coin_pulse[2];

static int g_pad_coin_held[2];

/* Keyboard coins count when the key goes down. Controller coins count when the button is released, unless
 * Start was pressed during the hold: coin + start together opens the menu instead. */
static void coin_update(void) {
    static int key_before[2], pad_before[2], start_seen[2];
    S12Inputs raw;
    memset(&raw, 0, sizeof raw);
    pad_apply(&raw);
    for (int i = 0; i < 2; i++) {
        int pulse = 0;
        int key = !g_headless && video_key(i ? '6' : '5');
        if (key && !key_before[i]) pulse = 1;
        key_before[i] = key;
        int pc = raw.coin[i], ps = (raw.player[i] & 0x80) != 0;
        if (pc) {
            if (!pad_before[i]) start_seen[i] = 0;
            if (ps) start_seen[i] = 1;
        } else if (pad_before[i] && !start_seen[i] && !ui_menu_wants_pause()) {
            pulse = 1;
        }
        pad_before[i] = pc;
        g_pad_coin_held[i] = pc;
        if (pulse) g_coin_pulse[i] = COIN_PULSE_FRAMES;
        else if (g_coin_pulse[i] > 0) g_coin_pulse[i]--;
    }
}

void plat_read_inputs(S12Inputs *in) {
    int blocked = !g_headless && menu_host_input_blocked();     /* menu open, or its button still held */
    if (!blocked) pad_apply(in);
    for (int i = 0; i < 2; i++) if (g_pad_coin_held[i]) in->player[i] &= (uint16_t)~0x80;   /* coin+start = menu */
    in->coin[0] = in->coin[1] = 0;              /* keyboard/controller coins arrive as the pulse below */
    script_inputs((long)g_frame, in);           /* scripted coins are already short presses */
    if (g_coin_pulse[0]) in->coin[0] = 1;
    if (g_coin_pulse[1]) in->coin[1] = 1;
    if (g_test_pulse > 0) in->test = 1;
    if (g_headless || blocked) return;
    static const struct { int vk; int pl; uint16_t bit; } map[] = {
        {VK_UP, 0, 0x20}, {VK_DOWN, 0, 0x10}, {VK_LEFT, 0, 0x08}, {VK_RIGHT, 0, 0x04},
        {'Z', 0, 0x02}, {'X', 0, 0x01}, {'C', 0, 0x8000}, {'A', 0, 0x4000}, {'S', 0, 0x2000}, {'D', 0, 0x1000},
        {'1', 0, 0x80}, {'9', 0, 0x40},
        {'I', 1, 0x20}, {'K', 1, 0x10}, {'J', 1, 0x08}, {'L', 1, 0x04},
        {'Q', 1, 0x02}, {'W', 1, 0x01}, {'E', 1, 0x8000}, {'R', 1, 0x4000}, {'T', 1, 0x2000}, {'Y', 1, 0x1000},
        {'2', 1, 0x80},
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (video_key(map[i].vk)) in->player[map[i].pl] |= map[i].bit;
    if (video_key(VK_F2)) in->test = 1;
    if (video_key(VK_F1)) in->service_dip = 1;
}

static void at_exit(void) {
    wav_close();
    eeprom_flush();
    if (g_headless) {                 /* debugging aids for test runs; players don't need them */
        write_vram_bmp("vram.bmp");
        write_rgb_bmp("screen.bmp", g_screen, g_screen_w, g_screen_h);
    }
    rt_log("exit after %llu frames", (unsigned long long)g_frame);
}

/* Audio, once per frame. The display sets the pace, so the sound card's clock can drift against it.
 * Rate control: each frame's audio is stretched or squeezed by a fraction of a percent so that about three
 * buffers stay queued. The pitch change is far below what anyone can hear, and the card never runs dry. */
static void audio_frame(void) {
    static int16_t raw[2048 * 2], dec[1024 * 2], out[800 * 2];
    int n = s12sound_audio_take_raw(raw, 2048);
    int m = n / 2;                                  /* 88.2 kHz -> 44.1 kHz: average pairs */
    for (int i = 0; i < m; i++) {
        dec[i * 2] = (int16_t)((raw[i * 4] + raw[i * 4 + 2]) / 2);
        dec[i * 2 + 1] = (int16_t)((raw[i * 4 + 1] + raw[i * 4 + 3]) / 2);
    }
    wav_write(dec, m);                              /* recordings are exact, no rate control */
    if (g_mute || g_headless || m == 0) return;
    if (g_volume < 100) {                           /* volume from the menu (perceptual curve) */
        int v = g_volume * g_volume / 100;
        for (int i = 0; i < m * 2; i++) dec[i] = (int16_t)(dec[i] * v / 100);
    }
    int q = audio_queued();
    if (q < 0) return;
    if (q == 0) {                                   /* ran dry (start-up, or a stall): add a cushion */
        static const int16_t silence[735 * 2];
        audio_push(silence, 735);
        audio_push(silence, 735);
    }
    int target = m - (q - 3) * 3;                   /* fewer samples when too much is queued, more when too little */
    if (target < m - 12) target = m - 12;
    if (target > m + 12) target = m + 12;
    if (target > 800) target = 800;
    for (int i = 0; i < target; i++) {              /* linear resample m -> target frames */
        int64_t pos = (int64_t)i * (m - 1) * 65536 / (target > 1 ? target - 1 : 1);
        int k = (int)(pos >> 16), f = (int)(pos & 0xFFFF);
        int k2 = k + 1 < m ? k + 1 : k;
        out[i * 2] = (int16_t)(dec[k * 2] + (((dec[k2 * 2] - dec[k * 2]) * f) >> 16));
        out[i * 2 + 1] = (int16_t)(dec[k * 2 + 1] + (((dec[k2 * 2 + 1] - dec[k * 2 + 1]) * f) >> 16));
    }
    audio_push(out, target);
}

/* frame work-time statistics (time spent emulating, excluding the wait for the next frame) */
static LARGE_INTEGER g_work_start;
static double g_work_max, g_work_sum;
static int g_work_over;
static double g_gap_max, g_gap_min = 1e9;   /* time between displayed frames */
static int g_late;                          /* frames that missed a display refresh */
static int g_hd_frames;                     /* frames drawn by the high-resolution renderer */
static const char *g_pending_hd_shot;
int hd_d3d_save(const char *path);
extern int g_hd_scale_override, g_hd_affine;
static double g_t_audio, g_t_present;
float g_ui_fps = 60;
void d3d_request_backbuffer_save(const char *path);
static void ui_demo(const char *prefix);

/* Opt out of Windows 10/11 power throttling ("efficiency mode"), which runs the CPU slowly for processes it
 * thinks are idle; our frames are mostly waiting, so it kicks in and makes some frames miss the display. */
static void disable_power_throttling(void) {
    typedef struct { ULONG Version, ControlMask, StateMask; } PowerThrottle;
    typedef BOOL (WINAPI *SetProcInfoFn)(HANDLE, int, LPVOID, DWORD);
    SetProcInfoFn fn = (SetProcInfoFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    if (!fn) return;
    PowerThrottle pt = {1, 0x1 /* EXECUTION_SPEED */, 0 /* off */};
    BOOL ok = fn(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &pt, sizeof pt);
    rt_log("power throttling opt-out: %s", ok ? "ok" : "not available");
}

/* FGT_STRESS=<start frame>:<steps>: while the game runs, switch display modes and open the menu over and over
 * (one action every few frames, keys sent the way the keyboard sends them). Exits when done. */
static void stress_tick(void) {
    static int checked, start, steps, step, wait, phase;
    static unsigned seed = 12345, act;
    static long t;
    if (!checked) {
        const char *e = getenv("FGT_STRESS");
        checked = 1;
        if (!e || sscanf(e, "%d:%d", &start, &steps) != 2) steps = 0;
    }
    if (!steps || (long)g_frame < start) return;
    {   /* FGT_STRESS_KEYS=k1,k2,...: one key every 20 frames (hex virtual-key codes) */
        static const char *e;
        static int checked2;
        if (!checked2) { checked2 = 1; e = getenv("FGT_STRESS_KEYS"); }
        if (e) {
            if (++t < 20) return;
            t = 0;
            if (!*e) return;
            int vk = (int)strtol(e, (char **)&e, 16);
            if (*e == ',') e++;
            rt_log("stress key %02X at frame %llu", vk, (unsigned long long)g_frame);
            video_inject_key(vk);
            return;
        }
    }
    {   /* FGT_STRESS_COMBOS: every display mode x graphics x quality x filter x shape, set directly, 20 frames each,
         * with the window minimised and restored at every display mode */
        static int combos = -1, c;
        if (combos < 0) combos = getenv("FGT_STRESS_COMBOS") != NULL;
        if (combos) {
            if (++t < wait) return;
            t = 0;
            wait = 20;
            int total = 3 * 7 * 3 * 3;
            if (getenv("FGT_STRESS_ALTTAB") && c < total) c = total + 6;   /* only the focus test */
            if (c >= total + 6 && c < total + 6 + 20) {   /* exclusive full screen, focus away and back 10 times */
                if (c == total + 6) { g_settings.display_mode = DISPLAY_EXCLUSIVE; menu_host_apply(); }
                video_test_defocus((c - total) % 2 == 0);
                rt_log("stress: focus %s", (c - total) % 2 == 0 ? "away" : "back");
                c++;
                wait = 60;
                return;
            }
            if (c == total + 26) { rt_log("stress done"); exit(0); }
            if (c >= total) {              /* finally: minimise and restore, in each display mode */
                int m = (c - total) / 2;
                if ((c - total) % 2 == 0) { g_settings.display_mode = m; menu_host_apply(); video_test_minimize(1); wait = 90; }
                else { video_test_minimize(0); wait = 90; }
                rt_log("stress: display %d, %s", m, (c - total) % 2 ? "restored" : "minimised");
                c++;
                return;
            }
            int v = c++;
            g_settings.aspect = v % 3; v /= 3;
            g_settings.filter = v % 3; v /= 3;
            int q = v % 7; v /= 7;         /* 0-5 quality with enhanced graphics, 6 = original graphics */
            g_settings.enhanced = q < 6;
            if (q < 6) g_settings.quality = q;
            g_settings.display_mode = v;
            menu_host_apply();
            if (c % 9 == 1) rt_log("stress combo %d/%d at frame %llu", c, total, (unsigned long long)g_frame);
            return;
        }
    }
    {   /* FGT_STRESS_SWEEP: instead of random actions, every option of the Display, Audio, Controls and Game pages,
         * one at a time: open the menu, step through its values (or toggle it twice), close, let the fight run */
        static int sweep = -1, n, k;
        static int keys[4000][2];                  /* key, frames to wait after it */
        if (sweep < 0) {
            sweep = getenv("FGT_STRESS_SWEEP") != NULL;
            if (sweep) {
#define K(v, w) do { if (n < 4000) { keys[n][0] = (v); keys[n][1] = (w); n++; } } while (0)
                static const int rows[4] = {10, 2, 11, 11};
                for (int pg = 0; pg < 4; pg++)
                    for (int r = 0; r < rows[pg] + 1; r++) {
                        K(-1, 10);
                        if (r) K(VK_DOWN, 4);
                        for (int i = 0; i < 6; i++) K(VK_RIGHT, 12);        /* every value of a choice / slider */
                        K(VK_RETURN, 12);                                   /* toggles, and a button capture... */
                        K(-3, 8);                                           /* ...cancelled */
                        K(VK_RETURN, 12);
                        K(-3, 8);
                        for (int i = 0; i < 3; i++) K(VK_LEFT, 12);
                        K(-2, 45);                                          /* close, the fight runs */
                        if (r == rows[pg]) { K(-1, 10); K('E', 8); K(-2, 30); }   /* next page */
                    }
#undef K
                rt_log("stress sweep: %d key presses", n);
            }
        }
        if (sweep) {
            if (++t < wait) return;
            t = 0;
            if (k == n) { rt_log("stress done"); exit(0); }
            if (k % 20 == 0) rt_log("stress sweep %d/%d at frame %llu", k, n, (unsigned long long)g_frame);
            int v = keys[k][0];                    /* -1 open, -2 close, -3 cancel a button capture (if any) */
            if (v == -1) v = ui_menu_wants_pause() ? 0 : VK_TAB;
            else if (v == -2) v = ui_menu_wants_pause() ? VK_TAB : 0;
            else if (v == -3) v = menu_host_capturing() ? VK_ESCAPE : 0;
            if (v) video_inject_key(v);
            wait = keys[k][1];
            k++;
            return;
        }
    }
    if (++t < wait) return;
    t = 0;
    if (phase == 0) {
        if (step == steps) { rt_log("stress done"); exit(0); }
        seed = seed * 1103515245u + 12345u;
        act = (seed >> 16) % 5;
        wait = 1 + (int)((seed >> 8) % 40);
        rt_log("stress %d at frame %llu: %s, %d frames apart", step, (unsigned long long)g_frame,
               (const char *[]){"F11", "menu open/close", "menu display mode", "menu open + F11", "F9"}[act], wait);
        step++;
    }
    static const int seq[5][4] = {{VK_F11}, {VK_TAB, VK_TAB}, {VK_TAB, VK_RIGHT, VK_TAB}, {VK_TAB, VK_F11, VK_TAB}, {VK_F9}};
    int vk = phase < 4 ? seq[act][phase] : 0;
    if (vk) { video_inject_key(vk); phase++; }
    if (!vk || phase == 4 || !seq[act][phase]) phase = 0;
}

void plat_vblank(void) {
    {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (g_work_start.QuadPart) {
            double ms = (double)(now.QuadPart - g_work_start.QuadPart) * 1000.0 / (double)freq.QuadPart;
            if (ms > g_work_max) g_work_max = ms;
            g_work_sum += ms;
            if (ms > 16.0) g_work_over++;
        }

    }
    if (g_frame % 60 == 0) {
        /* real (wall-clock) frame rate over the last 60 frames */
        static LARGE_INTEGER last, freq;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        double fps = last.QuadPart ? 60.0 * (double)freq.QuadPart / (double)(now.QuadPart - last.QuadPart) : 0;
        last = now;
        rt_log("-- frame %llu -- %.1f fps, work avg %.1f ms, worst %.1f ms, %d frames over 16 ms, high-res %d/60, frame gap %.1f-%.1f ms (%d late), repeated frames on screen %d, audio queue %d, slowest audio %.1f ms, present %.1f ms, GPU %.2f ms at %dx",
               (unsigned long long)g_frame, fps, g_work_sum / 60.0, g_work_max, g_work_over, g_hd_frames, g_gap_min, g_gap_max, g_late, g_headless ? -1 : video_take_repeats(), audio_queued(), g_t_audio, g_t_present,
               g_headless ? 0.0f : hd_d3d_gpu_ms(), g_headless ? 0 : hd_d3d_scale());
        g_late = 0; g_t_audio = g_t_present = 0; g_hd_frames = 0;
        g_gap_max = 0; g_gap_min = 1e9;
        g_work_max = g_work_sum = 0;
        g_work_over = 0;
    }
    grab_display();
    pad_poll();
    coin_update();
    if (g_test_pulse > 0) g_test_pulse--;
    {   /* debug: FGT_RAM_DUMP=<frame>:<path> writes the 4 MB RAM image at that frame */
        static long dump_frame = -2;
        static char dump_path[260];
        if (dump_frame == -2) {
            const char *e = getenv("FGT_RAM_DUMP");
            dump_frame = -1;
            if (e && sscanf(e, "%ld:%259s", &dump_frame, dump_path) != 2) dump_frame = -1;
        }
        if (dump_frame >= 0 && (long)g_frame == dump_frame) {
            FILE *f = fopen(dump_path, "wb");
            if (f) { fwrite(g_ram, 1, RAM_SIZE, f); fclose(f); rt_log("RAM dumped to %s", dump_path); }
        }
    }
    if (!g_headless) {
        char st[160];
        snprintf(st, sizeof st, "frame %llu, menu %s, display %d, enhanced %d, quality %d, filter %d",
                 (unsigned long long)g_frame, ui_menu_visible() ? "open" : "closed", g_settings.display_mode,
                 g_settings.enhanced, g_settings.quality, g_settings.filter);
        watchdog_note(st);
        {   /* FGT_TEST_HANG=<frame>: stall for 5 seconds there (checks the watchdog) */
            static long hang = -2;
            if (hang == -2) hang = getenv("FGT_TEST_HANG") ? atol(getenv("FGT_TEST_HANG")) : -1;
            if ((long)g_frame == hang) Sleep(5000);
        }
        stress_tick();
        menu_host_frame(1.0f / 60.0f);
        static const char *demo;
        static int demo_checked;
        if (!demo_checked) { demo_checked = 1; demo = getenv("FGT_UI_DEMO"); }
        if (demo && g_frame == 2400) ui_demo(demo);
    }
    {   /* FGT_WATCH=<hex address>[,2]: log every change of that RAM byte (or 16-bit value) (finding game variables) */
        static int checked, wide;
        static uint32_t watch;
        static int last = -1;
        if (!checked) {
            const char *e = getenv("FGT_WATCH");
            checked = 1;
            if (e) { char *end; watch = (uint32_t)strtoul(e, &end, 16); wide = end[0] == ',' && end[1] == '2'; }
        }
        if (watch) {
            int v = wide ? MEM_R16(watch) : MEM_R8(watch);
            if (v != last) { rt_log("watch %08X: %d -> %d at frame %llu", watch, last, v, (unsigned long long)g_frame); last = v; }
        }
    }
    for (int i = 0; ; i++) {                      /* scripted "set name=value" (arcade settings) */
        const char *set = script_set((long)g_frame, i);
        if (!set) break;
        char name[48];
        int value;
        unsigned addr;
        if (set[0] == '@' && sscanf(set + 1, "%x=%d", &addr, &value) == 2) {      /* poke */
            MEM_W8(addr, (uint8_t)value);
            rt_log("script: poke %08X = %d", addr, value);
        } else if (set[0] == '!') {                                               /* vram dump */
            char path[300];
            snprintf(path, sizeof path, "%s.vram", set + 1);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(g_vram, sizeof g_vram, 1, f); fclose(f); }
            snprintf(path, sizeof path, "%s.ram", set + 1);                   /* and main RAM */
            f = fopen(path, "wb");
            if (f) { fwrite(g_ram, RAM_SIZE, 1, f); fclose(f); }
            rt_log("script: vram and ram saved as %s.vram/.ram", set + 1);
        } else if (sscanf(set, "%47[^=]=%d", name, &value) == 2 && arcade_setting_find(name) >= 0) {
            arcade_setting_set(arcade_setting_find(name), value);
            rt_log("script: %s = %d", name, value);
        } else rt_log("script: bad setting '%s'", set);
    }
    {
        const char *shot = script_shot((long)g_frame);
        if (shot) {
            char path[300];
            snprintf(path, sizeof path, "%s.bmp", shot);
            write_rgb_bmp(path, g_screen, g_screen_w, g_screen_h);
            rt_log("screenshot %s at frame %llu", path, (unsigned long long)g_frame);
            g_pending_hd_shot = shot;      /* saved after this frame is drawn */
        }
    }
    LARGE_INTEGER t0, t1, t2, t3, qf;
    QueryPerformanceCounter(&t0);
    audio_frame();
    QueryPerformanceCounter(&t1);
    if (!g_headless) {
        static char bb_path[300];
        if (g_pending_hd_shot) {            /* also what reaches the window (filters, menu): <name>_screen.bmp */
            snprintf(bb_path, sizeof bb_path, "%s_screen.bmp", g_pending_hd_shot);
            d3d_request_backbuffer_save(bb_path);
        }
        if (!video_present(g_screen, g_screen_w, g_screen_h, gpu_hd_frame())) exit(0);
        if (video_hd_used()) g_hd_frames++;
        if (g_pending_hd_shot) {
            char path[300];
            snprintf(path, sizeof path, "%s_hd.bmp", g_pending_hd_shot);
            if (video_hd_used() && hd_d3d_save(path)) rt_log("screenshot %s (high resolution)", path);
            g_pending_hd_shot = NULL;
        }
        QueryPerformanceCounter(&t2);
        video_wait_frame();
        QueryPerformanceCounter(&t3);
        QueryPerformanceFrequency(&qf);
        double k = 1000.0 / (double)qf.QuadPart;
        double ta = (double)(t1.QuadPart - t0.QuadPart) * k, tp = (double)(t2.QuadPart - t1.QuadPart) * k;
        if (ta > g_t_audio) g_t_audio = ta;
        if (tp > g_t_present) g_t_present = tp;
        /* time between frames reaching the screen */
        static LARGE_INTEGER prev;
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (prev.QuadPart) {
            double gap = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)freq.QuadPart;
            if (gap > g_gap_max) g_gap_max = gap;
            if (gap < g_gap_min) g_gap_min = gap;
            if (gap > 20.0) g_late++;
        }
        prev = now;
        {   /* frame rate shown by the overlay counter (smoothed) */
            static LARGE_INTEGER last_fps;
            if (last_fps.QuadPart) {
                double ms = (double)(now.QuadPart - last_fps.QuadPart) * 1000.0 / (double)freq.QuadPart;
                if (ms > 0) g_ui_fps = g_ui_fps * 0.95f + (float)(1000.0 / ms) * 0.05f;
            }
            last_fps = now;
        }
        /* menu open: the game stays paused here, the frozen frame keeps being shown under the menu */
        while (ui_menu_wants_pause()) {
            pad_poll();
            stress_tick();
            menu_host_frame(1.0f / 60.0f);
            if (!video_present(g_screen, g_screen_w, g_screen_h, gpu_hd_frame())) exit(0);
            video_wait_frame();
        }
    }
    QueryPerformanceCounter(&g_work_start);
    if (g_max_frames > 0 && (long)g_frame >= g_max_frames) {
        rt_log("frame limit reached");
        exit(0);
    }
}

int main(int argc, char **argv) {
    {
        int rom_given = 0;
        for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--rom")) rom_given = 1;
        find_game_files(rom_given);
    }
#ifdef FGT_MEMWATCH
    memwatch_init();
#endif
    settings_defaults(&g_settings);
    settings_load(&g_settings, "settings.ini");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--rom") && i + 1 < argc) g_rom_dir = argv[++i];
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--fullscreen")) g_start_fullscreen = 1;
        else if (!strcmp(argv[i], "--keep-eeprom")) g_keep_eeprom = 1;
        else if (!strcmp(argv[i], "--native")) g_settings.enhanced = 0;
        else if (!strcmp(argv[i], "--affine")) g_settings.perspective = 0;
        else if (!strcmp(argv[i], "--render-scale") && i + 1 < argc) { extern int g_hd_scale_override; g_hd_scale_override = atoi(argv[++i]); g_cli_scale = g_hd_scale_override; }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) g_wav_path = argv[++i];
        else if (!strcmp(argv[i], "--mute")) g_mute = 1;
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) { if (!script_load(argv[++i])) return 1; }
    }
    /* Scripted / headless runs always start from a blank EEPROM (first power-on) and never overwrite the
     * player's eeprom.bin. --keep-eeprom opts back in. */
    if (g_headless && !g_keep_eeprom) g_eeprom_volatile = 1;
    uint32_t main_size;
    g_prg_rom = load_file("prg.bin", NULL);
    g_banked_rom = load_file("banked.bin", &g_banked_size);
    uint8_t *mainbin = load_file("main.bin", &main_size);

    /* sound / I-O board: H8 program and C352 sample ROM */
    uint32_t snd_size, wave_size;
    uint8_t *snd = load_file("sound.bin", &snd_size);
    uint8_t *wave = load_file("wave.bin", &wave_size);
    s12sound_init(snd, snd_size, wave, wave_size);
    free(snd);

    /* System 12 loader: copy the game image from ROM 0x1FC28000 to RAM 0x801F0000 */
    memcpy(g_ram + 0x1F0000, mainbin, main_size);
    free(mainbin);

    atexit(at_exit);
    if (!g_headless) {
        if (g_start_fullscreen) g_settings.display_mode = DISPLAY_BORDERLESS;   /* --fullscreen overrides settings.ini */
        g_gpu_choice = g_settings.gpu;              /* the graphics card is picked once, at start-up */
        g_aspect_mode = g_settings.aspect;
        video_init("ReLayer", 960, 720);
        /* keep background programs from stealing time slices mid-frame */
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        disable_power_throttling();
    }
    if (!g_headless && !g_mute) audio_init(S12SND_OUT_RATE);
    if (!g_headless || getenv("FGT_FAKE_PAD")) pad_init("controls.ini");
    if (!g_headless) { watchdog_start(); menu_host_init(); }          /* applies the settings (full screen, graphics, volume...) */
    g_unlock_all = g_settings.unlock_all;
    if (g_cli_scale) { extern int g_hd_scale_override; g_hd_scale_override = g_cli_scale; }
    if (g_wav_path) wav_open(g_wav_path, S12SND_OUT_RATE);
    hw_init();
    cpu_set_sr(0x40000000u);       /* COP2 enabled, interrupts off */
    R(29) = 0x801FFFF0u;
    R(31) = 0;
    rt_log("starting game at 801F0000");
    f_801F0000();
    rt_log("game returned to the loader");
    return 0;
}

/* ---- FGT_UI_DEMO=<prefix>: open the menu mid-fight, step through every page and save screenshots ---- */
void menu_host_cancel_capture(void);
static void demo_frames(int n) {
    for (int i = 0; i < n; i++) {
        pad_poll();
        menu_host_frame(1.0f / 60.0f);
        if (!video_present(g_screen, g_screen_w, g_screen_h, gpu_hd_frame())) exit(0);
        video_wait_frame();
    }
}
static void demo_shot(const char *prefix, const char *name) {
    static char path[8][200];
    static int n;
    char *p = path[n++ & 7];
    snprintf(p, 200, "%s_%s.bmp", prefix, name);
    d3d_request_backbuffer_save(p);
    demo_frames(1);
}
static void ui_demo(const char *prefix) {
    Settings saved = g_settings;
    if (getenv("FGT_UI_DEMO_BORDERLESS")) {        /* the menu's own way of switching to borderless */
        ui_menu_toggle(UI_DEV_PAD);
        demo_frames(30);
        ui_key(UI_RIGHT, UI_DEV_PAD);               /* Display mode: Window -> Borderless */
        demo_frames(60);
        demo_shot(prefix, "b1_menu_borderless");
        ui_key(UI_CLOSE, UI_DEV_PAD);
        demo_frames(90);
        demo_shot(prefix, "b2_game_borderless");
        g_settings = saved;
        settings_save(&g_settings, "settings.ini");
        rt_log("ui demo done");
        exit(0);
    }
    if (getenv("FGT_UI_DEMO_MENU")) {              /* just the menu open over the game: one picture per page */
        ui_menu_toggle(UI_DEV_PAD);
        demo_frames(45);
        demo_shot(prefix, "menu_display");
        for (int k = 0; k < 4; k++) {
            static const char *pages[] = {"menu_audio", "menu_controls", "menu_game", "menu_system"};
            ui_key(UI_NEXT_TAB, UI_DEV_PAD);
            demo_frames(30);
            demo_shot(prefix, pages[k]);
        }
        g_settings = saved;
        settings_save(&g_settings, "settings.ini");
        rt_log("ui demo done");
        exit(0);
    }
    if (getenv("FGT_UI_DEMO_FILTER")) {            /* the menu's screen filter: Sharp -> Smooth -> Scanlines -> Sharp */
        static const char *names[] = {"f1_smooth", "f2_scanlines", "f3_sharp"};
        if (getenv("FGT_UI_DEMO_FILTER")[0] == 'b') { video_apply_display(DISPLAY_BORDERLESS, saved.window_size, 0); demo_frames(90); }
        if (strchr(getenv("FGT_UI_DEMO_FILTER"), 'x')) { video_apply_display(DISPLAY_EXCLUSIVE, saved.window_size, 0); demo_frames(120); }
        if (strchr(getenv("FGT_UI_DEMO_FILTER"), 'o')) { extern int g_hd_enabled; g_settings.enhanced = 0; g_hd_enabled = 0; demo_frames(30); }
        for (int k = 0; k < 3; k++) {
            ui_menu_toggle(UI_DEV_PAD);
            demo_frames(30);
            if (k == 0) for (int i = 0; i < 7; i++) { ui_key(UI_DOWN, UI_DEV_PAD); demo_frames(3); }   /* Screen filter (the menu
                                                                                                    * keeps its row) */
            ui_key(UI_RIGHT, UI_DEV_PAD);
            demo_frames(10);
            ui_key(UI_CLOSE, UI_DEV_PAD);
            demo_frames(60);
            demo_shot(prefix, names[k]);
        }
        g_settings = saved;
        settings_save(&g_settings, "settings.ini");
        rt_log("ui demo done");
        exit(0);
    }
    ui_toast("Controller 1 connected");
    demo_frames(30);
    demo_shot(prefix, "0_toast");
    ui_menu_toggle(UI_DEV_PAD);
    demo_frames(40);
    demo_shot(prefix, "1_display");
    for (int i = 0; i < 9; i++) { ui_key(UI_DOWN, UI_DEV_PAD); demo_frames(3); }   /* frame rate counter */
    ui_key(UI_OK, UI_DEV_PAD);
    demo_frames(30);
    demo_shot(prefix, "2_display_fps");
    ui_key(UI_NEXT_TAB, UI_DEV_PAD);
    demo_frames(30);
    demo_shot(prefix, "3_audio");
    ui_key(UI_NEXT_TAB, UI_DEV_PAD);
    for (int i = 0; i < 2; i++) { ui_key(UI_DOWN, UI_DEV_PAD); demo_frames(4); }
    ui_key(UI_OK, UI_DEV_PAD);
    demo_frames(30);
    demo_shot(prefix, "4_controls_capture");
    menu_host_cancel_capture();
    ui_key(UI_NEXT_TAB, UI_DEV_PAD);
    demo_frames(30);
    demo_shot(prefix, "5_game");
    ui_key(UI_NEXT_TAB, UI_DEV_KEYBOARD);
    demo_frames(30);
    demo_shot(prefix, "6_system_keyboard");
    ui_key(UI_CLOSE, UI_DEV_KEYBOARD);
    demo_frames(30);
    /* display modes and picture shapes, without the menu */
    video_apply_display(DISPLAY_BORDERLESS, saved.window_size, 0);
    demo_frames(60);
    demo_shot(prefix, "7_borderless");
    g_aspect_mode = ASPECT_INTEGER;
    demo_frames(10);
    demo_shot(prefix, "8_borderless_integer");
    g_aspect_mode = ASPECT_STRETCH;
    demo_frames(10);
    demo_shot(prefix, "9_borderless_stretch");
    g_aspect_mode = saved.aspect;
    video_apply_display(DISPLAY_EXCLUSIVE, saved.window_size, 0);
    demo_frames(120);
    demo_shot(prefix, "10_exclusive");
    video_apply_display(DISPLAY_WINDOW, 3, 0);
    demo_frames(60);
    demo_shot(prefix, "11_window_1440x1080");
    video_apply_display(saved.display_mode, saved.window_size, saved.monitor);
    demo_frames(30);
    g_settings = saved;
    settings_save(&g_settings, "settings.ini");
    rt_log("ui demo done");
    exit(0);
}
