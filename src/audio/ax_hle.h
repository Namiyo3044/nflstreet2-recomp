#ifndef NFLS2_AX_HLE_H
#define NFLS2_AX_HLE_H

#include "cpu/cpu.h"

/* Native stand-in for the AX audio program that normally runs on the DSP.
 *
 * Each 5 ms audio frame, AX hands the DSP a command list. Among other things
 * it points at the list of voice parameter blocks (PBs) and at the buffer
 * the mixed output goes to.
 *
 * This decodes each playing voice (ADPCM, PCM8, PCM16 from audio RAM),
 * resamples it to 32 kHz, applies its volume envelope, low-pass filter and
 * left/right mix volumes, and writes the mixed 5 ms frame to the output
 * buffer the audio interface plays, updating the voice state as the DSP
 * does. The aux (effects) buses are not processed yet.
 */
void ax_hle_command_list(CPUState* cpu, u32 address);

void ax_hle_report(void);

#endif
