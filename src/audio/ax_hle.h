#ifndef NFLS2_AX_HLE_H
#define NFLS2_AX_HLE_H

#include "cpu/cpu.h"

/* Native stand-in for the AX audio program that normally runs on the DSP.
 *
 * Each 5 ms audio frame, AX hands the DSP a command list. Among other things
 * it points at the list of voice parameter blocks (PBs). For every playing
 * voice the DSP advances the play position by one frame's worth of samples
 * (at the voice's pitch), loops or stops it at the end of its sample, and
 * writes the state back for the game to read.
 *
 * This does that bookkeeping exactly, so sounds start, loop and finish on
 * time and code waiting for a sound to end carries on. It does not produce
 * any audio samples yet; mixing comes later.
 */
void ax_hle_command_list(CPUState* cpu, u32 address);

void ax_hle_report(void);

#endif
