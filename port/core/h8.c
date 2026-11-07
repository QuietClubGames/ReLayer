/*
 * H8/3002 (H8/300H, advanced mode) interpreter.
 * Encodings follow the H8/300H programming manual (cross-checked with MAME's h8.lst).
 * Cycle counts are approximate (2 states per instruction word + 2 per data access); timers count
 * these states, so tempo depends on the timer setup, not on exact instruction timing.
 */
#include "h8.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ CPU state */
static uint32_t ER[8];
static uint32_t PC;
static uint8_t CCR;
static int sleeping;
static int64_t states;          /* states consumed in the current h8_run() call */
static uint64_t total_states;

enum { F_C = 1, F_V = 2, F_Z = 4, F_N = 8, F_U = 16, F_H = 32, F_UI = 64, F_I = 128 };

/* ------------------------------------------------------------------ on-chip peripherals */
static uint8_t onchip_ram[0x1000];      /* 0xFFF000-0xFFFFFF backing (RAM + unmodelled registers) */
static uint8_t irq_line[6], irq_isr, iscr, ier;
static uint8_t syscr = 0x0B;
static uint8_t port_dr[12], port_ddr[12];
static uint8_t adcsr, adcr;
static uint16_t adc_data[4];

typedef struct {
    uint8_t tcr, tior, tier, tsr;
    uint16_t tcnt, gra, grb;
    uint32_t prescale_acc;
} ItuCh;
static ItuCh itu[5];
static uint8_t tstr, tsnc, tmdr, tfcr;

static const uint32_t itu_base[5] = {0xFFFF64, 0xFFFF6E, 0xFFFF78, 0xFFFF82, 0xFFFF92};

static int itu_reg(uint32_t a, int *ch, int *off) {
    for (int i = 0; i < 5; i++) {
        int span = i < 3 ? 10 : 14;
        if (a >= itu_base[i] && a < itu_base[i] + (uint32_t)span) { *ch = i; *off = (int)(a - itu_base[i]); return 1; }
    }
    return 0;
}

/* port number from data/ddr register address; returns 0 if not a port register */
static int port_of(uint32_t a, int *is_ddr) {
    static const struct { uint32_t a; int port, ddr; } m[] = {
        {0xFFFFC0, 1, 1}, {0xFFFFC1, 2, 1}, {0xFFFFC2, 1, 0}, {0xFFFFC3, 2, 0},
        {0xFFFFC4, 3, 1}, {0xFFFFC5, 4, 1}, {0xFFFFC6, 3, 0}, {0xFFFFC7, 4, 0},
        {0xFFFFC8, 5, 1}, {0xFFFFC9, 6, 1}, {0xFFFFCA, 5, 0}, {0xFFFFCB, 6, 0},
        {0xFFFFCD, 8, 1}, {0xFFFFCE, 7, 0}, {0xFFFFCF, 8, 0},
        {0xFFFFD0, 9, 1}, {0xFFFFD1, 10, 1}, {0xFFFFD2, 9, 0}, {0xFFFFD3, 10, 0},
        {0xFFFFD4, 11, 1}, {0xFFFFD6, 11, 0},
    };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++)
        if (m[i].a == a) { *is_ddr = m[i].ddr; return m[i].port; }
    return 0;
}

static uint8_t onchip_r8(uint32_t a) {
    if (a < 0xFFFF1C) return onchip_ram[a & 0xFFF];
    int ch, off, ddr;
    if (itu_reg(a, &ch, &off)) {
        ItuCh *t = &itu[ch];
        switch (off) {
        case 0: return t->tcr; case 1: return t->tior; case 2: return t->tier | 0xF8; case 3: return t->tsr | 0xF8;
        case 4: return (uint8_t)(t->tcnt >> 8); case 5: return (uint8_t)t->tcnt;
        case 6: return (uint8_t)(t->gra >> 8); case 7: return (uint8_t)t->gra;
        case 8: return (uint8_t)(t->grb >> 8); case 9: return (uint8_t)t->grb;
        default: return onchip_ram[a & 0xFFF];
        }
    }
    int p = port_of(a, &ddr);
    if (p) {
        if (ddr) return 0xFF;
        if (p == 7) return h8port_r(7);
        return (uint8_t)((port_dr[p] & port_ddr[p]) | (h8port_r(p) & ~port_ddr[p]));
    }
    switch (a) {
    case 0xFFFF60: return tstr | 0xE0;
    case 0xFFFF61: return tsnc | 0xE0;
    case 0xFFFF62: return tmdr;
    case 0xFFFF63: return tfcr;
    case 0xFFFFF2: return syscr;
    case 0xFFFFF4: return iscr;
    case 0xFFFFF5: return ier;
    case 0xFFFFF6: return irq_isr;
    case 0xFFFFE0: case 0xFFFFE2: case 0xFFFFE4: case 0xFFFFE6: return (uint8_t)(adc_data[(a - 0xFFFFE0) >> 1] >> 2);
    case 0xFFFFE1: case 0xFFFFE3: case 0xFFFFE5: case 0xFFFFE7: return (uint8_t)(adc_data[(a - 0xFFFFE1) >> 1] << 6);
    case 0xFFFFE8: return adcsr;
    case 0xFFFFE9: return adcr;
    case 0xFFFFB4: case 0xFFFFBC: return 0x84;      /* SCI SSR: TDRE | TEND (transmit always ready) */
    }
    return onchip_ram[a & 0xFFF];
}

