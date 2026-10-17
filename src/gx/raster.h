#ifndef NFLS2_GX_RASTER_H
#define NFLS2_GX_RASTER_H

#include "cpu/cpu.h"

/* Reference software renderer, stage 1: decodes vertices, applies the game's
 * matrices, projection and viewport, and rasterises triangles into an
 * emulated embedded framebuffer (EFB) with depth testing and blending.
 * Colours come from vertex colours / material colours only (no textures or
 * TEV yet). Every Nth displayed frame is saved as a .bmp image.
 */

/* Dump every `every`-th displayed frame into `dir` (0 = off). */
void gx_raster_config(const char* dir, u32 every);

/* True while the current frame is one that will be dumped; draws in other
   frames are skipped to keep the run fast. */
bool gx_raster_active(void);

/* A draw command: primitive opcode (0x80-0xB8), vertex format, count, and
   the vertex data that followed it in the command stream. */
void gx_raster_draw(CPUState* cpu, u8 prim, u32 vat, u32 count, const u8* data);

/* PE_COPY_EXECUTE (BP 0x52): copy to display or texture, optional clear. */
void gx_raster_copy(u32 copy_cmd);

void gx_raster_report(void);

#endif
