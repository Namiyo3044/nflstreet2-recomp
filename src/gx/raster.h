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

/* Writes every draw of `frames` frames, starting at that much game time,
   with the GPU setup each uses, to drawlog.txt. */
void gx_raster_draw_log(double at_seconds, int frames);

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

/* Whether the game is drawing for a 16:9 screen (its widescreen option):
   1 = yes, 0 = no (4:3), -1 = not known yet (no 3D drawn so far). */
int gx_raster_widescreen(void);

/* Forget what was detected so far (start detecting afresh). */
void gx_raster_widescreen_reset(void);

/* RAM at addr was written outside the GPU's frame-buffer copies (the
   redirected gather pipe, as movie players use). */
void gx_raster_note_ram_write(u32 addr);

/* The CPU reading the frame buffer directly (0xC8000000-0xC8FFFFFF:
   GXPeekARGB, GXPeekZ): colour as 0xAARRGGBB or 24-bit depth. */
u32 gx_raster_peek(u32 ea);
void gx_raster_peek_report(void);

/* Renderer timing since the last call: seconds the game waited for the
   drawing threads, average and worst time to draw a frame, triangles per frame. */
void gx_raster_take_perf(double* wait_s, double* draw_avg_s, double* draw_max_s, u32* tris_per_frame);

/* Draw with the graphics card from now on (OpenGL 3.3). The context must be
   current on this thread for the call, and released afterwards: drawing
   happens on the renderer's own GPU thread, which calls make_current before
   each batch of GL work. scale: internal resolution as a multiple of the
   GameCube's. Returns false (the software renderer stays) if the GPU setup
   fails. */
bool gx_raster_use_gpu(void* (*getproc)(const char*), int scale, void (*make_current)(void));
/* The graphics card's name for menus ("" with the software renderer). */
const char* gx_raster_gpu_name(void);
/* Frames the game clears to black are cleared to this colour instead
   (0xRRGGBB; 0 = off): the sides of ultrawide menus. */
void gx_raster_set_side_fill(u32 rgb);
/* A match is on (ultrawide: the match HUD's team panels move out). */
void gx_raster_set_match(bool on);
/* Rain drawn PS2-style (1) or as the GameCube draws it (0). */
extern int g_rain_style;
/* The graphics card's internal resolution, changed while running. */
void gx_raster_set_gpu_scale(int scale);

/* GPU mode: the window shows frames from the GPU (call once the presenting
   context is set up), so copies to the TV buffer aren't written to RAM. */
void gx_raster_present_from_gpu(bool on);

/* GPU mode: draws the picture the TV shows into the current context's window
   (win_w x win_h pixels, TV shape `aspect`), from the GPU's copy of the
   frame or from RAM; saves a screenshot if shot_path is set. */
void gx_raster_present_gpu(CPUState* cpu, u32 tfbl, u32 picture_cfg, u32 vtr, int win_w, int win_h, float aspect,
                           const char* shot_path);

/* The command stream wrote n words to transform unit address addr
   (0x000-0x7FF memory, 0x1000-0x10FF registers), after updating gx_xf_*. */
void gx_raster_xf_write(u32 addr, const u32* values, u32 n);

/* Free camera (debug): m is a 3x4 row-major matrix applied to view-space
   positions of 3D draws before projection (on = false: the game's camera). */
void gx_raster_set_free_camera(bool on, const float m[12]);

/* The screen shape the window shows when fixed (1 = 4:3, 2 = 16:9, 0 =
   following the game): full-screen 3D drawn for the other shape is corrected
   to fit. */
void gx_raster_set_target_shape(int shape);

void gx_raster_report(void);

/* Shows a w x h picture (0xAARRGGBB) instead of the TV picture (GPU mode,
   window thread). */
void gx_raster_present_pixels(const u32* px, u32 w, u32 h, int win_w, int win_h, float aspect);

#endif
