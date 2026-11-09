/*
 * Hang watchdog. A background thread checks that frames keep reaching the screen. If none has for 3 seconds,
 * it writes to fgtlayer.log what the game was doing: where the main thread is (sampled a few times, with the
 * return addresses on its stack, as offsets into the module) and what the display and menu were set to. A
 * player's log then says where a freeze happened, which nobody could otherwise find out.
 */
#include "watchdog.h"
#include "../core/log.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

static volatile LONG g_beat;
static HANDLE g_main;
static char g_state[160];

void watchdog_beat(void) { InterlockedIncrement(&g_beat); }
void watchdog_note(const char *state) {
    strncpy(g_state, state, sizeof g_state - 1);
}

/* "fgtlayer.exe+1A2B3C" style name for a code address */
static void where(DWORD addr, char *out, size_t n) {
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(UINT_PTR)addr, &m) && m) {
        GetModuleFileNameA(m, path, sizeof path);
        const char *base = strrchr(path, '\\');
        snprintf(out, n, "%s+%lX", base ? base + 1 : path, (unsigned long)(addr - (DWORD)(UINT_PTR)m));
    } else snprintf(out, n, "%08lX", (unsigned long)addr);
}

static void sample(int k) {
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(g_main) == (DWORD)-1) return;
    char line[1024], w[96];
    int len = 0;
    if (GetThreadContext(g_main, &c)) {
#if defined(_M_IX86) || defined(__i386__)
        DWORD ip = c.Eip, bp = c.Ebp;
#else
        DWORD ip = (DWORD)c.Rip, bp = (DWORD)c.Rbp;
#endif
        where(ip, w, sizeof w);
        len = snprintf(line, sizeof line, "watchdog: sample %d at %s; called from", k, w);
        /* walk the frame-pointer chain (read safely: the thread is stopped, but the chain may be junk) */
        for (int i = 0; i < 10 && bp; i++) {
            DWORD fr[2];
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(UINT_PTR)bp, fr, sizeof fr, &got) || got != sizeof fr) break;
            if (!fr[1]) break;
            where(fr[1], w, sizeof w);
            len += snprintf(line + len, sizeof line - (size_t)len, " %s", w);
            if (fr[0] <= bp || len > (int)sizeof line - 100) break;
            bp = fr[0];
        }
    } else len = snprintf(line, sizeof line, "watchdog: couldn't read the main thread");
    ResumeThread(g_main);
    rt_log("%s", line);
}

static DWORD WINAPI run(LPVOID p) {
    (void)p;
    LONG last = g_beat;
    DWORD still = 0;
    int reported = 0;
    for (;;) {
        Sleep(250);
        LONG b = g_beat;
        if (b != last) {
            if (reported) rt_log("watchdog: frames are being drawn again (stopped for %.1f s)", still / 1000.0);
            last = b; still = 0; reported = 0;
            continue;
        }
        still += 250;
        if (still >= 3000 && !reported) {
            reported = 1;
            rt_log("watchdog: no frame drawn for 3 seconds. State: %s", g_state);
            for (int k = 0; k < 4; k++) { sample(k); Sleep(150); }
        }
    }
    return 0;
}

void watchdog_start(void) {
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) return;
    HANDLE t = CreateThread(NULL, 64 * 1024, run, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
