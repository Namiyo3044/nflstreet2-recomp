#ifndef NFLS2_HLE_EXCEPTIONS_H
#define NFLS2_HLE_EXCEPTIONS_H

#include "cpu/cpu.h"

/* Handles an exception the way the OS's low-memory handler would and resumes
   execution. Returns false for exceptions that aren't handled (yet). */
bool hle_handle_exception(CPUState* cpu);

void hle_exceptions_report(void);

#endif
