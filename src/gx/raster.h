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

/* TV frame buffer dumps: every `every_fields` video fields (0 = off), save
   the frame buffer the video interface is showing, as xfb_<field>.bmp. */
void gx_raster_config_xfb(u32 every_fields);

/* Called once per video field with the VI top-field base (0x201C) and
   picture configuration (0x2048) registers. */
void gx_raster_xfb_field(CPUState* cpu, u64 field, u32 tfbl, u32 picture_cfg);

/* Number of frames copied to the display so far. */
u32 gx_raster_frames(void);

void gx_raster_report(void);

#endif
