#pragma once
#include <stdint.h>
#include <stdio.h>

/* System 12 / PS1 hardware (I/O space 0x1F000000-0x1FBFFFFF). */
uint32_t hw_read(uint32_t phys, int size);
void     hw_write(uint32_t phys, uint32_t v, int size);
void     hw_irq_raise(int bit);
void     hw_irq_check(void);
void     hw_init(void);
void     eeprom_flush(void);
uint8_t  eeprom_get(int idx);                 /* settings EEPROM, byte index 0-0x7FF */
void     eeprom_set(int idx, uint8_t v);
/* host local time for the board's real-time clock (implemented by the platform layer) */
void     plat_local_time(int *year, int *month, int *day, int *weekday, int *hour, int *minute, int *second);
extern int g_eeprom_volatile;   /* set before running: blank EEPROM in memory only */

/* cpu.c */
int      cpu_irq_enabled(void);
void     cpu_set_sr(uint32_t v);
uint32_t cpu_get_sr(void);

/* bios.c: deliver a hardware interrupt to whatever the game hooked */
void     bios_interrupt(void);

/* ROM images (loaded by the platform layer) */
extern uint8_t *g_prg_rom;      /* 4 MB, mapped at 0x1FC00000 */
extern uint8_t *g_banked_rom;   /* 32 MB */
extern uint32_t g_banked_size;

extern uint64_t g_frame;
