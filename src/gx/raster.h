#ifndef NFLS2_GX_RASTER_H
#define NFLS2_GX_RASTER_H

#include "cpu/cpu.h"

/* Software renderer. The game thread decodes and transforms vertices and
 * records triangles with a snapshot of the GPU state; worker threads then
 * draw each frame into an emulated embedded frame buffer (EFB), one band of
 * rows per thread, with texturing, the TEV, alpha test, depth and blending,
 * and copy it to the TV frame buffer in RAM. Frames can be saved as .bmp.
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

/* Called once per video field with the VI top-field base (0x201C), picture
   configuration (0x2048) and vertical timing (0x2000) registers. */
void gx_raster_xfb_field(CPUState* cpu, u64 field, u32 tfbl, u32 picture_cfg, u32 vtr);

/* Converts the frame buffer the TV shows into 0xAARRGGBB pixels (at most
   max_w x max_h; rows packed at the output width). Waits for drawing to
   finish first. Returns false if the VI isn't showing a valid buffer. */
bool gx_raster_xfb_rgb(CPUState* cpu, u32 tfbl, u32 picture_cfg, u32 vtr, u32* out, u32 max_w, u32 max_h,
                       u32* out_w, u32* out_h);

/* Draw every frame (needed when frames are shown live). */
void gx_raster_render_all(void);

/* Number of threads that draw frames (0 = on the game thread; -1 = automatic,
   one less than the number of CPU cores, at most 8). Call before the first
   frame. */
void gx_raster_set_threads(int n);

/* Waits until every recorded frame has been drawn. */
void gx_raster_sync(void);

/* Number of frames copied to the display so far. */
u32 gx_raster_frames(void);

void gx_raster_report(void);

#endif