static void onchip_w8(uint32_t a, uint8_t v) {
    if (a < 0xFFFF1C) { onchip_ram[a & 0xFFF] = v; return; }
    int ch, off, ddr;
    if (itu_reg(a, &ch, &off)) {
        ItuCh *t = &itu[ch];
        switch (off) {
        case 0: t->tcr = v; return; case 1: t->tior = v; return; case 2: t->tier = v & 7; return;
        case 3: t->tsr &= v | 0xF8; return;                /* write 0 to clear a flag */
        case 4: t->tcnt = (uint16_t)((t->tcnt & 0xFF) | (v << 8)); return;
        case 5: t->tcnt = (uint16_t)((t->tcnt & 0xFF00) | v); return;
        case 6: t->gra = (uint16_t)((t->gra & 0xFF) | (v << 8)); return;
        case 7: t->gra = (uint16_t)((t->gra & 0xFF00) | v); return;
        case 8: t->grb = (uint16_t)((t->grb & 0xFF) | (v << 8)); return;
        case 9: t->grb = (uint16_t)((t->grb & 0xFF00) | v); return;
        default: onchip_ram[a & 0xFFF] = v; return;
        }
    }
    int p = port_of(a, &ddr);
    if (p) {
        if (ddr) port_ddr[p] = v; else port_dr[p] = v;
        if (!ddr) h8port_w(p, v);
        return;
    }
    switch (a) {
    case 0xFFFF60: tstr = v & 0x1F; return;
    case 0xFFFF61: tsnc = v; return;
    case 0xFFFF62: tmdr = v; return;
    case 0xFFFF63: tfcr = v; return;
    case 0xFFFFF2: syscr = v; return;
    case 0xFFFFF4: iscr = v; return;
    case 0xFFFFF5: ier = v; return;
    case 0xFFFFF6: irq_isr &= v; return;
    case 0xFFFFE8:
        adcsr = (uint8_t)((adcsr & 0x80 & v) | (v & 0x7F));
        if (v & 0x20) {                                 /* ADST: convert immediately */
            for (int i = 0; i < 4; i++) adc_data[i] = h8adc_r(i) & 0x3FF;
            adcsr = (uint8_t)((adcsr & ~0x20) | 0x80);  /* ADF */
        }
        return;
    case 0xFFFFE9: adcr = v; return;
    }
    if (rt_once(ONCE_MISC, 0x8A000000u | (a & 0xFFFF))) rt_log("h8: on-chip write %06X = %02X (unmodelled)", a, v);
    onchip_ram[a & 0xFFF] = v;
}

/* ------------------------------------------------------------------ memory */
static inline uint8_t rd8(uint32_t a) {
    a &= 0xFFFFFF;
    states += 2;
    return a >= 0xFFF000 ? onchip_r8(a) : h8ext_r8(a);
}
static inline uint16_t rd16(uint32_t a) {
    a &= 0xFFFFFE;
    states += 2;
    if (a >= 0xFFF000) return (uint16_t)((onchip_r8(a) << 8) | onchip_r8(a + 1));
    return h8ext_r16(a);
}
static inline uint32_t rd32(uint32_t a) { return ((uint32_t)rd16(a) << 16) | rd16(a + 2); }
static inline void wr8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    states += 2;
    if (a >= 0xFFF000) onchip_w8(a, v); else h8ext_w8(a, v);
}
static inline void wr16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFE;
    states += 2;
    if (a >= 0xFFF000) { onchip_w8(a, (uint8_t)(v >> 8)); onchip_w8(a + 1, (uint8_t)v); }
    else h8ext_w16(a, v);
}
static inline void wr32(uint32_t a, uint32_t v) { wr16(a, (uint16_t)(v >> 16)); wr16(a + 2, (uint16_t)v); }

static inline uint16_t fetch(void) { uint16_t v = rd16(PC); PC = (PC + 2) & 0xFFFFFF; return v; }

/* ------------------------------------------------------------------ registers */
static inline uint8_t RB(int r) { return r < 8 ? (uint8_t)(ER[r] >> 8) : (uint8_t)ER[r - 8]; }
static inline void WB(int r, uint8_t v) {
    if (r < 8) ER[r] = (ER[r] & 0xFFFF00FFu) | ((uint32_t)v << 8);
    else ER[r - 8] = (ER[r - 8] & 0xFFFFFF00u) | v;
}
static inline uint16_t RW(int r) { return r < 8 ? (uint16_t)ER[r] : (uint16_t)(ER[r - 8] >> 16); }
static inline void WW(int r, uint16_t v) {
    if (r < 8) ER[r] = (ER[r] & 0xFFFF0000u) | v;
    else ER[r - 8] = (ER[r - 8] & 0x0000FFFFu) | ((uint32_t)v << 16);
}

/* ------------------------------------------------------------------ flags */
static inline void set_nz(uint32_t v, int bits) {
    uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    CCR &= (uint8_t)~(F_N | F_Z);
    if ((v & mask) == 0) CCR |= F_Z;
    if (v & (1u << (bits - 1))) CCR |= F_N;
}
static inline void set_logic(uint32_t v, int bits) { set_nz(v, bits); CCR &= (uint8_t)~F_V; }

