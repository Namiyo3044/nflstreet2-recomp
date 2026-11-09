#ifndef NFLS2_GX_FRONT_H
#define NFLS2_GX_FRONT_H

#include "cpu/cpu.h"
#include <stdbool.h>

/* The graphics thread: the port's version of Dolphin's "dual core".
 *
 * Without it, the game thread itself reads the graphics command stream,
 * decodes the vertices, snapshots the GPU state and decodes textures, in
 * between running the game's code (a third of its time on a mid-range PC).
 * With it, the game thread only appends what the game writes to the
 * write-gather pipe to a ring buffer; this thread does the rest (gx/fifo.c,
 * gx/raster.c, gx/texture.c, as before), and the renderer's GPU thread draws
 * the finished lists as before.
 *
 * The game thread catches up with it where the game itself waits for the
 * graphics hardware: when it has nothing to run (waiting for "draw done"),
 * when it reads the pixel engine's registers (tokens), when it reads the
 * frame buffer directly, and for the window's own changes (custom textures).
 * What the stream asks of the CPU side (draw done, tokens, a frame copied
 * to the TV buffer) comes back as events, acted on by the game thread in
 * order. Real hardware runs the GPU behind the CPU the same way.
 *
 * Off: netplay and input recording (timing must match exactly), the
 * software renderer, frame dumps and draw logs, and --no-gx-thread.
 */

extern int g_gx_front_on;              /* the thread is running (set once, before the game starts) */
extern volatile int g_gx_front_events; /* events waiting for the game thread */

/* Starts the thread (the GPU renderer is on); false if it couldn't. */
bool gx_front_start(CPUState* cpu);

/* Game thread: bytes the game wrote to the pipe (1, 2 or 4, big-endian value). */
void gx_front_push(u32 value, u8 size);

/* Game thread: makes the bytes written so far visible to the thread (they
   go in blocks; the waits below do this too). */
void gx_front_flush(void);

/* Game thread: waits until the thread has handled everything written so
   far, then acts on its events. */
void gx_front_sync(void);

/* Game thread, about to skip idle time: if the thread still has work, waits
   for it and acts on its events; true if it did (the game may have
   something to run now). */
bool gx_front_idle_sync(void);

/* Game thread: acts on the events the thread has sent (interrupts_poll). */
void gx_front_drain(void);

/* On the graphics thread (where the stream's effects must become events). */
bool gx_front_on_thread(void);

/* Graphics thread: the stream's effects on the CPU side. */
enum { GXF_EV_FINISH, GXF_EV_TOKEN, GXF_EV_TOKEN_INT, GXF_EV_DISPLAY_COPY };
void gx_front_event(int type, u32 value);

/* Lock around handing frames to the renderer and showing them (raster.c and
   the window): no-ops when the thread is off. */
void gx_front_lock(void);
void gx_front_unlock(void);

/* Seconds the thread spent working since the last call (the performance log). */
double gx_front_take_busy(void);

void gx_front_report(void);

#endif
