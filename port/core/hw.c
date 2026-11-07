/*
 * Hardware layer: memory slow paths, PS1-style I/O registers (IRQ, DMA, timers, GPU ports) and the
 * Namco System 12 extras. Anything not modelled yet is logged once so we can see what the game needs.
 */
#include "recomp.h"
#include "hw.h"
#include "gpu.h"
#include "log.h"
#include "s12sound.h"
#include "h8.h"
#include <stdio.h>

uint8_t g_ram[RAM_SIZE];
uint8_t g_scratch[SCRATCH_SIZE];
uint8_t *g_prg_rom;
uint8_t *g_banked_rom;
uint32_t g_banked_size;
uint64_t g_frame;
int32_t g_budget;

static uint32_t i_stat, i_mask;
static uint32_t dma_madr[7], dma_bcr[7], dma_chcr[7], dpcr = 0x07654321u, dicr;
static uint32_t tmr_mode[3], tmr_target[3];
static uint64_t tmr_base[3];             /* clock value when the counter was last reset */
static uint64_t emu_cycles(void);        /* approximate CPU cycles since boot */

/* Root counters: 0 = system clock, 1 = hblank (when mode bit 8 set), 2 = system clock (/8 if mode bit 9). */
static uint32_t tmr_value(int t) {
    uint64_t c = emu_cycles() - tmr_base[t];
    uint64_t v;
    if (t == 1 && (tmr_mode[1] & 0x100u)) v = c / 2146u;              /* ~263 lines per frame */
    else if (t == 2 && (tmr_mode[2] & 0x200u)) v = c / 8u;
    else v = c;
    if ((tmr_mode[t] & 8u) && tmr_target[t]) v %= (uint64_t)tmr_target[t] + 1u;
    return (uint32_t)(v & 0xFFFFu);
}
static uint32_t io_regs[0x1000 / 4];     /* backing store for unmodelled 0x1F801000-0x1F801FFF */

/* ------------------------------------------------------------------ IRQ */
void hw_irq_raise(int bit) {
    i_stat |= 1u << bit;
    hw_irq_check();
}

static int g_in_irq;
void hw_irq_check(void) {
    if (g_in_irq) return;
    if ((i_stat & i_mask) && cpu_irq_enabled()) {
        g_in_irq = 1;
        bios_interrupt();
        g_in_irq = 0;
    }
}