static uint32_t do_add(uint32_t a, uint32_t b, int c, int bits, int keepz) {
    uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1), sign = 1u << (bits - 1);
    uint32_t hm = (1u << (bits - 4)) - 1;
    a &= mask; b &= mask;
    uint64_t r64 = (uint64_t)a + b + (uint64_t)c;
    uint32_t r = (uint32_t)r64 & mask;
    int oldz = CCR & F_Z;
    CCR &= (uint8_t)~(F_H | F_N | F_Z | F_V | F_C);
    if (((a & hm) + (b & hm) + (uint32_t)c) > hm) CCR |= F_H;
    if (r & sign) CCR |= F_N;
    if (r == 0 && (!keepz || oldz)) CCR |= F_Z;
    if ((a ^ r) & (b ^ r) & sign) CCR |= F_V;
    if (r64 > mask) CCR |= F_C;
    return r;
}
static uint32_t do_sub(uint32_t a, uint32_t b, int c, int bits, int keepz) {
    uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1), sign = 1u << (bits - 1);
    uint32_t hm = (1u << (bits - 4)) - 1;
    a &= mask; b &= mask;
    uint32_t r = (a - b - (uint32_t)c) & mask;
    int oldz = CCR & F_Z;
    CCR &= (uint8_t)~(F_H | F_N | F_Z | F_V | F_C);
    if ((a & hm) < (b & hm) + (uint32_t)c) CCR |= F_H;
    if (r & sign) CCR |= F_N;
    if (r == 0 && (!keepz || oldz)) CCR |= F_Z;
    if ((a ^ b) & (a ^ r) & sign) CCR |= F_V;
    if ((uint64_t)b + (uint64_t)c > a) CCR |= F_C;
    return r;
}
/* INC/DEC: N Z V only */
static uint32_t do_incdec(uint32_t a, int delta, int bits) {
    uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1), sign = 1u << (bits - 1);
    uint32_t r = (a + (uint32_t)delta) & mask;
    CCR &= (uint8_t)~(F_N | F_Z | F_V);
    if (r & sign) CCR |= F_N;
    if (!r) CCR |= F_Z;
    if (delta > 0 ? ((~a & r) & sign) : ((a & ~r) & sign)) CCR |= F_V;
    return r;
}

/* shifts/rotates; kind: 0 SHLL 1 SHAL 2 SHLR 3 SHAR 4 ROTXL 5 ROTL 6 ROTXR 7 ROTR */
static uint32_t do_shift(uint32_t v, int kind, int bits) {
    uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1), sign = 1u << (bits - 1);
    v &= mask;
    int cin = CCR & F_C, cout = 0, ovf = 0;
    uint32_t r = 0;
    switch (kind) {
    case 0: cout = (v & sign) != 0; r = (v << 1) & mask; break;
    case 1: cout = (v & sign) != 0; r = (v << 1) & mask; ovf = ((v ^ r) & sign) != 0; break;
    case 2: cout = v & 1; r = v >> 1; break;
    case 3: cout = v & 1; r = (v >> 1) | (v & sign); break;
    case 4: cout = (v & sign) != 0; r = ((v << 1) | (uint32_t)(cin ? 1 : 0)) & mask; break;
    case 5: cout = (v & sign) != 0; r = ((v << 1) | (cout ? 1u : 0u)) & mask; break;
    case 6: cout = v & 1; r = (v >> 1) | (cin ? sign : 0); break;
    case 7: cout = v & 1; r = (v >> 1) | (cout ? sign : 0); break;
    }
    CCR &= (uint8_t)~(F_N | F_Z | F_V | F_C);
    if (r & sign) CCR |= F_N;
    if (!r) CCR |= F_Z;
    if (ovf) CCR |= F_V;
    if (cout) CCR |= F_C;
    return r;
}

static int cond(int c) {
    int C = CCR & F_C, V = (CCR & F_V) != 0, Z = CCR & F_Z, N = (CCR & F_N) != 0;
    switch (c & 15) {
    case 0: return 1;           case 1: return 0;
    case 2: return !(C || Z);   case 3: return C || Z;
    case 4: return !C;          case 5: return C != 0;
    case 6: return !Z;          case 7: return Z != 0;
    case 8: return !V;          case 9: return V;
    case 10: return !N;         case 11: return N;
    case 12: return N == V;     case 13: return N != V;
    case 14: return !Z && N == V;
    default: return Z || N != V;
    }
}

/* ------------------------------------------------------------------ bit operations */
/* op: second-byte opcode (60-63, 67, 70-77), inv: bit 7 of the bit-number nibble */
static int bitop(uint8_t *val, int op, int bit, int inv) {
    int b = (*val >> (bit & 7)) & 1, wrote = 0;
    switch (op) {
    case 0x60: case 0x70: *val |= (uint8_t)(1 << (bit & 7)); wrote = 1; break;               /* BSET */
    case 0x61: case 0x71: *val ^= (uint8_t)(1 << (bit & 7)); wrote = 1; break;               /* BNOT */
    case 0x62: case 0x72: *val &= (uint8_t)~(1 << (bit & 7)); wrote = 1; break;              /* BCLR */
    case 0x63: case 0x73: if (b) CCR &= (uint8_t)~F_Z; else CCR |= F_Z; break;               /* BTST */
    case 0x67: {                                                                            /* BST/BIST */
        int c = (CCR & F_C) != 0;
        if (inv) c = !c;
        if (c) *val |= (uint8_t)(1 << (bit & 7)); else *val &= (uint8_t)~(1 << (bit & 7));
        wrote = 1;
        break;
    }
    case 0x74: if (inv ? !b : b) CCR |= F_C; break;                                          /* BOR/BIOR */
    case 0x75: if (inv ? !b : b) CCR ^= F_C; break;                                          /* BXOR/BIXOR */
    case 0x76: if (!(inv ? !b : b)) CCR &= (uint8_t)~F_C; break;                             /* BAND/BIAND */
    case 0x77: if (inv ? !b : b) CCR |= F_C; else CCR &= (uint8_t)~F_C; break;               /* BLD/BILD */
    }
    return wrote;
}

/* ------------------------------------------------------------------ exceptions */
static void push32(uint32_t v) { ER[7] -= 4; wr32(ER[7], v); }
static uint32_t pop32(void) { uint32_t v = rd32(ER[7]); ER[7] += 4; return v; }

static void take_exception(int vec) {
    push32(((uint32_t)CCR << 24) | (PC & 0xFFFFFF));
    CCR |= F_I;
    PC = rd32((uint32_t)vec * 4) & 0xFFFFFF;
    sleeping = 0;
}

