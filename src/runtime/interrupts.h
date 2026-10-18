#ifndef NFLS2_INTERRUPTS_H
#define NFLS2_INTERRUPTS_H

#include "cpu/cpu.h"

/* Advances device timers (video retrace) and, if interrupts are enabled and
   one is pending, enters the OS interrupt handler. Call between steps. */
void interrupts_poll(CPUState* cpu);

/* Game time so far, in seconds, counted in video fields. */
double interrupts_game_seconds(void);

/* Timebase value at which the next timed event (video field, audio DMA) is due. */
u64 interrupts_next_event(void);

/* Optional function called once at the start of every video field (the live
   window uses it to show the frame, read input and keep real-time pace). */
void interrupts_set_field_callback(void (*cb)(CPUState* cpu));

void interrupts_report(void);

#endif