/* ------------------------------------------------------------------ DMA */
static void dma_run(int ch) {
    uint32_t chcr = dma_chcr[ch], madr = dma_madr[ch] & 0x3FFFFCu, bcr = dma_bcr[ch];
    switch (ch) {
    case 2: /* GPU */
        if ((chcr & 0x600u) == 0x400u) {               /* linked list (ordering table) */
            uint32_t a = madr, guard = 0;
            if (rt_once(ONCE_MISC, 0xDD000000u | (madr & 0xFFFFFF))) {
                char b[600]; int o = 0; uint32_t x = madr;
                for (int k = 0; k < 24 && o < 560; k++) {
                    uint32_t h = MEM_R32(x);
                    o += snprintf(b + o, sizeof b - o, " %06X:%08X", x, h);
                    if (h & 0x800000u) break;
                    x = h & 0x3FFFFCu;
                }
                rt_log("GPU DMA list (chcr %08X ra %08X):%s", chcr, R(31), b);
                if (MEM_R32(madr) == 0) {
                    o = 0;
                    for (int k = -8; k < 24; k++) o += snprintf(b + o, sizeof b - o, " %08X", MEM_R32(madr + 4u * (uint32_t)k));
                    rt_log("   around %06X (from -0x20):%s", madr, b);
                    rt_log("   regs a0=%08X a1=%08X s0=%08X s2=%08X s3=%08X", R(4), R(5), R(16), R(18), R(19));
                }
            }
            for (;;) {
                uint32_t hdr = MEM_R32(a);
                uint32_t n = hdr >> 24;
                for (uint32_t i = 0; i < n; i++) gpu_gp0(MEM_R32(a + 4 + i * 4));
                if (hdr & 0x800000u) break;
                a = hdr & 0x3FFFFCu;
                if (++guard > 0x100000u) {
                    rt_log("GPU DMA: runaway list from %08X (hdr %08X at %08X)", madr, hdr, a);
                    break;
                }
            }
        } else {                                       /* block transfer */
            uint32_t words = (bcr & 0xFFFFu) * ((bcr >> 16) ? (bcr >> 16) : 1u);
            if (chcr & 1u) {
                for (uint32_t i = 0; i < words; i++) gpu_gp0(MEM_R32(madr + i * 4));
            } else {
                for (uint32_t i = 0; i < words; i++) MEM_W32(madr + i * 4, gpu_read());
            }
        }
        break;
    case 6: { /* ordering-table clear: reverse linked list ending in 0xFFFFFF */
        uint32_t n = (bcr & 0xFFFFu) ? (bcr & 0xFFFFu) : 0x10000u, a = madr;
        if (rt_once(ONCE_MISC, 0xDE000000u | (madr & 0xFFFFFF))) rt_log("OTC clear %06X n=%u", madr, n);
        for (uint32_t i = 0; i < n; i++, a -= 4)
            MEM_W32(a, i == n - 1 ? 0x00FFFFFFu : ((a - 4) & 0x3FFFFFu));
        break;
    }
    default:
        if (rt_once(ONCE_MISC, 0xD0000000u | ch))
            rt_log("DMA channel %d started (chcr=%08X madr=%08X bcr=%08X): not modelled", ch, chcr, madr, bcr);
        break;
    }
    dma_chcr[ch] &= ~0x01000000u;
    if (dicr & (1u << (16 + ch))) {
        dicr |= 1u << (24 + ch);
        if (dicr & 0x00800000u) { dicr |= 0x80000000u; hw_irq_raise(3); }
    }
}

/* ------------------------------------------------------------------ PS1 I/O block */
static uint32_t io_read(uint32_t p) {
    uint32_t o = p - 0x1F801000u;
    if (p == 0x1F801070u) return i_stat;
    if (p == 0x1F801074u) return i_mask;
    if (p >= 0x1F801080u && p < 0x1F8010F0u) {
        int ch = (int)((p - 0x1F801080u) >> 4);
        switch (p & 0xF) { case 0: return dma_madr[ch]; case 4: return dma_bcr[ch]; case 8: return dma_chcr[ch]; }
    }
    if (p == 0x1F8010F0u) return dpcr;
    if (p == 0x1F8010F4u) return dicr;
    if (p >= 0x1F801100u && p < 0x1F801130u) {
        int t = (int)((p - 0x1F801100u) >> 4);
        switch (p & 0xF) {
        case 0: return tmr_value(t);
        case 4: return tmr_mode[t] | 0x800u;
        case 8: return tmr_target[t];
        }
    }
    if (p == 0x1F801810u) return gpu_read();
    if (p == 0x1F801814u) return gpu_stat();
    if (rt_once(ONCE_IO_R, p)) rt_log("io read  %08X (unmodelled)", p);
    return io_regs[(o & 0xFFF) >> 2];
}

