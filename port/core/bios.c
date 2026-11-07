/*
 * High-level emulation of the Sony kernel calls the game uses (A0/B0/C0 tables).
 * The real kernel ROM is not executed; only what the game asks for is implemented.
 */
#include "recomp.h"
#include "bios.h"
#include "hw.h"
#include "log.h"
#include <setjmp.h>
#include <stdio.h>

/* ---------------------------------------------------------------- heap (A33/A34/A37/A38/A39) */
static uint32_t heap_start, heap_end;
/* block header in RAM: [size_with_header | used_bit] */
static void heap_init(uint32_t start, uint32_t size) {
    heap_start = (start + 7u) & ~7u;
    heap_end = (start + size) & ~7u;
    MEM_W32(heap_start, heap_end - heap_start);
    rt_log("InitHeap %08X size %X", start, size);
}
static uint32_t heap_alloc(uint32_t n) {
    n = (n + 4u + 7u) & ~7u;
    for (uint32_t a = heap_start; a < heap_end;) {
        uint32_t h = MEM_R32(a), sz = h & ~1u;
        if (sz == 0) break;
        if (!(h & 1u)) {
            /* merge following free blocks */
            uint32_t nx = a + sz;
            while (nx < heap_end && !(MEM_R32(nx) & 1u) && (MEM_R32(nx) & ~1u)) {
                sz += MEM_R32(nx) & ~1u;
                nx = a + sz;
            }
            if (sz >= n) {
                if (sz - n >= 16u) { MEM_W32(a + n, sz - n); sz = n; }
                MEM_W32(a, sz | 1u);
                return a + 4u;
            }
            MEM_W32(a, sz);
        }
        a += sz;
    }
    rt_log("malloc(%u) failed", n);
    return 0;
}
static void heap_free(uint32_t p) {
    if (p) MEM_W32(p - 4u, MEM_R32(p - 4u) & ~1u);
}

/* ---------------------------------------------------------------- printf (A3F) */
static uint32_t va_arg_n(int i) { return i < 4 ? R(4 + i) : MEM_R32(R(29) + 4u * (uint32_t)i); }

static void ram_cstr(uint32_t a, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { char c = (char)MEM_R8(a + (uint32_t)i); if (!c) break; out[i] = c; }
    out[i] = 0;
}

static void bios_printf(void) {
    char fmt[512], out[1024], spec[32], tmp[512];
    ram_cstr(R(4), fmt, sizeof fmt);
    int arg = 1, o = 0;
    for (const char *p = fmt; *p && o < (int)sizeof out - 64; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        int s = 0;
        spec[s++] = *p++;
        while (*p && strchr("-+ #0123456789.lh", *p) && s < 28) spec[s++] = *p++;
        if (!*p) break;
        spec[s++] = *p == 'p' ? 'x' : *p;
        spec[s] = 0;
        /* strip length modifiers: everything is 32-bit */
        char clean[32]; int c = 0;
        for (int k = 0; spec[k]; k++) if (spec[k] != 'l' && spec[k] != 'h') clean[c++] = spec[k];
        clean[c] = 0;
        if (*p == '%') { out[o++] = '%'; continue; }
        if (*p == 's') { ram_cstr(va_arg_n(arg++), tmp, sizeof tmp); o += snprintf(out + o, sizeof out - o, clean, tmp); }
        else o += snprintf(out + o, sizeof out - o, clean, va_arg_n(arg++));
    }
    out[o] = 0;
    rt_log("[game] %s", out);
    R(2) = (uint32_t)o;
}

/* ---------------------------------------------------------------- events (B07-B0D) */
typedef struct { uint32_t cls, spec, mode, func; int enabled, ready, used; } Event;
static Event g_events[32];

static uint32_t open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func) {
    for (int i = 0; i < 32; i++)
        if (!g_events[i].used) {
            g_events[i] = (Event){cls, spec, mode, func, 0, 0, 1};
            rt_log("OpenEvent(class=%08X spec=%04X mode=%04X func=%08X) -> %d", cls, spec, mode, func, i);
            return 0xF1000000u | (uint32_t)i;
        }
    return 0xFFFFFFFFu;
}

