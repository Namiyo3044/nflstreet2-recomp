#ifndef NFLS2_TEXTSCAN_H
#define NFLS2_TEXTSCAN_H
#include "cpu/cpu.h"

/* Search guest memory for on-screen text (F8): finds every copy of the
   phrase in RAM and the virtual memory window, as plain bytes or as 16-bit
   characters, and logs each with its surroundings and the disc file it was
   loaded from. Lines start with "text:". */
void textscan_run(CPUState* cpu, const char* phrase);

#endif
