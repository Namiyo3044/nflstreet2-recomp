#ifndef NFLS2_GX_FIFO_H
#define NFLS2_GX_FIFO_H

#include "cpu/cpu.h"

/* The GameCube's graphics command stream ("GX FIFO").
 *
 * The game writes commands to the write-gather pipe at 0xCC008000. This
 * module decodes that stream: it tracks the command-processor registers that
 * describe vertex layouts (so it can step over draw commands correctly),
 * follows display lists, and acts on the commands that the CPU side waits
 * for -- "draw done" and tokens, which raise pixel-engine interrupts.
 * Nothing is rendered yet; this is the parser the renderer will sit on.
 */

/* Called for every write to the write-gather pipe (size 1, 2 or 4 bytes). */
void gx_fifo_write(CPUState* cpu, u32 value, u8 size);

void gx_fifo_report(void);

#endif