static void io_write(uint32_t p, uint32_t v) {
    uint32_t o = p - 0x1F801000u;
    if (p == 0x1F801070u) { i_stat &= v; return; }
    if (p == 0x1F801074u) { i_mask = v & 0x7FFu; hw_irq_check(); return; }
    if (p >= 0x1F801080u && p < 0x1F8010F0u) {
        int ch = (int)((p - 0x1F801080u) >> 4);
        switch (p & 0xF) {
        case 0: dma_madr[ch] = v & 0xFFFFFFu; return;
        case 4: dma_bcr[ch] = v; return;
        case 8: dma_chcr[ch] = v; if (v & 0x01000000u) dma_run(ch); return;
        }
    }
    if (p == 0x1F8010F0u) { dpcr = v; return; }
    if (p == 0x1F8010F4u) {
        dicr = (dicr & 0x7F000000u & ~(v & 0x7F000000u)) | (v & 0x00FF803Fu);
        if (!(dicr & 0x7F000000u)) dicr &= ~0x80000000u;
        return;
    }
    if (p >= 0x1F801100u && p < 0x1F801130u) {
        int t = (int)((p - 0x1F801100u) >> 4);
        switch (p & 0xF) {
        case 0: tmr_base[t] = emu_cycles() - (v & 0xFFFFu); return;
        case 4: tmr_mode[t] = v; tmr_base[t] = emu_cycles(); return;
        case 8: tmr_target[t] = v & 0xFFFFu; return;
        }
    }
    if (p == 0x1F801810u) { gpu_gp0(v); return; }
    if (p == 0x1F801814u) { gpu_gp1(v); return; }
    if (rt_once(ONCE_IO_W, p)) rt_log("io write %08X = %08X (unmodelled)", p, v);
    io_regs[(o & 0xFFF) >> 2] = v;
}

/* ------------------------------------------------------------------ System 12 specific space */
/* 0x1F080000-0x1F083FFF: RAM shared with the H8/3002 sound/I-O CPU (emulated in s12sound.c). */
#define SHARED_BASE 0x1F080000u
#define SHARED_SIZE 0x4000u

/* 0x1F140000-0x1F140FFF: AT28C16 2 KB settings EEPROM, one byte per 16-bit word. Saved to eeprom.bin. */
#define EEPROM_BASE 0x1F140000u
static uint8_t g_eeprom[0x800];
static int g_eeprom_loaded, g_eeprom_dirty;
int g_eeprom_volatile;          /* 1: start blank and never touch eeprom.bin (scripted test runs) */
static void eeprom_load(void) {
    if (g_eeprom_loaded) return;
    g_eeprom_loaded = 1;
    memset(g_eeprom, 0xFF, sizeof g_eeprom);
    if (g_eeprom_volatile) return;
    FILE *f = fopen("eeprom.bin", "rb");
    if (f) { fread(g_eeprom, 1, sizeof g_eeprom, f); fclose(f); }
}
uint8_t eeprom_get(int idx) { eeprom_load(); return g_eeprom[idx & 0x7FF]; }
void eeprom_set(int idx, uint8_t v) {
    eeprom_load();
    if (g_eeprom[idx & 0x7FF] != v) { g_eeprom[idx & 0x7FF] = v; g_eeprom_dirty = 1; }
}

/* Real-time clock. On the board an Epson RTC-4543 hangs off the sound CPU's serial port and the sound CPU
 * copies the time to shared RAM 0x3030-0x3037 every frame; that serial link isn't emulated, so the main
 * CPU's view of those 8 bytes comes straight from the host clock instead. Layout (main CPU, 16-bit words,
 * BCD): 3030 lo = year (2 digits), 3032 hi = month, 3032 lo = day, 3034 hi = weekday, 3034 lo = hour,
 * 3036 hi = minute, 3036 lo = second. */
static uint8_t bcd(int v) { return (uint8_t)(((v / 10) % 10) << 4 | (v % 10)); }
static uint32_t rtc_read(uint32_t ofs, int size) {
    int y, mo, d, wd, h, mi, sec;
    plat_local_time(&y, &mo, &d, &wd, &h, &mi, &sec);
    uint8_t b[8] = {bcd(y % 100), 0, bcd(d), bcd(mo), bcd(h), (uint8_t)wd, bcd(sec), bcd(mi)};
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)b[(ofs - 0x3030 + (uint32_t)i) & 7] << (8 * i);
    return v;
}

void eeprom_flush(void) {
    if (!g_eeprom_dirty || g_eeprom_volatile) return;
    FILE *f = fopen("eeprom.bin", "wb");
    if (f) { fwrite(g_eeprom, 1, sizeof g_eeprom, f); fclose(f); }
    g_eeprom_dirty = 0;
}

/* Banked data ROM: 0x1F000000 selects a 2 MB bank (file offset >> 21), visible at 0x1FA00000-0x1FBFFFFF.
 * (Found by matching reads against the file table: bank 8 + 0x78674 = TIM/FONTX.TIM at 0x1078674.) */
