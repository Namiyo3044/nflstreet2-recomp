#ifndef NFLS2_MMIO_H
#define NFLS2_MMIO_H

#include "cpu/cpu.h"

/* Hardware registers at 0xCC000000-0xCC00FFFF (VI, PI, DSP, DI, SI, EXI, AI,
   and the graphics write-gather pipe). Each device is modelled only as far as
   the game needs; everything else is a plain register file that remembers
   writes, and the first access to each register is logged. */
void mmio_install(CPUState* cpu);

void mmio_report(void);

/* Interrupt plumbing used by interrupts.c. */
u32 mmio_pi_cause(void);         /* PI interrupt cause register, from device state */
u32 mmio_pi_mask(void);          /* PI interrupt mask register */
void mmio_vi_raise(u32 which);   /* assert VI display interrupt DI0-DI3 */

/* Pixel-engine events from the graphics command stream. */
void mmio_pe_finish(void);
void mmio_pe_token(u16 token, bool interrupt);

#endif
