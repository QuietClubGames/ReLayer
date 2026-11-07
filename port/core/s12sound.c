/*
 * System 12 sound / I-O sub-board glue. H8 memory map (from MAME's namcos12 driver):
 *   0x000000-0x07FFFF  program ROM (512 KB, big-endian)
 *   0x080000-0x08FFFF  RAM shared with the main CPU (main CPU sees the first 16 KB)
 *   0x280000-0x287FFF  C352 registers (16-bit)
 *   0x300000-0x300003  cabinet inputs (JAMMA)
 *   0x300010, 0x300030 dummy writes (wait states)
 * Port 7 = DIP switches, port B bit 7 = vblank, IRQ1 = vblank.
 */
#include "s12sound.h"
#include "exsnd.h"
#include "h8.h"
#include "c352.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

static uint8_t *rom;                /* big-endian H8 image */
static uint32_t rom_size;
/* Shared RAM is 16-bit wide. Stored here in the main CPU's little-endian byte order:
 * the H8 (big-endian) sees byte address a at g_shared[a ^ 1]; 16-bit words are identical on both sides. */
static uint8_t g_shared[0x10000];
static uint8_t portb = 0x50;
static int vblank_state;

/* ---- audio ring (88200 Hz stereo) ---- */
#define RING 32768
static int16_t ring[RING * 2];
static uint32_t ring_w, ring_r;
static uint32_t sample_phase;       /* H8 states not yet turned into samples */

/* ---------------------------------------------------------------- inputs */
static uint8_t bits8(uint16_t v, const int *src) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) if ((v >> src[i]) & 1) r |= (uint8_t)(0x80 >> i);
    return r;
}

static uint8_t in_r(int off) {
    S12Inputs in;
    memset(&in, 0, sizeof in);
    plat_read_inputs(&in);
    static const int pl[8] = {7, 15, 0, 1, 5, 4, 3, 2};   /* START B3 B2 B1 UP DOWN LEFT RIGHT */
    uint16_t p1 = in.player[0], p2 = in.player[1];
    switch (off & 3) {
    case 0: return (uint8_t)~bits8(p2, pl);
    case 1: return (uint8_t)~bits8(p1, pl);
    case 2: return (uint8_t)~((((p1 >> 6) & 1) << 7) | ((in.test & 1) << 6) | ((in.coin[0] & 1) << 5) |
                              ((in.coin[1] & 1) << 4) | (((p1 >> 11) & 1) << 3) | (((p2 >> 11) & 1) << 2) |
                              (((p1 >> 10) & 1) << 1) | ((p2 >> 10) & 1));
    default: return (uint8_t)~((((p2 >> 12) & 1) << 7) | (((p2 >> 13) & 1) << 6) | (((p2 >> 14) & 1) << 5) |
                               (((p1 >> 12) & 1) << 3) | (((p1 >> 13) & 1) << 2) | (((p1 >> 14) & 1) << 1));
    }
}

/* ---------------------------------------------------------------- H8 external bus */
uint8_t h8ext_r8(uint32_t a) {
    if (a < 0x080000) return a < rom_size ? rom[a] : 0xFF;
    if (a < 0x090000) return g_shared[(a & 0xFFFF) ^ 1];
    if (a >= 0x280000 && a < 0x288000) {
        uint16_t w = c352_read((int)((a - 0x280000) >> 1));
        return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
    }
    if (a >= 0x300000 && a < 0x300004) return in_r((int)(a & 3));
    if (rt_once(ONCE_MISC, 0x8C000000u | a)) rt_log("h8: read8 %06X (unmapped) pc=%06X", a, h8_pc());
    return 0xFF;
}

uint16_t h8ext_r16(uint32_t a) {
    if (a < 0x080000) return a + 1 < rom_size ? (uint16_t)((rom[a] << 8) | rom[a + 1]) : 0xFFFF;
    if (a < 0x090000) { uint32_t o = a & 0xFFFE; return (uint16_t)(g_shared[o] | (g_shared[o + 1] << 8)); }
    if (a >= 0x280000 && a < 0x288000) return c352_read((int)((a - 0x280000) >> 1));
    if (a >= 0x300000 && a < 0x300004) return (uint16_t)((in_r((int)(a & 3)) << 8) | in_r((int)((a + 1) & 3)));
    if (rt_once(ONCE_MISC, 0x8C000000u | a)) rt_log("h8: read16 %06X (unmapped) pc=%06X", a, h8_pc());
    return 0xFFFF;
}

void h8ext_w8(uint32_t a, uint8_t v) {
    if (a < 0x080000) return;
    if (a < 0x090000) { g_shared[(a & 0xFFFF) ^ 1] = v; return; }
    if (a >= 0x280000 && a < 0x288000) {
        int off = (int)((a - 0x280000) >> 1);
        if (a & 1) c352_write(off, v, 0x00FF); else c352_write(off, (uint16_t)(v << 8), 0xFF00);
        return;
    }
    if (a == 0x300010 || a == 0x300011 || a == 0x300030 || a == 0x300031) return;
    if (rt_once(ONCE_MISC, 0x8D000000u | a)) rt_log("h8: write8 %06X = %02X (unmapped) pc=%06X", a, v, h8_pc());
}

