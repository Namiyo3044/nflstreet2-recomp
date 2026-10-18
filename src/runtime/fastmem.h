#ifndef NFLS2_FASTMEM_H
#define NFLS2_FASTMEM_H

/* Fast memory access for the recompiled game code.
 *
 * DolRecomp's generated code includes the CPU header named by
 * DOLRECOMP_CPU_HEADER (CMakeLists points it here for the game library).
 * Every guest load and store calls mem_read32 / mem_write32 / ..., which
 * live in another file and check every address region, so each one costs a
 * real function call. Almost all accesses hit main RAM at 0x80000000, so
 * these inline versions do that case directly (big-endian, like the
 * GameCube) and hand everything else (uncached mirror, hardware registers,
 * locked cache, virtual memory, reservations) to the normal functions.
 * Paired-single loads and stores of plain floats get the same treatment.
 */
#include "cpu/cpu.h"
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h>
#define FM_BSWAP16(x) _byteswap_ushort(x)
#define FM_BSWAP32(x) _byteswap_ulong(x)
#define FM_BSWAP64(x) _byteswap_uint64(x)
#else
#define FM_BSWAP16(x) __builtin_bswap16(x)
#define FM_BSWAP32(x) __builtin_bswap32(x)
#define FM_BSWAP64(x) __builtin_bswap64(x)
#endif

/* Graphics command writes (0xCC008000-0xCC00FFFF) go straight to the pipe. */
void mmio_pipe_write(CPUState* cpu, u32 value, u8 size);
#define FM_PIPE(addr) (((addr) & 0xFFFF8000u) == 0xCC008000u)

#define FM_RAM(cpu, addr, n, off) (((off) = (addr) - 0x80000000u) <= (cpu)->ram_size - (n))

static inline u8 fm_read8(CPUState* cpu, u32 addr) {
    u32 off;
    if (FM_RAM(cpu, addr, 1u, off))
        return cpu->ram[off];
    return (mem_read8)(cpu, addr);
}

static inline u16 fm_read16(CPUState* cpu, u32 addr) {
    u32 off;
    if (FM_RAM(cpu, addr, 2u, off)) {
        u16 v;
        memcpy(&v, cpu->ram + off, 2);
        return FM_BSWAP16(v);
    }
    return (mem_read16)(cpu, addr);
}

static inline u32 fm_read32(CPUState* cpu, u32 addr) {
    u32 off;
    if (FM_RAM(cpu, addr, 4u, off)) {
        u32 v;
        memcpy(&v, cpu->ram + off, 4);
        return FM_BSWAP32(v);
    }
    return (mem_read32)(cpu, addr);
}

static inline u64 fm_read64(CPUState* cpu, u32 addr) {
    u32 off;
    if (FM_RAM(cpu, addr, 8u, off)) {
        u64 v;
        memcpy(&v, cpu->ram + off, 8);
        return FM_BSWAP64(v);
    }
    return (mem_read64)(cpu, addr);
}

/* Stores take the slow path while a lwarx reservation is held, so the
   normal function can clear it. */
static inline void fm_write8(CPUState* cpu, u32 addr, u8 value) {
    u32 off;
    if (!cpu->reserve_valid && FM_RAM(cpu, addr, 1u, off)) {
        cpu->ram[off] = value;
        return;
    }
    if (FM_PIPE(addr)) {
        mmio_pipe_write(cpu, value, 1u);
        return;
    }
    (mem_write8)(cpu, addr, value);
}

static inline void fm_write16(CPUState* cpu, u32 addr, u16 value) {
    u32 off;
    if (!cpu->reserve_valid && FM_RAM(cpu, addr, 2u, off)) {
        u16 v = FM_BSWAP16(value);
        memcpy(cpu->ram + off, &v, 2);
        return;
    }
    if (FM_PIPE(addr)) {
        mmio_pipe_write(cpu, value, 2u);
        return;
    }
    (mem_write16)(cpu, addr, value);
}

static inline void fm_write32(CPUState* cpu, u32 addr, u32 value) {
    u32 off;
    if (!cpu->reserve_valid && FM_RAM(cpu, addr, 4u, off)) {
        u32 v = FM_BSWAP32(value);
        memcpy(cpu->ram + off, &v, 4);
        return;
    }
    if (FM_PIPE(addr)) {
        mmio_pipe_write(cpu, value, 4u);
        return;
    }
    (mem_write32)(cpu, addr, value);
}

static inline void fm_write64(CPUState* cpu, u32 addr, u64 value) {
    u32 off;
    if (!cpu->reserve_valid && FM_RAM(cpu, addr, 8u, off)) {
        u64 v = FM_BSWAP64(value);
        memcpy(cpu->ram + off, &v, 8);
        return;
    }
    (mem_write64)(cpu, addr, value);
}

