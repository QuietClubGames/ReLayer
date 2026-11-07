#pragma once
#include <stdint.h>

/* High-level replacement for the Sony kernel's A0/B0/C0 call tables. */
void bios_call(int table, uint32_t fn);
