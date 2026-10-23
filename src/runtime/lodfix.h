#ifndef NFLS2_LODFIX_H
#define NFLS2_LODFIX_H
#include "cpu/cpu.h"

/* Ultrawide shapes: the engine's level-of-detail thresholds matched to the
   wider camera (see lodfix.c). Once per video field. */
/* Returns whether a match looks to be on (players registered). */
bool lodfix_frame(CPUState* cpu);

#endif