static void deliver_event(uint32_t cls, uint32_t spec) {
    for (int i = 0; i < 32; i++) {
        Event *e = &g_events[i];
        if (e->used && e->enabled && e->cls == cls && (e->spec & spec)) {
            if (e->mode == 0x2000u) e->ready = 1;
            else if (e->mode == 0x1000u && e->func) {
                CPUState save = cpu;
                R(29) = 0x8000E000u;             /* kernel exception stack (unused low RAM) */
                R(31) = 0;
                call_indirect(e->func);
                cpu = save;
            }
        }
    }
}

/* ---------------------------------------------------------------- interrupts */
static uint32_t g_hook;                 /* HookEntryInt jmp_buf address (0 = none) */
static jmp_buf g_rfe_jb;
static int g_in_exception;

void bios_interrupt(void) {
    /* kernel handlers run first: vblank delivers the root-counter-3 event once per vblank */
    static uint64_t vbl_delivered = ~0ull;
    uint32_t pending = hw_read(0x1F801070u, 4) & hw_read(0x1F801074u, 4);
    if ((pending & 1u) && vbl_delivered != g_frame) {
        vbl_delivered = g_frame;
        deliver_event(0xF2000003u, 2);
    }
    if (!g_hook) {
        /* kernel default: root-counter/vblank events, then acknowledge so we don't spin */
        hw_write(0x1F801070u, ~pending, 4);
        return;
    }
    if (g_in_exception) return;
    CPUState save = cpu;
    uint32_t sr = cpu_get_sr();
    cpu_set_sr((sr & ~0x3Fu) | ((sr << 2) & 0x3Fu));   /* enter exception: interrupts off */
    /* jmp_buf layout: ra, sp, fp, s0-s7, gp */
    uint32_t jb = g_hook;
    uint32_t pc = MEM_R32(jb + 0);
    R(29) = MEM_R32(jb + 4);
    R(30) = MEM_R32(jb + 8);
    for (int i = 0; i < 8; i++) R(16 + i) = MEM_R32(jb + 12 + 4u * (uint32_t)i);
    R(28) = MEM_R32(jb + 44);
    R(2) = 1;
    g_in_exception = 1;
    if (setjmp(g_rfe_jb) == 0) call_indirect(pc);
    g_in_exception = 0;
    cpu = save;
    cpu_set_sr(sr);
}

static void return_from_exception(void) {
    if (g_in_exception) longjmp(g_rfe_jb, 1);
    rt_log("ReturnFromException outside an exception");
}

/* ---------------------------------------------------------------- threads (B0E/B0F/B10) */
#include "coro.h"
#define MAX_THREADS 8
typedef struct { int used; CPUState regs; uint32_t pc; Coro *coro; } Thread;
static Thread g_th[MAX_THREADS];
static int g_cur_th;

static void thread_entry(void *arg) {
    Thread *t = (Thread *)arg;
    call_indirect(t->pc);
    rt_fatal("thread %d (pc %08X) returned", (int)(t - g_th), t->pc);
}

static uint32_t open_thread(uint32_t pc, uint32_t sp, uint32_t gp) {
    if (!g_th[0].used) { g_th[0].used = 1; g_th[0].coro = coro_init_main(); }
    for (int i = 1; i < MAX_THREADS; i++)
        if (!g_th[i].used) {
            Thread *t = &g_th[i];
            memset(t, 0, sizeof *t);
            t->used = 1;
            t->pc = pc;
            t->regs.r[29] = sp;
            t->regs.r[30] = sp;
            t->regs.r[28] = gp;
            rt_log("OpenTh(pc=%08X sp=%08X gp=%08X) -> %d", pc, sp, gp, i);
            return 0xFF000000u | (uint32_t)i;
        }
    return 0xFFFFFFFFu;
}