static int pending_vector(void) {
    /* external IRQ0-5: vectors 12-17 */
    for (int i = 0; i < 6; i++) {
        int edge = (iscr >> i) & 1;
        if (!edge) {                                                /* level-sensitive: flag follows the line */
            if (irq_line[i]) irq_isr |= (uint8_t)(1 << i); else irq_isr &= (uint8_t)~(1 << i);
        }
        if ((ier >> i) & 1 && (irq_isr >> i) & 1) {
            if (edge) irq_isr &= (uint8_t)~(1 << i);     /* edge requests clear when accepted */
            return 12 + i;
        }
    }
    for (int c = 0; c < 5; c++) {
        ItuCh *t = &itu[c];
        if ((t->tier & 1) && (t->tsr & 1)) return 24 + c * 4;
        if ((t->tier & 2) && (t->tsr & 2)) return 25 + c * 4;
        if ((t->tier & 4) && (t->tsr & 4)) return 26 + c * 4;
    }
    if ((adcsr & 0xC0) == 0xC0) return 60;
    return -1;
}

void h8_set_irq(int line, int state) {
    if (line < 0 || line > 5) return;
    int edge = (iscr >> line) & 1;
    if (edge && state && !irq_line[line]) irq_isr |= (uint8_t)(1 << line);   /* falling edge of /IRQ */
    irq_line[line] = (uint8_t)(state != 0);
}

/* ------------------------------------------------------------------ timers */
static void itu_advance(int st) {
    for (int c = 0; c < 5; c++) {
        if (!((tstr >> c) & 1)) continue;
        ItuCh *t = &itu[c];
        int tpsc = t->tcr & 7;
        if (tpsc > 3) continue;                     /* external clocks: not used */
        t->prescale_acc += (uint32_t)st;
        uint32_t ticks = t->prescale_acc >> tpsc;
        t->prescale_acc -= ticks << tpsc;
        int cclr = (t->tcr >> 5) & 3;
        while (ticks) {
            /* step to the next event to keep compare matches exact */
            uint32_t to_a = (uint16_t)(t->gra - t->tcnt), to_b = (uint16_t)(t->grb - t->tcnt);
            uint32_t to_ovf = 0x10000u - t->tcnt;
            uint32_t step = ticks;
            if (to_a && to_a < step) step = to_a;
            if (to_b && to_b < step) step = to_b;
            if (to_ovf < step) step = to_ovf;
            uint32_t n = t->tcnt + step;
            ticks -= step;
            if (n >= 0x10000u) { t->tsr |= 4; n &= 0xFFFF; }
            t->tcnt = (uint16_t)n;
            if (t->tcnt == t->gra) { t->tsr |= 1; if (cclr == 1) t->tcnt = 0; }
            if (t->tcnt == t->grb) { t->tsr |= 2; if (cclr == 2) t->tcnt = 0; }
        }
    }
}

/* ------------------------------------------------------------------ effective addresses */
static inline uint32_t abs16(uint16_t a) { return (uint32_t)(int32_t)(int16_t)a & 0xFFFFFF; }

void h8_reset(void) {
    memset(ER, 0, sizeof ER);
    memset(itu, 0, sizeof itu);
    memset(port_dr, 0, sizeof port_dr);
    memset(port_ddr, 0, sizeof port_ddr);
    tstr = 0; ier = 0; irq_isr = 0; iscr = 0; adcsr = 0; syscr = 0x0B;
    for (int c = 0; c < 5; c++) { itu[c].gra = itu[c].grb = 0xFFFF; }
    CCR = F_I;
    sleeping = 0;
    states = 0;
    PC = rd32(0) & 0xFFFFFF;
    rt_log("h8: reset, PC=%06X", PC);
}

uint32_t h8_pc(void) { return PC; }
uint64_t h8_total_cycles(void) { return total_states; }

static void illegal(uint16_t op, uint32_t at) {
    if (rt_once(ONCE_MISC, 0x8B000000u | op)) rt_log("h8: illegal/unimplemented opcode %04X at %06X", op, at);
}

/* MOV/LDC memory forms: compute address for 0x68-0x6F families and 0x78 (d24) */

