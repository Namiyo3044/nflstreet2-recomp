#ifndef NFLS2_INTERRUPTS_H
#define NFLS2_INTERRUPTS_H

#include "cpu/cpu.h"

/* Advances device timers (video retrace) and, if interrupts are enabled and
   one is pending, enters the OS interrupt handler. Call between steps. */
void interrupts_poll(CPUState* cpu);

void interrupts_report(void);

#endif
