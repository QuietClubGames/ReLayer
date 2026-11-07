/*
 * Namco C352: 32-voice PCM chip (8-bit linear / mu-law samples, 4 outputs).
 * Behaviour follows MAME's c352.cpp (BSD-3-Clause, R. Belmont / superctr): register map, flag bits,
 * volume ramping, linear interpolation, noise LFSR and looping rules. This is an independent C version.
 */
#include "c352.h"
#include <string.h>

enum {
    FLG_BUSY = 0x8000, FLG_KEYON = 0x4000, FLG_KEYOFF = 0x2000, FLG_LOOPTRG = 0x1000,
    FLG_LOOPHIST = 0x0800, FLG_FM = 0x0400, FLG_PHASERL = 0x0200, FLG_PHASEFL = 0x0100,
    FLG_PHASEFR = 0x0080, FLG_LDIR = 0x0040, FLG_LINK = 0x0020, FLG_NOISE = 0x0010,
    FLG_MULAW = 0x0008, FLG_FILTER = 0x0004, FLG_LOOP = 0x0002, FLG_REVERSE = 0x0001
};

/* register order inside a voice: vol_f, vol_r, freq, flags, wave_bank, wave_start, wave_end, wave_loop */
typedef struct {
    uint32_t pos, counter;
    int16_t sample, last_sample;
    uint16_t reg[8];
    uint8_t curr_vol[4];
} Voice;
#define VOL_F reg[0]
#define VOL_R reg[1]
#define FREQ  reg[2]
#define FLAGS reg[3]
#define BANK  reg[4]
#define START reg[5]
#define END   reg[6]
#define LOOP  reg[7]

static Voice v[32];
static uint16_t control, rnd = 0x1234;
static int16_t mulaw[256];
static const uint8_t *rom;
static uint32_t rom_mask;

void c352_init(const uint8_t *wave_rom, uint32_t size) {
    rom = wave_rom;
    rom_mask = size - 1;   /* size is a power of two (16 MB) */
    int j = 0;
    for (int i = 0; i < 128; i++) {
        mulaw[i] = (int16_t)(j << 5);
        if (i < 16) j += 1;
        else if (i < 24) j += 2;
        else if (i < 48) j += 4;
        else if (i < 100) j += 8;
        else j += 16;
    }
    for (int i = 0; i < 128; i++) mulaw[i + 128] = (int16_t)((~mulaw[i]) & 0xFFE0);
    c352_reset();
}

void c352_reset(void) {
    memset(v, 0, sizeof v);
    rnd = 0x1234;
    control = 0;
}

uint16_t c352_read(int off) {
    if (off < 0x100) return v[off / 8].reg[off % 8];
    if (off == 0x200) return control;
    return 0;
}

void c352_write(int off, uint16_t data, uint16_t mask) {
    if (off < 0x100) {
        uint16_t *r = &v[off / 8].reg[off % 8];
        *r = (uint16_t)((*r & ~mask) | (data & mask));
    } else if (off == 0x200) {
        control = (uint16_t)((control & ~mask) | (data & mask));
    } else if (off == 0x202) {          /* execute key-ons / key-offs (16-bit writes only) */
        if (mask != 0xFFFF) return;
        for (int i = 0; i < 32; i++) {
            Voice *c = &v[i];
            if (c->FLAGS & FLG_KEYON) {
                c->pos = ((uint32_t)c->BANK << 16) | c->START;
                c->sample = c->last_sample = 0;
                c->counter = 0xFFFF;
                c->FLAGS |= FLG_BUSY;
                c->FLAGS &= (uint16_t)~(FLG_KEYON | FLG_LOOPHIST);
                memset(c->curr_vol, 0, 4);
            }
            if (c->FLAGS & FLG_KEYOFF) {
                c->FLAGS &= (uint16_t)~(FLG_BUSY | FLG_KEYOFF);
                c->counter = 0xFFFF;
            }
        }
    }
}