#define BANK_WINDOW 0x1FA00000u
#define BANK_SIZE   0x200000u
static uint32_t g_bank;

static uint32_t s12_read(uint32_t p, int size) {
    if (p >= BANK_WINDOW && p < BANK_WINDOW + BANK_SIZE) {
        uint32_t o = (g_bank << 21) + (p - BANK_WINDOW), v = 0;
        if (o + (uint32_t)size <= g_banked_size) memcpy(&v, g_banked_rom + o, size);
        return v;
    }
    if (p == 0x1F000000u) return g_bank;
    if (p >= EEPROM_BASE && p < EEPROM_BASE + 0x1000u) { eeprom_load(); return g_eeprom[(p - EEPROM_BASE) >> 1]; }
    if (p >= SHARED_BASE + 0x3030u && p < SHARED_BASE + 0x3038u) return rtc_read(p - SHARED_BASE, size);
    if (p >= SHARED_BASE && p < SHARED_BASE + SHARED_SIZE) return s12sound_shared_r(p - SHARED_BASE, size);
    if (rt_once(ONCE_IO_R, p)) rt_log("s12 read%d  %08X (unmodelled)", size * 8, p);
    return 0;
}
static void s12_write(uint32_t p, uint32_t v, int size) {
    if (p == 0x1F000000u) { g_bank = v & 0x1F; return; }
    if (p >= EEPROM_BASE && p < EEPROM_BASE + 0x1000u) {
        eeprom_load();
        g_eeprom[(p - EEPROM_BASE) >> 1] = (uint8_t)v;
        g_eeprom_dirty = 1;
        return;
    }
    if (p >= SHARED_BASE && p < SHARED_BASE + SHARED_SIZE) { s12sound_shared_w(p - SHARED_BASE, v, size); return; }
    if (p == 0x1F018000u) return;                  /* watchdog kick */
    if (rt_once(ONCE_IO_W, p)) rt_log("s12 write%d %08X = %08X (unmodelled)", size * 8, p, v);
}

/* ------------------------------------------------------------------ dispatch */
uint32_t hw_read(uint32_t p, int size) {
    if (p >= SCRATCH_BASE && p < SCRATCH_BASE + SCRATCH_SIZE) {
        uint32_t o = p - SCRATCH_BASE, v = 0;
        memcpy(&v, g_scratch + o, size);
        return v;
    }
    if (p >= 0x1FC00000u) {
        uint32_t o = p - 0x1FC00000u, v = 0;
        if (o + size <= 0x400000u) memcpy(&v, g_prg_rom + o, size);
        return v;
    }
    if (p >= 0x1F801000u && p < 0x1F802000u) {
        uint32_t w = io_read(p & ~3u);
        return (w >> ((p & 3) * 8)) & (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu);
    }
    if (p >= 0xFFFE0000u) return 0;
    return s12_read(p, size);
}

void hw_write(uint32_t p, uint32_t v, int size) {
    if (p >= SCRATCH_BASE && p < SCRATCH_BASE + SCRATCH_SIZE) {
        memcpy(g_scratch + (p - SCRATCH_BASE), &v, size);
        return;
    }
    if (p >= 0x1F801000u && p < 0x1F802000u) {
        if (size == 4) io_write(p, v);
        else {
            /* sub-word writes: merge into the containing register */
            uint32_t a = p & ~3u, sh = (p & 3) * 8, m = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
            uint32_t old = (a == 0x1F801070u) ? 0xFFFFFFFFu : io_read(a);
            io_write(a, (old & ~m) | ((v << sh) & m));
        }
        return;
    }
    if (p >= 0x1FC00000u) return;              /* ROM */
    s12_write(p, v, size);
}