static void step(void) {
    uint32_t at = PC;
    uint16_t op = fetch();
    int hi = op >> 8, lo = op & 0xFF, a4 = lo >> 4, b4 = lo & 15;
    uint32_t ea;
    uint16_t w2;

    switch (hi) {
    case 0x00: break;                                                           /* NOP */
    case 0x01: {
        w2 = fetch();
        if (lo == 0x80) { sleeping = 1; break; }                                /* SLEEP */
        if (lo == 0x00 || lo == 0x40) {
            int is_ccr = lo == 0x40, size = is_ccr ? 2 : 4;
            int h2 = w2 >> 8, l2 = w2 & 0xFF, s = (l2 >> 4) & 7, d = l2 & 7, st = (l2 & 0x80) != 0;
            uint32_t v = 0;
            int disp_reg = s;
            switch (h2) {
            case 0x69: ea = ER[s]; break;                                       /* @ERn */
            case 0x6B: {
                uint16_t w3 = fetch();
                if ((l2 & 0x70) == 0x20) { uint16_t w4 = fetch(); ea = ((uint32_t)(w3 & 0xFF) << 16) | w4; }
                else ea = abs16(w3);
                st = (l2 & 0x80) != 0;
                break;
            }
            case 0x6D:                                                          /* @ERn+ / @-ERn */
                if (st) { ER[s] -= (uint32_t)size; ea = ER[s]; }
                else { ea = ER[s]; ER[s] += (uint32_t)size; }
                break;
            case 0x6F: ea = (ER[s] + (uint32_t)(int32_t)(int16_t)fetch()) & 0xFFFFFF; break;
            case 0x78: {                                                        /* 78 e0 6B 2x/Ax xx DDDDDD */
                uint16_t w3 = fetch(), w4 = fetch(), w5 = fetch();
                ea = (ER[s] + (((uint32_t)(w4 & 0xFF) << 16) | w5)) & 0xFFFFFF;
                st = (w3 & 0x80) != 0;
                l2 = w3 & 0xFF; d = l2 & 7;
                break;
            }
            default: illegal(op, at); return;
            }
            (void)disp_reg;
            if (h2 == 0x6B || h2 == 0x78) { /* d already the low nibble */ }
            if (is_ccr) {
                if (st) wr16(ea, (uint16_t)((CCR << 8) | CCR));
                else CCR = (uint8_t)(rd16(ea) >> 8);
            } else {
                int reg = (h2 == 0x6B || h2 == 0x78) ? (l2 & 7) : d;
                if (st) { v = ER[reg]; wr32(ea, v); }
                else { v = rd32(ea); ER[reg] = v; }
                set_logic(v, 32);
            }
            break;
        }
        if (lo == 0xC0) {                                                       /* MULXS */
            int h2 = w2 >> 8, s = (w2 >> 4) & 15, d = w2 & 15;
            if (h2 == 0x50) { int32_t r = (int8_t)RB(s) * (int8_t)(RW(d) & 0xFF); WW(d, (uint16_t)r); set_nz((uint32_t)r, 16); }
            else if (h2 == 0x52) { int32_t r = (int16_t)RW(s) * (int16_t)(ER[d & 7] & 0xFFFF); ER[d & 7] = (uint32_t)r; set_nz((uint32_t)r, 32); }
            else illegal(op, at);
            states += 20;
            break;
        }
        if (lo == 0xD0) {                                                       /* DIVXS */
            int h2 = w2 >> 8, s = (w2 >> 4) & 15, d = w2 & 15;
            if (h2 == 0x51) {
                int8_t dv = (int8_t)RB(s); int16_t n = (int16_t)RW(d);
                CCR &= (uint8_t)~(F_N | F_Z);
                if (!dv) { CCR |= F_Z; break; }
                int q = n / dv, rm = n % dv;
                WW(d, (uint16_t)(((rm & 0xFF) << 8) | (q & 0xFF)));
                if (q < 0) CCR |= F_N;
            } else if (h2 == 0x53) {
                int16_t dv = (int16_t)RW(s); int32_t n = (int32_t)ER[d & 7];
                CCR &= (uint8_t)~(F_N | F_Z);
                if (!dv) { CCR |= F_Z; break; }
                int32_t q = n / dv, rm = n % dv;
                ER[d & 7] = ((uint32_t)(rm & 0xFFFF) << 16) | (uint32_t)(q & 0xFFFF);
                if (q < 0) CCR |= F_N;
            } else illegal(op, at);
            states += 20;
            break;
        }
        if (lo == 0xF0) {                                                       /* AND/OR/XOR.L ERs,ERd */
            int h2 = w2 >> 8, s = (w2 >> 4) & 7, d = w2 & 7;
            if (h2 == 0x64) ER[d] |= ER[s];
            else if (h2 == 0x65) ER[d] ^= ER[s];
            else if (h2 == 0x66) ER[d] &= ER[s];
            else { illegal(op, at); break; }
            set_logic(ER[d], 32);
            break;
        }
        illegal(op, at);
        break;
    }
    case 0x02: WB(b4, CCR); break;                                              /* STC CCR,Rd */
    case 0x03: CCR = RB(b4); break;                                             /* LDC Rs,CCR */
    case 0x04: CCR |= (uint8_t)lo; break;                                       /* ORC */
    case 0x05: CCR ^= (uint8_t)lo; break;                                       /* XORC */
    case 0x06: CCR &= (uint8_t)lo; break;                                       /* ANDC */
    case 0x07: CCR = (uint8_t)lo; break;                                        /* LDC #imm */
    case 0x08: WB(b4, (uint8_t)do_add(RB(b4), RB(a4), 0, 8, 0)); break;         /* ADD.B */
    case 0x09: WW(b4, (uint16_t)do_add(RW(b4), RW(a4), 0, 16, 0)); break;       /* ADD.W */
    case 0x0A:
        if (a4 == 0) WB(b4, (uint8_t)do_incdec(RB(b4), 1, 8));                  /* INC.B */
        else if (a4 & 8) ER[b4 & 7] = do_add(ER[b4 & 7], ER[a4 & 7], 0, 32, 0); /* ADD.L */
        else illegal(op, at);
        break;
    case 0x0B:
        switch (a4) {
        case 0x0: ER[b4 & 7] += 1; break;                                       /* ADDS #1 */
        case 0x8: ER[b4 & 7] += 2; break;
        case 0x9: ER[b4 & 7] += 4; break;
        case 0x5: WW(b4, (uint16_t)do_incdec(RW(b4), 1, 16)); break;            /* INC.W #1 */
        case 0xD: WW(b4, (uint16_t)do_incdec(RW(b4), 2, 16)); break;
        case 0x7: ER[b4 & 7] = do_incdec(ER[b4 & 7], 1, 32); break;             /* INC.L #1 */
        case 0xF: ER[b4 & 7] = do_incdec(ER[b4 & 7], 2, 32); break;
        default: illegal(op, at);
        }
        break;
    case 0x0C: { uint8_t v = RB(a4); WB(b4, v); set_logic(v, 8); break; }      /* MOV.B Rs,Rd */
    case 0x0D: { uint16_t v = RW(a4); WW(b4, v); set_logic(v, 16); break; }    /* MOV.W Rs,Rd */
    case 0x0E: WB(b4, (uint8_t)do_add(RB(b4), RB(a4), CCR & F_C, 8, 1)); break; /* ADDX.B */
    case 0x0F:
        if (a4 == 0) {                                                          /* DAA */
            uint8_t v = RB(b4); int c = CCR & F_C, h = CCR & F_H, adj = 0, nc = c;
            if (c || v > 0x99) { adj |= 0x60; nc = 1; }
            if (h || (v & 0x0F) > 9) adj |= 0x06;
            v = (uint8_t)(v + adj);
            WB(b4, v); set_nz(v, 8); if (nc) CCR |= F_C; else CCR &= (uint8_t)~F_C;
        } else if (a4 & 8) { ER[b4 & 7] = ER[a4 & 7]; set_logic(ER[b4 & 7], 32); } /* MOV.L */
        else illegal(op, at);
        break;
    case 0x10: case 0x11: case 0x12: case 0x13: {                               /* shifts/rotates */
        static const int kinds[4][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}};
        int kind = kinds[hi - 0x10][(a4 & 8) ? 1 : 0], sz = a4 & 7;
        if (sz == 0) WB(b4, (uint8_t)do_shift(RB(b4), kind, 8));
        else if (sz == 1) WW(b4, (uint16_t)do_shift(RW(b4), kind, 16));
        else if (sz == 3) ER[b4 & 7] = do_shift(ER[b4 & 7], kind, 32);
        else illegal(op, at);
        break;
    }
    case 0x14: { uint8_t v = RB(b4) | RB(a4); WB(b4, v); set_logic(v, 8); break; }
    case 0x15: { uint8_t v = RB(b4) ^ RB(a4); WB(b4, v); set_logic(v, 8); break; }
    case 0x16: { uint8_t v = RB(b4) & RB(a4); WB(b4, v); set_logic(v, 8); break; }
    case 0x17:
        switch (a4) {
        case 0x0: { uint8_t v = (uint8_t)~RB(b4); WB(b4, v); set_logic(v, 8); break; }
        case 0x1: { uint16_t v = (uint16_t)~RW(b4); WW(b4, v); set_logic(v, 16); break; }
        case 0x3: ER[b4 & 7] = ~ER[b4 & 7]; set_logic(ER[b4 & 7], 32); break;
        case 0x5: { uint16_t v = RW(b4) & 0xFF; WW(b4, v); set_logic(v, 16); break; }           /* EXTU.W */
        case 0x7: ER[b4 & 7] &= 0xFFFF; set_logic(ER[b4 & 7], 32); break;                       /* EXTU.L */
        case 0x8: WB(b4, (uint8_t)do_sub(0, RB(b4), 0, 8, 0)); break;                           /* NEG.B */
        case 0x9: WW(b4, (uint16_t)do_sub(0, RW(b4), 0, 16, 0)); break;
        case 0xB: ER[b4 & 7] = do_sub(0, ER[b4 & 7], 0, 32, 0); break;
        case 0xD: { uint16_t v = (uint16_t)(int16_t)(int8_t)RW(b4); WW(b4, v); set_logic(v, 16); break; } /* EXTS.W */
        case 0xF: ER[b4 & 7] = (uint32_t)(int32_t)(int16_t)ER[b4 & 7]; set_logic(ER[b4 & 7], 32); break;
        default: illegal(op, at);
        }
        break;
    case 0x18: WB(b4, (uint8_t)do_sub(RB(b4), RB(a4), 0, 8, 0)); break;         /* SUB.B */
    case 0x19: WW(b4, (uint16_t)do_sub(RW(b4), RW(a4), 0, 16, 0)); break;       /* SUB.W */
    case 0x1A:
        if (a4 == 0) WB(b4, (uint8_t)do_incdec(RB(b4), -1, 8));                 /* DEC.B */
        else if (a4 & 8) ER[b4 & 7] = do_sub(ER[b4 & 7], ER[a4 & 7], 0, 32, 0); /* SUB.L */
        else illegal(op, at);
        break;
    case 0x1B:
        switch (a4) {
        case 0x0: ER[b4 & 7] -= 1; break;                                       /* SUBS */
        case 0x8: ER[b4 & 7] -= 2; break;
        case 0x9: ER[b4 & 7] -= 4; break;
        case 0x5: WW(b4, (uint16_t)do_incdec(RW(b4), -1, 16)); break;           /* DEC.W */
        case 0xD: WW(b4, (uint16_t)do_incdec(RW(b4), -2, 16)); break;
        case 0x7: ER[b4 & 7] = do_incdec(ER[b4 & 7], -1, 32); break;            /* DEC.L */
        case 0xF: ER[b4 & 7] = do_incdec(ER[b4 & 7], -2, 32); break;
        default: illegal(op, at);
        }
        break;
    case 0x1C: do_sub(RB(b4), RB(a4), 0, 8, 0); break;                          /* CMP.B */
    case 0x1D: do_sub(RW(b4), RW(a4), 0, 16, 0); break;                         /* CMP.W */
    case 0x1E: WB(b4, (uint8_t)do_sub(RB(b4), RB(a4), CCR & F_C, 8, 1)); break; /* SUBX.B */
    case 0x1F:
        if (a4 == 0) {                                                          /* DAS */
            uint8_t v = RB(b4); int adj = 0;
            if (CCR & F_C) adj |= 0x60;
            if (CCR & F_H) adj |= 0x06;
            int c = CCR & F_C;
            v = (uint8_t)(v - adj);
            WB(b4, v); set_nz(v, 8); if (c) CCR |= F_C;
        } else if (a4 & 8) do_sub(ER[b4 & 7], ER[a4 & 7], 0, 32, 0);            /* CMP.L */
        else illegal(op, at);
        break;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
        uint8_t v = rd8(0xFFFF00u | (uint32_t)lo); WB(hi & 15, v); set_logic(v, 8); break;  /* MOV.B @aa:8,Rd */
    }
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: {
        uint8_t v = RB(hi & 15); wr8(0xFFFF00u | (uint32_t)lo, v); set_logic(v, 8); break;  /* MOV.B Rs,@aa:8 */
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        if (cond(hi)) { PC = (PC + (uint32_t)(int32_t)(int8_t)lo) & 0xFFFFFF; }
        break;
    case 0x50: { uint16_t r = (uint16_t)(RB(a4) * (RW(b4) & 0xFF)); WW(b4, r); states += 12; break; }  /* MULXU.B */
    case 0x51: {                                                                                        /* DIVXU.B */
        uint8_t dv = RB(a4); uint16_t n = RW(b4);
        CCR &= (uint8_t)~(F_N | F_Z);
        if (dv & 0x80) CCR |= F_N;
        if (!dv) { CCR |= F_Z; break; }
        WW(b4, (uint16_t)(((n % dv) << 8) | ((n / dv) & 0xFF)));
        states += 12;
        break;
    }
    case 0x52: { ER[b4 & 7] = (uint32_t)RW(a4) * (ER[b4 & 7] & 0xFFFF); states += 20; break; }         /* MULXU.W */
    case 0x53: {                                                                                        /* DIVXU.W */
        uint16_t dv = RW(a4); uint32_t n = ER[b4 & 7];
        CCR &= (uint8_t)~(F_N | F_Z);
        if (dv & 0x8000) CCR |= F_N;
        if (!dv) { CCR |= F_Z; break; }
        ER[b4 & 7] = ((n % dv) << 16) | ((n / dv) & 0xFFFF);
        states += 20;
        break;
    }
    case 0x54: PC = pop32() & 0xFFFFFF; break;                                  /* RTS */
    case 0x55: push32(PC); PC = (PC + (uint32_t)(int32_t)(int8_t)lo) & 0xFFFFFF; break;  /* BSR d8 */
    case 0x56: { uint32_t v = pop32(); CCR = (uint8_t)(v >> 24); PC = v & 0xFFFFFF; break; } /* RTE */
    case 0x57: take_exception(8 + ((lo >> 4) & 3)); break;                      /* TRAPA */
    case 0x58: { int16_t d = (int16_t)fetch(); if (cond(a4)) PC = (PC + (uint32_t)(int32_t)d) & 0xFFFFFF; break; }
    case 0x59: PC = ER[a4 & 7] & 0xFFFFFF; break;                               /* JMP @ERn */
    case 0x5A: { uint16_t w = fetch(); PC = ((uint32_t)lo << 16) | w; break; }  /* JMP @aa:24 */
    case 0x5B: PC = rd32(lo) & 0xFFFFFF; break;                                 /* JMP @@aa:8 */
    case 0x5C: { int16_t d = (int16_t)fetch(); push32(PC); PC = (PC + (uint32_t)(int32_t)d) & 0xFFFFFF; break; }
    case 0x5D: { uint32_t t = ER[a4 & 7] & 0xFFFFFF; push32(PC); PC = t; break; }
    case 0x5E: { uint16_t w = fetch(); push32(PC); PC = ((uint32_t)lo << 16) | w; break; }
    case 0x5F: { uint32_t t = rd32(lo) & 0xFFFFFF; push32(PC); PC = t; break; }
    case 0x60: case 0x61: case 0x62: case 0x63: {                               /* Bxxx Rn,Rd */
        uint8_t v = RB(b4);
        if (bitop(&v, hi, RB(a4) & 7, 0)) WB(b4, v);
        break;
    }
    case 0x64: { uint16_t v = RW(b4) | RW(a4); WW(b4, v); set_logic(v, 16); break; }
    case 0x65: { uint16_t v = RW(b4) ^ RW(a4); WW(b4, v); set_logic(v, 16); break; }
    case 0x66: { uint16_t v = RW(b4) & RW(a4); WW(b4, v); set_logic(v, 16); break; }
    case 0x67: { uint8_t v = RB(b4); bitop(&v, 0x67, a4 & 7, a4 & 8); WB(b4, v); break; }   /* BST/BIST */
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
        int word = hi & 1, st = 0, reg = b4;
        int sz = word ? 2 : 1;
        switch (hi) {
        case 0x68: case 0x69: ea = ER[a4 & 7]; st = a4 & 8; break;
        case 0x6A: case 0x6B: {
            uint16_t w = fetch();
            int mode = a4 & 7;
            st = a4 & 8;
            if (mode == 2) { uint16_t w3 = fetch(); ea = ((uint32_t)(w & 0xFF) << 16) | w3; }
            else ea = abs16(w);        /* mode 0 (and MOVFPE/MOVTPE 4) */
            break;
        }
        case 0x6C: case 0x6D:
            st = a4 & 8;
            if (st) { ER[a4 & 7] -= (uint32_t)sz; ea = ER[a4 & 7]; }
            else { ea = ER[a4 & 7]; ER[a4 & 7] += (uint32_t)sz; }
            break;
        default: ea = (ER[a4 & 7] + (uint32_t)(int32_t)(int16_t)fetch()) & 0xFFFFFF; st = a4 & 8; break;
        }
        if (word) {
            if (st) { uint16_t v = RW(reg); wr16(ea, v); set_logic(v, 16); }
            else { uint16_t v = rd16(ea); WW(reg, v); set_logic(v, 16); }
        } else {
            if (st) { uint8_t v = RB(reg); wr8(ea, v); set_logic(v, 8); }
            else { uint8_t v = rd8(ea); WB(reg, v); set_logic(v, 8); }
        }
        break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: {
        uint8_t v = RB(b4);
        if (bitop(&v, hi, a4 & 7, a4 & 8)) WB(b4, v);
        break;
    }
    case 0x78: {                                                                /* @(d24,ERn) byte/word */
        uint16_t w2b = fetch(), w3 = fetch(), w4 = fetch();
        int h2 = w2b >> 8, l2 = w2b & 0xFF, reg = l2 & 15, st = (l2 & 0x80) != 0;
        ea = (ER[a4 & 7] + (((uint32_t)(w3 & 0xFF) << 16) | w4)) & 0xFFFFFF;
        if (h2 == 0x6B) {
            if (st) { uint16_t v = RW(reg); wr16(ea, v); set_logic(v, 16); }
            else { uint16_t v = rd16(ea); WW(reg, v); set_logic(v, 16); }
        } else if (h2 == 0x6A) {
            if (st) { uint8_t v = RB(reg); wr8(ea, v); set_logic(v, 8); }
            else { uint8_t v = rd8(ea); WB(reg, v); set_logic(v, 8); }
        } else illegal(op, at);
        break;
    }
    case 0x79: {                                                                /* word immediate */
        uint16_t imm = fetch();
        switch (a4) {
        case 0: WW(b4, imm); set_logic(imm, 16); break;
        case 1: WW(b4, (uint16_t)do_add(RW(b4), imm, 0, 16, 0)); break;
        case 2: do_sub(RW(b4), imm, 0, 16, 0); break;
        case 3: WW(b4, (uint16_t)do_sub(RW(b4), imm, 0, 16, 0)); break;
        case 4: { uint16_t v = RW(b4) | imm; WW(b4, v); set_logic(v, 16); break; }
        case 5: { uint16_t v = RW(b4) ^ imm; WW(b4, v); set_logic(v, 16); break; }
        case 6: { uint16_t v = RW(b4) & imm; WW(b4, v); set_logic(v, 16); break; }
        default: illegal(op, at);
        }
        break;
    }
    case 0x7A: {                                                                /* long immediate */
        uint32_t imm = (uint32_t)fetch() << 16;
        imm |= fetch();
        int d = b4 & 7;
        switch (a4) {
        case 0: ER[d] = imm; set_logic(imm, 32); break;
        case 1: ER[d] = do_add(ER[d], imm, 0, 32, 0); break;
        case 2: do_sub(ER[d], imm, 0, 32, 0); break;
        case 3: ER[d] = do_sub(ER[d], imm, 0, 32, 0); break;
        case 4: ER[d] |= imm; set_logic(ER[d], 32); break;
        case 5: ER[d] ^= imm; set_logic(ER[d], 32); break;
        case 6: ER[d] &= imm; set_logic(ER[d], 32); break;
        default: illegal(op, at);
        }
        break;
    }
    case 0x7B: {                                                                /* EEPMOV */
        fetch();
        if (lo == 0x5C) {
            while (ER[4] & 0xFF) { wr8(ER[6], rd8(ER[5])); ER[5]++; ER[6]++; ER[4] = (ER[4] & ~0xFFu) | ((ER[4] - 1) & 0xFF); }
        } else {
            while (ER[4] & 0xFFFF) { wr8(ER[6], rd8(ER[5])); ER[5]++; ER[6]++; ER[4] = (ER[4] & ~0xFFFFu) | ((ER[4] - 1) & 0xFFFF); }
        }
        break;
    }
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {                               /* bit ops on memory */
        w2 = fetch();
        ea = (hi <= 0x7D) ? ER[a4 & 7] : (0xFFFF00u | (uint32_t)lo);
        int bop = w2 >> 8, bn = (w2 >> 4) & 15;
        uint8_t v = rd8(ea);
        int bit = (bop >= 0x60 && bop <= 0x63) ? (RB(bn) & 7) : (bn & 7);
        if (bitop(&v, bop, bit, (bop >= 0x67) ? (bn & 8) : 0)) wr8(ea, v);
        break;
    }
    default: {
        int r = hi & 15;
        uint8_t imm = (uint8_t)lo;
        switch (hi >> 4) {
        case 0x8: WB(r, (uint8_t)do_add(RB(r), imm, 0, 8, 0)); break;
        case 0x9: WB(r, (uint8_t)do_add(RB(r), imm, CCR & F_C, 8, 1)); break;
        case 0xA: do_sub(RB(r), imm, 0, 8, 0); break;
        case 0xB: WB(r, (uint8_t)do_sub(RB(r), imm, CCR & F_C, 8, 1)); break;
        case 0xC: { uint8_t v = RB(r) | imm; WB(r, v); set_logic(v, 8); break; }
        case 0xD: { uint8_t v = RB(r) ^ imm; WB(r, v); set_logic(v, 8); break; }
        case 0xE: { uint8_t v = RB(r) & imm; WB(r, v); set_logic(v, 8); break; }
        case 0xF: WB(r, imm); set_logic(imm, 8); break;
        default: illegal(op, at);
        }
    }
    }
}

