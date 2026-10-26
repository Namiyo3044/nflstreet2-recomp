#ifndef NFLS2_EXI_H
#define NFLS2_EXI_H

#include "cpu/cpu.h"

/* External interface: memory cards and the IPL chip (RTC, SRAM). See exi.c. */
void exi_init(void);

/* Puts a memory card image file in slot 0 (A) or 1 (B); creates a new,
   unformatted card if the file doesn't exist. */
/* Read-only: what the game saves isn't written back to the card file (for
   repeatable runs). */
void exi_set_memcard_readonly(bool on);
bool exi_insert_memcard(int slot, const char* path);
/* A card from memory (netplay: the host's), never written to a file. */
bool exi_insert_memcard_data(int slot, const u8* data, u32 size, const char* name);

bool exi_read(CPUState* cpu, u32 off, u8 size, u32* out);
bool exi_write(CPUState* cpu, u32 off, u32 value, u8 size);
bool exi_interrupt_pending(void);

/* Finishes memory card transfers and erases/programs that are due by now
   (timebase ticks); the time of the next one (~0 if none). */
void exi_tick(u64 now);
u64 exi_next_event(void);
void exi_report(void);

#endif