static void change_thread(uint32_t h) {
    int i = (int)(h & 0xFFFF);
    if (i < 0 || i >= MAX_THREADS || !g_th[i].used) { rt_log("ChangeTh(%08X): bad handle", h); R(2) = 0; return; }
    R(2) = 1;
    if (i == g_cur_th) return;
    Thread *from = &g_th[g_cur_th], *to = &g_th[i];
    from->regs = cpu;                          /* resumes later with v0 = 1 */
    if (!to->coro) to->coro = coro_create(thread_entry, to, 1u << 20);
    if (!to->coro) rt_fatal("could not create host coroutine for thread %d", i);
    g_cur_th = i;
    cpu = to->regs;
    coro_switch(to->coro);
}

/* ---------------------------------------------------------------- dispatch */
void bios_call(int table, uint32_t fn) {
    uint32_t key = ((uint32_t)table << 8) | fn;
    switch (key) {
    /* A0 */
    case 0xA039: heap_init(R(4), R(5)); return;
    case 0xA033: R(2) = heap_alloc(R(4)); return;
    case 0xA034: heap_free(R(4)); return;
    case 0xA037: { uint32_t n = R(4) * R(5), p = heap_alloc(n);
                   for (uint32_t i = 0; p && i < n; i++) MEM_W8(p + i, 0); R(2) = p; return; }
    case 0xA03F: bios_printf(); return;
    case 0xA044: return;                                    /* FlushCache */
    case 0xA049: hw_write(0x1F801810u, R(4), 4); return;    /* GPU_cw: GP0 command word */
    case 0xA072: return;                                    /* CdRemove */
    /* B0 */
    case 0xB007: deliver_event(R(4), R(5)); R(2) = 1; return;
    case 0xB008: R(2) = open_event(R(4), R(5), R(6), R(7)); return;
    case 0xB009: if ((R(4) & 0xFF) < 32) g_events[R(4) & 0xFF].used = 0; R(2) = 1; return;
    case 0xB00B: { Event *e = &g_events[R(4) & 31]; R(2) = e->ready; e->ready = 0; return; }
    case 0xB00C: g_events[R(4) & 31].enabled = 1; R(2) = 1; return;
    case 0xB00D: g_events[R(4) & 31].enabled = 0; R(2) = 1; return;
    case 0xB00E: R(2) = open_thread(R(4), R(5), R(6)); return;
    case 0xB00F: if ((R(4) & 0xFFFF) > 0 && (R(4) & 0xFFFF) < MAX_THREADS && (int)(R(4) & 0xFFFF) != g_cur_th)
                     g_th[R(4) & 0xFFFF].used = 0;
                 R(2) = 1; return;
    case 0xB010: change_thread(R(4)); return;
    case 0xB056: R(2) = 0x674; return;                      /* GetC0Table: kernel RAM, unused by HLE */
    case 0xB057: R(2) = 0x874; return;                      /* GetB0Table */
    case 0xB017: return_from_exception(); return;
    case 0xB018: g_hook = 0; return;
    case 0xB019: g_hook = R(4); rt_log("HookEntryInt(%08X) handler pc=%08X", R(4), MEM_R32(R(4))); return;
    case 0xB035: {                                          /* write(fd, buf, n) */
        char buf[512]; uint32_t n = R(6) < sizeof buf - 1 ? R(6) : sizeof buf - 1;
        for (uint32_t i = 0; i < n; i++) buf[i] = (char)MEM_R8(R(5) + i);
        buf[n] = 0;
        rt_log("[game:%u] %s", R(4), buf);
        R(2) = R(6);
        return;
    }
    /* C0 */
    case 0xC00A: R(2) = 1; return;                          /* ChangeClearRCnt */
    }
    if (rt_once(ONCE_BIOS, key))
        rt_log("BIOS %c(%02X) not implemented (a0=%08X a1=%08X a2=%08X ra=%08X)",
               table == 0xA0 ? 'A' : table == 0xB0 ? 'B' : 'C', fn, R(4), R(5), R(6), R(31));
    R(2) = 0;
}