/* debug: histogram of executed PCs, dumped by h8_debug_dump() */
#define PCH 64
static uint32_t pch_pc[PCH], pch_n[PCH];
static void pc_hist(uint32_t pc) {
    for (int i = 0; i < PCH; i++) {
        if (pch_pc[i] == pc) { pch_n[i]++; return; }
        if (!pch_n[i]) { pch_pc[i] = pc; pch_n[i] = 1; return; }
    }
}
void h8_debug_dump(void) {
    char buf[600]; int o = 0;
    for (int k = 0; k < 8; k++) {
        int b = -1;
        for (int i = 0; i < PCH; i++) if (pch_n[i] && (b < 0 || pch_n[i] > pch_n[b])) b = i;
        if (b < 0) break;
        o += snprintf(buf + o, sizeof buf - o, " %06X:%u", pch_pc[b], pch_n[b]);
        pch_n[b] = 0;
    }
    rt_log("h8 hot:%s  ccr=%02X ier=%02X isr=%02X tstr=%02X sleep=%d", buf, CCR, ier, irq_isr, tstr, sleeping);
    memset(pch_n, 0, sizeof pch_n);
    memset(pch_pc, 0, sizeof pch_pc);
}

int h8_run(int cycles) {
    states = 0;
    pc_hist(PC);
    while (states < cycles) {
        int64_t before = states;
        if (!(CCR & F_I)) {
            int v = pending_vector();
            if (v >= 0) take_exception(v);
        }
        if (sleeping) {
            /* idle until a timer or IRQ fires: advance time in small chunks */
            states += 16;
        } else {
            step();
        }
        itu_advance((int)(states - before));
    }
    total_states += (uint64_t)states;
    return (int)states;
}
