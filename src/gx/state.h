#ifndef NFLS2_GX_STATE_H
#define NFLS2_GX_STATE_H

#include "cpu/cpu.h"

/* Graphics hardware register state, written by the command stream (fifo.c)
   and read by the renderer. */

/* Command processor: vertex descriptors (0x50/0x60), attribute formats
   (0x70-0x97), array bases (0xA0-0xAF) and strides (0xB0-0xBF). */
extern u32 gx_cp[256];

/* Blitting processor: rasteriser, texture, TEV and pixel-engine state. */
extern u32 gx_bp[256];

/* Transform unit memory (matrices, lights) as raw float bits, addresses
   0x000-0x7FF, and its registers 0x1000-0x10FF (index 0-0xFF). */
extern u32 gx_xf_mem[0x800];
extern u32 gx_xf_reg[0x100];

/* TEV colour registers PREV, REG0-2 (signed 11-bit per component, RGBA) and
   konstant colours K0-K3 (8-bit RGBA). Both are written through BP 0xE0-0xE7;
   bit 23 of the value says which. */
extern s16 gx_tev_reg[4][4];
extern u8 gx_tev_konst[4][4];

/* Bumped whenever something the renderer's state snapshot reads may have
   changed (BP and XF register writes, a new frame), so consecutive draws
   with nothing changed in between can reuse the last snapshot. */
extern u32 gx_state_serial;

static inline float gx_f32(u32 bits) {
    union { u32 u; float f; } v;
    v.u = bits;
    return v.f;
}

#endif
