/*
 * Runtime interface for recompiled MIPS code (see tools/recomp/recomp.py).
 *
 * The recompiled game keeps the original memory model: a 4 MB RAM image addressed with the game's own
 * pointers (0x80000000-based), a 1 KB scratchpad at 0x1F800000, and hardware registers behind
 * slow-path handlers. This stays in place until the code is fully decompiled.
 */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t r[32];
    uint32_t hi, lo;
} CPUState;

extern CPUState cpu;
#define R(n) cpu.r[n]

/* ---- memory ---- */
#define RAM_SIZE      0x400000u
#define RAM_MASK      (RAM_SIZE - 1)
#define SCRATCH_BASE  0x1F800000u
#define SCRATCH_SIZE  0x400u

extern uint8_t g_ram[RAM_SIZE];
extern uint8_t g_scratch[SCRATCH_SIZE];

uint32_t mem_slow_r32(uint32_t a);
uint16_t mem_slow_r16(uint32_t a);
uint8_t  mem_slow_r8(uint32_t a);
void     mem_slow_w32(uint32_t a, uint32_t v);
void     mem_slow_w16(uint32_t a, uint16_t v);
void     mem_slow_w8(uint32_t a, uint8_t v);

/* KUSEG/KSEG0/KSEG1 all map onto the same physical space. RAM sits in the low 8 MB (mirrored). */
#define PHYS(a) ((a) & 0x1FFFFFFFu)

#ifdef FGT_MEMWATCH          /* debug builds only: report reads of a watched RAM range (see port/pc/main.c) */
extern uint32_t g_memwatch_lo, g_memwatch_hi;
void memwatch_hit(uint32_t addr);
#define MEMWATCH(a) do { uint32_t q_ = PHYS(a); if (q_ >= g_memwatch_lo && q_ < g_memwatch_hi) memwatch_hit(a); } while (0)
#else
#define MEMWATCH(a) ((void)0)
#endif

static inline uint32_t MEM_R32(uint32_t a) {
    MEMWATCH(a);
    uint32_t p = PHYS(a);
    if (p < 0x800000u) { uint32_t v; memcpy(&v, g_ram + (p & RAM_MASK & ~3u), 4); return v; }
    return mem_slow_r32(a);
}
static inline uint16_t MEM_R16(uint32_t a) {
    MEMWATCH(a);
    uint32_t p = PHYS(a);
    if (p < 0x800000u) { uint16_t v; memcpy(&v, g_ram + (p & RAM_MASK & ~1u), 2); return v; }
    return mem_slow_r16(a);
}
static inline uint8_t MEM_R8(uint32_t a) {
    MEMWATCH(a);
    uint32_t p = PHYS(a);
    if (p < 0x800000u) return g_ram[p & RAM_MASK];
    return mem_slow_r8(a);
}
static inline void MEM_W32(uint32_t a, uint32_t v) {
    MEMWATCH(a);
    uint32_t p = PHYS(a);
    if (p < 0x800000u) { memcpy(g_ram + (p & RAM_MASK & ~3u), &v, 4); return; }
    mem_slow_w32(a, v);
}
static inline void MEM_W16(uint32_t a, uint16_t v) {
    MEMWATCH(a);
    uint32_t p = PHYS(a);
    if (p < 0x800000u) { memcpy(g_ram + (p & RAM_MASK & ~1u), &v, 2); return; }
    mem_slow_w16(a, v);
}
static inline void MEM_W8(uint32_t a, uint8_t v) {
    MEMWATCH(a);
    uint32_t p = PHYS(a);
    if (p < 0x800000u) { g_ram[p & RAM_MASK] = v; return; }
    mem_slow_w8(a, v);
}

