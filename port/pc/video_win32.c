/* Win32 window: shows the game's display area through Direct3D 11 (video_d3d11.c), or GDI as a fallback,
 * paced to 60 Hz. */
#include "video.h"
#include "d3d11_present.h"
#include <windows.h>
#include <mmsystem.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "../core/log.h"
#include "watchdog.h"

static HWND g_wnd;
static int g_closing;
static BITMAPINFO g_bmi;
static LARGE_INTEGER g_freq, g_last;
static int g_hz, g_use_d3d;
int g_hd_enabled = 1;          /* high-resolution renderer on (F9 toggles, --native turns it off) */
int g_gpu_choice;              /* 0 = automatic, else 1-based adapter (read at start-up) */
int g_aspect_mode;             /* ASPECT_*: arcade 4:3, stretch, 4:3 whole-number scale */
static int g_hd_used;          /* last frame was drawn by the high-resolution renderer */

static int display_hz(void) {
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    return EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 0;
}

/* ---- monitors ---- */
#define MAX_MONS 8
static HMONITOR g_mons[MAX_MONS];
static char g_mon_names[MAX_MONS][64];
static int g_nmons;

static BOOL CALLBACK mon_cb(HMONITOR m, HDC dc, LPRECT r, LPARAM p) {
    (void)dc; (void)p;
    if (g_nmons < MAX_MONS) {
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoA(m, &mi);
        snprintf(g_mon_names[g_nmons], sizeof g_mon_names[0], "Display %d  %ld\xC3\x97%ld%s", g_nmons + 1,
                 r->right - r->left, r->bottom - r->top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? "  (main)" : "");
        g_mons[g_nmons++] = m;
    }
    return TRUE;
}
static void list_monitors(void) { g_nmons = 0; EnumDisplayMonitors(NULL, NULL, mon_cb, 0); }
int video_monitor_count(void) { if (!g_nmons) list_monitors(); return g_nmons; }
const char *video_monitor_name(int i) { if (!g_nmons) list_monitors(); return i >= 0 && i < g_nmons ? g_mon_names[i] : "?"; }

/* ---- display mode: window / borderless / exclusive full screen ---- */
static int g_reclaim_exclusive;
static int g_mode = -1;              /* 0 window, 1 borderless, 2 exclusive */
static int g_fullscreen;             /* borderless or exclusive */
static RECT g_window_rect;
static int g_have_window_rect;

