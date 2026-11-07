#pragma once
#include <stdint.h>

/*
 * Hitachi H8/3002 (H8/300H core, advanced mode) interpreter with the on-chip peripherals the
 * System 12 sound/I-O program uses: interrupt controller, 16-bit timer unit (ITU), I/O ports,
 * plus simple serial and A/D converter stand-ins.
 */

void     h8_reset(void);
/* Run for about `cycles` CPU states; returns states actually used. */
int      h8_run(int cycles);
/* External interrupt request line (IRQ0-5), level: 1 = asserted */
void     h8_set_irq(int line, int state);
uint32_t h8_pc(void);
uint64_t h8_total_cycles(void);
void     h8_debug_dump(void);

/* ---- board callbacks (implemented by the board, e.g. s12sound.c) ---- */
uint8_t  h8ext_r8(uint32_t a);
uint16_t h8ext_r16(uint32_t a);
void     h8ext_w8(uint32_t a, uint8_t v);
void     h8ext_w16(uint32_t a, uint16_t v);
uint8_t  h8port_r(int port);            /* external pin levels: port 1..9, 10 = A, 11 = B */
void     h8port_w(int port, uint8_t v);
uint16_t h8adc_r(int ch);               /* 10-bit */
