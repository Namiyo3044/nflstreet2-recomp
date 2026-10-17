#ifndef NFLS2_MMIO_H
#define NFLS2_MMIO_H

#include "cpu/cpu.h"

/* Hardware registers at 0xCC000000-0xCC00FFFF (VI, PI, DSP, DI, SI, EXI, AI,
   and the graphics write-gather pipe). Each device is modelled only as far as
   the game needs; everything else is a plain register file that remembers
   writes, and the first access to each register is logged. */
void mmio_install(CPUState* cpu);

void mmio_report(void);

#endif