/* Unaligned word access (little-endian semantics). All shifts stay below 32. */
static inline uint32_t MEM_LWL(uint32_t a, uint32_t rt) {
    uint32_t s = (a & 3u) * 8u, w = MEM_R32(a & ~3u);
    return (rt & (0x00FFFFFFu >> s)) | (w << (24u - s));
}
static inline uint32_t MEM_LWR(uint32_t a, uint32_t rt) {
    uint32_t s = (a & 3u) * 8u, w = MEM_R32(a & ~3u);
    return (rt & (0xFFFFFF00u << (24u - s))) | (w >> s);
}
static inline void MEM_SWL(uint32_t a, uint32_t rt) {
    uint32_t s = (a & 3u) * 8u, w = MEM_R32(a & ~3u);
    MEM_W32(a & ~3u, (w & (0xFFFFFF00u << s)) | (rt >> (24u - s)));
}
static inline void MEM_SWR(uint32_t a, uint32_t rt) {
    uint32_t s = (a & 3u) * 8u, w = MEM_R32(a & ~3u);
    MEM_W32(a & ~3u, (w & (0x00FFFFFFu >> (24u - s))) | (rt << s));
}

/* Pointer into emulated RAM for host-side helpers (BIOS HLE etc). */
static inline void *ram_ptr(uint32_t a) { return g_ram + (PHYS(a) & RAM_MASK); }

/* ---- arithmetic helpers ---- */
static inline void cpu_mult(uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    cpu.lo = (uint32_t)r; cpu.hi = (uint32_t)((uint64_t)r >> 32);
}
static inline void cpu_multu(uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a * (uint64_t)b;
    cpu.lo = (uint32_t)r; cpu.hi = (uint32_t)(r >> 32);
}
static inline void cpu_div(uint32_t a, uint32_t b) {
    int32_t n = (int32_t)a, d = (int32_t)b;
    if (d == 0) { cpu.hi = a; cpu.lo = n >= 0 ? 0xFFFFFFFFu : 1u; }
    else if (n == INT32_MIN && d == -1) { cpu.hi = 0; cpu.lo = a; }
    else { cpu.lo = (uint32_t)(n / d); cpu.hi = (uint32_t)(n % d); }
}
static inline void cpu_divu(uint32_t a, uint32_t b) {
    if (b == 0) { cpu.hi = a; cpu.lo = 0xFFFFFFFFu; }
    else { cpu.lo = a / b; cpu.hi = a % b; }
}

/* ---- control flow ---- */
typedef void (*RecompFunc)(void);
typedef struct { uint32_t addr; RecompFunc fn; } FuncEntry;
#define OVL_FINGERPRINT 64      /* first bytes of the overlay (a header of function pointers) */
#define OVL_PROBES 4            /* plus a few function entries: code that never changes at run time */
#define OVL_PROBE 16
typedef struct { uint32_t addr; uint8_t bytes[OVL_PROBE]; } OvlProbe;
typedef struct {
    const char *name; int slot; uint32_t vram; uint32_t size;
    const FuncEntry *funcs; int nfuncs;
    uint8_t fingerprint[OVL_FINGERPRINT];
    OvlProbe probes[OVL_PROBES]; int nprobes;
} OverlayInfo;

extern const FuncEntry g_main_funcs[];
extern const int g_main_funcs_count;
extern const OverlayInfo g_overlays[];
extern const int g_overlays_count;

void call_indirect(uint32_t addr);

/* ---- coprocessors / exceptions ---- */
uint32_t cop0_read(int reg);
void     cop0_write(int reg, uint32_t v);
void     cop0_rfe(void);
void     cpu_syscall(uint32_t pc);
void     cpu_break(uint32_t pc, uint32_t code);
void     cpu_unimpl(uint32_t pc, uint32_t word);

uint32_t gte_mfc2(int reg);
uint32_t gte_cfc2(int reg);
void     gte_mtc2(int reg, uint32_t v);
void     gte_ctc2(int reg, uint32_t v);
void     gte_op(uint32_t op);

/* ---- timing: recompiled loops count down a budget; at zero the platform services interrupts ---- */
extern int32_t g_budget;
void service_events(uint32_t loop_pc);
#define CHECK_EVENTS(pc) do { if (--g_budget < 0) service_events(pc); } while (0)
