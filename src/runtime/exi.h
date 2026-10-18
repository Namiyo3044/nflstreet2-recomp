#ifndef NFLS2_EXI_H
#define NFLS2_EXI_H

#include "cpu/cpu.h"

/* External interface: memory cards and the IPL chip (RTC, SRAM). See exi.c. */
void exi_init(void);

/* Puts a memory card image file in slot 0 (A) or 1 (B); creates a new,
   unformatted card if the file doesn't exist. */
bool exi_insert_memcard(int slot, const char* path);

bool exi_read(u32 off, u8 size, u32* out);
bool exi_write(CPUState* cpu, u32 off, u32 value, u8 size);
bool exi_interrupt_pending(void);
void exi_report(void);

#endif