#define mem_read8(cpu, addr) fm_read8((cpu), (addr))
#define mem_read16(cpu, addr) fm_read16((cpu), (addr))
#define mem_read32(cpu, addr) fm_read32((cpu), (addr))
#define mem_read64(cpu, addr) fm_read64((cpu), (addr))
#define mem_write8(cpu, addr, v) fm_write8((cpu), (addr), (v))
#define mem_write16(cpu, addr, v) fm_write16((cpu), (addr), (v))
#define mem_write32(cpu, addr, v) fm_write32((cpu), (addr), (v))
#define mem_write64(cpu, addr, v) fm_write64((cpu), (addr), (v))

/* Paired singles: the plain-float case (GQR type 0), aligned, in RAM, done
   inline; quantised types, NaN loads and denormal stores go to the full
   implementation, which has the exact conversion rules. */
/* Loads: any value but a NaN converts exactly with a plain cast. */
static inline bool fm_f32_load_ok(u32 bits) {
    return ((bits >> 23) & 0xFFu) != 0xFFu || (bits & 0x007FFFFFu) == 0;
}

/* Stores: the full version flushes denormal results to zero. */
static inline bool fm_f32_store_ok(u32 bits) {
    return ((bits >> 23) & 0xFFu) != 0 || (bits & 0x007FFFFFu) == 0;
}

static inline bool fm_psq_ok(CPUState* cpu, bool indexed) {
    return (cpu->hid2 & PPC_HID2_PSE) && (indexed || (cpu->hid2 & PPC_HID2_LSQE));
}

#undef ppc_psq_load_inline
#undef ppc_psq_store_inline
static inline bool fm_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) {
    u32 off;
    if (((cpu->gqr[gqr & 7u] >> 16) & 7u) == 0 && !(ea & 3u) && fm_psq_ok(cpu, indexed) &&
        FM_RAM(cpu, ea, w ? 4u : 8u, off)) {
        u32 a, b = 0;
        memcpy(&a, cpu->ram + off, 4);
        a = FM_BSWAP32(a);
        if (!w) { memcpy(&b, cpu->ram + off + 4, 4); b = FM_BSWAP32(b); }
        if (fm_f32_load_ok(a) && (w || fm_f32_load_ok(b))) {
            f32 fa, fb;
            memcpy(&fa, &a, 4);
            memcpy(&fb, &b, 4);
            cpu->fpr[frD] = (f64)fa;
            cpu->ps1[frD] = w ? 1.0 : (f64)fb;
            return true;
        }
    }
    return ppc_psq_load(cpu, frD, ea, w, gqr, indexed, cia);
}

static inline bool fm_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) {
    u32 off;
    if ((cpu->gqr[gqr & 7u] & 7u) == 0 && !(ea & 3u) && !cpu->reserve_valid && fm_psq_ok(cpu, indexed) &&
        FM_RAM(cpu, ea, w ? 4u : 8u, off)) {
        f32 fa = (f32)cpu->fpr[frS], fb = w ? 0.0f : (f32)cpu->ps1[frS];
        u32 a, b;
        memcpy(&a, &fa, 4);
        memcpy(&b, &fb, 4);
        if (fm_f32_store_ok(a) && (w || fm_f32_store_ok(b))) {
            a = FM_BSWAP32(a);
            memcpy(cpu->ram + off, &a, 4);
            if (!w) { b = FM_BSWAP32(b); memcpy(cpu->ram + off + 4, &b, 4); }
            return true;
        }
    }
    if ((cpu->gqr[gqr & 7u] & 7u) == 0 && FM_PIPE(ea) && fm_psq_ok(cpu, indexed)) {
        /* plain floats into the graphics pipe (vertex data) */
        f32 fa = (f32)cpu->fpr[frS], fb = w ? 0.0f : (f32)cpu->ps1[frS];
        u32 a, b;
        memcpy(&a, &fa, 4);
        memcpy(&b, &fb, 4);
        if (fm_f32_store_ok(a) && (w || fm_f32_store_ok(b))) {
            mmio_pipe_write(cpu, a, 4u);
            if (!w) mmio_pipe_write(cpu, b, 4u);
            return true;
        }
    }
    return ppc_psq_store(cpu, frS, ea, w, gqr, indexed, cia);
}

#define ppc_psq_load_inline(cpu, frD, ea, w, gqr, indexed, cia) fm_psq_load((cpu), (frD), (ea), (w), (gqr), (indexed), (cia))
#define ppc_psq_store_inline(cpu, frS, ea, w, gqr, indexed, cia) fm_psq_store((cpu), (frS), (ea), (w), (gqr), (indexed), (cia))

#endif
