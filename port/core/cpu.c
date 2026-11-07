/* CPU state, COP0, exceptions and indirect-call dispatch for recompiled code. */
#include "recomp.h"
#include "log.h"
#include "bios.h"
#include "hw.h"
#include <stdlib.h>

CPUState cpu;
static uint32_t g_cop0[32];
#define SR 12

uint32_t cop0_read(int reg) { return g_cop0[reg & 31]; }

void cop0_write(int reg, uint32_t v) {
    g_cop0[reg & 31] = v;
    if (reg == SR) hw_irq_check();
}

void cop0_rfe(void) {
    uint32_t sr = g_cop0[SR];
    g_cop0[SR] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
}

int cpu_irq_enabled(void) { return (g_cop0[SR] & 0x401u) == 0x401u; }
void cpu_set_sr(uint32_t v) { g_cop0[SR] = v; }
uint32_t cpu_get_sr(void) { return g_cop0[SR]; }

void cpu_syscall(uint32_t pc) {
    /* PsyQ: a0=1 EnterCriticalSection, a0=2 ExitCriticalSection. The real kernel edits the SR saved at
     * exception entry (IEp, bit 2), which rfe moves back to IEc (bit 0); without the exception round
     * trip we change IEc/IM2 directly. */
    switch (R(4)) {
    case 1:
        R(2) = (g_cop0[SR] & 0x401u) == 0x401u;
        g_cop0[SR] &= ~0x401u;
        break;
    case 2:
        g_cop0[SR] |= 0x401u;
        hw_irq_check();
        break;
    default:
        if (rt_once(ONCE_MISC, 0x5C000000u | R(4)))
            rt_log("syscall a0=%u at %08X", R(4), pc);
        break;
    }
}

void cpu_break(uint32_t pc, uint32_t code) {
    if (code == 1 || code == 0x400) {
        rt_log("break %u at %08X (program exit)", code, pc);
        exit(0);
    }
    rt_log("break %u at %08X (division by zero / assert?) ignored", code, pc);
}

void cpu_unimpl(uint32_t pc, uint32_t word) {
    rt_fatal("unimplemented instruction %08X at %08X", word, pc);
}

/* ---------------- indirect calls ---------------- */
static RecompFunc find_in(const FuncEntry *t, int n, uint32_t addr) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (t[mid].addr == addr) return t[mid].fn;
        if (t[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static const OverlayInfo *g_slot_ovl[2];

static int code_matches(const OverlayInfo *o) {
    if (o->nprobes == 0) return 0;
    for (int i = 0; i < o->nprobes; i++)
        if (memcmp(ram_ptr(o->probes[i].addr), o->probes[i].bytes, OVL_PROBE) != 0) return 0;
    return 1;
}

/* Which overlay is currently loaded in a slot? First compare the RAM image with each overlay's first bytes
 * (fast); if none match, compare code at a few function entries, because the game rewrites some overlays'
 * headers during a fight: the bosses keep a second table of move-data pointers at +0x40 and copy it over the
 * first after round one (seen with pl11 in slot 2, header words 6-8). */
static const OverlayInfo *overlay_in_slot(int slot) {
    uint32_t base = slot == 1 ? 0x8009E000u : 0x800AE000u;
    const uint8_t *ram = ram_ptr(base);
    const OverlayInfo *cur = g_slot_ovl[slot - 1];
    if (cur && memcmp(ram, cur->fingerprint, OVL_FINGERPRINT) == 0) return cur;
    for (int i = 0; i < g_overlays_count; i++) {
        const OverlayInfo *o = &g_overlays[i];
        if (o->slot == slot && memcmp(ram, o->fingerprint, OVL_FINGERPRINT) == 0) {
            if (cur != o) rt_log("overlay slot %d: %s", slot, o->name);
            g_slot_ovl[slot - 1] = o;
            return o;
        }
    }
    if (cur && code_matches(cur)) {
        if (rt_once(ONCE_MISC, 0x0F000000u | (uint32_t)(cur - g_overlays)))
        {
            rt_log("overlay slot %d: %s (header changed by the game; recognised by its code)", slot, cur->name);
            for (int w = 0; w < OVL_FINGERPRINT / 4; w++) {
                uint32_t was, now = MEM_R32(base + 4u * (uint32_t)w);
                memcpy(&was, cur->fingerprint + 4 * w, 4);
                if (was != now) rt_log("  header word %d: %08X -> %08X", w, was, now);
            }
        }
        return cur;
    }
    for (int i = 0; i < g_overlays_count; i++) {
        const OverlayInfo *o = &g_overlays[i];
        if (o->slot == slot && code_matches(o)) {
            rt_log("overlay slot %d: %s (recognised by its code)", slot, o->name);
            g_slot_ovl[slot - 1] = o;
            return o;
        }
    }
    rt_log("overlay slot %d: nothing recognised; header %08X %08X %08X %08X", slot, MEM_R32(base), MEM_R32(base + 4),
           MEM_R32(base + 8), MEM_R32(base + 12));
    return NULL;
}

static void record_missing(const char *unit, uint32_t addr) {
    FILE *f = fopen("missing_entries.txt", "a");
    if (f) { fprintf(f, "%s %08X\n", unit, addr); fclose(f); }
}

void call_indirect(uint32_t addr) {
    uint32_t p = PHYS(addr);
    if (p == 0xA0 || p == 0xB0 || p == 0xC0) {
        bios_call((int)p, R(9));
        return;
    }
    if (addr >= 0x801F0000u && addr < 0x80310000u) {
        RecompFunc fn = find_in(g_main_funcs, g_main_funcs_count, addr);
        if (fn) { fn(); return; }
        record_missing("main", addr);
        rt_fatal("call to %08X: not a known function entry in main (added to missing_entries.txt; "
                 "copy it into config/recomp_extra_funcs.txt and re-run the recompiler)", addr);
    }
    if (addr >= 0x8009E000u && addr < 0x800BE000u) {
        int slot = addr < 0x800AE000u ? 1 : 2;
        const OverlayInfo *o = overlay_in_slot(slot);
        if (!o) rt_fatal("call to %08X: no known overlay is loaded in slot %d", addr, slot);
        RecompFunc fn = find_in(o->funcs, o->nfuncs, addr);
        if (fn) { fn(); return; }
        char unit[32];
        snprintf(unit, sizeof unit, "%s_%d", o->name, slot);
        record_missing(unit, addr);
        rt_fatal("call to %08X: not a known function entry in overlay %s", addr, unit);
    }
    rt_fatal("call to %08X: address outside recompiled code", addr);
}