void h8ext_w16(uint32_t a, uint16_t v) {
    if (a < 0x080000) return;
    if (a < 0x090000) { uint32_t o = a & 0xFFFE; g_shared[o] = (uint8_t)v; g_shared[o + 1] = (uint8_t)(v >> 8); return; }
    if (a >= 0x280000 && a < 0x288000) { c352_write((int)((a - 0x280000) >> 1), v, 0xFFFF); return; }
    if (a == 0x300010 || a == 0x300030) return;
    if (rt_once(ONCE_MISC, 0x8D000000u | a)) rt_log("h8: write16 %06X = %04X (unmapped) pc=%06X", a, v, h8_pc());
}

uint8_t h8port_r(int port) {
    switch (port) {
    case 6: return 0xFF;                       /* bit 1: JVS sense (no JVS board) */
    case 7: {                                  /* DIP switches: bit 7 service mode (active low) */
        S12Inputs in;
        memset(&in, 0, sizeof in);
        plat_read_inputs(&in);
        return in.service_dip ? 0x7F : 0xFF;
    }
    case 8: return 0xEF;                       /* bit 4: JVS present = 0 */
    case 10: return 0xFF;                      /* port A */
    case 11: return (uint8_t)(portb | 0x7F);   /* port B: bit 7 = vblank */
    default: return 0xFF;
    }
}

void h8port_w(int port, uint8_t v) {
    if (port == 11) portb = (uint8_t)((portb & 0x80) | (v & 0x7F));
}

uint16_t h8adc_r(int ch) { (void)ch; return 0; }

/* ---------------------------------------------------------------- main CPU side */
uint32_t s12sound_shared_r(uint32_t ofs, int size) {
    uint32_t v = 0;
    ofs &= 0x3FFF;
    memcpy(&v, g_shared + ofs, (size_t)size);
    return v;
}

void s12sound_shared_w(uint32_t ofs, uint32_t v, int size) {
    ofs &= 0x3FFF;
    memcpy(g_shared + ofs, &v, (size_t)size);
}

/* ---------------------------------------------------------------- run / audio */
void s12sound_init(const uint8_t *h8_rom_le, uint32_t size, const uint8_t *wave, uint32_t wave_size) {
    /* the ROM dump is stored with 16-bit words byte-swapped (MAME ROM_LOAD16_WORD_SWAP) */
    rom = malloc(size);
    rom_size = size;
    for (uint32_t i = 0; i + 1 < size; i += 2) { rom[i] = h8_rom_le[i + 1]; rom[i + 1] = h8_rom_le[i]; }
    c352_init(wave, wave_size);
    h8_reset();
}

void s12sound_vblank(int state) {
    vblank_state = state;
    portb = (uint8_t)((portb & 0x7F) | (state ? 0x80 : 0));
    h8_set_irq(1, state);
}

void s12sound_run(int h8_states) {
    int used = h8_run(h8_states);
    /* 16934400 / 88200 = 192 H8 states per C352 sample */
    sample_phase += (uint32_t)used;
    int n = (int)(sample_phase / 192);
    sample_phase -= (uint32_t)n * 192;
    while (n > 0) {
        int16_t tmp[256 * 2];
        int k = n > 256 ? 256 : n;
        c352_render(tmp, k);
        exsnd_mix(tmp, k);                                    /* a converted character's own voice */
        for (int i = 0; i < k; i++) {
            if (ring_w - ring_r >= RING) ring_r++;            /* overflow: drop oldest */
            uint32_t w = ring_w % RING;
            ring[w * 2] = tmp[i * 2];
            ring[w * 2 + 1] = tmp[i * 2 + 1];
            ring_w++;
        }
        n -= k;
    }
}

int s12sound_audio_take_raw(int16_t *out, int max_frames) {
    int n = 0;
    for (; n < max_frames && ring_w != ring_r; n++) {
        uint32_t a = ring_r % RING;
        out[n * 2] = ring[a * 2];
        out[n * 2 + 1] = ring[a * 2 + 1];
        ring_r++;
    }
    return n;
}

int s12sound_audio_pull(int16_t *out, int frames) {
    int got = 0;
    for (; got < frames && ring_w - ring_r >= 2; got++) {
        uint32_t a = ring_r % RING, b = (ring_r + 1) % RING;
        out[got * 2] = (int16_t)((ring[a * 2] + ring[b * 2]) / 2);
        out[got * 2 + 1] = (int16_t)((ring[a * 2 + 1] + ring[b * 2 + 1]) / 2);
        ring_r += 2;
    }
    for (int i = got; i < frames; i++) out[i * 2] = out[i * 2 + 1] = 0;
    return got;
}
