#ifndef NFLS2_HLE_CPU_H
#define NFLS2_HLE_CPU_H

#include "cpu/cpu.h"

/* Installs the handlers for instructions the recompiler leaves to the runtime:
   supervisor SPR access, cache control, and anything else unknown. */
void hle_cpu_install(CPUState* cpu);

/* Counts of what the handlers have seen, for the end-of-run report. */
void hle_cpu_report(void);

#endif
