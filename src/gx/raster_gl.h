#ifndef NFLS2_GX_RASTER_GL_H
#define NFLS2_GX_RASTER_GL_H

#include "gx/raster_internal.h"

/* Graphics-card backend for the renderer (OpenGL 3.3 core).
 *
 * Draws the same per-frame command lists as the software rasteriser, into
 * an off-screen frame buffer `scale` times the GameCube's 640x528. The
 * TEV (the GameCube's colour combiner) runs in one fragment shader that
 * reads each draw's setup from a uniform block; depth, blending, culling and
 * scissor map onto the GPU's fixed-function stages.
 *
 * When a copy needs the picture in memory (to the TV buffer, or into a
 * texture), it is read back into g_color / g_depth at native size, and the
 * existing copy code takes it from there.
 *
 * Drawing and read-backs run on one thread (the renderer's GPU thread);
 * make_current is called first so the context is current there. Set up
 * (glr_init) happens on the caller's thread, which must then release the
 * context. glr_texture_freed may be called from another thread while the
 * GPU thread is idle.
 */

typedef void* (*GlrGetProc)(const char* name);

/* Loads OpenGL through getproc and sets everything up. The context must be
   current. Returns false (with a message printed) if anything is missing. */
bool glr_init(GlrGetProc getproc, int scale, void (*make_current)(void));

/* The graphics card's name, tidied for a menu ("" before glr_init). */
const char* glr_gpu_name(void);

/* A new internal resolution (1-8), used from the next list on (on the GPU
   thread). */
void glr_set_scale(int scale);

/* Draws a recorded list. TV-buffer copies in it are read back and written
   to RAM; clears are done on the GPU. */
void glr_run_list(const CmdList* l);

/* Makes the GPU's own copy of a frame-buffer copy into a texture (call
   after the list before it has been drawn); draws whose Stage.efb_copy is
   c->id then use it. */
void glr_tex_copy(const TexCopy* c);

/* Reads one native pixel (colour 0xAARRGGBB, or depth) of the frame buffer. */
void glr_read_point(u32 x, u32 y, bool depth, u32* out);

/* Reads the frame buffer back into g_color (and g_depth if depth). */
void glr_readback(bool depth);

/* A decoded texture buffer is about to be freed: drop its GPU copy. */
void glr_texture_freed(const u32* texels);

/* TV-buffer copies are always kept on the GPU (see glr_present); keep
   = true also reads each one back and writes it to RAM (needed when the
   picture is shown from RAM, or dumped). On by default. */
void glr_keep_ram_copies(bool keep);

/* Whether a copy to the TV buffer at this RAM address is held on the GPU. */
bool glr_xfb_on_gpu(u32 dest);

/* Showing the picture, from a second context sharing textures with the
   GPU thread's, on the thread that owns the window. Call glr_present_init
   once with that context current. */
enum { GLR_PIC_NONE, GLR_PIC_GPU, GLR_PIC_PIXELS };
typedef struct {
    int kind;
    u32 dest;                          /* GLR_PIC_GPU: TV buffer address */
    u32 tv_w, tv_h;                    /* size of the picture the TV shows */
    const u32* pixels;                 /* GLR_PIC_PIXELS: 0xAARRGGBB, top row first */
    u32 w, h;
} GlrPicture;
bool glr_present_init(void);

/* Draws the picture into target_fbo (0 = the window), win_w x win_h pixels,
   at the given TV shape with black borders; saves it as a BMP if shot_path
   is set. Only call while the GPU thread is idle. */
void glr_present(const GlrPicture* pic, unsigned target_fbo, int win_w, int win_h, float aspect,
                 const char* shot_path);

/* After glr_present: draws a window-sized picture (0xAARRGGBB, premultiplied
   alpha, top row first) over it; dirty = it changed since the last call. */
void glr_present_overlay(const u32* px, int w, int h, bool dirty);

/* A plain black window (on the window's thread, instead of glr_present). */
void glr_present_black(int w, int h);

/* Tests: glr_present draws into this frame buffer when given target 0. */
void glr_set_present_fbo(unsigned fbo);

void glr_report(void);

#endif
