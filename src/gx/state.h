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

static inline float gx_f32(u32 bits) {
    union { u32 u; float f; } v;
    v.u = bits;
    return v.f;
}

#endif