void video_apply_display(int mode, int window_size, int monitor) {
    static const int k_sizes[][2] = {{640, 480}, {960, 720}, {1280, 960}, {1600, 1200}, {1920, 1440}};
    if (!g_wnd) return;
    if (window_size < 0 || window_size > 4) window_size = 1;
    if (!g_nmons) list_monitors();
    HMONITOR mon = (monitor >= 1 && monitor <= g_nmons) ? g_mons[monitor - 1] : MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoA(mon, &mi);
    if (g_mode == 2 && mode != 2) d3d_set_exclusive(0, NULL);
    if (mode == 0) {
        int w = k_sizes[window_size][0], h = k_sizes[window_size][1];
        int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
        RECT r = {0, 0, w, h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        if (ww > aw || wh > ah) {               /* too big for this monitor: shrink, keeping 4:3 */
            float k = fminf((float)aw / (float)ww, (float)ah / (float)wh);
            ww = (int)((float)ww * k); wh = (int)((float)wh * k);
        }
        int x = mi.rcWork.left + (aw - ww) / 2, y = mi.rcWork.top + (ah - wh) / 2;
        if (g_mode == 0 && g_have_window_rect && monitor == 0) {   /* keep the user's position on resize */
            x = g_window_rect.left; y = g_window_rect.top;
        }
        SetWindowLongA(g_wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_wnd, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = 0;
        rt_log("video: window %dx%d", w, h);
    } else {
        if (g_mode == 0) { GetWindowRect(g_wnd, &g_window_rect); g_have_window_rect = 1; }
        SetWindowLongA(g_wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = 1;
        if (mode == 2) SetForegroundWindow(g_wnd);   /* DXGI only grants exclusive mode to the focused window */
        if (mode == 2 && !d3d_set_exclusive(1, mon)) {
            rt_log("video: exclusive full screen unavailable, using borderless");
            mode = 1;
        }
        rt_log("video: %s full screen %ldx%ld", mode == 2 ? "exclusive" : "borderless",
               mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
    g_mode = mode;
}

void video_set_fullscreen(int on) { video_apply_display(on ? 1 : 0, 1, 0); }

/* key presses for the menu / hotkeys (handled once per frame by menu_host.c) */
static int g_keyq[32];
static int g_keyq_n;
static void push_key(int vk) { if (g_keyq_n < 32) g_keyq[g_keyq_n++] = vk; }
void video_inject_key(int vk) { push_key(vk); }
void video_test_defocus(int away) {    /* like Alt+Tab to the desktop and back (tests) */
    if (away) SetForegroundWindow(GetShellWindow()); else { ShowWindow(g_wnd, SW_RESTORE); SetForegroundWindow(g_wnd); }
}
void video_test_minimize(int on) { ShowWindow(g_wnd, on ? SW_MINIMIZE : SW_RESTORE); if (!on) SetForegroundWindow(g_wnd); }
int video_pop_key(void) {
    if (!g_keyq_n) return 0;
    int vk = g_keyq[0];
    memmove(g_keyq, g_keyq + 1, sizeof(int) * (size_t)(--g_keyq_n));
    return vk;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: g_closing = 1; return 0;
    case WM_ACTIVATEAPP:
        /* DXGI leaves exclusive full screen when another program takes focus; take the monitor back on return */
        if (w && g_mode == 2) g_reclaim_exclusive = 1;
        break;
    case WM_KEYDOWN: push_key((int)w); return 0;          /* includes auto-repeat, handy in menus */
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) {            /* Alt+Enter */
            if (!(l & (1 << 30))) push_key(VK_F11);         /* ignore key repeat */
            return 0;
        }
        if (w == VK_F10) { push_key(VK_F10); return 0; }
        break;
    case WM_SYSCHAR:
        if (w == VK_RETURN) return 0;                        /* no "ding" for Alt+Enter */
        break;
    case WM_SETCURSOR:
        if (g_fullscreen && LOWORD(l) == HTCLIENT) { SetCursor(NULL); return TRUE; }   /* hide the mouse */
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

int video_init(const char *title, int w, int h) {

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "fgtlayer";
    RegisterClassA(&wc);
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA("fgtlayer", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    timeBeginPeriod(1);              /* 1 ms Sleep granularity (default is ~15.6 ms) */
    if (!g_wnd) return 0;
    g_hz = getenv("FGT_FORCE_HZ") ? atoi(getenv("FGT_FORCE_HZ")) : display_hz();   /* tests: act as on another display */
    g_use_d3d = getenv("FGT_FORCE_GDI") ? 0 : d3d_init(g_wnd, g_hz, g_gpu_choice);
    rt_log("video: display %d Hz, drawing with %s", g_hz, g_use_d3d ? "Direct3D 11" : "GDI");
    return 1;
}

int video_present(const uint32_t *rgb, int w, int h, const HdFrame *hd) {
    MSG msg;
    watchdog_beat();
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (g_closing) return 0;
    if (g_reclaim_exclusive) {
        g_reclaim_exclusive = 0;
        if (g_mode == 2 && g_use_d3d) d3d_set_exclusive(1, MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST));
    }
    g_hd_used = 0;
    if (g_use_d3d) {
        static long test_fail = -2, frames;
        if (test_fail == -2) test_fail = getenv("FGT_TEST_D3D_FAIL") ? atol(getenv("FGT_TEST_D3D_FAIL")) : -1;
        int ok;
        if (++frames == test_fail) { rt_log("video: simulating a Direct3D failure"); ok = 0; }
        else {
            int r = (hd && g_hd_enabled) ? d3d_present_hd(hd) : -1;
            if (r == 1) { g_hd_used = 1; return 1; }
            ok = r == 0 ? 0 : d3d_present(rgb, w, h);
        }
        if (ok) return 1;
        /* device lost (driver update or crash, graphics card switched...): start Direct3D again. GDI is the last
         * resort: it can't draw the menu, and its picture can stay hidden behind the old swap chain's. */
        static int restarts;
        d3d_shutdown();
        if (restarts < 5 && d3d_init(g_wnd, g_hz, g_gpu_choice)) {
            restarts++;
            rt_log("video: Direct3D failed; started it again");
            if (g_mode == 2) g_reclaim_exclusive = 1;
            return 1;
        }
        rt_log("video: Direct3D failed for good, drawing with GDI (no menu)");
        g_use_d3d = 0;
    }
    g_bmi.bmiHeader.biSize = sizeof g_bmi.bmiHeader;
    g_bmi.bmiHeader.biWidth = w;
    g_bmi.bmiHeader.biHeight = -h;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;
    RECT cr;
    GetClientRect(g_wnd, &cr);
    int px, py, pw, ph;
    picture_rect(cr.right, cr.bottom, &px, &py, &pw, &ph);
    HDC dc = GetDC(g_wnd);
    if (px > 0) { PatBlt(dc, 0, 0, px, cr.bottom, BLACKNESS); PatBlt(dc, px + pw, 0, cr.right - px - pw, cr.bottom, BLACKNESS); }
    if (py > 0) { PatBlt(dc, 0, 0, cr.right, py, BLACKNESS); PatBlt(dc, 0, py + ph, cr.right, cr.bottom - py - ph, BLACKNESS); }
    StretchDIBits(dc, px, py, pw, ph, 0, 0, w, h, rgb, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_wnd, dc);
    return 1;
}

/* Wait for the next 60 Hz frame on a fixed schedule (no drift).
 * Needs timeBeginPeriod(1) for Sleep(1) to really be ~1 ms. */
void video_pace(void) {
    const double frame = (double)g_freq.QuadPart / 60.0;
    static double next;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (next == 0 || (double)now.QuadPart - next > 4 * frame) next = (double)now.QuadPart;   /* fell far behind: resync */
    next += frame;
    for (;;) {
        QueryPerformanceCounter(&now);
        double left = next - (double)now.QuadPart;
        if (left <= 0) break;
        if (left > (double)g_freq.QuadPart / 500.0) Sleep(1);   /* > 2 ms to go */
    }
}

int video_key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0 && GetForegroundWindow() == g_wnd; }

/* ---- frame timing: vsync on ~60 Hz displays, otherwise the fixed 60 Hz schedule ---- */
typedef HRESULT (WINAPI *DwmFlushFn)(void);
static DwmFlushFn g_dwm_flush;
static int g_vsync_checked;

void video_wait_frame(void) {
    static LARGE_INTEGER last;
    LARGE_INTEGER now;
    if (g_use_d3d && d3d_vsync()) {
        /* Present() already waited for the display */
    } else {
        if (!g_vsync_checked) {
            g_vsync_checked = 1;
            HMODULE h = LoadLibraryA("dwmapi.dll");
            if (h && g_hz >= 59 && g_hz <= 61) g_dwm_flush = (DwmFlushFn)(void *)GetProcAddress(h, "DwmFlush");
            rt_log("video: %s, %s", g_use_d3d ? "Direct3D without vsync" : "GDI path", g_dwm_flush ? "vsync via DwmFlush" : "60 Hz timer");
        }
        if (g_dwm_flush) g_dwm_flush();
        else { video_pace(); QueryPerformanceCounter(&last); return; }
    }
    /* Safety net: if the wait returned early (window minimised or covered, so nothing is shown), use the
     * timer instead so the game doesn't run fast. */
    QueryPerformanceCounter(&now);
    double ms = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)g_freq.QuadPart : 16.7;
    if (ms < 12.0) video_pace();
    QueryPerformanceCounter(&last);
}

/* repeated frames on screen since the last call; -1 when the drawing path can't tell */
int video_take_repeats(void) { return g_use_d3d ? d3d_take_repeats() : -1; }

int video_hd_used(void) { return g_hd_used; }

int ui_d3d_failed(void);
/* the overlay (menu) can be drawn: Direct3D is running and the overlay renderer works */
int video_menu_drawable(void) { return g_use_d3d && !ui_d3d_failed(); }