uint32_t mem_slow_r32(uint32_t a) { uint32_t p = a >= 0xFFFE0000u ? a : PHYS(a); return hw_read(p, 4); }
uint16_t mem_slow_r16(uint32_t a) { uint32_t p = a >= 0xFFFE0000u ? a : PHYS(a); return (uint16_t)hw_read(p, 2); }
uint8_t  mem_slow_r8(uint32_t a)  { uint32_t p = a >= 0xFFFE0000u ? a : PHYS(a); return (uint8_t)hw_read(p, 1); }
void mem_slow_w32(uint32_t a, uint32_t v) { uint32_t p = a >= 0xFFFE0000u ? a : PHYS(a); hw_write(p, v, 4); }
void mem_slow_w16(uint32_t a, uint16_t v) { uint32_t p = a >= 0xFFFE0000u ? a : PHYS(a); hw_write(p, v, 2); }
void mem_slow_w8(uint32_t a, uint8_t v)   { uint32_t p = a >= 0xFFFE0000u ? a : PHYS(a); hw_write(p, v, 1); }

/* ------------------------------------------------------------------ time */
#define BUDGET_PER_SLICE 2000
#define SLICES_PER_FRAME 64
static int g_slices;
#define CYCLES_PER_FRAME 564480u                                   /* 33.8688 MHz / 60 */
static uint64_t emu_cycles(void) {
    uint64_t units = g_frame * (uint64_t)(BUDGET_PER_SLICE * SLICES_PER_FRAME)
                   + (uint64_t)g_slices * BUDGET_PER_SLICE + (uint64_t)(BUDGET_PER_SLICE - (g_budget > 0 ? g_budget : 0));
    return units * CYCLES_PER_FRAME / (BUDGET_PER_SLICE * SLICES_PER_FRAME);
}

void plat_vblank(void);

void hw_init(void) { g_budget = BUDGET_PER_SLICE; }

/* profile of where loops spend time (by loop branch address), dumped once a second */
#define PROF_SLOTS 64
static uint32_t prof_pc[PROF_SLOTS], prof_n[PROF_SLOTS];
static void prof_hit(uint32_t pc) {
    for (int i = 0; i < PROF_SLOTS; i++) {
        if (prof_pc[i] == pc) { prof_n[i]++; return; }
        if (!prof_pc[i]) { prof_pc[i] = pc; prof_n[i] = 1; return; }
    }
}
static void prof_dump(void) {
    char buf[512]; int o = 0;
    for (int k = 0; k < 6; k++) {
        int best = -1;
        for (int i = 0; i < PROF_SLOTS; i++) if (prof_pc[i] && (best < 0 || prof_n[i] > prof_n[best])) best = i;
        if (best < 0 || !prof_n[best]) break;
        o += snprintf(buf + o, sizeof buf - o, " %08X:%u", prof_pc[best], prof_n[best]);
        prof_n[best] = 0;
    }
    rt_log("hot loops:%s", buf);
    h8_debug_dump();
    rt_log("irq: stat=%03X mask=%03X sr=%08X dicr=%08X", i_stat, i_mask, cpu_get_sr(), dicr);
    memset(prof_pc, 0, sizeof prof_pc);
    memset(prof_n, 0, sizeof prof_n);
}

/* The sound CPU runs in step with the main CPU: one slice of H8 time per main-CPU slice. */
#define H8_STATES_PER_SLICE (S12SND_H8_HZ / 60 / SLICES_PER_FRAME)
#define VBLANK_SLICES 6          /* ~23 of 263 lines */

void service_events(uint32_t loop_pc) {
    g_budget = BUDGET_PER_SLICE;
    prof_hit(loop_pc);
    s12sound_run(H8_STATES_PER_SLICE);
    if (g_slices == VBLANK_SLICES - 1) s12sound_vblank(0);
    if (g_frame % 60 == 59 && g_slices == SLICES_PER_FRAME - 1) prof_dump();
    if (++g_slices >= SLICES_PER_FRAME) {
        g_slices = 0;
        g_frame++;
        gpu_vblank();
        if (g_frame % 60 == 0) eeprom_flush();
        plat_vblank();
        s12sound_vblank(1);
        hw_irq_raise(0);          /* VBLANK */
    }
    hw_irq_check();
}