static void fetch_sample(Voice *c) {
    c->last_sample = c->sample;
    if (c->FLAGS & FLG_NOISE) {
        rnd = (uint16_t)((rnd >> 1) ^ ((-(rnd & 1)) & 0xFFF6));
        c->sample = (int16_t)rnd;
        return;
    }
    int8_t s = (int8_t)rom[c->pos & rom_mask];
    c->sample = (c->FLAGS & FLG_MULAW) ? mulaw[(uint8_t)s] : (int16_t)(s * 256);
    uint16_t pos = (uint16_t)c->pos;
    if ((c->FLAGS & FLG_LOOP) && (c->FLAGS & FLG_REVERSE)) {
        if ((c->FLAGS & FLG_LDIR) && pos == c->LOOP) c->FLAGS &= (uint16_t)~FLG_LDIR;
        else if (!(c->FLAGS & FLG_LDIR) && pos == c->END) c->FLAGS |= FLG_LDIR;
        c->pos += (c->FLAGS & FLG_LDIR) ? (uint32_t)-1 : 1u;
    } else if (pos == c->END) {
        if ((c->FLAGS & FLG_LINK) && (c->FLAGS & FLG_LOOP)) {
            c->pos = ((uint32_t)c->START << 16) | c->LOOP;
            c->FLAGS |= FLG_LOOPHIST;
        } else if (c->FLAGS & FLG_LOOP) {
            c->pos = (c->pos & 0xFF0000u) | c->LOOP;
            c->FLAGS |= FLG_LOOPHIST;
        } else {
            c->FLAGS |= FLG_KEYOFF;
            c->FLAGS &= (uint16_t)~FLG_BUSY;
            c->sample = 0;
        }
    } else {
        c->pos += (c->FLAGS & FLG_REVERSE) ? (uint32_t)-1 : 1u;
    }
}

static inline void ramp(Voice *c, int ch, uint8_t target) {
    int d = (int)c->curr_vol[ch] - (int)target;
    if (d) c->curr_vol[ch] = (uint8_t)(c->curr_vol[ch] + (d > 0 ? -1 : 1));
}

void c352_render(int16_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int acc[4] = {0, 0, 0, 0};
        for (int j = 0; j < 32; j++) {
            Voice *c = &v[j];
            int s = 0;
            if (c->FLAGS & FLG_BUSY) {
                uint32_t next = c->counter + c->FREQ;
                if (next & 0x10000) fetch_sample(c);
                if ((next ^ c->counter) & 0x18000) {
                    ramp(c, 0, (uint8_t)(c->VOL_F >> 8));
                    ramp(c, 1, (uint8_t)c->VOL_F);
                    ramp(c, 2, (uint8_t)(c->VOL_R >> 8));
                    ramp(c, 3, (uint8_t)c->VOL_R);
                }
                c->counter = next & 0xFFFF;
                s = c->sample;
                if (!(c->FLAGS & FLG_FILTER))
                    s = (int16_t)(c->last_sample + (int32_t)((c->counter * (uint32_t)(c->sample - c->last_sample)) >> 16));
            }
            acc[0] += (((c->FLAGS & FLG_PHASEFL) ? -s : s) * c->curr_vol[0]) >> 8;
            acc[2] += (((c->FLAGS & FLG_PHASERL) ? -s : s) * c->curr_vol[2]) >> 8;
            acc[1] += (((c->FLAGS & FLG_PHASEFR) ? -s : s) * c->curr_vol[1]) >> 8;
            acc[3] += (((c->FLAGS & FLG_PHASEFR) ? -s : s) * c->curr_vol[3]) >> 8;
        }
        /* System 12 only wires the front pair to the speakers */
        out[i * 2 + 0] = (int16_t)(acc[0] >> 3);
        out[i * 2 + 1] = (int16_t)(acc[1] >> 3);
    }
}
