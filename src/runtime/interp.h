#ifndef NFLS2_INTERP_H
#define NFLS2_INTERP_H

#include "cpu/cpu.h"

/* Interpreter for code the game creates in memory at run time (see interp.c). */
typedef enum {
    INTERP_BRANCHED,      /* took a branch: dispatch the new pc */
    INTERP_BUDGET,        /* ran max_insns without branching away */
    INTERP_UNSUPPORTED,   /* hit an instruction it doesn't know (pc left on it) */
    INTERP_BAD_ADDRESS,   /* pc is not in RAM */
} InterpResult;

InterpResult interp_run(CPUState* cpu, u32 max_insns);
void interp_report(void);

#endif
